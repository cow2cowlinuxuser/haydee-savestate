/* pixtune - does hand tuning buy anything past the straight AVX512 kernels in
 * pixbench.c, on this machine, measured rather than assumed.
 *
 * pixbench.c answered "scalar vs AVX2 vs AVX512". This asks the follow-up: given
 * the AVX512 form, do the usual hand levers move it? Three are tested, each
 * against its own baseline and each checked bit-exact so a faster wrong answer
 * is caught:
 *
 *  1. non-temporal stores  - _mm512_stream on the write-once streams (swizzle,
 *     saturating add). Helps only if the op is DRAM-bound; if the buffers sit in
 *     L3 the streaming store bypasses the cache it wanted and can lose. The
 *     point is to find out which regime we are in, not to assume.
 *
 *  2. two accumulators     - source-over and modulate carry a multiply ->
 *     add -> shift -> shift div255 chain per lane group. Issuing two independent
 *     16-wide chains per iteration gives the out-of-order engine something to
 *     overlap if the op is latency-bound rather than bandwidth-bound.
 *
 *  3. fusion               - the real lever. A frame does decode -> modulate ->
 *     blend in sequence; doing them as three passes over three buffers moves the
 *     data across the bus three times, doing them in one loop moves it once.
 *     swrast.c's span kernel already fuses sample+modulate+blend+store; this
 *     measures what that fusion is worth so the decodes can be folded in too.
 *
 * Build (native x64), same shape as pixbench:
 *   zig cc -O2 -ffp-contract=off -target x86_64-windows-gnu -o pixtune.exe pixtune.c
 */
#include <windows.h>
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FW 1280
#define FH 720
#define PIX (FW * FH)
#define REPS 400

static int g_have_avx512;

static void cpuid_count(unsigned leaf, unsigned sub, unsigned r[4])
{
	__asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
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
	int zmm;
	cpuid_count(1, 0, r1);
	if (!((r1[2] >> 27) & 1))
		return;
	xcr0 = xgetbv0();
	zmm = (xcr0 & 0x6) == 0x6 && (xcr0 & 0xe0) == 0xe0;
	cpuid_count(7, 0, r7);
	g_have_avx512 = zmm && ((r7[1] >> 16) & 1) && ((r7[1] >> 30) & 1) && ((r7[1] >> 31) & 1);
}

static uint32_t div255s(unsigned v) { v += 128; return (v + (v >> 8)) >> 8; }

/* ---------------- scalar references (for the checks) ---------------- */

static void add_ref(uint32_t *d, const uint32_t *a, const uint32_t *b, size_t n)
{
	size_t i; int c;
	for (i = 0; i < n; i++) {
		uint32_t o = 0;
		for (c = 0; c < 4; c++) {
			int s = (int)((a[i] >> (c * 8)) & 255) + (int)((b[i] >> (c * 8)) & 255);
			o |= (uint32_t)(s > 255 ? 255 : s) << (c * 8);
		}
		d[i] = o;
	}
}
static void swz_ref(uint32_t *d, const uint32_t *s, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		uint32_t v = s[i];
		d[i] = (v & 0xff00ff00u) | ((v & 0xff0000u) >> 16) | ((v & 0xffu) << 16);
	}
}
static void over_ref(uint32_t *d, const uint32_t *s, const uint32_t *bg, size_t n)
{
	size_t i; int c;
	for (i = 0; i < n; i++) {
		unsigned sa = (s[i] >> 24) & 255, isa = 255 - sa; uint32_t o = 0;
		for (c = 0; c < 4; c++) {
			unsigned sc = (s[i] >> (c * 8)) & 255, dc = (bg[i] >> (c * 8)) & 255;
			o |= div255s(sc * sa + dc * isa) << (c * 8);
		}
		d[i] = o;
	}
}
/* the fused reference: decode a B5G6R5 texel, modulate by flat, over onto bg */
static void pipe_ref(uint32_t *d, const uint16_t *t, uint32_t flat, const uint32_t *bg, size_t n)
{
	size_t i; int c;
	for (i = 0; i < n; i++) {
		unsigned v = t[i], r5 = (v >> 11) & 31, g6 = (v >> 5) & 63, b5 = v & 31;
		uint32_t dec = 0xff000000u | (((r5 << 3 | r5 >> 2)) << 16) |
			       (((g6 << 2 | g6 >> 4)) << 8) | (b5 << 3 | b5 >> 2);
		uint32_t mod = 0; unsigned sa, isa; uint32_t o = 0;
		for (c = 0; c < 4; c++) {
			unsigned cc = (dec >> (c * 8)) & 255, ff = (flat >> (c * 8)) & 255;
			mod |= div255s(cc * ff) << (c * 8);
		}
		sa = (mod >> 24) & 255; isa = 255 - sa;
		for (c = 0; c < 4; c++) {
			unsigned sc = (mod >> (c * 8)) & 255, dc = (bg[i] >> (c * 8)) & 255;
			o |= div255s(sc * sa + dc * isa) << (c * 8);
		}
		d[i] = o;
	}
}

