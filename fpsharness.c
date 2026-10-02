/* fpsharness.exe - what stable 240 / 480 fps actually needs, and what
 * decoupling a fixed-timestep world from the present rate looks like.
 *
 * A standalone window. It attaches to nothing, loads no game, and shares no
 * code with the wrapper - it exists to answer two questions on their own:
 *
 *   1. What does it take to present a real DXGI swap chain at a stable 240 or
 *      480 fps in a window, and how much jitter is left once you have it?
 *   2. What are the ways a 60 Hz world can be shown at those rates WITHOUT the
 *      windmill effect - the world speeding up because logic advances per
 *      presented frame?
 *
 * Four triangles spin side by side, each a different answer, so the difference
 * is visible the instant you change the frame rate with the arrow keys:
 *
 *   1  COUPLED     angle += a fixed step every PRESENTED frame. This is the
 *                  bug: raise the fps and it spins faster. The windmill.
 *   2  DECOUPLED   angle = elapsed_seconds * rate. Constant real-world speed at
 *                  any fps. The render-rate-independent way to drive rotation.
 *   3  60Hz SNAP   a fixed 60 Hz logic tick advances the angle; every present
 *                  shows the latest tick's angle. Correct speed at any fps, but
 *                  at 480 fps each tick is shown eight times, so it judders.
 *   4  60Hz INTERP the same 60 Hz logic, but the present interpolates between
 *                  the last two ticks (alpha = accumulator / tick). Correct
 *                  speed AND smooth at any fps, with no invented frames - just
 *                  the state drawn where it is between two real updates. This
 *                  is the lever for the game before any pixel frame-generation.
 *
 * 2 and 4 stay calm and identical in speed as you climb to 480; 1 spins up; 3
 * stays the right speed but steps. That side-by-side is the whole point.
 *
 * The logic tick (default 60 Hz) is one shared fixed-timestep accumulator that
 * every entity reads, so adding a sprite that updates at its own rate later is
 * a matter of giving it its own accumulator - the presentation layer already
 * interpolates per entity. Keys:
 *
 *   Up / Down     target fps: 60, 120, 144, 240, 480, uncapped
 *   V             vsync on (Present sync=1) <-> off (allow-tearing, sync=0)
 *   K / L         logic rate down / up (30 / 60 / 120 / 240 Hz)
 *   Space         pause the world (freeze logic and the coupled spin)
 *   Esc           quit
 *
 * The title bar and console report target vs measured fps, frame-time
 * avg/min/max/99th and its standard deviation (the jitter that decides whether
 * a rate is "stable"), the pacing method in use, and the present mode.
 *
 * Frame times are present-to-present intervals, so the jitter figure is the
 * spacing the compositor actually receives, message pump included. "late"
 * counts intervals over 1.5 target periods.
 *
 * Build (standalone, no repo deps; d3d11 and dxgi are loaded from System32 at
 * run time, see load_create_device, so they must not be linked):
 *   zig cc -O2 -target x86_64-windows-gnu -o fpsharness.exe fpsharness.c \
 *       -ldxguid -luser32 -lwinmm -Wl,--subsystem,windows
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h> /* timeBeginPeriod */
#include <d3d11.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static FILE *g_log;
static void LOG(const char *fmt, ...)
{
	va_list ap;
	if (!g_log)
		return;
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
}

#ifndef DXGI_FEATURE_PRESENT_ALLOW_TEARING
#define DXGI_FEATURE_PRESENT_ALLOW_TEARING 39
#endif
#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL
#endif
#ifndef DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
#define DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING 2048
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* -------------------------------------------------------------- the pacer
 *
 * The absolute-deadline accumulator is the same shape the game wrapper's
 * present_wait uses: the next deadline is advanced by exactly one frame period
 * from the last one, not from "now", so a frame that runs long is paid back by
 * the next waiting less and the average rate holds. The wait itself is a
 * high-resolution waitable timer down to just short of the deadline, then a
 * busy-spin for the last stretch, because neither Sleep nor a plain timer is
 * accurate to the ~2.08 ms a 480 fps frame allows. */
typedef struct Pacer {
	LARGE_INTEGER freq;
	LARGE_INTEGER next; /* absolute QPC deadline of the frame being paced */
	HANDLE timer;       /* hi-res waitable timer, or NULL on older Windows */
	int have_timer;
} Pacer;

