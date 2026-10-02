/* gpuhost64.exe - presents the software renderer's frames through a real GPU,
 * from outside the game's process. See gpuhost.h for why it is a process.
 *
 *   gpuhost64.exe <mapping name> <game pid>
 *
 * The renderer launches it and hands over the rest through the mapping: the
 * window, the scaling wanted, and the frames. It stays until the game exits,
 * the renderer sets quit, or something fails, and on the way out it releases
 * the swap chain so the window falls back to GDI.
 *
 * gpuhost.hlsl beside this executable, when present, replaces the built-in
 * pixel shader - the same idea as an emulator's shader presets. It sees:
 *
 *   Texture2D Source : register(t0);    the finished frame
 *   SamplerState Samp : register(s0);   point or linear, per D3D11SW_SCALE
 *   cbuffer Frame : register(b0) {
 *       float4 SourceSize;              w, h, 1/w, 1/h of the frame
 *       float4 OutputSize;              the same for the area drawn into
 *       float4 FrameCount;              x = frames shown
 *   };
 *   float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
 *
 * A shader that fails to compile is reported in gpuhost.log and the built-in
 * one is used instead, so a typo costs the effect and not the picture. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "gpuhost.h"

static FILE *g_log;
static GhHeader *g_h;

static void say(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	SYSTEMTIME t;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	GetLocalTime(&t);
	if (g_log) {
		fprintf(g_log, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond,
			t.wMilliseconds, line);
		fflush(g_log);
	}
	if (g_h)
		lstrcpynA(g_h->msg, line, sizeof(g_h->msg));
}

static const char kVS[] =
	"struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
	"V main(uint id : SV_VertexID) {\n"
	"  V o; float2 t = float2((id << 1) & 2, id & 2);\n"
	"  o.uv = t; o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
	"  return o; }\n";

static const char kPS[] =
	"Texture2D Source : register(t0);\n"
	"SamplerState Samp : register(s0);\n"
	"cbuffer Frame : register(b0) { float4 SourceSize; float4 OutputSize; float4 FrameCount; };\n"
	"float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
	"  return float4(Source.Sample(Samp, uv).rgb, 1); }\n";

typedef HRESULT(WINAPI *CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
				   ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
				   ID3DBlob **);

static ID3DBlob *compile(CompileFn fn, const char *src, size_t len, const char *name,
			 const char *target)
{
	ID3DBlob *code = NULL, *err = NULL;
	HRESULT hr = fn(src, len, name, NULL, NULL, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
			0, &code, &err);

	if (FAILED(hr)) {
		say("shader %s failed to compile (%08lx): %s", name, (unsigned long)hr,
		    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "no message");
		if (code)
			ID3D10Blob_Release(code);
		code = NULL;
	}
	if (err)
		ID3D10Blob_Release(err);
	return code;
}

static char *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = n > 0 ? (char *)malloc((size_t)n + 1) : NULL;
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	if (buf) {
		buf[n] = 0;
		*len = (size_t)n;
	}
	return buf;
}

/* Mode 2's draw shaders: gpu_quad.hlsl, which is the in-process backend's
 * pipeline, so both GPU paths draw the same picture. */
static const char kDrawHLSL[] =
	"cbuffer Xform : register(b0) { float4 gXform; };\n"
	"cbuffer Ctl : register(b1) { float4 gCtl; };\n"
	"struct VIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
	"struct VOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
	"VOut vs_main(VIn i) { VOut o;\n"
	"  o.pos = float4(i.pos.x * gXform.x + gXform.z, i.pos.y * gXform.y + gXform.w, 0, 1);\n"
	"  o.uv = i.uv; o.col = i.col; return o; }\n"
	"Texture2D gTex : register(t0);\n"
	"SamplerState gSmp : register(s0);\n"
	"float4 ps_main(VOut i) : SV_TARGET {\n"
	"  float4 c = gTex.Sample(gSmp, i.uv);\n"
	"  if (gCtl.y > 1.0f) c.a = saturate((c.a - 0.5f) * gCtl.y + 0.5f);\n"
	"  c *= i.col;\n"
	"  if (gCtl.z != 0.0f) c.rgb = lerp(float3(1, 1, 1), c.rgb, c.a);\n"
	"  if (gCtl.x > 0.0f) clip(c.a - gCtl.x);\n"
	"  return c; }\n";

typedef struct HVert {
	float x, y, u, v;
	float r, g, b, a;
} HVert;

#define VB_VERTS 65536

typedef struct HTex {
	ID3D11Texture2D *tex;
	ID3D11ShaderResourceView *srv;
	UINT w, h;
} HTex;

typedef struct Gfx {
	ID3D11Device *dev;
	ID3D11DeviceContext *ctx;
	IDXGISwapChain1 *sc;
	ID3D11RenderTargetView *rtv;
	ID3D11VertexShader *vs;
	ID3D11PixelShader *ps;
	ID3D11SamplerState *samp;
	ID3D11Buffer *cb;
	ID3D11Texture2D *tex;
	ID3D11ShaderResourceView *srv;
	UINT tw, th, cw, ch;
	/* What the present pass shows: the slot texture or the draw target. */
	ID3D11ShaderResourceView *show;
	UINT sw, sh;
	/* Mode 2. */
	ID3D11VertexShader *dvs;
	ID3D11PixelShader *dps;
	ID3D11InputLayout *layout;
	ID3D11Buffer *xcb, *ctlcb, *vb;
	UINT vb_used;
	float ctl[4];
	ID3D11BlendState *blend[GH_BLEND_N];
	ID3D11SamplerState *dsmp[2];
	ID3D11RasterizerState *rs[2];
	ID3D11Texture2D *rt, *stage;
	ID3D11RenderTargetView *rt_rtv;
	ID3D11ShaderResourceView *rt_srv;
	UINT rt_w, rt_h;
	int in_frame;
	HTex *texs;
	uint32_t ntex;
} Gfx;

