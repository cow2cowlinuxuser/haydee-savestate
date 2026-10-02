/* pixbench - a harness for the pixel operations the software D3D11/D3D9 path
 * spends its per-frame time in, lifted out of d3d11_sw.c / swrast.c so each one
 * can be measured and widened on its own.
 *
 * The frame's cost outside the rasteriser lives in a handful of per-pixel
 * kernels: packing float colour to 8888 (pack_argb), unpacking it back, the
 * B5G6R5 and BC1 texture decodes (res_decode_pixels_inner), the in-place
 * red/blue swizzle that the same routine does for R8G8B8A8 textures, and the
 * blend/modulate math the span kernel runs (span_blend_over, span_blend_add,
 * span_blend_mod, span_modulate). swrast.c already carries AVX2 forms of the
 * blends at 256 bits; nothing here is 512 bits wide and nothing measures the
 * decodes in isolation.
 *
 * For each operation this file carries three implementations - a scalar
 * reference, an AVX2 form eight pixels wide, and an AVX512 form sixteen wide -
 * checks the two vector forms against the scalar one (bit-exact where the
 * arithmetic is integer, +/-1 is never needed because the vector rounding is
 * written to match the scalar rounding exactly), and then times all three over
 * a framebuffer-sized buffer. The number that matters is pixels per second and
 * the ratio to scalar; a widening that does not move that ratio is not worth
 * carrying into the DLL.
 *
 * Build (native x64, so it runs where it is built):
 *   zig cc -O2 -mavx512f -mavx512bw -mavx512vl -target x86_64-windows-gnu \
 *          -o pixbench.exe pixbench.c
 * The -mavx512* flags only let the 512-bit *encodings* exist in the object; the
 * paths that use them are still gated on a runtime cpuid check, so the binary
 * runs on an AVX2-only machine and simply reports the 512 path as absent.
 */
#include <windows.h>
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- cpu feature gate ---------------------------------------------- */

static int g_have_avx2;
static int g_have_avx512; /* F + BW + VL together; that is what the ops below need */

/* Raw cpuid + xgetbv, so the check reflects the running machine and needs no
 * libgcc helper. A feature bit in cpuid is not enough on its own: the registers
 * only hold their state across a context switch if the OS enabled them in XCR0,
 * so AVX2 also requires XCR0 bits 1-2 (SSE, YMM) and AVX512 additionally bits
 * 5-7 (opmask, ZMM hi256, hi16 ZMM). Skipping that is how a "supported" AVX512
 * instruction faults on a kernel that never turned it on. */
static void cpuid_count(unsigned leaf, unsigned sub, unsigned r[4])
{
	__asm__ volatile("cpuid"
			 : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
			 : "a"(leaf), "c"(sub));
}

static unsigned long long xgetbv0(void)
{
	unsigned lo, hi;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	return ((unsigned long long)hi << 32) | lo;
}

static void detect_cpu(void)
{
	unsigned r1[4], r7[4];
	unsigned long long xcr0;
	int osxsave, ymm, zmm;

	cpuid_count(1, 0, r1);
	osxsave = (r1[2] >> 27) & 1;
	if (!osxsave)
		return; /* cannot even query xgetbv; treat as scalar-only */
	xcr0 = xgetbv0();
	ymm = (xcr0 & 0x6) == 0x6;         /* SSE + YMM state saved */
	zmm = ymm && (xcr0 & 0xe0) == 0xe0; /* + opmask, ZMM hi256, hi16 ZMM */

	cpuid_count(7, 0, r7);
	g_have_avx2 = ymm && ((r7[1] >> 5) & 1);
	g_have_avx512 = zmm && ((r7[1] >> 16) & 1) /* F  */
			&& ((r7[1] >> 30) & 1)     /* BW */
			&& ((r7[1] >> 31) & 1);    /* VL */
}

/* ---------- 1. pack: float RGBA in [0,1] -> 0xAARRGGBB ------------------------
 *
 * pack_argb() in d3d11_sw.c, per pixel: clamp each channel to [0,1], scale by
 * 255, add 0.5, truncate, then assemble A<<24|R<<16|G<<8|B. Every path here
 * truncates a scaled-and-biased float so the three agree exactly. */

static void pack_scalar(uint32_t *dst, const float *r, const float *g, const float *b,
			const float *a, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		float cr = r[i], cg = g[i], cb = b[i], ca = a[i];
		int ir, ig, ib, ia;
		cr = cr < 0 ? 0 : (cr > 1 ? 1 : cr);
		cg = cg < 0 ? 0 : (cg > 1 ? 1 : cg);
		cb = cb < 0 ? 0 : (cb > 1 ? 1 : cb);
		ca = ca < 0 ? 0 : (ca > 1 ? 1 : ca);
		ir = (int)(cr * 255.0f + 0.5f);
		ig = (int)(cg * 255.0f + 0.5f);
		ib = (int)(cb * 255.0f + 0.5f);
		ia = (int)(ca * 255.0f + 0.5f);
		dst[i] = ((uint32_t)ia << 24) | ((uint32_t)ir << 16) | ((uint32_t)ig << 8) |
			 (uint32_t)ib;
	}
}

__attribute__((target("avx2")))
static __m256i pack8(__m256 r, __m256 g, __m256 b, __m256 a)
{
	const __m256 zero = _mm256_setzero_ps(), one = _mm256_set1_ps(1.0f);
	const __m256 s = _mm256_set1_ps(255.0f), half = _mm256_set1_ps(0.5f);
	__m256i ir, ig, ib, ia;
	r = _mm256_min_ps(_mm256_max_ps(r, zero), one);
	g = _mm256_min_ps(_mm256_max_ps(g, zero), one);
	b = _mm256_min_ps(_mm256_max_ps(b, zero), one);
	a = _mm256_min_ps(_mm256_max_ps(a, zero), one);
	/* truncating cast of (c*255 + 0.5) reproduces the scalar (int) exactly */
	ir = _mm256_cvttps_epi32(_mm256_add_ps(_mm256_mul_ps(r, s), half));
	ig = _mm256_cvttps_epi32(_mm256_add_ps(_mm256_mul_ps(g, s), half));
	ib = _mm256_cvttps_epi32(_mm256_add_ps(_mm256_mul_ps(b, s), half));
	ia = _mm256_cvttps_epi32(_mm256_add_ps(_mm256_mul_ps(a, s), half));
	return _mm256_or_si256(
		_mm256_or_si256(_mm256_slli_epi32(ia, 24), _mm256_slli_epi32(ir, 16)),
		_mm256_or_si256(_mm256_slli_epi32(ig, 8), ib));
}

__attribute__((target("avx2")))
static void pack_avx2(uint32_t *dst, const float *r, const float *g, const float *b,
		      const float *a, size_t n)
{
	size_t i = 0;
	for (; i + 8 <= n; i += 8)
		_mm256_storeu_si256((__m256i *)(dst + i),
				    pack8(_mm256_loadu_ps(r + i), _mm256_loadu_ps(g + i),
					  _mm256_loadu_ps(b + i), _mm256_loadu_ps(a + i)));
	if (i < n)
		pack_scalar(dst + i, r + i, g + i, b + i, a + i, n - i);
}

__attribute__((target("avx512f,evex512")))
static void pack_avx512(uint32_t *dst, const float *r, const float *g, const float *b,
			const float *a, size_t n)
{
	const __m512 zero = _mm512_setzero_ps(), one = _mm512_set1_ps(1.0f);
	const __m512 s = _mm512_set1_ps(255.0f), half = _mm512_set1_ps(0.5f);
	size_t i = 0;
	for (; i + 16 <= n; i += 16) {
		__m512 r0 = _mm512_min_ps(_mm512_max_ps(_mm512_loadu_ps(r + i), zero), one);
		__m512 g0 = _mm512_min_ps(_mm512_max_ps(_mm512_loadu_ps(g + i), zero), one);
		__m512 b0 = _mm512_min_ps(_mm512_max_ps(_mm512_loadu_ps(b + i), zero), one);
		__m512 a0 = _mm512_min_ps(_mm512_max_ps(_mm512_loadu_ps(a + i), zero), one);
		__m512i ir = _mm512_cvttps_epi32(_mm512_add_ps(_mm512_mul_ps(r0, s), half));
		__m512i ig = _mm512_cvttps_epi32(_mm512_add_ps(_mm512_mul_ps(g0, s), half));
		__m512i ib = _mm512_cvttps_epi32(_mm512_add_ps(_mm512_mul_ps(b0, s), half));
		__m512i ia = _mm512_cvttps_epi32(_mm512_add_ps(_mm512_mul_ps(a0, s), half));
		__m512i v = _mm512_or_si512(
			_mm512_or_si512(_mm512_slli_epi32(ia, 24), _mm512_slli_epi32(ir, 16)),
			_mm512_or_si512(_mm512_slli_epi32(ig, 8), ib));
		_mm512_storeu_si512((void *)(dst + i), v);
	}
	if (i < n)
		pack_scalar(dst + i, r + i, g + i, b + i, a + i, n - i);
}

