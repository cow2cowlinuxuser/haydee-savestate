/* gpuark.exe - can a live GPU's state be reconstructed from a CPU-side shadow
 * taken at an idle boundary, after every driver object is gone?
 *
 * This tests one claim, the one that decides whether the gpuhost approach can be
 * pointed at an emulator: that GPU-PRODUCED state (a rendertarget whose contents
 * the GPU wrote, and which later frames read back) cannot be captured and put
 * back from outside the driver - only re-derived by replaying history.
 *
 * The model of the emulator's hard case:
 *   - a static texture uploaded from the CPU (trivially shadowed: we keep bytes)
 *   - a ping-pong rendertarget pair where each frame advects and blends the
 *     PREVIOUS frame's output. After K frames the "current" target encodes the
 *     entire K-frame history; you cannot cheaply recompute it, only read it.
 *
 * Three runs over an identical, deterministic frame sequence:
 *   CONTROL   evolve 0..K+M on one device, hash the final target.
 *   FAITHFUL  evolve 0..K; read the current target back to CPU; DESTROY the
 *             device and every object; build a fresh device; recreate resources
 *             from the CPU shadow; re-install the read-back target; evolve
 *             K..K+M; hash.
 *   BROKEN    same as FAITHFUL but do NOT re-install the captured target (seed
 *             it with the frame-0 pattern instead) - proves the history matters.
 *
 * If FAITHFUL == CONTROL and BROKEN != CONTROL, then GPU-produced state IS
 * reconstructable from a CPU shadow taken at an idle boundary, and the blocker
 * for an emulator is the COST of that readback (measured here, and extrapolated
 * to VRAM scale), not its impossibility.
 *
 * Headless: no window, no swapchain. Loads System32's d3d11.dll by absolute path
 * so this repo's software d3d11.dll beside the .exe cannot shadow it.
 *
 * Build:
 *   zig cc -O2 -target x86_64-windows-gnu -o gpuark.exe gpuark.c -ldxguid -luser32
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef TEX
#define TEX 256		/* surface size; VRAM cost extrapolates from this */
#endif
#ifndef K_FRAMES
#define K_FRAMES 300	/* history depth before the save */
#endif
#ifndef M_FRAMES
#define M_FRAMES 120	/* frames after the restore, where divergence would show */
#endif

typedef HRESULT(WINAPI *CreateDeviceFn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
					const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
					D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
				   ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
				   ID3DBlob **);

static CreateDeviceFn g_create;
static CompileFn g_compile;

/* Everything a GPU state lives in. The whole point of the test is that on a
 * FAITHFUL restore we drop all of this and rebuild it from CPU-side bytes. */
typedef struct Gpu {
	ID3D11Device *dev;
	ID3D11DeviceContext *ctx;
	ID3D11VertexShader *vs;
	ID3D11PixelShader *ps;
	ID3D11SamplerState *samp;
	ID3D11Buffer *cb;
	ID3D11Texture2D *statTex;
	ID3D11ShaderResourceView *statSRV;
	ID3D11Texture2D *rt[2];
	ID3D11RenderTargetView *rtv[2];
	ID3D11ShaderResourceView *srv[2];
	ID3D11Texture2D *staging;
	int cur; /* which rt holds the live "current" content */
} Gpu;

static const char kVS[] =
	"struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
	"V main(uint id : SV_VertexID) {\n"
	"  V o; float2 t = float2((id << 1) & 2, id & 2);\n"
	"  o.uv = t; o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
	"  return o; }\n";

/* Advect the previous frame and blend in the static texture. The UV shift makes
 * frame K depend on the whole history, so the target cannot be recomputed
 * without the actual pixels - which is the property that makes an emulator's
 * rendertargets uncapturable by replay. p.x carries the absolute frame index so
 * the sequence is identical across runs. */