static HRESULT draw_init(Gfx *g, CompileFn fn);

static void rtv_make(Gfx *g)
{
	ID3D11Texture2D *bb = NULL;

	if (SUCCEEDED(IDXGISwapChain1_GetBuffer(g->sc, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
		ID3D11Device_CreateRenderTargetView(g->dev, (ID3D11Resource *)bb, NULL, &g->rtv);
		ID3D11Texture2D_Release(bb);
	}
}

static void client_size(HWND hwnd, UINT *w, UINT *h)
{
	RECT rc;

	GetClientRect(hwnd, &rc);
	*w = rc.right > rc.left ? (UINT)(rc.right - rc.left) : 1;
	*h = rc.bottom > rc.top ? (UINT)(rc.bottom - rc.top) : 1;
}

static HRESULT gfx_init(Gfx *g, HWND hwnd, const char *dir)
{
	IDXGIDevice *xd = NULL;
	IDXGIAdapter *ad = NULL;
	IDXGIFactory2 *fac = NULL;
	DXGI_SWAP_CHAIN_DESC1 d;
	D3D11_SAMPLER_DESC sd;
	D3D11_BUFFER_DESC bd;
	HMODULE dc;
	CompileFn fn;
	ID3DBlob *vsb, *psb = NULL;
	char path[MAX_PATH];
	char *user;
	size_t ulen = 0;
	HRESULT hr;

	hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
			       D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
			       &g->dev, NULL, &g->ctx);
	if (FAILED(hr)) {
		say("D3D11CreateDevice failed %08lx", (unsigned long)hr);
		return hr;
	}
	ID3D11Device_QueryInterface(g->dev, &IID_IDXGIDevice, (void **)&xd);
	IDXGIDevice_GetAdapter(xd, &ad);
	IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
	{
		DXGI_ADAPTER_DESC ade;
		char name[128];

		IDXGIAdapter_GetDesc(ad, &ade);
		WideCharToMultiByte(CP_UTF8, 0, ade.Description, -1, name, sizeof(name), NULL, NULL);
		say("adapter: %s", name);
	}
	IDXGIAdapter_Release(ad);
	IDXGIDevice_Release(xd);

	client_size(hwnd, &g->cw, &g->ch);
	memset(&d, 0, sizeof(d));
	d.Width = g->cw;
	d.Height = g->ch;
	d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	d.SampleDesc.Count = 1;
	d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	d.BufferCount = 2;
	d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)g->dev, hwnd, &d, NULL, NULL,
						  &g->sc);
	if (FAILED(hr)) {
		say("CreateSwapChainForHwnd on the game window failed %08lx", (unsigned long)hr);
		IDXGIFactory2_Release(fac);
		return hr;
	}
	/* The window, its size and its fullscreen state are the game's business
	 * and the renderer's; DXGI reacting to Alt+Enter would fight both. */
	IDXGIFactory2_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
	IDXGIFactory2_Release(fac);
	rtv_make(g);

	dc = LoadLibraryA("d3dcompiler_47.dll");
	fn = dc ? (CompileFn)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
	if (!fn) {
		say("d3dcompiler_47.dll not available");
		return E_FAIL;
	}
	vsb = compile(fn, kVS, sizeof(kVS) - 1, "builtin_vs", "vs_4_0");
	snprintf(path, sizeof(path), "%s\\gpuhost.hlsl", dir);
	user = read_file(path, &ulen);
	if (user) {
		psb = compile(fn, user, ulen, path, "ps_4_0");
		free(user);
		if (psb) {
			say("using %s", path);
			g_h->shader_user = 1;
		} else {
			say("falling back to the built-in shader");
		}
	}
	if (!psb)
		psb = compile(fn, kPS, sizeof(kPS) - 1, "builtin_ps", "ps_4_0");
	if (!vsb || !psb)
		return E_FAIL;
	hr = ID3D11Device_CreateVertexShader(g->dev, ID3D10Blob_GetBufferPointer(vsb),
					     ID3D10Blob_GetBufferSize(vsb), NULL, &g->vs);
	if (SUCCEEDED(hr))
		hr = ID3D11Device_CreatePixelShader(g->dev, ID3D10Blob_GetBufferPointer(psb),
						    ID3D10Blob_GetBufferSize(psb), NULL, &g->ps);
	ID3D10Blob_Release(vsb);
	ID3D10Blob_Release(psb);
	if (FAILED(hr)) {
		say("shader creation failed %08lx", (unsigned long)hr);
		return hr;
	}
	if (g_h->mode == 2) {
		IDXGIDevice1 *xd1 = NULL;

		hr = draw_init(g, fn);
		if (FAILED(hr))
			return hr;
		/* One frame queued, not three: the renderer already paces itself,
		 * and every frame the driver holds is a frame of input lag. */
		if (SUCCEEDED(ID3D11Device_QueryInterface(g->dev, &IID_IDXGIDevice1, (void **)&xd1))) {
			IDXGIDevice1_SetMaximumFrameLatency(xd1, 1);
			IDXGIDevice1_Release(xd1);
		}
	}

	memset(&sd, 0, sizeof(sd));
	sd.Filter = g_h->point ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	ID3D11Device_CreateSamplerState(g->dev, &sd, &g->samp);

	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = 48;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ID3D11Device_CreateBuffer(g->dev, &bd, NULL, &g->cb);
	return S_OK;
}