/* ---------- 2. unpack: 0xAARRGGBB -> float RGBA in [0,1] ------------------- */

static void unpack_scalar(float *r, float *g, float *b, float *a, const uint32_t *src,
			  size_t n)
{
	const float inv = 1.0f / 255.0f;
	size_t i;
	for (i = 0; i < n; i++) {
		uint32_t v = src[i];
		a[i] = ((v >> 24) & 255) * inv;
		r[i] = ((v >> 16) & 255) * inv;
		g[i] = ((v >> 8) & 255) * inv;
		b[i] = (v & 255) * inv;
	}
}

__attribute__((target("avx2")))
static void unpack_avx2(float *r, float *g, float *b, float *a, const uint32_t *src,
			size_t n)
{
	const __m256 inv = _mm256_set1_ps(1.0f / 255.0f);
	const __m256i m = _mm256_set1_epi32(255);
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		__m256i v = _mm256_loadu_si256((const __m256i *)(src + i));
		_mm256_storeu_ps(a + i, _mm256_mul_ps(_mm256_cvtepi32_ps(
			_mm256_and_si256(_mm256_srli_epi32(v, 24), m)), inv));
		_mm256_storeu_ps(r + i, _mm256_mul_ps(_mm256_cvtepi32_ps(
			_mm256_and_si256(_mm256_srli_epi32(v, 16), m)), inv));
		_mm256_storeu_ps(g + i, _mm256_mul_ps(_mm256_cvtepi32_ps(
			_mm256_and_si256(_mm256_srli_epi32(v, 8), m)), inv));
		_mm256_storeu_ps(b + i, _mm256_mul_ps(_mm256_cvtepi32_ps(
			_mm256_and_si256(v, m)), inv));
	}
	if (i < n)
		unpack_scalar(r + i, g + i, b + i, a + i, src + i, n - i);
}

__attribute__((target("avx512f,evex512")))
static void unpack_avx512(float *r, float *g, float *b, float *a, const uint32_t *src,
			  size_t n)
{
	const __m512 inv = _mm512_set1_ps(1.0f / 255.0f);
	const __m512i m = _mm512_set1_epi32(255);
	size_t i = 0;
	for (; i + 16 <= n; i += 16) {
		__m512i v = _mm512_loadu_si512((const void *)(src + i));
		_mm512_storeu_ps(a + i, _mm512_mul_ps(_mm512_cvtepi32_ps(
			_mm512_and_si512(_mm512_srli_epi32(v, 24), m)), inv));
		_mm512_storeu_ps(r + i, _mm512_mul_ps(_mm512_cvtepi32_ps(
			_mm512_and_si512(_mm512_srli_epi32(v, 16), m)), inv));
		_mm512_storeu_ps(g + i, _mm512_mul_ps(_mm512_cvtepi32_ps(
			_mm512_and_si512(_mm512_srli_epi32(v, 8), m)), inv));
		_mm512_storeu_ps(b + i, _mm512_mul_ps(_mm512_cvtepi32_ps(
			_mm512_and_si512(v, m)), inv));
	}
	if (i < n)
		unpack_scalar(r + i, g + i, b + i, a + i, src + i, n - i);
}

/* ---------- 3. decode B5G6R5_UNORM -> 0xFFRRGGBB --------------------------
 *
 * The channel-decode arithmetic a 16-bit texture pays per texel: expand 5/6/5
 * to 8 bits by bit-replication (r8 = r5<<3 | r5>>2), which is what makes 0 map
 * to 0 and 31 map to 255 exactly. Pure integer, so all three paths agree bit
 * for bit. */

static void dec565_scalar(uint32_t *dst, const uint16_t *src, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		unsigned v = src[i];
		unsigned r5 = (v >> 11) & 31, g6 = (v >> 5) & 63, b5 = v & 31;
		unsigned r8 = (r5 << 3) | (r5 >> 2);
		unsigned g8 = (g6 << 2) | (g6 >> 4);
		unsigned b8 = (b5 << 3) | (b5 >> 2);
		dst[i] = 0xff000000u | (r8 << 16) | (g8 << 8) | b8;
	}
}

__attribute__((target("avx2")))
static void dec565_avx2(uint32_t *dst, const uint16_t *src, size_t n)
{
	const __m256i r5m = _mm256_set1_epi32(31), g6m = _mm256_set1_epi32(63);
	const __m256i opaque = _mm256_set1_epi32((int)0xff000000u);
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		/* widen eight u16 to eight i32 */
		__m256i v = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)(src + i)));
		__m256i r5 = _mm256_and_si256(_mm256_srli_epi32(v, 11), r5m);
		__m256i g6 = _mm256_and_si256(_mm256_srli_epi32(v, 5), g6m);
		__m256i b5 = _mm256_and_si256(v, r5m);
		__m256i r8 = _mm256_or_si256(_mm256_slli_epi32(r5, 3), _mm256_srli_epi32(r5, 2));
		__m256i g8 = _mm256_or_si256(_mm256_slli_epi32(g6, 2), _mm256_srli_epi32(g6, 4));
		__m256i b8 = _mm256_or_si256(_mm256_slli_epi32(b5, 3), _mm256_srli_epi32(b5, 2));
		__m256i out = _mm256_or_si256(
			_mm256_or_si256(opaque, _mm256_slli_epi32(r8, 16)),
			_mm256_or_si256(_mm256_slli_epi32(g8, 8), b8));
		_mm256_storeu_si256((__m256i *)(dst + i), out);
	}
	if (i < n)
		dec565_scalar(dst + i, src + i, n - i);
}

__attribute__((target("avx512bw,evex512")))
static void dec565_avx512(uint32_t *dst, const uint16_t *src, size_t n)
{
	const __m512i r5m = _mm512_set1_epi32(31), g6m = _mm512_set1_epi32(63);
	const __m512i opaque = _mm512_set1_epi32((int)0xff000000u);
	size_t i = 0;
	for (; i + 16 <= n; i += 16) {
		__m512i v = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(src + i)));
		__m512i r5 = _mm512_and_si512(_mm512_srli_epi32(v, 11), r5m);
		__m512i g6 = _mm512_and_si512(_mm512_srli_epi32(v, 5), g6m);
		__m512i b5 = _mm512_and_si512(v, r5m);
		__m512i r8 = _mm512_or_si512(_mm512_slli_epi32(r5, 3), _mm512_srli_epi32(r5, 2));
		__m512i g8 = _mm512_or_si512(_mm512_slli_epi32(g6, 2), _mm512_srli_epi32(g6, 4));
		__m512i b8 = _mm512_or_si512(_mm512_slli_epi32(b5, 3), _mm512_srli_epi32(b5, 2));
		__m512i out = _mm512_or_si512(
			_mm512_or_si512(opaque, _mm512_slli_epi32(r8, 16)),
			_mm512_or_si512(_mm512_slli_epi32(g8, 8), b8));
		_mm512_storeu_si512((void *)(dst + i), out);
	}
	if (i < n)
		dec565_scalar(dst + i, src + i, n - i);
}

/* ---------- 4. red/blue swizzle: 0xAARRGGBB <-> 0xAABBGGRR -----------------
 *
 * res_decode_pixels_inner's alias path: p = (v & 0xff00ff00) | ((v>>16)&0xff) |
 * ((v&0xff)<<16). The game hands some textures in R8G8B8A8 and the rasteriser
 * wants B8G8R8A8, so this runs in place over the whole texture on first use. */