static const char kPS[] =
	"Texture2D prevT : register(t0);\n"
	"Texture2D statT : register(t1);\n"
	"SamplerState smp : register(s0);\n"
	"cbuffer C : register(b0) { float4 p; };\n"
	"float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
	"  float2 duv = float2(0.0037, 0.0021);\n"
	"  float4 pv = prevT.Sample(smp, uv + duv);\n"
	"  float4 st = statT.Sample(smp, uv);\n"
	"  float4 o = frac(pv * 0.985 + st * 0.02 + p.x * 0.0007);\n"
	"  o.a = 1.0; return o; }\n";

static ID3DBlob *compile(const char *src, size_t n, const char *entry, const char *target)
{
	ID3DBlob *code = NULL, *err = NULL;
	HRESULT hr = g_compile(src, n, "gpuark", NULL, NULL, entry, target,
			       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);

	if (FAILED(hr)) {
		printf("shader %s failed %08lx: %s\n", entry, (unsigned long)hr,
		       err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
		if (code)
			ID3D10Blob_Release(code);
		code = NULL;
	}
	if (err)
		ID3D10Blob_Release(err);
	return code;
}

/* The CPU-side pattern that seeds frame 0 and backs the static texture. Purely a
 * function of position, so every run produces the identical seed. */
static void fill_pattern(uint32_t *px)
{
	int y, x;

	for (y = 0; y < TEX; y++)
		for (x = 0; x < TEX; x++) {
			uint8_t r = (uint8_t)(x * 3 + y);
			uint8_t g = (uint8_t)(y * 5 + 17);
			uint8_t b = (uint8_t)((x ^ y) * 2 + 40);

			px[y * TEX + x] = 0xff000000u | (r << 16) | (g << 8) | b;
		}
}

/* Wait until the GPU has retired everything queued so far, so a timing that
 * follows measures only its own work and not the frames still in flight. */
static void gpu_idle(Gpu *g)
{
	D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
	ID3D11Query *q = NULL;
	BOOL done = FALSE;

	if (FAILED(ID3D11Device_CreateQuery(g->dev, &qd, &q)))
		return;
	ID3D11DeviceContext_End(g->ctx, (ID3D11Asynchronous *)q);
	ID3D11DeviceContext_Flush(g->ctx);
	while (ID3D11DeviceContext_GetData(g->ctx, (ID3D11Asynchronous *)q, &done, sizeof(done),
					   0) == S_FALSE)
		YieldProcessor();
	ID3D11Query_Release(q);
}

static int gpu_make(Gpu *g, const uint32_t *seed)
{
	D3D_FEATURE_LEVEL fl;
	D3D11_TEXTURE2D_DESC td;
	D3D11_SUBRESOURCE_DATA sd;
	D3D11_SAMPLER_DESC smp;
	D3D11_BUFFER_DESC bd;
	ID3DBlob *vsb, *psb;
	HRESULT hr;
	int i;

	memset(g, 0, sizeof(*g));
	hr = g_create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &g->dev,
		      &fl, &g->ctx);
	if (FAILED(hr))
		hr = g_create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &g->dev,
			      &fl, &g->ctx);
	if (FAILED(hr)) {
		printf("D3D11CreateDevice failed %08lx\n", (unsigned long)hr);
		return 0;
	}
	{
		static int said;
		IDXGIDevice *xd = NULL;
		IDXGIAdapter *ad = NULL;
		DXGI_ADAPTER_DESC ds;

		if (!said && SUCCEEDED(ID3D11Device_QueryInterface(g->dev, &IID_IDXGIDevice,
								   (void **)&xd))) {
			if (SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)) &&
			    SUCCEEDED(IDXGIAdapter_GetDesc(ad, &ds)))
				printf("  adapter: %ls, feature level %x\n", ds.Description,
				       (unsigned)fl);
			if (ad)
				IDXGIAdapter_Release(ad);
			IDXGIDevice_Release(xd);
			said = 1;
		}
	}

	vsb = compile(kVS, sizeof(kVS) - 1, "main", "vs_4_0");
	psb = compile(kPS, sizeof(kPS) - 1, "main", "ps_4_0");
	if (!vsb || !psb)
		return 0;
	ID3D11Device_CreateVertexShader(g->dev, ID3D10Blob_GetBufferPointer(vsb),
					ID3D10Blob_GetBufferSize(vsb), NULL, &g->vs);
	ID3D11Device_CreatePixelShader(g->dev, ID3D10Blob_GetBufferPointer(psb),
				       ID3D10Blob_GetBufferSize(psb), NULL, &g->ps);
	ID3D10Blob_Release(vsb);
	ID3D10Blob_Release(psb);

	/* Static texture: CPU-sourced, so its shadow is just the seed bytes. */
	memset(&td, 0, sizeof(td));
	td.Width = td.Height = TEX;
	td.MipLevels = td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	memset(&sd, 0, sizeof(sd));
	sd.pSysMem = seed;
	sd.SysMemPitch = TEX * 4;
	if (FAILED(ID3D11Device_CreateTexture2D(g->dev, &td, &sd, &g->statTex)))
		return 0;
	ID3D11Device_CreateShaderResourceView(g->dev, (ID3D11Resource *)g->statTex, NULL,
					      &g->statSRV);

	/* Ping-pong targets: GPU-produced, the surfaces the claim is about. */
	memset(&td, 0, sizeof(td));
	td.Width = td.Height = TEX;
	td.MipLevels = td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	for (i = 0; i < 2; i++) {
		if (FAILED(ID3D11Device_CreateTexture2D(g->dev, &td, NULL, &g->rt[i])))
			return 0;
		ID3D11Device_CreateRenderTargetView(g->dev, (ID3D11Resource *)g->rt[i], NULL,
						    &g->rtv[i]);
		ID3D11Device_CreateShaderResourceView(g->dev, (ID3D11Resource *)g->rt[i], NULL,
						      &g->srv[i]);
	}

	/* Staging surface for reading a target back to the CPU at the save. */
	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(ID3D11Device_CreateTexture2D(g->dev, &td, NULL, &g->staging)))
		return 0;

	memset(&smp, 0, sizeof(smp));
	smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	smp.MaxLOD = D3D11_FLOAT32_MAX;
	ID3D11Device_CreateSamplerState(g->dev, &smp, &g->samp);

	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = 16;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ID3D11Device_CreateBuffer(g->dev, &bd, NULL, &g->cb);

	g->cur = 0;
	return g->vs && g->ps && g->samp && g->cb && g->statSRV;
}