/* ---------------- AVX512 building blocks ---------------- */

__attribute__((target("avx512bw,evex512")))
static __m512i over512(__m512i s, __m512i d)
{
	const __m512i lomask = _mm512_set1_epi32(0x00ff00ff);
	const __m512i c255 = _mm512_set1_epi16(255), c128 = _mm512_set1_epi16(128);
	__m512i sa = _mm512_srli_epi32(s, 24);
	__m512i saw = _mm512_or_si512(sa, _mm512_slli_epi32(sa, 16));
	__m512i isaw = _mm512_sub_epi16(c255, saw);
	__m512i srb = _mm512_and_si512(s, lomask);
	__m512i sag = _mm512_and_si512(_mm512_srli_epi32(s, 8), lomask);
	__m512i drb = _mm512_and_si512(d, lomask);
	__m512i dag = _mm512_and_si512(_mm512_srli_epi32(d, 8), lomask);
	__m512i rb = _mm512_add_epi16(_mm512_add_epi16(_mm512_mullo_epi16(srb, saw),
						       _mm512_mullo_epi16(drb, isaw)), c128);
	__m512i ag = _mm512_add_epi16(_mm512_add_epi16(_mm512_mullo_epi16(sag, saw),
						       _mm512_mullo_epi16(dag, isaw)), c128);
	rb = _mm512_srli_epi16(_mm512_add_epi16(rb, _mm512_srli_epi16(rb, 8)), 8);
	ag = _mm512_srli_epi16(_mm512_add_epi16(ag, _mm512_srli_epi16(ag, 8)), 8);
	return _mm512_or_si512(_mm512_and_si512(rb, lomask),
			       _mm512_slli_epi32(_mm512_and_si512(ag, lomask), 8));
}

__attribute__((target("avx512bw,evex512")))
static __m512i mod512(__m512i col, __m512i frb, __m512i fag)
{
	const __m512i lomask = _mm512_set1_epi32(0x00ff00ff);
	const __m512i c128 = _mm512_set1_epi16(128);
	__m512i crb = _mm512_and_si512(col, lomask);
	__m512i cag = _mm512_and_si512(_mm512_srli_epi32(col, 8), lomask);
	__m512i rb = _mm512_add_epi16(_mm512_mullo_epi16(crb, frb), c128);
	__m512i ag = _mm512_add_epi16(_mm512_mullo_epi16(cag, fag), c128);
	rb = _mm512_srli_epi16(_mm512_add_epi16(rb, _mm512_srli_epi16(rb, 8)), 8);
	ag = _mm512_srli_epi16(_mm512_add_epi16(ag, _mm512_srli_epi16(ag, 8)), 8);
	return _mm512_or_si512(_mm512_and_si512(rb, lomask),
			       _mm512_slli_epi32(_mm512_and_si512(ag, lomask), 8));
}