static void swizzle_scalar(uint32_t *dst, const uint32_t *src, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		uint32_t v = src[i];
		dst[i] = (v & 0xff00ff00u) | ((v & 0x00ff0000u) >> 16) |
			 ((v & 0x000000ffu) << 16);
	}
}

__attribute__((target("avx2")))
static void swizzle_avx2(uint32_t *dst, const uint32_t *src, size_t n)
{
	/* A byte shuffle beats the shift/mask entirely: one vpshufb per 8 pixels,
	 * swapping bytes 0 and 2 of every 32-bit lane. The control is the same in
	 * both 128-bit halves. */
	const __m256i ctl = _mm256_setr_epi8(
		2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
		2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
	size_t i = 0;
	for (; i + 8 <= n; i += 8)
		_mm256_storeu_si256((__m256i *)(dst + i),
				    _mm256_shuffle_epi8(
					    _mm256_loadu_si256((const __m256i *)(src + i)), ctl));
	if (i < n)
		swizzle_scalar(dst + i, src + i, n - i);
}

__attribute__((target("avx512bw,evex512")))
static void swizzle_avx512(uint32_t *dst, const uint32_t *src, size_t n)
{
	const __m512i ctl = _mm512_broadcast_i32x4(_mm_setr_epi8(
		2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15));
	size_t i = 0;
	for (; i + 16 <= n; i += 16)
		_mm512_storeu_si512((void *)(dst + i),
				    _mm512_shuffle_epi8(
					    _mm512_loadu_si512((const void *)(src + i)), ctl));
	if (i < n)
		swizzle_scalar(dst + i, src + i, n - i);
}

/* ---------- 5. BC1/DXT1 texel expand -------------------------------------
 *
 * The scatter half of decode_dxt_color: a block's four palette entries are
 * built once (scalar - it is four colours, not a hot loop), then its sixteen
 * two-bit indices each select one. That select is the wide part: vpermd picks
 * sixteen colours from a four-entry table in one instruction. Output order is
 * row-major within the 4x4 block, matching the reference. */

static void dxt_palette(const unsigned char *blk, uint32_t out[4])
{
	unsigned c0 = blk[0] | ((unsigned)blk[1] << 8);
	unsigned c1 = blk[2] | ((unsigned)blk[3] << 8);
	unsigned r0 = (c0 >> 11) & 31, g0 = (c0 >> 5) & 63, b0 = c0 & 31;
	unsigned r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
	uint32_t a = 0xff000000u | (((r0 << 3 | r0 >> 2)) << 16) |
		     (((g0 << 2 | g0 >> 4)) << 8) | (b0 << 3 | b0 >> 2);
	uint32_t b = 0xff000000u | (((r1 << 3 | r1 >> 2)) << 16) |
		     (((g1 << 2 | g1 >> 4)) << 8) | (b1 << 3 | b1 >> 2);
	out[0] = a;
	out[1] = b;
	if (c0 > c1) {
		out[2] = 0xff000000u |
			 (((((a >> 16) & 255) * 2 + ((b >> 16) & 255)) / 3) << 16) |
			 (((((a >> 8) & 255) * 2 + ((b >> 8) & 255)) / 3) << 8) |
			 (((a & 255) * 2 + (b & 255)) / 3);
		out[3] = 0xff000000u |
			 (((((a >> 16) & 255) + ((b >> 16) & 255) * 2) / 3) << 16) |
			 (((((a >> 8) & 255) + ((b >> 8) & 255) * 2) / 3) << 8) |
			 (((a & 255) + (b & 255) * 2) / 3);
	} else {
		out[2] = 0xff000000u |
			 (((((a >> 16) & 255) + ((b >> 16) & 255)) / 2) << 16) |
			 (((((a >> 8) & 255) + ((b >> 8) & 255)) / 2) << 8) |
			 (((a & 255) + (b & 255)) / 2);
		out[3] = 0;
	}
}

static void dxt1_scalar(uint32_t *dst, const unsigned char *blocks, size_t nblk)
{
	size_t k;
	for (k = 0; k < nblk; k++) {
		const unsigned char *blk = blocks + k * 8;
		uint32_t pal[4];
		unsigned idx = blk[4] | ((unsigned)blk[5] << 8) | ((unsigned)blk[6] << 16) |
			       ((unsigned)blk[7] << 24);
		int t;
		dxt_palette(blk, pal);
		for (t = 0; t < 16; t++)
			dst[k * 16 + t] = pal[(idx >> (2 * t)) & 3];
	}
}

__attribute__((target("avx2")))
static void dxt1_avx2(uint32_t *dst, const unsigned char *blocks, size_t nblk)
{
	size_t k;
	for (k = 0; k < nblk; k++) {
		const unsigned char *blk = blocks + k * 8;
		uint32_t pal[4];
		unsigned idx = blk[4] | ((unsigned)blk[5] << 8) | ((unsigned)blk[6] << 16) |
			       ((unsigned)blk[7] << 24);
		/* palette in lanes 0..3; lanes 4..7 unused since indices are 0..3 */
		__m256i palv = _mm256_castsi128_si256(
			_mm_setr_epi32((int)pal[0], (int)pal[1], (int)pal[2], (int)pal[3]));
		__m256i lo, hi;
		dxt_palette(blk, pal);
		palv = _mm256_castsi128_si256(
			_mm_setr_epi32((int)pal[0], (int)pal[1], (int)pal[2], (int)pal[3]));
		/* eight 2-bit indices per half, expanded to i32 lanes */
		lo = _mm256_setr_epi32((idx >> 0) & 3, (idx >> 2) & 3, (idx >> 4) & 3,
				       (idx >> 6) & 3, (idx >> 8) & 3, (idx >> 10) & 3,
				       (idx >> 12) & 3, (idx >> 14) & 3);
		hi = _mm256_setr_epi32((idx >> 16) & 3, (idx >> 18) & 3, (idx >> 20) & 3,
				       (idx >> 22) & 3, (idx >> 24) & 3, (idx >> 26) & 3,
				       (idx >> 28) & 3, (idx >> 30) & 3);
		_mm256_storeu_si256((__m256i *)(dst + k * 16),
				    _mm256_permutevar8x32_epi32(palv, lo));
		_mm256_storeu_si256((__m256i *)(dst + k * 16 + 8),
				    _mm256_permutevar8x32_epi32(palv, hi));
	}
}

__attribute__((target("avx512f,evex512")))
static void dxt1_avx512(uint32_t *dst, const unsigned char *blocks, size_t nblk)
{
	size_t k;
	for (k = 0; k < nblk; k++) {
		const unsigned char *blk = blocks + k * 8;
		uint32_t pal[4];
		unsigned idx = blk[4] | ((unsigned)blk[5] << 8) | ((unsigned)blk[6] << 16) |
			       ((unsigned)blk[7] << 24);
		__m512i palv, idxv;
		dxt_palette(blk, pal);
		/* one 512-bit table of 16 lanes; only 0..3 are ever addressed */
		palv = _mm512_castsi128_si512(
			_mm_setr_epi32((int)pal[0], (int)pal[1], (int)pal[2], (int)pal[3]));
		idxv = _mm512_setr_epi32((idx >> 0) & 3, (idx >> 2) & 3, (idx >> 4) & 3,
					 (idx >> 6) & 3, (idx >> 8) & 3, (idx >> 10) & 3,
					 (idx >> 12) & 3, (idx >> 14) & 3, (idx >> 16) & 3,
					 (idx >> 18) & 3, (idx >> 20) & 3, (idx >> 22) & 3,
					 (idx >> 24) & 3, (idx >> 26) & 3, (idx >> 28) & 3,
					 (idx >> 30) & 3);
		_mm512_storeu_si512((void *)(dst + k * 16),
				    _mm512_permutexvar_epi32(idxv, palv));
	}
}

/* ---------- 6/7. saturating colour add and subtract -----------------------
 *
 * The additive-blend accumulate (span_blend_add's final _mm256_adds_epu8) and
 * a REVSUBTRACT taken to its limit: per-channel saturating add / subtract of
 * two colour buffers. adds/subs_epu8 clamp at 0 and 255 in one instruction, so
 * the scalar reference has to clamp by hand to agree. */

static uint32_t sat_add_px(uint32_t x, uint32_t y)
{
	uint32_t out = 0;
	int ch;
	for (ch = 0; ch < 4; ch++) {
		int s = (int)((x >> (ch * 8)) & 255) + (int)((y >> (ch * 8)) & 255);
		if (s > 255)
			s = 255;
		out |= (uint32_t)s << (ch * 8);
	}
	return out;
}

static uint32_t sat_sub_px(uint32_t x, uint32_t y)
{
	uint32_t out = 0;
	int ch;
	for (ch = 0; ch < 4; ch++) {
		int s = (int)((x >> (ch * 8)) & 255) - (int)((y >> (ch * 8)) & 255);
		if (s < 0)
			s = 0;
		out |= (uint32_t)s << (ch * 8);
	}
	return out;
}

static void add_scalar(uint32_t *dst, const uint32_t *a, const uint32_t *b, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		dst[i] = sat_add_px(a[i], b[i]);
}
static void sub_scalar(uint32_t *dst, const uint32_t *a, const uint32_t *b, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		dst[i] = sat_sub_px(a[i], b[i]);
}

__attribute__((target("avx2")))
static void add_avx2(uint32_t *dst, const uint32_t *a, const uint32_t *b, size_t n)
{
	size_t i = 0;
	for (; i + 8 <= n; i += 8)
		_mm256_storeu_si256((__m256i *)(dst + i),
				    _mm256_adds_epu8(_mm256_loadu_si256((const __m256i *)(a + i)),
						     _mm256_loadu_si256((const __m256i *)(b + i))));
	if (i < n)
		add_scalar(dst + i, a + i, b + i, n - i);
}
__attribute__((target("avx2")))
static void sub_avx2(uint32_t *dst, const uint32_t *a, const uint32_t *b, size_t n)
{
	size_t i = 0;
	for (; i + 8 <= n; i += 8)
		_mm256_storeu_si256((__m256i *)(dst + i),
				    _mm256_subs_epu8(_mm256_loadu_si256((const __m256i *)(a + i)),
						     _mm256_loadu_si256((const __m256i *)(b + i))));
	if (i < n)
		sub_scalar(dst + i, a + i, b + i, n - i);
}

__attribute__((target("avx512bw,evex512")))
static void add_avx512(uint32_t *dst, const uint32_t *a, const uint32_t *b, size_t n)
{
	size_t i = 0;
	for (; i + 16 <= n; i += 16)
		_mm512_storeu_si512((void *)(dst + i),
				    _mm512_adds_epu8(_mm512_loadu_si512((const void *)(a + i)),
						     _mm512_loadu_si512((const void *)(b + i))));
	if (i < n)
		add_scalar(dst + i, a + i, b + i, n - i);
}
__attribute__((target("avx512bw,evex512")))
static void sub_avx512(uint32_t *dst, const uint32_t *a, const uint32_t *b, size_t n)
{
	size_t i = 0;
	for (; i + 16 <= n; i += 16)
		_mm512_storeu_si512((void *)(dst + i),
				    _mm512_subs_epu8(_mm512_loadu_si512((const void *)(a + i)),
						     _mm512_loadu_si512((const void *)(b + i))));
	if (i < n)
		sub_scalar(dst + i, a + i, b + i, n - i);
}

/* ---------- 8. source-over mix -------------------------------------------
 *
 * span_blend_over, straight: out = src*sa/255 + dst*(255-sa)/255, alpha
 * carried through the same weights, div255 rounded as (v + (v>>8)) >> 8 after a
 * +128 bias. That bias-and-shift is the exact div255 the vector code uses, so
 * the scalar reference uses it too and the three agree bit for bit. */

static uint32_t div255(unsigned v)
{
	v += 128;
	return (v + (v >> 8)) >> 8;
}

static uint32_t over_px(uint32_t src, uint32_t dst)
{
	unsigned sa = (src >> 24) & 255, isa = 255 - sa;
	uint32_t out = 0;
	int ch;
	for (ch = 0; ch < 4; ch++) {
		unsigned s = (src >> (ch * 8)) & 255, d = (dst >> (ch * 8)) & 255;
		out |= div255(s * sa + d * isa) << (ch * 8);
	}
	return out;
}

static void over_scalar(uint32_t *dst, const uint32_t *src, const uint32_t *bg, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		dst[i] = over_px(src[i], bg[i]);
}

/* div255((a*wlo, ...)) on two channels packed as 0x00cc00cc, matching swrast's
 * span_lerp/span_blend rounding. */
__attribute__((target("avx2")))
static __m256i over8(__m256i src, __m256i dst)
{
	const __m256i lomask = _mm256_set1_epi32(0x00ff00ff);
	__m256i sa = _mm256_srli_epi32(src, 24);
	__m256i saw = _mm256_or_si256(sa, _mm256_slli_epi32(sa, 16));
	__m256i isaw = _mm256_sub_epi16(_mm256_set1_epi16(255), saw);
	__m256i srb = _mm256_and_si256(src, lomask);
	__m256i sag = _mm256_and_si256(_mm256_srli_epi32(src, 8), lomask);
	__m256i drb = _mm256_and_si256(dst, lomask);
	__m256i dag = _mm256_and_si256(_mm256_srli_epi32(dst, 8), lomask);
	__m256i rb = _mm256_add_epi16(
		_mm256_add_epi16(_mm256_mullo_epi16(srb, saw), _mm256_mullo_epi16(drb, isaw)),
		_mm256_set1_epi16(128));
	__m256i ag = _mm256_add_epi16(
		_mm256_add_epi16(_mm256_mullo_epi16(sag, saw), _mm256_mullo_epi16(dag, isaw)),
		_mm256_set1_epi16(128));
	rb = _mm256_srli_epi16(_mm256_add_epi16(rb, _mm256_srli_epi16(rb, 8)), 8);
	ag = _mm256_srli_epi16(_mm256_add_epi16(ag, _mm256_srli_epi16(ag, 8)), 8);
	return _mm256_or_si256(_mm256_and_si256(rb, lomask),
			       _mm256_slli_epi32(_mm256_and_si256(ag, lomask), 8));
}

__attribute__((target("avx2")))
static void over_avx2(uint32_t *dst, const uint32_t *src, const uint32_t *bg, size_t n)
{
	size_t i = 0;
	for (; i + 8 <= n; i += 8)
		_mm256_storeu_si256((__m256i *)(dst + i),
				    over8(_mm256_loadu_si256((const __m256i *)(src + i)),
					  _mm256_loadu_si256((const __m256i *)(bg + i))));
	if (i < n)
		over_scalar(dst + i, src + i, bg + i, n - i);
}

__attribute__((target("avx512bw,evex512")))
static void over_avx512(uint32_t *dst, const uint32_t *src, const uint32_t *bg, size_t n)
{
	const __m512i lomask = _mm512_set1_epi32(0x00ff00ff);
	const __m512i c255 = _mm512_set1_epi16(255), c128 = _mm512_set1_epi16(128);
	size_t i = 0;
	for (; i + 16 <= n; i += 16) {
		__m512i s = _mm512_loadu_si512((const void *)(src + i));
		__m512i d = _mm512_loadu_si512((const void *)(bg + i));
		__m512i sa = _mm512_srli_epi32(s, 24);
		__m512i saw = _mm512_or_si512(sa, _mm512_slli_epi32(sa, 16));
		__m512i isaw = _mm512_sub_epi16(c255, saw);
		__m512i srb = _mm512_and_si512(s, lomask);
		__m512i sag = _mm512_and_si512(_mm512_srli_epi32(s, 8), lomask);
		__m512i drb = _mm512_and_si512(d, lomask);
		__m512i dag = _mm512_and_si512(_mm512_srli_epi32(d, 8), lomask);
		__m512i rb = _mm512_add_epi16(
			_mm512_add_epi16(_mm512_mullo_epi16(srb, saw), _mm512_mullo_epi16(drb, isaw)),
			c128);
		__m512i ag = _mm512_add_epi16(
			_mm512_add_epi16(_mm512_mullo_epi16(sag, saw), _mm512_mullo_epi16(dag, isaw)),
			c128);
		rb = _mm512_srli_epi16(_mm512_add_epi16(rb, _mm512_srli_epi16(rb, 8)), 8);
		ag = _mm512_srli_epi16(_mm512_add_epi16(ag, _mm512_srli_epi16(ag, 8)), 8);
		_mm512_storeu_si512((void *)(dst + i),
				    _mm512_or_si512(_mm512_and_si512(rb, lomask),
						    _mm512_slli_epi32(_mm512_and_si512(ag, lomask), 8)));
	}
	if (i < n)
		over_scalar(dst + i, src + i, bg + i, n - i);
}

/* ---------- 9. modulate: div255(src * flat) per channel -------------------
 *
 * span_modulate: the vertex colour scaling every sampled texel, div255 rounded
 * the same way. Flat is a single constant colour here, the common case. */

static uint32_t mod_px(uint32_t c, uint32_t f)
{
	uint32_t out = 0;
	int ch;
	for (ch = 0; ch < 4; ch++) {
		unsigned cc = (c >> (ch * 8)) & 255, ff = (f >> (ch * 8)) & 255;
		out |= div255(cc * ff) << (ch * 8);
	}
	return out;
}

static void mod_scalar(uint32_t *dst, const uint32_t *src, uint32_t flat, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		dst[i] = mod_px(src[i], flat);
}

__attribute__((target("avx2")))
static void mod_avx2(uint32_t *dst, const uint32_t *src, uint32_t flat, size_t n)
{
	const __m256i lomask = _mm256_set1_epi32(0x00ff00ff);
	__m256i f = _mm256_set1_epi32((int)flat);
	__m256i frb = _mm256_and_si256(f, lomask);
	__m256i fag = _mm256_and_si256(_mm256_srli_epi32(f, 8), lomask);
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		__m256i col = _mm256_loadu_si256((const __m256i *)(src + i));
		__m256i crb = _mm256_and_si256(col, lomask);
		__m256i cag = _mm256_and_si256(_mm256_srli_epi32(col, 8), lomask);
		__m256i rb = _mm256_add_epi16(_mm256_mullo_epi16(crb, frb), _mm256_set1_epi16(128));
		__m256i ag = _mm256_add_epi16(_mm256_mullo_epi16(cag, fag), _mm256_set1_epi16(128));
		rb = _mm256_srli_epi16(_mm256_add_epi16(rb, _mm256_srli_epi16(rb, 8)), 8);
		ag = _mm256_srli_epi16(_mm256_add_epi16(ag, _mm256_srli_epi16(ag, 8)), 8);
		_mm256_storeu_si256((__m256i *)(dst + i),
				    _mm256_or_si256(_mm256_and_si256(rb, lomask),
						    _mm256_slli_epi32(_mm256_and_si256(ag, lomask), 8)));
	}
	if (i < n)
		mod_scalar(dst + i, src + i, flat, n - i);
}