static int tex_ensure(Gfx *g, UINT w, UINT h)
{
	D3D11_TEXTURE2D_DESC td;

	if (g->tex && g->tw == w && g->th == h)
		return 1;
	if (g->srv)
		ID3D11ShaderResourceView_Release(g->srv);
	if (g->tex)
		ID3D11Texture2D_Release(g->tex);
	g->srv = NULL;
	g->tex = NULL;
	memset(&td, 0, sizeof(td));
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DYNAMIC;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(ID3D11Device_CreateTexture2D(g->dev, &td, NULL, &g->tex)))
		return 0;
	ID3D11Device_CreateShaderResourceView(g->dev, (ID3D11Resource *)g->tex, NULL, &g->srv);
	g->tw = w;
	g->th = h;
	say("frame texture %ux%u", w, h);
	return 1;
}

static void upload(Gfx *g, const uint32_t *px, UINT w, UINT h)
{
	D3D11_MAPPED_SUBRESOURCE m;
	UINT y;

	if (FAILED(ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource *)g->tex, 0,
					   D3D11_MAP_WRITE_DISCARD, 0, &m)))
		return;
	for (y = 0; y < h; y++)
		memcpy((uint8_t *)m.pData + (size_t)y * m.RowPitch, px + (size_t)y * w, (size_t)w * 4);
	ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource *)g->tex, 0);
}

/* The same fit the GDI path makes: letterboxed, optionally whole multiples. */
static void fit(UINT cw, UINT ch, UINT w, UINT h, int integer, D3D11_VIEWPORT *vp)
{
	UINT dw = cw, dh = (UINT)(((unsigned long long)cw * h) / w);

	if (dh > ch) {
		dh = ch;
		dw = (UINT)(((unsigned long long)ch * w) / h);
	}
	/* Below one whole multiple there is no integer scale that fits, and
	 * taking k = 1 anyway would crop the frame; plain fit is the lesser evil. */
	if (integer) {
		UINT k = cw / w, ky = ch / h;

		if (ky < k)
			k = ky;
		if (k >= 1) {
			dw = w * k;
			dh = h * k;
		}
	}
	vp->TopLeftX = (float)(((int)cw - (int)dw) / 2);
	vp->TopLeftY = (float)(((int)ch - (int)dh) / 2);
	vp->Width = (float)dw;
	vp->Height = (float)dh;
	vp->MinDepth = 0.0f;
	vp->MaxDepth = 1.0f;
}

static HRESULT draw(Gfx *g, HWND hwnd)
{
	static const float black[4] = { 0, 0, 0, 1 };
	D3D11_VIEWPORT vp;
	D3D11_MAPPED_SUBRESOURCE m;
	UINT cw, ch;

	client_size(hwnd, &cw, &ch);
	if (cw != g->cw || ch != g->ch) {
		HRESULT hr;

		ID3D11DeviceContext_OMSetRenderTargets(g->ctx, 0, NULL, NULL);
		if (g->rtv)
			ID3D11RenderTargetView_Release(g->rtv);
		g->rtv = NULL;
		hr = IDXGISwapChain1_ResizeBuffers(g->sc, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
		if (FAILED(hr)) {
			say("ResizeBuffers %ux%u failed %08lx", cw, ch, (unsigned long)hr);
			return hr;
		}
		g->cw = cw;
		g->ch = ch;
		rtv_make(g);
	}
	if (!g->rtv || !g->show)
		return S_OK;
	fit(cw, ch, g->sw, g->sh, (int)g_h->integer, &vp);
	if (SUCCEEDED(ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource *)g->cb, 0,
					      D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		float *c = (float *)m.pData;

		c[0] = (float)g->sw;
		c[1] = (float)g->sh;
		c[2] = 1.0f / (float)g->sw;
		c[3] = 1.0f / (float)g->sh;
		c[4] = vp.Width;
		c[5] = vp.Height;
		c[6] = 1.0f / vp.Width;
		c[7] = 1.0f / vp.Height;
		c[8] = (float)g_h->shown;
		c[9] = c[10] = c[11] = 0.0f;
		ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource *)g->cb, 0);
	}
	ID3D11DeviceContext_OMSetRenderTargets(g->ctx, 1, &g->rtv, NULL);
	ID3D11DeviceContext_ClearRenderTargetView(g->ctx, g->rtv, black);
	ID3D11DeviceContext_RSSetViewports(g->ctx, 1, &vp);
	ID3D11DeviceContext_RSSetState(g->ctx, NULL);
	ID3D11DeviceContext_OMSetBlendState(g->ctx, NULL, NULL, 0xffffffffu);
	ID3D11DeviceContext_IASetPrimitiveTopology(g->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_IASetInputLayout(g->ctx, NULL);
	ID3D11DeviceContext_VSSetShader(g->ctx, g->vs, NULL, 0);
	ID3D11DeviceContext_PSSetShader(g->ctx, g->ps, NULL, 0);
	ID3D11DeviceContext_PSSetShaderResources(g->ctx, 0, 1, &g->show);
	ID3D11DeviceContext_PSSetSamplers(g->ctx, 0, 1, &g->samp);
	ID3D11DeviceContext_PSSetConstantBuffers(g->ctx, 0, 1, &g->cb);
	ID3D11DeviceContext_Draw(g->ctx, 3, 0);
	{
		ID3D11ShaderResourceView *none = NULL;

		/* The draw target is bound as a source here and as the target
		 * next frame; D3D unbinds one silently if both are set. */
		ID3D11DeviceContext_PSSetShaderResources(g->ctx, 0, 1, &none);
	}
	return IDXGISwapChain1_Present(g->sc, 1, 0);
}

