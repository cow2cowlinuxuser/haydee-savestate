/* pixbench_gpu - the other half of the "make pixels faster" question: is a real
 * GPU worth attaching for the per-pixel colour math, and under what terms.
 *
 * pixbench.c settles the CPU side - AVX2/AVX512 forms of add, subtract,
 * source-over, modulate, the B5G6R5 / BC1 decodes and the YCbCr baseband
 * transform, each a few tenths of a millisecond per 720p frame. This asks
 * whether offloading that same math to the GPU as an overlay beats it, and it
 * measures the thing that decides the answer: not the arithmetic, which a
 * 7900 XTX finishes in microseconds, but the cost of getting a frame onto the
 * card and back off it.
 *
 * Three regimes are timed for each op, all at 720p:
 *   dispatch-only  - the compute shader alone, GPU timestamp to GPU timestamp,
 *                    with the inputs already resident. This is the raw ALU
 *                    ceiling and the number a "render as video, decode on the
 *                    GPU, present from the GPU" pipeline would actually see.
 *   round-trip     - upload both inputs, dispatch, copy to a staging buffer,
 *                    map it back to system memory. This is what "offload the
 *                    math as an overlay" costs when the pixels start and end on
 *                    the CPU, which is the arrangement d3d11_sw.c is in today.
 *   resident       - dispatch repeated with no upload and no readback, wall
 *                    clock, to show the steady state once the data lives on the
 *                    card and stays there.
 *
 * The system d3d11.dll is loaded by absolute path on purpose: this repo ships
 * its own software d3d11.dll in the same folder, and the loader would find that
 * one first. A hardware device is what this harness is about, so it reaches
 * past the local copy to the real one. D3D11CreateDevice with a null adapter
 * picks the default hardware adapter, so no dxgi factory is needed and dxgi is
 * never shadowed either.
 *
 * Build (native x64):
 *   zig cc -O2 -target x86_64-windows-gnu -o pixbench_gpu.exe pixbench_gpu.c
 * No import libraries: d3d11 and d3dcompiler are both resolved at runtime, so a
 * machine with neither still links and simply reports them absent.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FW 1280
#define FH 720
#define PIX (FW * FH)
#define REPS 200

static double now_ms(void)
{
	LARGE_INTEGER f, t;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* ---- CPU scalar references, so the shader can be checked and the offload has
 * a same-machine baseline to be judged against. These are the same integer
 * recipes pixbench.c uses. ---- */

static uint32_t div255(unsigned v) { v += 128; return (v + (v >> 8)) >> 8; }
static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void cpu_op(int mode, uint32_t *dst, const uint32_t *a, const uint32_t *b, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		uint32_t x = a[i], y = b[i], o = 0;
		int c;
		if (mode == 0) { /* saturating add */
			for (c = 0; c < 4; c++) {
				int s = (int)((x >> (c * 8)) & 255) + (int)((y >> (c * 8)) & 255);
				o |= (uint32_t)(s > 255 ? 255 : s) << (c * 8);
			}
		} else if (mode == 1) { /* src-over: x over y */
			unsigned sa = (x >> 24) & 255, isa = 255 - sa;
			for (c = 0; c < 4; c++) {
				unsigned sc = (x >> (c * 8)) & 255, dc = (y >> (c * 8)) & 255;
				o |= div255(sc * sa + dc * isa) << (c * 8);
			}
		} else { /* YCbCr(x) -> RGB */
			int Y = (x >> 16) & 255, cb = ((x >> 8) & 255) - 128, cr = (x & 255) - 128;
			int r = Y + ((359 * cr + 128) >> 8);
			int g = Y - ((88 * cb + 183 * cr + 128) >> 8);
			int bl = Y + ((454 * cb + 128) >> 8);
			o = 0xff000000u | ((uint32_t)clamp255(r) << 16) |
			    ((uint32_t)clamp255(g) << 8) | (uint32_t)clamp255(bl);
		}
		dst[i] = o;
	}
}

/* The compute shader: one op selected by a constant, one uint (one RGBA8 pixel)
 * per thread. The integer recipes match cpu_op bit for bit - div255 the same
 * bias-and-shift, the YCbCr coefficients the same Q8 constants, signed shifts
 * arithmetic on both sides - so the readback is checked exactly, not by eye. */