/* decode 16 B5G6R5 texels (a 256-bit load of u16) to a 512-bit RGBA vector */
__attribute__((target("avx512bw,evex512")))
static __m512i dec565_512(const uint16_t *src)
{
	const __m512i r5m = _mm512_set1_epi32(31), g6m = _mm512_set1_epi32(63);
	const __m512i op = _mm512_set1_epi32((int)0xff000000u);
	__m512i v = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)src));
	__m512i r5 = _mm512_and_si512(_mm512_srli_epi32(v, 11), r5m);
	__m512i g6 = _mm512_and_si512(_mm512_srli_epi32(v, 5), g6m);
	__m512i b5 = _mm512_and_si512(v, r5m);
	__m512i r8 = _mm512_or_si512(_mm512_slli_epi32(r5, 3), _mm512_srli_epi32(r5, 2));
	__m512i g8 = _mm512_or_si512(_mm512_slli_epi32(g6, 2), _mm512_srli_epi32(g6, 4));
	__m512i b8 = _mm512_or_si512(_mm512_slli_epi32(b5, 3), _mm512_srli_epi32(b5, 2));
	return _mm512_or_si512(_mm512_or_si512(op, _mm512_slli_epi32(r8, 16)),
			       _mm512_or_si512(_mm512_slli_epi32(g8, 8), b8));
}

/* ---------------- the variants under test ---------------- */

/* add: baseline masked store vs streaming store */
__attribute__((target("avx512bw,evex512")))
static void add_base(uint32_t *d, const uint32_t *a, const uint32_t *b, size_t n)
{
	size_t i;
	for (i = 0; i + 16 <= n; i += 16)
		_mm512_storeu_si512((void *)(d + i),
				    _mm512_adds_epu8(_mm512_loadu_si512((const void *)(a + i)),
						     _mm512_loadu_si512((const void *)(b + i))));
}
__attribute__((target("avx512bw,evex512")))
static void add_nt(uint32_t *d, const uint32_t *a, const uint32_t *b, size_t n)
{
	size_t i;
	for (i = 0; i + 16 <= n; i += 16)
		_mm512_stream_si512((void *)(d + i),
				    _mm512_adds_epu8(_mm512_loadu_si512((const void *)(a + i)),
						     _mm512_loadu_si512((const void *)(b + i))));
	_mm_sfence();
}

/* swizzle: baseline vs streaming store */
__attribute__((target("avx512bw,evex512")))
static void swz_base(uint32_t *d, const uint32_t *s, size_t n)
{
	const __m512i ctl = _mm512_broadcast_i32x4(_mm_setr_epi8(
		2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15));
	size_t i;
	for (i = 0; i + 16 <= n; i += 16)
		_mm512_storeu_si512((void *)(d + i),
				    _mm512_shuffle_epi8(_mm512_loadu_si512((const void *)(s + i)), ctl));
}
__attribute__((target("avx512bw,evex512")))
static void swz_nt(uint32_t *d, const uint32_t *s, size_t n)
{
	const __m512i ctl = _mm512_broadcast_i32x4(_mm_setr_epi8(
		2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15));
	size_t i;
	for (i = 0; i + 16 <= n; i += 16)
		_mm512_stream_si512((void *)(d + i),
				    _mm512_shuffle_epi8(_mm512_loadu_si512((const void *)(s + i)), ctl));
	_mm_sfence();
}