/* --- mode 2: replaying the renderer's draws --- */

static ID3D11BlendState *blend_make(Gfx *g, D3D11_BLEND s, D3D11_BLEND d, D3D11_BLEND_OP op, int on)
{
	D3D11_BLEND_DESC bd;
	ID3D11BlendState *st = NULL;

	memset(&bd, 0, sizeof(bd));
	bd.RenderTarget[0].BlendEnable = on;
	bd.RenderTarget[0].SrcBlend = on ? s : D3D11_BLEND_ONE;
	bd.RenderTarget[0].DestBlend = on ? d : D3D11_BLEND_ZERO;
	bd.RenderTarget[0].BlendOp = op;
	/* As gpu.c: nothing reads the backbuffer's alpha, and the colour factors
	 * would darken it under a multiply for no one's benefit. */
	bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	ID3D11Device_CreateBlendState(g->dev, &bd, &st);
	return st;
}

static HRESULT draw_init(Gfx *g, CompileFn fn)
{
	static const D3D11_INPUT_ELEMENT_DESC el[3] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 }
	};
	ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
	D3D11_BUFFER_DESC bd;
	D3D11_SAMPLER_DESC sd;
	D3D11_RASTERIZER_DESC rd;
	HRESULT hr;
	int i;

	hr = fn(kDrawHLSL, sizeof(kDrawHLSL) - 1, "draw", NULL, NULL, "vs_main", "vs_4_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vb, &err);
	if (SUCCEEDED(hr))
		hr = fn(kDrawHLSL, sizeof(kDrawHLSL) - 1, "draw", NULL, NULL, "ps_main", "ps_4_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pb, &err);
	if (FAILED(hr)) {
		say("draw shaders failed to compile (%08lx): %s", (unsigned long)hr,
		    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "no message");
		goto out;
	}
	hr = ID3D11Device_CreateVertexShader(g->dev, ID3D10Blob_GetBufferPointer(vb),
					     ID3D10Blob_GetBufferSize(vb), NULL, &g->dvs);
	if (SUCCEEDED(hr))
		hr = ID3D11Device_CreatePixelShader(g->dev, ID3D10Blob_GetBufferPointer(pb),
						    ID3D10Blob_GetBufferSize(pb), NULL, &g->dps);
	if (SUCCEEDED(hr))
		hr = ID3D11Device_CreateInputLayout(g->dev, el, 3, ID3D10Blob_GetBufferPointer(vb),
						    ID3D10Blob_GetBufferSize(vb), &g->layout);
	if (FAILED(hr)) {
		say("draw pipeline refused %08lx", (unsigned long)hr);
		goto out;
	}
	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = 16;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ID3D11Device_CreateBuffer(g->dev, &bd, NULL, &g->xcb);
	ID3D11Device_CreateBuffer(g->dev, &bd, NULL, &g->ctlcb);
	bd.ByteWidth = VB_VERTS * sizeof(HVert);
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	ID3D11Device_CreateBuffer(g->dev, &bd, NULL, &g->vb);
	g->ctl[0] = -1.0f; /* forces the first upload */

	g->blend[GH_BLEND_OFF] = blend_make(g, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, 0);
	g->blend[GH_BLEND_OVER] = blend_make(g, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA,
					     D3D11_BLEND_OP_ADD, 1);
	g->blend[GH_BLEND_ADD] = blend_make(g, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, 1);
	g->blend[GH_BLEND_MUL] = blend_make(g, D3D11_BLEND_ZERO, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_OP_ADD, 1);
	g->blend[GH_BLEND_RSUB] = blend_make(g, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE,
					     D3D11_BLEND_OP_REV_SUBTRACT, 1);

	memset(&sd, 0, sizeof(sd));
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	ID3D11Device_CreateSamplerState(g->dev, &sd, &g->dsmp[0]);
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	ID3D11Device_CreateSamplerState(g->dev, &sd, &g->dsmp[1]);

	/* Culling off: the game's quads come in whichever winding the sprite's
	 * flip gave them, and the rasteriser never cared. */
	memset(&rd, 0, sizeof(rd));
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	ID3D11Device_CreateRasterizerState(g->dev, &rd, &g->rs[0]);
	rd.ScissorEnable = TRUE;
	ID3D11Device_CreateRasterizerState(g->dev, &rd, &g->rs[1]);

	hr = S_OK;
	for (i = 0; i < GH_BLEND_N; i++)
		if (!g->blend[i])
			hr = E_FAIL;
	if (!g->xcb || !g->ctlcb || !g->vb || !g->dsmp[0] || !g->dsmp[1] || !g->rs[0] || !g->rs[1])
		hr = E_FAIL;
	if (FAILED(hr))
		say("a buffer, blend, sampler or rasteriser state was refused");
out:
	if (vb)
		ID3D10Blob_Release(vb);
	if (pb)
		ID3D10Blob_Release(pb);
	if (err)
		ID3D10Blob_Release(err);
	return hr;
}

static HTex *tex_get(Gfx *g, uint32_t id)
{
	if (id >= g->ntex) {
		uint32_t n = g->ntex ? g->ntex : 1024;
		HTex *t;

		while (n <= id)
			n *= 2;
		t = (HTex *)realloc(g->texs, (size_t)n * sizeof(HTex));
		if (!t)
			return NULL;
		memset(t + g->ntex, 0, (size_t)(n - g->ntex) * sizeof(HTex));
		g->texs = t;
		g->ntex = n;
	}
	return &g->texs[id];
}

static void tex_free(HTex *t)
{
	if (t->srv)
		ID3D11ShaderResourceView_Release(t->srv);
	if (t->tex)
		ID3D11Texture2D_Release(t->tex);
	memset(t, 0, sizeof(*t));
}

static void op_tex_def(Gfx *g, const GhTexDef *d)
{
	HTex *t = tex_get(g, d->id);
	D3D11_TEXTURE2D_DESC td;

	if (!t || !d->w || !d->h)
		return;
	if (t->tex && t->w == d->w && t->h == d->h)
		return;
	tex_free(t);
	memset(&td, 0, sizeof(td));
	td.Width = d->w;
	td.Height = d->h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(ID3D11Device_CreateTexture2D(g->dev, &td, NULL, &t->tex)) ||
	    FAILED(ID3D11Device_CreateShaderResourceView(g->dev, (ID3D11Resource *)t->tex, NULL,
							  &t->srv))) {
		say("texture %u (%ux%u) refused", d->id, d->w, d->h);
		tex_free(t);
		return;
	}
	t->w = d->w;
	t->h = d->h;
	InterlockedIncrement((volatile LONG *)&g_h->gpu_textures);
}

