/* Renders identical randomised scenes through the scalar rasteriser and the
 * AVX2 span kernel and reports any pixel that differs. The scalar path is the
 * reference; the kernel is only allowed to round differently by one, and only
 * where bilinear filtering is involved. */
#include "swrast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 640
#define H 480
#define TW 256
#define TH 128

static unsigned rng_state = 12345;

static unsigned rnd(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state >> 8;
}

static float rndf(float lo, float hi)
{
	return lo + (hi - lo) * ((float)(rnd() & 0xffff) / 65535.0f);
}

static uint32_t texels[TW * TH];

/* Texture shape and addressing for run_case; the defaults are the original
 * power-of-two WRAP scene. Any width * height up to TW * TH fits the texels. */
static int g_tw = TW, g_th = TH, g_au = 1, g_av = 1;
static float g_uvlo = -2.0f, g_uvhi = 3.0f;
static int g_mul;

static void fill_tex(void)
{
	int i;
	for (i = 0; i < TW * TH; i++)
		texels[i] = (rnd() << 24) | (rnd() & 0xffffff);
}

static void build_scene(SwTri *tris, int n)
{
	int i, k;
	for (i = 0; i < n; i++) {
		SwVert *vs[3];
		uint32_t col = (rnd() << 24) | (rnd() & 0xffffff);
		float rhw = (rnd() & 1) ? 1.0f : rndf(0.2f, 3.0f);
		vs[0] = &tris[i].a;
		vs[1] = &tris[i].b;
		vs[2] = &tris[i].c;
		for (k = 0; k < 3; k++) {
			vs[k]->x = rndf(-40.0f, (float)W + 40.0f);
			vs[k]->y = rndf(-40.0f, (float)H + 40.0f);
			vs[k]->z = rndf(0.0f, 1.0f);
			vs[k]->rhw = rhw;
			vs[k]->color = col; /* flat: the configuration the kernel claims */
			vs[k]->u = rndf(g_uvlo, g_uvhi);
			vs[k]->v = rndf(g_uvlo, g_uvhi);
		}
	}
}

static int run_case(const char *name, int bilinear, int blend, int dst_blend, int alpha_test,
		    int alpha_func, int alpha_ref, int white, int ntris)
{
	SwRast ra, rb;
	SwState st;
	SwTex tex;
	SwTri *tris = (SwTri *)malloc(sizeof(SwTri) * ntris);
	int i, bad = 0, worst = 0;

	rng_state = 999;
	build_scene(tris, ntris);
	if (white)
		for (i = 0; i < ntris; i++)
			tris[i].a.color = tris[i].b.color = tris[i].c.color = 0xffffffffu;

	tex.width = g_tw;
	tex.height = g_th;
	tex.pixels = texels;
	tex.gen = 0;

	swrast_state_defaults(&st);
	st.bilinear = bilinear;
	st.z_enable = 0;
	st.z_write = 0;
	st.blend_enable = blend;
	st.src_blend = 5; /* D3DBLEND_SRCALPHA */
	st.dst_blend = dst_blend;
	if (g_mul) {
		/* Multiply with the identity fade, as d3d11_sw sets it up. */
		st.src_blend = 1; /* D3DBLEND_ZERO */
		st.dst_blend = 3; /* D3DBLEND_SRCCOLOR */
		st.mul_identity = 1;
	}
	st.blend_op = 1; /* D3DBLENDOP_ADD */
	st.alpha_test = alpha_test;
	st.alpha_func = alpha_func;
	st.alpha_ref = alpha_ref;
	st.addr_u = g_au;
	st.addr_v = g_av;
	st.cull = 1; /* NONE */

	memset(&ra, 0, sizeof(ra));
	memset(&rb, 0, sizeof(rb));
	swrast_init(&ra, NULL, W, H);
	swrast_init(&rb, NULL, W, H);
	swrast_clear_color(&ra, 0xff204060);
	swrast_clear_color(&rb, 0xff204060);

	swrast_simd_enable = 0;
	swrast_triangles(&ra, tris, ntris, &tex, &st);
	swrast_flush();

	swrast_simd_enable = 1;
	swrast_triangles(&rb, tris, ntris, &tex, &st);
	swrast_flush();

	for (i = 0; i < W * H; i++) {
		uint32_t x = ra.color[i], y = rb.color[i];
		int ch;
		for (ch = 0; ch < 4; ch++) {
			int d = (int)((x >> (ch * 8)) & 255) - (int)((y >> (ch * 8)) & 255);
			if (d < 0)
				d = -d;
			if (d > worst)
				worst = d;
			if (d > 1)
				bad++;
		}
	}
	printf("%-34s worst channel delta=%d  pixels off by >1: %d\n", name, worst, bad);

	free(tris);
	swrast_free(&ra);
	swrast_free(&rb);
	return bad == 0 && worst <= 1;
}

/* run_case on a texture of the given shape and addressing (D3DTADDRESS_*: 1
 * wrap, 2 mirror, 3 clamp, 5 mirror once). Agreement alone would pass if the
 * kernel declined and both runs went scalar, so this also requires the kernel
 * to have folded some area itself. */