static void pacer_init(Pacer *p)
{
	HANDLE (WINAPI *createex)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

	memset(p, 0, sizeof(*p));
	QueryPerformanceFrequency(&p->freq);
	/* 1 ms scheduler tick as a floor for the fallback path; harmless with the
	 * hi-res timer and required without it. */
	timeBeginPeriod(1);
	createex = (HANDLE(WINAPI *)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD))(void *)
		GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateWaitableTimerExW");
	if (createex)
		p->timer = createex(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
				    TIMER_MODIFY_STATE | SYNCHRONIZE);
	p->have_timer = p->timer != NULL;
}

static const char *pacer_method(const Pacer *p)
{
	return p->have_timer ? "hi-res waitable timer + spin" : "Sleep(1ms) + spin";
}

/* Pace to target_fps. 0 means uncapped: return at once and let Present decide.
 * Returns nothing; the caller presents immediately after. */
static void pacer_wait(Pacer *p, double target_fps)
{
	LARGE_INTEGER now;
	LONGLONG period, behind_limit;

	if (target_fps <= 0.0) {
		p->next.QuadPart = 0; /* restart the schedule when a cap comes back */
		return;
	}
	period = (LONGLONG)((double)p->freq.QuadPart / target_fps + 0.5);
	QueryPerformanceCounter(&now);
	/* First paced frame, or the schedule fell so far behind that catching up
	 * would run the world fast: restart from now. Two frames of slack, like
	 * the wrapper - jitter is absorbed, a real stall is not chased. */
	behind_limit = period * 2;
	if (!p->next.QuadPart || now.QuadPart > p->next.QuadPart + behind_limit)
		p->next.QuadPart = now.QuadPart + period;
	else
		p->next.QuadPart += period;

	for (;;) {
		LONGLONG left;

		QueryPerformanceCounter(&now);
		left = p->next.QuadPart - now.QuadPart;
		if (left <= 0)
			return;
		/* Hand the timer everything but the last ~0.5 ms and spin the rest;
		 * the fallback keeps a wider margin because Sleep is coarser. */
		{
			double left_s = (double)left / (double)p->freq.QuadPart;
			double margin = p->have_timer ? 0.0005 : 0.0015;

			if (left_s > margin) {
				if (p->have_timer) {
					LARGE_INTEGER due;
					/* relative 100 ns units, negative */
					due.QuadPart = -(LONGLONG)((left_s - margin) * 1.0e7);
					if (SetWaitableTimer(p->timer, &due, 0, NULL, NULL, FALSE))
						WaitForSingleObject(p->timer, INFINITE);
					else
						Sleep(0);
				} else {
					Sleep((DWORD)((left_s - margin) * 1000.0));
				}
			} else {
				YieldProcessor();
			}
		}
	}
}

/* ------------------------------------------------------------- frame stats */
#define STAT_N 2048
typedef struct Stats {
	double ms[STAT_N];
	int n, head;
} Stats;