static void op_tex_rows(Gfx *g, const GhTexRows *r, const void *px)
{
	HTex *t = r->id < g->ntex ? &g->texs[r->id] : NULL;
	D3D11_BOX box;

	if (!t || !t->tex || r->w != t->w || r->y0 + r->rows > t->h)
		return;
	box.left = 0;
	box.right = r->w;
	box.top = r->y0;
	box.bottom = r->y0 + r->rows;
	box.front = 0;
	box.back = 1;
	ID3D11DeviceContext_UpdateSubresource(g->ctx, (ID3D11Resource *)t->tex, 0, &box, px,
					      r->w * 4, 0);
}

static int rt_ensure(Gfx *g, UINT w, UINT h)
{
	D3D11_TEXTURE2D_DESC td;

	if (g->rt && g->rt_w == w && g->rt_h == h)
		return 1;
	if (g->show == g->rt_srv)
		g->show = NULL;
	if (g->rt_srv)
		ID3D11ShaderResourceView_Release(g->rt_srv);
	if (g->rt_rtv)
		ID3D11RenderTargetView_Release(g->rt_rtv);
	if (g->rt)
		ID3D11Texture2D_Release(g->rt);
	if (g->stage)
		ID3D11Texture2D_Release(g->stage);
	g->rt_srv = NULL;
	g->rt_rtv = NULL;
	g->rt = NULL;
	g->stage = NULL;
	memset(&td, 0, sizeof(td));
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(ID3D11Device_CreateTexture2D(g->dev, &td, NULL, &g->rt)) ||
	    FAILED(ID3D11Device_CreateRenderTargetView(g->dev, (ID3D11Resource *)g->rt, NULL,
							&g->rt_rtv)) ||
	    FAILED(ID3D11Device_CreateShaderResourceView(g->dev, (ID3D11Resource *)g->rt, NULL,
							  &g->rt_srv))) {
		say("draw target %ux%u refused", w, h);
		return 0;
	}
	g->rt_w = w;
	g->rt_h = h;
	say("draw target %ux%u", w, h);
	return 1;
}

static void op_frame_begin(Gfx *g, const GhFrameBegin *f)
{
	D3D11_VIEWPORT vp;
	D3D11_MAPPED_SUBRESOURCE m;
	UINT stride = sizeof(HVert), offset = 0;

	g->in_frame = 0;
	if (!f->w || !f->h || !rt_ensure(g, f->w, f->h))
		return;
	ID3D11DeviceContext_OMSetRenderTargets(g->ctx, 1, &g->rt_rtv, NULL);
	vp.TopLeftX = vp.TopLeftY = 0.0f;
	vp.Width = (float)f->w;
	vp.Height = (float)f->h;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	ID3D11DeviceContext_RSSetViewports(g->ctx, 1, &vp);
	ID3D11DeviceContext_IASetInputLayout(g->ctx, g->layout);
	ID3D11DeviceContext_IASetPrimitiveTopology(g->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_IASetVertexBuffers(g->ctx, 0, 1, &g->vb, &stride, &offset);
	ID3D11DeviceContext_VSSetShader(g->ctx, g->dvs, NULL, 0);
	ID3D11DeviceContext_VSSetConstantBuffers(g->ctx, 0, 1, &g->xcb);
	ID3D11DeviceContext_PSSetShader(g->ctx, g->dps, NULL, 0);
	ID3D11DeviceContext_PSSetConstantBuffers(g->ctx, 1, 1, &g->ctlcb);
	if (SUCCEEDED(ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource *)g->xcb, 0,
					      D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		float *x = (float *)m.pData;

		x[0] = 2.0f / (float)f->w;
		x[1] = -2.0f / (float)f->h;
		x[2] = -1.0f;
		x[3] = 1.0f;
		ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource *)g->xcb, 0);
	}
	if (f->clear) {
		float c[4];

		c[0] = (float)((f->argb >> 16) & 255) / 255.0f;
		c[1] = (float)((f->argb >> 8) & 255) / 255.0f;
		c[2] = (float)(f->argb & 255) / 255.0f;
		c[3] = (float)((f->argb >> 24) & 255) / 255.0f;
		ID3D11DeviceContext_ClearRenderTargetView(g->ctx, g->rt_rtv, c);
	}
	g->in_frame = 1;
}