static const char *g_hlsl =
	"cbuffer P : register(b0) { uint mode; uint count; uint2 _pad; };\n"
	"StructuredBuffer<uint> inA : register(t0);\n"
	"StructuredBuffer<uint> inB : register(t1);\n"
	"RWStructuredBuffer<uint> outp : register(u0);\n"
	"uint div255(uint v){ v+=128; return (v+(v>>8))>>8; }\n"
	"[numthreads(64,1,1)]\n"
	"void main(uint3 id : SV_DispatchThreadID){\n"
	"  uint i = id.x; if(i>=count) return;\n"
	"  uint a = inA[i], b = inB[i]; uint o = 0; uint c;\n"
	"  if(mode==0){\n"
	"    [unroll] for(c=0;c<4;c++){ uint s=((a>>(c*8))&255)+((b>>(c*8))&255); s=min(s,255u); o|=s<<(c*8); }\n"
	"  } else if(mode==1){\n"
	"    uint sa=(a>>24)&255, isa=255-sa;\n"
	"    [unroll] for(c=0;c<4;c++){ uint sc=(a>>(c*8))&255, dc=(b>>(c*8))&255; o|=div255(sc*sa+dc*isa)<<(c*8); }\n"
	"  } else {\n"
	"    int Y=(int)((a>>16)&255); int cb=(int)((a>>8)&255)-128; int cr=(int)(a&255)-128;\n"
	"    int r=Y+((359*cr+128)>>8); int g=Y-((88*cb+183*cr+128)>>8); int bl=Y+((454*cb+128)>>8);\n"
	"    r=clamp(r,0,255); g=clamp(g,0,255); bl=clamp(bl,0,255);\n"
	"    o=0xff000000u | ((uint)r<<16) | ((uint)g<<8) | (uint)bl;\n"
	"  }\n"
	"  outp[i]=o;\n"
	"}\n";

typedef HRESULT(WINAPI *pfn_create_device)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
					   const D3D_FEATURE_LEVEL *, UINT, UINT,
					   ID3D11Device **, D3D_FEATURE_LEVEL *,
					   ID3D11DeviceContext **);
typedef HRESULT(WINAPI *pfn_compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
				     ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
				     ID3DBlob **);

struct P {
	unsigned mode, count, pad0, pad1;
};

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;

static ID3D11Buffer *make_struct_buf(unsigned count, int uav, const void *init)
{
	D3D11_BUFFER_DESC bd;
	D3D11_SUBRESOURCE_DATA sd;
	ID3D11Buffer *b = NULL;
	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = count * 4;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = uav ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_SHADER_RESOURCE;
	bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	bd.StructureByteStride = 4;
	sd.pSysMem = init;
	sd.SysMemPitch = sd.SysMemSlicePitch = 0;
	ID3D11Device_CreateBuffer(dev, &bd, init ? &sd : NULL, &b);
	return b;
}