static void stats_add(Stats *s, double ms)
{
	s->ms[s->head] = ms;
	s->head = (s->head + 1) % STAT_N;
	if (s->n < STAT_N)
		s->n++;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

static void stats_summary(const Stats *s, double *avg, double *mn, double *mx, double *sd,
			  double *p99)
{
	double sum = 0.0, sq = 0.0;
	double tmp[STAT_N];
	int i;

	*avg = *mn = *mx = *sd = *p99 = 0.0;
	if (!s->n)
		return;
	for (i = 0; i < s->n; i++) {
		double v = s->ms[i];
		sum += v;
		if (i == 0 || v < *mn)
			*mn = v;
		if (i == 0 || v > *mx)
			*mx = v;
		tmp[i] = v;
	}
	*avg = sum / s->n;
	for (i = 0; i < s->n; i++)
		sq += (s->ms[i] - *avg) * (s->ms[i] - *avg);
	*sd = sqrt(sq / s->n);
	qsort(tmp, s->n, sizeof(double), cmp_double);
	*p99 = tmp[(int)(s->n * 0.99)];
}

/* --------------------------------------------------------------- entities */
typedef enum {
	MODE_COUPLED,	/* angle += step per presented frame */
	MODE_DECOUPLED, /* angle = time * rate */
	MODE_SNAP,	/* 60 Hz logic, present shows the latest tick */
	MODE_INTERP	/* 60 Hz logic, present interpolates between ticks */
} Mode;

typedef struct Entity {
	Mode mode;
	float cx;	  /* screen-space centre x in NDC */
	float deg_per_frame; /* coupled */
	float deg_per_sec;   /* decoupled and the logic-driven pair */
	/* continuous angle for coupled/decoupled */
	double angle;
	/* fixed-timestep logic state for snap/interp: previous and current tick */
	double logic_prev, logic_cur;
} Entity;

/* The four answers, left to right. */
static Entity g_ent[4] = {
	{ MODE_COUPLED,	  -0.75f, 6.0f, 0.0f,	0, 0, 0 },
	{ MODE_DECOUPLED, -0.25f, 0.0f, 360.0f,	0, 0, 0 },
	{ MODE_SNAP,	   0.25f, 0.0f, 360.0f,	0, 0, 0 },
	{ MODE_INTERP,	   0.75f, 0.0f, 360.0f,	0, 0, 0 },
};

/* ---------------------------------------------------------------- D3D11 */
typedef struct Gfx {
	ID3D11Device *dev;
	ID3D11DeviceContext *ctx;
	IDXGISwapChain1 *sc;
	ID3D11RenderTargetView *rtv;
	ID3D11VertexShader *vs;
	ID3D11PixelShader *ps;
	ID3D11InputLayout *layout;
	ID3D11Buffer *vb, *cb;
	UINT cw, ch;
	int tearing; /* allow-tearing supported by this adapter/OS */
} Gfx;

typedef struct Vtx {
	float x, y;
} Vtx;

static const char kHLSL[] =
	"cbuffer C : register(b0) {\n"
	"  float4 rot;   // x=cos, y=sin, z=scale, w=aspect(h/w)\n"
	"  float4 place; // x=cx, y=cy\n"
	"};\n"
	"float4 vs_main(float2 p : POSITION) : SV_POSITION {\n"
	"  float2 s = p * rot.z;\n"
	"  // clockwise: increasing angle turns the triangle to the right\n"
	"  float2 r = float2(s.x * rot.x + s.y * rot.y, -s.x * rot.y + s.y * rot.x);\n"
	"  r.x *= rot.w;\n"
	"  return float4(r + place.xy, 0, 1);\n"
	"}\n"
	"float4 ps_main() : SV_Target { return float4(1, 1, 1, 1); }\n";

typedef HRESULT(WINAPI *CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
				   ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
				   ID3DBlob **);

typedef HRESULT(WINAPI *CreateDeviceFn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
					const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
					D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

/* Resolve D3D11CreateDevice from the REAL system DLL by absolute path.
 *
 * This repo ships a software d3d11.dll (the game wrapper). If the harness
 * imported d3d11.dll by name, the loader would find the repo's copy sitting
 * beside the .exe first, and the wrapper would inject itself into this process
 * - which is exactly what happened: it faulted in its own Swap_Present, since
 * its instruction patches are meant for rabiribi.exe. Loading System32's copy
 * explicitly sidesteps the shadow, and linking nothing against d3d11/dxgi means
 * there is no name-based import to hijack. */
static CreateDeviceFn load_create_device(void)
{
	char sys[MAX_PATH], path[MAX_PATH];
	UINT n = GetSystemDirectoryA(sys, sizeof(sys));
	HMODULE m;

	if (!n || n >= sizeof(sys))
		return NULL;
	/* Pre-bind the real dxgi.dll by absolute path first. d3d11.dll imports it
	 * by name, and the repo's dxgi.dll forwarder sits beside the .exe; loading
	 * System32's copy first means that import resolves to the already-loaded
	 * system module rather than the repo shadow. */
	snprintf(path, sizeof(path), "%s\\dxgi.dll", sys);
	LoadLibraryA(path);
	snprintf(path, sizeof(path), "%s\\d3d11.dll", sys);
	m = LoadLibraryA(path);
	return m ? (CreateDeviceFn)(void *)GetProcAddress(m, "D3D11CreateDevice") : NULL;
}

static void client_size(HWND hwnd, UINT *w, UINT *h)
{
	RECT rc;
	GetClientRect(hwnd, &rc);
	*w = rc.right > rc.left ? (UINT)(rc.right - rc.left) : 1;
	*h = rc.bottom > rc.top ? (UINT)(rc.bottom - rc.top) : 1;
}

static void rtv_make(Gfx *g)
{
	ID3D11Texture2D *bb = NULL;

	if (SUCCEEDED(IDXGISwapChain1_GetBuffer(g->sc, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
		ID3D11Device_CreateRenderTargetView(g->dev, (ID3D11Resource *)bb, NULL, &g->rtv);
		ID3D11Texture2D_Release(bb);
	}
}

static HRESULT gfx_init(Gfx *g, HWND hwnd)
{
	IDXGIDevice *xd = NULL;
	IDXGIAdapter *ad = NULL;
	IDXGIFactory2 *fac = NULL;
	IDXGIFactory5 *fac5 = NULL;
	DXGI_SWAP_CHAIN_DESC1 d;
	D3D11_BUFFER_DESC bd;
	HMODULE dc;
	CompileFn fn;
	ID3DBlob *vsb, *psb;
	D3D_FEATURE_LEVEL fl;
	CreateDeviceFn create_device;
	HRESULT hr;
	static const D3D11_INPUT_ELEMENT_DESC el[1] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
	};
	/* Unit triangle pointing up. Taller than it is wide, so its heading reads
	 * from the silhouette alone. */
	Vtx tri[3];

	create_device = load_create_device();
	if (!create_device) {
		LOG("could not load System32\\d3d11.dll D3D11CreateDevice");
		return E_FAIL;
	}
	hr = create_device(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
			   &g->dev, &fl, &g->ctx);
	if (FAILED(hr)) {
		hr = create_device(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
				   &g->dev, &fl, &g->ctx);
		if (FAILED(hr))
			return hr;
	}
	LOG("D3D11CreateDevice ok, feature level %x", (unsigned)fl);
	ID3D11Device_QueryInterface(g->dev, &IID_IDXGIDevice, (void **)&xd);
	if (!xd) { LOG("QI IDXGIDevice failed"); return E_FAIL; }
	IDXGIDevice_GetAdapter(xd, &ad);
	if (!ad) { LOG("GetAdapter failed"); IDXGIDevice_Release(xd); return E_FAIL; }
	IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
	IDXGIAdapter_Release(ad);
	IDXGIDevice_Release(xd);
	if (!fac) { LOG("GetParent IDXGIFactory2 failed"); return E_FAIL; }

	/* Does the adapter allow tearing? Without it a windowed present cannot beat
	 * the refresh rate, so 240 / 480 on a 60 / 144 panel is impossible and the
	 * cap silently becomes the refresh - the single most important thing to
	 * know before trusting a high-fps number. */
	if (SUCCEEDED(IDXGIFactory2_QueryInterface(fac, &IID_IDXGIFactory5, (void **)&fac5))) {
		BOOL allow = FALSE;

		if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(
			    fac5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
			g->tearing = allow ? 1 : 0;
		IDXGIFactory5_Release(fac5);
	}

	client_size(hwnd, &g->cw, &g->ch);
	memset(&d, 0, sizeof(d));
	d.Width = g->cw;
	d.Height = g->ch;
	d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	d.SampleDesc.Count = 1;
	d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	d.BufferCount = 2;
	d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	d.Flags = g->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
	hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)g->dev, hwnd, &d, NULL, NULL,
						  &g->sc);
	IDXGIFactory2_MakeWindowAssociation(fac, hwnd,
					    DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
	IDXGIFactory2_Release(fac);
	if (FAILED(hr) || !g->sc) {
		LOG("CreateSwapChainForHwnd failed %08lx sc=%p", (unsigned long)hr, (void *)g->sc);
		return FAILED(hr) ? hr : E_FAIL;
	}
	LOG("swapchain ok, tearing=%d %ux%u", g->tearing, g->cw, g->ch);
	rtv_make(g);
	if (!g->rtv) { LOG("rtv_make failed"); return E_FAIL; }
	{
		IDXGIDevice1 *xd1 = NULL;
		/* One frame of latency: every frame the driver queues is a frame of
		 * input lag, and this harness paces itself. */
		if (SUCCEEDED(ID3D11Device_QueryInterface(g->dev, &IID_IDXGIDevice1,
							  (void **)&xd1))) {
			IDXGIDevice1_SetMaximumFrameLatency(xd1, 1);
			IDXGIDevice1_Release(xd1);
		}
	}

	dc = LoadLibraryA("d3dcompiler_47.dll");
	fn = dc ? (CompileFn)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
	if (!fn) {
		LOG("d3dcompiler_47.dll / D3DCompile not available");
		return E_FAIL;
	}
	{
		ID3DBlob *err = NULL;

		vsb = psb = NULL;
		hr = fn(kHLSL, sizeof(kHLSL) - 1, "fps", NULL, NULL, "vs_main", "vs_4_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err);
		if (SUCCEEDED(hr))
			hr = fn(kHLSL, sizeof(kHLSL) - 1, "fps", NULL, NULL, "ps_main", "ps_4_0",
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
		if (FAILED(hr)) {
			LOG("shader compile failed %08lx: %s", (unsigned long)hr,
			    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "no msg");
			if (err)
				ID3D10Blob_Release(err);
			return hr;
		}
		if (err)
			ID3D10Blob_Release(err);
	}
	hr = ID3D11Device_CreateVertexShader(g->dev, ID3D10Blob_GetBufferPointer(vsb),
					     ID3D10Blob_GetBufferSize(vsb), NULL, &g->vs);
	if (SUCCEEDED(hr))
		hr = ID3D11Device_CreatePixelShader(g->dev, ID3D10Blob_GetBufferPointer(psb),
						    ID3D10Blob_GetBufferSize(psb), NULL, &g->ps);
	if (SUCCEEDED(hr))
		hr = ID3D11Device_CreateInputLayout(g->dev, el, 1, ID3D10Blob_GetBufferPointer(vsb),
						    ID3D10Blob_GetBufferSize(vsb), &g->layout);
	ID3D10Blob_Release(vsb);
	ID3D10Blob_Release(psb);
	if (FAILED(hr))
		return hr;

	tri[0].x = 0.0f;   tri[0].y = 0.62f;
	tri[1].x = -0.52f; tri[1].y = -0.42f;
	tri[2].x = 0.52f;  tri[2].y = -0.42f;

	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = sizeof(tri);
	bd.Usage = D3D11_USAGE_IMMUTABLE;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	{
		D3D11_SUBRESOURCE_DATA sr;
		memset(&sr, 0, sizeof(sr));
		sr.pSysMem = tri;
		hr = ID3D11Device_CreateBuffer(g->dev, &bd, &sr, &g->vb);
		if (FAILED(hr))
			return hr;
	}
	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = 32; /* two float4 */
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	return ID3D11Device_CreateBuffer(g->dev, &bd, NULL, &g->cb);
}

static void gfx_resize(Gfx *g, HWND hwnd)
{
	UINT cw, ch;

	client_size(hwnd, &cw, &ch);
	if (cw == g->cw && ch == g->ch)
		return;
	ID3D11DeviceContext_OMSetRenderTargets(g->ctx, 0, NULL, NULL);
	if (g->rtv)
		ID3D11RenderTargetView_Release(g->rtv);
	g->rtv = NULL;
	IDXGISwapChain1_ResizeBuffers(g->sc, 0, cw, ch, DXGI_FORMAT_UNKNOWN,
				      g->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
	g->cw = cw;
	g->ch = ch;
	rtv_make(g);
}

static void draw_entity(Gfx *g, const Entity *e, double render_angle_deg)
{
	D3D11_MAPPED_SUBRESOURCE m;
	double a = render_angle_deg * 3.14159265358979323846 / 180.0;

	if (FAILED(ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource *)g->cb, 0,
					   D3D11_MAP_WRITE_DISCARD, 0, &m)))
		return;
	{
		float *c = (float *)m.pData;

		c[0] = (float)cos(a);
		c[1] = (float)sin(a);
		c[2] = 0.20f; /* scale */
		/* Squeeze x by height/width so the triangle stays round, not stretched,
		 * in a wide window. */
		c[3] = g->cw ? (float)g->ch / (float)g->cw : 1.0f;
		c[4] = e->cx;
		c[5] = 0.0f;
		c[6] = 0.0f;
		c[7] = 0.0f;
		ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource *)g->cb, 0);
	}
	ID3D11DeviceContext_Draw(g->ctx, 3, 0);
}

/* ------------------------------------------------------------- input state */
static double g_target_fps = 240.0;
static int g_vsync = 0;	 /* off by default so the cap can exceed refresh */
static double g_logic_hz = 60.0;
static int g_paused = 0;
static int g_run_secs = 0; /* scripted auto-quit; 0 = run until closed */
static volatile int g_quit = 0;

static const double kFpsSteps[] = { 60, 120, 144, 240, 480, 0 /* uncapped */ };
static int g_fps_idx = 3;
static const double kLogicSteps[] = { 30, 60, 120, 240 };
static int g_logic_idx = 1;

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_DESTROY:
		g_quit = 1;
		PostQuitMessage(0);
		return 0;
	case WM_KEYDOWN:
		/* Held keys auto-repeat; the toggles must flip once per press. */
		if ((lp & (1L << 30)) && (wp == 'V' || wp == VK_SPACE))
			return 0;
		switch (wp) {
		case VK_ESCAPE:
			g_quit = 1;
			break;
		case VK_UP:
			if (g_fps_idx < (int)(sizeof(kFpsSteps) / sizeof(kFpsSteps[0])) - 1)
				g_fps_idx++;
			g_target_fps = kFpsSteps[g_fps_idx];
			break;
		case VK_DOWN:
			if (g_fps_idx > 0)
				g_fps_idx--;
			g_target_fps = kFpsSteps[g_fps_idx];
			break;
		case 'V':
			g_vsync = !g_vsync;
			break;
		case 'L':
			if (g_logic_idx < (int)(sizeof(kLogicSteps) / sizeof(kLogicSteps[0])) - 1)
				g_logic_idx++;
			g_logic_hz = kLogicSteps[g_logic_idx];
			break;
		case 'K':
			if (g_logic_idx > 0)
				g_logic_idx--;
			g_logic_hz = kLogicSteps[g_logic_idx];
			break;
		case VK_SPACE:
			g_paused = !g_paused;
			break;
		}
		return 0;
	}
	return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
	WNDCLASSA wc;
	HWND hwnd;
	Gfx g;
	Pacer pacer;
	Stats stats;
	LARGE_INTEGER freq, prev_qpc, last_report, loop_start, last_present;
	double logic_accum = 0.0; /* seconds of unconsumed real time for the 60Hz tick */
	unsigned long frames_since_report = 0, late = 0;
	int i;

	(void)prev;
	(void)show;

	g_log = fopen("fpsharness.log", "w");
	LOG("fpsharness start");

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = "fpsharness";
	RegisterClassA(&wc);
	hwnd = CreateWindowExA(0, "fpsharness", "fpsharness", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
			       CW_USEDEFAULT, CW_USEDEFAULT, 1280, 400, NULL, NULL, inst, NULL);
	if (!hwnd) {
		LOG("CreateWindowEx failed %lu", (unsigned long)GetLastError());
		return 1;
	}
	LOG("window %p", (void *)hwnd);

	memset(&g, 0, sizeof(g));
	{
		HRESULT ir = gfx_init(&g, hwnd);
		if (FAILED(ir)) {
			LOG("gfx_init failed %08lx", (unsigned long)ir);
			MessageBoxA(hwnd, "D3D11 init failed", "fpsharness", MB_OK);
			return 2;
		}
	}
	LOG("gfx_init ok, entering loop");

	{
		FILE *f = NULL;
		/* A console makes the live numbers readable when launched from a
		 * shell; when there is no window station to attach one (headless
		 * launch), send stdout to NUL so the prints are harmless. */
		if (AllocConsole())
			freopen_s(&f, "CONOUT$", "w", stdout);
		else
			freopen_s(&f, "NUL", "w", stdout);
		LOG("console: %s", f ? "attached" : "none (stdout may be closed)");
	}
	printf("fpsharness - four ways to spin a triangle, left to right:\n");
	printf("  1 COUPLED    angle += 6 deg / PRESENTED frame  (windmill: faster at higher fps)\n");
	printf("  2 DECOUPLED  angle  = time * 360 deg/s          (constant at any fps)\n");
	printf("  3 60Hz SNAP  60Hz logic, show latest tick       (right speed, judders at high fps)\n");
	printf("  4 60Hz INTERP 60Hz logic, interpolate ticks     (right speed AND smooth)\n\n");
	printf("adapter allow-tearing: %s\n", g.tearing ? "YES" : "NO (windowed present cannot exceed refresh)");
	printf("keys: Up/Down fps  V vsync  K/L logic Hz  Space pause  Esc quit\n\n");

	pacer_init(&pacer);
	printf("pacing: %s\n\n", pacer_method(&pacer));
	g_target_fps = kFpsSteps[g_fps_idx];
	g_logic_hz = kLogicSteps[g_logic_idx];

	/* Optional scripted run: "fpsharness.exe <targetfps> <vsync0|1> <seconds>".
	 * targetfps 0 = uncapped. Any omitted arg keeps its default. seconds 0 =
	 * run until closed. Lets a measurement be taken without pressing keys. */
	{
		int a_fps = -1, a_vsync = -1, a_secs = 0, i2;

		sscanf(cmd ? cmd : "", "%d %d %d", &a_fps, &a_vsync, &a_secs);
		if (a_fps >= 0) {
			g_target_fps = (double)a_fps;
			/* snap the Up/Down index to the nearest listed step */
			for (i2 = 0; i2 < (int)(sizeof(kFpsSteps) / sizeof(kFpsSteps[0])); i2++)
				if ((int)kFpsSteps[i2] == a_fps)
					g_fps_idx = i2;
		}
		if (a_vsync == 0 || a_vsync == 1)
			g_vsync = a_vsync;
		g_run_secs = a_secs;
		LOG("scripted: target %.0f vsync %d secs %d", g_target_fps, g_vsync, a_secs);
	}

	memset(&stats, 0, sizeof(stats));
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&prev_qpc);
	last_report = prev_qpc;
	loop_start = prev_qpc;
	last_present.QuadPart = 0;
	LOG("loop start, target %.0f logic %.0f", g_target_fps, g_logic_hz);

	while (!g_quit) {
		MSG msg;
		LARGE_INTEGER now;
		double dt, alpha;
		float clear[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
		D3D11_VIEWPORT vp;
		UINT stride = sizeof(Vtx), offset = 0;

		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
		if (g_quit)
			break;

		/* --- real time elapsed since the last frame --- */
		QueryPerformanceCounter(&now);
		dt = (double)(now.QuadPart - prev_qpc.QuadPart) / (double)freq.QuadPart;
		prev_qpc = now;
		if (dt > 0.25)
			dt = 0.25; /* clamp a hitch so the accumulator does not explode */

		/* --- advance the world ---
		 * COUPLED steps once per rendered frame (the bug). DECOUPLED and the
		 * two 60Hz entities step by real time, DECOUPLED continuously and the
		 * 60Hz ones only when the fixed accumulator crosses a tick. */
		if (!g_paused) {
			double step = 1.0 / g_logic_hz;

			g_ent[0].angle += g_ent[0].deg_per_frame;	 /* per frame */
			g_ent[1].angle += g_ent[1].deg_per_sec * dt;	 /* per second */

			logic_accum += dt;
			while (logic_accum >= step) {
				logic_accum -= step;
				for (i = 2; i <= 3; i++) {
					g_ent[i].logic_prev = g_ent[i].logic_cur;
					g_ent[i].logic_cur += g_ent[i].deg_per_sec * step;
				}
			}
		}
		alpha = g_logic_hz > 0.0 ? logic_accum * g_logic_hz : 0.0;
		if (alpha > 1.0)
			alpha = 1.0;

		/* --- render --- */
		if (IsIconic(hwnd)) {
			/* Nothing is shown and a 1x1 resize would be wasted; the world
			 * keeps its time, the stats restart when the window comes back. */
			Sleep(10);
			last_present.QuadPart = 0;
			continue;
		}
		gfx_resize(&g, hwnd);
		if (!g.rtv) {
			Sleep(1);
			continue;
		}
		vp.TopLeftX = vp.TopLeftY = 0.0f;
		vp.Width = (float)g.cw;
		vp.Height = (float)g.ch;
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 1, &g.rtv, NULL);
		ID3D11DeviceContext_ClearRenderTargetView(g.ctx, g.rtv, clear);
		ID3D11DeviceContext_RSSetViewports(g.ctx, 1, &vp);
		ID3D11DeviceContext_IASetPrimitiveTopology(g.ctx,
							   D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ID3D11DeviceContext_IASetInputLayout(g.ctx, g.layout);
		ID3D11DeviceContext_IASetVertexBuffers(g.ctx, 0, 1, &g.vb, &stride, &offset);
		ID3D11DeviceContext_VSSetShader(g.ctx, g.vs, NULL, 0);
		ID3D11DeviceContext_VSSetConstantBuffers(g.ctx, 0, 1, &g.cb);
		ID3D11DeviceContext_PSSetShader(g.ctx, g.ps, NULL, 0);

		for (i = 0; i < 4; i++) {
			double ra;

			switch (g_ent[i].mode) {
			case MODE_COUPLED:
			case MODE_DECOUPLED:
				ra = g_ent[i].angle;
				break;
			case MODE_SNAP:
				ra = g_ent[i].logic_cur; /* latest tick, no blend */
				break;
			case MODE_INTERP:
			default:
				/* draw where the state is BETWEEN the last two ticks */
				ra = g_ent[i].logic_prev +
				     (g_ent[i].logic_cur - g_ent[i].logic_prev) * alpha;
				break;
			}
			draw_entity(&g, &g_ent[i], ra);
		}

		/* --- pace, then present --- */
		{
			static int once;
			if (!once) { once = 1; LOG("first frame drawn, about to present"); }
		}
		pacer_wait(&pacer, g_target_fps);
		{
			static int once2;
			if (!once2) { once2 = 1; LOG("pacer_wait returned, presenting"); }
		}
		{
			HRESULT phr;
			static int once3;

			if (g_vsync) {
				phr = IDXGISwapChain1_Present(g.sc, 1, 0);
			} else {
				UINT pf = g.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
				phr = IDXGISwapChain1_Present(g.sc, 0, pf);
			}
			if (!once3) { once3 = 1; LOG("present returned %08lx", (unsigned long)phr); }
		}

		/* --- measure this frame and report twice a second --- */
		{
			LARGE_INTEGER after;
			double frame_ms;

			QueryPerformanceCounter(&after);
			if (last_present.QuadPart) {
				frame_ms = (double)(after.QuadPart - last_present.QuadPart) /
					   (double)freq.QuadPart * 1000.0;
				stats_add(&stats, frame_ms);
				if (g_target_fps > 0 && frame_ms > 1500.0 / g_target_fps)
					late++;
			}
			last_present = after;
			frames_since_report++;
			if (g_run_secs > 0 &&
			    (double)(after.QuadPart - loop_start.QuadPart) / (double)freq.QuadPart >=
				    (double)g_run_secs)
				g_quit = 1;
			{
				double since = (double)(after.QuadPart - last_report.QuadPart) /
					       (double)freq.QuadPart;
				if (since >= 0.5) {
					double avg, mn, mx, sd, p99, meas;
					char tgt[16], line[320];
					const char *pmode = g_vsync ? "VSYNC"
								    : (g.tearing ? "TEARING" : "no-tear");

					stats_summary(&stats, &avg, &mn, &mx, &sd, &p99);
					meas = frames_since_report / since;
					if (g_target_fps > 0)
						snprintf(tgt, sizeof(tgt), "%.0f", g_target_fps);
					else
						snprintf(tgt, sizeof(tgt), "UNCAP");
					snprintf(line, sizeof(line),
						 "fpsharness  target %s  measured %.0f fps  |  ms avg "
						 "%.3f min %.3f max %.3f p99 %.3f jitter %.3f late %lu  |  "
						 "%s  logic %.0fHz%s",
						 tgt, meas, avg, mn, mx, p99, sd, late, pmode, g_logic_hz,
						 g_paused ? "  [PAUSED]" : "");
					SetWindowTextA(hwnd, line);
					printf("target %5s  measured %6.1f  ms avg %.3f min %.3f max "
					       "%.3f p99 %.3f jitter %.3f late %lu  %s  logic %.0fHz\n",
					       tgt, meas, avg, mn, mx, p99, sd, late, pmode, g_logic_hz);
					LOG("target %5s measured %6.1f  ms avg %.3f min %.3f max %.3f "
					    "p99 %.3f jitter %.3f late %lu  %s  logic %.0fHz",
					    tgt, meas, avg, mn, mx, p99, sd, late, pmode, g_logic_hz);
					frames_since_report = 0;
					late = 0;
					last_report = after;
					memset(&stats, 0, sizeof(stats));
				}
			}
		}
	}

	LOG("quit");
	ID3D11DeviceContext_ClearState(g.ctx);
	if (g.cb) ID3D11Buffer_Release(g.cb);
	if (g.vb) ID3D11Buffer_Release(g.vb);
	if (g.layout) ID3D11InputLayout_Release(g.layout);
	if (g.ps) ID3D11PixelShader_Release(g.ps);
	if (g.vs) ID3D11VertexShader_Release(g.vs);
	if (g.rtv) ID3D11RenderTargetView_Release(g.rtv);
	if (g.sc) IDXGISwapChain1_Release(g.sc);
	ID3D11DeviceContext_Release(g.ctx);
	ID3D11Device_Release(g.dev);
	if (pacer.timer)
		CloseHandle(pacer.timer);
	timeEndPeriod(1);
	if (g_log)
		fclose(g_log);
	return 0;
}