#define REL(p)                                          \
	do {                                            \
		if (p)                                  \
			IUnknown_Release((IUnknown *)(p)); \
		(p) = NULL;                             \
	} while (0)

static void gpu_free(Gpu *g)
{
	int i;

	if (g->ctx)
		ID3D11DeviceContext_ClearState(g->ctx);
	REL(g->staging);
	for (i = 0; i < 2; i++) {
		REL(g->srv[i]);
		REL(g->rtv[i]);
		REL(g->rt[i]);
	}
	REL(g->statSRV);
	REL(g->statTex);
	REL(g->cb);
	REL(g->samp);
	REL(g->ps);
	REL(g->vs);
	if (g->ctx) {
		ID3D11DeviceContext_Flush(g->ctx);
		ID3D11DeviceContext_Release(g->ctx);
	}
	REL(g->dev);
	memset(g, 0, sizeof(*g));
}

/* Overwrite a target's pixels from CPU bytes: seeds frame 0, and reinstalls the
 * read-back history on a FAITHFUL restore. */
static void upload_rt(Gpu *g, int slot, const uint32_t *px)
{
	ID3D11DeviceContext_UpdateSubresource(g->ctx, (ID3D11Resource *)g->rt[slot], 0, NULL, px,
					      TEX * 4, 0);
}