__attribute__((target("avx512bw,evex512")))
static void mod_avx512(uint32_t *dst, const uint32_t *src, uint32_t flat, size_t n)
{
	const __m512i lomask = _mm512_set1_epi32(0x00ff00ff);
	const __m512i c128 = _mm512_set1_epi16(128);
	__m512i f = _mm512_set1_epi32((int)flat);
	__m512i frb = _mm512_and_si512(f, lomask);
	__m512i fag = _mm512_and_si512(_mm512_srli_epi32(f, 8), lomask);
	size_t i = 0;
	for (; i + 16 <= n; i += 16) {
		__m512i col = _mm512_loadu_si512((const void *)(src + i));
		__m512i crb = _mm512_and_si512(col, lomask);
		__m512i cag = _mm512_and_si512(_mm512_srli_epi32(col, 8), lomask);
		__m512i rb = _mm512_add_epi16(_mm512_mullo_epi16(crb, frb), c128);
		__m512i ag = _mm512_add_epi16(_mm512_mullo_epi16(cag, fag), c128);
		rb = _mm512_srli_epi16(_mm512_add_epi16(rb, _mm512_srli_epi16(rb, 8)), 8);
		ag = _mm512_srli_epi16(_mm512_add_epi16(ag, _mm512_srli_epi16(ag, 8)), 8);
		_mm512_storeu_si512((void *)(dst + i),
				    _mm512_or_si512(_mm512_and_si512(rb, lomask),
						    _mm512_slli_epi32(_mm512_and_si512(ag, lomask), 8)));
	}
	if (i < n)
		mod_scalar(dst + i, src + i, flat, n - i);
}