static int run_fold(const char *name, int tw, int th, int au, int av, int bilinear)
{
	double sr[11];
	int ok;

	g_tw = tw;
	g_th = th;
	g_au = au;
	g_av = av;
	g_uvlo = -5.0f;
	g_uvhi = 6.0f;
	swrast_prof_simd(sr, 11);
	ok = run_case(name, bilinear, 1, 6, 1, 6, 0, 0, 400);
	swrast_prof_simd(sr, 11);
	if (sr[10] <= 0.0) {
		printf("  ^ kernel never folded (ok area %.0f)\n", sr[0]);
		ok = 0;
	}
	g_tw = TW;
	g_th = TH;
	g_au = g_av = 1;
	g_uvlo = -2.0f;
	g_uvhi = 3.0f;
	return ok;
}

/* 1:1 texel-to-pixel mapping (dudx*width == 1, dvdx == 0) with wrapping. */
static int run_1to1(const char *name, int bilinear)
{
	SwRast ra, rb;
	SwState st;
	SwTex tex;
	SwTri tris[2];
	int i, bad = 0, worst = 0;
	float uw = (float)W / (float)TW, vh = (float)H / (float)TH;
	SwVert tl = { 0, 0, 0.5f, 1.0f, 0xffffffffu, 0, 0 };
	SwVert tr = { (float)W, 0, 0.5f, 1.0f, 0xffffffffu, uw, 0 };
	SwVert bl = { 0, (float)H, 0.5f, 1.0f, 0xffffffffu, 0, vh };
	SwVert br = { (float)W, (float)H, 0.5f, 1.0f, 0xffffffffu, uw, vh };

	tex.width = TW;
	tex.height = TH;
	tex.pixels = texels;
	swrast_state_defaults(&st);
	st.bilinear = bilinear;
	st.z_enable = 0;
	st.blend_enable = 1;
	st.src_blend = 5;
	st.dst_blend = 6;
	st.blend_op = 1;
	st.alpha_test = 1;
	st.alpha_func = 6;
	st.alpha_ref = 0;
	st.addr_u = st.addr_v = 1;
	st.cull = 1;
	tris[0].a = tl;
	tris[0].b = tr;
	tris[0].c = br;
	tris[1].a = tl;
	tris[1].b = br;
	tris[1].c = bl;

	memset(&ra, 0, sizeof(ra));
	memset(&rb, 0, sizeof(rb));
	swrast_init(&ra, NULL, W, H);
	swrast_init(&rb, NULL, W, H);
	swrast_clear_color(&ra, 0xff204060);
	swrast_clear_color(&rb, 0xff204060);
	swrast_simd_enable = 0;
	swrast_triangles(&ra, tris, 2, &tex, &st);
	swrast_flush();
	swrast_simd_enable = 1;
	swrast_triangles(&rb, tris, 2, &tex, &st);
	swrast_flush();
	for (i = 0; i < W * H; i++) {
		uint32_t x = ra.color[i], y = rb.color[i];
		int ch;
		for (ch = 0; ch < 4; ch++) {
			int d = (int)((x >> (ch * 8)) & 255) - (int)((y >> (ch * 8)) & 255);
			if (d < 0)
				d = -d;
			if (d > worst)
				worst = d;
			if (d > 1)
				bad++;
		}
	}
	printf("%-34s worst channel delta=%d  pixels off by >1: %d\n", name, worst, bad);
	swrast_free(&ra);
	swrast_free(&rb);
	return bad == 0 && worst <= 1;
}

/* Big overdrawn spans in the shape the game actually submits: full-screen
 * quads, textured, alpha blended. onetoone uses dudx*width==1 so the sequential
 * load path engages. */