static void step(Gpu *g, int frame)
{
	int src = g->cur, dst = g->cur ^ 1;
	D3D11_VIEWPORT vp = { 0, 0, TEX, TEX, 0, 1 };
	D3D11_MAPPED_SUBRESOURCE m;
	ID3D11ShaderResourceView *srvs[2], *none[2] = { NULL, NULL };

	if (SUCCEEDED(ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource *)g->cb, 0,
					      D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		float *p = (float *)m.pData;
		p[0] = (float)frame;
		p[1] = p[2] = p[3] = 0.0f;
		ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource *)g->cb, 0);
	}
	ID3D11DeviceContext_OMSetRenderTargets(g->ctx, 1, &g->rtv[dst], NULL);
	ID3D11DeviceContext_RSSetViewports(g->ctx, 1, &vp);
	ID3D11DeviceContext_IASetInputLayout(g->ctx, NULL);
	ID3D11DeviceContext_IASetPrimitiveTopology(g->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_VSSetShader(g->ctx, g->vs, NULL, 0);
	ID3D11DeviceContext_PSSetShader(g->ctx, g->ps, NULL, 0);
	srvs[0] = g->srv[src];
	srvs[1] = g->statSRV;
	ID3D11DeviceContext_PSSetShaderResources(g->ctx, 0, 2, srvs);
	ID3D11DeviceContext_PSSetSamplers(g->ctx, 0, 1, &g->samp);
	ID3D11DeviceContext_PSSetConstantBuffers(g->ctx, 0, 1, &g->cb);
	ID3D11DeviceContext_Draw(g->ctx, 3, 0);
	ID3D11DeviceContext_PSSetShaderResources(g->ctx, 0, 2, none); /* unbind for next ping */
	g->cur = dst;
}

static void evolve(Gpu *g, int start, int n)
{
	int i;

	for (i = 0; i < n; i++)
		step(g, start + i);
}

/* Read the live target to CPU, tightly packed. This is the "save" of GPU state;
 * bytes_out and its time are the cost the emulator would pay per surface. */
static int readback(Gpu *g, uint32_t *out)
{
	D3D11_MAPPED_SUBRESOURCE m;
	int y;

	ID3D11DeviceContext_CopyResource(g->ctx, (ID3D11Resource *)g->staging,
					 (ID3D11Resource *)g->rt[g->cur]);
	if (FAILED(ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource *)g->staging, 0, D3D11_MAP_READ,
					   0, &m)))
		return 0;
	for (y = 0; y < TEX; y++)
		memcpy(out + y * TEX, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, TEX * 4);
	ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource *)g->staging, 0);
	return 1;
}

static uint64_t hash_current(Gpu *g)
{
	static uint32_t buf[TEX * TEX];
	uint64_t h = 1469598103934665603ull;
	int i;

	if (!readback(g, buf))
		return 0;
	for (i = 0; i < TEX * TEX; i++) {
		h ^= buf[i];
		h *= 1099511628211ull;
	}
	return h;
}