/* ---------- 10/11. YCbCr baseband encode / decode (BT.601, JFIF full range) --
 *
 * "Rendering as if it were video" starts here: the colour-equivalent transform
 * every video codec runs before it touches an entropy coder. RGB is rotated
 * into one luma axis (Y) and two opponent-colour chroma axes - Cb is the
 * blue<->yellow difference, Cr the red<->green - which is literally the
 * blue/yellow and red/green rendering the request asks for, and the space in
 * which chroma can be subsampled and baseband-encoded.
 *
 * Fixed point in Q8 (coefficients * 256) so the scalar and vector paths are the
 * same integer arithmetic and agree exactly. These are the standard JFIF
 * integer coefficients:
 *   Y  = ( 77R + 150G +  29B) >> 8
 *   Cb = (-43R -  85G + 128B) >> 8 + 128   (blue - yellow)
 *   Cr = (128R - 107G -  21B) >> 8 + 128   (red  - green)
 * and the inverse:
 *   R = Y +               359(Cr-128) >> 8
 *   G = Y - ( 88(Cb-128) + 183(Cr-128)) >> 8
 *   B = Y +  454(Cb-128)              >> 8
 * Output of the encode is packed 0xFF,Y,Cb,Cr so it reuses the 8888 tooling;
 * the decode reads that back and writes 0xFFRRGGBB. */

static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void ycc_enc_scalar(uint32_t *dst, const uint32_t *src, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		int r = (src[i] >> 16) & 255, g = (src[i] >> 8) & 255, b = src[i] & 255;
		int y = (77 * r + 150 * g + 29 * b + 128) >> 8;
		int cb = ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128;
		int cr = ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
		dst[i] = 0xff000000u | ((uint32_t)clamp255(y) << 16) |
			 ((uint32_t)clamp255(cb) << 8) | (uint32_t)clamp255(cr);
	}
}

static void ycc_dec_scalar(uint32_t *dst, const uint32_t *src, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		int y = (src[i] >> 16) & 255, cb = ((src[i] >> 8) & 255) - 128,
		    cr = (src[i] & 255) - 128;
		int r = y + ((359 * cr + 128) >> 8);
		int g = y - ((88 * cb + 183 * cr + 128) >> 8);
		int b = y + ((454 * cb + 128) >> 8);
		dst[i] = 0xff000000u | ((uint32_t)clamp255(r) << 16) |
			 ((uint32_t)clamp255(g) << 8) | (uint32_t)clamp255(b);
	}
}

/* extract r,g,b as i32 lanes from packed 0xAARRGGBB */
#define UNPK256(v, r, g, b)                                                              \
	do {                                                                             \
		__m256i _m = _mm256_set1_epi32(255);                                     \
		r = _mm256_and_si256(_mm256_srli_epi32(v, 16), _m);                     \
		g = _mm256_and_si256(_mm256_srli_epi32(v, 8), _m);                      \
		b = _mm256_and_si256(v, _m);                                            \
	} while (0)

__attribute__((target("avx2")))
static __m256i clamp255_256(__m256i v)
{
	return _mm256_max_epi32(_mm256_setzero_si256(),
				_mm256_min_epi32(v, _mm256_set1_epi32(255)));
}