/* over: baseline 16-wide vs two independent 16-wide chains per iteration */
__attribute__((target("avx512bw,evex512")))
static void over_base(uint32_t *d, const uint32_t *s, const uint32_t *bg, size_t n)
{
	size_t i;
	for (i = 0; i + 16 <= n; i += 16)
		_mm512_storeu_si512((void *)(d + i),
				    over512(_mm512_loadu_si512((const void *)(s + i)),
					    _mm512_loadu_si512((const void *)(bg + i))));
}
__attribute__((target("avx512bw,evex512")))
static void over_u2(uint32_t *d, const uint32_t *s, const uint32_t *bg, size_t n)
{
	size_t i = 0;
	for (; i + 32 <= n; i += 32) {
		__m512i r0 = over512(_mm512_loadu_si512((const void *)(s + i)),
				     _mm512_loadu_si512((const void *)(bg + i)));
		__m512i r1 = over512(_mm512_loadu_si512((const void *)(s + i + 16)),
				     _mm512_loadu_si512((const void *)(bg + i + 16)));
		_mm512_storeu_si512((void *)(d + i), r0);
		_mm512_storeu_si512((void *)(d + i + 16), r1);
	}
	for (; i + 16 <= n; i += 16)
		_mm512_storeu_si512((void *)(d + i),
				    over512(_mm512_loadu_si512((const void *)(s + i)),
					    _mm512_loadu_si512((const void *)(bg + i))));
}

/* the pipeline: decode565 -> modulate(flat) -> over(bg), as three passes over
 * two scratch buffers, vs one fused loop that never lands the intermediates. */
__attribute__((target("avx512bw,evex512")))
static void pipe_3pass(uint32_t *d, const uint16_t *t, uint32_t flat, const uint32_t *bg,
		       size_t n, uint32_t *tmp0, uint32_t *tmp1)
{
	const __m512i lomask = _mm512_set1_epi32(0x00ff00ff);
	__m512i f = _mm512_set1_epi32((int)flat);
	__m512i frb = _mm512_and_si512(f, lomask), fag = _mm512_and_si512(_mm512_srli_epi32(f, 8), lomask);
	size_t i;
	for (i = 0; i + 16 <= n; i += 16) /* pass 1: decode -> tmp0 */
		_mm512_storeu_si512((void *)(tmp0 + i), dec565_512(t + i));
	for (i = 0; i + 16 <= n; i += 16) /* pass 2: modulate tmp0 -> tmp1 */
		_mm512_storeu_si512((void *)(tmp1 + i),
				    mod512(_mm512_loadu_si512((const void *)(tmp0 + i)), frb, fag));
	for (i = 0; i + 16 <= n; i += 16) /* pass 3: over tmp1 onto bg -> d */
		_mm512_storeu_si512((void *)(d + i),
				    over512(_mm512_loadu_si512((const void *)(tmp1 + i)),
					    _mm512_loadu_si512((const void *)(bg + i))));
}
__attribute__((target("avx512bw,evex512")))
static void pipe_fused(uint32_t *d, const uint16_t *t, uint32_t flat, const uint32_t *bg, size_t n)
{
	const __m512i lomask = _mm512_set1_epi32(0x00ff00ff);
	__m512i f = _mm512_set1_epi32((int)flat);
	__m512i frb = _mm512_and_si512(f, lomask), fag = _mm512_and_si512(_mm512_srli_epi32(f, 8), lomask);
	size_t i;
	for (i = 0; i + 16 <= n; i += 16) {
		__m512i dec = dec565_512(t + i);
		__m512i mod = mod512(dec, frb, fag);
		__m512i out = over512(mod, _mm512_loadu_si512((const void *)(bg + i)));
		_mm512_storeu_si512((void *)(d + i), out);
	}
}

/* ---------------- harness ---------------- */