static void vert_out(HVert *o, const GhVert *v, float shift)
{
	uint32_t c = v->color;

	o->x = v->x - shift;
	o->y = v->y - shift;
	o->u = v->u;
	o->v = v->v;
	o->r = (float)((c >> 16) & 255) * (1.0f / 255.0f);
	o->g = (float)((c >> 8) & 255) * (1.0f / 255.0f);
	o->b = (float)(c & 255) * (1.0f / 255.0f);
	o->a = (float)((c >> 24) & 255) * (1.0f / 255.0f);
}

static void op_draw(Gfx *g, const GhDraw *d, const GhTri *tris)
{
	HTex *t = d->tex < g->ntex ? &g->texs[d->tex] : NULL;
	UINT nv = d->ntri * 3, i;
	D3D11_MAPPED_SUBRESOURCE m;
	float shift = g_h->halfpixel ? 0.5f : 0.0f;
	float ctl[4];

	if (!g->in_frame || !t || !t->srv || !nv || nv > VB_VERTS || d->blend >= GH_BLEND_N)
		return;
	if (g->vb_used + nv > VB_VERTS)
		g->vb_used = 0;
	if (FAILED(ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource *)g->vb, 0,
					   g->vb_used ? D3D11_MAP_WRITE_NO_OVERWRITE
						      : D3D11_MAP_WRITE_DISCARD,
					   0, &m)))
		return;
	{
		HVert *o = (HVert *)m.pData + g->vb_used;

		for (i = 0; i < d->ntri; i++) {
			vert_out(o++, &tris[i].a, shift);
			vert_out(o++, &tris[i].b, shift);
			vert_out(o++, &tris[i].c, shift);
		}
	}
	ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource *)g->vb, 0);

	ctl[0] = d->alpha_ref;
	ctl[1] = d->sharpen;
	ctl[2] = d->mul_identity ? 1.0f : 0.0f;
	ctl[3] = 0.0f;
	if (memcmp(ctl, g->ctl, sizeof(ctl)) &&
	    SUCCEEDED(ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource *)g->ctlcb, 0,
					      D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		memcpy(m.pData, ctl, sizeof(ctl));
		ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource *)g->ctlcb, 0);
		memcpy(g->ctl, ctl, sizeof(ctl));
	}
	if (d->scissor) {
		D3D11_RECT sc;

		sc.left = d->sx0;
		sc.top = d->sy0;
		sc.right = d->sx1;
		sc.bottom = d->sy1;
		ID3D11DeviceContext_RSSetScissorRects(g->ctx, 1, &sc);
	}
	ID3D11DeviceContext_RSSetState(g->ctx, g->rs[d->scissor ? 1 : 0]);
	ID3D11DeviceContext_OMSetBlendState(g->ctx, g->blend[d->blend], NULL, 0xffffffffu);
	ID3D11DeviceContext_PSSetShaderResources(g->ctx, 0, 1, &t->srv);
	ID3D11DeviceContext_PSSetSamplers(g->ctx, 0, 1, &g->dsmp[d->bilinear ? 1 : 0]);
	ID3D11DeviceContext_Draw(g->ctx, nv, g->vb_used);
	g->vb_used += nv;
	InterlockedIncrement((volatile LONG *)&g_h->gpu_draws);
}

/* A full stall, which is why the game only gets it a few times a session. */
static void op_readback(Gfx *g, const GhReadback *r)
{
	D3D11_MAPPED_SUBRESOURCE m;
	uint8_t *dst = (uint8_t *)g_h + g_h->rb_off;
	UINT w = r->w, h = r->h, y;
	int ok = 0;

	if (g->rt && w && h && (size_t)w * h * 4 <= g_h->rb_bytes) {
		if (!g->stage) {
			D3D11_TEXTURE2D_DESC td;

			ID3D11Texture2D_GetDesc(g->rt, &td);
			td.Usage = D3D11_USAGE_STAGING;
			td.BindFlags = 0;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ID3D11Device_CreateTexture2D(g->dev, &td, NULL, &g->stage);
		}
		if (g->stage) {
			ID3D11DeviceContext_CopyResource(g->ctx, (ID3D11Resource *)g->stage,
							 (ID3D11Resource *)g->rt);
			if (SUCCEEDED(ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource *)g->stage, 0,
							      D3D11_MAP_READ, 0, &m))) {
				if (w > g->rt_w)
					w = g->rt_w;
				if (h > g->rt_h)
					h = g->rt_h;
				for (y = 0; y < h; y++)
					memcpy(dst + (size_t)y * r->w * 4,
					       (const uint8_t *)m.pData + (size_t)y * m.RowPitch,
					       (size_t)w * 4);
				ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource *)g->stage, 0);
				ok = 1;
			}
		}
	}
	g_h->rb_ok = ok;
	MemoryBarrier();
	InterlockedExchange((volatile LONG *)&g_h->rb_done, r->seq);
	say("readback %ux%u #%d: %s", r->w, r->h, r->seq, ok ? "served" : "FAILED");
}

#define REL(p) \
	do { \
		if (p) \
			IUnknown_Release((IUnknown *)(p)); \
		(p) = NULL; \
	} while (0)