__attribute__((target("avx2")))
static void ycc_enc_avx2(uint32_t *dst, const uint32_t *src, size_t n)
{
	const __m256i op = _mm256_set1_epi32((int)0xff000000u), r128 = _mm256_set1_epi32(128);
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		__m256i v = _mm256_loadu_si256((const __m256i *)(src + i)), r, g, b, y, cb, cr;
		UNPK256(v, r, g, b);
		y = _mm256_srai_epi32(
			_mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(r, _mm256_set1_epi32(77)),
							  _mm256_mullo_epi32(g, _mm256_set1_epi32(150))),
					 _mm256_add_epi32(_mm256_mullo_epi32(b, _mm256_set1_epi32(29)), r128)),
			8);
		cb = _mm256_add_epi32(
			_mm256_srai_epi32(
				_mm256_add_epi32(
					_mm256_add_epi32(_mm256_mullo_epi32(r, _mm256_set1_epi32(-43)),
							 _mm256_mullo_epi32(g, _mm256_set1_epi32(-85))),
					_mm256_add_epi32(_mm256_mullo_epi32(b, _mm256_set1_epi32(128)), r128)),
				8),
			r128);
		cr = _mm256_add_epi32(
			_mm256_srai_epi32(
				_mm256_add_epi32(
					_mm256_add_epi32(_mm256_mullo_epi32(r, _mm256_set1_epi32(128)),
							 _mm256_mullo_epi32(g, _mm256_set1_epi32(-107))),
					_mm256_add_epi32(_mm256_mullo_epi32(b, _mm256_set1_epi32(-21)), r128)),
				8),
			r128);
		_mm256_storeu_si256((__m256i *)(dst + i),
				    _mm256_or_si256(op, _mm256_or_si256(
					_mm256_slli_epi32(clamp255_256(y), 16),
					_mm256_or_si256(_mm256_slli_epi32(clamp255_256(cb), 8),
							clamp255_256(cr)))));
	}
	if (i < n)
		ycc_enc_scalar(dst + i, src + i, n - i);
}

__attribute__((target("avx2")))
static void ycc_dec_avx2(uint32_t *dst, const uint32_t *src, size_t n)
{
	const __m256i op = _mm256_set1_epi32((int)0xff000000u), r128 = _mm256_set1_epi32(128);
	const __m256i m = _mm256_set1_epi32(255);
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		__m256i v = _mm256_loadu_si256((const __m256i *)(src + i));
		__m256i y = _mm256_and_si256(_mm256_srli_epi32(v, 16), m);
		__m256i cb = _mm256_sub_epi32(_mm256_and_si256(_mm256_srli_epi32(v, 8), m), r128);
		__m256i cr = _mm256_sub_epi32(_mm256_and_si256(v, m), r128);
		__m256i r = _mm256_add_epi32(y, _mm256_srai_epi32(
			_mm256_add_epi32(_mm256_mullo_epi32(cr, _mm256_set1_epi32(359)), r128), 8));
		__m256i g = _mm256_sub_epi32(y, _mm256_srai_epi32(
			_mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(cb, _mm256_set1_epi32(88)),
							  _mm256_mullo_epi32(cr, _mm256_set1_epi32(183))), r128), 8));
		__m256i b = _mm256_add_epi32(y, _mm256_srai_epi32(
			_mm256_add_epi32(_mm256_mullo_epi32(cb, _mm256_set1_epi32(454)), r128), 8));
		_mm256_storeu_si256((__m256i *)(dst + i),
				    _mm256_or_si256(op, _mm256_or_si256(
					_mm256_slli_epi32(clamp255_256(r), 16),
					_mm256_or_si256(_mm256_slli_epi32(clamp255_256(g), 8),
							clamp255_256(b)))));
	}
	if (i < n)
		ycc_dec_scalar(dst + i, src + i, n - i);
}

__attribute__((target("avx512f,evex512")))
static __m512i clamp255_512(__m512i v)
{
	return _mm512_max_epi32(_mm512_setzero_si512(),
				_mm512_min_epi32(v, _mm512_set1_epi32(255)));
}

__attribute__((target("avx512f,evex512")))
static void ycc_enc_avx512(uint32_t *dst, const uint32_t *src, size_t n)
{
	const __m512i op = _mm512_set1_epi32((int)0xff000000u), r128 = _mm512_set1_epi32(128);
	const __m512i m = _mm512_set1_epi32(255);
	size_t i = 0;
	for (; i + 16 <= n; i += 16) {
		__m512i v = _mm512_loadu_si512((const void *)(src + i));
		__m512i r = _mm512_and_si512(_mm512_srli_epi32(v, 16), m);
		__m512i g = _mm512_and_si512(_mm512_srli_epi32(v, 8), m);
		__m512i b = _mm512_and_si512(v, m);
		__m512i y = _mm512_srai_epi32(_mm512_add_epi32(
			_mm512_add_epi32(_mm512_mullo_epi32(r, _mm512_set1_epi32(77)),
					 _mm512_mullo_epi32(g, _mm512_set1_epi32(150))),
			_mm512_add_epi32(_mm512_mullo_epi32(b, _mm512_set1_epi32(29)), r128)), 8);
		__m512i cb = _mm512_add_epi32(_mm512_srai_epi32(_mm512_add_epi32(
			_mm512_add_epi32(_mm512_mullo_epi32(r, _mm512_set1_epi32(-43)),
					 _mm512_mullo_epi32(g, _mm512_set1_epi32(-85))),
			_mm512_add_epi32(_mm512_mullo_epi32(b, _mm512_set1_epi32(128)), r128)), 8), r128);
		__m512i cr = _mm512_add_epi32(_mm512_srai_epi32(_mm512_add_epi32(
			_mm512_add_epi32(_mm512_mullo_epi32(r, _mm512_set1_epi32(128)),
					 _mm512_mullo_epi32(g, _mm512_set1_epi32(-107))),
			_mm512_add_epi32(_mm512_mullo_epi32(b, _mm512_set1_epi32(-21)), r128)), 8), r128);
		_mm512_storeu_si512((void *)(dst + i),
				    _mm512_or_si512(op, _mm512_or_si512(
					_mm512_slli_epi32(clamp255_512(y), 16),
					_mm512_or_si512(_mm512_slli_epi32(clamp255_512(cb), 8),
							clamp255_512(cr)))));
	}
	if (i < n)
		ycc_enc_scalar(dst + i, src + i, n - i);
}

__attribute__((target("avx512f,evex512")))
static void ycc_dec_avx512(uint32_t *dst, const uint32_t *src, size_t n)
{
	const __m512i op = _mm512_set1_epi32((int)0xff000000u), r128 = _mm512_set1_epi32(128);
	const __m512i m = _mm512_set1_epi32(255);
	size_t i = 0;
	for (; i + 16 <= n; i += 16) {
		__m512i v = _mm512_loadu_si512((const void *)(src + i));
		__m512i y = _mm512_and_si512(_mm512_srli_epi32(v, 16), m);
		__m512i cb = _mm512_sub_epi32(_mm512_and_si512(_mm512_srli_epi32(v, 8), m), r128);
		__m512i cr = _mm512_sub_epi32(_mm512_and_si512(v, m), r128);
		__m512i r = _mm512_add_epi32(y, _mm512_srai_epi32(
			_mm512_add_epi32(_mm512_mullo_epi32(cr, _mm512_set1_epi32(359)), r128), 8));
		__m512i g = _mm512_sub_epi32(y, _mm512_srai_epi32(_mm512_add_epi32(
			_mm512_add_epi32(_mm512_mullo_epi32(cb, _mm512_set1_epi32(88)),
					 _mm512_mullo_epi32(cr, _mm512_set1_epi32(183))), r128), 8));
		__m512i b = _mm512_add_epi32(y, _mm512_srai_epi32(
			_mm512_add_epi32(_mm512_mullo_epi32(cb, _mm512_set1_epi32(454)), r128), 8));
		_mm512_storeu_si512((void *)(dst + i),
				    _mm512_or_si512(op, _mm512_or_si512(
					_mm512_slli_epi32(clamp255_512(r), 16),
					_mm512_or_si512(_mm512_slli_epi32(clamp255_512(g), 8),
							clamp255_512(b)))));
	}
	if (i < n)
		ycc_dec_scalar(dst + i, src + i, n - i);
}

/* ---------- harness -------------------------------------------------------- */

#define FW 1280
#define FH 720
#define PIX (FW * FH)  /* 921,600 px = one 720p frame; divisible by 16 for the 512 path */
#define REPS 240       /* one timed run == REPS full 720p frames */

static uint32_t rng = 0x1234567u;
static uint32_t rnd(void)
{
	rng = rng * 1664525u + 1013904223u;
	return rng;
}

static void *xalloc(size_t bytes)
{
	void *p = _mm_malloc(bytes, 64);
	if (!p) {
		fprintf(stderr, "out of memory (%zu bytes)\n", bytes);
		exit(2);
	}
	return p;
}