static double bench(int simd, int frames, int bilin, int dst_blend, int onetoone)
{
	SwRast r;
	SwState st;
	SwTex tex;
	SwTri tris[2];
	LARGE_INTEGER f, t0, t1;
	int i, q;
	float uw = onetoone ? (float)W / (float)TW : 4.0f;
	float vh = onetoone ? (float)H / (float)TH : 4.0f;

	tex.width = TW;
	tex.height = TH;
	tex.pixels = texels;
	swrast_state_defaults(&st);
	st.bilinear = bilin;
	st.blend_enable = 1;
	st.src_blend = 5;
	st.dst_blend = dst_blend;
	st.blend_op = 1;
	st.alpha_test = 1;
	st.alpha_func = 6;
	st.alpha_ref = 0;
	st.addr_u = st.addr_v = 1;
	st.cull = 1;

	memset(&r, 0, sizeof(r));
	swrast_init(&r, NULL, W, H);
	swrast_simd_enable = simd;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	for (i = 0; i < frames; i++) {
		swrast_clear_color(&r, 0xff000000);
		for (q = 0; q < 12; q++) {
			SwVert tl = { 0, 0, 0.5f, 1.0f, 0xc0ffffff, 0, 0 };
			SwVert tr = { (float)W, 0, 0.5f, 1.0f, 0xc0ffffff, uw, 0 };
			SwVert bl = { 0, (float)H, 0.5f, 1.0f, 0xc0ffffff, 0, vh };
			SwVert br = { (float)W, (float)H, 0.5f, 1.0f, 0xc0ffffff, uw, vh };
			tris[0].a = tl;
			tris[0].b = tr;
			tris[0].c = br;
			tris[1].a = tl;
			tris[1].b = br;
			tris[1].c = bl;
			swrast_triangles(&r, tris, 2, &tex, &st);
		}
		swrast_flush();
	}
	QueryPerformanceCounter(&t1);
	swrast_free(&r);
	return (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart / frames;
}

int main(void)
{
	int ok = 1;
	printf("cpu features = 0x%x\n", swrast_cpu_features());
	if (!(swrast_cpu_features() & 4)) {
		printf("no AVX2 on this machine; kernel never engages\n");
		return 0;
	}
	fill_tex();

	ok &= run_case("point, blend over, atest!=0", 0, 1, 6, 1, 6, 0, 0, 400);
	ok &= run_case("bilinear, blend over, atest!=0", 1, 1, 6, 1, 6, 0, 0, 400);
	ok &= run_case("point, no blend, no atest", 0, 0, 6, 0, 8, 0, 0, 400);
	ok &= run_case("bilinear, no blend, no atest", 1, 0, 6, 0, 8, 0, 0, 400);
	ok &= run_case("bilinear, over, white modulate", 1, 1, 6, 1, 6, 0, 1, 400);
	ok &= run_case("point, over, atest >= 128", 0, 1, 6, 1, 7, 128, 0, 400);
	ok &= run_case("bilinear, over, atest > 200", 1, 1, 6, 1, 5, 200, 0, 400);
	ok &= run_case("bilinear, additive blend", 1, 1, 2, 1, 6, 0, 0, 400);
	g_mul = 1;
	ok &= run_case("multiply + identity fade, point", 0, 1, 6, 0, 8, 0, 0, 400);
	ok &= run_case("multiply + identity fade, bilinear", 1, 1, 6, 0, 8, 0, 0, 400);
	ok &= run_case("multiply + fade, white modulate", 1, 1, 6, 0, 8, 0, 1, 400);
	ok &= run_fold("multiply + fade, npot 183x97 wrap", 183, 97, 1, 1, 1);
	g_mul = 0;
	ok &= run_fold("npot 200x120 wrap, point", 200, 120, 1, 1, 0);
	ok &= run_fold("npot 200x120 wrap, bilinear", 200, 120, 1, 1, 1);
	ok &= run_fold("npot 183x97 mirror, point", 183, 97, 2, 2, 0);
	ok &= run_fold("npot 183x97 mirror, bilinear", 183, 97, 2, 2, 1);
	ok &= run_fold("npot 183x97 mirror once, bilinear", 183, 97, 5, 5, 1);
	ok &= run_fold("pow2 256x128 mirror, bilinear", 256, 128, 2, 2, 1);
	ok &= run_fold("npot 200x120 wrap u, clamp v", 200, 120, 1, 3, 1);
	ok &= run_fold("pow2 256x128 clamp u, mirror v", 256, 128, 3, 2, 0);
	ok &= run_1to1("1:1 point sequential load", 0);
	ok &= run_1to1("1:1 bilinear sequential load", 1);

	{
		double sc, a2, ev, ins, seq;
		int have_evex = !!(swrast_cpu_features() & 8);

		swrast_gather_mode = 0;
		bench(0, 2, 1, 6, 0);
		sc = bench(0, 20, 1, 6, 0);
		a2 = bench(1, 20, 1, 6, 0);

		swrast_gather_mode = 2;
		ins = bench(1, 20, 1, 6, 0);

		ev = 0;
		if (have_evex) {
			swrast_gather_mode = 1;
			ev = bench(1, 20, 1, 6, 0);
		}

		swrast_gather_mode = have_evex ? 1 : 0;
		seq = bench(1, 20, 1, 6, 1);

		printf("\nscalar            %.3f ms\n", sc);
		printf("avx2 gather       %.3f ms  (%.2fx vs scalar)\n", a2, sc / a2);
		printf("insert (no gather)%.3f ms  (%.2fx vs scalar)\n", ins, sc / ins);
		if (have_evex)
			printf("evex256 gather    %.3f ms  (%.2fx vs scalar)\n", ev, sc / ev);
		printf("1:1 sequential    %.3f ms  (%.2fx vs scalar)\n", seq, sc / seq);
	}

	/* Wall time is what the frame sees; wall x threads approximates the CPU
	 * the host actually spends, which is the number that matters on a VM. */
	{
		const char *n = getenv("D3D9SW_THREADS");
		int t = swrast_thread_count();
		double ms;
		swrast_gather_mode = -1;
		bench(1, 2, 1, 6, 0);
		ms = bench(1, 30, 1, 6, 0);
		printf("\nthreads=%-3d (D3D9SW_THREADS=%s)  %.3f ms wall  ~%.1f ms cpu\n", t,
		       n ? n : "unset", ms, ms * t);
	}

	printf("\n%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