static void gfx_free(Gfx *g)
{
	uint32_t i;

	if (g->ctx)
		ID3D11DeviceContext_ClearState(g->ctx);
	for (i = 0; i < g->ntex; i++)
		tex_free(&g->texs[i]);
	free(g->texs);
	for (i = 0; i < GH_BLEND_N; i++)
		REL(g->blend[i]);
	REL(g->dsmp[0]);
	REL(g->dsmp[1]);
	REL(g->rs[0]);
	REL(g->rs[1]);
	REL(g->rt_srv);
	REL(g->rt_rtv);
	REL(g->rt);
	REL(g->stage);
	REL(g->vb);
	REL(g->ctlcb);
	REL(g->xcb);
	REL(g->layout);
	REL(g->dps);
	REL(g->dvs);
	if (g->srv)
		ID3D11ShaderResourceView_Release(g->srv);
	if (g->tex)
		ID3D11Texture2D_Release(g->tex);
	if (g->cb)
		ID3D11Buffer_Release(g->cb);
	if (g->samp)
		ID3D11SamplerState_Release(g->samp);
	if (g->ps)
		ID3D11PixelShader_Release(g->ps);
	if (g->vs)
		ID3D11VertexShader_Release(g->vs);
	if (g->rtv)
		ID3D11RenderTargetView_Release(g->rtv);
	if (g->sc)
		IDXGISwapChain1_Release(g->sc);
	if (g->ctx) {
		ID3D11DeviceContext_Flush(g->ctx);
		ID3D11DeviceContext_Release(g->ctx);
	}
	if (g->dev)
		ID3D11Device_Release(g->dev);
	memset(g, 0, sizeof(*g));
}

static void ring_copy(void *dst, uint32_t pos, uint32_t n)
{
	const uint8_t *ring = (const uint8_t *)g_h + g_h->ring_off;
	uint32_t mask = g_h->ring_bytes - 1, at = pos & mask, first = g_h->ring_bytes - at;

	if (first > n)
		first = n;
	memcpy(dst, ring + at, first);
	if (n > first)
		memcpy((uint8_t *)dst + first, ring, n - first);
}

/* Runs every record the renderer has published. Returns frames completed, or
 * -1 on a present failure or a record that makes no sense. */
static int ring_drain(Gfx *g, HWND hwnd, uint8_t **scratch, uint32_t *scap)
{
	const uint8_t *ring = (const uint8_t *)g_h + g_h->ring_off;
	uint32_t tail = g_h->ring_tail, mask = g_h->ring_bytes - 1;
	int frames = 0;

	for (;;) {
		uint32_t head = g_h->ring_head, body;
		const uint8_t *p;
		GhRec rec;

		MemoryBarrier();
		if (head == tail)
			break;
		ring_copy(&rec, tail, sizeof(rec));
		if (rec.bytes < sizeof(rec) || (rec.bytes & 7) || rec.bytes > head - tail) {
			say("ring record op %u bytes %u at %u (head %u) is corrupt", rec.op, rec.bytes,
			    tail, head);
			return -1;
		}
		body = rec.bytes - (uint32_t)sizeof(rec);
		if (((tail + sizeof(rec)) & mask) + body <= g_h->ring_bytes) {
			p = ring + ((tail + sizeof(rec)) & mask);
		} else {
			if (*scap < body) {
				uint8_t *s = (uint8_t *)realloc(*scratch, body);

				if (!s)
					return -1;
				*scratch = s;
				*scap = body;
			}
			ring_copy(*scratch, tail + (uint32_t)sizeof(rec), body);
			p = *scratch;
		}
		switch (rec.op) {
		case GH_OP_TEX_DEF:
			op_tex_def(g, (const GhTexDef *)p);
			break;
		case GH_OP_TEX_ROWS: {
			const GhTexRows *r = (const GhTexRows *)p;

			if (sizeof(*r) + (size_t)r->rows * r->w * 4 <= body)
				op_tex_rows(g, r, r + 1);
			break;
		}
		case GH_OP_TEX_DROP: {
			uint32_t id = ((const GhTexDrop *)p)->id;

			if (id < g->ntex)
				tex_free(&g->texs[id]);
			break;
		}
		case GH_OP_FRAME_BEGIN:
			op_frame_begin(g, (const GhFrameBegin *)p);
			break;
		case GH_OP_DRAW: {
			const GhDraw *d = (const GhDraw *)p;

			if (sizeof(*d) + (size_t)d->ntri * sizeof(GhTri) <= body)
				op_draw(g, d, (const GhTri *)(d + 1));
			break;
		}
		case GH_OP_FRAME_END:
			if (g->in_frame) {
				HRESULT hr;

				g->in_frame = 0;
				g->show = g->rt_srv;
				g->sw = g->rt_w;
				g->sh = g->rt_h;
				hr = draw(g, hwnd);
				if (FAILED(hr)) {
					g_h->error = (int32_t)hr;
					say("present failed %08lx", (unsigned long)hr);
					return -1;
				}
				/* Present rebinds the swap chain's target; the next
				 * frame's begin puts the draw state back. */
				InterlockedIncrement((volatile LONG *)&g_h->gpu_frames);
				InterlockedIncrement((volatile LONG *)&g_h->shown);
				frames++;
			}
			break;
		case GH_OP_READBACK:
			op_readback(g, (const GhReadback *)p);
			/* The readback left the draw target unbound as a copy
			 * source only, so drawing may simply carry on. */
			break;
		default:
			say("unknown ring op %u", rec.op);
			return -1;
		}
		tail += rec.bytes;
		InterlockedExchange((volatile LONG *)&g_h->ring_tail, (LONG)tail);
		/* A long burst of uploads is still a live host. */
		InterlockedIncrement((volatile LONG *)&g_h->heartbeat);
	}
	return frames;
}