static double now_ms(void)
{
	LARGE_INTEGER f, t;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static void *xa(size_t b)
{
	void *p = _mm_malloc(b, 64);
	if (!p) { fprintf(stderr, "oom\n"); exit(2); }
	return p;
}
static int diff(const uint32_t *a, const uint32_t *b, size_t n)
{
	size_t i; int bad = 0;
	for (i = 0; i < n; i++) if (a[i] != b[i]) bad++;
	return bad;
}

int main(void)
{
	size_t n = PIX;
	uint32_t *A = xa(n * 4), *B = xa(n * 4), *ref = xa(n * 4), *out = xa(n * 4);
	uint32_t *t0 = xa(n * 4), *t1 = xa(n * 4);
	uint16_t *tex = xa(n * 2);
	uint32_t flat = 0xc0a08040u, rng = 0x13579bdfu;
	size_t i; int r; double a, b0, b1;

	detect_cpu();
	if (!g_have_avx512) {
		printf("pixtune: no AVX512 on this machine; nothing to tune here\n");
		return 0;
	}
	for (i = 0; i < n; i++) {
		rng = rng * 1664525u + 1013904223u; A[i] = rng;
		rng = rng * 1664525u + 1013904223u; B[i] = rng;
		tex[i] = (uint16_t)(rng >> 8);
	}

	printf("pixtune - hand-tuning levers vs the straight AVX512 kernel, per 720p frame\n");
	printf("frame=%dx%d (%u px)  reps=%u   ms/frame (frames/sec), and delta vs baseline\n\n",
	       FW, FH, (unsigned)n, REPS);

#define BENCH(fn, ms) do { fn; a = now_ms(); for (r = 0; r < REPS; r++) { fn; } ms = (now_ms() - a) / REPS; } while (0)
#define LINE(name, base, tuned) \
	printf("%-26s  base %6.3f (%6.0f)   tuned %6.3f (%6.0f)   %+5.1f%%\n", name, \
	       base, 1000.0 / base, tuned, 1000.0 / tuned, (base / tuned - 1.0) * 100.0)

	/* add: baseline vs non-temporal store */
	add_ref(ref, A, B, n);
	add_base(out, A, B, n);
	printf(diff(ref, out, n) ? "  add MISMATCH\n" : "");
	add_nt(out, A, B, n);
	printf(diff(ref, out, n) ? "  add-nt MISMATCH\n" : "");
	BENCH(add_base(out, A, B, n), b0);
	BENCH(add_nt(out, A, B, n), b1);
	LINE("color add: stream store", b0, b1);

	/* swizzle: baseline vs non-temporal store */
	swz_ref(ref, A, n);
	swz_base(out, A, n);
	printf(diff(ref, out, n) ? "  swz MISMATCH\n" : "");
	swz_nt(out, A, n);
	printf(diff(ref, out, n) ? "  swz-nt MISMATCH\n" : "");
	BENCH(swz_base(out, A, n), b0);
	BENCH(swz_nt(out, A, n), b1);
	LINE("swizzle: stream store", b0, b1);

	/* over: baseline vs two accumulators */
	over_ref(ref, A, B, n);
	over_base(out, A, B, n);
	printf(diff(ref, out, n) ? "  over MISMATCH\n" : "");
	over_u2(out, A, B, n);
	printf(diff(ref, out, n) ? "  over-u2 MISMATCH\n" : "");
	BENCH(over_base(out, A, B, n), b0);
	BENCH(over_u2(out, A, B, n), b1);
	LINE("src-over: 2 accumulators", b0, b1);

	/* pipeline: three passes vs fused one pass */
	pipe_ref(ref, tex, flat, B, n);
	pipe_3pass(out, tex, flat, B, n, t0, t1);
	printf(diff(ref, out, n) ? "  pipe-3pass MISMATCH\n" : "");
	pipe_fused(out, tex, flat, B, n);
	printf(diff(ref, out, n) ? "  pipe-fused MISMATCH\n" : "");
	BENCH(pipe_3pass(out, tex, flat, B, n, t0, t1), b0);
	BENCH(pipe_fused(out, tex, flat, B, n), b1);
	LINE("decode+mod+over: fuse 3->1", b0, b1);

	printf("\nreading it: a positive %% means the tuned form is that much faster. A stream\n"
	       "store that loses means the buffers were cache-resident and bypassing the cache\n"
	       "cost more than it saved; fusion should win by the passes it removes from the bus.\n");
	_mm_free(A); _mm_free(B); _mm_free(ref); _mm_free(out);
	_mm_free(t0); _mm_free(t1); _mm_free(tex);
	return 0;
}