static double now_ms(void)
{
	LARGE_INTEGER f, t;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* Worst per-channel delta between two 8888 buffers, and count of pixels that
 * differ at all. */
static int cmp_argb(const uint32_t *a, const uint32_t *b, size_t n, int *worst)
{
	size_t i;
	int bad = 0, w = 0;
	for (i = 0; i < n; i++) {
		int ch;
		if (a[i] != b[i])
			bad++;
		for (ch = 0; ch < 4; ch++) {
			int d = (int)((a[i] >> (ch * 8)) & 255) - (int)((b[i] >> (ch * 8)) & 255);
			if (d < 0)
				d = -d;
			if (d > w)
				w = d;
		}
	}
	*worst = w;
	return bad;
}

static int cmp_f(const float *a, const float *b, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		if (a[i] != b[i]) /* both compute (v&255)/255.0f; identical bits expected */
			return 1;
	return 0;
}

/* One row of the report. Each timed call processes exactly one 720p frame, so
 * the measured per-call time is milliseconds per frame directly, and 1000/ms is
 * the frame rate the op alone could sustain. A missing path (avx512 on an older
 * cpu) comes in as 0 and prints as dashes. */
static void row(const char *name, double sc, double a2, double a5, double unused)
{
	(void)unused;
	printf("%-20s  %7.3f/%6.0f", name, sc, 1000.0 / sc);
	if (a2 > 0)
		printf("  | %7.3f/%6.0f %5.2fx", a2, 1000.0 / a2, sc / a2);
	else
		printf("  |       -/     -     -  ");
	if (a5 > 0)
		printf("  | %7.3f/%6.0f %5.2fx", a5, 1000.0 / a5, sc / a5);
	else
		printf("  |       -/     -     -  ");
	printf("\n");
}

#define TIME(dstbuf, refbuf, npix, call)                                                 \
	do {                                                                             \
		double _t0, _t1;                                                         \
		unsigned _r;                                                             \
		call; /* warm */                                                        \
		_t0 = now_ms();                                                          \
		for (_r = 0; _r < REPS; _r++) {                                          \
			call;                                                           \
		}                                                                       \
		_t1 = now_ms();                                                         \
		_ms = (_t1 - _t0) / REPS;                                                \
	} while (0)

int main(void)
{
	size_t n = PIX;
	double scale = (double)n / 1e3; /* unused by row() now; kept for the call shape */
	int fail = 0, worst;
	double _ms;

	/* buffers: two 8888 sources, one dst, plus float planes for pack/unpack */
	uint32_t *srcA = xalloc(n * 4), *srcB = xalloc(n * 4);
	uint32_t *dsc = xalloc(n * 4), *da2 = xalloc(n * 4), *da5 = xalloc(n * 4);
	float *fr = xalloc(n * 4), *fg = xalloc(n * 4), *fb = xalloc(n * 4), *fa = xalloc(n * 4);
	float *gr = xalloc(n * 4), *gg = xalloc(n * 4), *gb = xalloc(n * 4), *ga = xalloc(n * 4);
	uint16_t *s565 = xalloc(n * 2);
	unsigned char *blocks = xalloc((n / 16) * 8); /* n/16 BC1 blocks -> n texels */
	size_t i;
	uint32_t flat = 0xc080a0f0u;

	detect_cpu();

	for (i = 0; i < n; i++) {
		srcA[i] = rnd();
		srcB[i] = rnd();
		s565[i] = (uint16_t)rnd();
	}
	for (i = 0; i < n; i++) {
		fr[i] = (float)(rnd() & 0x1ffff) / 65536.0f - 0.5f; /* some out of [0,1] */
		fg[i] = (float)(rnd() & 0x1ffff) / 65536.0f - 0.5f;
		fb[i] = (float)(rnd() & 0x1ffff) / 65536.0f - 0.5f;
		fa[i] = (float)(rnd() & 0x1ffff) / 65536.0f - 0.5f;
	}
	for (i = 0; i < (n / 16) * 8; i++)
		blocks[i] = (unsigned char)rnd();

	printf("pixbench - pixel kernels, scalar vs AVX2 vs AVX512, per 720p frame\n");
	printf("cpu: AVX2=%d AVX512(F/BW/VL)=%d   frame=%dx%d (%u px)  reps=%u\n",
	       g_have_avx2, g_have_avx512, FW, FH, (unsigned)n, REPS);
	printf("columns are  ms-per-720p-frame / frames-per-sec  (and speedup vs scalar)\n\n");

	printf("%-20s  %-14s | %-14s%-6s | %-14s%-6s\n",
	       "operation", "scalar ms/fps", "avx2 ms/fps", "  x", "avx512 ms/fps", "  x");
	printf("----------------------------------------------------------------------------------\n");

	/* Every vector op is checked against scalar before it is timed; a nonzero
	 * worst delta is a correctness failure and the run ends nonzero. */

	/* pack */
	{
		double sc, a2 = 0, a5 = 0;
		pack_scalar(dsc, fr, fg, fb, fa, n);
		if (g_have_avx2) {
			pack_avx2(da2, fr, fg, fb, fa, n);
			if (cmp_argb(dsc, da2, n, &worst) || worst) {
				printf("  pack avx2 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		if (g_have_avx512) {
			pack_avx512(da5, fr, fg, fb, fa, n);
			if (cmp_argb(dsc, da5, n, &worst) || worst) {
				printf("  pack avx512 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		TIME(dsc, 0, n, pack_scalar(dsc, fr, fg, fb, fa, n));
		sc = _ms;
		if (g_have_avx2) { TIME(da2, 0, n, pack_avx2(da2, fr, fg, fb, fa, n)); a2 = _ms; }
		if (g_have_avx512) { TIME(da5, 0, n, pack_avx512(da5, fr, fg, fb, fa, n)); a5 = _ms; }
		row("pack f32->8888", sc, a2, a5, scale);
	}

	/* unpack: correctness is float-exact since all paths do (v&255)*(1/255) */
	{
		double sc, a2 = 0, a5 = 0;
		unpack_scalar(fr, fg, fb, fa, srcA, n);
		if (g_have_avx2) {
			unpack_avx2(gr, gg, gb, ga, srcA, n);
			if (cmp_f(fr, gr, n) || cmp_f(fg, gg, n) || cmp_f(fb, gb, n) ||
			    cmp_f(fa, ga, n)) {
				printf("  unpack avx2 MISMATCH\n");
				fail = 1;
			}
		}
		if (g_have_avx512) {
			unpack_avx512(gr, gg, gb, ga, srcA, n);
			if (cmp_f(fr, gr, n) || cmp_f(fg, gg, n) || cmp_f(fb, gb, n) ||
			    cmp_f(fa, ga, n)) {
				printf("  unpack avx512 MISMATCH\n");
				fail = 1;
			}
		}
		TIME(0, 0, n, unpack_scalar(fr, fg, fb, fa, srcA, n));
		sc = _ms;
		if (g_have_avx2) { TIME(0, 0, n, unpack_avx2(gr, gg, gb, ga, srcA, n)); a2 = _ms; }
		if (g_have_avx512) { TIME(0, 0, n, unpack_avx512(gr, gg, gb, ga, srcA, n)); a5 = _ms; }
		row("unpack 8888->f32", sc, a2, a5, scale);
	}

	/* decode 565 */
	{
		double sc, a2 = 0, a5 = 0;
		dec565_scalar(dsc, s565, n);
		if (g_have_avx2) {
			dec565_avx2(da2, s565, n);
			if (cmp_argb(dsc, da2, n, &worst) || worst) {
				printf("  dec565 avx2 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		if (g_have_avx512) {
			dec565_avx512(da5, s565, n);
			if (cmp_argb(dsc, da5, n, &worst) || worst) {
				printf("  dec565 avx512 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		TIME(0, 0, n, dec565_scalar(dsc, s565, n));
		sc = _ms;
		if (g_have_avx2) { TIME(0, 0, n, dec565_avx2(da2, s565, n)); a2 = _ms; }
		if (g_have_avx512) { TIME(0, 0, n, dec565_avx512(da5, s565, n)); a5 = _ms; }
		row("decode B5G6R5", sc, a2, a5, scale);
	}

	/* dxt1 */
	{
		double sc, a2 = 0, a5 = 0;
		size_t nb = n / 16;
		dxt1_scalar(dsc, blocks, nb);
		if (g_have_avx2) {
			dxt1_avx2(da2, blocks, nb);
			if (cmp_argb(dsc, da2, n, &worst) || worst) {
				printf("  dxt1 avx2 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		if (g_have_avx512) {
			dxt1_avx512(da5, blocks, nb);
			if (cmp_argb(dsc, da5, n, &worst) || worst) {
				printf("  dxt1 avx512 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		TIME(0, 0, n, dxt1_scalar(dsc, blocks, nb));
		sc = _ms;
		if (g_have_avx2) { TIME(0, 0, n, dxt1_avx2(da2, blocks, nb)); a2 = _ms; }
		if (g_have_avx512) { TIME(0, 0, n, dxt1_avx512(da5, blocks, nb)); a5 = _ms; }
		row("decode BC1 (texels)", sc, a2, a5, scale);
	}

	/* swizzle */
	{
		double sc, a2 = 0, a5 = 0;
		swizzle_scalar(dsc, srcA, n);
		if (g_have_avx2) {
			swizzle_avx2(da2, srcA, n);
			if (cmp_argb(dsc, da2, n, &worst) || worst) {
				printf("  swizzle avx2 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		if (g_have_avx512) {
			swizzle_avx512(da5, srcA, n);
			if (cmp_argb(dsc, da5, n, &worst) || worst) {
				printf("  swizzle avx512 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		TIME(0, 0, n, swizzle_scalar(dsc, srcA, n));
		sc = _ms;
		if (g_have_avx2) { TIME(0, 0, n, swizzle_avx2(da2, srcA, n)); a2 = _ms; }
		if (g_have_avx512) { TIME(0, 0, n, swizzle_avx512(da5, srcA, n)); a5 = _ms; }
		row("swizzle R<->B", sc, a2, a5, scale);
	}

	/* add */
	{
		double sc, a2 = 0, a5 = 0;
		add_scalar(dsc, srcA, srcB, n);
		if (g_have_avx2) {
			add_avx2(da2, srcA, srcB, n);
			if (cmp_argb(dsc, da2, n, &worst) || worst) {
				printf("  add avx2 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		if (g_have_avx512) {
			add_avx512(da5, srcA, srcB, n);
			if (cmp_argb(dsc, da5, n, &worst) || worst) {
				printf("  add avx512 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		TIME(0, 0, n, add_scalar(dsc, srcA, srcB, n));
		sc = _ms;
		if (g_have_avx2) { TIME(0, 0, n, add_avx2(da2, srcA, srcB, n)); a2 = _ms; }
		if (g_have_avx512) { TIME(0, 0, n, add_avx512(da5, srcA, srcB, n)); a5 = _ms; }
		row("color add (sat)", sc, a2, a5, scale);
	}

	/* sub */
	{
		double sc, a2 = 0, a5 = 0;
		sub_scalar(dsc, srcA, srcB, n);
		if (g_have_avx2) {
			sub_avx2(da2, srcA, srcB, n);
			if (cmp_argb(dsc, da2, n, &worst) || worst) {
				printf("  sub avx2 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		if (g_have_avx512) {
			sub_avx512(da5, srcA, srcB, n);
			if (cmp_argb(dsc, da5, n, &worst) || worst) {
				printf("  sub avx512 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		TIME(0, 0, n, sub_scalar(dsc, srcA, srcB, n));
		sc = _ms;
		if (g_have_avx2) { TIME(0, 0, n, sub_avx2(da2, srcA, srcB, n)); a2 = _ms; }
		if (g_have_avx512) { TIME(0, 0, n, sub_avx512(da5, srcA, srcB, n)); a5 = _ms; }
		row("color sub (sat)", sc, a2, a5, scale);
	}

	/* over */
	{
		double sc, a2 = 0, a5 = 0;
		over_scalar(dsc, srcA, srcB, n);
		if (g_have_avx2) {
			over_avx2(da2, srcA, srcB, n);
			if (cmp_argb(dsc, da2, n, &worst) || worst) {
				printf("  over avx2 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		if (g_have_avx512) {
			over_avx512(da5, srcA, srcB, n);
			if (cmp_argb(dsc, da5, n, &worst) || worst) {
				printf("  over avx512 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		TIME(0, 0, n, over_scalar(dsc, srcA, srcB, n));
		sc = _ms;
		if (g_have_avx2) { TIME(0, 0, n, over_avx2(da2, srcA, srcB, n)); a2 = _ms; }
		if (g_have_avx512) { TIME(0, 0, n, over_avx512(da5, srcA, srcB, n)); a5 = _ms; }
		row("mix / src-over", sc, a2, a5, scale);
	}

	/* modulate */
	{
		double sc, a2 = 0, a5 = 0;
		mod_scalar(dsc, srcA, flat, n);
		if (g_have_avx2) {
			mod_avx2(da2, srcA, flat, n);
			if (cmp_argb(dsc, da2, n, &worst) || worst) {
				printf("  modulate avx2 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		if (g_have_avx512) {
			mod_avx512(da5, srcA, flat, n);
			if (cmp_argb(dsc, da5, n, &worst) || worst) {
				printf("  modulate avx512 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		TIME(0, 0, n, mod_scalar(dsc, srcA, flat, n));
		sc = _ms;
		if (g_have_avx2) { TIME(0, 0, n, mod_avx2(da2, srcA, flat, n)); a2 = _ms; }
		if (g_have_avx512) { TIME(0, 0, n, mod_avx512(da5, srcA, flat, n)); a5 = _ms; }
		row("modulate (div255)", sc, a2, a5, scale);
	}

	/* YCbCr encode (RGB -> luma + blue/yellow + red/green) */
	{
		double sc, a2 = 0, a5 = 0;
		ycc_enc_scalar(dsc, srcA, n);
		if (g_have_avx2) {
			ycc_enc_avx2(da2, srcA, n);
			if (cmp_argb(dsc, da2, n, &worst) || worst) {
				printf("  ycc-enc avx2 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		if (g_have_avx512) {
			ycc_enc_avx512(da5, srcA, n);
			if (cmp_argb(dsc, da5, n, &worst) || worst) {
				printf("  ycc-enc avx512 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		TIME(0, 0, n, ycc_enc_scalar(dsc, srcA, n));
		sc = _ms;
		if (g_have_avx2) { TIME(0, 0, n, ycc_enc_avx2(da2, srcA, n)); a2 = _ms; }
		if (g_have_avx512) { TIME(0, 0, n, ycc_enc_avx512(da5, srcA, n)); a5 = _ms; }
		row("baseband RGB->YCbCr", sc, a2, a5, scale);
	}

	/* YCbCr decode (baseband -> RGB) - the video-frame color path */
	{
		double sc, a2 = 0, a5 = 0;
		ycc_enc_scalar(srcB, srcA, n); /* feed the decode a valid YCbCr frame */
		ycc_dec_scalar(dsc, srcB, n);
		if (g_have_avx2) {
			ycc_dec_avx2(da2, srcB, n);
			if (cmp_argb(dsc, da2, n, &worst) || worst) {
				printf("  ycc-dec avx2 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		if (g_have_avx512) {
			ycc_dec_avx512(da5, srcB, n);
			if (cmp_argb(dsc, da5, n, &worst) || worst) {
				printf("  ycc-dec avx512 MISMATCH worst=%d\n", worst);
				fail = 1;
			}
		}
		TIME(0, 0, n, ycc_dec_scalar(dsc, srcB, n));
		sc = _ms;
		if (g_have_avx2) { TIME(0, 0, n, ycc_dec_avx2(da2, srcB, n)); a2 = _ms; }
		if (g_have_avx512) { TIME(0, 0, n, ycc_dec_avx512(da5, srcB, n)); a5 = _ms; }
		row("baseband YCbCr->RGB", sc, a2, a5, scale);
	}

	printf("\n%s\n", fail ? "FAIL (a vector path diverged from scalar)"
			       : "PASS (every vector path is bit-exact with scalar)");

	_mm_free(srcA); _mm_free(srcB); _mm_free(dsc); _mm_free(da2); _mm_free(da5);
	_mm_free(fr); _mm_free(fg); _mm_free(fb); _mm_free(fa);
	_mm_free(gr); _mm_free(gg); _mm_free(gb); _mm_free(ga);
	_mm_free(s565); _mm_free(blocks);
	return fail ? 1 : 0;
}