int main(void)
{
	HMODULE hd3d, hcomp;
	pfn_create_device create;
	pfn_compile compile;
	D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0, got;
	HRESULT hr;
	ID3DBlob *cso = NULL, *err = NULL;
	ID3D11ComputeShader *cs = NULL;
	ID3D11Buffer *inA, *inB, *outb, *stag, *cbuf;
	ID3D11ShaderResourceView *srvA = NULL, *srvB = NULL;
	ID3D11UnorderedAccessView *uav = NULL;
	ID3D11Query *qdis = NULL, *q0 = NULL, *q1 = NULL;
	D3D11_QUERY_DESC qd;
	D3D11_BUFFER_DESC bd;
	D3D11_SHADER_RESOURCE_VIEW_DESC svd;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uvd;
	uint32_t *srcA, *srcB, *ref, *back;
	unsigned n = PIX, groups = (PIX + 63) / 64, rng = 0x2468ace0u, i, m;
	int bad_total = 0;
	const char *opname[3] = { "color add (sat)", "mix / src-over", "baseband YCbCr->RGB" };

	hd3d = LoadLibraryA("C:\\Windows\\System32\\d3d11.dll");
	hcomp = LoadLibraryA("C:\\Windows\\System32\\d3dcompiler_47.dll");
	if (!hd3d || !hcomp) {
		printf("pixbench_gpu: system d3d11.dll=%p d3dcompiler_47.dll=%p - one is "
		       "missing, GPU offload not measurable here\n",
		       (void *)hd3d, (void *)hcomp);
		return 0;
	}
	create = (pfn_create_device)(void *)GetProcAddress(hd3d, "D3D11CreateDevice");
	compile = (pfn_compile)(void *)GetProcAddress(hcomp, "D3DCompile");
	if (!create || !compile) {
		printf("pixbench_gpu: entry points missing (create=%p compile=%p)\n",
		       (void *)create, (void *)compile);
		return 0;
	}

	hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &want, 1, D3D11_SDK_VERSION, &dev,
		    &got, &ctx);
	if (FAILED(hr)) {
		printf("pixbench_gpu: no hardware D3D11 device (hr=0x%08lx), trying WARP\n",
		       (unsigned long)hr);
		hr = create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &want, 1, D3D11_SDK_VERSION,
			    &dev, &got, &ctx);
	}
	if (FAILED(hr)) {
		printf("pixbench_gpu: no D3D11 device at all (hr=0x%08lx)\n", (unsigned long)hr);
		return 0;
	}

	hr = compile(g_hlsl, strlen(g_hlsl), "pixbench.hlsl", NULL, NULL, "main", "cs_5_0",
		     0, 0, &cso, &err);
	if (FAILED(hr)) {
		printf("pixbench_gpu: shader compile failed: %s\n",
		       err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "(no message)");
		return 1;
	}
	ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(cso),
					 ID3D10Blob_GetBufferSize(cso), NULL, &cs);

	srcA = malloc(n * 4);
	srcB = malloc(n * 4);
	ref = malloc(n * 4);
	back = malloc(n * 4);
	for (i = 0; i < n; i++) {
		rng = rng * 1664525u + 1013904223u;
		srcA[i] = rng;
		rng = rng * 1664525u + 1013904223u;
		srcB[i] = rng;
	}

	inA = make_struct_buf(n, 0, srcA);
	inB = make_struct_buf(n, 0, srcB);
	outb = make_struct_buf(n, 1, NULL);

	/* staging buffer for readback */
	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = n * 4;
	bd.Usage = D3D11_USAGE_STAGING;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	bd.StructureByteStride = 4;
	ID3D11Device_CreateBuffer(dev, &bd, NULL, &stag);

	/* constant buffer */
	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = sizeof(struct P);
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ID3D11Device_CreateBuffer(dev, &bd, NULL, &cbuf);

	memset(&svd, 0, sizeof(svd));
	svd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	svd.Buffer.NumElements = n;
	ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)inA, &svd, &srvA);
	ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)inB, &svd, &srvB);
	memset(&uvd, 0, sizeof(uvd));
	uvd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uvd.Buffer.NumElements = n;
	ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)outb, &uvd, &uav);

	qd.MiscFlags = 0;
	qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
	ID3D11Device_CreateQuery(dev, &qd, &qdis);
	qd.Query = D3D11_QUERY_TIMESTAMP;
	ID3D11Device_CreateQuery(dev, &qd, &q0);
	ID3D11Device_CreateQuery(dev, &qd, &q1);

	ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
	{
		ID3D11ShaderResourceView *srvs[2] = { srvA, srvB };
		ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 2, srvs);
		ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &uav, NULL);
		ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cbuf);
	}

	printf("pixbench_gpu - color math on the GPU vs the CPU, per 720p frame\n");
	printf("device: feature level 0x%04x   frame=%dx%d (%u px)  reps=%u\n\n",
	       (unsigned)got, FW, FH, n, REPS);
	printf("%-22s  %-12s  %-14s  %-14s  %-12s\n", "operation", "cpu-scalar",
	       "gpu dispatch", "gpu roundtrip", "gpu resident");
	printf("            (ms/frame, and frames/sec in parens)\n");
	printf("-----------------------------------------------------------------------------"
	       "-----------\n");

	for (m = 0; m < 3; m++) {
		D3D11_MAPPED_SUBRESOURCE ms;
		struct P p;
		double cpu_ms, rt_ms, res_ms, disp_ms = 0;
		int r, bad = 0;
		UINT64 t0 = 0, t1 = 0;
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;

		p.mode = m;
		p.count = n;
		p.pad0 = p.pad1 = 0;
		ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cbuf, 0, D3D11_MAP_WRITE_DISCARD,
					0, &ms);
		memcpy(ms.pData, &p, sizeof(p));
		ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cbuf, 0);

		/* cpu reference + baseline timing */
		cpu_op(m, ref, srcA, srcB, n);
		{
			double a = now_ms();
			for (r = 0; r < REPS; r++)
				cpu_op(m, ref, srcA, srcB, n);
			cpu_ms = (now_ms() - a) / REPS;
		}

		/* one dispatch, checked against the cpu reference */
		ID3D11DeviceContext_Dispatch(ctx, groups, 1, 1);
		ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stag,
						 (ID3D11Resource *)outb);
		ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stag, 0, D3D11_MAP_READ, 0, &ms);
		memcpy(back, ms.pData, n * 4);
		ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stag, 0);
		for (i = 0; i < n; i++)
			if (back[i] != ref[i])
				bad++;
		bad_total += bad;

		/* dispatch-only, GPU timestamps, inputs already resident */
		ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)qdis);
		ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q0);
		for (r = 0; r < REPS; r++)
			ID3D11DeviceContext_Dispatch(ctx, groups, 1, 1);
		ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q1);
		ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)qdis);
		ID3D11DeviceContext_Flush(ctx);
		while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)qdis, &dj,
						   sizeof(dj), 0) != S_OK)
			;
		while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q0, &t0,
						   sizeof(t0), 0) != S_OK)
			;
		while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q1, &t1,
						   sizeof(t1), 0) != S_OK)
			;
		if (!dj.Disjoint && dj.Frequency)
			disp_ms = (double)(t1 - t0) * 1000.0 / (double)dj.Frequency / REPS;

		/* resident: dispatch repeated, wall clock, no upload/readback */
		ID3D11DeviceContext_Dispatch(ctx, groups, 1, 1);
		ID3D11DeviceContext_Flush(ctx);
		{
			double a = now_ms();
			for (r = 0; r < REPS; r++)
				ID3D11DeviceContext_Dispatch(ctx, groups, 1, 1);
			ID3D11DeviceContext_Flush(ctx);
			res_ms = (now_ms() - a) / REPS;
		}

		/* round-trip: upload both inputs, dispatch, copy to staging, read back */
		{
			double a = now_ms();
			for (r = 0; r < REPS; r++) {
				ID3D11DeviceContext_UpdateSubresource(
					ctx, (ID3D11Resource *)inA, 0, NULL, srcA, 0, 0);
				ID3D11DeviceContext_UpdateSubresource(
					ctx, (ID3D11Resource *)inB, 0, NULL, srcB, 0, 0);
				ID3D11DeviceContext_Dispatch(ctx, groups, 1, 1);
				ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stag,
								 (ID3D11Resource *)outb);
				ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stag, 0,
							D3D11_MAP_READ, 0, &ms);
				memcpy(back, ms.pData, n * 4);
				ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stag, 0);
			}
			rt_ms = (now_ms() - a) / REPS;
		}

		printf("%-22s  %6.3f(%5.0f)  %6.3f(%6.0f) %6.3f(%5.0f)  %6.3f(%6.0f)%s\n",
		       opname[m], cpu_ms, 1000.0 / cpu_ms, disp_ms,
		       disp_ms > 0 ? 1000.0 / disp_ms : 0, rt_ms, 1000.0 / rt_ms, res_ms,
		       1000.0 / res_ms, bad ? "  <-- MISMATCH" : "");
	}

	printf("\ncorrectness: %s (shader vs cpu reference across all ops)\n",
	       bad_total ? "FAIL" : "PASS, bit-exact");
	printf("\nnote: dispatch is the GPU ALU alone with data resident; round-trip adds the\n"
	       "PCIe upload of two 720p inputs and the readback of one, which is the real cost\n"
	       "of using the GPU as an overlay when the pixels live on the CPU.\n");
	return bad_total ? 1 : 0;
}