int main(void)
{
	HMODULE dc;
	char sys[MAX_PATH], path[MAX_PATH];
	UINT n;
	Gpu g;
	static uint32_t seed[TEX * TEX];
	static uint32_t cap[TEX * TEX];
	uint64_t h_control, h_faithful, h_broken;
	int cap_slot, cap_frame;
	LARGE_INTEGER f, t0, t1;

	QueryPerformanceFrequency(&f);
	n = GetSystemDirectoryA(sys, sizeof(sys));
	if (!n || n >= sizeof(sys))
		return 1;
	snprintf(path, sizeof(path), "%s\\dxgi.dll", sys);
	LoadLibraryA(path);
	snprintf(path, sizeof(path), "%s\\d3d11.dll", sys);
	dc = LoadLibraryA(path);
	g_create = dc ? (CreateDeviceFn)(void *)GetProcAddress(dc, "D3D11CreateDevice") : NULL;
	dc = LoadLibraryA("d3dcompiler_47.dll");
	g_compile = dc ? (CompileFn)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
	if (!g_create || !g_compile) {
		printf("could not resolve D3D11CreateDevice / D3DCompile\n");
		return 1;
	}
	fill_pattern(seed);

	printf("gpuark: reconstruct GPU-produced state from a CPU shadow?\n");
	printf("  surface %dx%d RGBA8 (%zu KB), history K=%d, post-restore M=%d\n\n", TEX, TEX,
	       (size_t)TEX * TEX * 4 / 1024, K_FRAMES, M_FRAMES);

	/* CONTROL: one device, straight through. */
	if (!gpu_make(&g, seed))
		return 2;
	upload_rt(&g, 0, seed);
	g.cur = 0;
	evolve(&g, 0, K_FRAMES + M_FRAMES);
	h_control = hash_current(&g);
	gpu_free(&g);
	printf("CONTROL   evolve 0..%d straight            -> %016llx\n", K_FRAMES + M_FRAMES,
	       (unsigned long long)h_control);

	/* FAITHFUL: evolve to K, capture the live target, DESTROY everything, rebuild
	 * from the CPU shadow, reinstall the captured target, evolve the rest. */
	if (!gpu_make(&g, seed))
		return 2;
	upload_rt(&g, 0, seed);
	g.cur = 0;
	evolve(&g, 0, K_FRAMES);
	gpu_idle(&g);
	QueryPerformanceCounter(&t0);
	readback(&g, cap);
	QueryPerformanceCounter(&t1);
	cap_slot = g.cur;
	cap_frame = K_FRAMES;
	gpu_free(&g); /* every driver object is now gone */
	{
		LARGE_INTEGER r0, r1;
		QueryPerformanceCounter(&r0);
		if (!gpu_make(&g, seed))
			return 2;
		upload_rt(&g, cap_slot, cap); /* reinstall history from CPU bytes */
		g.cur = cap_slot;
		gpu_idle(&g);
		QueryPerformanceCounter(&r1);
		evolve(&g, cap_frame, M_FRAMES);
		h_faithful = hash_current(&g);
		gpu_free(&g);
		printf("FAITHFUL  save@%d, teardown, rebuild, %d..%d -> %016llx  %s\n", K_FRAMES,
		       K_FRAMES, K_FRAMES + M_FRAMES, (unsigned long long)h_faithful,
		       h_faithful == h_control ? "== CONTROL (bit-exact)" : "!= CONTROL");
		printf("            readback %.3f ms, rebuild %.3f ms for %zu KB\n",
		       (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart,
		       (double)(r1.QuadPart - r0.QuadPart) * 1000.0 / (double)f.QuadPart,
		       (size_t)TEX * TEX * 4 / 1024);
	}

	/* BROKEN: identical restore but the captured history is NOT reinstalled -
	 * the target is seeded with the frame-0 pattern. Must diverge, or the test
	 * would pass even when the GPU-produced state was thrown away. */
	if (!gpu_make(&g, seed))
		return 2;
	upload_rt(&g, 0, seed);
	g.cur = 0;
	evolve(&g, 0, K_FRAMES);
	gpu_free(&g);
	if (!gpu_make(&g, seed))
		return 2;
	upload_rt(&g, cap_slot, seed); /* wrong: frame-0 content, not the history */
	g.cur = cap_slot;
	evolve(&g, cap_frame, M_FRAMES);
	h_broken = hash_current(&g);
	gpu_free(&g);
	printf("BROKEN    same restore, history DISCARDED  -> %016llx  %s\n\n",
	       (unsigned long long)h_broken, h_broken != h_control ? "!= CONTROL (as expected)"
								   : "== CONTROL (test is degenerate!)");

	printf("VERDICT: ");
	if (h_faithful == h_control && h_broken != h_control) {
		double mb = (double)TEX * TEX * 4 / (1024.0 * 1024.0);
		double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;

		printf("GPU-produced state WAS reconstructed bit-exact from a CPU shadow\n");
		printf("         taken at an idle boundary. The blocker is COST, not "
		       "possibility.\n");
		if (ms > 0.0)
			printf("         readback ~%.0f MB/s; 2 GB of live VRAM ~= %.1f s per save.\n",
			       mb / (ms / 1000.0), 2048.0 / (mb / (ms / 1000.0)));
		if (mb < 4.0)
			printf("         (small surface: MB/s here is latency-bound, not "
			       "bandwidth - rebuild with -DTEX=2048 for a real figure.)\n");
		return 0;
	}
	if (h_faithful != h_control)
		printf("FAITHFUL diverged - reconstruction was NOT sufficient. Claim stands.\n");
	else
		printf("BROKEN matched - the test does not actually exercise history.\n");
	return 3;
}