int main(int argc, char **argv)
{
	char dir[MAX_PATH], path[MAX_PATH], evname[128], *slash;
	HANDLE map, ev, game;
	HWND hwnd;
	Gfx g;
	int front = 2;
	HRESULT hr;
	DWORD pid;
	uint8_t *scratch = NULL;
	uint32_t scap = 0;

	/* Into the mapping before anything else, so the renderer can tell a host
	 * that never ran from one that ran and got stuck. */
	map = argc >= 3 ? OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, argv[1]) : NULL;
	g_h = map ? (GhHeader *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
	if (g_h && (g_h->magic != GH_MAGIC || g_h->version != GH_VERSION))
		g_h = NULL;
	if (g_h) {
		g_h->host_pid = GetCurrentProcessId();
		lstrcpynA(g_h->msg, "main entered", sizeof(g_h->msg));
		InterlockedIncrement((volatile LONG *)&g_h->heartbeat);
	}
	GetModuleFileNameA(NULL, dir, sizeof(dir));
	slash = strrchr(dir, '\\');
	if (slash)
		*slash = 0;
	snprintf(path, sizeof(path), "%s\\gpuhost.log", dir);
	g_log = fopen(path, "a");
	if (!g_log) {
		char tmp[MAX_PATH];

		if (GetTempPathA(sizeof(tmp), tmp)) {
			snprintf(path, sizeof(path), "%sgpuhost.log", tmp);
			g_log = fopen(path, "a");
		}
	}
	if (argc < 3) {
		say("usage: gpuhost64.exe <mapping> <game pid>");
		return 2;
	}
	pid = (DWORD)strtoul(argv[2], NULL, 10);
	say("---- gpuhost %s for pid %lu, log %s", argv[1], (unsigned long)pid, path);
	if (!g_h) {
		say("mapping %s missing or not ours", argv[1]);
		return 3;
	}
	snprintf(evname, sizeof(evname), "%s_f", argv[1]);
	ev = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, evname);
	game = OpenProcess(SYNCHRONIZE, FALSE, pid);
	hwnd = (HWND)(uintptr_t)g_h->hwnd;
	if (!ev || !game || !IsWindow(hwnd)) {
		g_h->state = GH_FAILED;
		say("event %p, game %p, window %p valid %d", (void *)ev, (void *)game, (void *)hwnd,
		    IsWindow(hwnd));
		return 4;
	}
	memset(&g, 0, sizeof(g));
	say("creating the device and the swap chain");
	hr = gfx_init(&g, hwnd, dir);
	if (FAILED(hr)) {
		g_h->error = (int32_t)hr;
		g_h->state = GH_FAILED;
		gfx_free(&g);
		return 5;
	}
	InterlockedExchange((volatile LONG *)&g_h->state, GH_READY);
	say("ready: window %p, %ux%u client, %s, %s, %s", (void *)hwnd, g.cw, g.ch,
	    g_h->point ? "point" : "linear", g_h->integer ? "integer" : "fit",
	    g_h->mode == 2 ? "drawing on the GPU (mode 2)" : "presenting frames (mode 1)");

	for (;;) {
		HANDLE w[2] = { ev, game };
		DWORD r = WaitForMultipleObjects(2, w, FALSE, 100);
		int fresh = 0, frames = 0;

		InterlockedIncrement((volatile LONG *)&g_h->heartbeat);
		if (r == WAIT_OBJECT_0 + 1) {
			say("game exited");
			break;
		}
		if (g_h->quit) {
			say("renderer asked us to quit");
			break;
		}
		if (g_h->mode == 2) {
			frames = ring_drain(&g, hwnd, &scratch, &scap);
			if (frames < 0) {
				InterlockedExchange((volatile LONG *)&g_h->state, GH_FAILED);
				say("handing the window back after %ld GPU frame(s)",
				    (long)g_h->gpu_frames);
				free(scratch);
				gfx_free(&g);
				return 7;
			}
			if (frames && g_h->gpu_frames == frames)
				say("first frame drawn on the GPU: %ld draw(s), %ld texture(s)",
				    (long)g_h->gpu_draws, (long)g_h->gpu_textures);
		}
		/* Not in the middle of a replayed frame: presenting rebinds the
		 * pipeline under the draws still to come. */
		if (g.in_frame)
			continue;
		if (g_h->middle & GH_FRESH) {
			front = (int)(InterlockedExchange((volatile LONG *)&g_h->middle, front) & 0xff);
			fresh = front >= 0 && front < GH_SLOTS;
		}
		if (fresh) {
			UINT fw = g_h->slot_w[front], fh = g_h->slot_h[front];

			if (fw && fh && (size_t)fw * fh * 4 <= g_h->slot_bytes && tex_ensure(&g, fw, fh)) {
				upload(&g, GH_SLOT(g_h, front), fw, fh);
				g.show = g.srv;
				g.sw = fw;
				g.sh = fh;
			}
		}
		/* A timeout still redraws, so a resize while the game is paused or
		 * frozen for a save shows the last frame at the new size. */
		if (fresh || (r == WAIT_TIMEOUT && !frames)) {
			hr = draw(&g, hwnd);
			if (FAILED(hr)) {
				g_h->error = (int32_t)hr;
				InterlockedExchange((volatile LONG *)&g_h->state, GH_FAILED);
				say("present failed %08lx; handing the window back", (unsigned long)hr);
				gfx_free(&g);
				return 6;
			}
			if (fresh)
				InterlockedIncrement((volatile LONG *)&g_h->shown);
		}
	}
	free(scratch);
	gfx_free(&g);
	InterlockedExchange((volatile LONG *)&g_h->state, GH_EXITED);
	say("exited after %ld frame(s)", (long)g_h->shown);
	return 0;
}
