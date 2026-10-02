#define CINTERFACE
#define COBJMACROS
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d9types.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dxbc.h"
#include "savestate.h"
#include "swalloc.h"
#include "swrast.h"
#include "gpuhost.h"
#include "vtbl_arity.h"

#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

/* Software D3D11 + DXGI so a Unity player can rewind: every texture, buffer
 * and target is process memory, Present is the save/load seam. 1 fps is a
 * successful boot, not a failure. */

typedef struct SwPriv {
	GUID guid;
	UINT size;
	unsigned char *data;
	struct SwPriv *next;
} SwPriv;

typedef struct Sw11Device Sw11Device;
typedef struct Sw11Context Sw11Context;
typedef struct Sw11Factory Sw11Factory;
typedef struct Sw11Adapter Sw11Adapter;
typedef struct Sw11Output Sw11Output;
typedef struct Sw11Swap Sw11Swap;
typedef struct Sw11Res Sw11Res;
typedef struct Sw11View Sw11View;
typedef struct Sw11Shader Sw11Shader;
typedef struct Sw11Layout Sw11Layout;
typedef struct Sw11Blend Sw11Blend;
typedef struct Sw11DS Sw11DS;
typedef struct Sw11Rast Sw11Rast;
typedef struct Sw11Samp Sw11Samp;
typedef struct Sw11Query Sw11Query;

struct Sw11Factory {
	IDXGIFactory2 iface;
	LONG ref;
	HWND assoc;
	UINT assoc_flags;
	SwPriv *priv;
};

struct Sw11Adapter {
	IDXGIAdapter1 iface;
	LONG ref;
	Sw11Factory *factory;
};

struct Sw11Output {
	IDXGIOutput iface;
	LONG ref;
	Sw11Adapter *adapter;
	UINT index; /* into the monitor table below */
};

struct Sw11Device {
	ID3D11Device1 iface;
	IDXGIDevice1 dxgi;
	LONG ref;
	UINT flags;
	D3D_FEATURE_LEVEL level;
	Sw11Factory *factory;
	Sw11Adapter *adapter;
	Sw11Context *imm;
	CRITICAL_SECTION lock;
	SwPriv *priv;
	UINT latency;
};

/* What a texture's colours look like, taken under D3D11SW_PAINTSTAT once per
 * content change. Alpha 0 counts as one colour whatever its RGB, because it
 * paints nothing either way. A block is 4x4 texels; a solid block is one colour
 * throughout. Only a solid block whose left neighbour block is a different
 * colour (or not solid) starts a run: the rest continue the one beside them,
 * which is what a run-length fill would get for free. Texel runs are the same
 * question asked per texel along each row. */
typedef struct TexPal {
	unsigned gen; /* gpu_gen + 1 it describes; 0 never taken */
	unsigned at;  /* present count when taken */
	unsigned colors; /* distinct colours, 257 meaning more than 256 */
	unsigned blocks, clear, solid, white, black, runs;
	unsigned texels, texel_runs;
} TexPal;

struct Sw11Res {
	union {
		ID3D11Texture2D tex;
		ID3D11Buffer buf;
	} iface;
	LONG ref;
	Sw11Device *dev;
	SwPriv *priv;
	int kind; /* 0 buffer, 1 tex2d */
	DXGI_FORMAT format;
	UINT width, height, bind, usage, cpu_access;
	UINT byte_width;
	UINT row_pitch;
	unsigned char *cpu;
	UINT cpu_size;
	uint32_t *pixels;
	/* pixels points at cpu rather than at its own allocation, because keeping
	 * the same image twice is what exhausts a 32-bit process. Not owned, so
	 * it must not be freed. */
	/* The adapter's copy of this texture, and the content generation it was
	 * uploaded at. Opaque here on purpose: the backend owns the handle and
	 * this file only has to remember where it lives and when it went stale. */
	void *gtex;
	unsigned gpu_gen;
	/* Its bytes live on the adapter now and the CPU planes were let go. Not the
	 * same as a texture that never had any: a map for write rebuilds them. */
	int gpu_released;
	/* Written by the game after we moved it once, so it stays here. */
	int gpu_no_release;
	int pixels_alias;
	/* An aliased plane whose format needed a channel swap, so the shared
	 * bytes were converted in place and are no longer in the format the game
	 * supplied. Only ever set for resources the game cannot map. */
	int pixels_swizzled;
	/* cpu has been written since the last decode. Without this, converting in
	 * place would run a second time on unchanged bytes and swap the channels
	 * back - CreateSRV in particular decodes on every call. */
	int cpu_dirty;
	/* Every resource currently holding memory, so a census can say where the
	 * process went rather than only how much of it is gone. */
	Sw11Res *live_next, *live_prev;
	int listed;
	float *depth;
	int is_bb;
	int has_texels;
	int warned_empty;
	/* Capped by the resident-budget sweep (D3D11SW_RESIDENT_CAP_MB): the full
	 * description is kept but no pixels are allocated, so it costs zero resident
	 * bytes, renders as a magenta placeholder, and never crashes. This is exactly
	 * the faucet's evicted state - a header whose content is not resident. */
	int phantom;
	/* Faucet (D3D11SW_FAUCET): the authoritative copy of this texture's content,
	 * on the CRT heap (a disk file later). While the texture is resident its
	 * pixels live in the arena; evicting frees the arena copy and leaves the bank,
	 * so it can be painted back into a fresh arena slot when next drawn. The game's
	 * handle never changes - only the payload behind it moves. */
	unsigned char *bank;
	UINT bank_size;
	unsigned lru;        /* present count at last use, for least-recently-used evict */
	unsigned evicted_at; /* present count at eviction, to tell thrash from churn */
	int id;
	/* Retired rather than freed, so the game can still name it. Distinct from a
	 * NULL pixels pointer, which also means an ordinary untextured draw. */
	int retired;
	/* Retired objects whose PAYLOAD was kept as well as their header, chained so
	 * the pixels can be released once no savestate can ask for them again. */
	Sw11Res *ret_next;
	size_t ret_bytes;
	/* Already linked into the retain list, so a second retirement does not
	 * link it twice. */
	int ret_listed;
	/* Which save this object was born after, so a release can tell whether any
	 * snapshot could still name it. The ledger already draws this line for raw
	 * payloads; this draws it for the objects that own them. */
	LONG born_gen;
	/* The same question asked of the PLANES rather than the object, because
	 * they are not allocated together. res_ensure_pixels can hand a texture a
	 * pixels or depth plane long after it was created, from CreateSRV or a
	 * copy. An object born before the save with a plane allocated after it has
	 * born_gen below the current generation, so it was retained, while the new
	 * plane's ledger entry sat above the mark, so the reap freed it - the
	 * retain list then pointed at released address space. Both halves have to
	 * predate the save for the object to be worth keeping. */
	LONG plane_gen;
	/* The size the GAME asked for, when render-target scaling made the surface
	 * we actually rasterise into smaller than that. Zero when nothing was
	 * scaled, so virt_w != 0 is also the test for "this resource is scaled".
	 *
	 * Kept because the game must go on seeing the size it requested - it sizes
	 * its viewports, its UI layout and its own copies from that number - while
	 * every pixel address inside here is in the smaller real space. Everything
	 * that crosses between the two has to be converted, and the conversions are
	 * exactly rt_scale_x/y below plus the copy paths. */
	UINT virt_w, virt_h;
	TexPal pal;
};

/* Per-frame accounting of which render target actually received geometry.
 * Unity renders through several same-sized offscreen targets, so dimensions
 * alone cannot tell the backbuffer apart from an orphaned intermediate. */
#define FRAME_RT_MAX 16
typedef struct FrameRt {
	Sw11Res *res;
	UINT tris;
	UINT draws;
} FrameRt;

static FrameRt g_frame_rt[FRAME_RT_MAX];
static int g_frame_nrt;
static UINT g_bb_writes;
static LONG g_res_seq;
static LONG g_cb_maps;
static LONG g_buf_maps;

/* F9 logs every draw of the next frame: where it lands, what it samples, how
 * it blends, and the screen rectangle it covers.
 *
 * Four theories about the stray rectangles have now died (render-target
 * scaling, unfilled storage, stencil masking, dropped geometry), every one of
 * them killed by a test that could only say "no". This says "yes": the boxes
 * have a position on screen, so whichever draw covers that position is the one
 * putting them there, and its target and texture are then facts rather than
 * candidates. Armed for exactly one frame because a full frame of Rabi-Ribi is
 * already hundreds of lines. */
static void d11_log(const char *fmt, ...);

static volatile LONG g_census_arm;
static int g_census_on;
static unsigned g_census_seq;

static void census_draw(const Sw11Res *rt, const SwTri *tris, int ntri, const Sw11Res *src,
			const SwState *st)
{
	float x0, y0, x1, y1;
	float u0, v0, u1, v1;
	int i;

	if (!g_census_on || !tris || ntri <= 0)
		return;
	x0 = y0 = u0 = v0 = 1e9f;
	x1 = y1 = u1 = v1 = -1e9f;
	for (i = 0; i < ntri; i++) {
		const SwVert *v[3];
		int k;

		v[0] = &tris[i].a;
		v[1] = &tris[i].b;
		v[2] = &tris[i].c;
		for (k = 0; k < 3; k++) {
			if (v[k]->x < x0)
				x0 = v[k]->x;
			if (v[k]->x > x1)
				x1 = v[k]->x;
			if (v[k]->y < y0)
				y0 = v[k]->y;
			if (v[k]->y > y1)
				y1 = v[k]->y;
			if (v[k]->u < u0)
				u0 = v[k]->u;
			if (v[k]->u > u1)
				u1 = v[k]->u;
			if (v[k]->v < v0)
				v0 = v[k]->v;
			if (v[k]->v > v1)
				v1 = v[k]->v;
		}
	}
	g_census_seq++;
	d11_log("  draw %3u: %4d tri -> %s #%d %ux%u | rect %d,%d %dx%d | tex %s | "
		"blend %s (src=%d dst=%d) | wmask %08X | scissor %s",
		g_census_seq, ntri, rt ? (rt->is_bb ? "BACKBUFFER" : "rendertarget") : "(none)",
		rt ? rt->id : -1, rt ? rt->width : 0, rt ? rt->height : 0, (int)x0, (int)y0,
		(int)(x1 - x0), (int)(y1 - y0),
		src ? "yes" : "NONE (untextured)",
		st && st->blend_enable ? "ON" : "off", st ? st->src_blend : -1,
		st ? st->dst_blend : -1, st ? st->write_mask : 0,
		st && st->scissor_enable ? "on" : "off");
	/* A draw is usually a run of quads, and one bounding box around four of
	 * them describes none of them. Break small draws into their quads so a
	 * rectangle in the log can be matched against a rectangle on screen. */
	if (ntri >= 2 && ntri <= 16) {
		int q;

		for (q = 0; q + 1 < ntri; q += 2) {
			const SwVert *v[6];
			float qx0, qy0, qx1, qy1;
			int k;

			v[0] = &tris[q].a;
			v[1] = &tris[q].b;
			v[2] = &tris[q].c;
			v[3] = &tris[q + 1].a;
			v[4] = &tris[q + 1].b;
			v[5] = &tris[q + 1].c;
			qx0 = qy0 = 1e9f;
			qx1 = qy1 = -1e9f;
			for (k = 0; k < 6; k++) {
				if (v[k]->x < qx0)
					qx0 = v[k]->x;
				if (v[k]->x > qx1)
					qx1 = v[k]->x;
				if (v[k]->y < qy0)
					qy0 = v[k]->y;
				if (v[k]->y > qy1)
					qy1 = v[k]->y;
			}
			d11_log("               quad %d: %d,%d %dx%d | argb %08X", q / 2, (int)qx0,
				(int)qy0, (int)(qx1 - qx0), (int)(qy1 - qy0),
				(unsigned)tris[q].a.color);
		}
	}
	if (src)
		/* Texel coordinates as well as normalised, because "which corner of
		 * the atlas" is the question and 0.30..0.60 does not answer it at a
		 * glance. Vertex alpha too: these quads blend, and a faded-out
		 * element we draw solid would look exactly like a stray one. */
		d11_log("             texture #%d %ux%u%s | uv %.4f,%.4f..%.4f,%.4f "
			"= texels %d,%d..%d,%d | vertex alpha %u",
			src->id, src->width, src->height, src->pixels ? "" : " (NO PIXELS)", u0, v0,
			u1, v1, (int)(u0 * (float)src->width), (int)(v0 * (float)src->height),
			(int)(u1 * (float)src->width), (int)(v1 * (float)src->height),
			(unsigned)(tris[0].a.color >> 24));
}

static void frame_note_draw(Sw11Res *rt, UINT tris)
{
	int i;
	if (!rt)
		return;
	for (i = 0; i < g_frame_nrt; i++)
		if (g_frame_rt[i].res == rt) {
			g_frame_rt[i].tris += tris;
			g_frame_rt[i].draws++;
			return;
		}
	if (g_frame_nrt < FRAME_RT_MAX) {
		g_frame_rt[g_frame_nrt].res = rt;
		g_frame_rt[g_frame_nrt].tris = tris;
		g_frame_rt[g_frame_nrt].draws = 1;
		g_frame_nrt++;
	}
}

/* Overdraw is the dominant cost, so it has to be attributed to a target rather
 * than reported as one frame-wide total. Kept separate from the per-frame RT
 * table above, which Present clears before the profiler runs. */
#define PERF_RT_MAX 12
typedef struct PerfRt {
	int id;
	UINT w, h;
	int is_bb;
	double px;
	unsigned draws;
} PerfRt;
static PerfRt g_perf_rt[PERF_RT_MAX];
static int g_perf_nrt;

/* Overdraw is now the largest addressable cost, so it also gets attributed to
 * the texture being painted. A target total says how much is wasted; this says
 * which layer to go looking at. */
#define PERF_TEX_MAX 16
typedef struct PerfTex {
	int id;
	int w, h;
	int blend;
	double px;
	unsigned draws;
	TexPal pal;
} PerfTex;
static PerfTex g_perf_tex[PERF_TEX_MAX];
static int g_perf_ntex;

/* D3DBLENDOP/D3DBLEND spelled out. The per-texture report prints these as hex
 * pairs, which is fine when you already know which pair you are looking for and
 * useless when the question is which one you have never handled. */
static const char *blend_op_name(int op)
{
	switch (op) {
	case D3DBLENDOP_ADD:
		return "ADD";
	case D3DBLENDOP_SUBTRACT:
		return "SUB";
	case D3DBLENDOP_REVSUBTRACT:
		return "REVSUB";
	case D3DBLENDOP_MIN:
		return "MIN";
	case D3DBLENDOP_MAX:
		return "MAX";
	default:
		return "?";
	}
}

static const char *blend_factor_name(int f)
{
	switch (f) {
	case D3DBLEND_ZERO:
		return "ZERO";
	case D3DBLEND_ONE:
		return "ONE";
	case D3DBLEND_SRCCOLOR:
		return "SRCCOL";
	case D3DBLEND_INVSRCCOLOR:
		return "INVSRCCOL";
	case D3DBLEND_SRCALPHA:
		return "SRCA";
	case D3DBLEND_INVSRCALPHA:
		return "INVSRCA";
	case D3DBLEND_DESTALPHA:
		return "DSTA";
	case D3DBLEND_INVDESTALPHA:
		return "INVDSTA";
	case D3DBLEND_DESTCOLOR:
		return "DSTCOL";
	case D3DBLEND_INVDESTCOLOR:
		return "INVDSTCOL";
	default:
		return "?";
	}
}

/* Screen area drawn this window from textures with a colour census, weighted by
 * what the census found, so the answer is "how much of what we paint comes from
 * flat art" rather than "how many textures are flat". */
enum { PAL_PX, PAL_PX_RT, PAL_PX_NONE, PAL_C2, PAL_C16, PAL_C256, PAL_CMANY, PAL_CLEAR,
       PAL_SOLID, PAL_WHITE, PAL_BLACK, PAL_RUNS, PAL_TEXRUN, PAL_N };
static double g_pal[PAL_N];
static volatile LONG g_present_n;

static int pal_is_rt(const Sw11Res *r)
{
	return (r->bind & D3D11_BIND_RENDER_TARGET) != 0;
}

static void pal_take(Sw11Res *r)
{
	/* Open addressing over twice the cap; alpha 0 folds to 0, and 0 is also the
	 * empty marker, so it is tracked with its own flag. */
	uint32_t set[512];
	int have_clear = 0;
	unsigned w = r->width, h = r->height, x, y, bx, by, ncol = 0;
	const uint32_t *p = r->pixels;
	TexPal t;

	memset(&t, 0, sizeof(t));
	memset(set, 0, sizeof(set));
#define PAL_NORM(v) (((v) >> 24) ? (v) : 0u)
	for (y = 0; y < h; y++) {
		const uint32_t *row = p + (size_t)y * w;
		uint32_t prev = 0;

		for (x = 0; x < w; x++) {
			uint32_t v = PAL_NORM(row[x]);

			if (x == 0 || v != prev)
				t.texel_runs++;
			prev = v;
			if (ncol > 256)
				continue;
			if (!v) {
				if (!have_clear) {
					have_clear = 1;
					ncol++;
				}
			} else {
				unsigned k = (v * 2654435761u) >> 23;

				while (set[k] && set[k] != v)
					k = (k + 1) & 511;
				if (!set[k]) {
					set[k] = v;
					ncol++;
				}
			}
		}
	}
	t.texels = w * h;
	t.colors = ncol > 256 ? 257 : ncol;
	for (by = 0; by + 4 <= h; by += 4) {
		int prev_solid = 0;
		uint32_t prev_col = 0;

		for (bx = 0; bx + 4 <= w; bx += 4) {
			const uint32_t *b = p + (size_t)by * w + bx;
			uint32_t c = PAL_NORM(b[0]);
			int solid = 1, i;

			for (i = 0; i < 16 && solid; i++)
				solid = PAL_NORM(b[(size_t)(i >> 2) * w + (i & 3)]) == c;
			t.blocks++;
			if (solid) {
				if (!c)
					t.clear++;
				else
					t.solid++;
				if (c == 0xffffffffu)
					t.white++;
				else if (c && !(c & 0x00ffffffu))
					t.black++;
				if (!prev_solid || prev_col != c)
					t.runs++;
			}
			prev_solid = solid;
			prev_col = c;
		}
	}
#undef PAL_NORM
	t.gen = r->gpu_gen + 1;
	t.at = (unsigned)g_present_n;
	r->pal = t;
}

/* Once per content change, but a texture the game rewrites every frame would
 * then be rescanned every frame, so a changed one waits two seconds. */
static void pal_maybe_take(Sw11Res *r)
{
	if (!swrast_paintstat || !r || !r->pixels || pal_is_rt(r) || r->phantom)
		return;
	if (r->pal.gen == r->gpu_gen + 1)
		return;
	if (r->pal.gen && (unsigned)g_present_n - r->pal.at < 120)
		return;
	pal_take(r);
}

static void pal_note(const Sw11Res *r, double px)
{
	const TexPal *t;
	double b;

	if (!swrast_paintstat || !r || px <= 0.0)
		return;
	g_pal[PAL_PX] += px;
	if (pal_is_rt(r)) {
		g_pal[PAL_PX_RT] += px;
		return;
	}
	t = &r->pal;
	if (!t->gen || !t->texels) {
		g_pal[PAL_PX_NONE] += px;
		return;
	}
	g_pal[t->colors <= 2 ? PAL_C2 : t->colors <= 16 ? PAL_C16 : t->colors <= 256 ? PAL_C256
									     : PAL_CMANY] += px;
	b = t->blocks ? px / (double)t->blocks : 0.0;
	g_pal[PAL_CLEAR] += b * t->clear;
	g_pal[PAL_SOLID] += b * t->solid;
	g_pal[PAL_WHITE] += b * t->white;
	g_pal[PAL_BLACK] += b * t->black;
	g_pal[PAL_RUNS] += b * t->runs;
	g_pal[PAL_TEXRUN] += px * (double)t->texel_runs / (double)t->texels;
}

static void perf_note_tex(const Sw11Res *src, int id, int tw, int th, int blend, double px)
{
	int i;

	pal_note(src, px);
	for (i = 0; i < g_perf_ntex; i++)
		if (g_perf_tex[i].id == id) {
			g_perf_tex[i].px += px;
			g_perf_tex[i].draws++;
			if (src)
				g_perf_tex[i].pal = src->pal;
			return;
		}
	if (g_perf_ntex < PERF_TEX_MAX) {
		g_perf_tex[g_perf_ntex].id = id;
		g_perf_tex[g_perf_ntex].w = tw;
		g_perf_tex[g_perf_ntex].h = th;
		g_perf_tex[g_perf_ntex].blend = blend;
		g_perf_tex[g_perf_ntex].px = px;
		g_perf_tex[g_perf_ntex].draws = 1;
		if (src)
			g_perf_tex[g_perf_ntex].pal = src->pal;
		g_perf_ntex++;
	}
}

static void perf_note_area(Sw11Res *rt, const SwTri *tris, unsigned n, const Sw11Res *src,
			   int tw, int th, int blend)
{
	int tex_id = src ? src->id : -1;
	double px = 0.0;
	unsigned t;
	int i;
	if (!rt || !tris)
		return;
	for (t = 0; t < n; t++) {
		const SwTri *v = &tris[t];
		float x0 = fminf(v->a.x, fminf(v->b.x, v->c.x));
		float x1 = fmaxf(v->a.x, fmaxf(v->b.x, v->c.x));
		float y0 = fminf(v->a.y, fminf(v->b.y, v->c.y));
		float y1 = fmaxf(v->a.y, fmaxf(v->b.y, v->c.y));
		if (x0 < 0.0f)
			x0 = 0.0f;
		if (y0 < 0.0f)
			y0 = 0.0f;
		if (x1 > (float)rt->width)
			x1 = (float)rt->width;
		if (y1 > (float)rt->height)
			y1 = (float)rt->height;
		if (x1 > x0 && y1 > y0)
			px += 0.5 * (double)(x1 - x0) * (double)(y1 - y0);
	}
	perf_note_tex(src, tex_id, tw, th, blend, px);
	for (i = 0; i < g_perf_nrt; i++)
		if (g_perf_rt[i].id == rt->id) {
			g_perf_rt[i].px += px;
			g_perf_rt[i].draws += n ? 1 : 0;
			return;
		}
	if (g_perf_nrt < PERF_RT_MAX) {
		g_perf_rt[g_perf_nrt].id = rt->id;
		g_perf_rt[g_perf_nrt].w = rt->width;
		g_perf_rt[g_perf_nrt].h = rt->height;
		g_perf_rt[g_perf_nrt].is_bb = rt->is_bb;
		g_perf_rt[g_perf_nrt].px = px;
		g_perf_rt[g_perf_nrt].draws = 1;
		g_perf_nrt++;
	}
}

struct Sw11View {
	union {
		ID3D11RenderTargetView rtv;
		ID3D11ShaderResourceView srv;
		ID3D11DepthStencilView dsv;
	} iface;
	LONG ref;
	Sw11Device *dev;
	Sw11Res *res;
	int kind; /* 0 rtv, 1 srv, 2 dsv */
	DXGI_FORMAT format;
};

struct Sw11Shader {
	ID3D11VertexShader iface;
	LONG ref;
	Sw11Device *dev;
	int is_ps;
	unsigned char *code;
	SIZE_T bytes;
	DxbcShader dxbc;
	int parsed;
};

struct Sw11Layout {
	ID3D11InputLayout iface;
	LONG ref;
	Sw11Device *dev;
	UINT n;
	D3D11_INPUT_ELEMENT_DESC *elems;
};

struct Sw11Blend {
	ID3D11BlendState iface;
	LONG ref;
	Sw11Device *dev;
	D3D11_BLEND_DESC desc;
};

struct Sw11DS {
	ID3D11DepthStencilState iface;
	LONG ref;
	Sw11Device *dev;
	D3D11_DEPTH_STENCIL_DESC desc;
};

struct Sw11Rast {
	ID3D11RasterizerState iface;
	LONG ref;
	Sw11Device *dev;
	D3D11_RASTERIZER_DESC desc;
};

struct Sw11Samp {
	ID3D11SamplerState iface;
	LONG ref;
	Sw11Device *dev;
	D3D11_SAMPLER_DESC desc;
};

struct Sw11Query {
	ID3D11Query iface;
	LONG ref;
	Sw11Device *dev;
	D3D11_QUERY_DESC desc;
};

struct Sw11Swap {
	IDXGISwapChain1 iface;
	LONG ref;
	Sw11Device *dev;
	HWND hwnd;
	DXGI_SWAP_CHAIN_DESC desc;
	Sw11Res *bb;
	SwPriv *priv;
	/* Enough to put the window back the way it was when leaving fullscreen. */
	int fullscreen;
	LONG saved_style, saved_exstyle;
	RECT saved_rect;
};

struct Sw11Context {
	ID3D11DeviceContext1 iface;
	LONG ref;
	Sw11Device *dev;
	int deferred;
	Sw11Res *vb[16];
	UINT vb_stride[16];
	UINT vb_off[16];
	Sw11Res *ib;
	DXGI_FORMAT ib_fmt;
	UINT ib_off;
	D3D11_PRIMITIVE_TOPOLOGY topo;
	Sw11Layout *layout;
	Sw11Shader *vs;
	Sw11Shader *ps;
	Sw11Res *cb_vs[14];
	Sw11Res *cb_ps[14];
	UINT cb_vs_first[14];
	UINT cb_vs_n[14];
	UINT cb_ps_first[14];
	UINT cb_ps_n[14];
	Sw11View *ps_srv[16];
	Sw11Samp *ps_samp[16];
	Sw11View *rtv[4];
	Sw11View *dsv;
	Sw11Blend *blend;
	Sw11DS *dss;
	Sw11Rast *rs;
	D3D11_VIEWPORT vp[8];
	UINT nvp;
	D3D11_RECT scissor[8];
	UINT nscissor;
	FLOAT blend_factor[4];
	UINT sample_mask;
	UINT stencil_ref;
};

static ID3D11Device1Vtbl kDevVtbl;
static ID3D11DeviceContext1Vtbl kCtxVtbl;
static IDXGIFactory2Vtbl kFactVtbl;
static IDXGIAdapter1Vtbl kAdpVtbl;
static IDXGIOutputVtbl kOutVtbl;
static IDXGISwapChain1Vtbl kSwapVtbl;
static IDXGIDevice1Vtbl kDxgiDevVtbl;
static ID3D11Texture2DVtbl kTexVtbl;
static ID3D11BufferVtbl kBufVtbl;
static ID3D11RenderTargetViewVtbl kRtvVtbl;
static ID3D11ShaderResourceViewVtbl kSrvVtbl;
static ID3D11DepthStencilViewVtbl kDsvVtbl;
static ID3D11VertexShaderVtbl kVSVtbl;
static ID3D11PixelShaderVtbl kPSVtbl;
static ID3D11InputLayoutVtbl kLayVtbl;
static ID3D11BlendStateVtbl kBlendVtbl;
static ID3D11DepthStencilStateVtbl kDSVtbl;
static ID3D11RasterizerStateVtbl kRastVtbl;
static ID3D11SamplerStateVtbl kSampVtbl;
static ID3D11QueryVtbl kQueryVtbl;
static int g_vtbl_ready;

static void ensure_vtbls(void);
static HRESULT create_tex2d(Sw11Device *dev, const D3D11_TEXTURE2D_DESC *desc,
			     const D3D11_SUBRESOURCE_DATA *init, Sw11Res **out);

/* Resource accounting.
 *
 * A 32-bit game gets under 2 GB of address space, and every texture it makes
 * costs a real CPU-side allocation here that a GPU driver would have put in
 * video memory instead. So this backend can run a title out of address space
 * that would never have come close on hardware. Worse, the failure is silent:
 * create returns E_OUTOFMEMORY, and a game that does not check may carry on
 * with a null or a garbage size. Track it, and say so loudly when it happens.
 *
 * The largest free block matters more than the total. Long runs fragment the
 * space, and an allocation fails when no single hole is big enough regardless
 * of how much is free in aggregate. */
static void d11_log(const char *fmt, ...);
static volatile LONG g_res_live;
static volatile LONG64 g_res_bytes;

/* Everything this resource owns, which is what a free returns to the process.
 * An aliased pixels plane is not counted: it is the cpu plane. */
static LONG64 res_payload_bytes(const struct Sw11Res *r);

/* Texture payloads come straight from the OS rather than from the CRT heap.
 *
 * free() returns memory to the heap, not to the process, and the heap keeps it
 * for reuse. That is normally the right trade, but here the blocks are whole
 * textures - megabytes each, allocated and released constantly as the game
 * moves between rooms - and a heap holding those for reuse means the address
 * space used never falls, which is precisely what we were seeing: the figure
 * climbs, plateaus, and never comes back down. Worse, a released block only
 * gets reused by a request of a similar size, so a run of differently-sized
 * textures leaves the space in pieces, and eventually no single hole is large
 * enough even though plenty is free in total.
 *
 * VirtualFree with MEM_RELEASE gives the pages back immediately and leaves no
 * fragment behind. The 128 KB floor keeps small allocations on the heap, where
 * they belong: each of these costs a 64 KB-aligned reservation, so doing it for
 * everything would waste more than it saves. */
#define PAYLOAD_VA_MIN (128u * 1024u)

/* Defined below, next to the rest of the ledger. */
static void ledger_add(void *p, size_t bytes);
static void ledger_remove(void *p);

/* Reports whether a fixed-base arena is even possible in this process.
 *
 * A separate harness showed that a VirtualAlloc region restores at its original
 * address in a brand new process, 12 out of 12 on 32-bit, but only when the
 * base is chosen rather than inherited from the heap. That harness was an empty
 * program. This one has hundreds of DLLs and a heap that has already been
 * churning, so the address it wants may simply be taken, and every later step
 * depends on it not being. Reserve, measure, release: nothing is kept, so this
 * cannot change how the game behaves. It only answers whether the ground is
 * there before anyone builds on it. */
static void arena_probe(void)
{
	static const size_t sizes[] = { 1024, 768, 512, 256, 128, 64 };
	uintptr_t bases[4];
	unsigned bi, si;
	char buf[32];
	unsigned n = savestate_getenv("D3D11SW_ARENA_PROBE", buf, sizeof(buf));

	if (!n || buf[0] == '0')
		return;
	bases[0] = 0x40000000u; /* 1 GB, what the harness used */
	bases[1] = 0x30000000u;
	bases[2] = 0x50000000u;
	bases[3] = 0x20000000u;
	d11_log("arena probe: can a fixed-base reservation be had in the real process?");
	for (bi = 0; bi < 4; bi++) {
		MEMORY_BASIC_INFORMATION mbi;
		int reported = 0;

		if (VirtualQuery((void *)bases[bi], &mbi, sizeof(mbi))) {
			d11_log("  base %08X currently %s (region %.1f MB)", (unsigned)bases[bi],
				mbi.State == MEM_FREE	  ? "FREE"
				: mbi.State == MEM_COMMIT ? "COMMITTED"
							  : "RESERVED",
				(double)mbi.RegionSize / (1024.0 * 1024.0));
		}
		for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
			size_t want = sizes[si] * 1024u * 1024u;
			void *p = VirtualAlloc((void *)bases[bi], want, MEM_RESERVE, PAGE_NOACCESS);

			if (p) {
				d11_log("    reserved %4u MB at %08X - released again",
					(unsigned)sizes[si], (unsigned)(uintptr_t)p);
				VirtualFree(p, 0, MEM_RELEASE);
				reported = 1;
				break;
			}
		}
		if (!reported)
			d11_log("    nothing from 64 MB upward is available at this base "
				"(err=%lu)",
				GetLastError());
	}
}

/* A fixed-base arena for texture payloads, enabled with D3D11SW_ARENA_MB.
 *
 * Two things recommend it. The probe above shows one contiguous ~1.15 GB hole
 * from 20000000 at DLL attach, and a region reserved at a chosen base comes
 * back at the same address in a new process, which is the precondition for a
 * save file that outlives the run. Second, and useful immediately: payloads
 * currently land wherever VirtualAlloc puts them, which is what drove the
 * largest free block down to a few megabytes while hundreds of megabytes were
 * still nominally free. Confining them to one reservation cannot fragment
 * anything outside it, and freeing decommits the pages so the memory returns to
 * the OS while the address space stays ours.
 *
 * Extents live in their own allocation rather than inline headers, so the arena
 * holds nothing but payload bytes and a freed block can be decommitted whole.
 * Off by default: it changes where every large allocation in the process lives. */
#define ARENA_MAX_EXT 8192u

struct aext {
	unsigned off;
	unsigned size;
	int used;
};

static struct {
	unsigned char *base;
	size_t size;
	struct aext *ext;
	unsigned n;
	CRITICAL_SECTION cs;
	int ready;
	LONG64 live_bytes;
	unsigned fallbacks;
	unsigned peak_ext;
	int excluded;
	int told; /* the travels-with-us note is printed once, not per save */
} g_arena;

static void arena_init(void)
{
	/* 0x20000000 is now the gameheap arena's fixed base (deterministic, cross-
	 * session critical), so this texture arena tries the others first and only
	 * falls to 0x20000000 last, if the gameheap pin did not take it. */
	static const uintptr_t bases[] = { 0x40000000u, 0x30000000u, 0x20000000u };
	char buf[32];
	unsigned n = savestate_getenv("D3D11SW_ARENA_MB", buf, sizeof(buf));
	unsigned long mb = n ? strtoul(buf, NULL, 10) : 0;
	unsigned bi;

	if (!mb)
		return;
	if (mb > 1536)
		mb = 1536;
	g_arena.ext = (struct aext *)VirtualAlloc(
		NULL, ARENA_MAX_EXT * sizeof(struct aext), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!g_arena.ext) {
		d11_log("arena: extent table refused, arena disabled");
		return;
	}
	for (bi = 0; bi < sizeof(bases) / sizeof(bases[0]) && !g_arena.base; bi++) {
		unsigned long try_mb = mb;

		/* Take what is there rather than nothing: a smaller arena still
		 * confines fragmentation, it just holds fewer textures. */
		while (try_mb >= 64 && !g_arena.base) {
			g_arena.base = (unsigned char *)VirtualAlloc(
				(void *)bases[bi], (size_t)try_mb << 20, MEM_RESERVE, PAGE_NOACCESS);
			if (!g_arena.base)
				try_mb -= 64;
		}
		if (g_arena.base)
			g_arena.size = (size_t)try_mb << 20;
	}
	if (!g_arena.base) {
		VirtualFree(g_arena.ext, 0, MEM_RELEASE);
		g_arena.ext = NULL;
		d11_log("arena: no fixed base available, falling back to scattered "
			"VirtualAlloc as before");
		return;
	}
	InitializeCriticalSection(&g_arena.cs);
	g_arena.ext[0].off = 0;
	g_arena.ext[0].size = (unsigned)g_arena.size;
	g_arena.ext[0].used = 0;
	g_arena.n = 1;
	g_arena.ready = 1;
	d11_log("arena: reserved %.0f MB at %08X (asked for %lu MB)",
		(double)g_arena.size / (1024.0 * 1024.0), (unsigned)(uintptr_t)g_arena.base, mb);
}

static void *arena_alloc(size_t want)
{
	unsigned i;
	void *p = NULL;

	if (!g_arena.ready)
		return NULL;
	want = (want + 0xFFFu) & ~(size_t)0xFFF;
	if (!want || want > g_arena.size)
		return NULL;
	EnterCriticalSection(&g_arena.cs);
	for (i = 0; i < g_arena.n; i++) {
		if (g_arena.ext[i].used || g_arena.ext[i].size < want)
			continue;
		if (g_arena.ext[i].size > want && g_arena.n < ARENA_MAX_EXT) {
			/* Split: the tail stays free. */
			memmove(&g_arena.ext[i + 2], &g_arena.ext[i + 1],
				(g_arena.n - i - 1) * sizeof(struct aext));
			g_arena.ext[i + 1].off = g_arena.ext[i].off + (unsigned)want;
			g_arena.ext[i + 1].size = g_arena.ext[i].size - (unsigned)want;
			g_arena.ext[i + 1].used = 0;
			g_arena.ext[i].size = (unsigned)want;
			g_arena.n++;
		} else if (g_arena.ext[i].size > want) {
			continue; /* no room to record the split */
		}
		p = VirtualAlloc(g_arena.base + g_arena.ext[i].off, g_arena.ext[i].size,
				 MEM_COMMIT, PAGE_READWRITE);
		if (!p)
			break;
		g_arena.ext[i].used = 1;
		g_arena.live_bytes += g_arena.ext[i].size;
		if (g_arena.n > g_arena.peak_ext)
			g_arena.peak_ext = g_arena.n;
		break;
	}
	if (!p)
		g_arena.fallbacks++;
	LeaveCriticalSection(&g_arena.cs);
	return p;
}

static int arena_owns(const void *p)
{
	return g_arena.ready && (const unsigned char *)p >= g_arena.base &&
	       (const unsigned char *)p < g_arena.base + g_arena.size;
}

static void arena_free(void *p)
{
	unsigned off = (unsigned)((unsigned char *)p - g_arena.base);
	unsigned i;

	EnterCriticalSection(&g_arena.cs);
	for (i = 0; i < g_arena.n; i++) {
		if (g_arena.ext[i].off != off || !g_arena.ext[i].used)
			continue;
		/* Decommit rather than release: the pages go back to the OS, the
		 * address space stays reserved so nothing else can take it. */
		VirtualFree(g_arena.base + off, g_arena.ext[i].size, MEM_DECOMMIT);
		g_arena.ext[i].used = 0;
		g_arena.live_bytes -= g_arena.ext[i].size;
		/* Coalesce forward then backward, so neighbouring frees do not
		 * leave the arena in shards. */
		while (i + 1 < g_arena.n && !g_arena.ext[i + 1].used) {
			g_arena.ext[i].size += g_arena.ext[i + 1].size;
			memmove(&g_arena.ext[i + 1], &g_arena.ext[i + 2],
				(g_arena.n - i - 2) * sizeof(struct aext));
			g_arena.n--;
		}
		while (i > 0 && !g_arena.ext[i - 1].used) {
			g_arena.ext[i - 1].size += g_arena.ext[i].size;
			memmove(&g_arena.ext[i], &g_arena.ext[i + 1],
				(g_arena.n - i - 1) * sizeof(struct aext));
			g_arena.n--;
			i--;
		}
		break;
	}
	LeaveCriticalSection(&g_arena.cs);
}

/* Kill the non-deterministic fallback (D3D9SW_NOFALLBACK=1). A payload that does
 * not fit the pinned arena normally falls to a NULL-base VirtualAlloc, which the
 * OS places at a different address every launch - and a cross-session restore
 * cannot keep the game's cached pointer into it valid, which is the rabiribi+33803
 * fault. This knob refuses that fallback instead: the payload fails to allocate
 * (callers all handle NULL), so we can watch how load-bearing those payloads are.
 * If the game shrugs, forcing everything through the arena is the fix; if it
 * screams, the log says exactly how much and how big was refused. */
static int nofallback(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		unsigned n = savestate_getenv("D3D9SW_NOFALLBACK", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '1') ? 1 : 0;
	}
	return cached;
}

/* What a payload is FOR, so the resident cap starves only the set a bank-swap
 * window would actually hold - the game's own shader-resource textures - and
 * never our presenter. Denying a render-target/depth/backbuffer plane blacks the
 * output; denying a reclaim drops a write (the missing-text path); neither is the
 * game failing, and reading either as "the game stopped" would make the sweep
 * measure us. Only PL_TEXTURE, and only when it is a pure shader resource, is
 * cappable. */
enum pl_tag { PL_TEXTURE, PL_BUFFER, PL_PIXELS, PL_DEPTH, PL_RECLAIM };

/* Resident-budget stress cap (D3D11SW_RESIDENT_CAP_MB, 0 = off). Once live payload
 * bytes reach the cap, deny NEW game shader-resource textures (the clean
 * E_OUTOFMEMORY the game already survives) - and nothing of ours. Sweeping the cap
 * down finds where the game goes from "renders wrong but playable" to "actually
 * stops working": its minimum viable texture working set, the hard floor for how
 * small a bank-swap resident window can be. Re-read every second so the cap can be
 * walked down within a single launch, if the value's source updates live. */
static unsigned resident_cap_mb(void)
{
	static volatile LONG cached = -1;
	static volatile LONG last_ms;
	DWORD now = GetTickCount();

	if (cached < 0 || (LONG)(now - (DWORD)last_ms) > 1000) {
		char v[16];
		unsigned n = savestate_getenv("D3D11SW_RESIDENT_CAP_MB", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v)) ? (LONG)strtoul(v, NULL, 10) : 0;
		last_ms = (LONG)now;
	}
	return (unsigned)(cached > 0 ? cached : 0);
}

/* True only for a game texture we would hold in a bank-swap window: a shader
 * resource that is not also a render target and not a staging (readback) surface. */
static int pl_cappable(const Sw11Res *r, int tag)
{
	return tag == PL_TEXTURE && r &&
	       (r->bind & D3D11_BIND_SHADER_RESOURCE) &&
	       !(r->bind & D3D11_BIND_RENDER_TARGET) &&
	       r->usage != D3D11_USAGE_STAGING;
}

/* True when a fresh game texture would put live payload over the cap, so
 * CreateTexture2D should build a phantom instead of allocating it. Since a phantom
 * accounts zero bytes, once the budget is reached every further cappable texture
 * becomes a phantom and g_res_bytes holds at the cap. */
static int phantom_capped(const Sw11Res *r)
{
	unsigned cap = resident_cap_mb();

	return cap && pl_cappable(r, PL_TEXTURE) &&
	       (LONG64)g_res_bytes >= (LONG64)cap * 1024 * 1024;
}

/* A shared magenta placeholder sampled in place of a phantom's absent pixels. The
 * phantom keeps its real description, but at the draw site we bind THIS at 16x16
 * with clamp addressing, so the sampler can never read past it whatever the
 * texture's real dimensions were - the trap the 1x1-with-a-faked-size idea would
 * have sprung (an upload or Map sized for the real dimensions into a tiny buffer).
 */
#define PHANTOM_DIM 16
static uint32_t g_phantom_px[PHANTOM_DIM * PHANTOM_DIM];
/* One scratch buffer, grown to the largest phantom ever mapped, so a Map of a
 * phantom hands back a correctly-sized target instead of NULL. Writes through it
 * are thrown away on Unmap. */
static unsigned char *g_phantom_scratch;
static size_t g_phantom_scratch_sz;
static CRITICAL_SECTION g_phantom_cs;
static int g_phantom_ready;

static void phantom_init(void)
{
	int i;

	if (g_phantom_ready)
		return;
	for (i = 0; i < PHANTOM_DIM * PHANTOM_DIM; i++)
		g_phantom_px[i] = 0xFFFF00FFu; /* opaque magenta */
	InitializeCriticalSection(&g_phantom_cs);
	g_phantom_ready = 1;
}

static unsigned char *phantom_scratch(size_t n)
{
	unsigned char *p = NULL;

	if (!g_phantom_ready)
		phantom_init();
	EnterCriticalSection(&g_phantom_cs);
	if (n > g_phantom_scratch_sz) {
		unsigned char *q = (unsigned char *)realloc(g_phantom_scratch, n);

		if (q) {
			g_phantom_scratch = q;
			g_phantom_scratch_sz = n;
		}
	}
	if (g_phantom_scratch_sz >= n)
		p = g_phantom_scratch;
	LeaveCriticalSection(&g_phantom_cs);
	return p;
}

static void *payload_alloc(size_t n, const Sw11Res *r, int tag)
{
	(void)r;
	(void)tag; /* the cap decision now lives in CreateTexture2D as a phantom */
	if (!n)
		n = 4;
	if (n >= PAYLOAD_VA_MIN) {
		void *p = arena_alloc(n);

		/* Arena allocations are not ledgered: the ledger reaps with
		 * MEM_RELEASE, which is not how arena blocks are returned. Live
		 * with it while the arena is experimental, and note the cost -
		 * textures created after a save are not reclaimed on restore. */
		if (p)
			return p;
		if (nofallback()) {
			d11_log("NOFALLBACK: arena full, REFUSING a %lu KB payload rather "
				"than placing it at a non-deterministic base",
				(unsigned long)(n >> 10));
			return NULL;
		}
		p = VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (p)
			ledger_add(p, n);
		return p;
	}
	return calloc(1, n);
}

/* The size must be the one the allocation was made with, or this frees through
 * the wrong allocator. */
static void payload_free(void *p, size_t n)
{
	if (!p)
		return;
	if (!n)
		n = 4;
	/* The pointer decides the allocator, not the size: an arena block is
	 * inside the reservation and nothing else is. That keeps the two paths
	 * from ever crossing even if a caller rounds a size differently. */
	if (arena_owns(p)) {
		arena_free(p);
		return;
	}
	if (n >= PAYLOAD_VA_MIN) {
		ledger_remove(p);
		VirtualFree(p, 0, MEM_RELEASE);
	} else
		free(p);
}

/* Every VirtualAlloc'd payload, recorded outside the snapshot.
 *
 * A restore rewinds this module's globals along with the game's memory, so the
 * live list, the retain list and the byte counters all come back describing the
 * instant of the save. The allocations made since then do not come back: putting
 * memory contents back does not un-reserve address space, so every payload
 * created after the save survives the rewind with nothing left pointing at it.
 * The counters then read 610 MB while the process holds well over a gigabyte,
 * and a session of saving and restoring fills the address space until a texture
 * cannot be placed. That is the crash, and it is invisible to the accounting
 * precisely because the accounting is rewound too.
 *
 * This ledger sits in its own excluded allocation so it describes the present
 * while everything else goes backwards - the same device the savestate engine
 * uses for its unwind-table and critical-section records. After a restore it
 * names exactly the payloads the rewound state can no longer reach.
 *
 * The VirtualAlloc side only. Small payloads come from the CRT heap, whose own
 * bookkeeping IS inside the snapshot, so the rewind already reclaims those;
 * freeing one afterwards would be a double free against a heap that has been
 * told the allocation never happened. */
#define LEDGER_CAP 65536u

struct ledger_ent {
	void *p;
	size_t bytes;
	unsigned seq;
};

struct ledger {
	volatile LONG lock;
	unsigned n, seq, mark, full, excluded;
	struct ledger_ent e[LEDGER_CAP];
};

/* The pointer is an ordinary global and so is rewound, but it is only ever
 * assigned once and always to the same address, so a rewind restores the value
 * it already had. The allocation happens on the first payload, long before a
 * savestate can exist. */
/* Defined with the retain generation, used by the arena exclusion above it. */
static int sw_heap_rewinds(void);

static struct ledger *g_led;

static struct ledger *ledger_get(void)
{
	if (!g_led) {
		/* VirtualAlloc only. Registering the exclusion here would be the
		 * natural place and is wrong: savestate_exclude brings the whole
		 * engine up, and the first payload is allocated during startup,
		 * before the game has populated its heaps. The engine then took
		 * its census of a process that barely existed yet, judged the
		 * ucrtbase process heap to be the game's on no evidence at all,
		 * and rewound it - which corrupted that heap on the next restore.
		 * Registration waits for ledger_register, on the savestate path,
		 * where the engine would have started anyway. */
		g_led = (struct ledger *)VirtualAlloc(NULL, sizeof(struct ledger),
						      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	}
	return g_led;
}

/* Called on the savestate path, before anything can take a snapshot, so the
 * ledger is out of the rewind by the time one exists. */
/* Defined with the fault handler further down; needed here to re-order the
 * handler once the savestate engine has installed its own. */
static int heal_cap(void);
static LONG CALLBACK d11_veh(EXCEPTION_POINTERS *ep);
static void mapprobe_summary(void);
static PVOID g_veh_tok;

void xa2_sw_trace_dump(void (*emit)(const char *));

/* d11_log takes a format string; the trace dump hands over finished lines, and
 * a line with a stray percent in it must not be reinterpreted. */
static void d11_log_line(const char *s)
{
	d11_log("%s", s);
}

static void ledger_register(void)
{
	struct ledger *l = ledger_get();

	if (!l || l->excluded)
		return;
	l->excluded = 1;
	savestate_exclude(l, sizeof(*l));

	/* The arena has to go with it, and for the same reason.
	 *
	 * Its extent table lives in this module, which is held in the present
	 * along with our heap. The pages it hands out are ordinary committed
	 * private memory, so without this the engine captures them and, on
	 * restore, reclaims whatever was committed after the save. The
	 * bookkeeping then describes memory that is no longer there: the table
	 * says a block is live and the pages under it are reserved, not
	 * committed. That is the fault at d3d11.dll+16CF4 reading 2E992000 with
	 * allocation base 20000000 - our own read, into our own arena, of a page
	 * the restore took back. It needs churn to show, which is why it
	 * survived one cycle and died on the second.
	 *
	 * Excluding it means texture pixels do not rewind. That is already true
	 * of everything else we own while REWIND_SWHEAP is off, so the two
	 * halves stay consistent, which is the only property that matters.
	 *
	 * Which is why the exclusion has to follow the knob rather than be
	 * unconditional. With REWIND_SWHEAP on, our heap and our image rewind and
	 * the arena did not, putting the two halves on opposite sides - the exact
	 * split this comment was written to prevent, arrived at by turning a
	 * setting rather than by editing this code. It showed up as a restore that
	 * brought back the previous room's objects wearing the current room's
	 * pixels, and the coverage check named it: 233 MB, private, writable,
	 * changed since the save, excluded by us.
	 *
	 * Read through savestate_getenv rather than getenv, because a game
	 * launched from Steam inherits no shell variables and the setting lives in
	 * d3d9_sw.cfg. Plain getenv here would report "off" on every Steam run and
	 * hold the arena back no matter what the file said. */
	if (g_arena.ready && !g_arena.excluded && !sw_heap_rewinds()) {
		g_arena.excluded = 1;
		savestate_exclude(g_arena.base, g_arena.size);
		d11_log("arena: %.0f MB at %08X excluded from the snapshot, so its pages "
			"and its extent table stay on the same side of a restore",
			(double)g_arena.size / (1024.0 * 1024.0),
			(unsigned)(uintptr_t)g_arena.base);
	} else if (g_arena.ready && !g_arena.excluded && !g_arena.told) {
		/* Said once. ledger_register runs on every savestate call, and the
		 * branch above latches itself by setting excluded; this one has
		 * nothing to latch, so without a flag it reprints forever. */
		g_arena.told = 1;
		d11_log("arena: %.0f MB at %08X travels WITH the snapshot, because "
			"REWIND_SWHEAP puts our heap and our image in the past and the "
			"pixels have to go with the objects that name them. Saves grow by "
			"about that much",
			(double)g_arena.size / (1024.0 * 1024.0),
			(unsigned)(uintptr_t)g_arena.base);
	}

	/* Move our handler back to the front, now that the engine has installed
	 * its own. Windows calls the most recently registered "first" handler
	 * first, and the engine initialises after we do - so its handler was
	 * seeing every fault and continuing execution before ours ran. That is
	 * why a read of reserved-but-uncommitted memory, exactly the case the
	 * guard exists for, was reported by the engine and healed by nobody. */
	if (heal_cap() > 0 && g_veh_tok) {
		RemoveVectoredExceptionHandler(g_veh_tok);
		g_veh_tok = AddVectoredExceptionHandler(1, d11_veh);
		d11_log("heal guard armed, cap %d, and moved ahead of the savestate "
			"engine's handler",
			heal_cap());
	}
	/* A tally beside every save: how the game reaches payloads (handle vs raw
	 * pointer), the number the whole aperture plan turns on. */
	mapprobe_summary();
}

static void ledger_lock(struct ledger *l)
{
	while (InterlockedCompareExchange(&l->lock, 1, 0) != 0)
		YieldProcessor();
}

static void ledger_unlock(struct ledger *l)
{
	InterlockedExchange(&l->lock, 0);
}

static void ledger_add(void *p, size_t bytes)
{
	struct ledger *l = ledger_get();

	if (!l || !p)
		return;
	ledger_lock(l);
	if (l->n < LEDGER_CAP) {
		l->e[l->n].p = p;
		l->e[l->n].bytes = bytes;
		l->e[l->n].seq = ++l->seq;
		l->n++;
	} else if (!l->full) {
		l->full = 1;
		d11_log("payload ledger full at %u entries - payloads beyond this point "
			"cannot be reclaimed after a restore",
			LEDGER_CAP);
	}
	ledger_unlock(l);
}

static void ledger_remove(void *p)
{
	struct ledger *l = g_led;
	unsigned i;

	if (!l || !p)
		return;
	ledger_lock(l);
	/* Backwards: the common case is a payload freed near the time it was
	 * made, and a forward scan turns that into a walk of the whole table. */
	for (i = l->n; i-- > 0;)
		if (l->e[i].p == p) {
			l->e[i] = l->e[l->n - 1];
			l->n--;
			break;
		}
	ledger_unlock(l);
}

/* A save has succeeded, so everything recorded so far is part of the moment the
 * snapshot describes and has to survive any restore of it. */
static void ledger_mark(void)
{
	struct ledger *l = ledger_get();

	if (!l)
		return;
	ledger_lock(l);
	l->mark = l->seq;
	ledger_unlock(l);
}

/* Every payload pointer a live resource still owns, gathered just before a reap.
 * Defined below with the live list, which is declared after this point. */
static int owned_build(void);
static void owned_done(void);
static int owned_holds(const void *p);

/* The objects the game keeps one of - device, immediate context, swap chain,
 * factory, adapter, output - at the same address in every launch.
 *
 * The game stores raw pointers to them in its own memory, and a restore from
 * another process puts those pointers back. From the heap they land wherever
 * that launch's heap happened to be; the first cross-session restore to get
 * this far called RSSetViewports through an immediate context pointer that
 * now pointed into the savestate control block. This pool is static storage in
 * an image pinned at a fixed base, and first-fit over the same sequence of
 * creates and releases, so the Nth object comes out at the same address each
 * launch. The image is held in the present, so the object there is this
 * launch's own, working one. The pin log line is how two launches are compared
 * to confirm the sequence really does repeat. */
#define PIN_UNIT 64u
#define PIN_UNITS 131072u
static unsigned char g_pin_mem[PIN_UNIT * PIN_UNITS] __attribute__((aligned(64)));
static unsigned short g_pin_len[PIN_UNITS];
/* What each run holds, so a load from another launch can say how many of the
 * game's restored pointers land on an object of the same kind in this one. */
enum { PK_FREE, PK_SINGLE, PK_RES, PK_VIEW, PK_SHADER, PK_LAYOUT, PK_BLEND, PK_DS, PK_RAST,
       PK_SAMP, PK_QUERY, PK_N };
static const char *const g_pk_name[PK_N] = { "free",   "singleton", "resource", "view",
					     "shader", "layout",    "blend",    "depth-stencil",
					     "raster", "sampler",   "query" };
static unsigned char g_pin_kind[PIN_UNITS];
static volatile LONG g_pin_lock;
static unsigned g_pin_seq;
static LONG g_pin_full_logged;
/* Unit index + 1 the next pin_calloc on this thread must use instead of first
 * fit, for rebuilding a saved object at the address the game holds for it. */
static unsigned g_pin_at;
static DWORD g_pin_at_tid;
static int g_pin_foreign_seen;

static void *pin_calloc(size_t bytes, const char *what, unsigned char kind)
{
	unsigned need = (unsigned)((bytes + PIN_UNIT - 1) / PIN_UNIT), i = 0, run;
	void *p = NULL;

	while (InterlockedCompareExchange(&g_pin_lock, 1, 0) != 0)
		YieldProcessor();
	if (!g_pin_seq)
		savestate_follow_save_range(g_pin_mem, sizeof(g_pin_mem));
	if (g_pin_at && g_pin_at_tid == GetCurrentThreadId()) {
		i = g_pin_at - 1;
		g_pin_at = 0;
		for (run = 0; run < need && i + run < PIN_UNITS && !g_pin_len[i + run]; run++)
			;
		if (need && need <= 0xFFFF && run == need) {
			g_pin_len[i] = (unsigned short)need;
			g_pin_kind[i] = kind;
			for (run = 1; run < need; run++)
				g_pin_len[i + run] = 0xFFFF;
			p = g_pin_mem + (size_t)i * PIN_UNIT;
		}
		i = 0;
	}
	while (!p && need && need <= 0xFFFF && i + need <= PIN_UNITS) {
		for (run = 0; run < need && !g_pin_len[i + run]; run++)
			;
		if (run == need) {
			g_pin_len[i] = (unsigned short)need;
			g_pin_kind[i] = kind;
			for (run = 1; run < need; run++)
				g_pin_len[i + run] = 0xFFFF;
			p = g_pin_mem + (size_t)i * PIN_UNIT;
			break;
		}
		i += run + (g_pin_len[i + run] == 0xFFFF ? 1 : g_pin_len[i + run]);
	}
	g_pin_seq++;
	InterlockedExchange(&g_pin_lock, 0);
	if (!p) {
		if (!InterlockedExchange(&g_pin_full_logged, 1))
			d11_log("pin: pool full at %s (%u bytes) - this and later objects come "
				"from the heap and will not keep their address across launches",
				what, (unsigned)bytes);
		return calloc(1, bytes);
	}
	memset(p, 0, bytes);
	if (kind == PK_SINGLE)
		d11_log("pin: %s #%u at %p, %u bytes", what, g_pin_seq, p, (unsigned)bytes);
	return p;
}

/* The pool's layout as the save saw it, beside the slot. Written before the save
 * rather than after, because a restored thread resumes inside the save and
 * returns through it. */
static void pin_map_write(int slot)
{
	char path[64];
	HANDLE f;
	DWORD w;

	wsprintfA(path, "d3d9sw_slot%d.pins", slot);
	f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	while (InterlockedCompareExchange(&g_pin_lock, 1, 0) != 0)
		YieldProcessor();
	WriteFile(f, g_pin_len, sizeof(g_pin_len), &w, NULL);
	WriteFile(f, g_pin_kind, sizeof(g_pin_kind), &w, NULL);
	WriteFile(f, g_pin_mem, sizeof(g_pin_mem), &w, NULL);
	InterlockedExchange(&g_pin_lock, 0);
	CloseHandle(f);
}

static void pin_rebuild(const unsigned short *sl, const unsigned char *sk,
			const unsigned char *smem);

/* After a load from another launch: for every object the save's pool held, is
 * there an object of the same kind and size at that address now? The game's
 * restored pointers go to those addresses, and nothing else is put there. */
static void pin_map_compare(int slot)
{
	static unsigned short sl[PIN_UNITS];
	static unsigned char sk[PIN_UNITS];
	unsigned same[PK_N], sized[PK_N], other[PK_N], gone[PK_N], extra = 0, i;
	char path[64];
	HANDLE f;
	DWORD r1 = 0, r2 = 0, r3 = 0;
	unsigned char *smem;
	int k;

	wsprintfA(path, "d3d9sw_slot%d.pins", slot);
	f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		d11_log("pin map: no %s to compare against", path);
		return;
	}
	ReadFile(f, sl, sizeof(sl), &r1, NULL);
	ReadFile(f, sk, sizeof(sk), &r2, NULL);
	if (r1 != sizeof(sl) || r2 != sizeof(sk)) {
		CloseHandle(f);
		d11_log("pin map: %s is from a different pool size, not compared", path);
		return;
	}
	smem = (unsigned char *)VirtualAlloc(NULL, sizeof(g_pin_mem), MEM_COMMIT | MEM_RESERVE,
					     PAGE_READWRITE);
	if (smem && (!ReadFile(f, smem, sizeof(g_pin_mem), &r3, NULL) || r3 != sizeof(g_pin_mem))) {
		VirtualFree(smem, 0, MEM_RELEASE);
		smem = NULL;
	}
	CloseHandle(f);
	memset(same, 0, sizeof(same));
	memset(sized, 0, sizeof(sized));
	memset(other, 0, sizeof(other));
	memset(gone, 0, sizeof(gone));
	for (i = 0; i < PIN_UNITS; i++) {
		unsigned kind = sk[i] < PK_N ? sk[i] : PK_FREE;
		int live_start = g_pin_len[i] && g_pin_len[i] != 0xFFFF;

		if (live_start && !(sl[i] && sl[i] != 0xFFFF))
			extra++;
		if (!sl[i] || sl[i] == 0xFFFF)
			continue;
		if (!live_start)
			gone[kind]++;
		else if (g_pin_kind[i] != sk[i])
			other[kind]++;
		else if (g_pin_len[i] != sl[i])
			sized[kind]++;
		else
			same[kind]++;
	}
	d11_log("pin map: slot %d's objects against this launch's pool - same kind and size / "
		"same kind, other size / another kind there / nothing there",
		slot);
	for (k = 1; k < PK_N; k++)
		if (same[k] + sized[k] + other[k] + gone[k])
			d11_log("  %-14s %6u / %6u / %6u / %6u", g_pk_name[k], same[k], sized[k],
				other[k], gone[k]);
	d11_log("  and %u object(s) in this launch's pool at addresses the save had empty",
		extra);
	if (!smem) {
		d11_log("pin map: %s holds no object contents (written by an older build), so "
			"nothing is rebuilt",
			path);
		return;
	}
	pin_rebuild(sl, sk, smem);
	VirtualFree(smem, 0, MEM_RELEASE);
}

static void obj_free(void *p)
{
	unsigned i, n;

	if ((unsigned char *)p < g_pin_mem ||
	    (unsigned char *)p >= g_pin_mem + sizeof(g_pin_mem)) {
		free(p);
		return;
	}
	i = (unsigned)(((unsigned char *)p - g_pin_mem) / PIN_UNIT);
	while (InterlockedCompareExchange(&g_pin_lock, 1, 0) != 0)
		YieldProcessor();
	n = g_pin_len[i];
	g_pin_kind[i] = PK_FREE;
	while (n && n != 0xFFFF && n--)
		g_pin_len[i + n] = 0;
	InterlockedExchange(&g_pin_lock, 0);
}

/* A restore has completed and the threads are running again. Anything recorded
 * after the mark is unreachable: the game's memory no longer holds a pointer to
 * it, and neither does this module's, because both were rewound past its
 * creation. Nothing will ever release it, so release it here.
 *
 * That second clause is the whole justification, and under D3D9SW_REWIND_SWHEAP=0
 * it is false. The engine then excludes this wrapper's image and heap, so the
 * live list, the retain list and every Sw11Res survive the restore intact. A
 * texture created after the save is still on g_res_head with a positive
 * reference count, quite possibly still bound to the immediate context, and
 * this walk would hand its pixels back to the operating system underneath it.
 * The rasterizer then gathers from released address space. The fault handler's
 * heal can paper over the read by committing a zero page, which turns a crash
 * into a black sprite and hides the cause - the reason this went unnoticed.
 *
 * So the rule narrows to what is actually provable: free what was allocated
 * after the mark AND is not still owned by a live resource. What remains -
 * created after the save, still held by us, unreachable from the rewound game -
 * is a genuine orphan, but releasing it needs the object torn down rather than
 * the memory pulled out from under it, which is a different piece of work.
 * Leaking it is the safe half of that trade and is what happens here.
 *
 * The sequence counter is deliberately not rewound to the mark. Left running, a
 * second restore against the same snapshot reclaims what the first restore's
 * gameplay allocated, which is the case that made the address space climb
 * across a session rather than across a single load. */
static void ledger_reap(void)
{
	struct ledger *l = g_led;
	unsigned i = 0, freed = 0, held = 0;
	double bytes = 0.0, held_bytes = 0.0;

	if (!l)
		return;
	/* The mark, the live list and the payloads were this process's; a slot
	 * from another one leaves the list holding that process's pointers, and
	 * walking it faulted in owned_build. Leaking is the safe half here too. */
	if (savestate_last_load_foreign()) {
		d11_log("skipped the payload reap: the slot came from another process, "
			"so what this one created since the save is not something the "
			"restored memory can say anything about");
		return;
	}
	/* Before the ledger lock, because it takes the live-list lock and the
	 * retain lock, and nothing else acquires those in the other order. */
	if (!owned_build()) {
		d11_log("skipped the payload reap: could not take a stable census of "
			"what the live resources own, and freeing on a partial census "
			"could pull memory out from under a resource still using it");
		return;
	}
	ledger_lock(l);
	while (i < l->n) {
		void *p = l->e[i].p;

		if (l->e[i].seq <= l->mark) {
			i++;
			continue;
		}
		if (owned_holds(p)) {
			held++;
			held_bytes += (double)l->e[i].bytes;
			i++;
			continue;
		}
		/* A worker may still be gathering from this texture: the restore
		 * put the game's pointers back but said nothing to the rasterizer. */
		swrast_flush_if_pending(p);
		bytes += (double)l->e[i].bytes;
		freed++;
		l->e[i] = l->e[l->n - 1];
		l->n--;
		/* i is not advanced: the entry swapped into this slot has not
		 * been looked at yet. */
		VirtualFree(p, 0, MEM_RELEASE);
	}
	ledger_unlock(l);
	owned_done();
	if (freed)
		d11_log("reclaimed %u orphaned payload(s), %.1f MB, created after the save "
			"and left unreachable by the restore",
			freed, bytes / (1024.0 * 1024.0));
	if (held)
		d11_log("kept %u post-save payload(s), %.1f MB, that a live resource still "
			"owns - freeing them would have released memory still in use. They "
			"go back when the resource does",
			held, held_bytes / (1024.0 * 1024.0));
}

/* The live list. A plain spinlock rather than a critical section because the
 * only operations are a few pointer writes, and this is touched on resource
 * create and destroy, not per draw. */
static Sw11Res *g_res_head;
static volatile LONG g_res_list_lock;

static void res_list_lock(void)
{
	while (InterlockedCompareExchange(&g_res_list_lock, 1, 0) != 0)
		YieldProcessor();
}

static void res_list_unlock(void)
{
	InterlockedExchange(&g_res_list_lock, 0);
}

/* Saves taken so far. An object created while this reads N did not exist at
 * save N, because save N had already been taken when it was made.
 *
 * This counter lives in the module's own data, which is only in the present
 * because D3D9SW_REWIND_SWHEAP is 0 and the savestate engine then excludes this
 * wrapper's image along with its heap. Were the wrapper rewound, a restore
 * would put this back to its value at the save and every object would compare
 * as post-save, so nothing would be retained and sprites released after a save
 * would go missing after a restore. Silent, and slow to trace back to here,
 * which is why the first save says out loud which way the knob is set. */
static volatile LONG g_save_gen;

/* One reader for the knob, so the arena, the image and this warning cannot
 * disagree about which way it is set. Matches the engine's own default: absent
 * means rewind. */
static int sw_heap_rewinds(void)
{
	char v[8];
	unsigned n = savestate_getenv("D3D9SW_REWIND_SWHEAP", v, sizeof(v));

	return (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
}

static void save_gen_note_once(void)
{
	static LONG said;

	if (InterlockedIncrement(&said) != 1)
		return;
	if (!sw_heap_rewinds())
		return;
	d11_log("REWIND_SWHEAP is on, so this wrapper rewinds with the game. The retain "
		"generation stops working, because the counter goes back too and every "
		"object then looks newer than the save. That is harmless only because the "
		"arena rewinds as well, so a release after the save is undone rather than "
		"needing to be retained - if the arena is ever held back again while this "
		"is on, sprites will vanish after a restore and this line is the reason");
}

static void res_list_add(Sw11Res *r)
{
	if (!r || r->listed)
		return;
	r->born_gen = g_save_gen;
	r->plane_gen = g_save_gen;
	res_list_lock();
	r->live_prev = NULL;
	r->live_next = g_res_head;
	if (g_res_head)
		g_res_head->live_prev = r;
	g_res_head = r;
	r->listed = 1;
	res_list_unlock();
}

static void res_list_remove(Sw11Res *r)
{
	if (!r || !r->listed)
		return;
	res_list_lock();
	if (r->live_prev)
		r->live_prev->live_next = r->live_next;
	else if (g_res_head == r)
		g_res_head = r->live_next;
	if (r->live_next)
		r->live_next->live_prev = r->live_prev;
	r->live_next = NULL;
	r->live_prev = NULL;
	r->listed = 0;
	res_list_unlock();
}

/* The payload pointers the live resources own, sorted so the reap can ask about
 * one in log time rather than walking the list per ledger entry. Retained
 * objects are covered without special handling: they stay on the live list,
 * because keeping their payload means res_free_payload never ran.
 *
 * Built and thrown away around a single reap, which happens once per restore,
 * so the allocation is not on any hot path. */
static void **g_own;
static unsigned g_own_n;

static int own_cmp(const void *a, const void *b)
{
	void *const x = *(void *const *)a, *const y = *(void *const *)b;

	return x < y ? -1 : (x > y ? 1 : 0);
}

static int owned_build(void)
{
	unsigned cap;
	Sw11Res *r;

	g_own = NULL;
	g_own_n = 0;
	/* Three planes each, plus room for creates racing this census. Sized
	 * from a lock-free read so the allocation happens before the lock. */
	cap = (unsigned)(g_res_live > 0 ? g_res_live : 0) * 3u + 768u;
	g_own = (void **)calloc(cap, sizeof(void *));
	if (!g_own)
		return 0;
	res_list_lock();
	for (r = g_res_head; r; r = r->live_next) {
		/* Refusing to answer beats answering with a gap: a pointer missed
		 * here reads as unowned and its memory is handed back while the
		 * resource is still using it. The caller skips the reap instead. */
		if (g_own_n + 3u > cap) {
			res_list_unlock();
			free(g_own);
			g_own = NULL;
			g_own_n = 0;
			return 0;
		}
		if (r->cpu)
			g_own[g_own_n++] = r->cpu;
		/* An aliased plane is r->cpu again, already recorded. */
		if (r->pixels && !r->pixels_alias)
			g_own[g_own_n++] = r->pixels;
		if (r->depth)
			g_own[g_own_n++] = r->depth;
	}
	res_list_unlock();
	if (g_own_n > 1)
		qsort(g_own, g_own_n, sizeof(void *), own_cmp);
	return 1;
}

static void owned_done(void)
{
	free(g_own);
	g_own = NULL;
	g_own_n = 0;
}

static int owned_holds(const void *p)
{
	unsigned lo = 0, hi = g_own_n;

	if (!g_own || !p)
		return 0;
	while (lo < hi) {
		unsigned mid = lo + (hi - lo) / 2;

		if (g_own[mid] == p)
			return 1;
		if (g_own[mid] < p)
			lo = mid + 1;
		else
			hi = mid;
	}
	return 0;
}

/* Cumulative, not current.
 *
 * g_res_live answers "how many now", and that turned out to be the wrong
 * question: across a session of restores it climbed from 225 to 12509 while
 * also falling back to 225 on its own more than once, so the same number was
 * consistent with a leak, with churn, and with both at once. A running total of
 * births and deaths separates them. If births keep climbing while deaths track
 * them, the wrapper is churning and the peak is a watermark; if deaths fall
 * behind, objects are being orphaned and the difference is exactly how many. */
static volatile LONG g_res_born, g_res_died;

static void res_account(LONG64 bytes, int live_delta)
{
	InterlockedExchangeAdd64(&g_res_bytes, bytes);
	InterlockedExchangeAdd(&g_res_live, live_delta);
	if (live_delta > 0)
		InterlockedExchangeAdd(&g_res_born, live_delta);
	else if (live_delta < 0)
		InterlockedExchangeAdd(&g_res_died, -live_delta);
}

static void va_stats(double *used_mb, double *free_mb, double *largest_mb)
{
	MEMORY_BASIC_INFORMATION mbi;
	char *p = NULL;
	SIZE_T used = 0, freed = 0, largest = 0;

	while (VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
		if (mbi.State == MEM_FREE) {
			freed += mbi.RegionSize;
			if (mbi.RegionSize > largest)
				largest = mbi.RegionSize;
		} else {
			used += mbi.RegionSize;
		}
		if ((char *)mbi.BaseAddress + mbi.RegionSize <= p)
			break; /* no forward progress, stop rather than spin */
		p = (char *)mbi.BaseAddress + mbi.RegionSize;
	}
	*used_mb = used / (1024.0 * 1024.0);
	*free_mb = freed / (1024.0 * 1024.0);
	*largest_mb = largest / (1024.0 * 1024.0);
}

static void log_oom(const char *what, LONG64 bytes)
{
	static LONG said;
	double used, freed, largest;

	if (InterlockedIncrement(&said) > 8)
		return;
	va_stats(&used, &freed, &largest);
	d11_log("OUT OF MEMORY creating %s of %.2f MB. Live resources %ld holding %.1f MB. "
		"Address space: %.0f MB used, %.0f MB free, largest single free block %.1f MB. "
		"The game is being told E_OUTOFMEMORY and may not be checking",
		what, bytes / (1024.0 * 1024.0), (long)g_res_live,
		g_res_bytes / (1024.0 * 1024.0), used, freed, largest);
}
static HRESULT sw11_create_device(IDXGIAdapter *adapter, UINT flags, D3D_FEATURE_LEVEL *level,
				  Sw11Device **out);
static HRESULT sw11_create_swap(Sw11Device *dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC *desc,
				Sw11Swap **out);
static void ctx_draw(Sw11Context *c, UINT count, UINT start, INT base, int indexed, UINT inst);

/* Per-draw tracing reopens and flushes the log for every line, which costs far
 * more than the rasterising it describes. Keep it available for diagnosis but
 * off unless D3D11SW_TRACE is set. */
/* D3D11SW_POINT_MAG=1 drops bilinear on draws that magnify by 2x or more. */
static int point_mag_on(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *v = getenv("D3D11SW_POINT_MAG");
		cached = (v && v[0] && v[0] != '0') ? 1 : 0;
	}
	return cached;
}

/* D3D11SW_TEXT_SHARPEN sets how steeply a font atlas's alpha ramp is pulled
 * about its halfway point, in multiples of the ramp's own slope. A distance
 * field spreads its edge over several texels, so the factor needed to land the
 * transition inside one pixel is that spread times the magnification - scaling
 * by magnification alone leaves minified text, which is most text, at a factor
 * of one and therefore unsharpened, and an unsharpened distance ramp used as
 * opacity fills every glyph solid. 0 restores the hard cut at 0.5. */
static float text_sharpen(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *v = getenv("D3D11SW_TEXT_SHARPEN");
		cached = (v && v[0]) ? atoi(v) : 8;
		if (cached < 0)
			cached = 0;
		else if (cached > 64)
			cached = 64;
	}
	return (float)cached;
}

static int trace_on(void)
{
	static int v = -1;
	if (v < 0) {
		const char *s = getenv("D3D11SW_TRACE");
		v = (s && *s && *s != '0') ? 1 : 0;
	}
	return v;
}

static void d11_vlog(const char *fmt, va_list ap)
{
	static volatile LONG n;
	/* Opened once and kept. Every line used to open, append, flush and close,
	 * which is several syscalls plus a real-time antivirus inspection each
	 * time, against a path under Program Files. Flushing without closing is
	 * just as safe against the process dying - the bytes are with the OS
	 * either way - so the close was buying nothing but the cost. */
	static FILE *g_logf;
	char line[640];
	LONG seq = InterlockedIncrement(&n);
	/* The cap exists so an unattended session cannot fill a disk, but going
	 * quiet without saying so makes the end of the log ambiguous: a log that
	 * stops looks exactly like a process that died, and that ambiguity was read
	 * the wrong way once already. One line, at the boundary, says which it is. */
	if (seq > 20000) {
		if (seq == 20001) {
			if (!g_logf)
				g_logf = fopen("d3d11_sw.log", "a");
			if (g_logf) {
				fprintf(g_logf,
					"log capped at 20000 lines; nothing after this "
					"is recorded, and the process is still running\n");
				fflush(g_logf);
			}
		}
		return;
	}
	_vsnprintf(line, sizeof(line), fmt, ap);
	line[sizeof(line) - 1] = 0;
	/* Three kernel round-trips through a system-wide mutex, for a listener
	 * that is almost never attached. Reading the PEB to find that out costs
	 * nothing by comparison. */
	if (IsDebuggerPresent()) {
		OutputDebugStringA("[d3d11_sw] ");
		OutputDebugStringA(line);
		OutputDebugStringA("\n");
	}
	if (!g_logf)
		g_logf = fopen("d3d11_sw.log", "a");
	if (g_logf) {
		fprintf(g_logf, "%s\n", line);
		fflush(g_logf);
	}
}

/* Infrequent events: device and swapchain creation, shader signatures,
 * unimplemented opcodes. Always recorded. */
static void d11_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	d11_vlog(fmt, ap);
	va_end(ap);
}

/* Hot paths: draws, clears, presents, maps and copies. */
static void d11_trace_impl(const char *fmt, ...)
{
	va_list ap;
	if (!trace_on())
		return;
	va_start(ap, fmt);
	d11_vlog(fmt, ap);
	va_end(ap);
}

/* The test has to happen before the arguments are built, not after.
 *
 * As a plain function the gate lived inside the callee, so every call site
 * evaluated its whole argument list on every draw and then threw the result
 * away. That is wasted work on the hottest path in the wrapper, and on the draw
 * path it was worse than wasted: one of the arguments dereferences a texture's
 * payload to quote its first texel, and the payload is memory the release
 * machinery is allowed to hand back to the adapter and decommit. A draw that
 * sampled a texture released a moment earlier read a reserved-but-uncommitted
 * page inside the texture arena and took the process down - a crash caused
 * entirely by preparing a log line nobody had asked for. */
#define d11_trace(...)                                                                             \
	do {                                                                                       \
		if (trace_on())                                                                    \
			d11_trace_impl(__VA_ARGS__);                                               \
	} while (0)

/* The log simply stops when the process dies, which leaves no way to tell a
 * crash from an ordinary quit. A vectored handler runs ahead of both Unity's
 * crash handler and any SEH frame, so a fault gets recorded here first;
 * returning CONTINUE_SEARCH leaves Unity's own dump untouched. */
static volatile LONG g_present_n;

/* Backbuffer geometry in words, for the fault logger to quote. Written when the
 * surface is made and read from a handler, so it is a fixed buffer rather than
 * anything that needs allocating at fault time. */
static char g_bb_desc[160];

/* The last few pointers Map handed out.
 *
 * When a game faults just past the end of a buffer, the one thing worth
 * knowing immediately is whether that buffer came from us. A resource we
 * under-allocated and a local array the game overflowed on its own stack look
 * identical in a fault record, and they need completely different fixes, so
 * keep enough history to tell them apart at the moment it matters. */
#define MAP_TRACK 16
static struct {
	const unsigned char *p;
	size_t bytes;
	int id, bind, kind;
} g_map_track[MAP_TRACK];
static volatile LONG g_map_track_n;

static void map_track(const unsigned char *p, size_t bytes, int id, int bind, int kind)
{
	LONG i = (InterlockedIncrement(&g_map_track_n) - 1) & (MAP_TRACK - 1);
	g_map_track[i].p = p;
	g_map_track[i].bytes = bytes;
	g_map_track[i].id = id;
	g_map_track[i].bind = bind;
	g_map_track[i].kind = kind;
}

/* ---- guard-page-on-Unmap probe (D3D9SW_MAPPROBE=1) -------------------------
 *
 * The hinge for the whole bank-swap/aperture plan: does the game reach a payload
 * only through our API (BY HANDLE - then the payload can float freely through a
 * resident window) or does it cache the Map pointer and dereference the payload
 * while it is UNMAPPED (BY RAW POINTER - then that payload needs a stable address
 * or a fault-in)? This measures it directly. On Unmap we arm the payload's pages
 * with PAGE_GUARD; on Map we disarm (a mapped access is legitimate, not the
 * signal). A guard fault therefore means an access to the payload WHILE UNMAPPED,
 * and the faulting instruction's module says who: our own raster reading a
 * texture to draw it (expected, uninteresting) vs the game touching a pointer it
 * kept (the raw-pointer case that crashed cross-session at rabiribi+33803). Once
 * an entry is confirmed as raster we stop re-arming it, so a per-frame dynamic
 * texture does not flood; a resource the game raw-touches is what we are hunting.
 * PAGE_GUARD is one-shot (auto-clears on trigger), so the faulting access simply
 * re-runs on a normal page - the probe reads only, it never changes behaviour.
 * Page-aligned payloads >=64 KB only (the VirtualAlloc/arena ones; small calloc
 * blocks share pages and cannot be guarded without hitting neighbours). */
#define MAPPROBE_MAX 2048
static struct mp_ent {
	uintptr_t base;
	SIZE_T size;
	int id, kind;
	volatile LONG armed; /* 1 = guard set and not yet fired */
	volatile LONG cls;   /* 0 unknown, 1 raster (ours), 2 game/other */
} g_mp[MAPPROBE_MAX];
static volatile LONG g_mp_n;
static volatile LONG g_mp_arms, g_mp_game, g_mp_raster, g_mp_other, g_mp_remap;
static HMODULE g_mp_self;
static volatile LONG g_mp_veh_on;
/* Map census (no guard pages): every Map counted by size and access, so we can
 * SEE whether the game is ever handed a pointer into a big payload at all. If
 * big maps are ~0, the big textures are immutable/handle-only and float freely -
 * the guard-page arms being 0 is then a positive result, not a miss. */
static volatile LONG g_mc_calls, g_mc_read, g_mc_big, g_mc_bigread, g_mc_aligned;

static int mapprobe_on(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		unsigned n = savestate_getenv("D3D9SW_MAPPROBE", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '1') ? 1 : 0;
	}
	return cached;
}

static struct mp_ent *mp_find(uintptr_t base)
{
	LONG i, n = g_mp_n;

	for (i = 0; i < n; i++)
		if (g_mp[i].base == base)
			return &g_mp[i];
	return NULL;
}

static LONG CALLBACK mapprobe_veh(EXCEPTION_POINTERS *ep)
{
	const EXCEPTION_RECORD *er = ep->ExceptionRecord;
	ULONG_PTR at;
	LONG i, n;

	if (er->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;
	at = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
	n = g_mp_n;
	for (i = 0; i < n; i++) {
		if (!g_mp[i].armed || at < g_mp[i].base ||
		    at >= g_mp[i].base + g_mp[i].size)
			continue;
		{
			void *pc = er->ExceptionAddress;
			HMODULE mod = NULL;
			int ours = 0, exe = 0;

			if (GetModuleHandleExA(
				    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				    (LPCSTR)pc, &mod)) {
				ours = (mod == g_mp_self);
				exe = (mod == GetModuleHandleA(NULL));
			}
			g_mp[i].armed = 0; /* PAGE_GUARD auto-cleared on this fault */
			if (ours) {
				g_mp[i].cls = 1;
				InterlockedIncrement(&g_mp_raster);
			} else {
				g_mp[i].cls = 2;
				if (exe) {
					if (InterlockedIncrement(&g_mp_game) <= 16)
						d11_log("MAPPROBE: GAME touched UNMAPPED "
							"payload #%d (%s) at %p from "
							"rabiribi.exe+%p - RAW-POINTER access",
							g_mp[i].id,
							g_mp[i].kind ? "texture" : "buffer",
							(void *)at,
							(void *)((char *)pc -
								 (char *)GetModuleHandleA(NULL)));
				} else {
					InterlockedIncrement(&g_mp_other);
				}
			}
		}
		return EXCEPTION_CONTINUE_EXECUTION; /* re-run on the now-normal page */
	}
	return EXCEPTION_CONTINUE_SEARCH; /* not ours (e.g. a real stack guard page) */
}

static void mapprobe_clear(const Sw11Res *r)
{
	struct mp_ent *e;
	uintptr_t base;
	DWORD old;

	if (!mapprobe_on() || !r || !r->cpu)
		return;
	base = (uintptr_t)r->cpu;
	e = mp_find(base);
	if (e && e->armed) {
		/* Being legitimately re-mapped: drop the guard so the mapped access
		 * does not count as a stale-pointer touch. A clean re-map with no
		 * unmapped access in between is the handle-only pattern. */
		VirtualProtect((void *)base, e->size, PAGE_READWRITE, &old);
		e->armed = 0;
		InterlockedIncrement(&g_mp_remap);
	}
}

static void mapprobe_arm(const Sw11Res *r)
{
	struct mp_ent *e;
	uintptr_t base;
	DWORD old;
	LONG i;

	if (!mapprobe_on() || !r || !r->cpu)
		return;
	base = (uintptr_t)r->cpu;
	if ((base & 0xFFF) || r->cpu_size < 0x10000)
		return; /* page-aligned, >=64 KB payloads only */
	if (!InterlockedCompareExchange(&g_mp_veh_on, 1, 0)) {
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				   (LPCSTR)&mapprobe_arm, &g_mp_self);
		AddVectoredExceptionHandler(1, mapprobe_veh);
		d11_log("MAPPROBE: armed - guard-page on Unmap, classifying "
			"unmapped-payload access as raster (ours) vs game (raw pointer)");
	}
	e = mp_find(base);
	if (e && e->cls == 1)
		return; /* confirmed raster reads this every draw - do not flood */
	if (!e) {
		i = InterlockedIncrement(&g_mp_n) - 1;
		if (i >= MAPPROBE_MAX)
			return;
		e = &g_mp[i];
		e->base = base;
		e->cls = 0;
	}
	e->size = r->cpu_size;
	e->id = r->id;
	e->kind = r->kind;
	if (VirtualProtect((void *)base, r->cpu_size, PAGE_READWRITE | PAGE_GUARD, &old)) {
		e->armed = 1;
		InterlockedIncrement(&g_mp_arms);
	}
}

/* Logged from the savestate path (ledger_register), so a tally lands beside every
 * save. game>0 proves raw-pointer access exists and how common; a high remap+
 * (never-fired) share means most payloads are handle-only and can float. */
static void mapprobe_summary(void)
{
	LONG i, n = g_mp_n, gcls = 0, rcls = 0, ucls = 0;

	if (!mapprobe_on())
		return;
	for (i = 0; i < n; i++) {
		if (g_mp[i].cls == 2) gcls++;
		else if (g_mp[i].cls == 1) rcls++;
		else ucls++;
	}
	d11_log("MAPPROBE census: %ld Map call(s) - %ld for READ, %ld big(>=128K), "
		"%ld big+READ, %ld page-aligned. If big ~0 the big textures are never "
		"mapped = handle-only = FLOATABLE",
		(long)g_mc_calls, (long)g_mc_read, (long)g_mc_big, (long)g_mc_bigread,
		(long)g_mc_aligned);
	d11_log("MAPPROBE tally: %ld arm(s); accesses while UNMAPPED - game(raw) %ld, "
		"raster(ours) %ld, other %ld; clean re-maps %ld. Distinct payloads: "
		"%ld game-touched, %ld raster-only, %ld never-touched(floatable)",
		(long)g_mp_arms, (long)g_mp_game, (long)g_mp_raster, (long)g_mp_other,
		(long)g_mp_remap, gcls, rcls, ucls);
}

/* Module names for the fault handler, gathered in advance.
 *
 * The handler cannot ask the loader who owns an address. This crash proves why:
 * it fires inside RtlFreeHeap, so the faulting thread already holds the heap
 * lock, and GetModuleFileNameA reaches the allocator and blocks on a lock it
 * can never get. The crash report becomes a hang, which is strictly worse - the
 * process is wedged and says nothing about why.
 *
 * So the table is built from the PEB loader list from a safe context and the
 * handler only reads it. Refreshed from Present rather than once at attach,
 * because modules the game loads later are the interesting ones. */
struct modent {
	uintptr_t base, end;
	char name[48];
};

static struct modent g_mods[192];
static volatile LONG g_nmods;

typedef struct SwUstr {
	USHORT Length, MaximumLength;
	PWSTR Buffer;
} SwUstr;

typedef struct SwLdrEnt {
	LIST_ENTRY InLoadOrderLinks;
	LIST_ENTRY InMemoryOrderLinks;
	LIST_ENTRY InInitializationOrderLinks;
	PVOID DllBase;
	PVOID EntryPoint;
	ULONG SizeOfImage;
	SwUstr FullDllName;
	SwUstr BaseDllName;
} SwLdrEnt;

static void mod_snapshot(void)
{
	LIST_ENTRY *head, *p;
	char *ldr;
	int n = 0;

#if defined(__i386__) || defined(_M_IX86)
	char *peb = (char *)(uintptr_t)__readfsdword(0x30);

	if (!peb)
		return;
	ldr = *(char **)(peb + 0x0C);
#else
	char *peb = (char *)(uintptr_t)__readgsqword(0x60);

	if (!peb)
		return;
	ldr = *(char **)(peb + 0x18);
#endif
	if (!ldr)
		return;
	head = (LIST_ENTRY *)(ldr + sizeof(ULONG) + sizeof(PVOID) * 2);
	for (p = head->Flink; p && p != head && n < (int)(sizeof(g_mods) / sizeof(g_mods[0]));
	     p = p->Flink) {
		SwLdrEnt *e = (SwLdrEnt *)p;
		int i;

		if (!e->DllBase || !e->SizeOfImage)
			continue;
		g_mods[n].base = (uintptr_t)e->DllBase;
		g_mods[n].end = g_mods[n].base + e->SizeOfImage;
		g_mods[n].name[0] = 0;
		for (i = 0; e->BaseDllName.Buffer && i < (int)sizeof(g_mods[n].name) - 1 &&
			    i < e->BaseDllName.Length / 2;
		     i++)
			g_mods[n].name[i] = (char)e->BaseDllName.Buffer[i];
		g_mods[n].name[i > 0 ? i : 0] = 0;
		n++;
	}
	InterlockedExchange(&g_nmods, n);
}

/* Pure table scan. Safe to call with any lock held. */
static const char *mod_name(uintptr_t v, uintptr_t *base)
{
	LONG n = g_nmods, i;

	for (i = 0; i < n; i++)
		if (v >= g_mods[i].base && v < g_mods[i].end) {
			if (base)
				*base = g_mods[i].base;
			return g_mods[i].name;
		}
	if (base)
		*base = 0;
	return NULL;
}

/* Where a value points, in one line, for a fault report.
 *
 * A stale pointer after a restore is the recurring failure here, and the fix
 * depends entirely on which side of the rewind the memory sits on: a game heap
 * that travels back, a heap deliberately left in the present, an image, or our
 * own arena. The fault tells us a pointer was wrong; it does not say where it
 * pointed, and without that the next move is guesswork.
 *
 * Allocates nothing. The handler can run with the heap lock already held by
 * this same thread, so anything that reaches the allocator turns a crash report
 * into a hang. AllocationBase is reported raw so it can be matched against the
 * heap census the savestate engine writes at save time. */
static void d11_where(ULONG_PTR v, char *out, size_t n)
{
	MEMORY_BASIC_INFORMATION mi;
	const char *state, *type;

	out[0] = 0;
	if (v < 0x10000) {
		_snprintf(out, n, "not a pointer");
		return;
	}
	if (!VirtualQuery((void *)v, &mi, sizeof(mi))) {
		_snprintf(out, n, "unmapped");
		return;
	}
	state = mi.State == MEM_COMMIT ? "COMMIT" : mi.State == MEM_RESERVE ? "RESERVE" : "FREE";
	type = mi.Type == MEM_IMAGE ? "IMAGE"
	       : mi.Type == MEM_MAPPED ? "MAPPED"
	       : mi.Type == MEM_PRIVATE ? "PRIVATE"
				       : "-";
	if (arena_owns((void *)v)) {
		_snprintf(out, n, "%s %s in OUR ARENA at +%#lx", state, type,
			  (unsigned long)(v - (ULONG_PTR)g_arena.base));
		return;
	}
	if (mi.Type == MEM_IMAGE) {
		uintptr_t mb = 0;
		const char *nm = mod_name(v, &mb);

		_snprintf(out, n, "%s IMAGE +%#lx in %s", state, (unsigned long)(v - mb),
			  nm ? nm : "?");
		return;
	}
	{
		char hn[64];
		int rw = savestate_rewinds((const void *)v, hn, sizeof(hn));

		_snprintf(out, n, "%s %s alloc_base %08lX prot %lx%s%s%s", state, type,
			  (unsigned long)(ULONG_PTR)mi.AllocationBase, (unsigned long)mi.Protect,
			  rw < 0 ? "" : rw ? " | REWOUND heap: " : " | present heap: ",
			  rw < 0 ? "" : hn, rw < 0 ? "" : "");
	}
}

/* Surviving a read that should have crashed.
 *
 * Same idea as the overrun guard in DDPR, which caught a rep movsd running past
 * a fixed-capacity buffer and set ECX to zero so the copy stopped instead of the
 * game dying. The difference is worth stating plainly, because it decides how
 * much to trust what comes out the other side. That guard was a REPAIR: we knew
 * the buffer's capacity and truncating the copy was the correct behaviour. This
 * is SURVIVAL. We do not know what corrupts the heap here, so healing a read
 * keeps the process running without making it right, and the game continues on
 * a value we invented.
 *
 * Reads only, deliberately. A healed read fabricates a zero the game then acts
 * on; a healed write would discard data the game believed it had stored, and
 * this game writes a save file. Losing a session to a crash is recoverable,
 * quietly corrupting a save is not.
 *
 * Two mechanisms, because the faults come in two shapes:
 *
 *   A page that is reserved or free, read just past something committed - the
 *   overrun signature. Commit it as zeros and let the read succeed.
 *
 *   A null dereference, which cannot be healed that way because page zero can
 *   never be committed. These are all simple loads - mov esi,[ecx+0x34] - so
 *   decode the instruction, put zero in the destination, and step over it.
 *
 * Off unless D3D11SW_HEAL is set to a number, which is also the cap: after that
 * many the guard stands down and the next fault is a real crash, so a run that
 * is failing continuously still fails visibly instead of grinding on forever. */
static volatile LONG g_heals;
static volatile LONG g_trunc_told;

static int heal_cap(void)
{
	static int v = -1;

	if (v < 0) {
		char b[16];

		v = savestate_getenv("D3D11SW_HEAL", b, sizeof(b)) ? atoi(b) : 0;
	}
	return v;
}

#if defined(__i386__) || defined(_M_IX86)
/* Zero the destination of a simple register load and step over it. Returns the
 * instruction length, or 0 if it is not a form we are sure of - in which case
 * nothing is touched and the fault stands. Being conservative is the whole
 * point: guessing an instruction's length lands the CPU mid-instruction. */
static int heal_skip_load(CONTEXT *c)
{
	const unsigned char *p = (const unsigned char *)c->Eip;
	DWORD *regs[8];
	int i = 0, modrm, mod, reg, rm, len;

	regs[0] = &c->Eax;
	regs[1] = &c->Ecx;
	regs[2] = &c->Edx;
	regs[3] = &c->Ebx;
	regs[4] = &c->Esp;
	regs[5] = &c->Ebp;
	regs[6] = &c->Esi;
	regs[7] = &c->Edi;

	if (IsBadReadPtr(p, 8))
		return 0;
	if (p[i] == 0x0F && (p[i + 1] == 0xB6 || p[i + 1] == 0xB7))
		i += 2; /* movzx r32, r/m8 or r/m16 */
	else if (p[i] == 0x8B)
		i += 1; /* mov r32, r/m32 */
	else
		return 0;
	modrm = p[i++];
	mod = modrm >> 6;
	reg = (modrm >> 3) & 7;
	rm = modrm & 7;
	if (mod == 3)
		return 0; /* register source cannot have faulted */
	if (rm == 4)
		i++; /* SIB */
	if (mod == 1)
		i += 1;
	else if (mod == 2 || (mod == 0 && rm == 5))
		i += 4;
	len = i;
	*regs[reg] = 0;
	c->Eip += len;
	return len;
}
#endif

static int heal_fault(EXCEPTION_POINTERS *ep)
{
	const EXCEPTION_RECORD *er = ep->ExceptionRecord;
	ULONG_PTR rw, at;
	MEMORY_BASIC_INFORMATION mi;
	int cap = heal_cap();

	if (cap <= 0 || er->NumberParameters < 2)
		return 0;
	rw = er->ExceptionInformation[0];
	at = er->ExceptionInformation[1];
	if (InterlockedIncrement(&g_heals) > cap) {
		static LONG said;

		if (InterlockedIncrement(&said) == 1)
			d11_log("HEAL cap of %d reached - standing down, the next fault is "
				"a real crash",
				cap);
		return 0;
	}
#if defined(__i386__) || defined(_M_IX86)
	/* A runaway string move: stop it, do not feed it.
	 *
	 * The evidence is unambiguous. Sixty-four heals, every one from the same
	 * instruction, every one a page higher than the last - 0E744000 through
	 * 0E783000 with no gaps. Decoding around it gives the CRT's memcpy, and
	 * the faulting instruction is rep movsb with ECX = FFFA47F0, which is
	 * -374800 as a length: a subtraction that ran the wrong way and was then
	 * handed over unsigned. Committing pages under a loop like that is not
	 * healing, it is catering - it would have eaten the address space a page
	 * per fault, which is exactly what the cap of 64 stopped.
	 *
	 * Truncating is the same move the DDPR overrun guard makes. Setting ECX
	 * to zero lets the rep finish immediately and execution carries on at
	 * the next instruction with a short copy, which is wrong data but bounded
	 * wrong data - and the alternative is a dead process.
	 *
	 * Both reads and writes, and checked before anything else. The first
	 * version of this gated on reads and so sat behind the read-only test
	 * that guards the commit path below; the very next fault was the same
	 * runaway seen from its writing side and sailed straight past. A rep move
	 * faults on whichever end runs out of mapping first, and neither end is
	 * more legitimate than the other. */
	{
		const unsigned char *ip = (const unsigned char *)ep->ContextRecord->Eip;

		if (!IsBadReadPtr(ip, 2) && ip[0] == 0xF3 &&
		    (ip[1] == 0xA4 || ip[1] == 0xA5 || ip[1] == 0xAA || ip[1] == 0xAB)) {
			DWORD had = ep->ContextRecord->Ecx;
			DWORD wide = (ip[1] == 0xA5 || ip[1] == 0xAB) ? 4 : 1;
			ULONG_PTR from = ep->ContextRecord->Edi;

			/* Print what is left, because added to how far the copy got
			 * it recovers the count the caller passed - and that number
			 * names the bug. The write-side fault worked out to -3: not
			 * a corrupt size field, just two counters that should have
			 * agreed and were three apart. */
			ep->ContextRecord->Ecx = 0;
			d11_log("HEAL #%ld truncated a rep %s at pc %p on a %s of %p - "
				"%ld more element(s) of %lu byte(s) to go, so the caller "
				"asked for a length that reads as negative. Short copy "
				"from here; the destination past %p keeps whatever it had",
				(long)g_heals,
				(ip[1] == 0xA4 || ip[1] == 0xA5) ? "movs" : "stos",
				er->ExceptionAddress, rw ? "write" : "read", (void *)at,
				(long)had, (unsigned long)wide, (void *)from);
			/* Who asked for it, once.
			 *
			 * Truncating means this site no longer reaches the fault
			 * reporter, so the stack walk that would have named the
			 * subsystem vanished the moment the guard started working.
			 * The copy did not invent the length, its caller did, so the
			 * bytes immediately before each return address matter more
			 * than the return address itself: that is the call site and
			 * the arithmetic feeding it. Twice, then quiet - this runs
			 * inside a fault on a thread that may hold the heap lock. */
			if (InterlockedIncrement(&g_trunc_told) <= 2) {
				static const char H[] = "0123456789ABCDEF";
				ULONG_PTR fp = ep->ContextRecord->Ebp;
				int depth;

				for (depth = 0; depth < 8 && fp; depth++) {
					const ULONG_PTR *fr = (const ULONG_PTR *)fp;
					ULONG_PTR ret;
					uintptr_t fb = 0; /* mod_name wants uintptr_t* */
					const unsigned char *c;
					const char *fn;
					char hex[80];
					int j, o = 0;

					if (IsBadReadPtr(fr, 2 * sizeof(*fr)))
						break;
					ret = fr[1];
					if (!ret)
						break;
					fn = mod_name(ret, &fb);
					c = (const unsigned char *)(ret - 24);
					if (!IsBadReadPtr(c, 24))
						for (j = 0; j < 24; j++) {
							hex[o++] = H[c[j] >> 4];
							hex[o++] = H[c[j] & 15];
							hex[o++] = ' ';
						}
					hex[o] = 0;
					d11_log("    caller[%d] %p (+%#lx in %s), 24 bytes "
						"before the return: %s",
						depth, (void *)ret, (unsigned long)(ret - fb),
						fn ? fn : "?", hex);
					if (fr[0] <= fp)
						break; /* the chain must ascend */
					fp = fr[0];
				}
			}
			return 1;
		}
	}
#endif
	if (rw != 0)
		return 0; /* only reads get a page invented for them */
	if (at >= 0x10000 && VirtualQuery((void *)at, &mi, sizeof(mi)) == sizeof(mi) &&
	    mi.State != MEM_COMMIT) {
		void *page = (void *)(at & ~(ULONG_PTR)0xFFF);
		void *got = NULL;

		if (mi.State == MEM_RESERVE) {
			got = VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE);
		} else {
			/* Free memory has to be reserved first, and reservation
			 * snaps to 64 KB - so claim the granule, then the page. */
			void *gran = (void *)(at & ~(ULONG_PTR)0xFFFF);

			if (VirtualAlloc(gran, 0x10000, MEM_RESERVE, PAGE_NOACCESS))
				got = VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE);
		}
		if (got) {
			d11_log("HEAL #%ld committed a zero page at %p for a read of %p "
				"(%s) from pc %p - the process survives on data we "
				"invented, so treat anything after this as suspect",
				(long)g_heals, page, (void *)at,
				mi.State == MEM_RESERVE ? "reserved" : "free",
				er->ExceptionAddress);
			return 1;
		}
	}
#if defined(__i386__) || defined(_M_IX86)
	if (at < 0x10000) {
		int len = heal_skip_load(ep->ContextRecord);

		if (len) {
			d11_log("HEAL #%ld null read of %p at pc %p - zeroed the "
				"destination and stepped over %d byte(s); the game now "
				"believes it loaded 0",
				(long)g_heals, (void *)at, er->ExceptionAddress, len);
			return 1;
		}
	}
#endif
	InterlockedDecrement(&g_heals); /* did not actually heal */
	return 0;
}

static LONG CALLBACK d11_veh(EXCEPTION_POINTERS *ep)
{
	static volatile LONG n;
	const EXCEPTION_RECORD *er = ep->ExceptionRecord;

	/* Mono raises first-chance exceptions constantly as ordinary control
	 * flow. Only memory faults are interesting, and only the first few. */
	if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && heal_fault(ep))
		return EXCEPTION_CONTINUE_EXECUTION;
	if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
	    InterlockedIncrement(&n) <= 8) {
		void *pc = er->ExceptionAddress;
		ULONG_PTR rw = er->NumberParameters >= 2 ? er->ExceptionInformation[0] : 0;
		ULONG_PTR at = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
		HMODULE mod = NULL;
		char name[MAX_PATH];
		name[0] = 0;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				       (LPCSTR)pc, &mod))
			GetModuleFileNameA(mod, name, sizeof(name));
		d11_log("FAULT access violation on %s of %p at pc %p (+%#llx in %s) "
			"after %ld presents",
			rw == 1 ? "write" : rw == 8 ? "execute" : "read", (void *)at, pc,
			(unsigned long long)((char *)pc - (char *)mod),
			name[0] ? name : "?", g_present_n);
		/* If the software XAudio2 is serving, the calls before the fault are
		 * the most useful thing anyone can read here - and the savestate log
		 * cannot carry them, because a death this early happens before that
		 * engine has booted. Silent when nothing has called into it. */
		xa2_sw_trace_dump(d11_log_line);
		/* A write that lands exactly on a page boundary is the signature
		 * of a walk off the end of a buffer rather than a stray pointer,
		 * so describe the neighbourhood: the committed block below the
		 * fault is the buffer that was overrun, and its size is the
		 * capacity nobody checked. The registers say how it was being
		 * written, which for a rep movs/stos names the count too. */
		if (rw != 8 && at) {
			MEMORY_BASIC_INFORMATION mi;
			/* Printed beside the fault because the first thing to rule
			 * out is the surface being smaller than the size we told
			 * the game it was: a game that walks a mapped or copied
			 * buffer by its reported dimensions runs off the end of a
			 * scaled one, and the byte count below makes that a
			 * comparison rather than a theory. */
			if (g_bb_desc[0])
				d11_log("  %s", g_bb_desc);
			{
				/* A game that ignored an E_OUTOFMEMORY minutes
				 * ago can fault anywhere afterwards, so say how
				 * much room was left even when the fault looks
				 * unrelated to us. */
				double used, freed, largest;

				va_stats(&used, &freed, &largest);
				d11_log("  live resources %ld holding %.1f MB | address space "
					"%.0f MB used, %.0f MB free, largest free block %.1f MB",
					(long)g_res_live, g_res_bytes / (1024.0 * 1024.0), used,
					freed, largest);
			}
			if (VirtualQuery((void *)at, &mi, sizeof(mi)))
				d11_log("  target page: base %p size %#llx state %s prot %#lx",
					mi.BaseAddress, (unsigned long long)mi.RegionSize,
					mi.State == MEM_COMMIT	 ? "COMMIT"
					: mi.State == MEM_RESERVE ? "RESERVE"
								  : "FREE",
					(unsigned long)mi.Protect);
			/* Whose memory was it. A buffer we handed out is our bug;
			 * the thread's own stack is the game overflowing a local
			 * array, which no amount of correctness here prevents. */
			{
				int i;
				for (i = 0; i < MAP_TRACK; i++) {
					const unsigned char *p = g_map_track[i].p;
					if (!p || at < (ULONG_PTR)p ||
					    at >= (ULONG_PTR)p + g_map_track[i].bytes + 0x1000)
						continue;
					d11_log("  lands in or just past a mapped %s #%d: "
						"base %p size %#llx bind %#x, offset %#llx",
						g_map_track[i].kind ? "texture" : "buffer",
						g_map_track[i].id, (const void *)p,
						(unsigned long long)g_map_track[i].bytes,
						(unsigned)g_map_track[i].bind,
						(unsigned long long)(at - (ULONG_PTR)p));
				}
#if defined(__i386__) || defined(_M_IX86)
				MEMORY_BASIC_INFORMATION ms;
				if (VirtualQuery((void *)(ULONG_PTR)ep->ContextRecord->Esp, &ms,
						 sizeof(ms)) &&
				    VirtualQuery((void *)(at - 1), &mi, sizeof(mi)) &&
				    ms.AllocationBase == mi.AllocationBase)
					d11_log("  this is the faulting thread's own stack "
						"(allocation base %p): the game overflowed a "
						"local array, not memory we provided",
						ms.AllocationBase);
#endif
			}
			if (VirtualQuery((void *)(at - 1), &mi, sizeof(mi)))
				d11_log("  block below: base %p size %#llx state %s prot %#lx "
					"(overrun %#llx past its start)",
					mi.BaseAddress, (unsigned long long)mi.RegionSize,
					mi.State == MEM_COMMIT	 ? "COMMIT"
					: mi.State == MEM_RESERVE ? "RESERVE"
								  : "FREE",
					(unsigned long)mi.Protect,
					(unsigned long long)(at - (ULONG_PTR)mi.BaseAddress));
#if defined(__i386__) || defined(_M_IX86)
			d11_log("  eax=%08lX ecx=%08lX edx=%08lX ebx=%08lX esi=%08lX edi=%08lX "
				"esp=%08lX ebp=%08lX",
				ep->ContextRecord->Eax, ep->ContextRecord->Ecx,
				ep->ContextRecord->Edx, ep->ContextRecord->Ebx,
				ep->ContextRecord->Esi, ep->ContextRecord->Edi,
				ep->ContextRecord->Esp, ep->ContextRecord->Ebp);
#endif
#if defined(__i386__) || defined(_M_IX86)
			/* The arguments this frame was called with, and where each
			 * one points. When the faulting code is a two-line getter
			 * walking from an object to one of its fields, the object
			 * is arg0, and its region is the whole answer. */
			{
				const ULONG_PTR *a = (const ULONG_PTR *)(ep->ContextRecord->Ebp + 8);
				char w[220];
				int k;

				if (!IsBadReadPtr(a, 5 * sizeof(*a)))
					for (k = 0; k < 5; k++) {
						if (!a[k])
							continue;
						d11_where(a[k], w, sizeof(w));
						d11_log("  arg[%d] = %08lX  %s", k, (unsigned long)a[k],
							w);
					}
				/* Every register, because the pointer that was wrong
				 * is rarely the one that faulted. Here dsound loaded
				 * a null out of [edx] and died on [ecx+0x34]: ecx is
				 * the symptom, edx is the evidence, and whether that
				 * memory rewinds or stays in the present is the whole
				 * question. */
				{
					static const char *const rn[] = { "eax", "ecx", "edx",
									  "ebx", "esi", "edi" };
					const DWORD rv[] = { ep->ContextRecord->Eax,
							     ep->ContextRecord->Ecx,
							     ep->ContextRecord->Edx,
							     ep->ContextRecord->Ebx,
							     ep->ContextRecord->Esi,
							     ep->ContextRecord->Edi };
					int q;

					for (q = 0; q < 6; q++) {
						if (rv[q] < 0x10000)
							continue;
						d11_where((ULONG_PTR)rv[q], w, sizeof(w));
						d11_log("  %s -> %s", rn[q], w);
					}
				}
				d11_where(at, w, sizeof(w));
				d11_log("  fault address -> %s", w);
			}
#endif
			if (!IsBadReadPtr((char *)pc - 8, 24)) {
				const unsigned char *q = (const unsigned char *)pc - 8;
				d11_log("  bytes at pc: %02X %02X %02X %02X %02X %02X %02X %02X | "
					"%02X %02X %02X %02X %02X %02X %02X %02X",
					q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8],
					q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
			}
			/* Who called it. The loop that faults is bounded by a total
			 * it was handed, so the caller is where that total came
			 * from and the callee cannot explain itself. These frames
			 * keep a frame pointer, so walking ebp is reliable here and
			 * gives the chain rather than a scan of plausible-looking
			 * stack words. */
#if defined(__i386__) || defined(_M_IX86)
			{
				ULONG_PTR fp = ep->ContextRecord->Ebp;
				int depth;

				for (depth = 0; depth < 8 && fp; depth++) {
					const ULONG_PTR *frame = (const ULONG_PTR *)fp;
					ULONG_PTR ret;
					uintptr_t fb = 0; /* mod_name wants uintptr_t* */
					const char *fn;

					if (IsBadReadPtr(frame, 2 * sizeof(*frame)))
						break;
					ret = frame[1];
					if (!ret)
						break;
					/* Table lookup, not the loader: this walk
					 * runs with the heap lock held whenever the
					 * fault is inside the allocator. */
					fn = mod_name(ret, &fb);
					d11_log("  frame[%d] return %p (+%#llx in %s)", depth,
						(void *)ret, (unsigned long long)(ret - fb),
						fn ? fn : "?");
					if (frame[0] <= fp)
						break; /* chain must ascend */
					fp = frame[0];
				}
			}
#endif
			/* The whole loop, not just the instruction that faulted.
			 *
			 * Sixteen bytes say what the write was; they do not say what
			 * bounded it, and the bound is the entire question - whether
			 * this buffer has a fixed capacity the game exceeded, or a
			 * computed one that we influenced. Reading it needs the loop
			 * disassembled, and the executable cannot be disassembled from
			 * disk because the Steam wrapper ships it encrypted. In here
			 * it is plaintext, so dump it and read it offline. */
			{
				const unsigned char *base = (const unsigned char *)pc - 0x80;
				char hex[3 * 32 + 1];
				int row, col;

				for (row = 0; row < 8; row++) {
					const unsigned char *q = base + row * 32;
					if (IsBadReadPtr(q, 32))
						continue;
					for (col = 0; col < 32; col++)
						_snprintf(hex + col * 3, 4, "%02X ", q[col]);
					d11_log("  code +%#llx: %s",
						(unsigned long long)((const char *)q - (char *)mod),
						hex);
				}
			}
		}
#if defined(__i386__) || defined(_M_IX86)
		/* Executing at a bad address says where it landed and nothing about
		 * who sent it there, and a null call is entirely a question of who.
		 * The answer is already on the stack: call pushes the return address
		 * and only then faults on the destination, so the top of the stack
		 * is the instruction after the call. Report it with its module, and
		 * a few more stack slots after it - a vtable dispatch through a null
		 * slot leaves the object pointer just above the return address. */
		if (rw == 8 && ep->ContextRecord) {
			const void **sp = (const void **)ep->ContextRecord->Esp;
			int i;
			for (i = 0; i < 6; i++) {
				const void *v;
				HMODULE m = NULL;
				char mn[MAX_PATH];
				if (IsBadReadPtr(sp + i, sizeof(*sp)))
					break;
				v = sp[i];
				mn[0] = 0;
				if (v &&
				    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
						       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						       (LPCSTR)v, &m))
					GetModuleFileNameA(m, mn, sizeof(mn));
				if (!mn[0]) {
					d11_log("  stack[%d] = %p", i, v);
					continue;
				}
				d11_log("  stack[%d] = %p  (+%#llx in %s)", i, v,
					(unsigned long long)((char *)v - (char *)m), mn);
				/* Anything that resolves to a module is a plausible return
				 * address, and the bytes just before a return address are
				 * the call that produced it. The game's code is encrypted
				 * on disk by the Steam wrapper and only readable here, in
				 * the process, which is the one place this can be seen.
				 *
				 * A call through a variable shows its operand inline -
				 * FF 15 gives the absolute address of the pointer that was
				 * null - which names the culprit instead of guessing at it
				 * from a list of fifty exports. */
				if (!IsBadReadPtr((char *)v - 16, 16)) {
					const unsigned char *q = (const unsigned char *)v - 16;
					d11_log("      bytes before: %02X %02X %02X %02X %02X %02X "
						"%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
						q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8],
						q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
					/* FF 15 disp32 = call dword ptr [disp32]: the operand is
					 * the pointer that was called, so read what it holds. */
					if (q[10] == 0xFF && q[11] == 0x15) {
						void **slot = *(void ***)(q + 12);
						d11_log("      -> call [%p], which holds %p", (void *)slot,
							IsBadReadPtr(slot, sizeof(*slot)) ? NULL : *slot);
					}
					/* E8 rel32 = a direct call to the game's own code, which
					 * only moves the question one frame along: that callee is
					 * where the null jump happens. Follow it and show its
					 * opening bytes, since a forwarding thunk announces itself
					 * in the first few - FF 25 is jmp dword ptr [addr], and
					 * that addr is the pointer nobody checked. */
					if (q[11] == 0xE8) {
						const unsigned char *t =
							(const unsigned char *)v + *(const int *)(q + 12);
						d11_log("      -> direct call to %p", (const void *)t);
						if (!IsBadReadPtr(t, 32)) {
							char hex[3 * 32 + 1];
							int k;
							for (k = 0; k < 32; k++)
								_snprintf(hex + k * 3, 4, "%02X ", t[k]);
							d11_log("         callee bytes: %s", hex);
							for (k = 0; k < 26; k++) {
								if (t[k] == 0xFF && t[k + 1] == 0x25) {
									void **slot = *(void ***)(t + k + 2);
									d11_log("         -> jmp [%p], which holds "
										"%p",
										(void *)slot,
										IsBadReadPtr(slot, sizeof(*slot))
											? NULL
											: *slot);
									break;
								}
							}
						}
					}
				}
			}
		}
#endif
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

/* Every knob here is an environment variable, and this game relaunches itself
 * when it is started from a shell, which is the one place those are convenient
 * to set. Seeding the environment from a file beside the DLL makes them reachable
 * from an ordinary Steam launch. A real environment variable still wins, so a
 * shell run can override the file. */
/* Set a knob unless it already has a value, and report whether it took.
 *
 * A process has two environments and the knobs are split across both.
 * SetEnvironmentVariable updates the block the Win32 API reads; the C runtime
 * keeps a separate copy, taken once at startup, and that is what getenv
 * answers from. Setting only the first meant every knob read through getenv -
 * which is all the render ones - stayed at its default no matter what was
 * configured, while the savestate ones read through GetEnvironmentVariable and
 * worked. That asymmetry looked like the settings being wrong rather than
 * unread.
 *
 * The pair string is kept alive because putenv is documented to copy it on
 * some runtimes and to retain it on others, and a static pool avoids handing
 * the runtime a pointer from our own allocator for it to free with a different
 * one. */
static int env_default(const char *key, const char *val)
{
	static char pool[32][192];
	static int npool;
	char probe[4];
	size_t kl, vl;

	if (GetEnvironmentVariableA(key, probe, sizeof(probe)) != 0 ||
	    GetLastError() != ERROR_ENVVAR_NOT_FOUND)
		return 0; /* already set, and an explicit setting always wins */

	kl = strlen(key);
	vl = strlen(val);
	SetEnvironmentVariableA(key, val);
	if (npool < (int)(sizeof(pool) / sizeof(pool[0])) &&
	    kl + vl + 2 <= sizeof(pool[0])) {
		char *pair = pool[npool++];

		memcpy(pair, key, kl);
		pair[kl] = '=';
		memcpy(pair + kl + 1, val, vl + 1);
		_putenv(pair);
	}
	return 1;
}

static void config_seed_env(HINSTANCE self)
{
	char path[MAX_PATH], line[512];
	static const char name[] = "d3d11_sw.cfg";
	char *slash;
	DWORD n = GetModuleFileNameA(self, path, sizeof(path));
	FILE *f;
	int applied = 0;

	if (!n || n >= sizeof(path))
		return;
	slash = strrchr(path, '\\');
	if (!slash || (size_t)(slash - path) + sizeof(name) + 1 > sizeof(path))
		return;
	memcpy(slash + 1, name, sizeof(name));
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *key = line, *val, *eq, *end;
		while (*key == ' ' || *key == '\t')
			key++;
		if (*key == '#' || *key == ';' || *key == '\r' || *key == '\n' || !*key)
			continue;
		eq = strchr(key, '=');
		if (!eq)
			continue;
		*eq = 0;
		val = eq + 1;
		for (end = eq; end > key && (end[-1] == ' ' || end[-1] == '\t'); end--)
			end[-1] = 0;
		while (*val == ' ' || *val == '\t')
			val++;
		for (end = val + strlen(val);
		     end > val && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' ||
				   end[-1] == '\t');
		     end--)
			end[-1] = 0;
		if (!*key)
			continue;
		applied += env_default(key, val);
	}
	fclose(f);
	/* Echoed through the same call the consumers use, so a knob that failed to
	 * arrive is visible here rather than inferred from behaviour later. */
	d11_log("config: %d setting(s) applied from %s | sharpen=%s threads=%s drift=%s "
		"point_mag=%s gather=%s",
		applied, name, getenv("D3D11SW_TEXT_SHARPEN") ? getenv("D3D11SW_TEXT_SHARPEN") : "-",
		getenv("D3D9SW_THREADS") ? getenv("D3D9SW_THREADS") : "-",
		getenv("D3D9SW_DRIFT") ? getenv("D3D9SW_DRIFT") : "-",
		getenv("D3D11SW_POINT_MAG") ? getenv("D3D11SW_POINT_MAG") : "-",
		getenv("D3D9SW_GATHER") ? getenv("D3D9SW_GATHER") : "-");
}

/* Settings a specific game is known to need.
 *
 * The render-target scaler is the largest lever there is over frame time, but
 * it cannot be on by default: it trades picture for speed, the right cap is a
 * property of the individual game, and a title that already runs at full rate
 * would only lose sharpness. Asking every player to find a config file is not
 * much better, since the game is unplayable until they do.
 *
 * So carry the answer for the games that have been measured, keyed on the
 * executable. This is deliberately the weakest form of configuration in the
 * stack: the environment wins over d3d11_sw.cfg, which wins over this, so a
 * profile only ever supplies a value nobody else had an opinion about, and
 * anyone who disagrees with one can override it without editing the table.
 *
 * Rabi-Ribi draws its whole frame into the backbuffer - 518 draws into one
 * 1280x720 target, shading 17.1 Mpx per frame for 0.92 Mpx of output, an 18.6x
 * overdraw that put raster at 31 ms of a 40 ms frame. Halving each axis quarters
 * that. 360 is chosen over any other cap because 640x360 is an exact 2x of the
 * 720p window, so the nearest-neighbour upscale in present lands every texel on
 * a clean 2x2 block, which suits the pixel art rather than fighting it. */
struct game_profile {
	const char *exe; /* lowercase leaf name */
	const char *key;
	const char *val;
};

static const struct game_profile g_profiles[] = {
	/* The title this heuristic was written for. It sets a canvas-sized
	 * scissor against a much larger backbuffer, so it keeps the old
	 * behaviour while everyone else gets honest clipping. */
	{ "osfe.exe", "D3D11SW_SCISSOR", "0" },
	{ "rabiribi.exe", "D3D11SW_SCALE_RT", "1" },
	{ "rabiribi.exe", "D3D11SW_MAX_HEIGHT", "360" },
	/* It opens on whichever monitor it feels like regardless of what DXGI
	 * reports, so put it on the primary unless told otherwise. */
	{ "rabiribi.exe", "D3D11SW_MONITOR", "0" },
	{ NULL, NULL, NULL },
};

/* D3D11SW_MUL_IDENTITY=0 disables the multiply-blend identity fade described at
 * its use site. Default on: without it a transparent source under a multiply
 * erases the destination to black instead of leaving it alone. */
static int mul_identity(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[8];
		unsigned n = savestate_getenv("D3D11SW_MUL_IDENTITY", buf, sizeof(buf));

		v = (n && buf[0] == '0') ? 0 : 1;
	}
	return v;
}

/* D3D11SW_BREAK_BLEND deliberately corrupts blending, to find out which draws
 * are responsible for pixels we cannot otherwise account for.
 *
 * Reading the code has produced four dead theories, because inspection can only
 * confirm what a mechanism would do, never what it is doing. Perturbation asks
 * the opposite question: change one rule and see which pixels move. A draw that
 * stops painting when a rule is broken was obeying that rule.
 *
 *   1  every blend becomes additive (ONE/ONE). Nothing can darken the target,
 *      so any rectangle that is still black is not being produced by blending.
 *   2  neutralise only multiply-style draws (source ZERO or destination
 *      SRCCOLOR) by masking off their colour writes. The frame stays otherwise
 *      intact, so if the boxes vanish they are exactly these draws.
 *   3  neutralise every blended draw. The blunt version, for when 2 says no.
 *
 * All three are diagnostics and all three make the game look wrong on purpose. */
/* D3D11SW_EXP_TRANSP asks what transparency costs, by removing it:
 *
 *   1  skip every blended draw outright. The ceiling on what sprites and
 *      their transparent area cost; opaque parts of them vanish too.
 *   2  draw everything opaque, so nothing reads the destination. Every
 *      pixel is still sampled, so against 0 this is the cost of blending
 *      alone, not of transparent area.
 *
 * Re-read every 120 presents so it can be flipped while the game runs. The
 * picture is wrong on purpose under both. */
static volatile LONG g_exp_transp;

static void exp_transp_refresh(void)
{
	static const char key[] = "D3D11SW_EXP_TRANSP";
	char path[MAX_PATH], line[256], *slash;
	HMODULE self = NULL;
	DWORD n;
	FILE *f;
	LONG v = 0;

	/* Straight from the file each time: the environment route is read once
	 * and memoised, which is right for every other knob and wrong for this. */
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			       (LPCSTR)(void *)exp_transp_refresh, &self))
		return;
	n = GetModuleFileNameA(self, path, sizeof(path));
	if (!n || n >= sizeof(path) || !(slash = strrchr(path, '\\')) ||
	    (size_t)(slash - path) + sizeof("d3d11_sw.cfg") + 1 > sizeof(path))
		return;
	memcpy(slash + 1, "d3d11_sw.cfg", sizeof("d3d11_sw.cfg"));
	f = fopen(path, "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			char *p = line;

			while (*p == ' ' || *p == '\t')
				p++;
			if (strncmp(p, key, sizeof(key) - 1) != 0)
				continue;
			p += sizeof(key) - 1;
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p++ != '=')
				continue;
			while (*p == ' ' || *p == '\t')
				p++;
			v = (LONG)(*p - '0');
		}
		fclose(f);
	}
	if (v < 0 || v > 2)
		v = 0;
	if (v != g_exp_transp)
		d11_log("D3D11SW_EXP_TRANSP now %ld (%s)", (long)v,
			v == 1 ? "blended draws skipped" : v == 2 ? "blending off" : "normal");
	g_exp_transp = v;
}

static int break_blend(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[8];
		unsigned n = savestate_getenv("D3D11SW_BREAK_BLEND", buf, sizeof(buf));

		v = n ? (buf[0] - '0') : 0;
		if (v < 0 || v > 3)
			v = 0;
	}
	return v;
}

/* 1 (default) clips exactly where the game asked. 0 restores the size
 * heuristic described at the use site. */
static int scissor_honour(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[8];
		unsigned n = savestate_getenv("D3D11SW_SCISSOR", buf, sizeof(buf));

		v = (n && buf[0] == '0') ? 0 : 1;
	}
	return v;
}

static void profile_seed_env(void)
{
	char path[MAX_PATH], exe[64];
	const char *leaf;
	DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
	size_t i;
	int applied = 0;

	if (!n || n >= sizeof(path))
		return;
	leaf = strrchr(path, '\\');
	leaf = leaf ? leaf + 1 : path;
	if (strlen(leaf) >= sizeof(exe))
		return;
	for (i = 0; leaf[i]; i++)
		exe[i] = (char)((leaf[i] >= 'A' && leaf[i] <= 'Z') ? leaf[i] + 32 : leaf[i]);
	exe[i] = 0;

	for (i = 0; g_profiles[i].exe; i++) {
		if (strcmp(g_profiles[i].exe, exe) != 0)
			continue;
		if (env_default(g_profiles[i].key, g_profiles[i].val)) {
			d11_log("profile %s: %s=%s", exe, g_profiles[i].key,
				g_profiles[i].val);
			applied++;
		} else
			d11_log("profile %s: %s already set, leaving it alone", exe,
				g_profiles[i].key);
	}
	if (!applied)
		d11_log("no profile settings applied for %s", exe);
}

/* Watch what the game asks for by name.
 *
 * We answer to d3d11.dll and dxgi.dll while exporting five of the fifty-one
 * names and three of the twenty that the real ones do. Anything that resolves a
 * function by name gets a null back for the rest, and code that does not check
 * calls it - which is a jump to address zero, exactly the crash being chased,
 * with no record of what was wanted.
 *
 * Recording it in the process beats catching it in a debugger: the executable
 * is encrypted on disk by the Steam wrapper, so a breakpoint set before it runs
 * has nothing to attach to, while by the time this DLL is loaded the real code
 * is present and the import table is already built.
 *
 * The patch is to the game's own import table, not to kernel32, so nothing
 * outside this process is touched and it disappears when the process does. */
static FARPROC(WINAPI *real_gpa)(HMODULE, LPCSTR);

static FARPROC WINAPI hook_gpa(HMODULE mod, LPCSTR name)
{
	FARPROC r = real_gpa(mod, name);
	/* An ordinal request has no name to print and is not what we are here
	 * for. The high bits being clear is how the loader tells them apart. */
	if (!((ULONG_PTR)name >> 16))
		return r;
	if (!r) {
		char mn[MAX_PATH];
		const char *leaf;
		mn[0] = 0;
		GetModuleFileNameA(mod, mn, sizeof(mn));
		leaf = strrchr(mn, '\\');
		d11_log("GetProcAddress MISS: %s in %s", name, leaf ? leaf + 1 : mn);
	}
	return r;
}

static void hook_getprocaddress(void)
{
	HMODULE exe = GetModuleHandleA(NULL);
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)exe;
	const IMAGE_NT_HEADERS *nt;
	const IMAGE_IMPORT_DESCRIPTOR *imp;
	DWORD rva;
	FARPROC target;

	if (!exe || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return;
	nt = (const IMAGE_NT_HEADERS *)((const char *)exe + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return;
	rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
	if (!rva)
		return;
	target = GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcAddress");
	if (!target)
		return;
	real_gpa = (FARPROC(WINAPI *)(HMODULE, LPCSTR))target;

	for (imp = (const IMAGE_IMPORT_DESCRIPTOR *)((const char *)exe + rva); imp->Name; imp++) {
		FARPROC *thunk = (FARPROC *)((char *)exe + imp->FirstThunk);
		for (; *thunk; thunk++) {
			DWORD old;
			if (*thunk != target)
				continue;
			if (!VirtualProtect(thunk, sizeof(*thunk), PAGE_READWRITE, &old))
				continue;
			*thunk = (FARPROC)hook_gpa;
			VirtualProtect(thunk, sizeof(*thunk), old, &old);
			d11_log("watching GetProcAddress for missed lookups");
			return;
		}
	}
	d11_log("GetProcAddress not found in the executable's imports; "
		"if it resolves names it does so some other way");
}

/* Claim the name dsound.dll before anything else does.
 *
 * The same-folder stub is the right mechanism and it demonstrably works: a probe
 * executable placed in the game directory gets our copy from
 * LoadLibraryA("DSound.DLL"), loads it, and resolves DirectSoundCreate8 out of
 * it. Inside the real process it lost anyway, which leaves only one explanation
 * - by the time DxLib asked for the name, a dsound.dll was already in the
 * loader's list, and a plain name matches a loaded module before any directory
 * is searched. Steam puts six of its own DLLs in here before the game runs.
 *
 * So ask first. If the name is unclaimed we take it, and DxLib's later request
 * resolves to the module we already loaded regardless of where it sits on disk.
 * If somebody beat us to it there is nothing to be done from here, and the log
 * says so plainly rather than leaving another silent no-op to be discovered by
 * its absence.
 *
 * Loading from DllMain is normally worth avoiding. This particular DLL imports
 * nothing but kernel32, its attach handler only calls
 * DisableThreadLibraryCalls, and it starts no threads, so there is no second
 * lock for it to want. */
int ds_sw_take_over(void);
int gameheap_install(void);

static void dsound_claim(void)
{
	wchar_t path[MAX_PATH], *slash;
	char shown[MAX_PATH];
	HMODULE self = NULL, h;

	h = GetModuleHandleA("dsound.dll");
	if (h) {
		int took;

		if (!GetModuleFileNameA(h, shown, MAX_PATH))
			lstrcpynA(shown, "(no path)", MAX_PATH);
		/* The software device is silent by design (see ds_sw.c: "Phase one is
		 * silent"), so taking the entry point means no sound whatever the
		 * launch flags say. On Wine that hides the whole Pulse/ALSA/PipeWire
		 * path, which a native Linux build has to survive anyway. 0 leaves
		 * Wine's DirectSound in place and lets the mixer actually run. */
		{
			char v[8];
			unsigned got = savestate_getenv("D3D9SW_DSTAKEOVER", v, sizeof(v));

			if (got > 0 && got < sizeof(v) && v[0] == '0') {
				d11_log("dsound: already loaded from %s, and D3D9SW_DSTAKEOVER=0 "
					"- leaving its create entry point alone, so the game "
					"gets the real mixer and we only hook its buffers",
					shown);
				return;
			}
		}
		/* Second to the name, so take the entry point instead. Which file won
		 * stops mattering once DirectSoundCreate8 lands in our code. */
		took = ds_sw_take_over();
		d11_log("dsound: already loaded before we attached, from %s - %s", shown,
			took ? "so we took its create entry point(s) instead; the game "
			       "will get a software device"
			     : "and its create entry point could not be taken either, so "
			       "the game gets Windows' DirectSound");
		return;
	}
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCWSTR)dsound_claim, &self);
	if (!self || !GetModuleFileNameW(self, path, MAX_PATH))
		return;
	slash = wcsrchr(path, L'\\');
	if (!slash)
		return;
	/* Off unless asked for. With the XAudio2 stand-in under test, claiming the
	 * DirectSound name as well puts two of our audio implementations in one
	 * process and makes a crash ambiguous about which one caused it - which is
	 * exactly what happened on the run that found the vtable slot error. The
	 * DirectSound side has never been shown to serve a device anyway. */
	{
		char v[8];
		DWORD got = GetEnvironmentVariableA("D3D9SW_DSCLAIM", v, sizeof(v));

		if (!(got > 0 && got < sizeof(v) && v[0] == '1')) {
			d11_log("dsound: the name is free, but not claiming it - "
				"D3D9SW_DSCLAIM is not set, and one audio stand-in at a "
				"time is the only way a crash names its own cause");
			return;
		}
	}
	wcscpy(slash + 1, L"dsound.dll");
	h = LoadLibraryW(path);
	if (!h) {
		d11_log("dsound: the stub next to us would not load (error %lu), so the "
			"game will get Windows' DirectSound",
			GetLastError());
		return;
	}
	if (!GetModuleFileNameA(h, shown, MAX_PATH))
		lstrcpynA(shown, "(no path)", MAX_PATH);
	d11_log("dsound: claimed the name first, from %s - DxLib's LoadLibrary will "
		"resolve to this one",
		shown);
}

static void gh_shutdown(void);

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	/* Announce the attach so that a missing detach line is evidence of
	 * TerminateProcess (which runs neither handler) rather than evidence
	 * that this hook was never wired up in the first place. */
	if (reason == DLL_PROCESS_ATTACH) {
		g_veh_tok = AddVectoredExceptionHandler(1, d11_veh);
		d11_log("process attach, fault logger armed");
		config_seed_env(inst);
		arena_probe();
		/* At attach, which is the whole point: the free hole the probe
		 * reports shrinks as the game loads, so this is the moment when
		 * there is most of it to claim. */
		arena_init();
		profile_seed_env();
		hook_getprocaddress();
		dsound_claim();
	} else if (reason == DLL_PROCESS_DETACH) {
		d11_log("process detach (%s) after %ld presents",
			reserved ? "process exiting" : "FreeLibrary", g_present_n);
		gh_shutdown();
	}
	return TRUE;
}

static void d11_ni(const char *name)
{
	static char seen[96][48];
	static int n;
	int i;
	for (i = 0; i < n; i++)
		if (strcmp(seen[i], name) == 0)
			return;
	if (n < 96) {
		strncpy(seen[n], name, 47);
		seen[n][47] = 0;
		n++;
	}
	d11_log("E_NOTIMPL %s", name);
}

/* Named at the point of use rather than declared up front, so this list cannot
 * drift out of step with the vtables it describes. */
struct vtbl_name {
	const char *name;
	const void *vtbl;
};
static const struct vtbl_name g_vtbl_names[];

/* The filler for every slot no method was assigned to.
 *
 * Worth being honest about what this is on 32-bit. WINAPI is __stdcall, where
 * the callee pops the arguments, and this one pops the single argument it
 * declares - so standing in for a method that takes three leaves the caller's
 * stack eight bytes adrift. On x64 the caller cleans up and the same stub is
 * exact, which is why it has never mattered before now.
 *
 * It is still better than the alternative. An unfilled slot is a call to
 * address zero, which is an immediate crash with nothing to show for it; this
 * at least names the interface on the way past, and a game that checks the
 * HRESULT may simply carry on. The real fix for any slot that shows up in the
 * log is to implement the method, not to improve the stub. */
static HRESULT WINAPI hr_ni(void *this)
{
	const char *which = "unknown interface";
	if (this) {
		const void *v = *(void **)this;
		int i;
		for (i = 0; g_vtbl_names[i].name; i++) {
			if (g_vtbl_names[i].vtbl == v) {
				which = g_vtbl_names[i].name;
				break;
			}
		}
	}
	d11_ni(which);
	return E_NOTIMPL;
}

/* Every slot of every vtable gets a stub before the real methods are assigned
 * over the top. Four of these were being prefilled and sixteen were not, which
 * is invisible until a game calls one of the sixteen - Rabi-Ribi does, and got
 * a jump to address zero before its first present. */
/* A stub has to pop the argument bytes the real method would.
 *
 * COM methods are stdcall, so the callee clears the arguments. One stub taking
 * a single argument therefore pops four bytes no matter how many the caller
 * pushed, and everything above that stays behind. The caller's epilogue then
 * reads its saved registers and its return address from the wrong slots and
 * returns to whatever happened to be there, which for a call passing three
 * nulls is a jump to address zero, a long way from the stub that caused it.
 *
 * So there is a stub per argument count, and kArity says which one each slot
 * needs. Ten is the widest method in the interfaces we implement. */
typedef void *VA;
#define NI_STUB(n, params) \
	static HRESULT WINAPI ni_##n params { return hr_ni(self); }
NI_STUB(1, (void *self))
NI_STUB(2, (void *self, VA a))
NI_STUB(3, (void *self, VA a, VA b))
NI_STUB(4, (void *self, VA a, VA b, VA c))
NI_STUB(5, (void *self, VA a, VA b, VA c, VA d))
NI_STUB(6, (void *self, VA a, VA b, VA c, VA d, VA e))
NI_STUB(7, (void *self, VA a, VA b, VA c, VA d, VA e, VA f))
NI_STUB(8, (void *self, VA a, VA b, VA c, VA d, VA e, VA f, VA g))
NI_STUB(9, (void *self, VA a, VA b, VA c, VA d, VA e, VA f, VA g, VA h))
NI_STUB(10, (void *self, VA a, VA b, VA c, VA d, VA e, VA f, VA g, VA h, VA i))
NI_STUB(11, (void *self, VA a, VA b, VA c, VA d, VA e, VA f, VA g, VA h, VA i, VA j))
NI_STUB(12, (void *self, VA a, VA b, VA c, VA d, VA e, VA f, VA g, VA h, VA i, VA j, VA k))
#undef NI_STUB

static void *const kNiStub[13] = { NULL,   ni_1, ni_2,  ni_3,  ni_4,	ni_5, ni_6,
				   ni_7,   ni_8, ni_9, ni_10, ni_11, ni_12 };

/* Fill every slot, then let the real methods be written over the top. The
 * count comes from the same headers the vtable struct does, so a mismatch here
 * means the two have drifted apart and the table needs regenerating. */
static void vtbl_fill_arity(void *v, size_t bytes, const unsigned char *arity,
			    size_t n, const char *what)
{
	void **s = (void **)v;
	size_t i;
	if (bytes / sizeof(void *) != n) {
		d11_log("vtable %s has %u slots but the arity table has %u; "
			"regenerate with tools/gen_vtbl_arity.py",
			what, (unsigned)(bytes / sizeof(void *)), (unsigned)n);
		if (n > bytes / sizeof(void *))
			n = bytes / sizeof(void *);
	}
	for (i = 0; i < n; i++)
		s[i] = arity[i] && arity[i] < 13 ? kNiStub[arity[i]] : (void *)hr_ni;
}

#define VTBL_FILL(v, type)                                                    \
	vtbl_fill_arity(&(v), sizeof(v), kArity_##type, sizeof(kArity_##type), \
			#type)

static int guid_eq(REFIID a, const GUID *b)
{
	return IsEqualGUID(a, b);
}

static void priv_free(SwPriv *p)
{
	while (p) {
		SwPriv *n = p->next;
		free(p->data);
		free(p);
		p = n;
	}
}

static HRESULT priv_set(SwPriv **head, REFGUID guid, UINT size, const void *data)
{
	SwPriv *p, **pp;
	if (!guid)
		return E_INVALIDARG;
	for (pp = head; *pp; pp = &(*pp)->next) {
		if (IsEqualGUID(&(*pp)->guid, guid)) {
			p = *pp;
			*pp = p->next;
			free(p->data);
			free(p);
			break;
		}
	}
	if (!data || !size)
		return S_OK;
	p = (SwPriv *)calloc(1, sizeof(*p));
	if (!p)
		return E_OUTOFMEMORY;
	p->guid = *guid;
	p->size = size;
	p->data = (unsigned char *)malloc(size);
	if (!p->data) {
		free(p);
		return E_OUTOFMEMORY;
	}
	memcpy(p->data, data, size);
	p->next = *head;
	*head = p;
	return S_OK;
}

static HRESULT priv_get(SwPriv *head, REFGUID guid, UINT *size, void *data)
{
	SwPriv *p;
	if (!guid || !size)
		return E_INVALIDARG;
	for (p = head; p; p = p->next) {
		if (!IsEqualGUID(&p->guid, guid))
			continue;
		if (!data) {
			*size = p->size;
			return S_OK;
		}
		if (*size < p->size) {
			*size = p->size;
			return DXGI_ERROR_MORE_DATA;
		}
		memcpy(data, p->data, p->size);
		*size = p->size;
		return S_OK;
	}
	return DXGI_ERROR_NOT_FOUND;
}

static void lock_dev(Sw11Device *d)
{
	if (d)
		EnterCriticalSection(&d->lock);
}

static void unlock_dev(Sw11Device *d)
{
	if (d)
		LeaveCriticalSection(&d->lock);
}

static uint32_t rgb565(unsigned c)
{
	unsigned r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
	r = (r * 255u + 15u) / 31u;
	g = (g * 255u + 31u) / 63u;
	b = (b * 255u + 15u) / 31u;
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

static void dxt_colors(unsigned c0, unsigned c1, uint32_t out[4], int dxt1)
{
	uint32_t a = rgb565(c0), b = rgb565(c1);
	out[0] = a;
	out[1] = b;
	if (!dxt1 || c0 > c1) {
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

static void decode_dxt_color(const unsigned char *src, uint32_t *dst, int dw, int dh, int bx,
			     int by, int dxt1)
{
	unsigned c0 = src[0] | ((unsigned)src[1] << 8);
	unsigned c1 = src[2] | ((unsigned)src[3] << 8);
	unsigned idx = src[4] | ((unsigned)src[5] << 8) | ((unsigned)src[6] << 16) |
		       ((unsigned)src[7] << 24);
	uint32_t col[4];
	int x, y;
	dxt_colors(c0, c1, col, dxt1);
	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++) {
			int px = bx + x, py = by + y;
			if (px >= 0 && py >= 0 && px < dw && py < dh)
				dst[py * dw + px] = col[(int)((idx >> (2 * (y * 4 + x))) & 3)];
		}
}

static void decode_dxt5_alpha(const unsigned char *src, uint32_t *dst, int dw, int dh, int bx,
			      int by)
{
	unsigned a0 = src[0], a1 = src[1], a[8];
	unsigned long long bits = 0;
	int i, x, y;
	a[0] = a0;
	a[1] = a1;
	if (a0 > a1) {
		for (i = 1; i <= 6; i++)
			a[i + 1] = (unsigned)(((7 - i) * a0 + i * a1) / 7);
	} else {
		for (i = 1; i <= 4; i++)
			a[i + 1] = (unsigned)(((5 - i) * a0 + i * a1) / 5);
		a[6] = 0;
		a[7] = 255;
	}
	for (i = 0; i < 6; i++)
		bits |= (unsigned long long)src[2 + i] << (8 * i);
	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++) {
			int px = bx + x, py = by + y;
			int sel = (int)((bits >> (3 * (y * 4 + x))) & 7);
			if (px >= 0 && py >= 0 && px < dw && py < dh)
				dst[py * dw + px] = (dst[py * dw + px] & 0x00ffffffu) | (a[sel] << 24);
		}
}

static int fmt_bc_block(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC4_TYPELESS:
	case DXGI_FORMAT_BC4_UNORM:
	case DXGI_FORMAT_BC4_SNORM:
		return 8;
	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC5_TYPELESS:
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
	case DXGI_FORMAT_BC6H_TYPELESS:
	case DXGI_FORMAT_BC6H_UF16:
	case DXGI_FORMAT_BC6H_SF16:
	case DXGI_FORMAT_BC7_TYPELESS:
	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		return 16;
	default:
		return 0;
	}
}

static int fmt_is_dxt(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
		return 1;
	default:
		return 0;
	}
}

static UINT fmt_stride(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		return 16;
	case DXGI_FORMAT_R32G32B32_FLOAT:
		return 12;
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
		return 8;
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R8G8_B8G8_UNORM:
		return 4;
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_B5G6R5_UNORM:
		return 2;
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_A8_UNORM:
	case DXGI_FORMAT_R8_UINT:
		return 1;
	default:
		return fmt_bc_block(f) ? 0 : 4;
	}
}

static UINT fmt_row_pitch(DXGI_FORMAT f, UINT w)
{
	int bs = fmt_bc_block(f);
	if (bs)
		return ((w + 3) / 4) * (UINT)bs;
	return w * fmt_stride(f);
}

static UINT fmt_size(DXGI_FORMAT f, UINT w, UINT h)
{
	int bs = fmt_bc_block(f);
	if (bs)
		return ((w + 3) / 4) * ((h + 3) / 4) * (UINT)bs;
	return fmt_row_pitch(f, w) * h;
}

static int fmt_is_depth(DXGI_FORMAT f)
{
	return f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_D32_FLOAT ||
	       f == DXGI_FORMAT_D16_UNORM || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
	       f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_R32_TYPELESS;
}

static uint32_t pack_argb(float r, float g, float b, float a)
{
	int ir, ig, ib, ia;
	if (r < 0)
		r = 0;
	if (r > 1)
		r = 1;
	if (g < 0)
		g = 0;
	if (g > 1)
		g = 1;
	if (b < 0)
		b = 0;
	if (b > 1)
		b = 1;
	if (a < 0)
		a = 0;
	if (a > 1)
		a = 1;
	ir = (int)(r * 255.0f + 0.5f);
	ig = (int)(g * 255.0f + 0.5f);
	ib = (int)(b * 255.0f + 0.5f);
	ia = (int)(a * 255.0f + 0.5f);
	return ((UINT)ia << 24) | ((UINT)ir << 16) | ((UINT)ig << 8) | (UINT)ib;
}

/* Once the rasteriser stopped dominating the frame, the remaining time had no
 * owner. These accumulate ticks per zone so the split can be read off rather
 * than guessed at; the cost is two QPC calls per draw. ZONE_RASTER time is
 * nested inside DRAW and PRESENT, so it is reported, not subtracted. */
/* ZONE_PACE is its own bucket because it used to fall outside all of them.
 * The present zone closes before present_wait runs, so every DwmFlush and
 * every Sleep in the pacer landed in the residual - which the split line then
 * labelled as the game's own code. A frame spent blocked in our pacing and a
 * frame spent inside the game read identically, and the whole question of why
 * frame time varies is exactly the question those two answer differently. */
enum { ZONE_DRAW, ZONE_DECODE, ZONE_PRESENT, ZONE_PACE, ZONE_HOOKS, ZONE_N };

/* Written by present_wait, read and zeroed by perf_tick. File scope rather
 * than statics inside the pacer so the report does not need a second entry
 * point into it. */
static double g_pace_dwm;
static unsigned g_pace_calls, g_pace_resets, g_pace_late;
static double g_pace_hz, g_pace_target;
static LONGLONG g_zone[ZONE_N];

/* Once the split showed 13.6 ms of a 32 ms frame inside draw but outside the
 * rasteriser, that time needed an owner too, and the geometry loop is too hot
 * for QueryPerformanceCounter - at tens of thousands of vertices a frame the
 * clock call would cost more than the work. Cycle counts are cheap enough to
 * take per draw and are reported as shares of the draw path, so they need no
 * conversion to be meaningful. Written only from the thread holding the device
 * lock. */
static LONGLONG g_tsc_draw, g_tsc_batch;
static unsigned g_n_draws, g_n_verts, g_n_tris_in, g_n_tris_out;
/* Draws whose wrap addressing was reinterpreted as clamp because their
 * coordinates never leave the texture. Counted because it is a deliberate
 * substitution, not an identity, and the number is how visible it could be. */
static unsigned g_n_npot_clamped;
/* Readbacks of rendered output, and how many were answered with the rendered
 * pixels rather than whatever the cpu plane happened to hold. These are not
 * per-frame: they run at transitions, so they accumulate for the session. */
static unsigned g_n_readback, g_n_readback_served;

int gpuprobe_run(void (*sink)(const char *));
int gpuprobe_wants_real_factory(void);
void *gpuprobe_real_factory_fn(void);

/* The probe formats its own lines and knows nothing about this file's logger. */
static void gpuprobe_sink(const char *s)
{
	d11_log("%s", s);
}

/* 0 software throughout, 1 the adapter presents the software frame, 2 the
 * adapter also draws it. Read once: the setting cannot change mid-session and
 * this is on the draw path. */
static int gpu_mode_knob(void)
{
	static int cached = -1;
	char b[8];

	if (cached < 0)
		cached = (savestate_getenv("D3D11SW_GPU", b, sizeof(b)) && b[0] >= '0' &&
			  b[0] <= '9')
				 ? b[0] - '0'
				 : 0;
	return cached;
}

/* Latched once a frame at present, so a frame is either all host or all
 * software; see rg_tick. */
static int g_rg_live;

/* D3D11SW_GPUHOST=2 is mode 3 with the adapter in another process: the draw
 * path, readback and accounting below all key off this, and fall back to
 * software the moment the host is not there. */
static int gpu_mode(void)
{
	return g_rg_live ? 3 : gpu_mode_knob();
}

/* D3D11SW_GPUHOST=1: present through gpuhost64.exe, a separate process that
 * owns the only GPU device. See gpuhost.h. Needs D3D11SW_GPU=0; the two are
 * different answers to the same question and cannot share a window.
 *
 * Kept in plain globals: this module's image is held in the present across a
 * restore, so a restore cannot bring back "host not started" and launch a
 * second one onto a window the first still owns. If that ever changes, the
 * mapping is found by name and reattached rather than recreated. */
static struct {
	int tried, dead, told_big;
	HANDLE map, ev, proc;
	GhHeader *h;
	int back;
	int32_t beat;
	DWORD beat_tick, launch_tick;
} g_gh;

/* The renderer's side of the mode-2 draw ring. */
static struct {
	uint32_t head, signalled, next_id;
	int32_t seq;
	int release; /* D3D11SW_GPUHOST_RELEASE: let go of CPU texture copies */
	unsigned draws, verts, uploads, frames, waits, readbacks;
	unsigned long long bytes, up_bytes;
} g_rg;

/* 0 off, 1 present frames through the host, 2 draw on the host. */
static int gh_mode(void)
{
	static int cached = -1;
	char b[8];

	if (cached < 0) {
		unsigned n = savestate_getenv("D3D11SW_GPUHOST", b, sizeof(b));

		cached = n && (b[0] == '1' || b[0] == '2') && gpu_mode_knob() == 0 ? b[0] - '0' : 0;
		d11_log("gpuhost: %s (D3D11SW_GPUHOST=%s, D3D11SW_GPU=%d)",
			cached == 2 ? "on, drawing on the GPU" : cached ? "on, presenting only" : "off",
			n ? b : "unset", gpu_mode_knob());
	}
	return cached;
}

static int gh_on(void)
{
	return gh_mode() != 0;
}

static int gh_remote(void)
{
	return gh_mode() == 2;
}

static int gh_scale_is(const char *want)
{
	char b[32];
	unsigned n = savestate_getenv("D3D11SW_SCALE", b, sizeof(b)), i, j;

	for (i = 0; i < n; i++) {
		for (j = 0; want[j] && i + j < n && (b[i + j] | 0x20) == want[j]; j++)
			;
		if (!want[j])
			return 1;
	}
	return 0;
}

static void gh_fail(const char *why)
{
	DWORD code = 0;
	int alive = g_gh.proc && WaitForSingleObject(g_gh.proc, 0) == WAIT_TIMEOUT;

	if (g_gh.proc && !alive)
		GetExitCodeProcess(g_gh.proc, &code);
	g_gh.dead = 1;
	g_rg_live = 0;
	d11_log("gpuhost: %s - presenting with GDI from here on | %lu ms after launch, "
		"host %s, heartbeat %ld, host pid %lu%s%s",
		why, (unsigned long)(GetTickCount() - g_gh.launch_tick),
		!g_gh.proc ? "never launched" : alive ? "still running" : "exited",
		g_gh.h ? (long)g_gh.h->heartbeat : -1L, g_gh.h ? (unsigned long)g_gh.h->host_pid : 0ul,
		g_gh.h && g_gh.h->msg[0] ? "; host said: " : "",
		g_gh.h && g_gh.h->msg[0] ? g_gh.h->msg : "");
	if (g_gh.proc && !alive)
		d11_log("gpuhost: host exit code %#lx", (unsigned long)code);
	if (g_gh.proc) {
		/* A host that is still alive but not answering keeps its swap chain
		 * on the window, which would hide every GDI frame behind it. */
		if (WaitForSingleObject(g_gh.proc, 0) == WAIT_TIMEOUT)
			TerminateProcess(g_gh.proc, 1);
	}
}

static int gh_start(HWND hwnd, int w, int h)
{
	char name[64], evname[72], path[MAX_PATH], cmd[MAX_PATH + 96], *slash;
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	HMODULE self = NULL;
	DWORD slot = ((DWORD)w * (DWORD)h * 4u + 0xffffu) & ~0xffffu;
	DWORD slots_end = GH_HEADER_BYTES + GH_SLOTS * slot;
	/* The readback area is a slot's size: it holds one backbuffer. */
	DWORD total = slots_end + (gh_remote() ? GH_RING_BYTES + slot : 0);
	int existed;

	snprintf(name, sizeof(name), "Local\\rrsw_gpuhost_%lu", (unsigned long)GetCurrentProcessId());
	snprintf(evname, sizeof(evname), "%s_f", name);
	g_gh.map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, total, name);
	existed = GetLastError() == ERROR_ALREADY_EXISTS;
	g_gh.h = g_gh.map ? (GhHeader *)MapViewOfFile(g_gh.map, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
	g_gh.ev = CreateEventA(NULL, FALSE, FALSE, evname);
	if (!g_gh.h || !g_gh.ev) {
		gh_fail("could not create the frame mapping");
		return 0;
	}
	if (existed && g_gh.h->magic == GH_MAGIC && g_gh.h->state == GH_READY && g_gh.h->host_pid) {
		g_gh.proc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, g_gh.h->host_pid);
		if (g_gh.proc && WaitForSingleObject(g_gh.proc, 0) == WAIT_TIMEOUT &&
		    g_gh.h->mode == (uint32_t)(gh_remote() ? 2 : 0)) {
			g_gh.back = g_gh.h->back;
			g_rg.head = g_rg.signalled = g_gh.h->ring_head;
			g_gh.beat = g_gh.h->heartbeat;
			g_gh.beat_tick = g_gh.launch_tick = GetTickCount();
			d11_log("gpuhost: reattached to host pid %lu", (unsigned long)g_gh.h->host_pid);
			return 1;
		}
	}
	memset(g_gh.h, 0, sizeof(*g_gh.h));
	g_gh.h->version = GH_VERSION;
	g_gh.h->header_bytes = GH_HEADER_BYTES;
	g_gh.h->slot_bytes = slot;
	g_gh.h->game_pid = GetCurrentProcessId();
	g_gh.h->hwnd = (uint64_t)(uintptr_t)hwnd;
	g_gh.h->middle = 1;
	g_gh.h->point = !gh_scale_is("linear");
	g_gh.h->integer = gh_scale_is("integer");
	if (gh_remote()) {
		char hp[8];

		g_gh.h->mode = 2;
		g_gh.h->ring_off = slots_end;
		g_gh.h->ring_bytes = GH_RING_BYTES;
		g_gh.h->rb_off = slots_end + GH_RING_BYTES;
		g_gh.h->rb_bytes = slot;
		g_gh.h->halfpixel = savestate_getenv("D3D11SW_GPU_HALFPIXEL", hp, sizeof(hp)) &&
				    hp[0] == '1';
	}
	g_gh.back = 0;
	MemoryBarrier();
	g_gh.h->magic = GH_MAGIC;

	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			       (LPCSTR)(void *)gh_start, &self) ||
	    !GetModuleFileNameA(self, path, sizeof(path)) || !(slash = strrchr(path, '\\'))) {
		gh_fail("could not find our own folder");
		return 0;
	}
	lstrcpynA(slash + 1, "gpuhost64.exe", (int)(sizeof(path) - (size_t)(slash + 1 - path)));
	snprintf(cmd, sizeof(cmd), "\"%s\" %s %lu", path, name, (unsigned long)GetCurrentProcessId());
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	if (!CreateProcessA(path, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		char why[MAX_PATH + 48];

		snprintf(why, sizeof(why), "could not launch %s (error %lu)", path,
			 (unsigned long)GetLastError());
		gh_fail(why);
		return 0;
	}
	CloseHandle(pi.hThread);
	g_gh.proc = pi.hProcess;
	g_gh.beat_tick = g_gh.launch_tick = GetTickCount();
	d11_log("gpuhost: launched pid %lu for %dx%d frames, %u KB per slot, %s%s%s", pi.dwProcessId,
		w, h, (unsigned)(slot / 1024), g_gh.h->point ? "point" : "linear",
		g_gh.h->integer ? ", integer" : "",
		gh_remote() ? ", with a 16 MB draw ring" : "");
	return 1;
}

/* Liveness, shared by both modes. Returns 1 when the host is up and
 * answering; a host found dead is failed here, once. */
static int gh_check(void)
{
	GhHeader *hd = g_gh.h;
	DWORD now = GetTickCount();

	if (g_gh.dead || !hd || !g_gh.proc)
		return 0;
	if (WaitForSingleObject(g_gh.proc, 0) != WAIT_TIMEOUT) {
		gh_fail("host process ended");
		return 0;
	}
	if (hd->state == GH_FAILED) {
		gh_fail("host failed");
		return 0;
	}
	if (hd->heartbeat != g_gh.beat) {
		g_gh.beat = hd->heartbeat;
		g_gh.beat_tick = now;
	} else if (now - g_gh.beat_tick > (hd->state == GH_READY ? 2000u : 15000u)) {
		gh_fail(hd->state == GH_READY ? "host stopped answering" : "host never came up");
		return 0;
	}
	return hd->state == GH_READY;
}

/* Returns 1 when the frame went to the host, 0 to let GDI present it. */
static int gh_present(HWND hwnd, const uint32_t *px, int w, int h)
{
	GhHeader *hd;
	uint32_t *dst;
	int y;

	if (g_gh.dead || !hwnd || w <= 0 || h <= 0)
		return 0;
	if (!g_gh.tried) {
		g_gh.tried = 1;
		if (!gh_start(hwnd, w, h))
			return 0;
	}
	/* After the launch, which stamps beat_tick: checked before it, a tick
	 * that lands inside CreateProcess makes now - beat_tick wrap to ~4e9 and
	 * the host is declared dead the moment it is born. */
	if (!gh_check())
		return 0;
	hd = g_gh.h;
	if ((size_t)w * (size_t)h * 4u > hd->slot_bytes) {
		if (!g_gh.told_big) {
			g_gh.told_big = 1;
			d11_log("gpuhost: %dx%d frame is larger than the %u-byte slots; those "
				"frames go through GDI, which the host's swap chain will hide",
				w, h, hd->slot_bytes);
		}
		return 0;
	}
	dst = GH_SLOT(hd, g_gh.back);
	for (y = 0; y < h; y++)
		memcpy(dst + (size_t)y * w, px + (size_t)y * w, (size_t)w * 4);
	hd->slot_w[g_gh.back] = (uint32_t)w;
	hd->slot_h[g_gh.back] = (uint32_t)h;
	g_gh.back = (int)(InterlockedExchange((volatile LONG *)&hd->middle, g_gh.back | GH_FRESH) & 0xff);
	hd->back = g_gh.back;
	InterlockedIncrement((volatile LONG *)&hd->published);
	SetEvent(g_gh.ev);
	return 1;
}

static void gh_shutdown(void)
{
	if (g_gh.h && !g_gh.dead) {
		g_gh.h->quit = 1;
		if (g_gh.ev)
			SetEvent(g_gh.ev);
	}
}

/* --- mode 2: the gpu.c interface, carried to the host ---
 *
 * Same calls and the same answers as the in-process backend, so the mode-3
 * code below runs unchanged; the macros after this block pick the remote one
 * while the host is live. Nothing here waits on the host except a full ring
 * and a readback, and a draw is accepted or declined on this side by the same
 * rules gpu_draw applies, so there is no round trip per draw. */

typedef struct RgTex {
	uint32_t id;
	int w, h;
	unsigned gen;
} RgTex;

typedef char rg_tri_matches[sizeof(SwTri) == sizeof(GhTri) ? 1 : -1];

/* Once a frame, at present: the frame about to start is drawn by whoever
 * this says, start to finish. */
static void rg_tick(void)
{
	static int told;
	int live = g_gh.tried && gh_check();

	if (live && !told) {
		char b[8];

		told = 1;
		g_rg.release = savestate_getenv("D3D11SW_GPUHOST_RELEASE", b, sizeof(b)) &&
			       b[0] == '1';
		d11_log("gpuhost: host is up - backbuffer draws go to the GPU from the next "
			"frame; CPU texture copies are %s",
			g_rg.release ? "RELEASED once uploaded (D3D11SW_GPUHOST_RELEASE=1): "
				       "no software fallback for them if the host dies"
				     : "kept, so the software path can take over if the host dies");
	}
	g_rg_live = live;
}

static int rg_space(uint32_t need)
{
	GhHeader *h = g_gh.h;
	DWORD t0 = 0;

	while (h->ring_bytes - (g_rg.head - h->ring_tail) < need) {
		SetEvent(g_gh.ev);
		g_rg.signalled = g_rg.head;
		if (!t0) {
			t0 = GetTickCount();
			g_rg.waits++;
		}
		if (!gh_check())
			return 0;
		if (GetTickCount() - t0 > 2000) {
			gh_fail("draw ring stayed full for 2 s");
			return 0;
		}
		Sleep(1);
	}
	return 1;
}

static void rg_write(const void *src, uint32_t n)
{
	uint8_t *ring = (uint8_t *)g_gh.h + g_gh.h->ring_off;
	uint32_t at = g_rg.head & (g_gh.h->ring_bytes - 1), first = g_gh.h->ring_bytes - at;

	if (first > n)
		first = n;
	memcpy(ring + at, src, first);
	if (n > first)
		memcpy(ring, (const uint8_t *)src + first, n - first);
	g_rg.head += n;
}

static int rg_rec(uint32_t op, const void *a, uint32_t na, const void *b, uint32_t nb)
{
	GhRec rec;

	rec.op = op;
	rec.bytes = ((uint32_t)sizeof(rec) + na + nb + 7u) & ~7u;
	if (!g_rg_live || !rg_space(rec.bytes))
		return 0;
	rg_write(&rec, sizeof(rec));
	if (na)
		rg_write(a, na);
	if (nb)
		rg_write(b, nb);
	g_rg.head += rec.bytes - (uint32_t)sizeof(rec) - na - nb;
	MemoryBarrier();
	InterlockedExchange((volatile LONG *)&g_gh.h->ring_head, (LONG)g_rg.head);
	g_rg.bytes += rec.bytes;
	/* A loading burst is megabytes of uploads before the frame's end;
	 * waking the host every megabyte lets it work through them alongside. */
	if (g_rg.head - g_rg.signalled >= GH_CHUNK_BYTES) {
		SetEvent(g_gh.ev);
		g_rg.signalled = g_rg.head;
	}
	return 1;
}

static int rg_is_up(void)
{
	return g_rg_live;
}

static int rg_tex_sync(void **slot, const uint32_t *pixels, int w, int h, unsigned gen)
{
	RgTex *t;

	if (!g_rg_live || !slot || !pixels || w <= 0 || h <= 0)
		return 0;
	t = (RgTex *)*slot;
	if (!t) {
		t = (RgTex *)calloc(1, sizeof(*t));
		if (!t)
			return 0;
		t->id = ++g_rg.next_id;
		*slot = t;
	}
	if (t->w != w || t->h != h) {
		GhTexDef d;

		d.id = t->id;
		d.w = (uint32_t)w;
		d.h = (uint32_t)h;
		d.pad = 0;
		if (!rg_rec(GH_OP_TEX_DEF, &d, sizeof(d), NULL, 0))
			return 0;
		t->w = w;
		t->h = h;
		t->gen = gen - 1;
	}
	if (t->gen != gen) {
		uint32_t pitch = (uint32_t)w * 4u, per = GH_CHUNK_BYTES / pitch, y;

		if (!per)
			per = 1;
		for (y = 0; y < (uint32_t)h; y += per) {
			GhTexRows r;

			r.id = t->id;
			r.y0 = y;
			r.rows = (uint32_t)h - y < per ? (uint32_t)h - y : per;
			r.w = (uint32_t)w;
			if (!rg_rec(GH_OP_TEX_ROWS, &r, sizeof(r), pixels + (size_t)y * w,
				    r.rows * pitch))
				return 0;
		}
		t->gen = gen;
		g_rg.uploads++;
		g_rg.up_bytes += (unsigned long long)pitch * (unsigned)h;
	}
	return 1;
}

static void rg_tex_drop(void **slot)
{
	RgTex *t = slot ? (RgTex *)*slot : NULL;

	if (!t)
		return;
	if (g_rg_live) {
		GhTexDrop d;

		d.id = t->id;
		d.pad = 0;
		rg_rec(GH_OP_TEX_DROP, &d, sizeof(d), NULL, 0);
	}
	free(t);
	*slot = NULL;
}

static int rg_frame_begin(int w, int h, int clear, uint32_t argb)
{
	GhFrameBegin f;

	if (w <= 0 || h <= 0)
		return 0;
	f.w = (uint32_t)w;
	f.h = (uint32_t)h;
	f.clear = clear ? 1u : 0u;
	f.argb = argb;
	return rg_rec(GH_OP_FRAME_BEGIN, &f, sizeof(f), NULL, 0);
}

/* gpu_draw's acceptance, in D3D9 blend terms: SRCALPHA 5, INVSRCALPHA 6,
 * ONE 2, ZERO 1, SRCCOLOR 3; ADD 1, REVSUBTRACT 3. -1 declines. */
static int rg_blend_class(const SwState *st)
{
	int s = st->src_blend, d = st->dst_blend;

	if (!st->blend_enable)
		return GH_BLEND_OFF;
	if (st->blend_op == 1) {
		if (s == 5 && d == 6)
			return GH_BLEND_OVER;
		if (s == 5 && d == 2)
			return GH_BLEND_ADD;
		if (s == 1 && d == 3)
			return GH_BLEND_MUL;
		return -1;
	}
	if (st->blend_op == 3 && s == 5 && d == 2)
		return GH_BLEND_RSUB;
	return -1;
}

static int rg_draw(const SwTri *tris, int n, void *texslot, const SwState *st)
{
	RgTex *t = (RgTex *)texslot;
	GhDraw d;
	int bc;

	if (!g_rg_live || !tris || n <= 0 || n * 3 > 16384 || !t)
		return 0;
	if (st->alpha_test && st->alpha_func != 0 && st->alpha_func != 6 && st->alpha_func != 7)
		return 0;
	bc = rg_blend_class(st);
	if (bc < 0)
		return 0;
	memset(&d, 0, sizeof(d));
	d.tex = t->id;
	d.ntri = (uint32_t)n;
	d.blend = (uint8_t)bc;
	d.bilinear = st->bilinear ? 1 : 0;
	d.mul_identity = st->mul_identity ? 1 : 0;
	d.scissor = st->scissor_enable ? 1 : 0;
	d.alpha_ref = st->alpha_test ? (float)st->alpha_ref / 255.0f : 0.0f;
	d.sharpen = st->alpha_sharpen > 0 ? (float)st->alpha_sharpen / 256.0f : 0.0f;
	d.sx0 = st->scissor_x0;
	d.sy0 = st->scissor_y0;
	d.sx1 = st->scissor_x1;
	d.sy1 = st->scissor_y1;
	if (!rg_rec(GH_OP_DRAW, &d, sizeof(d), tris, (uint32_t)n * (uint32_t)sizeof(SwTri)))
		return 0;
	g_rg.draws++;
	g_rg.verts += (unsigned)n * 3u;
	return 1;
}

static int rg_frame_end(void)
{
	if (!rg_rec(GH_OP_FRAME_END, NULL, 0, NULL, 0))
		return 0;
	SetEvent(g_gh.ev);
	g_rg.signalled = g_rg.head;
	g_rg.frames++;
	return 1;
}

static int rg_readback(uint32_t *dst, unsigned dst_pitch, int w, int h)
{
	GhHeader *hd = g_gh.h;
	GhReadback r;
	DWORD t0;
	int y;

	if (!g_rg_live || !dst || w <= 0 || h <= 0 || (size_t)w * h * 4 > hd->rb_bytes)
		return 0;
	r.w = (uint32_t)w;
	r.h = (uint32_t)h;
	r.seq = ++g_rg.seq;
	r.pad = 0;
	if (!rg_rec(GH_OP_READBACK, &r, sizeof(r), NULL, 0))
		return 0;
	SetEvent(g_gh.ev);
	g_rg.signalled = g_rg.head;
	t0 = GetTickCount();
	while (hd->rb_done != r.seq) {
		if (!gh_check())
			return 0;
		if (GetTickCount() - t0 > 2000) {
			gh_fail("readback not answered within 2 s");
			return 0;
		}
		Sleep(0);
	}
	MemoryBarrier();
	g_rg.readbacks++;
	if (!hd->rb_ok)
		return 0;
	for (y = 0; y < h; y++)
		memcpy((uint8_t *)dst + (size_t)y * dst_pitch,
		       (const uint8_t *)hd + hd->rb_off + (size_t)y * w * 4, (size_t)w * 4);
	return 1;
}

static void rg_prof_take(unsigned *uploads, unsigned *draws, unsigned *verts)
{
	*uploads = g_rg.uploads;
	*draws = g_rg.draws;
	*verts = g_rg.verts;
	g_rg.uploads = g_rg.draws = g_rg.verts = 0;
}

#define gpu_is_up() (gh_remote() ? rg_is_up() : gpu_is_up())
#define gpu_tex_sync(s, p, w, h, g) \
	(gh_remote() ? rg_tex_sync(s, p, w, h, g) : gpu_tex_sync(s, p, w, h, g))
#define gpu_tex_drop(s) (gh_remote() ? rg_tex_drop(s) : gpu_tex_drop(s))
#define gpu_frame_begin(w, h, c, a) \
	(gh_remote() ? rg_frame_begin(w, h, c, a) : gpu_frame_begin(w, h, c, a))
#define gpu_draw(t, n, s, st) (gh_remote() ? rg_draw(t, n, s, st) : gpu_draw(t, n, s, st))
#define gpu_frame_end() (gh_remote() ? rg_frame_end() : gpu_frame_end())
#define gpu_readback(d, p, w, h) (gh_remote() ? rg_readback(d, p, w, h) : gpu_readback(d, p, w, h))
#define gpu_prof_take(u, d, v) (gh_remote() ? rg_prof_take(u, d, v) : gpu_prof_take(u, d, v))

/* Open while the adapter has a frame in progress. Cleared at present, so a
 * frame that never drew anything does not try to show itself. */
static int g_gpu_open;
static uint32_t g_gpu_clear_argb = 0xff000000u;
/* Draws the backend would not honour exactly. Zero is the bar for handing the
 * frame over; anything else names a state still missing from it. */
static unsigned g_gpu_declined, g_gpu_mirrored;

/* Lets go of the CPU copy of a texture the adapter now holds.
 *
 * This is the entire point of the exercise. The arena carries up to 641 MB of
 * texture payload inside a 2 GB process, and once those bytes exist in video
 * memory the copy here is dead weight that a save has to work around.
 *
 * Only for textures the game cannot read back. A texture it can map for READ
 * has to keep its bytes, because the adapter's copy is reachable only through a
 * staging copy and a stall, and a game that reads its own textures would be
 * paying that price at a rate nobody has measured. Render targets and the
 * backbuffer are excluded for a different reason: they are outputs, and their
 * contents are produced rather than uploaded.
 *
 * A later map for WRITE is still fine - see Ctx_Map, which rebuilds the plane
 * rather than refusing. A write replaces the contents wholesale, so there is
 * nothing in the old bytes worth having kept.
 */
static unsigned g_gpu_rebuilt;
static unsigned g_gpu_uploads_total;

/* Takes a released texture back into this process, because something is about
 * to write to it.
 *
 * Every path that writes a texture's bytes has to come through here first. The
 * one that did not cost a whole run: UpdateSubresource does its work inside an
 * "if (r->cpu)", so a released texture swallowed every update it was given
 * without a word. This game fills its text atlases that way, which is why the
 * text disappeared while the map counter sat at zero.
 *
 * Rebuilt empty rather than read back from the adapter. The only textures
 * eligible for release are ones the game cannot map for READ, so this is always
 * a write, and a write replaces what was there; a staging copy would stall the
 * pipeline to recover bytes that are about to be overwritten anyway.
 */
static void res_reclaim_cpu(Sw11Res *r)
{
	size_t want;

	if (!r || r->kind != 1 || !r->gpu_released || r->cpu)
		return;
	want = fmt_size(r->format, r->width, r->height);
	r->cpu = (unsigned char *)payload_alloc(want, r, PL_RECLAIM);
	if (!r->cpu)
		return;
	r->cpu_size = (UINT)want;
	r->gpu_released = 0;
	/* And never again. A texture the game writes after we have moved it is
	 * one it intends to keep writing, and moving it back out would start a
	 * release-and-reclaim cycle that runs every frame - which is what 4964
	 * releases against 225 live resources was, and what the audio was
	 * hearing. One texture kept in memory is cheaper than that. */
	r->gpu_no_release = 1;
	res_account((long long)res_payload_bytes(r), 0);
	g_gpu_rebuilt++;
}
static unsigned g_gpu_freed_n;
static size_t g_gpu_freed_bytes;

static void gpu_release_cpu_copy(Sw11Res *r)
{
	size_t bytes;

	/* The host is a process that can die, and a texture whose only copy was
	 * in it is gone with it. Kept unless asked, so the fallback is real. */
	if (gh_remote() && !g_rg.release)
		return;
	if (!r || r->kind != 1 || r->gpu_released || r->gpu_no_release || !r->gtex)
		return;
	if (r->is_bb || (r->bind & D3D11_BIND_RENDER_TARGET))
		return;
	if (r->cpu_access & D3D11_CPU_ACCESS_READ)
		return;
	if (!r->cpu && !r->pixels)
		return;
	bytes = res_payload_bytes(r);
	res_account(-(long long)bytes, 0);
	payload_free(r->cpu, r->cpu_size);
	if (!r->pixels_alias)
		payload_free(r->pixels, (size_t)r->width * r->height * 4);
	r->cpu = NULL;
	r->pixels = NULL;
	r->pixels_alias = 0;
	r->pixels_swizzled = 0;
	r->cpu_size = 0;
	/* Distinct from a texture that never had pixels: this one has them, they
	 * are just somewhere else. Ctx_Map reads this to know a rebuild is the
	 * right answer rather than a refusal. */
	r->gpu_released = 1;
	g_gpu_freed_n++;
	g_gpu_freed_bytes += bytes;
}

static int res_ensure_pixels(Sw11Res *r);
static void res_decode_pixels_written(Sw11Res *r);

/* Moves every eligible texture to the adapter, whether or not it is on screen.
 *
 * Releasing on the draw path alone only reaches what the current scene happens
 * to be showing: the first run of this freed 51 textures of 225 and left 636 MB
 * of payload behind, because the other 174 were loaded and simply not visible
 * at that moment. They are exactly as movable, and in a 2 GB process the ones
 * nobody is looking at are the best possible thing to be rid of.
 *
 * Cheap to call repeatedly. Everything already moved fails the first test, so a
 * steady state costs one pointer chase per resource per frame.
 */
static unsigned g_gpu_swept;

static void gpu_sweep_release(void)
{
	Sw11Res *r;

	/* Without releasing, sweeping only uploads what nobody is drawing. */
	if (gh_remote() && !g_rg.release)
		return;
	if (!gpu_is_up())
		return;
	for (r = g_res_head; r; r = r->live_next) {
		if (r->kind != 1 || r->gpu_released || r->gpu_no_release || r->retired)
			continue;
		if (r->is_bb || (r->bind & D3D11_BIND_RENDER_TARGET))
			continue;
		if (r->cpu_access & D3D11_CPU_ACCESS_READ)
			continue;
		if (!r->width || !r->height)
			continue;
		/* Decoded at least once, and not merely present.
		 *
		 * gpu_gen is bumped by the decode and by nothing else, so it is the
		 * question "are these bytes in the rasteriser's channel order yet"
		 * asked in the only place that knows the answer. Some of this
		 * game's textures arrive as R8G8B8A8 and have red and blue swapped
		 * in place on first use; handing one to the adapter before that
		 * happens gives it the channels backwards, which is the wrong hue.
		 *
		 * It also happens to be the cure for the churn. The game creates
		 * and destroys about three and a half textures a frame - 69116 born
		 * against 225 live - and almost none of them are ever decoded or
		 * drawn, so previously each one bought a driver allocation and an
		 * upload on its way to being thrown away. That was 7164 uploads,
		 * and it is what the audio was hitching against. */
		if (!r->gpu_gen || !r->pixels)
			continue;
		if (!gpu_tex_sync(&r->gtex, r->pixels, (int)r->width, (int)r->height,
				  r->gpu_gen))
			continue;
		gpu_release_cpu_copy(r);
		g_gpu_swept++;
	}
}

static int gpu_shadow_draw(Sw11Res *rt, const SwTri *batch, int ntri, Sw11Res *src,
			   const SwTex *tex, const SwState *st)
{
	if (!g_gpu_open) {
		g_gpu_open = gpu_frame_begin((int)rt->width, (int)rt->height, 1,
					     g_gpu_clear_argb);
		if (!g_gpu_open)
			return 0;
	}
	/* A released texture has no CPU bytes to offer and does not need to: the
	 * adapter's copy is the only one, and it is already current. */
	/* Counted as a decline, not waved through. At mode 3 the rasteriser's
	 * output is never presented, so a draw that lands there is an invisible
	 * sprite rather than a safe fallback - and a number that does not say so
	 * is worse than no number. */
	if (!src->gpu_gen) {
		g_gpu_declined++;
		return 0;
	}
	if (!src->gpu_released &&
	    !gpu_tex_sync(&src->gtex, tex->pixels, tex->width, tex->height, src->gpu_gen)) {
		g_gpu_declined++;
		return 0;
	}
	if (!src->gtex) {
		g_gpu_declined++;
		return 0;
	}
	g_gpu_mirrored++;
	if (!gpu_draw(batch, ntri, src->gtex, st)) {
		g_gpu_declined++;
		return 0;
	}
	/* Only once the draw has actually gone through. A texture whose draw was
	 * declined may still be needed by the rasteriser, and letting go of its
	 * bytes on the strength of an upload alone would throw away the fallback
	 * at the exact moment it is wanted. */
	if (gpu_mode() >= 3)
		gpu_release_cpu_copy(src);
	return 1;
}

/* Flush time that happened inside the draw and present zones, so the rest can
 * be named as flushes triggered from elsewhere rather than being lumped in with
 * the game's own time. Without this the residual read as 30-odd ms of Unity,
 * which sent the search in the wrong direction entirely. */
static double g_raster_nested_ms;

static void res_decode_pixels_inner(Sw11Res *r);

/* Use after writing to cpu. Decoding is driven by this rather than run
 * unconditionally because an in-place conversion is not idempotent: running it
 * twice on the same bytes swaps the channels back. */
static void res_decode_pixels(Sw11Res *r);

static void res_decode_pixels_written(Sw11Res *r)
{
	if (r)
		r->cpu_dirty = 1;
	res_decode_pixels(r);
}

/* Use after copying one texture's stored bytes into another's.
 *
 * A shared plane may hold its channels in the rasteriser's order rather than
 * the game's, so the bytes that arrive are already converted exactly when the
 * source was converted too. Copying between two textures of the same format is
 * the overwhelmingly common case and both sides then agree, which needs no work
 * at all; the destination only has to be converted when the two disagree. */
static void res_after_cpu_copy(Sw11Res *d, const Sw11Res *s)
{
	if (!d || !s)
		return;
	if (d->pixels_alias && d->pixels_swizzled) {
		d->cpu_dirty = (s->pixels_swizzled == d->pixels_swizzled) ? 0 : 1;
	} else if (s->pixels_swizzled) {
		/* Converted bytes into a destination that will read them as the
		 * game's order. Not reachable for same-format copies, which is
		 * all this game does, but silence here would be a colour bug with
		 * no trail leading back to sharing. */
		static LONG said;

		if (InterlockedIncrement(&said) == 1)
			d11_log("WARNING copy from a channel-swapped texture #%d into #%d, which "
				"does not share its plane: colours in the destination will be "
				"wrong. Set D3D11SW_DEDUP=0 if this is visible",
				s->id, d->id);
		d->cpu_dirty = 1;
	} else {
		d->cpu_dirty = 1;
	}
	res_decode_pixels(d);
}

static void res_decode_pixels(Sw11Res *r)
{
	LARGE_INTEGER a, b;
	QueryPerformanceCounter(&a);
	res_decode_pixels_inner(r);
	QueryPerformanceCounter(&b);
	g_zone[ZONE_DECODE] += b.QuadPart - a.QuadPart;
}

static void res_decode_pixels_inner(Sw11Res *r)
{
	UINT x, y, w, h;
	int bs;
	if (!r || r->kind != 1 || !r->pixels || !r->cpu)
		return;
	if (r->pixels_alias) {
		UINT n, i;
		uint32_t *p;

		/* Same memory, so a verbatim decode has nothing to do. */
		if (!r->pixels_swizzled)
			return;
		/* Shared but in the wrong channel order, so convert the one copy
		 * in place. Only when something has actually written to it: this
		 * is its own inverse, and a second pass over unchanged bytes
		 * would put red and blue back the wrong way round. */
		if (!r->cpu_dirty)
			return;
		n = r->width * r->height;
		p = r->pixels;
		for (i = 0; i < n; i++) {
			uint32_t v = p[i];

			p[i] = (v & 0xff00ff00u) | ((v & 0x00ff0000u) >> 16) |
			       ((v & 0x000000ffu) << 16);
		}
		r->cpu_dirty = 0;
		r->gpu_gen++;
		return;
	}
	r->cpu_dirty = 0;
	r->gpu_gen++;
	w = r->width;
	h = r->height;
	bs = fmt_bc_block(r->format);
	if (bs) {
		UINT bx, by, nblk, maxb;
		int dxt1 = (r->format == DXGI_FORMAT_BC1_TYPELESS ||
			    r->format == DXGI_FORMAT_BC1_UNORM ||
			    r->format == DXGI_FORMAT_BC1_UNORM_SRGB);
		int is_bc7 = (r->format == DXGI_FORMAT_BC7_TYPELESS ||
			      r->format == DXGI_FORMAT_BC7_UNORM ||
			      r->format == DXGI_FORMAT_BC7_UNORM_SRGB);
		int is_bc4 = (r->format == DXGI_FORMAT_BC4_TYPELESS ||
			      r->format == DXGI_FORMAT_BC4_UNORM ||
			      r->format == DXGI_FORMAT_BC4_SNORM);
		int is_bc5 = (r->format == DXGI_FORMAT_BC5_TYPELESS ||
			      r->format == DXGI_FORMAT_BC5_UNORM ||
			      r->format == DXGI_FORMAT_BC5_SNORM);
		memset(r->pixels, 0, (size_t)w * h * 4);
		nblk = ((w + 3) / 4) * ((h + 3) / 4);
		maxb = r->cpu_size / (UINT)bs;
		if (nblk > maxb)
			nblk = maxb;
		for (by = 0; by < h; by += 4)
			for (bx = 0; bx < w; bx += 4) {
				UINT bi = (by / 4) * ((w + 3) / 4) + (bx / 4);
				const unsigned char *blk;
				int x, y;
				if (bi >= nblk)
					return;
				blk = r->cpu + bi * (UINT)bs;
				if (is_bc7) {
					unsigned char rgba[16 * 4];
					bcdec_bc7(blk, rgba, 16);
					for (y = 0; y < 4; y++)
						for (x = 0; x < 4; x++) {
							int px = (int)bx + x, py = (int)by + y;
							unsigned char *p = rgba + (y * 4 + x) * 4;
							if (px >= 0 && py >= 0 && px < (int)w && py < (int)h)
								r->pixels[py * w + px] =
									((UINT)p[3] << 24) | ((UINT)p[0] << 16) |
									((UINT)p[1] << 8) | p[2];
						}
				} else if (is_bc4) {
					unsigned char red[16];
					bcdec_bc4(blk, red, 4);
					for (y = 0; y < 4; y++)
						for (x = 0; x < 4; x++) {
							int px = (int)bx + x, py = (int)by + y;
							unsigned v = red[y * 4 + x];
							if (px >= 0 && py >= 0 && px < (int)w && py < (int)h)
								r->pixels[py * w + px] =
									0xff000000u | (v << 16) | (v << 8) | v;
						}
				} else if (is_bc5) {
					unsigned char rg[16 * 2];
					bcdec_bc5(blk, rg, 8);
					for (y = 0; y < 4; y++)
						for (x = 0; x < 4; x++) {
							int px = (int)bx + x, py = (int)by + y;
							unsigned rv = rg[(y * 4 + x) * 2];
							unsigned gv = rg[(y * 4 + x) * 2 + 1];
							if (px >= 0 && py >= 0 && px < (int)w && py < (int)h)
								r->pixels[py * w + px] =
									0xff000000u | (rv << 16) | (gv << 8);
						}
				} else if (fmt_is_dxt(r->format)) {
					if (bs == 16) {
						decode_dxt_color(blk + 8, r->pixels, (int)w, (int)h, (int)bx,
								  (int)by, 0);
						decode_dxt5_alpha(blk, r->pixels, (int)w, (int)h, (int)bx,
								  (int)by);
					} else
						decode_dxt_color(blk, r->pixels, (int)w, (int)h, (int)bx,
								  (int)by, dxt1);
				}
			}
		return;
	}
	switch (r->format) {
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
		for (y = 0; y < h; y++)
			memcpy(r->pixels + (size_t)y * w, r->cpu + (size_t)y * r->row_pitch, w * 4u);
		break;
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		for (y = 0; y < h; y++) {
			const unsigned char *s = r->cpu + (size_t)y * r->row_pitch;
			for (x = 0; x < w; x++)
				r->pixels[y * w + x] = ((UINT)s[x * 4 + 3] << 24) |
						       ((UINT)s[x * 4] << 16) | ((UINT)s[x * 4 + 1] << 8) |
						       s[x * 4 + 2];
		}
		break;
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_A8_UNORM:
		r->has_texels = 0;
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++) {
				unsigned v = r->cpu[y * r->row_pitch + x];
				r->pixels[y * w + x] = (v << 24) | 0x00ffffffu;
				if (v)
					r->has_texels = 1;
			}
		break;
	default:
		if (fmt_stride(r->format) == 4)
			for (y = 0; y < h; y++)
				memcpy(r->pixels + (size_t)y * w, r->cpu + (size_t)y * r->row_pitch,
				       w * 4u);
		break;
	}
}

/* Where the process actually went, by category and by biggest offender.
 *
 * The total alone cannot tell us whether there is anything to reclaim. A game
 * holding 800 MB of art it genuinely references is not a leak and nothing here
 * may free it - those textures belong to the game, and releasing one behind its
 * back produces a blank sprite or a crash. But 800 MB sitting in planes the
 * game never reads would be ours to fix. These are indistinguishable from the
 * total and obvious from the breakdown, so print the breakdown. */
static void res_census(void)
{
	struct {
		const Sw11Res *r;
		LONG64 bytes;
	} top[10];
	LONG64 cpu_b = 0, px_b = 0, depth_b = 0, saved_b = 0;
	LONG64 rt_b = 0, tex_b = 0, buf_b = 0;
	int n_rt = 0, n_tex = 0, n_buf = 0, ntop = 0, i;
	const Sw11Res *r;

	memset(top, 0, sizeof(top));
	res_list_lock();
	for (r = g_res_head; r; r = r->live_next) {
		LONG64 own = (LONG64)r->cpu_size;
		LONG64 pb = (r->pixels && !r->pixels_alias)
				    ? (LONG64)r->width * r->height * 4
				    : 0;
		LONG64 db = r->depth ? (LONG64)r->width * r->height * (LONG64)sizeof(float) : 0;
		LONG64 tot = own + pb + db;

		cpu_b += own;
		px_b += pb;
		depth_b += db;
		/* What sharing is worth right now, so the knob's value is a
		 * measurement rather than a claim. */
		if (r->pixels_alias)
			saved_b += (LONG64)r->width * r->height * 4;
		if (r->kind == 0) {
			buf_b += tot;
			n_buf++;
		} else if (r->bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) {
			rt_b += tot;
			n_rt++;
		} else {
			tex_b += tot;
			n_tex++;
		}
		for (i = 0; i < ntop; i++)
			if (tot > top[i].bytes)
				break;
		if (i < 10) {
			int j;

			for (j = (ntop < 10 ? ntop : 9); j > i; j--)
				top[j] = top[j - 1];
			top[i].r = r;
			top[i].bytes = tot;
			if (ntop < 10)
				ntop++;
		}
	}
	res_list_unlock();

	d11_log("census: %d render surfaces %.1f MB | %d sampled textures %.1f MB | %d buffers "
		"%.1f MB",
		n_rt, rt_b / (1024.0 * 1024.0), n_tex, tex_b / (1024.0 * 1024.0), n_buf,
		buf_b / (1024.0 * 1024.0));
	d11_log("census: planes - stored %.1f MB, decoded %.1f MB, depth %.1f MB | sharing is "
		"currently avoiding a further %.1f MB",
		cpu_b / (1024.0 * 1024.0), px_b / (1024.0 * 1024.0), depth_b / (1024.0 * 1024.0),
		saved_b / (1024.0 * 1024.0));
	for (i = 0; i < ntop; i++) {
		const Sw11Res *t = top[i].r;

		d11_log("census: #%-6d %5ux%-5u fmt=%-3d bind=%#-5x %s %6.2f MB", t->id, t->width,
			t->height, (int)t->format, (unsigned)t->bind,
			t->pixels_alias ? "shared " : "2 planes", top[i].bytes / (1024.0 * 1024.0));
	}
}

/* The live memory readout, drawn over the frame. F8 toggles it, D3D11SW_OSD=1
 * starts it on.
 *
 * This exists because the interesting failure is gradual: a 32-bit title runs
 * for minutes with everything looking fine while the address space fills, and
 * the only warning is in a log nobody reads mid-game. On screen it is visible
 * while playing, so the moment a room or a boss costs 200 MB is something you
 * watch happen rather than reconstruct afterwards.
 *
 * Refreshed twice a second, not per frame: va_stats walks every region in the
 * process, which is far too expensive to do 60 times a second and would itself
 * become the thing being measured. */
static int osd_on(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[8];
		unsigned n = savestate_getenv("D3D11SW_OSD", buf, sizeof(buf));

		v = (n && buf[0] != '0') ? 1 : 0;
	}
	/* Only while this game is in front. GetAsyncKeyState reports the key
	 * whoever owns the keyboard pressed, so an ungated poll toggles the
	 * overlay when F8 is pressed in another application entirely. */
	{
		DWORD pid = 0;

		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		if (pid == GetCurrentProcessId() && savestate_key_edge(VK_F8))
			v = !v;
	}
	return v;
}

static void osd_update(void)
{
	static LARGE_INTEGER last, freq;
	static double used, freed, largest;
	LARGE_INTEGER now;
	char line[256];
	double cap;

	static int was_on;

	if (!osd_on()) {
		/* Clear once on the way down, not every frame: leaving stale text
		 * behind after a toggle-off would be worse than the cost, but
		 * taking the lock 60 times a second to write nothing is waste. */
		if (was_on) {
			swrast_overlay_set(NULL);
			was_on = 0;
		}
		return;
	}
	was_on = 1;
	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	if (!last.QuadPart || (double)(now.QuadPart - last.QuadPart) / freq.QuadPart >= 0.5) {
		last = now;
		va_stats(&used, &freed, &largest);
	}
	/* Against the total the process can actually address, which is what runs
	 * out - not against installed RAM, which is irrelevant here and would
	 * make a process about to die look healthy. */
	cap = used + freed;
	_snprintf(line, sizeof(line) - 1,
		  "VA %.0f / %.0f MB used   free %.0f MB   largest block %.1f MB\n"
		  "textures %ld live, %.0f MB",
		  used, cap, freed, largest, (long)g_res_live,
		  g_res_bytes / (1024.0 * 1024.0));
	line[sizeof(line) - 1] = 0;
	swrast_overlay_set(line);
}

/* True only for the formats res_decode_pixels copies verbatim.
 *
 * Being 32 bits per pixel is not enough to make the two planes identical:
 * R8G8B8A8 is the same width but the opposite channel order, and its decode is
 * a swizzle rather than a copy. Aliasing that one skips the swizzle and every
 * texture comes out with red and blue exchanged. This must stay in step with
 * the memcpy case in res_decode_pixels; anything not listed here gets its own
 * plane, which is the safe direction to be wrong in. */
static int fmt_pixels_identical(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
		return 1;
	default:
		return 0;
	}
}

/* Same size and same information as the rasteriser's layout, but with red and
 * blue exchanged - so the two planes can still be shared, provided the shared
 * copy is converted in place. Most game art is this format, which is why it is
 * worth the extra care rather than just giving it a second plane. */
static int fmt_pixels_swappable(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		return 1;
	default:
		return 0;
	}
}

/* D3D11SW_TEX_SCALE divides the stored size of large sprite atlases.
 *
 * A 2304x5760 atlas is 50 MB of uncompressed 32-bit pixels, and it is being
 * sampled into a 640x360 target. Hardware would pick a low mip level and never
 * read the full-resolution data at all; we have no mip chain, so we keep every
 * texel resident and then discard most of the detail at sample time. Halving
 * each axis is a quarter of the memory for detail that is already being thrown
 * away, and because D3D11 texture coordinates are normalised, sampling needs no
 * adjustment whatsoever - a smaller surface simply resolves to the same place.
 *
 * 2 halves each axis, 4 quarters it, 0 or 1 disables. Off by default: it is a
 * quality trade, and it should be a decision rather than a surprise. */
static UINT tex_scale(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[8];
		unsigned n = savestate_getenv("D3D11SW_TEX_SCALE", buf, sizeof(buf));

		v = (n && buf[0] >= '0' && buf[0] <= '9') ? buf[0] - '0' : 1;
		if (v < 1)
			v = 1;
		if (v > 8)
			v = 8;
		/* A power of two only. Any other divisor puts a fractional number
		 * of source texels in each destination texel, and the filter
		 * below assumes an exact box. */
		while (v & (v - 1))
			v--;
	}
	return (UINT)v;
}

/* Whether a texture may be stored smaller than the game asked for.
 *
 * The requirement is that nothing outside this file can ever see the stored
 * bytes, because they would be the wrong size. No CPU access flags means D3D11
 * forbids mapping it. Shader-resource-only excludes anything rendered into,
 * whose contents arrive through a viewport sized for the real surface. And it
 * must arrive complete at creation: a later partial update addresses texels in
 * the game's coordinates, and downsampling an arbitrary sub-rectangle of an
 * atlas is not something to attempt on a guess. */
static int tex_scale_eligible(const D3D11_TEXTURE2D_DESC *d, const D3D11_SUBRESOURCE_DATA *init)
{
	const char *why = NULL;

	if (tex_scale() <= 1 || !d)
		return 0;
	if (d->Width < 512 || d->Height < 512)
		return 0; /* not where the memory is; not worth reporting */

	/* Past the size filter this is a texture we WANT to shrink, so when it is
	 * turned down, say which rule did it. A silent no-op that looks exactly
	 * like the feature being off is the worst possible outcome. */
	/* Contents at creation are not required. This game creates every atlas
	 * empty and blits the decoded image in immediately afterwards, so
	 * demanding pSysMem here rejected all of them and made the whole feature
	 * a silent no-op. UpdateSubresource shrinks the incoming image to match
	 * whatever the storage ended up being. */
	(void)init;
	if (d->BindFlags != D3D11_BIND_SHADER_RESOURCE)
		why = "it is not a plain shader resource";
	else if (d->CPUAccessFlags)
		why = "the game can map it";
	else if (d->MipLevels > 1 || d->ArraySize > 1)
		why = "it has mips or array slices";
	else if (fmt_bc_block(d->Format) || fmt_is_depth(d->Format) || fmt_stride(d->Format) != 4)
		why = "the format is not uncompressed 32-bit";
	if (!why)
		return 1;
	{
		static LONG said;

		if (InterlockedIncrement(&said) <= 40)
			d11_log("TEX_SCALE skipped a %ux%u texture (fmt=%d bind=%#x usage=%d "
				"cpu=%#x mips=%u array=%u): %s",
				d->Width, d->Height, (int)d->Format, (unsigned)d->BindFlags,
				(int)d->Usage, (unsigned)d->CPUAccessFlags, d->MipLevels,
				d->ArraySize, why);
	}
	return 0;
}

/* D3D11SW_MARK_EMPTY=1 fills freshly created texture storage with magenta.
 *
 * Both allocators hand back zeroed pages, and zero in any 32-bit colour format
 * is opaque black. A texture the game creates and never fills is therefore
 * indistinguishable from one it deliberately painted black, which is exactly
 * the ambiguity behind the black rectangles: they are the right shape for a
 * quad, they are stable rather than noisy, and black is what "nothing was ever
 * written here" looks like.
 *
 * Magenta is not a colour this artwork uses, so anything that turns magenta was
 * never filled. Anything that stays black is real content and the search moves
 * elsewhere. Diagnostic only: it makes the gap obvious, it does not close it. */
static int mark_empty(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[8];
		unsigned n = savestate_getenv("D3D11SW_MARK_EMPTY", buf, sizeof(buf));

		v = (n && buf[0] == '1') ? 1 : 0;
	}
	return v;
}

static void fill_magenta(unsigned char *p, size_t bytes)
{
	size_t i;

	/* FF 00 FF FF reads as magenta in both channel orders we handle, so this
	 * does not need to know whether the texture is RGBA or BGRA. */
	for (i = 0; i + 4 <= bytes; i += 4) {
		p[i + 0] = 0xFF;
		p[i + 1] = 0x00;
		p[i + 2] = 0xFF;
		p[i + 3] = 0xFF;
	}
}

/* sRGB <-> linear, as tables.
 *
 * Averaging gamma-encoded values does not average light: the midpoint of black
 * and white in sRGB is a good deal darker than half the photons. Downscaling in
 * sRGB therefore dims thin bright features and dirties every high-contrast
 * edge, which on a sprite atlas is most of the interesting pixels.
 *
 * Tables rather than pow(): the forward direction has only 256 inputs, and the
 * reverse is sampled finely enough that the worst error is the maximum slope of
 * the curve times one step, 12.92/16384 - about a fifth of an 8-bit level. */
#define SRGB_LUT_N 16384

static float g_lin_from_srgb[256];
static unsigned char g_srgb_from_lin[SRGB_LUT_N];
static volatile LONG g_srgb_ready;

static void srgb_tables_once(void)
{
	int i;

	if (InterlockedCompareExchange(&g_srgb_ready, 1, 0) != 0) {
		/* Someone else is building them, or already has. */
		while (g_srgb_ready != 2)
			YieldProcessor();
		return;
	}
	for (i = 0; i < 256; i++) {
		double c = i / 255.0;

		g_lin_from_srgb[i] =
			(float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
	}
	for (i = 0; i < SRGB_LUT_N; i++) {
		double l = (double)i / (SRGB_LUT_N - 1);
		double s = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
		int v = (int)(s * 255.0 + 0.5);

		g_srgb_from_lin[i] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
	}
	InterlockedExchange(&g_srgb_ready, 2);
}

static unsigned char srgb_of_linear(double l)
{
	int i;

	if (!(l > 0.0))
		return 0;
	if (l >= 1.0)
		return 255;
	i = (int)(l * (SRGB_LUT_N - 1) + 0.5);
	return g_srgb_from_lin[i];
}

/* Area-average an image down to an arbitrary smaller size.
 *
 * Every destination pixel is the exact area-weighted mean of the source pixels
 * its footprint covers, so this needs no relationship at all between the two
 * sizes. That matters: the previous version halved repeatedly, which is only
 * defined when both axes are even, and quietly refused to shrink anything with
 * an odd dimension. Packed atlases are routinely an odd number of pixels tall,
 * so most of the memory this was written to reclaim was being skipped.
 *
 * Colour is averaged in linear light and premultiplied by alpha first. Straight
 * alpha stores fully transparent pixels as transparent BLACK, so averaging the
 * colour channels on their own drags every sprite edge towards black and leaves
 * a dark fringe once it is composited. Premultiplying makes the filter commute
 * with the "over" it will later take part in, which is the whole reason that
 * representation exists. Alpha itself is coverage and is neither gamma-encoded
 * nor premultiplied by anything, so it is averaged as-is.
 *
 * Bytes 0..2 are treated as colour and byte 3 as alpha, which holds for both
 * R8G8B8A8 and B8G8R8A8 - the same curve applies to all three colour channels,
 * so their order does not matter here.
 *
 * What this cannot fix: sprites packed edge to edge with no gutter will still
 * bleed into each other, because the boundary simply is not in the data. Only
 * padding the atlas would fix that, and it belongs to whoever built it. */
static void area_downscale_rgba(unsigned char *dst, UINT dst_pitch, UINT dw, UINT dh,
				const unsigned char *src, UINT src_pitch, UINT sw, UINT sh)
{
	double xs, ys;
	UINT x, y;

	if (!dst || !src || !dw || !dh || !sw || !sh)
		return;
	srgb_tables_once();
	xs = (double)sw / (double)dw;
	ys = (double)sh / (double)dh;
	for (y = 0; y < dh; y++) {
		double y0 = y * ys, y1 = y0 + ys;
		unsigned char *o = dst + (size_t)y * dst_pitch;
		UINT iy0, iy1, sy;

		if (y1 > (double)sh)
			y1 = (double)sh;
		iy0 = (UINT)y0;
		iy1 = (UINT)ceil(y1);
		if (iy1 > sh)
			iy1 = sh;
		for (x = 0; x < dw; x++) {
			double x0 = x * xs, x1 = x0 + xs;
			double ar = 0.0, ag = 0.0, ab = 0.0, aa = 0.0, wsum = 0.0;
			UINT ix0, ix1;

			if (x1 > (double)sw)
				x1 = (double)sw;
			ix0 = (UINT)x0;
			ix1 = (UINT)ceil(x1);
			if (ix1 > sw)
				ix1 = sw;
			for (sy = iy0; sy < iy1; sy++) {
				const unsigned char *row = src + (size_t)sy * src_pitch;
				double t0 = (double)sy, t1 = t0 + 1.0;
				double wy = (y1 < t1 ? y1 : t1) - (y0 > t0 ? y0 : t0);
				UINT sx;

				if (wy <= 0.0)
					continue;
				for (sx = ix0; sx < ix1; sx++) {
					const unsigned char *p = row + (size_t)sx * 4;
					double u0 = (double)sx, u1 = u0 + 1.0;
					double wx = (x1 < u1 ? x1 : u1) - (x0 > u0 ? x0 : u0);
					double w, a;

					if (wx <= 0.0)
						continue;
					w = wx * wy;
					a = p[3] * (1.0 / 255.0);
					ar += g_lin_from_srgb[p[0]] * a * w;
					ag += g_lin_from_srgb[p[1]] * a * w;
					ab += g_lin_from_srgb[p[2]] * a * w;
					aa += a * w;
					wsum += w;
				}
			}
			if (wsum > 0.0) {
				double a = aa / wsum;

				/* Back to straight alpha, which is what the game
				 * handed us and what the sampler expects. Below
				 * roughly one part in 255 there is no colour left
				 * to recover and dividing only amplifies noise. */
				if (a > (0.5 / 255.0)) {
					double inv = 1.0 / (wsum * a);

					o[0] = srgb_of_linear(ar * inv);
					o[1] = srgb_of_linear(ag * inv);
					o[2] = srgb_of_linear(ab * inv);
				} else {
					o[0] = o[1] = o[2] = 0;
				}
				o[3] = (unsigned char)(a * 255.0 + 0.5);
			} else {
				o[0] = o[1] = o[2] = o[3] = 0;
			}
			o += 4;
		}
	}
}

/* D3D11SW_DEDUP=0 gives every texture its own decoded plane again. The escape
 * hatch for the sharing above: if a title ever shows swapped channels, this
 * says so in one setting instead of needing a rebuild. */
static int dedup_on(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[8];
		unsigned n = savestate_getenv("D3D11SW_DEDUP", buf, sizeof(buf));

		v = (n && buf[0] == '0') ? 0 : 1;
	}
	return v;
}

static LONG64 res_payload_bytes(const Sw11Res *r)
{
	LONG64 n;

	if (!r || r->phantom)
		return 0; /* a phantom holds no payload, so it accounts for nothing */
	n = (LONG64)r->cpu_size;
	if (r->pixels && !r->pixels_alias)
		n += (LONG64)r->width * r->height * 4;
	if (r->depth)
		n += (LONG64)r->width * r->height * (LONG64)sizeof(float);
	return n;
}

/* Release the three planes and account for them. Every free path goes through
 * here so the sizes handed to payload_free always match the ones it was
 * allocated with, and so an aliased plane is never freed twice. */
static void res_free_payload(Sw11Res *r, int drop_live)
{
	gpu_tex_drop(&r->gtex);
	if (!r)
		return;
	res_list_remove(r);
	res_account(-res_payload_bytes(r), drop_live ? -1 : 0);
	payload_free(r->cpu, r->cpu_size);
	if (!r->pixels_alias)
		payload_free(r->pixels, (size_t)r->width * r->height * 4);
	payload_free(r->depth, (size_t)r->width * r->height * sizeof(float));
	r->cpu = NULL;
	r->pixels = NULL;
	r->depth = NULL;
	r->pixels_alias = 0;
	r->pixels_swizzled = 0;
	r->cpu_size = 0;
	r->has_texels = 0;
	free(r->bank); /* the faucet's backing copy; NULL-safe, and re-NULLed */
	r->bank = NULL;
	r->bank_size = 0;
}

static int res_ensure_pixels(Sw11Res *r)
{
	if (!r || r->kind != 1)
		return 0;
	if (r->phantom)
		return 0; /* no pixels by design; the draw site binds magenta instead */
	if (!r->pixels) {
		/* Alias instead of duplicating where the two planes would hold
		 * identical bytes.
		 *
		 * cpu holds the texture as the game supplied it; pixels holds it
		 * in the rasteriser's 32-bit layout. When the format is already
		 * that layout and the rows are tight, res_decode_pixels does
		 * nothing but memcpy one onto the other, so the second plane is
		 * a second copy of the same image. On a 32-bit title that is not
		 * a minor waste: the process has under 2 GB of address space and
		 * every texture a GPU would have kept in video memory lives here
		 * instead, so the duplicate is the difference between running and
		 * running out.
		 *
		 * Excluded when the surface is a render target or depth-stencil,
		 * because those are drawn INTO through pixels and must not write
		 * through to the game's copy. Bind flags are fixed at creation in
		 * D3D11, so a texture that is not one now never becomes one. */
		int swap = fmt_pixels_swappable(r->format);

		/* Converting in place rewrites the game's own copy, so it is only
		 * allowed where the game can never look at that copy. No CPU
		 * access flags means D3D11 forbids mapping the resource at all,
		 * which is exactly that guarantee. A verbatim alias needs no such
		 * promise, because those bytes are never altered. */
		if (r->cpu && dedup_on() &&
		    !(r->bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) &&
		    !fmt_is_depth(r->format) &&
		    (fmt_pixels_identical(r->format) || (swap && !r->cpu_access)) &&
		    r->row_pitch == r->width * 4u &&
		    r->cpu_size >= (size_t)r->width * r->height * 4u) {
			r->pixels = (uint32_t *)r->cpu;
			r->pixels_alias = 1;
			r->pixels_swizzled = swap;
			/* Whatever is already there has not been converted yet. */
			if (swap)
				r->cpu_dirty = 1;
		} else {
			r->pixels = (uint32_t *)payload_alloc(
				(size_t)r->width * r->height * 4, r, PL_PIXELS);
			if (!r->pixels)
				return 0;
			r->plane_gen = g_save_gen;
			res_account((LONG64)r->width * r->height * 4, 0);
		}
	}
	if (fmt_is_depth(r->format) && !r->depth) {
		r->depth = (float *)payload_alloc(
			(size_t)r->width * r->height * sizeof(float), r, PL_DEPTH);
		if (!r->depth)
			return 0;
		r->plane_gen = g_save_gen;
		res_account((LONG64)r->width * r->height * (LONG64)sizeof(float), 0);
	}
	return 1;
}

/* ---- faucet: evict cold texture payloads to a bank, paint them back on draw ----
 *
 * D3D11SW_FAUCET=1 turns the resident cap into a WINDOW: only ~cap MB of texture
 * pixels stay in the arena; the rest are evicted (arena copy freed, bank copy kept)
 * and painted back into a fresh arena slot the next time they are drawn. The game
 * holds a handle and never sees this - only the payload behind it moves. An evicted
 * texture IS a phantom with a bank. Fault-in and eviction run ONLY on the draw
 * thread, so no two allocations race the raster workers.
 *
 * Recency is by frame, not by draw. A per-draw clock only protected the draw in
 * progress, so a scene slightly bigger than the window evicted a texture it had
 * drawn a moment ago and painted it back on the next pass: eight textures went
 * round in a cycle, #130 (15 MB) evicted so #131 could take its slot and painted
 * back straight after, ~35 MB copied per cycle for nothing. Anything drawn in the
 * last D3D11SW_FAUCET_HOT frames is never evicted; when nothing colder is left the
 * window is overcommitted instead, which the arena absorbs (it is larger than any
 * window worth running).
 *
 * Only arena-sized textures are managed. Below PAYLOAD_VA_MIN a payload comes from
 * calloc, never touches the arena, frees almost nothing when evicted, and is often
 * the most-drawn texture in the frame - so managed == arena-backed == evictable.
 *
 * This is Step 1 - an in-RAM bank; a disk bank comes later. */
static int faucet_on(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		unsigned n = savestate_getenv("D3D11SW_FAUCET", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '1') ? 1 : 0;
	}
	return cached;
}

/* Frames a drawn texture stays protected from eviction. 1 protects only the
 * current frame; the default covers a scene that alternates what it draws. */
static unsigned faucet_hot_frames(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		unsigned n = savestate_getenv("D3D11SW_FAUCET_HOT", v, sizeof(v));
		unsigned long k = n ? strtoul(v, NULL, 10) : 3;

		cached = k ? (int)k : 1;
	}
	return (unsigned)cached;
}

/* A paint-in this soon after the same texture's eviction means the window evicted
 * something the scene was still using. About a second at 60 fps. */
#define FAUCET_THRASH_FRAMES 60u

static int faucet_managed(const Sw11Res *r)
{
	return faucet_on() && pl_cappable(r, PL_TEXTURE) &&
	       r->cpu_size >= PAYLOAD_VA_MIN;
}

static int faucet_hot(const Sw11Res *r)
{
	return (unsigned)g_present_n - r->lru < faucet_hot_frames();
}

/* Counters are touched only on the draw thread, which is also the one that reads
 * them in perf_tick. */
static volatile LONG g_faucet_in, g_faucet_evict, g_faucet_stuck;
static LONG g_faucet_thrash, g_faucet_over;
static LONG64 g_faucet_in_bytes, g_faucet_over_peak;
static LONG64 g_faucet_hot_peak, g_faucet_hot_peak_all;

/* Copy the current resident content to the bank, so it can be painted back after
 * eviction. Called when content is established (create with data, upload). */
static void faucet_bank_store(Sw11Res *r)
{
	if (!r || !r->cpu || !faucet_managed(r))
		return;
	if (r->bank_size != r->cpu_size) {
		unsigned char *q = (unsigned char *)realloc(r->bank, r->cpu_size);

		if (!q)
			return;
		r->bank = q;
		r->bank_size = r->cpu_size;
	}
	memcpy(r->bank, r->cpu, r->cpu_size);
}

/* Free the arena copy of the least-recently-used cold texture, keeping its bank
 * and description. Returns bytes freed, 0 if nothing was evictable. */
static LONG64 faucet_evict_one(const Sw11Res *keep)
{
	Sw11Res *r, *victim = NULL;
	unsigned best = 0;
	LONG64 freed;

	for (r = g_res_head; r; r = r->live_next) {
		if (r == keep || r->kind != 1 || r->phantom || !r->cpu || !r->bank)
			continue;
		if (!faucet_managed(r) || faucet_hot(r))
			continue;
		if (!victim || r->lru < best) {
			victim = r;
			best = r->lru;
		}
	}
	if (!victim)
		return 0;
	swrast_flush_if_pending(victim->pixels); /* no worker still reading it */
	freed = res_payload_bytes(victim);
	res_account(-freed, 0);
	payload_free(victim->cpu, victim->cpu_size);
	if (!victim->pixels_alias)
		payload_free(victim->pixels,
			     (size_t)victim->width * victim->height * 4);
	victim->cpu = NULL;
	victim->pixels = NULL;
	victim->pixels_alias = 0;
	victim->pixels_swizzled = 0;
	victim->phantom = 1; /* evicted: a phantom with a bank behind it */
	victim->evicted_at = (unsigned)g_present_n;
	InterlockedIncrement(&g_faucet_evict);
	return freed;
}

/* Overcommit is expected and soft: the scene's hot set is bigger than the window,
 * so residency goes over it rather than evicting something still in use. Distinct
 * from stuck, which is an allocation that actually failed. */
static void faucet_ensure_room(size_t need, const Sw11Res *keep)
{
	unsigned cap = resident_cap_mb();
	LONG64 win, over;

	if (!cap)
		return;
	win = (LONG64)cap * 1024 * 1024;
	while ((LONG64)g_res_bytes + (LONG64)need > win)
		if (!faucet_evict_one(keep))
			break;
	over = (LONG64)g_res_bytes + (LONG64)need - win;
	if (over > 0) {
		g_faucet_over++;
		if (over > g_faucet_over_peak)
			g_faucet_over_peak = over;
	}
}

/* Stamp a texture as drawn this frame. */
static void faucet_touch(Sw11Res *r)
{
	r->lru = (unsigned)g_present_n;
}

/* Per present: bytes of managed textures drawn in the hot span. Its peak is the
 * window this scene needs to run without evicting anything it is using - the
 * working-set figure the cap sweep could only bound from below. A walk of ~230
 * entries a frame. */
static void faucet_frame(void)
{
	Sw11Res *r;
	LONG64 hot = 0;

	if (!faucet_on())
		return;
	for (r = g_res_head; r; r = r->live_next)
		if (r->kind == 1 && !r->phantom && faucet_managed(r) && faucet_hot(r))
			hot += (LONG64)r->cpu_size;
	if (hot > g_faucet_hot_peak)
		g_faucet_hot_peak = hot;
	if (hot > g_faucet_hot_peak_all)
		g_faucet_hot_peak_all = hot;
}

/* Paint an evicted texture back from its bank into a fresh arena slot, so the draw
 * about to sample it gets real pixels. Draw thread only. */
static int faucet_fault_in(Sw11Res *r)
{
	if (!r || !r->phantom || !r->bank)
		return 0;
	faucet_ensure_room(r->cpu_size, r);
	r->cpu = (unsigned char *)payload_alloc(r->cpu_size, r, PL_TEXTURE);
	if (!r->cpu) {
		InterlockedIncrement(&g_faucet_stuck); /* window too small for even one */
		return 0;                              /* stays magenta this draw */
	}
	memcpy(r->cpu, r->bank, r->cpu_size);
	r->phantom = 0;
	r->cpu_dirty = 1;
	res_account((LONG64)r->cpu_size, 0);
	faucet_touch(r);
	g_faucet_in_bytes += (LONG64)r->cpu_size;
	{
		int thrash = r->evicted_at &&
			     (unsigned)g_present_n - r->evicted_at <= FAUCET_THRASH_FRAMES;

		if (thrash)
			g_faucet_thrash++;
		if (InterlockedIncrement(&g_faucet_in) <= 24)
			d11_log("FAUCET paint-in #%d %ux%u -> %s %p (live %.1f MB, %ld "
				"evict(s) so far)%s",
				r->id, r->width, r->height,
				arena_owns(r->cpu) ? "arena" : "outside the arena",
				(void *)r->cpu, g_res_bytes / (1024.0 * 1024.0),
				(long)g_faucet_evict,
				thrash ? " - THRASH, evicted under a second ago" : "");
	}
	return 1;
}

/* Ensure an evicted texture has a resident cpu plane before the game writes to it
 * (an upload). Paints in from the bank if there is one, else allocates an empty
 * resident plane for the upload to land in; either way it is banked afterward. */
static void faucet_make_resident(Sw11Res *r)
{
	if (!r || !r->phantom || !faucet_managed(r))
		return;
	if (r->bank) {
		faucet_fault_in(r);
		return;
	}
	faucet_ensure_room(r->cpu_size, r);
	r->cpu = (unsigned char *)payload_alloc(r->cpu_size, r, PL_TEXTURE);
	if (r->cpu) {
		r->phantom = 0;
		res_account((LONG64)r->cpu_size, 0);
		faucet_touch(r);
	} else
		InterlockedIncrement(&g_faucet_stuck);
}

static void res_copy_tex_rect(Sw11Res *d, UINT dx, UINT dy, Sw11Res *s, UINT sx, UINT sy,
			       UINT tw, UINT th)
{
	UINT bpp, y;
	if (!d || !s || !d->cpu || !s->cpu)
		return;
	if (fmt_bc_block(d->format) || fmt_bc_block(s->format) || d->format != s->format) {
		UINT n;
		if (!d->cpu_size || !s->cpu_size)
			return;
		n = d->cpu_size < s->cpu_size ? d->cpu_size : s->cpu_size;
		memcpy(d->cpu, s->cpu, n);
		if (d->bind & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET))
			res_ensure_pixels(d);
		if (d->pixels)
			res_after_cpu_copy(d, s);
		return;
	}
	bpp = fmt_stride(d->format);
	if (!bpp)
		return;
	if (dx >= d->width || dy >= d->height || sx >= s->width || sy >= s->height)
		return;
	if (tw > d->width - dx)
		tw = d->width - dx;
	if (th > d->height - dy)
		th = d->height - dy;
	if (tw > s->width - sx)
		tw = s->width - sx;
	if (th > s->height - sy)
		th = s->height - sy;
	for (y = 0; y < th; y++)
		memcpy(d->cpu + (size_t)(dy + y) * d->row_pitch + (size_t)dx * bpp,
		       s->cpu + (size_t)(sy + y) * s->row_pitch + (size_t)sx * bpp,
		       (size_t)tw * bpp);
	if (d->bind & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET))
		res_ensure_pixels(d);
	if (d->pixels)
		res_after_cpu_copy(d, s);
}

/* pixels -> cpu, the one direction the decode path never has to run.
 *
 * Rendered output lives in the pixels plane. The cpu plane holds whatever was
 * last uploaded, and nothing uploads to a render target, so for the backbuffer
 * it has never been written at all. A readback asks for the rendered image and
 * an ordinary copy hands over the other plane, which is how the game has been
 * reading memory no one initialised.
 *
 * Returns 0 without writing anything where it cannot do the job, so the caller
 * can say so plainly instead of leaving wrong colours behind.
 */
static int res_readback_pixels(Sw11Res *d, Sw11Res *s)
{
	UINT x, y, w, h;
	int swap;

	if (!d || !s || !d->cpu || !s->pixels || !d->row_pitch)
		return 0;
	/* Aliased planes are the same memory, so the ordinary copy already moves
	 * the rendered pixels and this would be a second opinion on bytes that
	 * are not in dispute. */
	if (s->pixels_alias)
		return 0;
	if (fmt_bc_block(d->format) || fmt_stride(d->format) != 4)
		return 0;
	if (fmt_pixels_swappable(d->format))
		swap = 1; /* R8G8B8A8, against a rasteriser that works in BGRA */
	else if (d->format == DXGI_FORMAT_B8G8R8A8_UNORM ||
		 d->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
		 d->format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
		swap = 0;
	else
		return 0;
	w = d->width < s->width ? d->width : s->width;
	h = d->height < s->height ? d->height : s->height;
	for (y = 0; y < h; y++) {
		const uint32_t *sp = s->pixels + (size_t)y * s->width;
		uint32_t *dp = (uint32_t *)(d->cpu + (size_t)y * d->row_pitch);

		if (!swap) {
			memcpy(dp, sp, (size_t)w * 4);
			continue;
		}
		for (x = 0; x < w; x++) {
			uint32_t v = sp[x];

			dp[x] = (v & 0xff00ff00u) | ((v & 0x00ff0000u) >> 16) |
				((v & 0x000000ffu) << 16);
		}
	}
	return 1;
}

/* Point-sampled stretch over the full extent of both surfaces.
 *
 * Needed only because render-target scaling can leave a copy's source and
 * destination at different REAL sizes for a copy the game believes is
 * size-for-size - D3D11 requires CopyResource's two resources to match, and as
 * far as the game is concerned they still do. Stretching preserves what the
 * copy meant; a straight blit would put a small image in a corner.
 *
 * Both planes are handled because they hold different things: cpu is the
 * decoded-from source image and pixels is where rasterised output actually
 * lands, with no path back from pixels to cpu. Copying only cpu would silently
 * discard everything that was drawn. */
static void res_stretch_tex(Sw11Res *d, Sw11Res *s)
{
	UINT bpp, x, y;

	if (!d || !s || !d->width || !d->height || !s->width || !s->height)
		return;
	swrast_flush_if_pending(s->pixels);
	swrast_flush_if_pending(d->pixels);
	if (d->pixels && s->pixels) {
		for (y = 0; y < d->height; y++) {
			UINT sy = (UINT)((uint64_t)y * s->height / d->height);
			const uint32_t *srow = s->pixels + (size_t)sy * s->width;
			uint32_t *drow = d->pixels + (size_t)y * d->width;

			for (x = 0; x < d->width; x++)
				drow[x] = srow[(UINT)((uint64_t)x * s->width / d->width)];
		}
	}
	bpp = fmt_stride(d->format);
	if (!d->cpu || !s->cpu || !bpp || d->format != s->format || fmt_bc_block(d->format) ||
	    fmt_bc_block(s->format))
		return;
	for (y = 0; y < d->height; y++) {
		UINT sy = (UINT)((uint64_t)y * s->height / d->height);
		unsigned char *drow = d->cpu + (size_t)y * d->row_pitch;
		const unsigned char *srow = s->cpu + (size_t)sy * s->row_pitch;

		for (x = 0; x < d->width; x++)
			memcpy(drow + (size_t)x * bpp,
			       srow + (size_t)((uint64_t)x * s->width / d->width) * bpp, bpp);
	}
}

/* Point-sampled stretch of one target onto another. Used only as a rescue when
 * a frame drew geometry but never wrote the backbuffer, which means the real
 * composite pass did not survive our incomplete shader pipeline. */
static void composite_upscale(Sw11Res *dst, Sw11Res *src)
{
	UINT x, y;
	if (!dst || !src || !dst->pixels || !src->pixels)
		return;
	if (!dst->width || !dst->height || !src->width || !src->height)
		return;
	swrast_flush_if_pending(src->pixels);
	swrast_flush_if_pending(dst->pixels);
	for (y = 0; y < dst->height; y++) {
		UINT sy = (UINT)((uint64_t)y * src->height / dst->height);
		const uint32_t *srow = src->pixels + (size_t)sy * src->width;
		uint32_t *drow = dst->pixels + (size_t)y * dst->width;
		for (x = 0; x < dst->width; x++)
			drow[x] = srow[(UINT)((uint64_t)x * src->width / dst->width)] | 0xff000000u;
	}
}

static Sw11Res *res_from_unk(ID3D11Resource *u)
{
	if (!u)
		return NULL;
	return (Sw11Res *)u;
}

static void child_get_device(Sw11Device *dev, ID3D11Device **out)
{
	if (!out)
		return;
	*out = (ID3D11Device *)&dev->iface;
	dev->iface.lpVtbl->AddRef(&dev->iface);
}

/* ---------- factory / adapter / output ---------- */

static HRESULT WINAPI Fact_QI(IDXGIFactory2 *this, REFIID riid, void **ppv)
{
	Sw11Factory *f = (Sw11Factory *)this;
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IDXGIObject) ||
	    guid_eq(riid, &IID_IDXGIFactory) || guid_eq(riid, &IID_IDXGIFactory1) ||
	    guid_eq(riid, &IID_IDXGIFactory2)) {
		*ppv = this;
		f->iface.lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI Fact_AddRef(IDXGIFactory2 *this)
{
	return (ULONG)InterlockedIncrement(&((Sw11Factory *)this)->ref);
}

static ULONG WINAPI Fact_Release(IDXGIFactory2 *this)
{
	Sw11Factory *f = (Sw11Factory *)this;
	LONG n = InterlockedDecrement(&f->ref);
	if (n == 0) {
		priv_free(f->priv);
		obj_free(f);
	}
	return (ULONG)n;
}

static HRESULT WINAPI Fact_SetPrivateData(IDXGIFactory2 *this, REFGUID g, UINT n, const void *d)
{
	return priv_set(&((Sw11Factory *)this)->priv, g, n, d);
}

static HRESULT WINAPI Fact_SetPrivateDataInterface(IDXGIFactory2 *this, REFGUID g,
						   const IUnknown *o)
{
	return priv_set(&((Sw11Factory *)this)->priv, g, sizeof(o), &o);
}

static HRESULT WINAPI Fact_GetPrivateData(IDXGIFactory2 *this, REFGUID g, UINT *n, void *d)
{
	return priv_get(((Sw11Factory *)this)->priv, g, n, d);
}

static HRESULT WINAPI Fact_GetParent(IDXGIFactory2 *this, REFIID riid, void **pp)
{
	(void)this;
	(void)riid;
	if (pp)
		*pp = NULL;
	return E_NOINTERFACE;
}

static HRESULT Adp_create(Sw11Factory *f, IDXGIAdapter1 **out);

static HRESULT WINAPI Fact_EnumAdapters(IDXGIFactory2 *this, UINT i, IDXGIAdapter **out)
{
	return ((IDXGIFactory2Vtbl *)this->lpVtbl)->EnumAdapters1(this, i, (IDXGIAdapter1 **)out);
}

static HRESULT WINAPI Fact_EnumAdapters1(IDXGIFactory2 *this, UINT i, IDXGIAdapter1 **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (i != 0)
		return DXGI_ERROR_NOT_FOUND;
	return Adp_create((Sw11Factory *)this, out);
}

static HRESULT WINAPI Fact_MakeWindowAssociation(IDXGIFactory2 *this, HWND hwnd, UINT flags)
{
	Sw11Factory *f = (Sw11Factory *)this;
	f->assoc = hwnd;
	f->assoc_flags = flags;
	return S_OK;
}

static HRESULT WINAPI Fact_GetWindowAssociation(IDXGIFactory2 *this, HWND *hwnd)
{
	if (!hwnd)
		return E_POINTER;
	*hwnd = ((Sw11Factory *)this)->assoc;
	return S_OK;
}

static Sw11Device *dev_from_unk(IUnknown *u)
{
	ID3D11Device *d = NULL;
	Sw11Device *dev;
	if (!u)
		return NULL;
	if (FAILED(u->lpVtbl->QueryInterface(u, &IID_ID3D11Device, (void **)&d)) || !d)
		return NULL;
	dev = (Sw11Device *)d;
	d->lpVtbl->Release(d);
	if (dev->iface.lpVtbl != &kDevVtbl)
		return NULL;
	return dev;
}

static HRESULT WINAPI Fact_CreateSwapChain(IDXGIFactory2 *this, IUnknown *device,
					     DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **out)
{
	Sw11Device *dev;
	Sw11Swap *s;
	HRESULT hr;
	(void)this;
	if (!out)
		return E_POINTER;
	*out = NULL;
	dev = dev_from_unk(device);
	if (!dev || !desc)
		return DXGI_ERROR_INVALID_CALL;
	hr = sw11_create_swap(dev, desc->OutputWindow, desc, &s);
	if (FAILED(hr))
		return hr;
	*out = (IDXGISwapChain *)&s->iface;
	return S_OK;
}

static HRESULT WINAPI Fact_CreateSoftwareAdapter(IDXGIFactory2 *this, HMODULE sw,
						IDXGIAdapter **out)
{
	(void)sw;
	return Fact_EnumAdapters(this, 0, out);
}

static BOOL WINAPI Fact_IsCurrent(IDXGIFactory2 *this)
{
	(void)this;
	return TRUE;
}

static BOOL WINAPI Fact_IsWindowedStereoEnabled(IDXGIFactory2 *this)
{
	(void)this;
	return FALSE;
}

static HRESULT WINAPI Fact_CreateSwapChainForHwnd(IDXGIFactory2 *this, IUnknown *device, HWND hwnd,
						  const DXGI_SWAP_CHAIN_DESC1 *d1,
						  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs,
						  IDXGIOutput *restrict_out, IDXGISwapChain1 **out)
{
	DXGI_SWAP_CHAIN_DESC d;
	(void)fs;
	(void)restrict_out;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!d1)
		return DXGI_ERROR_INVALID_CALL;
	memset(&d, 0, sizeof(d));
	d.BufferDesc.Width = d1->Width;
	d.BufferDesc.Height = d1->Height;
	d.BufferDesc.Format = d1->Format;
	d.BufferDesc.RefreshRate.Numerator = 60;
	d.BufferDesc.RefreshRate.Denominator = 1;
	d.SampleDesc = d1->SampleDesc;
	d.BufferUsage = d1->BufferUsage;
	d.BufferCount = d1->BufferCount;
	d.OutputWindow = hwnd;
	d.Windowed = TRUE;
	d.SwapEffect = (DXGI_SWAP_EFFECT)d1->SwapEffect;
	d.Flags = d1->Flags;
	return Fact_CreateSwapChain(this, device, &d, (IDXGISwapChain **)out);
}

static HRESULT WINAPI Adp_QI(IDXGIAdapter1 *this, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IDXGIObject) ||
	    guid_eq(riid, &IID_IDXGIAdapter) || guid_eq(riid, &IID_IDXGIAdapter1)) {
		*ppv = this;
		this->lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI Adp_AddRef(IDXGIAdapter1 *this)
{
	return (ULONG)InterlockedIncrement(&((Sw11Adapter *)this)->ref);
}

static ULONG WINAPI Adp_Release(IDXGIAdapter1 *this)
{
	Sw11Adapter *a = (Sw11Adapter *)this;
	LONG n = InterlockedDecrement(&a->ref);
	if (n == 0) {
		if (a->factory)
			a->factory->iface.lpVtbl->Release(&a->factory->iface);
		obj_free(a);
	}
	return (ULONG)n;
}

static HRESULT WINAPI Adp_SetPD(IDXGIAdapter1 *this, REFGUID g, UINT n, const void *d)
{
	(void)this;
	(void)g;
	(void)n;
	(void)d;
	return S_OK;
}

static HRESULT WINAPI Adp_SetPDI(IDXGIAdapter1 *this, REFGUID g, const IUnknown *o)
{
	(void)this;
	(void)g;
	(void)o;
	return S_OK;
}

static HRESULT WINAPI Adp_GetPD(IDXGIAdapter1 *this, REFGUID g, UINT *n, void *d)
{
	(void)this;
	(void)g;
	(void)n;
	(void)d;
	return DXGI_ERROR_NOT_FOUND;
}

static HRESULT WINAPI Adp_GetParent(IDXGIAdapter1 *this, REFIID riid, void **pp)
{
	Sw11Adapter *a = (Sw11Adapter *)this;
	if (!pp)
		return E_POINTER;
	return a->factory->iface.lpVtbl->QueryInterface(&a->factory->iface, riid, pp);
}

static void fill_adapter_desc(DXGI_ADAPTER_DESC *d)
{
	memset(d, 0, sizeof(*d));
	wcscpy(d->Description, L"d3d11_sw software raster");
	d->VendorId = 0x1414;
	d->DeviceId = 0x008c;
	d->DedicatedVideoMemory = 512u * 1024u * 1024u;
	d->DedicatedSystemMemory = 0;
	d->SharedSystemMemory = 2u * 1024u * 1024u * 1024u;
}

/* The monitors that actually exist.
 *
 * This used to answer every query with one hardcoded output called
 * \\.\DISPLAY1, sized to the primary monitor and placed at the origin. On a
 * single-monitor machine that is accidentally correct. On any other it is
 * wrong twice over: DISPLAY1 is not necessarily the primary - the name is
 * assigned by the display driver, not by which screen the user chose - and a
 * game that positions itself from the output it is given therefore opens on
 * whichever panel happens to be called DISPLAY1. Reporting only one output
 * also means a game offering a monitor picker has nothing to put in it.
 *
 * So enumerate them properly, and order the list with the primary first,
 * because a game that does not offer a choice takes output 0. */
#define MAX_OUTPUTS 8
static struct sw_monitor {
	HMONITOR mon;
	RECT rc;
	int primary;
	WCHAR name[32];
} g_monitors[MAX_OUTPUTS];
static UINT g_n_monitors;

static BOOL CALLBACK mon_collect(HMONITOR mon, HDC dc, LPRECT rc, LPARAM p)
{
	MONITORINFOEXW mi;
	(void)dc;
	(void)rc;
	(void)p;
	if (g_n_monitors >= MAX_OUTPUTS)
		return FALSE;
	memset(&mi, 0, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi))
		return TRUE;
	g_monitors[g_n_monitors].mon = mon;
	g_monitors[g_n_monitors].rc = mi.rcMonitor;
	g_monitors[g_n_monitors].primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;
	wcsncpy(g_monitors[g_n_monitors].name, mi.szDevice,
		sizeof(g_monitors[0].name) / sizeof(WCHAR) - 1);
	g_n_monitors++;
	return TRUE;
}

static void monitors_once(void)
{
	static LONG done;
	UINT i;

	if (InterlockedCompareExchange(&done, 1, 0) != 0)
		return;
	EnumDisplayMonitors(NULL, NULL, mon_collect, 0);
	/* Primary to the front, order of the rest preserved. */
	for (i = 0; i < g_n_monitors; i++) {
		if (!g_monitors[i].primary)
			continue;
		if (i) {
			struct sw_monitor t = g_monitors[i];
			while (i > 0) {
				g_monitors[i] = g_monitors[i - 1];
				i--;
			}
			g_monitors[0] = t;
		}
		break;
	}
	if (!g_n_monitors) {
		/* Nothing enumerated is not a state worth crashing over. */
		g_monitors[0].rc.right = GetSystemMetrics(SM_CXSCREEN);
		g_monitors[0].rc.bottom = GetSystemMetrics(SM_CYSCREEN);
		g_monitors[0].primary = 1;
		wcscpy(g_monitors[0].name, L"\\\\.\\DISPLAY1");
		g_n_monitors = 1;
	}
	for (i = 0; i < g_n_monitors; i++)
		d11_log("monitor %u: %ls %ldx%ld at %ld,%ld%s", i, g_monitors[i].name,
			g_monitors[i].rc.right - g_monitors[i].rc.left,
			g_monitors[i].rc.bottom - g_monitors[i].rc.top, g_monitors[i].rc.left,
			g_monitors[i].rc.top, g_monitors[i].primary ? " (primary)" : "");
}

/* Which monitor the game should open on. Default 0, which after the ordering
 * above is the primary - the answer a player expects when they have not asked
 * for anything else. */
static int g_monitor_forced;

static UINT want_monitor(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[16];
		unsigned n = savestate_getenv("D3D11SW_MONITOR", buf, sizeof(buf));
		unsigned i;
		int acc = 0;

		for (i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
			acc = acc * 10 + (buf[i] - '0');
		v = n ? acc : 0;
		g_monitor_forced = n ? 1 : 0;
	}
	monitors_once();
	return (UINT)v < g_n_monitors ? (UINT)v : 0;
}

/* Put the window where the player asked for it.
 *
 * Reporting the outputs truthfully fixes a game that derives its position from
 * DXGI, but not one that remembers a position in a settings file or picks a
 * screen by its own logic. Since the window is ours to see and the monitor
 * geometry is now known, move it. Only when asked: silently relocating a window
 * that opened exactly where someone wanted it is its own bug. */
static void place_on_monitor(HWND hwnd)
{
	UINT want = want_monitor();
	const struct sw_monitor *m;
	RECT wr;
	int w, h, x, y;

	if (!g_monitor_forced || !hwnd || !IsWindow(hwnd))
		return;
	m = &g_monitors[want];
	if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == m->mon)
		return; /* already there */
	if (!GetWindowRect(hwnd, &wr))
		return;
	w = wr.right - wr.left;
	h = wr.bottom - wr.top;
	x = m->rc.left + ((m->rc.right - m->rc.left) - w) / 2;
	y = m->rc.top + ((m->rc.bottom - m->rc.top) - h) / 2;
	if (x < m->rc.left)
		x = m->rc.left;
	if (y < m->rc.top)
		y = m->rc.top;
	SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	d11_log("moved the window to monitor %u (%ls) at %d,%d", want, m->name, x, y);
}

static HRESULT WINAPI Adp_EnumOutputs(IDXGIAdapter1 *this, UINT i, IDXGIOutput **out)
{
	Sw11Output *o;
	if (!out)
		return E_POINTER;
	*out = NULL;
	monitors_once();
	d11_log("EnumOutputs %u -> %s", i, i < g_n_monitors ? "output" : "NOT_FOUND");
	if (i >= g_n_monitors)
		return DXGI_ERROR_NOT_FOUND;
	o = (Sw11Output *)pin_calloc(sizeof(*o), "output", PK_SINGLE);
	if (!o)
		return E_OUTOFMEMORY;
	ensure_vtbls();
	o->iface.lpVtbl = &kOutVtbl;
	o->ref = 1;
	o->index = i;
	o->adapter = (Sw11Adapter *)this;
	this->lpVtbl->AddRef(this);
	*out = &o->iface;
	return S_OK;
}

static HRESULT WINAPI Adp_GetDesc(IDXGIAdapter1 *this, DXGI_ADAPTER_DESC *d)
{
	(void)this;
	if (!d)
		return E_POINTER;
	fill_adapter_desc(d);
	return S_OK;
}

static HRESULT WINAPI Adp_CheckInterfaceSupport(IDXGIAdapter1 *this, REFGUID g, LARGE_INTEGER *u)
{
	(void)this;
	(void)g;
	if (u)
		u->QuadPart = 0;
	return S_OK;
}

static HRESULT WINAPI Adp_GetDesc1(IDXGIAdapter1 *this, DXGI_ADAPTER_DESC1 *d)
{
	DXGI_ADAPTER_DESC base;
	(void)this;
	if (!d)
		return E_POINTER;
	fill_adapter_desc(&base);
	memset(d, 0, sizeof(*d));
	memcpy(d, &base, sizeof(base));
	d->Flags = 0;
	return S_OK;
}

static HRESULT Adp_create(Sw11Factory *f, IDXGIAdapter1 **out)
{
	Sw11Adapter *a = (Sw11Adapter *)pin_calloc(sizeof(*a), "adapter", PK_SINGLE);
	if (!a)
		return E_OUTOFMEMORY;
	ensure_vtbls();
	a->iface.lpVtbl = &kAdpVtbl;
	a->ref = 1;
	a->factory = f;
	f->iface.lpVtbl->AddRef(&f->iface);
	*out = &a->iface;
	return S_OK;
}

static HRESULT WINAPI Out_QI(IDXGIOutput *this, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IDXGIObject) ||
	    guid_eq(riid, &IID_IDXGIOutput)) {
		*ppv = this;
		this->lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI Out_AddRef(IDXGIOutput *this)
{
	return (ULONG)InterlockedIncrement(&((Sw11Output *)this)->ref);
}

static ULONG WINAPI Out_Release(IDXGIOutput *this)
{
	Sw11Output *o = (Sw11Output *)this;
	LONG n = InterlockedDecrement(&o->ref);
	if (n == 0) {
		if (o->adapter)
			o->adapter->iface.lpVtbl->Release(&o->adapter->iface);
		obj_free(o);
	}
	return (ULONG)n;
}

static HRESULT WINAPI Out_SetPD(IDXGIOutput *this, REFGUID g, UINT n, const void *d)
{
	(void)this;
	(void)g;
	(void)n;
	(void)d;
	return S_OK;
}
static HRESULT WINAPI Out_SetPDI(IDXGIOutput *this, REFGUID g, const IUnknown *o)
{
	(void)this;
	(void)g;
	(void)o;
	return S_OK;
}
static HRESULT WINAPI Out_GetPD(IDXGIOutput *this, REFGUID g, UINT *n, void *d)
{
	(void)this;
	(void)g;
	(void)n;
	(void)d;
	return DXGI_ERROR_NOT_FOUND;
}
static HRESULT WINAPI Out_GetParent(IDXGIOutput *this, REFIID riid, void **pp)
{
	Sw11Output *o = (Sw11Output *)this;
	if (!pp)
		return E_POINTER;
	return o->adapter->iface.lpVtbl->QueryInterface(&o->adapter->iface, riid, pp);
}

static HRESULT WINAPI Out_GetDesc(IDXGIOutput *this, DXGI_OUTPUT_DESC *d)
{
	Sw11Output *o = (Sw11Output *)this;
	const struct sw_monitor *m;

	if (!d)
		return E_POINTER;
	monitors_once();
	m = &g_monitors[o && o->index < g_n_monitors ? o->index : 0];
	memset(d, 0, sizeof(*d));
	wcsncpy(d->DeviceName, m->name, sizeof(d->DeviceName) / sizeof(WCHAR) - 1);
	/* The real position in the virtual desktop, not the origin: a game that
	 * places its window from this is otherwise told every monitor starts at
	 * 0,0 and puts the window on whichever one actually does. */
	d->DesktopCoordinates = m->rc;
	d->Monitor = m->mon;
	d->AttachedToDesktop = TRUE;
	d->Rotation = DXGI_MODE_ROTATION_IDENTITY;
	return S_OK;
}

static HRESULT WINAPI Out_GetDisplayModeList(IDXGIOutput *this, DXGI_FORMAT fmt, UINT flags,
					       UINT *n, DXGI_MODE_DESC *m)
{
	/* A game builds its resolution menu from this list, so reporting only the
	 * desktop mode leaves the player no way to pick a cheaper one. That
	 * matters more on a CPU rasteriser than on a GPU: resolution is the
	 * largest single lever over frame time. Standard 16:9 modes up to the
	 * desktop size, ascending, which is the order these lists arrive in. */
	static const UINT modes[][2] = { { 640, 360 },	{ 854, 480 },	{ 960, 540 },
					 { 1024, 576 }, { 1280, 720 },	{ 1366, 768 },
					 { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 },
					 { 3840, 2160 } };
	UINT dw = (UINT)GetSystemMetrics(SM_CXSCREEN);
	UINT dh = (UINT)GetSystemMetrics(SM_CYSCREEN);
	UINT avail = 0, i, cap;
	(void)this;
	(void)fmt;
	(void)flags;
	if (!n)
		return E_POINTER;
	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
		if (modes[i][0] <= dw && modes[i][1] <= dh)
			avail++;
	if (!avail)
		avail = 1;
	if (!m) {
		d11_log("GetDisplayModeList count query fmt=%d -> %u modes", (int)fmt, avail);
		*n = avail;
		return S_OK;
	}
	if (*n < avail) {
		*n = avail;
		return DXGI_ERROR_MORE_DATA;
	}
	d11_log("GetDisplayModeList fmt=%d flags=%x asked=%u avail=%u fill=%d", (int)fmt, flags,
		*n, avail, m ? 1 : 0);
	cap = 0;
	for (i = 0; i < sizeof(modes) / sizeof(modes[0]) && cap < avail; i++) {
		if (modes[i][0] > dw || modes[i][1] > dh)
			continue;
		memset(&m[cap], 0, sizeof(m[cap]));
		m[cap].Width = modes[i][0];
		m[cap].Height = modes[i][1];
		m[cap].RefreshRate.Numerator = 60;
		m[cap].RefreshRate.Denominator = 1;
		m[cap].Format = fmt ? fmt : DXGI_FORMAT_R8G8B8A8_UNORM;
		m[cap].ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
		m[cap].Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
		cap++;
	}
	if (!cap) {
		memset(&m[0], 0, sizeof(m[0]));
		m[0].Width = dw;
		m[0].Height = dh;
		m[0].RefreshRate.Numerator = 60;
		m[0].RefreshRate.Denominator = 1;
		m[0].Format = fmt ? fmt : DXGI_FORMAT_R8G8B8A8_UNORM;
		m[0].ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
		cap = 1;
	}
	*n = cap;
	return S_OK;
}

static HRESULT WINAPI Out_FindClosestMatchingMode(IDXGIOutput *this, const DXGI_MODE_DESC *in,
						     DXGI_MODE_DESC *out, IUnknown *dev)
{
	DXGI_MODE_DESC list[16];
	UINT n = 16, i, best = 0;
	double bestd = 0;
	HRESULT hr;
	(void)dev;
	if (!out)
		return E_POINTER;
	hr = Out_GetDisplayModeList(this, in ? in->Format : DXGI_FORMAT_R8G8B8A8_UNORM, 0, &n,
				    list);
	if (FAILED(hr) || !n)
		return hr;
	/* Nearest by pixel-count difference; the caller's request need not be one
	 * of the modes we advertise. */
	if (in && in->Width && in->Height) {
		double want = (double)in->Width * (double)in->Height;
		for (i = 0; i < n; i++) {
			double d = (double)list[i].Width * (double)list[i].Height - want;
			if (d < 0)
				d = -d;
			if (i == 0 || d < bestd) {
				bestd = d;
				best = i;
			}
		}
	} else {
		best = n - 1;
	}
	*out = list[best];
	if (in && in->RefreshRate.Numerator)
		out->RefreshRate = in->RefreshRate;
	return S_OK;
}

static HRESULT WINAPI Out_WaitForVBlank(IDXGIOutput *this)
{
	(void)this;
	return S_OK;
}
static HRESULT WINAPI Out_TakeOwnership(IDXGIOutput *this, IUnknown *d, BOOL e)
{
	(void)this;
	(void)d;
	(void)e;
	return S_OK;
}
static void WINAPI Out_ReleaseOwnership(IDXGIOutput *this)
{
	(void)this;
}
static HRESULT WINAPI Out_GetGammaControlCapabilities(IDXGIOutput *this,
							 DXGI_GAMMA_CONTROL_CAPABILITIES *c)
{
	(void)this;
	if (c)
		memset(c, 0, sizeof(*c));
	return S_OK;
}
static HRESULT WINAPI Out_SetGammaControl(IDXGIOutput *this, const DXGI_GAMMA_CONTROL *c)
{
	(void)this;
	(void)c;
	return S_OK;
}
static HRESULT WINAPI Out_GetGammaControl(IDXGIOutput *this, DXGI_GAMMA_CONTROL *c)
{
	(void)this;
	if (c)
		memset(c, 0, sizeof(*c));
	return S_OK;
}
static HRESULT WINAPI Out_SetDisplaySurface(IDXGIOutput *this, IDXGISurface *s)
{
	(void)this;
	(void)s;
	return S_OK;
}
static HRESULT WINAPI Out_GetDisplaySurfaceData(IDXGIOutput *this, IDXGISurface *s)
{
	(void)this;
	(void)s;
	return S_OK;
}
static HRESULT WINAPI Out_GetFrameStatistics(IDXGIOutput *this, DXGI_FRAME_STATISTICS *s)
{
	(void)this;
	if (s)
		memset(s, 0, sizeof(*s));
	return S_OK;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void **pp);

static Sw11Factory *factory_new(void)
{
	Sw11Factory *f = (Sw11Factory *)pin_calloc(sizeof(*f), "factory", PK_SINGLE);
	if (!f)
		return NULL;
	ensure_vtbls();
	f->iface.lpVtbl = &kFactVtbl;
	f->ref = 1;
	return f;
}

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **pp)
{
	return CreateDXGIFactory2(0, riid, pp);
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **pp)
{
	return CreateDXGIFactory2(0, riid, pp);
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void **pp)
{
	Sw11Factory *f;
	(void)flags;
	/* Windows' own d3d11.dll imports exactly one thing from dxgi.dll, and
	 * this is it. When it is the caller, our software factory is not an
	 * answer - it will be used as a real IDXGIFactory2 and the call will walk
	 * off a vtable that does not match. The probe raises a flag around its
	 * device creation so the real factory can be handed over for the length
	 * of that one call, and the game's own calls are untouched. */
	if (gpuprobe_wants_real_factory()) {
		HRESULT (WINAPI *real)(UINT, REFIID, void **) =
			(HRESULT(WINAPI *)(UINT, REFIID, void **))gpuprobe_real_factory_fn();

		if (real)
			return real(flags, riid, pp);
		return E_FAIL;
	}
	savestate_hooks_install();
	ensure_vtbls();
	if (!pp)
		return E_POINTER;
	*pp = NULL;
	f = factory_new();
	if (!f)
		return E_OUTOFMEMORY;
	d11_log("CreateDXGIFactory2");
	return f->iface.lpVtbl->QueryInterface(&f->iface, riid, pp) == S_OK
		       ? (f->iface.lpVtbl->Release(&f->iface), S_OK)
		       : (f->iface.lpVtbl->Release(&f->iface), E_NOINTERFACE);
}

/* ---------- resources / views / state ---------- */

static ULONG WINAPI Res_AddRef(Sw11Res *r)
{
	return (ULONG)InterlockedIncrement(&r->ref);
}

/* Objects the game can still name after a rewind are retired, not freed.
 *
 * Our objects are the seam between two different moments. The game's pointers to
 * them live in the game's memory, which travels back in time; the objects live
 * in our heap, which does not. So the game can come out of a restore holding a
 * pointer it acquired before the save, to an object it released after the save,
 * whose block our allocator has since handed to something else entirely.
 *
 * That is not a theory. The crash was at GfxDeviceD3D11Base::PresentFrame+0x1e7,
 * which the symbols show walking a std::list<ID3D11Query *> and calling vtable
 * slot 0x10 - Release - on each entry. Unity's list came back from the save,
 * our queries did not, and the first word of the block it called through was
 * whatever now occupies that memory. It faulted on the first present after every
 * restore, in three different heap configurations, because present is the first
 * thing that happens after any restore.
 *
 * Keeping the header keeps the vtable, so a stale Release lands on real code and
 * decrements a refcount nobody reads. The payload still goes back to the heap,
 * which is where the bytes actually are - a retired texture costs its header,
 * not its pixels. Pointers are cleared rather than left dangling so that code
 * reached through a stale handle sees nothing instead of freed memory. */
static int retire_objects(void)
{
	static int v = -1;

	if (v < 0) {
		const char *s = getenv("D3D11SW_RETIRE_OBJECTS");
		v = (s && s[0] == '0') ? 0 : 1;
	}
	return v;
}

/* A cap on backbuffer height, for games that give the player no way to set one.
 *
 * Resolution is the largest single lever over frame time on a CPU rasteriser,
 * and it is normally the game's own menu that offers it - which is why the fix
 * for OSFE was to stop reporting a one-entry display mode list and let Unity
 * build a real dropdown. Some games have no such menu at all. Rabbit and
 * Steel's graphics options have a DISPLAY mode toggle and nothing else, so in
 * fullscreen it takes the desktop size and every one of those pixels is shaded
 * here. Measured in that game: ~47 ms of raster per frame at 2560x1440 against
 * ~19.7 ms at 1600x900, so raster time tracks pixel count almost exactly.
 *
 * Capping costs nothing to display. swrast_present already ends in
 * StretchDIBits from the backbuffer size to the client rect, so a smaller
 * buffer is upscaled by GDI inside the blit that was happening anyway.
 *
 * The cap is reported truthfully through GetDesc, because a game sizes its own
 * render targets and viewports from the backbuffer and a lie there would break
 * drawing outright. That has one consequence, and it is the reason this is off
 * by default: a game that reads the mouse in window client pixels and tests it
 * against a UI laid out in backbuffer pixels will have its hit-testing offset
 * by the scale factor. Whether it does is a property of the game and not
 * something this wrapper can correct, so it needs checking by eye per title. */
static UINT render_max_h(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[16];
		unsigned n = savestate_getenv("D3D11SW_MAX_HEIGHT", buf, sizeof(buf));
		unsigned i;
		int acc = 0;

		for (i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
			acc = acc * 10 + (buf[i] - '0');
		/* Below this a backbuffer is not a cheap resolution, it is a typo. */
		v = acc >= 64 ? acc : 0;
	}
	return (UINT)v;
}

/* The cap arithmetic on its own, so the backbuffer and the render-target
 * scaler cannot drift apart in how they round. Returns 1 if it changed
 * anything. */
static int scale_to_cap(UINT *w, UINT *h)
{
	UINT cap = render_max_h();
	UINT ow = *w, oh = *h;

	if (!cap || !ow || !oh || oh <= cap)
		return 0;
	/* Width from the original ratio in one rounded division, so the common
	 * case of 2560x1440 capped to 720 gives exactly 1280. */
	*w = (UINT)(((unsigned long long)ow * cap + oh / 2) / oh);
	*h = cap;
	if (!*w)
		*w = 1;
	return 1;
}

/* Accepts 0, n[o], f[alse] and off as false; anything else present is true.
 *
 * Tested past the first character on purpose. This project's thread-policy
 * knob read only buf[0], and since "on" and "off" share it, anyone who wrote
 * the setting out in full to be explicit got the exact opposite, silently. */
static int knob_bool(const char *s, unsigned n)
{
	if (!n)
		return 0;
	if (s[0] == '0' || s[0] == 'n' || s[0] == 'N' || s[0] == 'f' || s[0] == 'F')
		return 0;
	if (n >= 3 && (s[0] == 'o' || s[0] == 'O') && (s[1] == 'f' || s[1] == 'F'))
		return 0;
	return 1;
}

/* Should the cap apply to the game's OWN render targets, not just the
 * backbuffer?
 *
 * Separate from D3D11SW_MAX_HEIGHT and off by default, because capping the
 * backbuffer is safe and this is not. The measurement that forced it: with the
 * backbuffer capped to 1280x720 in Rabbit and Steel, the backbuffer carried
 * ONE draw and 0.92 Mpx per frame while the game's own target #638 stayed at
 * 2560x1440 and carried 62 draws and 45.83 Mpx. The game sizes its offscreen
 * targets from the display rather than from the swapchain, so the backbuffer
 * cap shrank the final composite and left the entire scene at native size.
 *
 * What makes scaling them viable at all is that the composite is a DRAW - a
 * full-screen quad sampling the scene target - and texture sampling is in
 * normalised 0..1 coordinates, which mean the same thing at any size. What
 * breaks is anything that addresses a scaled surface in PIXELS: the copy paths
 * (converted below), and Map or readback (warned about, not corrected, because
 * the game's buffer is sized to what it asked for). */
/* On unless explicitly disabled: honouring the interval is what a swap chain
 * is supposed to do, and free-running is the special case worth asking for. */
static int vsync_on(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[16];
		unsigned n = savestate_getenv("D3D11SW_VSYNC", buf, sizeof(buf));

		v = n ? knob_bool(buf, n) : 1;
	}
	return v;
}

static int scale_rt_on(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[16];
		unsigned n = savestate_getenv("D3D11SW_SCALE_RT", buf, sizeof(buf));

		v = knob_bool(buf, n);
	}
	return v;
}

/* The factor between what the game thinks this target is and what it really
 * is. 1.0 for everything unscaled, which is every resource unless the knob is
 * on. */
static float rt_scale_x(const Sw11Res *r)
{
	if (!r || !r->virt_w || r->virt_w == r->width)
		return 1.0f;
	return (float)r->width / (float)r->virt_w;
}

static float rt_scale_y(const Sw11Res *r)
{
	if (!r || !r->virt_h || r->virt_h == r->height)
		return 1.0f;
	return (float)r->height / (float)r->virt_h;
}

/* Scales a requested backbuffer down so its height is at most the cap, keeping
 * the requested aspect ratio. */
static void clamp_backbuffer(UINT *w, UINT *h, const char *who)
{
	UINT cap = render_max_h();
	UINT ow = *w, oh = *h;
	static LONG said;

	/* Stated once whether or not it bites. Without this, "no cap line in the
	 * log" means either that the knob is off or that it was set and never
	 * read, and those need telling apart - a setting that cannot be confirmed
	 * is a setting that cannot be tested. Note the cfg is opened by relative
	 * path, exactly like the log, so it is found only if the game's working
	 * directory is its install folder. */
	if (InterlockedIncrement(&said) == 1) {
		if (cap)
			d11_log("backbuffer height cap: D3D11SW_MAX_HEIGHT=%u is ACTIVE, "
				"first request %ux%u, render-target scaling (D3D11SW_SCALE_RT) "
				"is %s",
				cap, ow, oh, scale_rt_on() ? "ON" : "off");
		else
			d11_log("backbuffer height cap: off (set D3D11SW_MAX_HEIGHT in the "
				"environment or in d3d9_sw.cfg beside the log to cap it), "
				"first request %ux%u",
				ow, oh);
	}
	if (!scale_to_cap(w, h))
		return;
	/* Only when it changes. Rabi-Ribi calls ResizeBuffers once per present,
	 * so an unconditional line here is one per frame. */
	{
		static UINT last_w, last_h;
		if (*w != last_w || *h != last_h) {
			last_w = *w;
			last_h = *h;
			d11_log("%s: capping backbuffer %ux%u -> %ux%u "
				"(D3D11SW_MAX_HEIGHT=%u), present upscales it to the "
				"window. If mouse hit-testing comes out offset, this "
				"game maps input in window pixels and the cap has to "
				"come off",
				who, ow, oh, *w, *h, cap);
		}
	}
}

/* Cap the backbuffer, and decide whether to admit it to the game.
 *
 * Two games want opposite things here. Rabbit and Steel reads the backbuffer
 * size back and sizes its viewport from it, so the honest answer is the useful
 * one: report the capped size and it renders a correctly framed smaller
 * picture. Rabi-Ribi never asks. It sets a 1280x720 viewport because that is
 * what it always renders at, so a truthfully capped 640x360 backbuffer
 * receives a full-size image of which only the top-left quarter lands in the
 * buffer - which present then upscales, giving a 2x zoom into the corner.
 *
 * For that second kind the surface has to shrink while the reported size stays
 * put, which is what virt_w/virt_h already express for game-created targets:
 * the viewport, the scissor and the copy paths all scale by rt_scale_x/y off
 * the difference. So reuse it here and let D3D11SW_SCALE_RT pick, since that
 * knob already means "scale render targets behind the game's back" and was
 * off for Rabbit and Steel, whose behaviour is therefore unchanged.
 *
 * Returns the real allocation size in w/h, and in vw/vh either the size to
 * keep reporting or zero when the honest size is being reported. */
static void backbuffer_cap(UINT *w, UINT *h, UINT *vw, UINT *vh, const char *who)
{
	UINT lw = *w, lh = *h;

	*vw = 0;
	*vh = 0;
	clamp_backbuffer(w, h, who);
	if (scale_rt_on() && (*w != lw || *h != lh)) {
		*vw = lw;
		*vh = lh;
	}
	_snprintf(g_bb_desc, sizeof(g_bb_desc) - 1,
		  "backbuffer is really %ux%u (%u bytes), reported to the game as %ux%u "
		  "(%u bytes)",
		  *w, *h, *w * *h * 4, *vw ? *vw : *w, *vh ? *vh : *h,
		  (*vw ? *vw : *w) * (*vh ? *vh : *h) * 4);
	g_bb_desc[sizeof(g_bb_desc) - 1] = 0;
}

static LONG g_retired_n;
static LONG g_retired_bytes;

/* Retired objects whose texels were kept, and the running cost of keeping them. */
static Sw11Res *g_ret_head;
static volatile LONG g_ret_lock;
static LONG g_ret_kept_n;
static double g_ret_kept_bytes;
static LONG g_ret_capped;

/* How much texture content may be held back for a savestate, in MB. */
static double retain_cap_mb(void)
{
	static double v = -1.0;

	if (v < 0.0) {
		const char *s = getenv("D3D11SW_RETAIN_MB");
		v = 512.0;
		if (s && s[0]) {
			double d = 0.0;
			int i;
			for (i = 0; s[i] >= '0' && s[i] <= '9'; i++)
				d = d * 10.0 + (s[i] - '0');
			v = d;
		}
	}
	return v;
}

/* Whether a released object's PIXELS must be kept, not just its header.
 *
 * Retirement was built to stop a crash and it does, but it answers only half the
 * question. The game comes out of a restore holding a pointer to a texture it
 * released after the save; keeping the header means that pointer finds an object
 * rather than freed memory, and keeping the header ALONE means the object it
 * finds is empty. The draw path then drops the sprite, which is the missing
 * artwork after a reload - not a Unity fault, and not the render-path crash
 * either. It is this decision.
 *
 * A savestate is a claim that the program can go back to a moment. Everything
 * visible at that moment is part of the moment, so texture content released
 * afterwards is not garbage yet - it is state the snapshot still refers to. The
 * memory is real and worst case is roughly one scene's texture set, since a
 * quit-to-menu releases the lot; the cap keeps that bounded and says so out loud
 * rather than growing without limit. */
/* Whether the address space can still afford to retain anything.
 *
 * The megabyte budget above cannot know what else is going on. Rabi-Ribi was
 * already holding 1473 MB of VA at the moment of the save, so a 512 MB budget
 * aimed the retained set at 1985 MB - the 2 GB wall, near enough to touch. The
 * session bore that out: it ran on to 1901 MB with 19 MB as its largest free
 * block, which is not heading toward exhaustion, it is already there.
 *
 * So the real limit is not a number chosen in advance, it is how much room is
 * left. Retention stops while a floor of free space remains, which costs
 * sprites released late in a long session but keeps the process alive to draw
 * the rest. Sampled rather than measured per release: walking the whole address
 * space runs to hundreds of regions and Res_Release is on the hot path. */
static int va_room_to_retain(void)
{
	static double floor_mb = -1.0;
	static DWORD next_check;
	static int answer = 1;
	DWORD now;

	if (floor_mb < 0.0) {
		const char *s = getenv("D3D11SW_RETAIN_FLOOR_MB");

		floor_mb = (s && s[0]) ? strtod(s, NULL) : 384.0;
	}
	if (floor_mb <= 0.0)
		return 1;
	now = GetTickCount();
	if ((int)(now - next_check) >= 0) {
		double used = 0.0, freed = 0.0, largest = 0.0;

		next_check = now + 250;
		va_stats(&used, &freed, &largest);
		if (answer && freed < floor_mb) {
			answer = 0;
			d11_log("retention stopped with %.0f MB of address space free, under "
				"the %.0f MB floor - textures released from here on lose their "
				"pixels and may be missing after a restore. This is the "
				"alternative to running out of address space entirely",
				freed, floor_mb);
		} else if (!answer && freed > floor_mb * 1.5) {
			answer = 1;
			d11_log("retention resumed, %.0f MB of address space free", freed);
		}
	}
	return answer;
}

/* Retains declined because the object postdates every snapshot. */
static volatile LONG g_ret_skipped_new;

/* Whether THIS object's pixels have to be kept.
 *
 * The rule above - keep what a snapshot may still refer to - was applied to
 * every release while a savestate existed, which is broader than the rule
 * itself. An object created AFTER the save cannot be referred to by that save:
 * the game's memory at the snapshot held no pointer to it, because it did not
 * exist yet, and a restore puts that memory back exactly as it was. So the
 * game comes out of the restore unable to name it, by construction.
 *
 * Retaining it therefore buys nothing and costs twice. It costs the payload,
 * which is most of the growth: a restore-heavy session releases mostly objects
 * made since the save, and they were all being kept. And it costs correctness,
 * because ledger_reap frees post-save payloads on restore while the retain list
 * goes on pointing at them - a later flush then frees the same address twice,
 * or worse, frees whatever has since been allocated over it.
 *
 * This is the same line the ledger draws with its mark, applied to the objects
 * rather than to their memory, which is why drawing it here makes the two agree
 * instead of quietly contradicting each other. */
static int retain_payload(Sw11Res *r)
{
	int k;

	if (!retire_objects())
		return 0;
	if (r && (r->born_gen >= g_save_gen || r->plane_gen >= g_save_gen)) {
		InterlockedIncrement(&g_ret_skipped_new);
		return 0;
	}
	if (!va_room_to_retain())
		return 0;
	/* Read under the lock. A double is eight bytes and this is a 32-bit
	 * target, so an unlocked read while ret_push is adding to it can catch
	 * one half updated and the other not - a value that is not merely stale
	 * but was never held, which lands either side of the cap and so either
	 * stops retention early or ignores the budget entirely. */
	{
		double kept;

		while (InterlockedCompareExchange(&g_ret_lock, 1, 0) != 0)
			YieldProcessor();
		kept = g_ret_kept_bytes;
		InterlockedExchange(&g_ret_lock, 0);
		if (kept >= retain_cap_mb() * 1024.0 * 1024.0) {
			if (InterlockedIncrement(&g_ret_capped) == 1)
				d11_log("retained texture budget of %.0f MB is full - "
					"further released textures lose their pixels and "
					"their sprites will be missing after a restore. "
					"Raise D3D11SW_RETAIN_MB or save less often",
					retain_cap_mb());
			return 0;
		}
	}
	for (k = 0; k < SAVESTATE_SLOTS; k++)
		if (savestate_slot_valid(k))
			return 1;
	return 0;
}

/* Cumulative pushes and flushes, for the same reason the birth and death
 * counters exist: the retain list is only emptied when a new save supersedes
 * the old one, so a session of many restores between two saves has only two
 * chances to give anything back. Whether that is what grew is a question about
 * totals, and g_ret_kept_n only ever reported the current depth. */
static volatile LONG g_ret_pushed, g_ret_flushes;

static void ret_push(Sw11Res *r)
{
	while (InterlockedCompareExchange(&g_ret_lock, 1, 0) != 0)
		YieldProcessor();
	/* A retired object can be brought back to life by an AddRef and released
	 * a second time, which arrives here with the object already on the list.
	 * Linking it again sets ret_next to whatever is at the head - and if that
	 * is this same object, to itself, which makes the flush loop forever and
	 * free the same payload on every turn. Once on the list is enough. */
	if (r->ret_listed) {
		InterlockedExchange(&g_ret_lock, 0);
		return;
	}
	r->ret_listed = 1;
	r->ret_next = g_ret_head;
	g_ret_head = r;
	g_ret_kept_n++;
	g_ret_kept_bytes += (double)r->ret_bytes;
	InterlockedExchange(&g_ret_lock, 0);
	InterlockedIncrement(&g_ret_pushed);
}

/* Releases every kept payload. Called when a new save supersedes the old one:
 * with one slot, anything released before the new snapshot is not referenced by
 * it, because the game had already let it go when the snapshot was taken. */
void d3d11sw_retain_flush(void)
{
	Sw11Res *r;
	LONG n;
	double mb;

	while (InterlockedCompareExchange(&g_ret_lock, 1, 0) != 0)
		YieldProcessor();
	r = g_ret_head;
	g_ret_head = NULL;
	n = g_ret_kept_n;
	mb = g_ret_kept_bytes / (1024.0 * 1024.0);
	g_ret_kept_n = 0;
	g_ret_kept_bytes = 0.0;
	InterlockedExchange(&g_ret_lock, 0);
	InterlockedExchange(&g_ret_capped, 0);
	InterlockedIncrement(&g_ret_flushes);
	while (r) {
		Sw11Res *next = r->ret_next;

		swrast_flush_if_pending(r->pixels);
		swrast_flush_if_pending(r->depth);
		res_free_payload(r, 1);
		r->ret_next = NULL;
		r->ret_bytes = 0;
		r->ret_listed = 0;
		r = next;
	}
	if (n)
		d11_log("released %ld retired texture payload(s), %.2f MB, now that a new "
			"snapshot supersedes the one that needed them",
			(long)n, mb);
}

static ULONG WINAPI Res_Release(Sw11Res *r)
{
	LONG n = InterlockedDecrement(&r->ref);

	/* A retired object's count was forced to zero while the game still held
	 * what it thought was a reference, so the release that follows a restore
	 * takes it below zero. Returning that as a ULONG hands the caller
	 * 0xFFFFFFFF, which reads as a very much alive object and is the opposite
	 * of what happened. Pin it at zero and say zero: the object is retired,
	 * which is the honest answer to "how many references remain". */
	if (n < 0 && r->retired) {
		InterlockedExchange(&r->ref, 0);
		return 0;
	}
	if (n == 0) {
		int keep = retain_payload(r);

		priv_free(r->priv);
		if (!keep) {
			/* Queued draws hold the texel pointer, not a reference,
			 * so a release while the workers are still behind hands
			 * them freed memory. A texture this size goes straight
			 * back to the OS rather than onto a free list, so the
			 * read faults instead of quietly returning rubbish. */
			swrast_flush_if_pending(r->pixels);
			swrast_flush_if_pending(r->depth);
			res_free_payload(r, 1);
		}
		if (retire_objects()) {
			r->priv = NULL;
			if (!keep) {
				r->cpu = NULL;
				r->pixels = NULL;
				r->depth = NULL;
				r->cpu_size = 0;
				r->has_texels = 0;
			}
			/* A retired texture is never drawn again, so its faucet bank is
			 * dead weight - drop it even when the payload is retained. */
			free(r->bank);
			r->bank = NULL;
			r->bank_size = 0;
			r->retired = 1;
			InterlockedExchange(&r->ref, 0);
			InterlockedIncrement(&g_retired_n);
			InterlockedExchangeAdd(&g_retired_bytes, (LONG)sizeof(*r));
			if (keep) {
				/* The same reckoning the free path uses, which
				 * skips an aliased pixels plane because it is
				 * r->cpu under another name. Counting it twice
				 * inflated the retain budget and stopped
				 * retention early, losing sprites to a cap that
				 * had not really been reached. */
				r->ret_bytes = (size_t)res_payload_bytes(r);
				/* Hand ownership of these payloads to the retain
				 * list alone. Left in the ledger, a payload
				 * allocated after the save is freed by
				 * ledger_reap on the next restore while this
				 * list still points at it, and the flush after
				 * that frees the same address a second time -
				 * or frees whatever has since been allocated
				 * over it. The flush is now the only route out
				 * for anything held here. */
				ledger_remove(r->cpu);
				if (!r->pixels_alias)
					ledger_remove(r->pixels);
				ledger_remove(r->depth);
				ret_push(r);
			}
			return 0;
		}
		obj_free(r);
	}
	return (ULONG)n;
}

static HRESULT WINAPI Tex_QI(ID3D11Texture2D *this, REFIID riid, void **ppv)
{
	Sw11Res *r = (Sw11Res *)this;
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_ID3D11DeviceChild) ||
	    guid_eq(riid, &IID_ID3D11Resource) || guid_eq(riid, &IID_ID3D11Texture2D)) {
		*ppv = this;
		Res_AddRef(r);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI Tex_AddRef(ID3D11Texture2D *this)
{
	return Res_AddRef((Sw11Res *)this);
}
static ULONG WINAPI Tex_Release(ID3D11Texture2D *this)
{
	return Res_Release((Sw11Res *)this);
}
static void WINAPI Tex_GetDevice(ID3D11Texture2D *this, ID3D11Device **out)
{
	child_get_device(((Sw11Res *)this)->dev, out);
}
static HRESULT WINAPI Tex_GetPD(ID3D11Texture2D *this, REFGUID g, UINT *n, void *d)
{
	return priv_get(((Sw11Res *)this)->priv, g, n, d);
}
static HRESULT WINAPI Tex_SetPD(ID3D11Texture2D *this, REFGUID g, UINT n, const void *d)
{
	return priv_set(&((Sw11Res *)this)->priv, g, n, d);
}
static HRESULT WINAPI Tex_SetPDI(ID3D11Texture2D *this, REFGUID g, const IUnknown *o)
{
	return priv_set(&((Sw11Res *)this)->priv, g, sizeof(o), &o);
}
static void WINAPI Tex_GetType(ID3D11Texture2D *this, D3D11_RESOURCE_DIMENSION *t)
{
	(void)this;
	if (t)
		*t = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
}
static void WINAPI Tex_SetEvict(ID3D11Texture2D *this, UINT p)
{
	(void)this;
	(void)p;
}
static UINT WINAPI Tex_GetEvict(ID3D11Texture2D *this)
{
	(void)this;
	return DXGI_RESOURCE_PRIORITY_NORMAL;
}
static void WINAPI Tex_GetDesc(ID3D11Texture2D *this, D3D11_TEXTURE2D_DESC *d)
{
	Sw11Res *r = (Sw11Res *)this;
	if (!d)
		return;
	memset(d, 0, sizeof(*d));
	/* The requested size for a scaled target, not the real one. The game
	 * lays out its viewports and its UI from this number, and every one of
	 * those goes back through a path that converts, so telling it the truth
	 * here would make it draw a correctly-scaled scene into a corner of the
	 * surface. Note this is the opposite of the backbuffer cap, which IS
	 * reported truthfully - there the game's target is the swapchain and
	 * nothing downstream of us converts. */
	d->Width = r->virt_w ? r->virt_w : r->width;
	d->Height = r->virt_h ? r->virt_h : r->height;
	d->MipLevels = 1;
	d->ArraySize = 1;
	d->Format = r->format;
	d->SampleDesc.Count = 1;
	d->Usage = (D3D11_USAGE)r->usage;
	d->BindFlags = r->bind;
	d->CPUAccessFlags = r->cpu_access;
}

static HRESULT WINAPI Buf_QI(ID3D11Buffer *this, REFIID riid, void **ppv)
{
	Sw11Res *r = (Sw11Res *)this;
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_ID3D11DeviceChild) ||
	    guid_eq(riid, &IID_ID3D11Resource) || guid_eq(riid, &IID_ID3D11Buffer)) {
		*ppv = this;
		Res_AddRef(r);
		return S_OK;
	}
	return E_NOINTERFACE;
}
static ULONG WINAPI Buf_AddRef(ID3D11Buffer *this)
{
	return Res_AddRef((Sw11Res *)this);
}
static ULONG WINAPI Buf_Release(ID3D11Buffer *this)
{
	return Res_Release((Sw11Res *)this);
}
static void WINAPI Buf_GetDevice(ID3D11Buffer *this, ID3D11Device **out)
{
	child_get_device(((Sw11Res *)this)->dev, out);
}
static HRESULT WINAPI Buf_GetPD(ID3D11Buffer *this, REFGUID g, UINT *n, void *d)
{
	return priv_get(((Sw11Res *)this)->priv, g, n, d);
}
static HRESULT WINAPI Buf_SetPD(ID3D11Buffer *this, REFGUID g, UINT n, const void *d)
{
	return priv_set(&((Sw11Res *)this)->priv, g, n, d);
}
static HRESULT WINAPI Buf_SetPDI(ID3D11Buffer *this, REFGUID g, const IUnknown *o)
{
	return priv_set(&((Sw11Res *)this)->priv, g, sizeof(o), &o);
}
static void WINAPI Buf_GetType(ID3D11Buffer *this, D3D11_RESOURCE_DIMENSION *t)
{
	(void)this;
	if (t)
		*t = D3D11_RESOURCE_DIMENSION_BUFFER;
}
static void WINAPI Buf_SetEvict(ID3D11Buffer *this, UINT p)
{
	(void)this;
	(void)p;
}
static UINT WINAPI Buf_GetEvict(ID3D11Buffer *this)
{
	(void)this;
	return DXGI_RESOURCE_PRIORITY_NORMAL;
}
static void WINAPI Buf_GetDesc(ID3D11Buffer *this, D3D11_BUFFER_DESC *d)
{
	Sw11Res *r = (Sw11Res *)this;
	if (!d)
		return;
	memset(d, 0, sizeof(*d));
	d->ByteWidth = r->byte_width;
	d->Usage = (D3D11_USAGE)r->usage;
	d->BindFlags = r->bind;
	d->CPUAccessFlags = r->cpu_access;
}

#define VIEW_QI(name, iid)                                                                          \
	static HRESULT WINAPI name##_QI(void *this, REFIID riid, void **ppv)                     \
	{                                                                                           \
		Sw11View *v = (Sw11View *)this;                                                     \
		if (!ppv)                                                                          \
			return E_POINTER;                                                          \
		*ppv = NULL;                                                                        \
		if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_ID3D11DeviceChild) ||        \
		    guid_eq(riid, &IID_ID3D11View) || guid_eq(riid, &iid)) {                       \
			*ppv = this;                                                               \
			InterlockedIncrement(&v->ref);                                            \
			return S_OK;                                                              \
		}                                                                                   \
		return E_NOINTERFACE;                                                             \
	}

VIEW_QI(Rtv, IID_ID3D11RenderTargetView)
VIEW_QI(Srv, IID_ID3D11ShaderResourceView)
VIEW_QI(Dsv, IID_ID3D11DepthStencilView)

static ULONG WINAPI View_AddRef(void *this)
{
	return (ULONG)InterlockedIncrement(&((Sw11View *)this)->ref);
}
static ULONG WINAPI View_Release(void *this)
{
	Sw11View *v = (Sw11View *)this;
	LONG n = InterlockedDecrement(&v->ref);
	if (n == 0) {
		if (v->res)
			Res_Release(v->res);
		obj_free(v);
	}
	return (ULONG)n;
}
static void WINAPI View_GetDevice(void *this, ID3D11Device **out)
{
	child_get_device(((Sw11View *)this)->dev, out);
}
static HRESULT WINAPI View_GetPD(void *this, REFGUID g, UINT *n, void *d)
{
	(void)this;
	(void)g;
	(void)n;
	(void)d;
	return DXGI_ERROR_NOT_FOUND;
}
static HRESULT WINAPI View_SetPD(void *this, REFGUID g, UINT n, const void *d)
{
	(void)this;
	(void)g;
	(void)n;
	(void)d;
	return S_OK;
}
static HRESULT WINAPI View_SetPDI(void *this, REFGUID g, const IUnknown *o)
{
	(void)this;
	(void)g;
	(void)o;
	return S_OK;
}
static void WINAPI View_GetResource(void *this, ID3D11Resource **out)
{
	Sw11View *v = (Sw11View *)this;
	if (!out)
		return;
	*out = (ID3D11Resource *)&v->res->iface;
	Res_AddRef(v->res);
}
static void WINAPI Rtv_GetDesc(ID3D11RenderTargetView *this, D3D11_RENDER_TARGET_VIEW_DESC *d)
{
	Sw11View *v = (Sw11View *)this;
	if (!d)
		return;
	memset(d, 0, sizeof(*d));
	d->Format = v->format;
	d->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
}
static void WINAPI Srv_GetDesc(ID3D11ShaderResourceView *this, D3D11_SHADER_RESOURCE_VIEW_DESC *d)
{
	Sw11View *v = (Sw11View *)this;
	if (!d)
		return;
	memset(d, 0, sizeof(*d));
	d->Format = v->format;
	d->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	d->Texture2D.MipLevels = 1;
}
static void WINAPI Dsv_GetDesc(ID3D11DepthStencilView *this, D3D11_DEPTH_STENCIL_VIEW_DESC *d)
{
	Sw11View *v = (Sw11View *)this;
	if (!d)
		return;
	memset(d, 0, sizeof(*d));
	d->Format = v->format;
	d->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
}

static Sw11View *view_new(Sw11Device *dev, Sw11Res *res, int kind, DXGI_FORMAT fmt)
{
	Sw11View *v = (Sw11View *)pin_calloc(sizeof(*v), "view", PK_VIEW);
	if (!v)
		return NULL;
	v->ref = 1;
	v->dev = dev;
	v->res = res;
	v->kind = kind;
	v->format = fmt ? fmt : res->format;
	Res_AddRef(res);
	if (kind == 0)
		v->iface.rtv.lpVtbl = &kRtvVtbl;
	else if (kind == 1)
		v->iface.srv.lpVtbl = &kSrvVtbl;
	else
		v->iface.dsv.lpVtbl = &kDsvVtbl;
	return v;
}

#define CHILD_BOILER(T, tag, iid)                                                                   \
	static HRESULT WINAPI tag##_QI(void *this, REFIID riid, void **ppv)                      \
	{                                                                                           \
		if (!ppv)                                                                           \
			return E_POINTER;                                                          \
		*ppv = NULL;                                                                        \
		if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_ID3D11DeviceChild) ||        \
		    guid_eq(riid, &iid)) {                                                         \
			*ppv = this;                                                               \
			InterlockedIncrement(&((T *)this)->ref);                                   \
			return S_OK;                                                              \
		}                                                                                   \
		return E_NOINTERFACE;                                                             \
	}                                                                                           \
	static ULONG WINAPI tag##_AddRef(void *this)                                              \
	{                                                                                           \
		return (ULONG)InterlockedIncrement(&((T *)this)->ref);                             \
	}                                                                                           \
	static ULONG WINAPI tag##_Release(void *this)                                            \
	{                                                                                           \
		T *o = (T *)this;                                                                   \
		LONG n = InterlockedDecrement(&o->ref);                                           \
		if (n == 0) {                                                                       \
			/* Retired rather than freed - see Res_Release. These are the        \
			 * objects the query crash was actually about, and they are          \
			 * small enough that keeping them is cheaper than reasoning          \
			 * about who still points at them. */                                \
			if (retire_objects()) {                                             \
				InterlockedExchange(&o->ref, 0);                            \
				InterlockedIncrement(&g_retired_n);                         \
				InterlockedExchangeAdd(&g_retired_bytes, (LONG)sizeof(*o)); \
				return 0;                                                   \
			}                                                                   \
			obj_free(o);                                                       \
		}                                                                                   \
		return (ULONG)n;                                                                   \
	}                                                                                           \
	static void WINAPI tag##_GetDevice(void *this, ID3D11Device **out)                         \
	{                                                                                           \
		child_get_device(((T *)this)->dev, out);                                           \
	}                                                                                           \
	static HRESULT WINAPI tag##_GetPD(void *this, REFGUID g, UINT *n, void *d)                 \
	{                                                                                           \
		(void)this;                                                                         \
		(void)g;                                                                            \
		(void)n;                                                                            \
		(void)d;                                                                            \
		return DXGI_ERROR_NOT_FOUND;                                                      \
	}                                                                                           \
	static HRESULT WINAPI tag##_SetPD(void *this, REFGUID g, UINT n, const void *d)            \
	{                                                                                           \
		(void)this;                                                                         \
		(void)g;                                                                            \
		(void)n;                                                                            \
		(void)d;                                                                            \
		return S_OK;                                                                       \
	}                                                                                           \
	static HRESULT WINAPI tag##_SetPDI(void *this, REFGUID g, const IUnknown *o)               \
	{                                                                                           \
		(void)this;                                                                         \
		(void)g;                                                                            \
		(void)o;                                                                            \
		return S_OK;                                                                       \
	}

CHILD_BOILER(Sw11Blend, Blend, IID_ID3D11BlendState)
CHILD_BOILER(Sw11DS, Dss, IID_ID3D11DepthStencilState)
CHILD_BOILER(Sw11Rast, Rast, IID_ID3D11RasterizerState)
CHILD_BOILER(Sw11Samp, Samp, IID_ID3D11SamplerState)
CHILD_BOILER(Sw11Query, Query, IID_ID3D11Query)
CHILD_BOILER(Sw11Layout, Lay, IID_ID3D11InputLayout)

static HRESULT WINAPI Sh_QI(void *this, REFIID riid, void **ppv)
{
	Sw11Shader *s = (Sw11Shader *)this;
	const GUID *want = s->is_ps ? &IID_ID3D11PixelShader : &IID_ID3D11VertexShader;
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_ID3D11DeviceChild) ||
	    guid_eq(riid, want)) {
		*ppv = this;
		InterlockedIncrement(&s->ref);
		return S_OK;
	}
	return E_NOINTERFACE;
}
static ULONG WINAPI Sh_AddRef(void *this)
{
	return (ULONG)InterlockedIncrement(&((Sw11Shader *)this)->ref);
}
static ULONG WINAPI Sh_Release(void *this)
{
	Sw11Shader *s = (Sw11Shader *)this;
	LONG n = InterlockedDecrement(&s->ref);
	if (n == 0) {
		free(s->code);
		obj_free(s);
	}
	return (ULONG)n;
}
static void WINAPI Sh_GetDevice(void *this, ID3D11Device **out)
{
	child_get_device(((Sw11Shader *)this)->dev, out);
}
static HRESULT WINAPI Sh_GetPD(void *this, REFGUID g, UINT *n, void *d)
{
	(void)this;
	(void)g;
	(void)n;
	(void)d;
	return DXGI_ERROR_NOT_FOUND;
}
static HRESULT WINAPI Sh_SetPD(void *this, REFGUID g, UINT n, const void *d)
{
	(void)this;
	(void)g;
	(void)n;
	(void)d;
	return S_OK;
}
static HRESULT WINAPI Sh_SetPDI(void *this, REFGUID g, const IUnknown *o)
{
	(void)this;
	(void)g;
	(void)o;
	return S_OK;
}

static void WINAPI Blend_GetDesc(ID3D11BlendState *this, D3D11_BLEND_DESC *d)
{
	if (d)
		*d = ((Sw11Blend *)this)->desc;
}
static void WINAPI Dss_GetDesc(ID3D11DepthStencilState *this, D3D11_DEPTH_STENCIL_DESC *d)
{
	if (d)
		*d = ((Sw11DS *)this)->desc;
}
static void WINAPI Rast_GetDesc(ID3D11RasterizerState *this, D3D11_RASTERIZER_DESC *d)
{
	if (d)
		*d = ((Sw11Rast *)this)->desc;
}
static void WINAPI Samp_GetDesc(ID3D11SamplerState *this, D3D11_SAMPLER_DESC *d)
{
	if (d)
		*d = ((Sw11Samp *)this)->desc;
}
static UINT WINAPI Query_GetDataSize(ID3D11Query *this)
{
	(void)this;
	return sizeof(UINT64);
}
static void WINAPI Query_GetDesc(ID3D11Query *this, D3D11_QUERY_DESC *d)
{
	if (d)
		*d = ((Sw11Query *)this)->desc;
}

static HRESULT create_tex2d(Sw11Device *dev, const D3D11_TEXTURE2D_DESC *desc,
			     const D3D11_SUBRESOURCE_DATA *init, Sw11Res **out)
{
	Sw11Res *r;
	UINT w, h, vw, vh, tex_div = 1;
	if (!desc || !out)
		return E_INVALIDARG;
	*out = NULL;
	w = desc->Width ? desc->Width : 1;
	h = desc->Height ? desc->Height : 1;
	vw = 0;
	vh = 0;
	/* Render-target scaling. Restricted to surfaces the game renders INTO and
	 * never reads back by hand:
	 *
	 * - a render target or depth-stencil, because those are addressed through
	 *   a viewport (which scales) or a sampler (which is normalised), and a
	 *   depth buffer must scale with its colour target or the two stop lining
	 *   up at all;
	 * - USAGE_DEFAULT with no CPU access, which excludes staging and dynamic
	 *   textures - the ones the game Maps and walks in its own pixel
	 *   coordinates, where a smaller surface would be read as garbage;
	 * - taller than the cap AND at least as wide, which leaves small
	 *   fixed-size targets such as shadow maps and glow buffers completely
	 *   alone. Height alone is not enough: the first run of this caught
	 *   Rabbit and Steel's 570x850 portrait panels, which are UI elements and
	 *   not screen-derived at all - scaling one to 483x720 saved 0.13 Mpx and
	 *   risked softening text for it. Requiring both dimensions to clear the
	 *   cap keeps every genuinely display-sized surface and errs toward
	 *   leaving things alone, which is the safe direction for a feature whose
	 *   failure mode is a wrong-looking picture. */
	if (scale_rt_on() && render_max_h() &&
	    (desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) &&
	    desc->Usage == D3D11_USAGE_DEFAULT && !desc->CPUAccessFlags &&
	    h > render_max_h() && w >= render_max_h()) {
		UINT rw = w, rh = h;

		if (scale_to_cap(&rw, &rh)) {
			vw = w;
			vh = h;
			w = rw;
			h = rh;
		}
	}
	/* Sprite atlas downscaling. Separate from the render-target path above
	 * and mutually exclusive with it in practice, since that one requires a
	 * render-target bind and this one forbids it. Both report the requested
	 * size through virt_w/virt_h, so the game is never told anything
	 * different either way. */
	if (tex_scale_eligible(desc, init)) {
		UINT d = tex_scale();

		/* Any ratio, not just a power of two. The resampler averages over
		 * the exact source footprint of each destination pixel, so an odd
		 * dimension is no longer a reason to give up - which it used to
		 * be, silently, on most of the atlases worth shrinking. The two
		 * axes are allowed to end up on slightly different ratios when one
		 * of them hits the floor, because texture coordinates are
		 * normalised and nothing downstream can tell. */
		UINT nw = w / d, nh = h / d;

		if (nw < 256)
			nw = 256;
		if (nh < 256)
			nh = 256;
		if (nw < w && nh < h) {
			vw = w;
			vh = h;
			w = nw;
			h = nh;
			tex_div = d;
		}
	}
	r = (Sw11Res *)pin_calloc(sizeof(*r), "resource", PK_RES);
	if (!r)
		return E_OUTOFMEMORY;
	r->virt_w = vw;
	r->virt_h = vh;
	r->iface.tex.lpVtbl = &kTexVtbl;
	r->ref = 1;
	r->dev = dev;
	r->kind = 1;
	r->id = (int)InterlockedIncrement(&g_res_seq);
	r->format = desc->Format;
	r->width = w;
	r->height = h;
	r->bind = desc->BindFlags;
	r->usage = desc->Usage;
	r->cpu_access = desc->CPUAccessFlags;
	r->row_pitch = fmt_row_pitch(desc->Format, w);
	r->cpu_size = fmt_size(desc->Format, w, h);
	if (faucet_managed(r)) {
		/* Faucet: make window room by evicting cold textures, then allocate this
		 * one resident. It is banked below once its content is in place, so it can
		 * be evicted and painted back later. If it will not fit even after
		 * eviction, it is born evicted (a bank-less phantom for now - it gets a
		 * bank on its first upload and paints in after that). */
		faucet_ensure_room(r->cpu_size, NULL);
		res_account((LONG64)r->cpu_size, 1);
		if (r->cpu_size) {
			r->cpu = (unsigned char *)payload_alloc(r->cpu_size, r, PL_TEXTURE);
			if (!r->cpu) {
				res_account(-(LONG64)r->cpu_size, 0); /* stays live, 0 bytes */
				r->phantom = 1;
				phantom_init();
				InterlockedIncrement(&g_faucet_stuck);
			} else if (mark_empty() && (!init || !init->pSysMem) &&
				   !fmt_bc_block(desc->Format) &&
				   !fmt_is_depth(desc->Format) &&
				   fmt_stride(desc->Format) == 4) {
				fill_magenta(r->cpu, r->cpu_size);
			}
		}
		faucet_touch(r);
	} else if (!faucet_on() && phantom_capped(r)) {
		/* Over the resident cap (cap-only mode, no faucet): a phantom. Keep the
		 * full description, hold no pixels, count zero resident bytes. It renders
		 * magenta, never crashes, and is exactly the faucet's evicted state. */
		static volatile LONG said;

		r->phantom = 1;
		res_account(0, 1); /* live, but holds nothing */
		phantom_init();
		if (InterlockedIncrement(&said) <= 32 || (said & 0x3FF) == 0)
			d11_log("PHANTOM texture #%d %ux%u fmt=%d bind=%#x usage=%d - over "
				"the %u MB cap, no pixels, live %.1f MB",
				r->id, r->width, r->height, (int)r->format,
				(unsigned)r->bind, (int)r->usage, resident_cap_mb(),
				g_res_bytes / (1024.0 * 1024.0));
	} else {
		/* Counted as soon as it exists, so the failure path below can hand the
		 * whole resource to res_free_payload and have the books balance. */
		res_account((LONG64)r->cpu_size, 1);
		if (r->cpu_size) {
			r->cpu = (unsigned char *)payload_alloc(r->cpu_size, r,
								PL_TEXTURE);
			if (!r->cpu) {
				log_oom("a texture", (LONG64)r->cpu_size);
				res_account(-(LONG64)r->cpu_size, -1);
				obj_free(r);
				return E_OUTOFMEMORY;
			}
			if (mark_empty() && (!init || !init->pSysMem) &&
			    !fmt_bc_block(desc->Format) && !fmt_is_depth(desc->Format) &&
			    fmt_stride(desc->Format) == 4)
				fill_magenta(r->cpu, r->cpu_size);
		}
	}
	if (!r->phantom &&
	    ((desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
				 D3D11_BIND_DEPTH_STENCIL)) ||
	     fmt_is_depth(desc->Format))) {
		if (!res_ensure_pixels(r)) {
			log_oom("a render surface", (LONG64)w * h * 4);
			/* It may have got one plane before failing on the next,
			 * and this path is reached precisely when memory is
			 * scarce, so give back whatever it did manage. */
			res_free_payload(r, 1);
			obj_free(r);
			return E_OUTOFMEMORY;
		}
	}
	res_list_add(r);
	if (vw && tex_div == 1)
		d11_log("scaling render target #%d %ux%u -> %ux%u (bind=%#x), the game will "
			"go on seeing %ux%u through GetDesc",
			r->id, vw, vh, w, h, (unsigned)desc->BindFlags, vw, vh);
	if (tex_div > 1) {
		static LONG said;
		static LONG64 saved;

		saved += (LONG64)vw * vh * 4 - (LONG64)w * h * 4;
		if (InterlockedIncrement(&said) <= 3 || (said % 64) == 0)
			d11_log("texture #%d stored at %ux%u instead of %ux%u (1/%u each axis); "
				"%.1f MB saved so far by D3D11SW_TEX_SCALE",
				r->id, w, h, vw, vh, tex_div, saved / (1024.0 * 1024.0));
	}
	if (init && init->pSysMem && r->cpu) {
		UINT pitch = init->SysMemPitch ? init->SysMemPitch : r->row_pitch;
		UINT y;
		UINT hh = fmt_bc_block(desc->Format) ? (h + 3) / 4 : h;
		UINT rp = fmt_bc_block(desc->Format) ? r->row_pitch : r->row_pitch;

		if (tex_div > 1) {
			area_downscale_rgba(r->cpu, r->row_pitch, w, h,
					    (const unsigned char *)init->pSysMem, pitch, vw, vh);
		} else {
			for (y = 0; y < hh; y++) {
				UINT n = r->row_pitch;
				if (n > pitch)
					n = pitch;
				memcpy(r->cpu + (size_t)y * rp,
				       (const char *)init->pSysMem + (size_t)y * pitch, n);
			}
		}
		if (r->pixels)
			res_decode_pixels_written(r);
	}
	/* Bank the content now it is in place, so the faucet can evict and paint it
	 * back. A texture created empty (no init data) gets its bank on first upload. */
	if (init && init->pSysMem)
		faucet_bank_store(r);
	*out = r;
	return S_OK;
}

static HRESULT create_buf(Sw11Device *dev, const D3D11_BUFFER_DESC *desc,
			   const D3D11_SUBRESOURCE_DATA *init, Sw11Res **out)
{
	Sw11Res *r;
	if (!desc || !out)
		return E_INVALIDARG;
	*out = NULL;
	r = (Sw11Res *)pin_calloc(sizeof(*r), "resource", PK_RES);
	if (!r)
		return E_OUTOFMEMORY;
	r->iface.buf.lpVtbl = &kBufVtbl;
	r->ref = 1;
	r->dev = dev;
	r->kind = 0;
	r->bind = desc->BindFlags;
	r->usage = desc->Usage;
	r->cpu_access = desc->CPUAccessFlags;
	r->byte_width = desc->ByteWidth;
	r->cpu_size = desc->ByteWidth;
	r->cpu = (unsigned char *)payload_alloc(desc->ByteWidth, r, PL_BUFFER);
	if (!r->cpu) {
		log_oom("a buffer", (LONG64)desc->ByteWidth);
		obj_free(r);
		return E_OUTOFMEMORY;
	}
	res_account((LONG64)r->cpu_size, 1);
	res_list_add(r);
	if (init && init->pSysMem)
		memcpy(r->cpu, init->pSysMem, desc->ByteWidth);
	*out = r;
	return S_OK;
}

static int pin_rebuild_on(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[8];
		unsigned n = savestate_getenv("D3D11SW_PIN_REBUILD", buf, sizeof(buf));

		v = (n && buf[0] == '0') ? 0 : 1;
	}
	return v;
}

/* The pool object of this kind starting exactly at p, or NULL. */
static void *pin_obj_at(const void *p, unsigned char kind)
{
	const unsigned char *b = (const unsigned char *)p;
	unsigned i;

	if (b < g_pin_mem || b >= g_pin_mem + sizeof(g_pin_mem) ||
	    (size_t)(b - g_pin_mem) % PIN_UNIT)
		return NULL;
	i = (unsigned)((size_t)(b - g_pin_mem) / PIN_UNIT);
	if (!g_pin_len[i] || g_pin_len[i] == 0xFFFF || g_pin_kind[i] != kind)
		return NULL;
	return (void *)p;
}

/* Clear every live run touching units [lo, hi). Whatever this launch had there
 * is referenced only by memory the restore has just replaced, so it is dropped
 * without releasing anything it holds; a resource is only unlinked so the
 * census and the reap stop walking it. */
static unsigned pin_evict(unsigned lo, unsigned hi)
{
	unsigned i = lo, n, gone = 0;

	while (i > 0 && g_pin_len[i] == 0xFFFF)
		i--;
	while (i < hi) {
		n = g_pin_len[i];
		if (!n || n == 0xFFFF) {
			i++;
			continue;
		}
		if (g_pin_kind[i] == PK_RES) {
			Sw11Res *r = (Sw11Res *)(g_pin_mem + (size_t)i * PIN_UNIT);

			swrast_flush_if_pending(r->pixels);
			swrast_flush_if_pending(r->depth);
			res_list_remove(r);
		}
		obj_free(g_pin_mem + (size_t)i * PIN_UNIT);
		gone++;
		i += n;
	}
	return gone;
}

static void pin_place_next(unsigned i)
{
	g_pin_at_tid = GetCurrentThreadId();
	g_pin_at = i + 1;
}

static int res_desc_same(const Sw11Res *t, const Sw11Res *l)
{
	return t->kind == l->kind && t->format == l->format && t->width == l->width &&
	       t->height == l->height && t->virt_w == l->virt_w && t->virt_h == l->virt_h &&
	       t->bind == l->bind && t->usage == l->usage && t->cpu_access == l->cpu_access &&
	       t->byte_width == l->byte_width && t->is_bb == l->is_bb &&
	       (!l->retired || t->retired);
}

/* After a load from another launch, make the pool hold what the save's pool
 * held, address for address, because those are the addresses the restored game
 * memory names. Descriptions and reference counts come from the save; pixel and
 * buffer contents do not, so a rebuilt resource starts empty. */
static void pin_rebuild(const unsigned short *sl, const unsigned char *sk,
			const unsigned char *smem)
{
	unsigned i, res_same = 0, res_new = 0, res_fail = 0, res_bb = 0, view_same = 0,
		    view_repoint = 0, view_new_n = 0, view_fail = 0, st_same = 0, st_new = 0,
		    st_fail = 0, code_same = 0, code_diff = 0, code_gone = 0, evicted = 0,
		    elsewhere = 0, logged = 0;
	int pass;

	if (!pin_rebuild_on()) {
		d11_log("pin rebuild: off (D3D11SW_PIN_REBUILD=0)");
		return;
	}
	for (pass = 0; pass < 3; pass++) {
		for (i = 0; i < PIN_UNITS; i++) {
			unsigned n = sl[i], k = sk[i];
			const unsigned char *tb = smem + (size_t)i * PIN_UNIT;
			unsigned char *lb = g_pin_mem + (size_t)i * PIN_UNIT;
			int live_same = g_pin_len[i] == n && g_pin_kind[i] == k;

			if (!n || n == 0xFFFF || k == PK_FREE || k == PK_SINGLE)
				continue;
			if (pass == 0 && k == PK_RES) {
				const Sw11Res *t = (const Sw11Res *)tb;
				Sw11Res *r = NULL;
				HRESULT hr;

				if (live_same && res_desc_same(t, (const Sw11Res *)lb)) {
					((Sw11Res *)lb)->ref = t->retired ? 0 : t->ref;
					res_same++;
					continue;
				}
				if (t->is_bb) {
					res_bb++;
					continue;
				}
				if (!pin_obj_at(t->dev, PK_SINGLE)) {
					res_fail++;
					continue;
				}
				evicted += pin_evict(i, i + n);
				pin_place_next(i);
				if (t->kind == 1) {
					D3D11_TEXTURE2D_DESC d;

					memset(&d, 0, sizeof(d));
					d.Width = t->virt_w ? t->virt_w : t->width;
					d.Height = t->virt_h ? t->virt_h : t->height;
					d.MipLevels = 1;
					d.ArraySize = 1;
					d.Format = t->format;
					d.SampleDesc.Count = 1;
					d.Usage = (D3D11_USAGE)t->usage;
					d.BindFlags = t->bind;
					d.CPUAccessFlags = t->cpu_access;
					hr = create_tex2d(t->dev, &d, NULL, &r);
				} else {
					D3D11_BUFFER_DESC d;

					memset(&d, 0, sizeof(d));
					d.ByteWidth = t->byte_width;
					d.Usage = (D3D11_USAGE)t->usage;
					d.BindFlags = t->bind;
					d.CPUAccessFlags = t->cpu_access;
					hr = create_buf(t->dev, &d, NULL, &r);
				}
				g_pin_at = 0;
				if (FAILED(hr) || !r) {
					res_fail++;
					continue;
				}
				if ((unsigned char *)r != lb) {
					elsewhere++;
					continue;
				}
				r->ref = t->ref;
				if (t->retired) {
					r->retired = 1;
					r->ref = 0;
				}
				res_new++;
				if (logged++ < 24)
					d11_log("pin rebuild: %s #%d %ux%u fmt=%d bind=%#x at %p, ref %ld, "
						"empty",
						t->kind ? "texture" : "buffer", r->id,
						t->kind ? (t->virt_w ? t->virt_w : t->width)
							: t->byte_width,
						t->kind ? (t->virt_h ? t->virt_h : t->height) : 1,
						(int)t->format,
						(unsigned)t->bind, lb, (long)r->ref);
			} else if (pass == 1 && k == PK_VIEW) {
				const Sw11View *t = (const Sw11View *)tb;
				Sw11Res *res = (Sw11Res *)pin_obj_at(t->res, PK_RES);
				Sw11View *v;

				if (!res || !pin_obj_at(t->dev, PK_SINGLE)) {
					view_fail++;
					continue;
				}
				if (live_same) {
					v = (Sw11View *)lb;
					if (v->res != res)
						view_repoint++;
					else
						view_same++;
					v->res = res;
					v->kind = t->kind;
					v->format = t->format;
					v->iface = t->iface;
					v->ref = t->ref;
					continue;
				}
				evicted += pin_evict(i, i + n);
				res_ensure_pixels(res);
				pin_place_next(i);
				v = view_new(t->dev, res, t->kind, t->format);
				g_pin_at = 0;
				if (!v) {
					view_fail++;
					continue;
				}
				if ((unsigned char *)v != lb) {
					elsewhere++;
					continue;
				}
				InterlockedDecrement(&res->ref);
				v->ref = t->ref;
				view_new_n++;
			} else if (pass == 2 && (k == PK_BLEND || k == PK_DS || k == PK_RAST ||
						 k == PK_SAMP || k == PK_QUERY)) {
				void *o;

				if (live_same) {
					memcpy(lb, tb, (size_t)n * PIN_UNIT);
					st_same++;
					continue;
				}
				evicted += pin_evict(i, i + n);
				pin_place_next(i);
				o = pin_calloc((size_t)n * PIN_UNIT, g_pk_name[k], (unsigned char)k);
				g_pin_at = 0;
				if (o != lb) {
					if (o)
						elsewhere++;
					else
						st_fail++;
					continue;
				}
				memcpy(lb, tb, (size_t)n * PIN_UNIT);
				st_new++;
			} else if (pass == 2 && (k == PK_SHADER || k == PK_LAYOUT)) {
				if (!live_same)
					code_gone++;
				else if (k == PK_SHADER
						 ? (((const Sw11Shader *)tb)->is_ps !=
							    ((const Sw11Shader *)lb)->is_ps ||
						    ((const Sw11Shader *)tb)->bytes !=
							    ((const Sw11Shader *)lb)->bytes)
						 : ((const Sw11Layout *)tb)->n != ((const Sw11Layout *)lb)->n)
					code_diff++;
				else
					code_same++;
			}
		}
	}
	for (i = 0; i < PIN_UNITS; i++) {
		const Sw11Context *t = (const Sw11Context *)(smem + (size_t)i * PIN_UNIT);
		Sw11Context *c = (Sw11Context *)(g_pin_mem + (size_t)i * PIN_UNIT);
		unsigned dropped = 0, j;

		if (sk[i] != PK_SINGLE || !sl[i] || sl[i] == 0xFFFF ||
		    g_pin_kind[i] != PK_SINGLE || g_pin_len[i] != sl[i] ||
		    t->iface.lpVtbl != &kCtxVtbl || c->iface.lpVtbl != &kCtxVtbl)
			continue;
		lock_dev(c->dev);
		memcpy((unsigned char *)c + offsetof(Sw11Context, vb),
		       (const unsigned char *)t + offsetof(Sw11Context, vb),
		       sizeof(*c) - offsetof(Sw11Context, vb));
#define PIN_KEEP(f, kind)                                    \
	do {                                                 \
		if ((f) && !pin_obj_at((f), (kind))) {       \
			(f) = NULL;                          \
			dropped++;                           \
		}                                            \
	} while (0)
		for (j = 0; j < 16; j++) {
			PIN_KEEP(c->vb[j], PK_RES);
			PIN_KEEP(c->ps_srv[j], PK_VIEW);
			PIN_KEEP(c->ps_samp[j], PK_SAMP);
		}
		for (j = 0; j < 14; j++) {
			PIN_KEEP(c->cb_vs[j], PK_RES);
			PIN_KEEP(c->cb_ps[j], PK_RES);
		}
		for (j = 0; j < 4; j++)
			PIN_KEEP(c->rtv[j], PK_VIEW);
		PIN_KEEP(c->ib, PK_RES);
		PIN_KEEP(c->layout, PK_LAYOUT);
		PIN_KEEP(c->vs, PK_SHADER);
		PIN_KEEP(c->ps, PK_SHADER);
		PIN_KEEP(c->dsv, PK_VIEW);
		PIN_KEEP(c->blend, PK_BLEND);
		PIN_KEEP(c->dss, PK_DS);
		PIN_KEEP(c->rs, PK_RAST);
#undef PIN_KEEP
		unlock_dev(c->dev);
		d11_log("pin rebuild: immediate context at %p given the save's bindings, %u "
			"binding(s) cleared for pointing at nothing of the right kind",
			c, dropped);
	}
	d11_log("pin rebuild: resources %u kept (same description), %u rebuilt empty, %u "
		"failed, %u backbuffer(s) left alone",
		res_same, res_new, res_fail, res_bb);
	d11_log("pin rebuild: views %u kept, %u re-pointed at the save's resource, %u rebuilt, "
		"%u failed (resource or device missing)",
		view_same, view_repoint, view_new_n, view_fail);
	d11_log("pin rebuild: state objects %u given the save's description, %u rebuilt, %u "
		"failed",
		st_same, st_new, st_fail);
	d11_log("pin rebuild: shaders and layouts %u look the same, %u differ in size or stage, "
		"%u missing and not rebuildable (their code is not in the save)",
		code_same, code_diff, code_gone);
	d11_log("pin rebuild: %u of this launch's object(s) evicted to make room, %u rebuilt "
		"object(s) landed at the wrong address",
		evicted, elsewhere);
}

static Sw11Shader *shader_new(Sw11Device *dev, const void *code, SIZE_T bytes, int is_ps)
{
	Sw11Shader *s = (Sw11Shader *)pin_calloc(sizeof(*s), "shader", PK_SHADER);
	if (!s)
		return NULL;
	s->iface.lpVtbl = is_ps ? (ID3D11VertexShaderVtbl *)&kPSVtbl : &kVSVtbl;
	s->ref = 1;
	s->dev = dev;
	s->is_ps = is_ps;
	s->bytes = bytes;
	if (bytes && code) {
		s->code = (unsigned char *)malloc(bytes);
		if (!s->code) {
			obj_free(s);
			return NULL;
		}
		memcpy(s->code, code, bytes);
		s->parsed = dxbc_parse(s->code, (unsigned)bytes, &s->dxbc);
	}
	return s;
}

/* ---------- swapchain ---------- */

static HRESULT WINAPI Swap_QI(IDXGISwapChain1 *this, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IDXGIObject) ||
	    guid_eq(riid, &IID_IDXGIDeviceSubObject) || guid_eq(riid, &IID_IDXGISwapChain) ||
	    guid_eq(riid, &IID_IDXGISwapChain1)) {
		*ppv = this;
		this->lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}
static ULONG WINAPI Swap_AddRef(IDXGISwapChain1 *this)
{
	return (ULONG)InterlockedIncrement(&((Sw11Swap *)this)->ref);
}
static ULONG WINAPI Swap_Release(IDXGISwapChain1 *this)
{
	Sw11Swap *s = (Sw11Swap *)this;
	LONG n = InterlockedDecrement(&s->ref);
	if (n == 0) {
		/* Never leave the desktop in the mode we switched it to. */
		if (s->fullscreen)
			ChangeDisplaySettingsExA(NULL, NULL, NULL, 0, NULL);
		if (s->bb)
			Res_Release(s->bb);
		priv_free(s->priv);
		obj_free(s);
	}
	return (ULONG)n;
}
static HRESULT WINAPI Swap_SetPD(IDXGISwapChain1 *this, REFGUID g, UINT n, const void *d)
{
	return priv_set(&((Sw11Swap *)this)->priv, g, n, d);
}
static HRESULT WINAPI Swap_SetPDI(IDXGISwapChain1 *this, REFGUID g, const IUnknown *o)
{
	return priv_set(&((Sw11Swap *)this)->priv, g, sizeof(o), &o);
}
static HRESULT WINAPI Swap_GetPD(IDXGISwapChain1 *this, REFGUID g, UINT *n, void *d)
{
	return priv_get(((Sw11Swap *)this)->priv, g, n, d);
}
static HRESULT WINAPI Swap_GetParent(IDXGISwapChain1 *this, REFIID riid, void **pp)
{
	Sw11Swap *s = (Sw11Swap *)this;
	if (!pp)
		return E_POINTER;
	return s->dev->factory->iface.lpVtbl->QueryInterface(&s->dev->factory->iface, riid, pp);
}
static HRESULT WINAPI Swap_GetDevice(IDXGISwapChain1 *this, REFIID riid, void **pp)
{
	Sw11Swap *s = (Sw11Swap *)this;
	return s->dev->iface.lpVtbl->QueryInterface(&s->dev->iface, riid, pp);
}

/* The rasteriser already counts its own work; nothing on this path was reading
 * it. Reported every couple of seconds so the cost is irrelevant, and always on
 * because guessing at where a software frame goes is how time gets wasted.
 *
 * The mix percentages matter more than the totals: the AVX2 span kernel only
 * takes textured, flat-coloured, depth-free spans under source-over or additive
 * blending, so "flat" and "ztest" say how much of the frame can use it at all. */
static void perf_tick(void)
{
	static LARGE_INTEGER freq, last;
	static unsigned frames;
	LARGE_INTEGER now;

	if (!freq.QuadPart) {
		/* D3D11SW_PAINTSTAT=1 measures every frame, N > 1 two frames in
		 * every N. It costs raster time while measuring, so read the other
		 * perf lines from a run without it. */
		char buf[16];

		if (savestate_getenv("D3D11SW_PAINTSTAT", buf, sizeof(buf)))
			swrast_paintstat = atoi(buf);
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&last);
		d11_log("swrast: %d worker threads, cpu features %#x, simd %d, paintstat %d",
			swrast_thread_count(), swrast_cpu_features(), swrast_simd_enable,
			swrast_paintstat);
	}
	swrast_paintstat_frame();
	frames++;
	if (InterlockedIncrement(&g_present_n) % 120 == 1)
		exp_transp_refresh();
	faucet_frame();
	QueryPerformanceCounter(&now);
	if (now.QuadPart - last.QuadPart < freq.QuadPart * 2)
		return;
	{
		double secs = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
		double raster_ms = 0, area = 0, bbox = 0, mix[9];
		unsigned fl = 0, tris = 0, bins = 0;
		int tw = 0, th = 0, i;
		double f = frames ? (double)frames : 1.0;
		swrast_prof_take(&raster_ms, &fl, &tris, &bins);
		swrast_prof_take2(&area, &bbox, &tw, &th);
		for (i = 0; i < 9; i++)
			mix[i] = 0;
		swrast_prof_mix(mix, 9);
		d11_log("perf %.1f fps (%.1f ms/frame) raster %.1f ms/frame tris %.0f flushes %.0f "
			"bins %.0f shaded %.2f Mpx/frame bbox %.2f Mpx/frame",
			frames / secs, 1000.0 * secs / f, raster_ms / f, tris / f, fl / f,
			bins / f, area / f / 1e6, bbox / f / 1e6);
		{
			/* Address space is the scarce resource for a 32-bit title
			 * against a backend that keeps every surface in RAM, and
			 * exhaustion shows up as a slow squeeze rather than a
			 * clean error. Print the trend so a crash minutes later
			 * can be read against it. */
			double used, freed, largest;

			va_stats(&used, &freed, &largest);
			/* orphaned subtracts the retained list, and that correction
			 * matters more than it looks.
			 *
			 * died only increments inside res_free_payload, and the retain
			 * path in Res_Release deliberately skips it - a retained object
			 * is born and not yet died. Reporting born-died alone would
			 * therefore have counted every deliberately held texture as an
			 * orphan, which is precisely the confusion this line exists to
			 * remove. The retain list empties on the next superseding save
			 * and died catches up then.
			 *
			 * Note live includes retained too, so live falling back to ~225
			 * after a save is the flush, not a leak resolving itself. */
			d11_log("perf mem: %ld live resources, %.1f MB of payload | born "
			"%ld died %ld unretained-live %ld | retained %ld now, %ld pushed, "
			"%ld post-save skipped, %ld flush(es) | VA %.0f MB used, "
			"%.0f MB free, largest "
				"block %.1f MB",
				(long)g_res_live, g_res_bytes / (1024.0 * 1024.0),
				(long)g_res_born, (long)g_res_died,
				(long)(g_res_born - g_res_died - g_ret_kept_n),
				(long)g_ret_kept_n,
				(long)g_ret_pushed, (long)g_ret_skipped_new,
				(long)g_ret_flushes, used, freed,
				largest);
			if (g_arena.ready)
				d11_log("perf arena: %.0f of %.0f MB live at %08X | %u extent(s), "
					"peak %u | %u allocation(s) spilled outside",
					(double)g_arena.live_bytes / (1024.0 * 1024.0),
					(double)g_arena.size / (1024.0 * 1024.0),
					(unsigned)(uintptr_t)g_arena.base, g_arena.n,
					g_arena.peak_ext, g_arena.fallbacks);
			if (faucet_on()) {
				/* Per window, because totals cannot say whether the copying
				 * was a load burst or is still going on. */
				static LONG p_in, p_ev, p_th, p_ov, p_st;
				static LONG64 p_bytes;
				const double mb = 1024.0 * 1024.0;
				LONG d_in = g_faucet_in - p_in, d_ev = g_faucet_evict - p_ev;
				LONG d_th = g_faucet_thrash - p_th, d_ov = g_faucet_over - p_ov;
				LONG d_st = g_faucet_stuck - p_st;
				double d_mb = (double)(g_faucet_in_bytes - p_bytes) / mb;

				d11_log("perf faucet: %ld paint-in(s) %.1f MB (%.2f MB/frame), "
					"%ld evict(s), %ld thrash, %ld overcommit(s) (peak %.1f MB "
					"over), %ld stuck | hot set %.1f MB peak this window, "
					"%.1f MB peak ever (last %u frame(s)) | resident %.1f MB "
					"of a %u MB window",
					(long)d_in, d_mb, d_mb / f, (long)d_ev, (long)d_th,
					(long)d_ov, (double)g_faucet_over_peak / mb, (long)d_st,
					(double)g_faucet_hot_peak / mb,
					(double)g_faucet_hot_peak_all / mb, faucet_hot_frames(),
					g_res_bytes / mb, resident_cap_mb());
				p_in = g_faucet_in;
				p_ev = g_faucet_evict;
				p_th = g_faucet_thrash;
				p_ov = g_faucet_over;
				p_st = g_faucet_stuck;
				p_bytes = g_faucet_in_bytes;
				g_faucet_hot_peak = 0;
				g_faucet_over_peak = 0;
			}
		}
		{
			/* Reported next to memory because that is what makes a bin
			 * fail to grow. Silence here means the rectangles are
			 * something we DRAW, not something we fail to draw. */
			static unsigned said_tris, said_tiles;
			unsigned dt = 0, dl = 0;

			swrast_prof_drops(&dt, &dl);
			if (dt != said_tris || dl != said_tiles) {
				said_tris = dt;
				said_tiles = dl;
				d11_log("WARNING geometry dropped for want of memory: %u triangle(s) "
					"never binned, %u tile insertion(s) lost. Each lost "
					"insertion is a 64px hole showing whatever the buffer "
					"held before",
					dt, dl);
			}
		}
		if (area > 0.0)
			d11_log("perf mix%% tex=%.0f bilin=%.0f over=%.0f add=%.0f otherblend=%.0f "
				"atest=%.0f ztest=%.0f flat=%.0f linear=%.0f",
				100 * mix[0] / area, 100 * mix[1] / area, 100 * mix[2] / area,
				100 * mix[3] / area, 100 * mix[4] / area, 100 * mix[5] / area,
				100 * mix[6] / area, 100 * mix[7] / area, 100 * mix[8] / area);
		if (swrast_paintstat) {
			/* What share of the shaded area changes nothing, split by the
			 * trick that would skip it. Per measured frame, since the
			 * sampling mode leaves most frames unmeasured. */
			double p[SWPS_N], mf, sh;

			swrast_prof_paint(p);
			mf = p[SWPS_FRAMES] > 0.0 ? p[SWPS_FRAMES] : 1.0;
			sh = p[SWPS_SHADED] > 0.0 ? p[SWPS_SHADED] : 1.0;
			if (p[SWPS_FRAMES] > 0.0) {
				d11_log("perf paint (%.0f of %u frame(s) measured): shaded %.2f "
					"Mpx/frame | killed %.1f%% zero-alpha %.1f%% no-op %.1f%% "
					"opaque %.1f%% blended %.1f%% | hidden %.1f%% (%.2f "
					"Mpx/frame buried under a later opaque write) | uniform "
					"8-runs %.1f%% of %.2f Mpx/frame full vector groups "
					"(clear %.1f%% opaque %.1f%% translucent %.1f%%) (vector "
					"%.0f%% of shaded)",
					p[SWPS_FRAMES], frames, p[SWPS_SHADED] / mf / 1e6,
					100 * p[SWPS_KILLED] / sh, 100 * p[SWPS_ZERO] / sh,
					100 * p[SWPS_NOOP] / sh, 100 * p[SWPS_OPAQUE] / sh,
					100 * p[SWPS_RMW] / sh, 100 * p[SWPS_HIDDEN] / sh,
					p[SWPS_HIDDEN] / mf / 1e6,
					p[SWPS_FULL8] > 0.0 ? 100 * p[SWPS_UNIFORM] / p[SWPS_FULL8]
							    : 0.0,
					p[SWPS_FULL8] / mf / 1e6,
					p[SWPS_FULL8] > 0.0 ? 100 * p[SWPS_UNIF_CLEAR] / p[SWPS_FULL8]
							    : 0.0,
					p[SWPS_FULL8] > 0.0 ? 100 * p[SWPS_UNIF_OPAQUE] / p[SWPS_FULL8]
							    : 0.0,
					p[SWPS_FULL8] > 0.0
						? 100 *
							  (p[SWPS_UNIFORM] - p[SWPS_UNIF_CLEAR] -
							   p[SWPS_UNIF_OPAQUE]) /
							  p[SWPS_FULL8]
						: 0.0,
					100 * p[SWPS_VEC] / sh);
				d11_log("perf paint reuse: %.0f%% of touched tiles (%.0f%% of their "
					"%.2f Mpx/frame) drew exactly what they drew last frame | "
					"%.0f of %.0f target-frame(s) fully unchanged | quads "
					"%.2f Mpx/frame: 1:1 %.2f, integer upscale %.2f, "
					"whole-target %.2f (%.1f/frame)",
					p[SWPS_TILES] > 0.0 ? 100 * p[SWPS_TILES_SAME] / p[SWPS_TILES]
							    : 0.0,
					p[SWPS_TILE_PX] > 0.0
						? 100 * p[SWPS_TILE_PX_SAME] / p[SWPS_TILE_PX]
						: 0.0,
					p[SWPS_TILE_PX] / mf / 1e6, p[SWPS_TGT_STATIC],
					p[SWPS_TGT_FRAMES], p[SWPS_RECT] / mf / 1e6,
					p[SWPS_RECT_1TO1] / mf / 1e6, p[SWPS_RECT_INT] / mf / 1e6,
					p[SWPS_RECT_FULL] / mf / 1e6, p[SWPS_RECT_FULL_N] / mf);
			}
			/* Weighted by screen area drawn from each texture, not by
			 * texture count, and over the whole window. */
			if (g_pal[PAL_PX] > 0.0) {
				double tp = g_pal[PAL_PX];
				double cp = tp - g_pal[PAL_PX_RT] - g_pal[PAL_PX_NONE];
				double c = cp > 0.0 ? 100.0 / cp : 0.0;
				double sb = g_pal[PAL_SOLID] + g_pal[PAL_CLEAR];

				d11_log("perf paint colour: of %.2f Mpx/frame textured, %.0f%% from "
					"render targets and %.0f%% not yet taken | the rest by "
					"source palette: <=2 %.0f%%, <=16 %.0f%%, <=256 %.0f%%, "
					">256 %.0f%% | 4x4 blocks clear %.0f%% solid %.0f%% (white "
					"%.1f%% black %.1f%%) mixed %.0f%% | %.0f%% of solid "
					"blocks start a run | mean texel run %.1f",
					tp / f / 1e6, 100 * g_pal[PAL_PX_RT] / tp,
					100 * g_pal[PAL_PX_NONE] / tp, c * g_pal[PAL_C2],
					c * g_pal[PAL_C16], c * g_pal[PAL_C256], c * g_pal[PAL_CMANY],
					c * g_pal[PAL_CLEAR], c * g_pal[PAL_SOLID], c * g_pal[PAL_WHITE],
					c * g_pal[PAL_BLACK], 100.0 - c * sb,
					sb > 0.0 ? 100 * g_pal[PAL_RUNS] / sb : 0.0,
					g_pal[PAL_TEXRUN] > 0.0 ? cp / g_pal[PAL_TEXRUN] : 0.0);
			}
			memset(g_pal, 0, sizeof(g_pal));
		}
		{
			double sr[11], tot = 0;
			for (i = 0; i < 11; i++)
				sr[i] = 0;
			swrast_prof_simd(sr, 11);
			for (i = 0; i < 10; i++)
				tot += sr[i];
			if (tot > 0.0)
				d11_log("perf simd%% ok=%.0f (wrap/mirror fold %.0f) | noavx2=%.0f "
					"notex=%.0f notflat=%.0f depth=%.0f mask=%.0f blend=%.0f "
					"npot=%.0f addr=%.0f other=%.0f",
					100 * sr[0] / tot, 100 * sr[10] / tot, 100 * sr[1] / tot,
					100 * sr[2] / tot, 100 * sr[3] / tot, 100 * sr[4] / tot,
					100 * sr[5] / tot, 100 * sr[6] / tot, 100 * sr[7] / tot,
					100 * sr[8] / tot, 100 * sr[9] / tot);
			/* The percentage above is the bill; this is the itemisation.
			 * Scalar pixels cost about six times what vector ones do, so
			 * whatever tops this list is the frame time. */
			if (gpu_mode() >= 2) {
				unsigned gu, gd, gv;

				gpu_prof_take(&gu, &gd, &gv);
				g_gpu_uploads_total += gu;
				/* Declines are the number that matters. The adapter
				 * cannot take the frame over until it is zero, because
				 * a declined draw is a sprite the hardware path would
				 * simply be missing. */
				d11_log("perf gpu: %u draw(s)/frame, %u vert(s)/frame, "
					"%u upload(s)/frame | %u declined/frame%s",
					(unsigned)(gd / (unsigned)(f < 1 ? 1 : f)),
					(unsigned)(gv / (unsigned)(f < 1 ? 1 : f)),
					(unsigned)(gu / (unsigned)(f < 1 ? 1 : f)),
					(unsigned)(g_gpu_declined / (unsigned)(f < 1 ? 1 : f)),
					g_gpu_declined ? " <<< states the backend does "
							 "not yet honour exactly"
						       : " - the frame is fully covered");
				/* Cumulative on purpose. Uploads and releases are
				 * one-off events per texture, and dividing them by
				 * the frame count buries them at zero - which is
				 * exactly what happened the first time this ran. */
				d11_log("perf gpu mem: %u texture(s) released to the "
					"adapter, %.1f MB no longer in this process | "
					"%u reclaimed by a write | %u upload(s) all told",
					g_gpu_freed_n,
					(double)g_gpu_freed_bytes / (1024.0 * 1024.0),
					g_gpu_rebuilt, g_gpu_uploads_total);
				g_gpu_declined = 0;
				g_gpu_mirrored = 0;
			}
			if (sr[6] > 0.0) {
				int bop[4], bsrc[4], bdst[4], nb, k;
				double barea[4];

				nb = swrast_prof_blend_other(bop, bsrc, bdst, barea, 4);
				for (k = 0; k < nb; k++)
					d11_log("perf blend-scalar #%d op=%s src=%s dst=%s "
						"%.2f Mpx/frame",
						k, blend_op_name(bop[k]),
						blend_factor_name(bsrc[k]),
						blend_factor_name(bdst[k]),
						barea[k] / f / 1e6);
			}
		}
		{
			double ms = 1000.0 / (double)freq.QuadPart;
			double dr = g_zone[ZONE_DRAW] * ms, de = g_zone[ZONE_DECODE] * ms,
			       pr = g_zone[ZONE_PRESENT] * ms, pa = g_zone[ZONE_PACE] * ms,
			       hk = g_zone[ZONE_HOOKS] * ms;
			double guard_ms = 0.0, audio_ms = 0.0;

			savestate_perf_take(&guard_ms, &audio_ms);
			double total = 1000.0 * secs;
			/* Flushes fire from wherever a surface is next read, which
			 * for this game is mostly a render-target switch, so most
			 * of the rasteriser's time falls outside draw and present.
			 * Naming it separately keeps the residual meaning the
			 * game's own code and nothing else. */
			double rout = raster_ms - g_raster_nested_ms;
			if (rout < 0.0)
				rout = 0.0;
			d11_log("perf split ms/frame: draw %.1f (raster %.1f nested) decode %.1f "
				"present %.1f pace %.1f hooks %.1f | flush elsewhere %.1f | "
				"game %.1f of %.1f",
				dr / f, g_raster_nested_ms / f, de / f, pr / f, pa / f, hk / f,
				rout / f, (total - dr - de - pr - pa - hk - rout) / f, total / f);
			/* Broken out because "is the audio eating the frame" is a question
			 * the totals cannot answer: the drain runs inside the guard, and
			 * the guard also walks the address space. */
			if (hk > 0.0)
				d11_log("perf hooks: %.1f ms/frame - guard %.1f (of which audio "
					"drain %.1f), watches %.1f",
					hk / f, guard_ms / f, audio_ms / f,
					(hk / f) - (guard_ms / f));
			/* Only worth a line when the pacer actually ran. What it
			 * says: how much of the pace bucket is the compositor
			 * holding us versus our own deadline arithmetic, how many
			 * flushes the display rate is asking for, and how often we
			 * arrive already past the deadline - which is the signature
			 * of the pacer being a victim rather than the cause. */
			if (pa > 0.0)
				d11_log("perf pace: %.1f ms/frame total - %.1f in DwmFlush "
					"(%.1f call(s)/frame), %.1f in sleep/spin | display "
					"%.0f Hz, target %.0f fps, %u arrived late, %u reset",
					pa / f, g_pace_dwm * ms / f,
					f > 0.0 ? (double)g_pace_calls / f : 0.0,
					(pa - g_pace_dwm * ms) / f, g_pace_hz, g_pace_target,
					g_pace_late, g_pace_resets);
			if (g_gh.h && !g_gh.dead) {
				static int32_t pub0, shown0;
				int32_t pub = g_gh.h->published, shown = g_gh.h->shown;

				d11_log("perf gpuhost: %s, %ld frame(s) handed over, %ld shown%s",
					g_gh.h->state == GH_READY ? "presenting" : "starting",
					(long)(pub - pub0), (long)(shown - shown0),
					g_gh.h->shader_user ? ", gpuhost.hlsl" : ", built-in shader");
				pub0 = pub;
				shown0 = shown;
				if (gh_remote()) {
					static unsigned long long by0, up0;
					static unsigned fr0, wt0;
					unsigned fr = g_rg.frames - fr0;

					d11_log("perf gpuhost draw: %u GPU frame(s), %.0f KB/frame "
						"through the ring, %.1f MB of texture uploads, %u "
						"stall(s) on a full ring, %ld draw(s) and %ld "
						"texture(s) on the host so far, %u readback(s)",
						fr,
						fr ? (double)(g_rg.bytes - by0 - (g_rg.up_bytes - up0)) /
							     1024.0 / fr
						   : 0.0,
						(double)(g_rg.up_bytes - up0) / (1024.0 * 1024.0),
						g_rg.waits - wt0, (long)g_gh.h->gpu_draws,
						(long)g_gh.h->gpu_textures, g_rg.readbacks);
					by0 = g_rg.bytes;
					up0 = g_rg.up_bytes;
					fr0 = g_rg.frames;
					wt0 = g_rg.waits;
				}
			}
			memset(g_zone, 0, sizeof(g_zone));
			g_pace_dwm = 0.0;
			g_pace_calls = g_pace_resets = g_pace_late = 0;
			g_raster_nested_ms = 0.0;
		}
		if (g_n_draws) {
			/* Geometry as a share of the draw path, plus what the
			 * geometry loop is being asked to chew: submitted against
			 * rasterised triangles says how much of it is thrown away,
			 * and vertices per draw says whether shared indices are
			 * being re-shaded. */
			d11_log("perf geom: %u draws/frame, %u verts/frame (%.1f/draw), "
				"tris %u in -> %u out (%.0f%% kept), batch %.0f%% of draw, "
				"%u wrap->clamp/frame",
				(unsigned)(g_n_draws / (unsigned)(f < 1 ? 1 : f)),
				(unsigned)(g_n_verts / (unsigned)(f < 1 ? 1 : f)),
				(double)g_n_verts / (double)g_n_draws,
				(unsigned)(g_n_tris_in / (unsigned)(f < 1 ? 1 : f)),
				(unsigned)(g_n_tris_out / (unsigned)(f < 1 ? 1 : f)),
				g_n_tris_in ? 100.0 * (double)g_n_tris_out / (double)g_n_tris_in
					    : 0.0,
				g_tsc_draw ? 100.0 * (double)g_tsc_batch / (double)g_tsc_draw
					   : 0.0,
				(unsigned)(g_n_npot_clamped / (unsigned)(f < 1 ? 1 : f)));
		}
		g_tsc_draw = g_tsc_batch = 0;
		g_n_draws = g_n_verts = g_n_tris_in = g_n_tris_out = 0;
		g_n_npot_clamped = 0;
		for (i = 0; i < g_perf_nrt; i++) {
			int best = i, j;
			for (j = i + 1; j < g_perf_nrt; j++)
				if (g_perf_rt[j].px > g_perf_rt[best].px)
					best = j;
			if (best != i) {
				PerfRt tmp = g_perf_rt[i];
				g_perf_rt[i] = g_perf_rt[best];
				g_perf_rt[best] = tmp;
			}
		}
		for (i = 0; i < g_perf_nrt && i < 6; i++) {
			double screens = (double)g_perf_rt[i].w * (double)g_perf_rt[i].h;
			d11_log("perf rt #%d %ux%u bb=%d %.2f Mpx/frame (%.1fx target) %.0f "
				"draws/frame",
				g_perf_rt[i].id, g_perf_rt[i].w, g_perf_rt[i].h,
				g_perf_rt[i].is_bb, g_perf_rt[i].px / f / 1e6,
				screens > 0 ? g_perf_rt[i].px / f / screens : 0.0,
				g_perf_rt[i].draws / f);
		}
		for (i = 0; i < g_perf_ntex; i++) {
			int best = i, j;
			for (j = i + 1; j < g_perf_ntex; j++)
				if (g_perf_tex[j].px > g_perf_tex[best].px)
					best = j;
			if (best != i) {
				PerfTex tmp = g_perf_tex[i];
				g_perf_tex[i] = g_perf_tex[best];
				g_perf_tex[best] = tmp;
			}
		}
		for (i = 0; i < g_perf_ntex && i < 6; i++) {
			const TexPal *t = &g_perf_tex[i].pal;
			char pal[160] = "";

			if (t->gen && t->blocks) {
				double b = 100.0 / (double)t->blocks;
				unsigned sb = t->solid + t->clear;

				snprintf(pal, sizeof(pal),
					 " | %s%u colour(s), 4x4 clear %.0f%% solid %.0f%% (white "
					 "%.0f%% black %.0f%%), %.0f%% of solid blocks start a run, "
					 "texel run %.1f",
					 t->colors > 256 ? ">" : "", t->colors > 256 ? 256u : t->colors,
					 b * t->clear, b * t->solid, b * t->white, b * t->black,
					 sb ? 100.0 * t->runs / (double)sb : 0.0,
					 t->texel_runs ? (double)t->texels / (double)t->texel_runs
						       : 0.0);
			}
			d11_log("perf tex #%d %dx%d blend=%04x %.2f Mpx/frame %.0f draws/frame%s",
				g_perf_tex[i].id, g_perf_tex[i].w, g_perf_tex[i].h,
				(unsigned)g_perf_tex[i].blend, g_perf_tex[i].px / f / 1e6,
				g_perf_tex[i].draws / f, pal);
		}
		g_perf_ntex = 0;
		memset(g_perf_tex, 0, sizeof(g_perf_tex));
		g_perf_nrt = 0;
		memset(g_perf_rt, 0, sizeof(g_perf_rt));
		frames = 0;
		last = now;
	}
}

/* Wait out the sync interval the game asked for.
 *
 * A real swap chain blocks here until the display has actually shown a frame.
 * Ignoring that let menus run at 1200 fps, which buys nothing - the monitor
 * shows 60 of those - while burning a core and, in a game whose logic advances
 * per frame rather than per unit of time, running the world far faster than
 * intended.
 *
 * DwmFlush is the honest wait: it returns when the compositor has finished the
 * frame it is on, which for a windowed swap chain is the moment that matters.
 * It is resolved at runtime rather than imported, because failing to find it
 * should cost the frame pacing and not the process.
 *
 * The fallback paces against the refresh rate instead. It is deliberately a
 * sleep to just short of the deadline followed by a spin, since Sleep rounds up
 * to the scheduler tick and would otherwise overshoot 16.7 ms often enough to
 * halve the frame rate. */
/* Refresh rate of the monitor the window is actually on. Re-read periodically
 * rather than cached forever, because dragging a window between a 60 Hz and a
 * 240 Hz panel changes the answer and nothing tells us when that happens. */
static double display_hz(HWND hwnd)
{
	MONITORINFOEXA mi;
	DEVMODEA dm;
	HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : NULL;

	memset(&mi, 0, sizeof(mi));
	mi.cbSize = sizeof(mi);
	memset(&dm, 0, sizeof(dm));
	dm.dmSize = sizeof(dm);
	if (mon && GetMonitorInfoA(mon, (MONITORINFO *)&mi) &&
	    EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
	    dm.dmDisplayFrequency > 1)
		return (double)dm.dmDisplayFrequency;
	if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
		return (double)dm.dmDisplayFrequency;
	return 60.0;
}

/* Frames per second to pace to, 0 to follow the display.
 *
 * Defaulted to 60 because these are fixed-timestep games: they advance the
 * world one step per frame and rely entirely on the swap chain to keep that
 * step at 1/60 s. Ours ran at 1200 fps with no wait at all, and following the
 * display exactly is only right when the display is 60 Hz - on the 240 Hz panel
 * this was tested on, one wait per present is 240 fps and the game runs four
 * times too fast. That is not an artefact of software rendering; the same game
 * on the same monitor does it on real hardware, which is why players cap it. */
static double fps_cap(void)
{
	static double v = -1.0;

	if (v < 0.0) {
		char buf[16];
		unsigned n = savestate_getenv("D3D11SW_FPS_CAP", buf, sizeof(buf));
		unsigned i;
		int acc = 0;

		for (i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
			acc = acc * 10 + (buf[i] - '0');
		v = n ? (double)acc : 60.0;
	}
	return v;
}

/* How many DwmFlush calls to spend per frame. 1 aligns the wait to a
 * composition boundary and lets the deadline do the pacing; 0 drops the
 * compositor out of it entirely; higher values restore the old
 * one-per-compositor-frame behaviour for comparison. */
static int pace_flushes(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[16];
		unsigned n = savestate_getenv("D3D11SW_PACE_FLUSH", buf, sizeof(buf));
		unsigned i;
		int acc = 0;

		for (i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
			acc = acc * 10 + (buf[i] - '0');
		v = n ? acc : 1;
	}
	return v;
}

/* How many frames of lateness to absorb before giving up on catching up.
 *
 * The deadline accumulates from a fixed origin, which is what lets it hold a
 * steady rate through frames of uneven work - a frame that runs long is paid
 * for by the next one waiting less. That is the right behaviour for jitter and
 * the wrong behaviour for a stall: after the process is parked for a savestate
 * the deadline is hundreds of milliseconds in the past, and every frame until
 * real time catches up presents with no wait at all. The game steps its logic
 * once per presented frame, so it does not skip that time, it replays it at
 * speed.
 *
 * Past this many frames the time is treated as gone rather than owed. Those
 * frames are lost either way; the only choice is whether they are lost quietly
 * or as a burst of fast motion.
 *
 * 0 gives up instantly and paces every frame from where it actually started.
 * A large value restores the old behaviour, which tolerated a full second - so
 * a 700 ms save never tripped it and a 1000 ms restore only just did, which is
 * why the speed-up came and went. */
static int pace_catchup(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[16];
		unsigned n = savestate_getenv("D3D11SW_PACE_CATCHUP", buf, sizeof(buf));
		unsigned i;
		int acc = 0;

		for (i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
			acc = acc * 10 + (buf[i] - '0');
		v = n ? acc : 2;
	}
	return v;
}

/* Whether a frame that arrives already behind presents at once.
 *
 * Giving up on catching up restarts the schedule from now, and the restart
 * used to put the next deadline a full frame ahead and then sleep to it. A
 * scene whose work alone takes longer than two frames trips the restart every
 * frame, so every frame paid a whole extra interval on top of its work plus a
 * compositor flush. Measured at 40 ms of work into a 60 fps target: 34 resets
 * in 34 frames, 18.4 ms of each 60 ms frame asleep.
 *
 * 1 (default) skips the flush when behind and presents a restarted frame
 * immediately, starting the new schedule from it. 0 restores the old wait. */
static int pace_late_skip(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[8];
		unsigned n = savestate_getenv("D3D11SW_PACE_LATE_SKIP", buf, sizeof(buf));

		v = (n && buf[0] == '0') ? 0 : 1;
	}
	return v;
}

static void present_wait(HWND hwnd, UINT sync)
{
	static HRESULT(WINAPI * dwm_flush)(void);
	static LARGE_INTEGER freq;
	static LARGE_INTEGER next;
	static double hz = 60.0;
	static LONG probe;
	static int ready;
	double cap = fps_cap();
	double period;
	LARGE_INTEGER now;
	UINT i;
	int waited;

	if (!ready) {
		HMODULE m = LoadLibraryA("dwmapi.dll");

		ready = 1;
		QueryPerformanceFrequency(&freq);
		if (m)
			dwm_flush = (HRESULT(WINAPI *)(void))GetProcAddress(m, "DwmFlush");
	}
	/* Cheap enough at this interval, and four seconds of wrong pacing after
	 * moving a window between monitors is not worth doing it every frame. */
	if ((probe++ % 240) == 0) {
		double fresh = display_hz(hwnd);
		if (fresh != hz) {
			hz = fresh;
			d11_log("present pacing: %s, display %.0f Hz, target %.0f fps "
				"(D3D11SW_FPS_CAP)",
				dwm_flush ? "DwmFlush" : "timer (DwmFlush unavailable)", hz,
				cap > 0.0 ? cap : hz);
		}
	}
	period = 1.0 / ((cap > 0.0 && cap < hz) ? cap : hz);
	g_pace_hz = hz;
	g_pace_target = 1.0 / period;

	/* Waiting on the compositor keeps the presents lined up with the display
	 * even when the target is a fraction of it: at 240 Hz and a 60 fps target
	 * that is every fourth compositor frame. */
	/* One flush, not one per compositor frame in the interval.
	 *
	 * DwmFlush blocks until the next composition, so calling it n times blocks
	 * for n compositions - and it does so from wherever the frame happened to
	 * finish, not from a fixed origin. At 480 Hz into a 60 fps target n was 8,
	 * each flush cost one 2.08 ms compositor frame, and the loop added a flat
	 * 16.2 ms to every frame no matter how much work preceded it. Measured:
	 * 8.4 ms of work became a 24.6 ms frame, and pace never varied by more
	 * than a millisecond between the lightest and heaviest frames in the log.
	 * It was a tax, not a limit.
	 *
	 * Presenting on every nth compositor frame is what the absolute deadline
	 * below already does, and correctly, because it accumulates from a fixed
	 * origin and so absorbs the work instead of following it. The single flush
	 * that remains is only to start the wait on a composition boundary. */
	QueryPerformanceCounter(&now);
	if (dwm_flush && pace_flushes() &&
	    !(pace_late_skip() && next.QuadPart && now.QuadPart > next.QuadPart)) {
		LARGE_INTEGER d0, d1;

		QueryPerformanceCounter(&d0);
		for (i = 0; i < (UINT)pace_flushes(); i++)
			dwm_flush();
		QueryPerformanceCounter(&d1);
		g_pace_dwm += (double)(d1.QuadPart - d0.QuadPart);
		g_pace_calls += (UINT)pace_flushes();
	}
	/* Backstop, and the whole mechanism when DwmFlush is missing. It also
	 * covers the refresh rate being misreported, since the deadline is real
	 * time and does not care what the display claimed. */
	QueryPerformanceCounter(&now);
	{
		LONGLONG frame = (LONGLONG)(period * (double)(sync ? sync : 1) *
					    (double)freq.QuadPart);
		LONGLONG slack = frame * (LONGLONG)pace_catchup();

		if (!next.QuadPart || now.QuadPart > next.QuadPart + slack) {
			/* Worth naming how far behind, because the two cases that
			 * land here look identical in a frame counter and are not:
			 * a few frames means the pacer is being handed work too
			 * late, while hundreds mean the process was stopped. */
			if (next.QuadPart) {
				double late = (double)(now.QuadPart - next.QuadPart) /
					      (double)freq.QuadPart;

				if (late > 0.05)
					d11_log("present pacing: %.0f ms behind (%.0f "
						"frame(s)) - pacing from now instead of "
						"catching up, which would have run the "
						"game fast to make the time back",
						late * 1000.0,
						frame ? (double)(now.QuadPart -
								 next.QuadPart) /
								(double)frame
						      : 0.0);
			}
			g_pace_resets++;
			if (next.QuadPart && pace_late_skip()) {
				next.QuadPart = now.QuadPart + frame;
				return;
			}
			next.QuadPart = now.QuadPart;
		}
		next.QuadPart += frame;
	}
	for (waited = 0;; waited = 1) {
		double left;
		QueryPerformanceCounter(&now);
		left = (double)(next.QuadPart - now.QuadPart) / (double)freq.QuadPart;
		if (left <= 0.0) {
			/* Already past the deadline on arrival means the backstop
			 * never slept and something upstream ate the interval. How
			 * often that happens separates a pacer that is too slow from
			 * one that is being handed frames too late to pace. */
			if (!waited)
				g_pace_late++;
			return;
		}
		/* Sleep rounds up to the scheduler tick, so hand it only the part
		 * that comfortably clears one and spin the remainder. */
		if (left > 0.002)
			Sleep((DWORD)((left - 0.001) * 1000.0));
		else
			YieldProcessor();
	}
}

/* What the game asks of the swap chain, and what it actually gets.
 *
 * Two unmeasured numbers decide whether our pacing has anything to do with the
 * game running fast: the sync interval the game passes, because present_wait is
 * skipped outright when it is zero and the game is then limiting itself, and
 * the frame rate we really deliver.
 *
 * Sampled against a QueryPerformanceCounter resolved straight out of kernel32
 * rather than whatever the imports point at. The savestate engine offsets every
 * clock the game can read, and a pacing measurement taken on a rewound clock
 * would be describing the rewind rather than the pacing.
 *
 * Window state is reported next to it because the compositor throttles frames
 * for windows nobody can see. A fixed-timestep game slowed down that way is
 * advancing its world at a rate its own logic has no idea about, which is the
 * off-screen case worth having evidence for rather than a theory. */
static void pace_probe(HWND hwnd, UINT sync, UINT flags)
{
	static BOOL(WINAPI * qpc)(LARGE_INTEGER *);
	static LARGE_INTEGER freq, mark;
	static unsigned long frames;
	static int last_state = -1;
	static UINT last_sync = 0xFFFFFFFFu;
	LARGE_INTEGER now;
	double el;
	int state;

	if (!qpc) {
		HMODULE k = GetModuleHandleA("kernel32.dll");

		if (k)
			qpc = (BOOL(WINAPI *)(LARGE_INTEGER *))(void *)GetProcAddress(
				k, "QueryPerformanceCounter");
		if (!qpc)
			return;
		QueryPerformanceFrequency(&freq);
		qpc(&mark);
	}
	frames++;
	/* Once per distinct value rather than once per frame: the event worth
	 * seeing is the game changing its mind, which it does at mode changes. */
	if (sync != last_sync) {
		last_sync = sync;
		d11_log("present: the game asked for SyncInterval=%u flags=%x - %s", sync,
			flags,
			sync ? "our pacing applies"
			     : "our pacing is SKIPPED, so the game is limiting itself");
	}
	state = IsIconic(hwnd) ? 2 : (GetForegroundWindow() == hwnd ? 0 : 1);
	if (state != last_state) {
		last_state = state;
		d11_log("present: window is now %s", state == 2	  ? "MINIMISED"
						     : state == 1 ? "in the background"
								  : "in the foreground");
		/* Also told to the savestate engine, so its fault report can say
		 * whether a focus change preceded a crash without the two logs having
		 * to be lined up by file timestamp afterwards. */
		savestate_note_focus(state);
	}
	if (!qpc(&now) || !freq.QuadPart)
		return;
	el = (double)(now.QuadPart - mark.QuadPart) / (double)freq.QuadPart;
	if (el >= 2.0) {
		d11_log("present: %.1f fps over %.1f s (%s, SyncInterval=%u)", frames / el, el,
			state == 2 ? "minimised" : state == 1 ? "background" : "foreground",
			sync);
		mark = now;
		frames = 0;
	}
}

static HRESULT WINAPI Swap_Present(IDXGISwapChain1 *this, UINT sync, UINT flags)
{
	Sw11Swap *s = (Sw11Swap *)this;
	SwRast r;
	int k, soak_act;
	(void)flags;
	d11_trace("Present #%d %ux%u hwnd=%p bbwrites=%u nrt=%d cbmaps=%ld bufmaps=%ld",
		s->bb ? s->bb->id : -1, s->bb ? s->bb->width : 0, s->bb ? s->bb->height : 0,
		s->hwnd, g_bb_writes, g_frame_nrt, g_cb_maps, g_buf_maps);
	/* Software CBs close here. Copy from this Present only after idle;
	 * never freeze the presenter mid-record, never End a dead encoder. */
	swrast_flush();
	{
		int i, best = -1;
		for (i = 0; i < g_frame_nrt; i++) {
			Sw11Res *r2 = g_frame_rt[i].res;
			d11_trace("  frame rt #%d %ux%u bb=%d draws=%u tris=%u", r2->id, r2->width,
				r2->height, r2->is_bb, g_frame_rt[i].draws, g_frame_rt[i].tris);
			if (r2->is_bb || !r2->pixels)
				continue;
			if (best < 0 || g_frame_rt[i].tris > g_frame_rt[best].tris)
				best = i;
		}
		if (!g_bb_writes && best >= 0 && s->bb && res_ensure_pixels(s->bb)) {
			d11_log("  rescue: compositing rt #%d %ux%u onto backbuffer",
				g_frame_rt[best].res->id, g_frame_rt[best].res->width,
				g_frame_rt[best].res->height);
			composite_upscale(s->bb, g_frame_rt[best].res);
		}
		g_frame_nrt = 0;
		g_bb_writes = 0;
		memset(g_frame_rt, 0, sizeof(g_frame_rt));
	}
	{
		static unsigned reported[8];
		int w, bit;
		for (w = 0; w < 8; w++) {
			unsigned fresh = dxbc_unhandled_ops[w] & ~reported[w];
			if (!fresh)
				continue;
			reported[w] |= fresh;
			for (bit = 0; bit < 32; bit++)
				if (fresh & (1u << bit))
					d11_log("VS opcode %d unimplemented", w * 32 + bit);
		}
	}
	/* Not at DLL_PROCESS_ATTACH, which is where this went first and where it
	 * was invisible: the savestate log does not exist that early, so every
	 * line the installer wrote went nowhere, and the config read as unset
	 * while the session header printed it as 1. Here both are up, and the
	 * result lands in the log the reader is already looking at. */
	{
		static int gh_tried;

		if (!gh_tried) {
			gh_tried = 1;
			d11_log("gameheap: %d import slot(s) redirected",
				gameheap_install());
			/* Here rather than at attach for the same reason: the log
			 * does not exist yet at attach, and a probe whose findings
			 * go into a closed file has not found anything. */
			gpuprobe_run(gpuprobe_sink);
			/* The backend brings itself up on the first present, which is
			 * the first moment the window is reachable. This only tells
			 * it where to say so. */
			gpu_set_log(gpuprobe_sink);
			/* Registered only when wanted, so the blackout can be taken
			 * out of the experiment entirely rather than merely turned
			 * down. D3D11SW_GPU_PARK=0 leaves the adapter running across
			 * a park, which is the other half of the question: does the
			 * freeze kill this, or does standing the adapter down kill
			 * it? One of the two is innocent and they cannot both be
			 * tested at once. */
			{
				char pk[8];

				if (!savestate_getenv("D3D11SW_GPU_PARK", pk, sizeof(pk)) ||
				    pk[0] != '0')
					savestate_set_gpu_park(gpu_park);
				else
					d11_log("gpu: park hook NOT installed - the adapter "
						"keeps running across a freeze");
			}
			if (gpu_mode() == 1)
				swrast_set_gpu_present(gpu_present_framebuffer);
			savestate_set_gpu_park(gpu_park);
		}
	}
	/* Timed as a block: these run before any zone opens, so without this they
	 * are indistinguishable from the game's own time in the split line. */
	{
		LARGE_INTEGER h0, h1;

		QueryPerformanceCounter(&h0);
		savestate_guard();
		savestate_pos_watch();
		savestate_watch_tick();
		savestate_object_watch();
		QueryPerformanceCounter(&h1);
		g_zone[ZONE_HOOKS] += h1.QuadPart - h0.QuadPart;
	}
	/* F6 arms the soak driver, which then drives save and restore by itself.
	 * Read once per frame outside the slot loop so the keystroke is consumed
	 * exactly once however many slots there are. */
	if (savestate_key_edge(VK_F6))
		savestate_soak_arm();
	soak_act = savestate_soak_action();
	for (k = 0; k < SAVESTATE_SLOTS; k++) {
		int want_load;
		int hk = savestate_hotkey(k);

		/* Hotkey or soak, but one body below either way. A separate soak
		 * call site would have to repeat ledger_register, the generation
		 * bump, ledger_mark and the retain flush, and any one of those
		 * missed is the retain-and-reap double free. */
		if (hk != SS_HOTKEY_NONE)
			want_load = (hk == SS_HOTKEY_LOAD);
		else if (k == 0 && soak_act != SS_SOAK_NOTHING)
			want_load = (soak_act == SS_SOAK_LOAD) ? 1 : 0;
		else
			continue;
		ledger_register();
		if (!want_load)
			pin_map_write(k);
		if (want_load) {
			if (savestate_load(k)) {
				ledger_reap();
				d11_log("savestate restored slot %d in %.1f ms", k,
					savestate_last_ms());
			}
		} else if (savestate_save(k)) {
			/* A restored thread resumes inside the save it was taking and
			 * returns through it, so a successful restore lands here and
			 * not in the branch above. Labelling it by which function was
			 * called reported restores as saves, and the absent "restored"
			 * line was then read as the process having died on the way out
			 * of one. */
			if (savestate_last_was_restore()) {
				/* Once one load came from another launch, the pool holds that
				 * launch's layout, and a later load of a slot from this one
				 * names this one's. Either way the pool has to be made to
				 * match the slot, after the reap so it cannot free what the
				 * rebuild creates. */
				if (savestate_last_load_foreign())
					g_pin_foreign_seen = 1;
				ledger_reap();
				if (g_pin_foreign_seen)
					pin_map_compare(k);
				/* Behavior: recreate GPU DXGI from the live HWND.
				 * Software swapchain COM lives on the held wrapper
				 * heap; real adapter COM must not be memcpy'd. */
				gpu_dxgi_reconcile(s->hwnd);
				d11_log("savestate restored slot %d in %.1f ms, resumed "
					"through the save it was taking",
					k, savestate_last_ms());
			} else {
				d11_log("savestate saved slot %d, %.1f MB in %.1f ms", k,
					savestate_last_mb(), savestate_last_ms());
				/* Before the retain flush: everything the new snapshot can
				 * refer to must be marked as protected first, so a payload
				 * released a moment later is not mistaken for an orphan by
				 * the next restore. */
				/* First of the three, because the save has already
				 * returned and the other threads are running. An
				 * object created between here and ledger_mark is
				 * stamped with the new generation, so it will not be
				 * retained, and lands below the mark, so the reap
				 * will not free it either - it simply gets released
				 * the ordinary way. Incrementing last left that same
				 * window meaning both instead of neither, which is
				 * the retain-and-reap double free, and the flush
				 * below can run for thousands of objects, so the
				 * window is not small. */
				InterlockedIncrement(&g_save_gen);
				save_gen_note_once();
				ledger_mark();
				/* The new snapshot replaces the old one, so texture content
				 * held back for the old one is finally free to go. Done
				 * AFTER the save rather than before, because a release
				 * during the save still belongs to the outgoing snapshot. */
				d3d11sw_retain_flush();
			}
		}
	}
	/* F7 takes a heap census. Separate key because its whole value is being
	 * usable without saving anything: press it before a scene transition and
	 * again after, and the log says what the transition did to the heap. */
	/* F11 marks the player position, shift-F11 puts it back. Deliberately not
	 * wired through the slot loop above: this writes eight bytes and needs
	 * none of the ledger work a real savestate does. */
	if (savestate_key_edge(VK_F11)) {
		if (savestate_key_held(VK_SHIFT))
			savestate_pos_restore();
		else
			savestate_pos_mark();
	}
	/* Shift+F7 writes the game's decrypted image out for Ghidra. On the
	 * diagnostics key because that is what it is, and behind shift because the
	 * census is the thing you want ninety-nine times out of a hundred. */
	if (savestate_key_edge(VK_F7)) {
		if (savestate_key_held(VK_SHIFT)) {
			d11_log("dump: writing the game's image out as it exists in "
				"memory");
			if (!savestate_dump_image())
				d11_log("dump: FAILED - see the savestate log for why");
		} else {
			savestate_chain_probe();
			savestate_object_report();
			savestate_probe_census();
			savestate_census();
			/* Reported next to the census because retiring objects trades
			 * memory for the absence of a dangling pointer, and the trade
			 * is only defensible while the number stays small. */
			d11_log("heap census taken | retired objects: %ld holding %.2f MB",
				(long)g_retired_n,
				(double)g_retired_bytes / (1024.0 * 1024.0));
			res_census();
		}
	}
	/* Shift+F9 parks the process: every thread held still for a while with no
	 * memory read or written, then let go. It is the control for every restore
	 * failure we have, because a restore freezes, copies and writes back, and
	 * only the last two have ever been varied. Length comes from
	 * D3D9SW_PARK_MS so the same key can ask a harder question.
	 *
	 * This was F10 first and never once fired. F10 is a Windows system key:
	 * DefWindowProc takes WM_SYSKEYDOWN as menu activation and the window stops
	 * pumping frames until the key comes back up. Every hotkey here is read
	 * inside Present, so a key that suspends presenting can never be seen -
	 * the poll next runs after the release and the edge has already gone.
	 *
	 * Sharing F9 with the census, on the same shift convention F5 already uses
	 * for load, rather than picking another bare function key the game might
	 * want for itself. */
	if (savestate_key_edge(VK_F9)) {
		if (savestate_key_held(VK_SHIFT)) {
			char v[16];
			unsigned n = savestate_getenv("D3D9SW_PARK_MS", v, sizeof(v));
			int ms = 0;
			unsigned i;

			for (i = 0; i < n && v[i] >= '0' && v[i] <= '9'; i++)
				ms = ms * 10 + (v[i] - '0');
			/* Announced from both logs. The savestate log is written
			 * through a handle that does not exist until the first save,
			 * so a park taken before one would otherwise leave no trace
			 * anywhere and read as a key that never fired. */
			d11_log("park: requested, %d ms", ms ? ms : 1000);
			if (savestate_park(ms ? ms : 1000))
				d11_log("park: returned, the game is running again");
			else
				d11_log("park: REFUSED - the savestate engine could not "
					"bring up its control block, so nothing was held");
		} else {
			/* Arm here, act at the top of the next frame, so the census
			 * covers a whole frame from its first draw rather than
			 * joining one midway. */
			InterlockedExchange(&g_census_arm, 1);
		}
	}
	if (g_census_on) {
		d11_log("==== end of frame census: %u draw(s) ====", g_census_seq);
		g_census_on = 0;
		g_census_seq = 0;
	}
	if (InterlockedExchange(&g_census_arm, 0)) {
		g_census_on = 1;
		g_census_seq = 0;
		d11_log("==== F9 draw census, one frame ====");
	}
	osd_update();
	/* Refresh the fault handler's module table from a context where taking
	 * the loader lock is safe. Cheap, and a stale table is what makes a
	 * crash report unreadable. */
	{
		static unsigned mod_tick;

		if (++mod_tick % 240 == 1)
			mod_snapshot();
	}
	memset(&r, 0, sizeof(r));
	if (s->bb && res_ensure_pixels(s->bb)) {
		r.color = s->bb->pixels;
		r.width = (int)s->bb->width;
		r.height = (int)s->bb->height;
		r.hwnd = s->hwnd;
		{
			LARGE_INTEGER a, b;
			double r0 = swrast_prof_peek_raster();
			QueryPerformanceCounter(&a);
			/* At mode 2 the adapter's frame is the one shown, so the
			 * rasteriser's output still has to be finished - readback and
			 * the savestate read it - but must not also be blitted, or
			 * the window would receive two different images a frame. */
			/* Here because this is the first place the window is
			 * reachable. It costs the first frame's draws, which no one
			 * will see. */
			if (!gh_remote() && gpu_mode() >= 2)
				gpu_ensure(s->hwnd);
			{
				static int gh_hooked;

				if (!gh_hooked && gh_on()) {
					gh_hooked = 1;
					swrast_set_gpu_present(gh_present);
				}
			}
			/* Once a frame, after the draws: a texture loaded during
			 * this frame is then moved before the next one, and the
			 * game's own loading pauses are where most of them arrive. */
			if (gpu_mode() >= 3)
				gpu_sweep_release();
			if (g_gpu_open) {
				swrast_flush();
				if (!gpu_frame_end())
					swrast_present(&r, s->hwnd);
				g_gpu_open = 0;
			} else {
				swrast_present(&r, s->hwnd);
			}
			if (gh_remote())
				rg_tick();
			QueryPerformanceCounter(&b);
			g_zone[ZONE_PRESENT] += b.QuadPart - a.QuadPart;
			g_raster_nested_ms += swrast_prof_peek_raster() - r0;
		}
	}
	/* After the blit, so the wait overlaps nothing the player is waiting on.
	 * D3D11SW_VSYNC=0 restores the old free-running behaviour for measuring
	 * how fast the rasteriser can actually go. */
	if (sync && vsync_on()) {
		LARGE_INTEGER p0, p1;

		QueryPerformanceCounter(&p0);
		present_wait(s->hwnd, sync);
		QueryPerformanceCounter(&p1);
		g_zone[ZONE_PACE] += p1.QuadPart - p0.QuadPart;
	}
	pace_probe(s->hwnd, sync, flags);
	perf_tick();
	return S_OK;
}

static HRESULT WINAPI Swap_GetBuffer(IDXGISwapChain1 *this, UINT i, REFIID riid, void **pp)
{
	Sw11Swap *s = (Sw11Swap *)this;
	if (!pp)
		return E_POINTER;
	*pp = NULL;
	if (i != 0 || !s->bb)
		return DXGI_ERROR_INVALID_CALL;
	return Tex_QI(&s->bb->iface.tex, riid, pp);
}

/* This used to return S_OK without doing anything, which quietly made the
 * expensive case the only case: the desktop stayed at its native size, the game
 * sized its backbuffer to match, and every one of those pixels came out of the
 * software rasteriser. Switching the display mode instead hands the upscale to
 * the display engine, where it is part of scanout rather than work per frame.
 * That is the one place scaling is genuinely free to us. */
static HRESULT WINAPI Swap_SetFullscreenState(IDXGISwapChain1 *this, BOOL fs, IDXGIOutput *o)
{
	Sw11Swap *s = (Sw11Swap *)this;
	UINT w, h;
	(void)o;
	if (!s || !s->hwnd)
		return S_OK;
	if (!!fs == s->fullscreen)
		return S_OK;
	w = s->desc.BufferDesc.Width;
	h = s->desc.BufferDesc.Height;
	if (fs) {
		DEVMODEA dm;
		LONG rc;
		memset(&dm, 0, sizeof(dm));
		dm.dmSize = sizeof(dm);
		dm.dmPelsWidth = w;
		dm.dmPelsHeight = h;
		dm.dmBitsPerPel = 32;
		dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
		rc = ChangeDisplaySettingsExA(NULL, &dm, NULL, CDS_FULLSCREEN, NULL);
		s->saved_style = GetWindowLongA(s->hwnd, GWL_STYLE);
		s->saved_exstyle = GetWindowLongA(s->hwnd, GWL_EXSTYLE);
		GetWindowRect(s->hwnd, &s->saved_rect);
		SetWindowLongA(s->hwnd, GWL_STYLE,
			       (s->saved_style & ~(WS_OVERLAPPEDWINDOW)) | WS_POPUP);
		SetWindowPos(s->hwnd, HWND_TOP, 0, 0, (int)w, (int)h,
			     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
		s->fullscreen = 1;
		d11_log("SetFullscreenState on: mode %ux%u rc=%ld (0 = applied, display "
			"scaler now does the upscale)",
			w, h, rc);
	} else {
		ChangeDisplaySettingsExA(NULL, NULL, NULL, 0, NULL);
		if (s->saved_style)
			SetWindowLongA(s->hwnd, GWL_STYLE, s->saved_style);
		if (s->saved_exstyle)
			SetWindowLongA(s->hwnd, GWL_EXSTYLE, s->saved_exstyle);
		SetWindowPos(s->hwnd, HWND_NOTOPMOST, s->saved_rect.left, s->saved_rect.top,
			     s->saved_rect.right - s->saved_rect.left,
			     s->saved_rect.bottom - s->saved_rect.top,
			     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
		s->fullscreen = 0;
		d11_log("SetFullscreenState off: display mode restored");
	}
	return S_OK;
}
static HRESULT WINAPI Swap_GetFullscreenState(IDXGISwapChain1 *this, BOOL *fs, IDXGIOutput **o)
{
	Sw11Swap *s = (Sw11Swap *)this;
	if (fs)
		*fs = (s && s->fullscreen) ? TRUE : FALSE;
	if (o)
		*o = NULL;
	return S_OK;
}
static HRESULT WINAPI Swap_GetDesc(IDXGISwapChain1 *this, DXGI_SWAP_CHAIN_DESC *d)
{
	if (!d)
		return E_POINTER;
	*d = ((Sw11Swap *)this)->desc;
	return S_OK;
}
static HRESULT WINAPI Swap_ResizeBuffers(IDXGISwapChain1 *this, UINT count, UINT w, UINT h,
					   DXGI_FORMAT fmt, UINT flags)
{
	Sw11Swap *s = (Sw11Swap *)this;
	D3D11_TEXTURE2D_DESC td;
	Sw11Res *nb;
	HRESULT hr;
	UINT vw, vh;
	(void)count;
	(void)flags;
	if (!w)
		w = s->desc.BufferDesc.Width;
	if (!h)
		h = s->desc.BufferDesc.Height;
	if (!fmt)
		fmt = s->desc.BufferDesc.Format;
	/* This game calls ResizeBuffers every frame with the size it already has -
	 * 13387 calls for 13387 presents. Honouring each one means allocating and
	 * freeing the backbuffer once a frame, and throwing away every render
	 * target view the game holds on it, all to arrive back where we started.
	 * Nothing observable changes by declining. */
	if (s->bb && w == s->desc.BufferDesc.Width && h == s->desc.BufferDesc.Height &&
	    fmt == s->desc.BufferDesc.Format)
		return S_OK;
	backbuffer_cap(&w, &h, &vw, &vh, "ResizeBuffers");
	memset(&td, 0, sizeof(td));
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	td.Usage = D3D11_USAGE_DEFAULT;
	hr = create_tex2d(s->dev, &td, NULL, &nb);
	if (FAILED(hr))
		return hr;
	nb->is_bb = 1;
	nb->virt_w = vw;
	nb->virt_h = vh;
	if (s->bb)
		Res_Release(s->bb);
	s->bb = nb;
	s->desc.BufferDesc.Width = vw ? vw : w;
	s->desc.BufferDesc.Height = vh ? vh : h;
	s->desc.BufferDesc.Format = fmt;
	return S_OK;
}
static HRESULT WINAPI Swap_ResizeTarget(IDXGISwapChain1 *this, const DXGI_MODE_DESC *m)
{
	Sw11Swap *s = (Sw11Swap *)this;
	if (!s || !s->hwnd || !m || !m->Width || !m->Height)
		return S_OK;
	if (s->fullscreen) {
		DEVMODEA dm;
		memset(&dm, 0, sizeof(dm));
		dm.dmSize = sizeof(dm);
		dm.dmPelsWidth = m->Width;
		dm.dmPelsHeight = m->Height;
		dm.dmBitsPerPel = 32;
		dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
		ChangeDisplaySettingsExA(NULL, &dm, NULL, CDS_FULLSCREEN, NULL);
		SetWindowPos(s->hwnd, HWND_TOP, 0, 0, (int)m->Width, (int)m->Height,
			     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	}
	/* Deliberately not touching the window while windowed. DXGI would resize
	 * it, but the game already owns its own window there, and doing it anyway
	 * hands Unity a client rect it did not ask for at startup. */
	d11_log("ResizeTarget %ux%u fs=%d", m->Width, m->Height, s->fullscreen);
	return S_OK;
}
static HRESULT WINAPI Swap_GetContainingOutput(IDXGISwapChain1 *this, IDXGIOutput **out)
{
	return Adp_EnumOutputs(&((Sw11Swap *)this)->dev->adapter->iface, 0, out);
}
static HRESULT WINAPI Swap_GetFrameStatistics(IDXGISwapChain1 *this, DXGI_FRAME_STATISTICS *st)
{
	(void)this;
	if (st)
		memset(st, 0, sizeof(*st));
	return S_OK;
}
static HRESULT WINAPI Swap_GetLastPresentCount(IDXGISwapChain1 *this, UINT *c)
{
	if (c)
		*c = 0;
	(void)this;
	return S_OK;
}
static HRESULT WINAPI Swap_GetDesc1(IDXGISwapChain1 *this, DXGI_SWAP_CHAIN_DESC1 *d)
{
	Sw11Swap *s = (Sw11Swap *)this;
	if (!d)
		return E_POINTER;
	memset(d, 0, sizeof(*d));
	d->Width = s->desc.BufferDesc.Width;
	d->Height = s->desc.BufferDesc.Height;
	d->Format = s->desc.BufferDesc.Format;
	d->SampleDesc = s->desc.SampleDesc;
	d->BufferUsage = s->desc.BufferUsage;
	d->BufferCount = s->desc.BufferCount;
	d->SwapEffect = s->desc.SwapEffect;
	d->Flags = s->desc.Flags;
	return S_OK;
}
static HRESULT WINAPI Swap_GetFullscreenDesc(IDXGISwapChain1 *this,
						DXGI_SWAP_CHAIN_FULLSCREEN_DESC *d)
{
	(void)this;
	if (d)
		memset(d, 0, sizeof(*d));
	if (d)
		d->Windowed = TRUE;
	return S_OK;
}
static HRESULT WINAPI Swap_GetHwnd(IDXGISwapChain1 *this, HWND *hwnd)
{
	if (!hwnd)
		return E_POINTER;
	*hwnd = ((Sw11Swap *)this)->hwnd;
	return S_OK;
}
static HRESULT WINAPI Swap_GetCoreWindow(IDXGISwapChain1 *this, REFIID riid, void **pp)
{
	(void)this;
	(void)riid;
	if (pp)
		*pp = NULL;
	return DXGI_ERROR_INVALID_CALL;
}
static HRESULT WINAPI Swap_Present1(IDXGISwapChain1 *this, UINT sync, UINT flags,
				     const DXGI_PRESENT_PARAMETERS *p)
{
	(void)p;
	return Swap_Present(this, sync, flags);
}
static BOOL WINAPI Swap_IsTemporaryMonoSupported(IDXGISwapChain1 *this)
{
	(void)this;
	return FALSE;
}
static HRESULT WINAPI Swap_GetRestrictToOutput(IDXGISwapChain1 *this, IDXGIOutput **o)
{
	if (o)
		*o = NULL;
	(void)this;
	return S_OK;
}
static HRESULT WINAPI Swap_SetBackgroundColor(IDXGISwapChain1 *this, const DXGI_RGBA *c)
{
	(void)this;
	(void)c;
	return S_OK;
}
static HRESULT WINAPI Swap_GetBackgroundColor(IDXGISwapChain1 *this, DXGI_RGBA *c)
{
	(void)this;
	if (c)
		memset(c, 0, sizeof(*c));
	return S_OK;
}
static HRESULT WINAPI Swap_SetRotation(IDXGISwapChain1 *this, DXGI_MODE_ROTATION r)
{
	(void)this;
	(void)r;
	return S_OK;
}
static HRESULT WINAPI Swap_GetRotation(IDXGISwapChain1 *this, DXGI_MODE_ROTATION *r)
{
	if (r)
		*r = DXGI_MODE_ROTATION_IDENTITY;
	return S_OK;
}

static HRESULT sw11_create_swap(Sw11Device *dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC *desc,
				Sw11Swap **out)
{
	Sw11Swap *s;
	D3D11_TEXTURE2D_DESC td;
	HRESULT hr;
	UINT w, h, vw, vh;
	*out = NULL;
	s = (Sw11Swap *)pin_calloc(sizeof(*s), "swap chain", PK_SINGLE);
	if (!s)
		return E_OUTOFMEMORY;
	s->iface.lpVtbl = &kSwapVtbl;
	s->ref = 1;
	s->dev = dev;
	s->hwnd = hwnd ? hwnd : desc->OutputWindow;
	s->desc = *desc;
	s->desc.OutputWindow = s->hwnd;
	w = desc->BufferDesc.Width;
	h = desc->BufferDesc.Height;
	if (!w || !h) {
		RECT rc;
		if (s->hwnd && GetClientRect(s->hwnd, &rc)) {
			w = (UINT)(rc.right - rc.left);
			h = (UINT)(rc.bottom - rc.top);
		}
		if (!w)
			w = 1280;
		if (!h)
			h = 720;
	}
	backbuffer_cap(&w, &h, &vw, &vh, "swapchain");
	/* Written back unconditionally. This used to happen only when the game
	 * asked for a zero size, which left s->desc holding the requested size
	 * rather than the one actually created - harmless while they were always
	 * equal, and wrong the moment anything adjusts them. */
	s->desc.BufferDesc.Width = vw ? vw : w;
	s->desc.BufferDesc.Height = vh ? vh : h;
	if (!s->desc.BufferDesc.Format)
		s->desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	memset(&td, 0, sizeof(td));
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = s->desc.BufferDesc.Format;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	hr = create_tex2d(dev, &td, NULL, &s->bb);
	if (FAILED(hr)) {
		obj_free(s);
		return hr;
	}
	s->bb->is_bb = 1;
	s->bb->virt_w = vw;
	s->bb->virt_h = vh;
	place_on_monitor(s->hwnd);
	d11_log("swapchain %ux%u fmt=%d hwnd=%p%s", w, h, (int)s->desc.BufferDesc.Format,
		s->hwnd,
		vw ? " (reported to the game as its requested size, viewport scales)" : "");
	*out = s;
	return S_OK;
}

/* ---------- device ---------- */

static HRESULT WINAPI Dev_QI(ID3D11Device1 *this, REFIID riid, void **ppv)
{
	Sw11Device *d = (Sw11Device *)this;
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_ID3D11Device) ||
	    guid_eq(riid, &IID_ID3D11Device1)) {
		*ppv = this;
		InterlockedIncrement(&d->ref);
		return S_OK;
	}
	if (guid_eq(riid, &IID_IDXGIDevice) || guid_eq(riid, &IID_IDXGIDevice1) ||
	    guid_eq(riid, &IID_IDXGIObject)) {
		*ppv = &d->dxgi;
		InterlockedIncrement(&d->ref);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI Dev_AddRef(ID3D11Device1 *this)
{
	return (ULONG)InterlockedIncrement(&((Sw11Device *)this)->ref);
}

static ULONG WINAPI Dev_Release(ID3D11Device1 *this)
{
	Sw11Device *d = (Sw11Device *)this;
	LONG n = InterlockedDecrement(&d->ref);
	if (n == 0) {
		if (d->imm) {
			d->imm->dev = NULL;
			d->imm->iface.lpVtbl->Release(&d->imm->iface);
		}
		if (d->adapter)
			d->adapter->iface.lpVtbl->Release(&d->adapter->iface);
		if (d->factory)
			d->factory->iface.lpVtbl->Release(&d->factory->iface);
		priv_free(d->priv);
		DeleteCriticalSection(&d->lock);
		obj_free(d);
	}
	return (ULONG)n;
}

static HRESULT WINAPI Dev_CreateBuffer(ID3D11Device1 *this, const D3D11_BUFFER_DESC *desc,
					 const D3D11_SUBRESOURCE_DATA *init, ID3D11Buffer **out)
{
	Sw11Res *r;
	HRESULT hr;
	if (!out)
		return E_POINTER;
	*out = NULL;
	hr = create_buf((Sw11Device *)this, desc, init, &r);
	if (FAILED(hr))
		return hr;
	*out = &r->iface.buf;
	return S_OK;
}

static HRESULT WINAPI Dev_CreateTexture1D(ID3D11Device1 *this, const D3D11_TEXTURE1D_DESC *desc,
					       const D3D11_SUBRESOURCE_DATA *init,
					       ID3D11Texture1D **out)
{
	D3D11_TEXTURE2D_DESC d2;
	Sw11Res *r;
	HRESULT hr;
	(void)this;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!desc)
		return E_INVALIDARG;
	memset(&d2, 0, sizeof(d2));
	d2.Width = desc->Width;
	d2.Height = 1;
	d2.MipLevels = desc->MipLevels;
	d2.ArraySize = desc->ArraySize;
	d2.Format = desc->Format;
	d2.SampleDesc.Count = 1;
	d2.Usage = desc->Usage;
	d2.BindFlags = desc->BindFlags;
	d2.CPUAccessFlags = desc->CPUAccessFlags;
	hr = create_tex2d((Sw11Device *)this, &d2, init, &r);
	if (FAILED(hr))
		return hr;
	*out = (ID3D11Texture1D *)&r->iface.tex;
	return S_OK;
}

static HRESULT WINAPI Dev_CreateTexture2D(ID3D11Device1 *this, const D3D11_TEXTURE2D_DESC *desc,
					       const D3D11_SUBRESOURCE_DATA *init,
					       ID3D11Texture2D **out)
{
	Sw11Res *r;
	HRESULT hr;
	if (!out)
		return E_POINTER;
	*out = NULL;
	hr = create_tex2d((Sw11Device *)this, desc, init, &r);
	if (FAILED(hr))
		return hr;
	d11_trace("CreateTexture2D #%d %ux%u fmt=%d bind=%x usage=%u cpu=%x init=%d", r->id, r->width,
		r->height, (int)r->format, r->bind, desc->Usage, desc->CPUAccessFlags,
		init && init->pSysMem ? 1 : 0);
	*out = &r->iface.tex;
	return S_OK;
}

static HRESULT WINAPI Dev_CreateTexture3D(ID3D11Device1 *this, const D3D11_TEXTURE3D_DESC *desc,
					       const D3D11_SUBRESOURCE_DATA *init,
					       ID3D11Texture3D **out)
{
	D3D11_TEXTURE2D_DESC d2;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!desc)
		return E_INVALIDARG;
	memset(&d2, 0, sizeof(d2));
	d2.Width = desc->Width;
	d2.Height = desc->Height;
	d2.MipLevels = 1;
	d2.ArraySize = 1;
	d2.Format = desc->Format;
	d2.SampleDesc.Count = 1;
	d2.Usage = desc->Usage;
	d2.BindFlags = desc->BindFlags;
	d2.CPUAccessFlags = desc->CPUAccessFlags;
	return Dev_CreateTexture2D(this, &d2, init, (ID3D11Texture2D **)out);
}

static HRESULT WINAPI Dev_CreateSRV(ID3D11Device1 *this, ID3D11Resource *res,
					const D3D11_SHADER_RESOURCE_VIEW_DESC *desc,
					ID3D11ShaderResourceView **out)
{
	Sw11Res *r = res_from_unk(res);
	Sw11View *v;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!r)
		return E_INVALIDARG;
	res_ensure_pixels(r);
	if (r->cpu && r->pixels && !(r->bind & D3D11_BIND_RENDER_TARGET))
		res_decode_pixels(r);
	v = view_new((Sw11Device *)this, r, 1, desc ? desc->Format : r->format);
	if (!v)
		return E_OUTOFMEMORY;
	*out = &v->iface.srv;
	return S_OK;
}

static HRESULT WINAPI Dev_CreateUAV(ID3D11Device1 *this, ID3D11Resource *res,
					const D3D11_UNORDERED_ACCESS_VIEW_DESC *desc,
					ID3D11UnorderedAccessView **out)
{
	(void)desc;
	return Dev_CreateSRV(this, res, NULL, (ID3D11ShaderResourceView **)out);
}

static HRESULT WINAPI Dev_CreateRTV(ID3D11Device1 *this, ID3D11Resource *res,
					const D3D11_RENDER_TARGET_VIEW_DESC *desc,
					ID3D11RenderTargetView **out)
{
	Sw11Res *r = res_from_unk(res);
	Sw11View *v;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!r)
		return E_INVALIDARG;
	res_ensure_pixels(r);
	v = view_new((Sw11Device *)this, r, 0, desc ? desc->Format : r->format);
	if (!v)
		return E_OUTOFMEMORY;
	*out = &v->iface.rtv;
	return S_OK;
}

static HRESULT WINAPI Dev_CreateDSV(ID3D11Device1 *this, ID3D11Resource *res,
					const D3D11_DEPTH_STENCIL_VIEW_DESC *desc,
					ID3D11DepthStencilView **out)
{
	Sw11Res *r = res_from_unk(res);
	Sw11View *v;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!r)
		return E_INVALIDARG;
	res_ensure_pixels(r);
	v = view_new((Sw11Device *)this, r, 2, desc ? desc->Format : r->format);
	if (!v)
		return E_OUTOFMEMORY;
	*out = &v->iface.dsv;
	return S_OK;
}

static HRESULT WINAPI Dev_CreateInputLayout(ID3D11Device1 *this,
					      const D3D11_INPUT_ELEMENT_DESC *elems, UINT n,
					      const void *sig, SIZE_T sig_n,
					      ID3D11InputLayout **out)
{
	Sw11Layout *l;
	UINT i, off[16];
	(void)sig;
	(void)sig_n;
	if (!out)
		return E_POINTER;
	*out = NULL;
	l = (Sw11Layout *)pin_calloc(sizeof(*l), "layout", PK_LAYOUT);
	if (!l)
		return E_OUTOFMEMORY;
	l->iface.lpVtbl = &kLayVtbl;
	l->ref = 1;
	l->dev = (Sw11Device *)this;
	l->n = n;
	l->elems = (D3D11_INPUT_ELEMENT_DESC *)calloc(n ? n : 1, sizeof(*l->elems));
	if (!l->elems) {
		obj_free(l);
		return E_OUTOFMEMORY;
	}
	memset(off, 0, sizeof(off));
	for (i = 0; i < n; i++) {
		l->elems[i] = elems[i];
		if (elems[i].SemanticName) {
			size_t len = strlen(elems[i].SemanticName) + 1;
			char *nm = (char *)malloc(len);
			if (nm)
				memcpy(nm, elems[i].SemanticName, len);
			l->elems[i].SemanticName = nm;
		}
		if (l->elems[i].AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT) {
			UINT slot = l->elems[i].InputSlot;
			if (slot >= 16)
				slot = 0;
			l->elems[i].AlignedByteOffset = off[slot];
		}
		{
			UINT slot = l->elems[i].InputSlot;
			UINT add = fmt_stride(l->elems[i].Format);
			if (slot < 16)
				off[slot] = l->elems[i].AlignedByteOffset + add;
		}
	}
	*out = &l->iface;
	return S_OK;
}

static HRESULT WINAPI Dev_CreateVS(ID3D11Device1 *this, const void *code, SIZE_T n,
				      ID3D11ClassLinkage *link, ID3D11VertexShader **out)
{
	Sw11Shader *s;
	(void)link;
	if (!out)
		return E_POINTER;
	*out = NULL;
	s = shader_new((Sw11Device *)this, code, n, 0);
	if (!s)
		return E_OUTOFMEMORY;
	{
		static volatile LONG nvs;
		if (s->parsed && InterlockedIncrement(&nvs) <= 6) {
			unsigned k;
			for (k = 0; k < s->dxbc.n_isgn && k < 8; k++)
				d11_log("VS isgn[%u] '%s' idx=%u reg=%u mask=%x", k,
					s->dxbc.isgn[k].name, s->dxbc.isgn[k].sem_index,
					s->dxbc.isgn[k].reg, s->dxbc.isgn[k].mask);
			for (k = 0; k < s->dxbc.n_osgn && k < 8; k++)
				d11_log("VS osgn[%u] '%s' idx=%u reg=%u sysval=%u", k,
					s->dxbc.osgn[k].name, s->dxbc.osgn[k].sem_index,
					s->dxbc.osgn[k].reg, s->dxbc.osgn[k].sysval);
		}
	}
	*out = &s->iface;
	return S_OK;
}

static HRESULT WINAPI Dev_CreateDummyShader(ID3D11Device1 *this, const void *code, SIZE_T n,
					       ID3D11ClassLinkage *link, void **out)
{
	Sw11Shader *s;
	(void)link;
	if (!out)
		return E_POINTER;
	*out = NULL;
	s = shader_new((Sw11Device *)this, code, n, 1);
	if (!s)
		return E_OUTOFMEMORY;
	*out = &s->iface;
	return S_OK;
}

static HRESULT WINAPI Dev_CreateGS(ID3D11Device1 *this, const void *code, SIZE_T n,
				      ID3D11ClassLinkage *link, ID3D11GeometryShader **out)
{
	return Dev_CreateDummyShader(this, code, n, link, (void **)out);
}

static HRESULT WINAPI Dev_CreateGSSO(ID3D11Device1 *this, const void *code, SIZE_T n,
					const D3D11_SO_DECLARATION_ENTRY *so, UINT nso,
					const UINT *strides, UINT nstr, UINT stream,
					ID3D11ClassLinkage *link, ID3D11GeometryShader **out)
{
	(void)so;
	(void)nso;
	(void)strides;
	(void)nstr;
	(void)stream;
	return Dev_CreateGS(this, code, n, link, out);
}

static HRESULT WINAPI Dev_CreatePS(ID3D11Device1 *this, const void *code, SIZE_T n,
				      ID3D11ClassLinkage *link, ID3D11PixelShader **out)
{
	Sw11Shader *s;
	(void)link;
	if (!out)
		return E_POINTER;
	*out = NULL;
	s = shader_new((Sw11Device *)this, code, n, 1);
	if (!s)
		return E_OUTOFMEMORY;
	*out = (ID3D11PixelShader *)&s->iface;
	return S_OK;
}

static HRESULT WINAPI Dev_CreateHS(ID3D11Device1 *this, const void *code, SIZE_T n,
				      ID3D11ClassLinkage *link, ID3D11HullShader **out)
{
	return Dev_CreateDummyShader(this, code, n, link, (void **)out);
}
static HRESULT WINAPI Dev_CreateDS(ID3D11Device1 *this, const void *code, SIZE_T n,
				      ID3D11ClassLinkage *link, ID3D11DomainShader **out)
{
	return Dev_CreateDummyShader(this, code, n, link, (void **)out);
}
static HRESULT WINAPI Dev_CreateCS(ID3D11Device1 *this, const void *code, SIZE_T n,
				      ID3D11ClassLinkage *link, ID3D11ComputeShader **out)
{
	return Dev_CreateDummyShader(this, code, n, link, (void **)out);
}

static HRESULT WINAPI Dev_CreateClassLinkage(ID3D11Device1 *this, ID3D11ClassLinkage **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	d11_ni("CreateClassLinkage");
	(void)this;
	return E_NOTIMPL;
}

static HRESULT WINAPI Dev_CreateBlendState(ID3D11Device1 *this, const D3D11_BLEND_DESC *desc,
					       ID3D11BlendState **out)
{
	Sw11Blend *o;
	if (!out)
		return E_POINTER;
	*out = NULL;
	o = (Sw11Blend *)pin_calloc(sizeof(*o), "blend", PK_BLEND);
	if (!o)
		return E_OUTOFMEMORY;
	o->iface.lpVtbl = &kBlendVtbl;
	o->ref = 1;
	o->dev = (Sw11Device *)this;
	if (desc)
		o->desc = *desc;
	*out = &o->iface;
	return S_OK;
}

static HRESULT WINAPI Dev_CreateDSS(ID3D11Device1 *this, const D3D11_DEPTH_STENCIL_DESC *desc,
				       ID3D11DepthStencilState **out)
{
	Sw11DS *o;
	if (!out)
		return E_POINTER;
	*out = NULL;
	o = (Sw11DS *)pin_calloc(sizeof(*o), "depth-stencil", PK_DS);
	if (!o)
		return E_OUTOFMEMORY;
	o->iface.lpVtbl = &kDSVtbl;
	o->ref = 1;
	o->dev = (Sw11Device *)this;
	if (desc)
		o->desc = *desc;
	/* The rasterizer has no stencil at all, so a state that enables it is a
	 * mask we are guaranteed to ignore. Masking UI into a panel is the usual
	 * reason to reach for one, which would put menu geometry on screen at
	 * full size instead of clipped to its window. Worth knowing before
	 * building anything, so say it once per distinct state. */
	if (desc && desc->StencilEnable) {
		static int said;

		if (said < 8) {
			said++;
			d11_log("NOTE depth-stencil state enables STENCIL, which this "
				"rasterizer does not implement: read mask %02X write mask %02X, "
				"front func %d (pass %d fail %d zfail %d), back func %d. Any draw "
				"masked by this will not be masked here",
				desc->StencilReadMask, desc->StencilWriteMask,
				(int)desc->FrontFace.StencilFunc,
				(int)desc->FrontFace.StencilPassOp,
				(int)desc->FrontFace.StencilFailOp,
				(int)desc->FrontFace.StencilDepthFailOp,
				(int)desc->BackFace.StencilFunc);
		}
	}
	*out = &o->iface;
	return S_OK;
}

static HRESULT WINAPI Dev_CreateRS(ID3D11Device1 *this, const D3D11_RASTERIZER_DESC *desc,
				      ID3D11RasterizerState **out)
{
	Sw11Rast *o;
	if (!out)
		return E_POINTER;
	*out = NULL;
	o = (Sw11Rast *)pin_calloc(sizeof(*o), "raster", PK_RAST);
	if (!o)
		return E_OUTOFMEMORY;
	o->iface.lpVtbl = &kRastVtbl;
	o->ref = 1;
	o->dev = (Sw11Device *)this;
	if (desc)
		o->desc = *desc;
	*out = &o->iface;
	return S_OK;
}

static HRESULT WINAPI Dev_CreateSampler(ID3D11Device1 *this, const D3D11_SAMPLER_DESC *desc,
					   ID3D11SamplerState **out)
{
	Sw11Samp *o;
	if (!out)
		return E_POINTER;
	*out = NULL;
	o = (Sw11Samp *)pin_calloc(sizeof(*o), "sampler", PK_SAMP);
	if (!o)
		return E_OUTOFMEMORY;
	o->iface.lpVtbl = &kSampVtbl;
	o->ref = 1;
	o->dev = (Sw11Device *)this;
	if (desc)
		o->desc = *desc;
	*out = &o->iface;
	return S_OK;
}

static HRESULT WINAPI Dev_CreateQuery(ID3D11Device1 *this, const D3D11_QUERY_DESC *desc,
					  ID3D11Query **out)
{
	Sw11Query *o;
	if (!out)
		return E_POINTER;
	*out = NULL;
	o = (Sw11Query *)pin_calloc(sizeof(*o), "query", PK_QUERY);
	if (!o)
		return E_OUTOFMEMORY;
	o->iface.lpVtbl = &kQueryVtbl;
	o->ref = 1;
	o->dev = (Sw11Device *)this;
	if (desc)
		o->desc = *desc;
	*out = &o->iface;
	return S_OK;
}

static HRESULT WINAPI Dev_CreatePredicate(ID3D11Device1 *this, const D3D11_QUERY_DESC *desc,
					      ID3D11Predicate **out)
{
	return Dev_CreateQuery(this, desc, (ID3D11Query **)out);
}

static HRESULT WINAPI Dev_CreateCounter(ID3D11Device1 *this, const D3D11_COUNTER_DESC *desc,
					   ID3D11Counter **out)
{
	(void)this;
	(void)desc;
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static Sw11Context *ctx_new(Sw11Device *dev, int deferred);

static HRESULT WINAPI Dev_CreateDeferredContext(ID3D11Device1 *this, UINT flags,
						    ID3D11DeviceContext **out)
{
	Sw11Context *c;
	(void)flags;
	if (!out)
		return E_POINTER;
	*out = NULL;
	c = ctx_new((Sw11Device *)this, 1);
	if (!c)
		return E_OUTOFMEMORY;
	*out = (ID3D11DeviceContext *)&c->iface;
	return S_OK;
}

static HRESULT WINAPI Dev_OpenSharedResource(ID3D11Device1 *this, HANDLE h, REFIID riid, void **pp)
{
	(void)this;
	(void)h;
	(void)riid;
	if (pp)
		*pp = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI Dev_CheckFormatSupport(ID3D11Device1 *this, DXGI_FORMAT fmt, UINT *supp)
{
	(void)this;
	if (!supp)
		return E_POINTER;
	*supp = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
		D3D11_FORMAT_SUPPORT_MIP | D3D11_FORMAT_SUPPORT_CPU_LOCKABLE;
	if (!fmt_bc_block(fmt) && !fmt_is_depth(fmt))
		*supp |= D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE |
			 D3D11_FORMAT_SUPPORT_DISPLAY | D3D11_FORMAT_SUPPORT_BUFFER |
			 D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER | D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER;
	if (fmt_is_depth(fmt))
		*supp |= D3D11_FORMAT_SUPPORT_DEPTH_STENCIL;
	return S_OK;
}

static HRESULT WINAPI Dev_CheckMultisampleQualityLevels(ID3D11Device1 *this, DXGI_FORMAT fmt,
							   UINT count, UINT *n)
{
	(void)this;
	(void)fmt;
	if (!n)
		return E_POINTER;
	*n = (count <= 1) ? 1 : 0;
	return S_OK;
}

static void WINAPI Dev_CheckCounterInfo(ID3D11Device1 *this, D3D11_COUNTER_INFO *info)
{
	(void)this;
	if (info)
		memset(info, 0, sizeof(*info));
}

static HRESULT WINAPI Dev_CheckCounter(ID3D11Device1 *this, const D3D11_COUNTER_DESC *desc,
					  D3D11_COUNTER_TYPE *type, UINT *active, LPSTR name,
					  UINT *nlen, LPSTR units, UINT *ulen, LPSTR descstr,
					  UINT *dlen)
{
	(void)this;
	(void)desc;
	(void)type;
	(void)active;
	(void)name;
	(void)nlen;
	(void)units;
	(void)ulen;
	(void)descstr;
	(void)dlen;
	return E_NOTIMPL;
}

static HRESULT WINAPI Dev_CheckFeatureSupport(ID3D11Device1 *this, D3D11_FEATURE feat, void *data,
						UINT size)
{
	(void)this;
	if (!data)
		return E_INVALIDARG;
	memset(data, 0, size);
	if (feat == D3D11_FEATURE_THREADING && size >= sizeof(D3D11_FEATURE_DATA_THREADING)) {
		D3D11_FEATURE_DATA_THREADING *t = (D3D11_FEATURE_DATA_THREADING *)data;
		t->DriverConcurrentCreates = TRUE;
		t->DriverCommandLists = FALSE;
	}
	if (feat == D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS &&
	    size >= sizeof(D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS)) {
		((D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS *)data)->ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x =
			FALSE;
	}
	return S_OK;
}

static HRESULT WINAPI Dev_GetPD(ID3D11Device1 *this, REFGUID g, UINT *n, void *d)
{
	return priv_get(((Sw11Device *)this)->priv, g, n, d);
}
static HRESULT WINAPI Dev_SetPD(ID3D11Device1 *this, REFGUID g, UINT n, const void *d)
{
	return priv_set(&((Sw11Device *)this)->priv, g, n, d);
}
static HRESULT WINAPI Dev_SetPDI(ID3D11Device1 *this, REFGUID g, const IUnknown *o)
{
	return priv_set(&((Sw11Device *)this)->priv, g, sizeof(o), &o);
}
static D3D_FEATURE_LEVEL WINAPI Dev_GetFeatureLevel(ID3D11Device1 *this)
{
	return ((Sw11Device *)this)->level;
}
static UINT WINAPI Dev_GetCreationFlags(ID3D11Device1 *this)
{
	return ((Sw11Device *)this)->flags;
}
static HRESULT WINAPI Dev_GetDeviceRemovedReason(ID3D11Device1 *this)
{
	(void)this;
	return S_OK;
}
static void WINAPI Dev_GetImmediateContext(ID3D11Device1 *this, ID3D11DeviceContext **out)
{
	Sw11Device *d = (Sw11Device *)this;
	if (!out)
		return;
	*out = (ID3D11DeviceContext *)&d->imm->iface;
	d->imm->iface.lpVtbl->AddRef(&d->imm->iface);
}
static HRESULT WINAPI Dev_SetExceptionMode(ID3D11Device1 *this, UINT flags)
{
	(void)this;
	(void)flags;
	return S_OK;
}
static UINT WINAPI Dev_GetExceptionMode(ID3D11Device1 *this)
{
	(void)this;
	return 0;
}
static void WINAPI Dev_GetImmediateContext1(ID3D11Device1 *this, ID3D11DeviceContext1 **out)
{
	Dev_GetImmediateContext(this, (ID3D11DeviceContext **)out);
}
static HRESULT WINAPI Dev_CreateDeferredContext1(ID3D11Device1 *this, UINT flags,
						     ID3D11DeviceContext1 **out)
{
	return Dev_CreateDeferredContext(this, flags, (ID3D11DeviceContext **)out);
}
static HRESULT WINAPI Dev_CreateBlendState1(ID3D11Device1 *this, const D3D11_BLEND_DESC1 *desc,
					       ID3D11BlendState1 **out)
{
	D3D11_BLEND_DESC d0;
	(void)desc;
	memset(&d0, 0, sizeof(d0));
	d0.RenderTarget[0].BlendEnable = TRUE;
	d0.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	d0.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	d0.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	d0.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	d0.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	d0.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	d0.RenderTarget[0].RenderTargetWriteMask = 0xf;
	return Dev_CreateBlendState(this, &d0, (ID3D11BlendState **)out);
}
static HRESULT WINAPI Dev_CreateRasterizerState1(ID3D11Device1 *this,
						    const D3D11_RASTERIZER_DESC1 *desc,
						    ID3D11RasterizerState1 **out)
{
	D3D11_RASTERIZER_DESC d0;
	memset(&d0, 0, sizeof(d0));
	if (desc) {
		d0.FillMode = desc->FillMode;
		d0.CullMode = desc->CullMode;
		d0.FrontCounterClockwise = desc->FrontCounterClockwise;
		d0.DepthClipEnable = desc->DepthClipEnable;
		d0.ScissorEnable = desc->ScissorEnable;
		d0.MultisampleEnable = desc->MultisampleEnable;
	} else
		d0.FillMode = D3D11_FILL_SOLID;
	return Dev_CreateRS(this, &d0, (ID3D11RasterizerState **)out);
}
static HRESULT WINAPI Dev_CreateDeviceContextState(ID3D11Device1 *this, UINT flags,
						      const D3D_FEATURE_LEVEL *levels, UINT n,
						      UINT sdk, REFIID iid, D3D_FEATURE_LEVEL *got,
						      ID3DDeviceContextState **out)
{
	(void)this;
	(void)flags;
	(void)levels;
	(void)n;
	(void)sdk;
	(void)iid;
	if (got)
		*got = D3D_FEATURE_LEVEL_11_0;
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}
static HRESULT WINAPI Dev_OpenSharedResource1(ID3D11Device1 *this, HANDLE h, REFIID riid, void **pp)
{
	return Dev_OpenSharedResource(this, h, riid, pp);
}
static HRESULT WINAPI Dev_OpenSharedResourceByName(ID3D11Device1 *this, LPCWSTR name, DWORD acc,
						      REFIID riid, void **pp)
{
	(void)this;
	(void)name;
	(void)acc;
	(void)riid;
	if (pp)
		*pp = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI DxgiDev_QI(IDXGIDevice1 *this, REFIID riid, void **ppv)
{
	Sw11Device *d = (Sw11Device *)((char *)this - offsetof(Sw11Device, dxgi));
	return Dev_QI(&d->iface, riid, ppv);
}
static ULONG WINAPI DxgiDev_AddRef(IDXGIDevice1 *this)
{
	Sw11Device *d = (Sw11Device *)((char *)this - offsetof(Sw11Device, dxgi));
	return Dev_AddRef(&d->iface);
}
static ULONG WINAPI DxgiDev_Release(IDXGIDevice1 *this)
{
	Sw11Device *d = (Sw11Device *)((char *)this - offsetof(Sw11Device, dxgi));
	return Dev_Release(&d->iface);
}
static HRESULT WINAPI DxgiDev_SetPD(IDXGIDevice1 *this, REFGUID g, UINT n, const void *d)
{
	Sw11Device *dev = (Sw11Device *)((char *)this - offsetof(Sw11Device, dxgi));
	return priv_set(&dev->priv, g, n, d);
}
static HRESULT WINAPI DxgiDev_SetPDI(IDXGIDevice1 *this, REFGUID g, const IUnknown *o)
{
	Sw11Device *d = (Sw11Device *)((char *)this - offsetof(Sw11Device, dxgi));
	return priv_set(&d->priv, g, sizeof(o), &o);
}
static HRESULT WINAPI DxgiDev_GetPD(IDXGIDevice1 *this, REFGUID g, UINT *n, void *d)
{
	Sw11Device *dev = (Sw11Device *)((char *)this - offsetof(Sw11Device, dxgi));
	return priv_get(dev->priv, g, n, d);
}
static HRESULT WINAPI DxgiDev_GetParent(IDXGIDevice1 *this, REFIID riid, void **pp)
{
	Sw11Device *d = (Sw11Device *)((char *)this - offsetof(Sw11Device, dxgi));
	if (!pp)
		return E_POINTER;
	return d->factory->iface.lpVtbl->QueryInterface(&d->factory->iface, riid, pp);
}
static HRESULT WINAPI DxgiDev_GetAdapter(IDXGIDevice1 *this, IDXGIAdapter **out)
{
	Sw11Device *d = (Sw11Device *)((char *)this - offsetof(Sw11Device, dxgi));
	if (!out)
		return E_POINTER;
	*out = (IDXGIAdapter *)&d->adapter->iface;
	d->adapter->iface.lpVtbl->AddRef(&d->adapter->iface);
	return S_OK;
}
static HRESULT WINAPI DxgiDev_CreateSurface(IDXGIDevice1 *this, const DXGI_SURFACE_DESC *desc,
					       UINT n, DXGI_USAGE usage,
					       const DXGI_SHARED_RESOURCE *share, IDXGISurface **out)
{
	(void)this;
	(void)desc;
	(void)n;
	(void)usage;
	(void)share;
	if (out)
		*out = NULL;
	d11_ni("IDXGIDevice.CreateSurface");
	return E_NOTIMPL;
}
static HRESULT WINAPI DxgiDev_QueryResourceResidency(IDXGIDevice1 *this, IUnknown *const *res,
						       DXGI_RESIDENCY *st, UINT n)
{
	UINT i;
	(void)this;
	(void)res;
	if (st)
		for (i = 0; i < n; i++)
			st[i] = DXGI_RESIDENCY_FULLY_RESIDENT;
	return S_OK;
}
static HRESULT WINAPI DxgiDev_SetGPUThreadPriority(IDXGIDevice1 *this, INT p)
{
	(void)this;
	(void)p;
	return S_OK;
}
static HRESULT WINAPI DxgiDev_GetGPUThreadPriority(IDXGIDevice1 *this, INT *p)
{
	if (p)
		*p = 0;
	(void)this;
	return S_OK;
}
static HRESULT WINAPI DxgiDev_SetMaximumFrameLatency(IDXGIDevice1 *this, UINT n)
{
	((Sw11Device *)((char *)this - offsetof(Sw11Device, dxgi)))->latency = n;
	return S_OK;
}
static HRESULT WINAPI DxgiDev_GetMaximumFrameLatency(IDXGIDevice1 *this, UINT *n)
{
	if (n)
		*n = ((Sw11Device *)((char *)this - offsetof(Sw11Device, dxgi)))->latency;
	return S_OK;
}

/* ---------- context + draw ---------- */

static void bind_res(Sw11Res **slot, Sw11Res *r)
{
	if (*slot)
		Res_Release(*slot);
	*slot = r;
	if (r)
		Res_AddRef(r);
}

static void bind_view(Sw11View **slot, Sw11View *v)
{
	if (*slot)
		View_Release(*slot);
	*slot = v;
	if (v)
		InterlockedIncrement(&v->ref);
}

static HRESULT WINAPI Ctx_QI(ID3D11DeviceContext1 *this, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_ID3D11DeviceChild) ||
	    guid_eq(riid, &IID_ID3D11DeviceContext) || guid_eq(riid, &IID_ID3D11DeviceContext1)) {
		*ppv = this;
		InterlockedIncrement(&((Sw11Context *)this)->ref);
		return S_OK;
	}
	return E_NOINTERFACE;
}
static ULONG WINAPI Ctx_AddRef(ID3D11DeviceContext1 *this)
{
	return (ULONG)InterlockedIncrement(&((Sw11Context *)this)->ref);
}
static ULONG WINAPI Ctx_Release(ID3D11DeviceContext1 *this)
{
	Sw11Context *c = (Sw11Context *)this;
	LONG n = InterlockedDecrement(&c->ref);
	if (n == 0)
		obj_free(c);
	return (ULONG)n;
}
static void WINAPI Ctx_GetDevice(ID3D11DeviceContext1 *this, ID3D11Device **out)
{
	Sw11Context *c = (Sw11Context *)this;
	if (c->dev)
		child_get_device(c->dev, out);
	else if (out)
		*out = NULL;
}

static HRESULT WINAPI Ctx_GetPD(ID3D11DeviceContext1 *this, REFGUID g, UINT *n, void *d)
{
	(void)this;
	(void)g;
	(void)n;
	(void)d;
	return DXGI_ERROR_NOT_FOUND;
}
static HRESULT WINAPI Ctx_SetPD(ID3D11DeviceContext1 *this, REFGUID g, UINT n, const void *d)
{
	(void)this;
	(void)g;
	(void)n;
	(void)d;
	return S_OK;
}
static HRESULT WINAPI Ctx_SetPDI(ID3D11DeviceContext1 *this, REFGUID g, const IUnknown *o)
{
	(void)this;
	(void)g;
	(void)o;
	return S_OK;
}

static void WINAPI Ctx_VSSetConstantBuffers(ID3D11DeviceContext1 *this, UINT start, UINT n,
					      ID3D11Buffer *const *bufs)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT i;
	lock_dev(c->dev);
	for (i = 0; i < n && start + i < 14; i++) {
		bind_res(&c->cb_vs[start + i], bufs ? (Sw11Res *)bufs[i] : NULL);
		c->cb_vs_first[start + i] = 0;
		c->cb_vs_n[start + i] = 0;
	}
	unlock_dev(c->dev);
}
static void WINAPI Ctx_PSSetConstantBuffers(ID3D11DeviceContext1 *this, UINT start, UINT n,
					      ID3D11Buffer *const *bufs)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT i;
	lock_dev(c->dev);
	for (i = 0; i < n && start + i < 14; i++) {
		bind_res(&c->cb_ps[start + i], bufs ? (Sw11Res *)bufs[i] : NULL);
		c->cb_ps_first[start + i] = 0;
		c->cb_ps_n[start + i] = 0;
	}
	unlock_dev(c->dev);
}
static void WINAPI Ctx_VSSetConstantBuffers1(ID3D11DeviceContext1 *this, UINT start, UINT n,
					       ID3D11Buffer *const *bufs, const UINT *first,
					       const UINT *count)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT i;
	lock_dev(c->dev);
	for (i = 0; i < n && start + i < 14; i++) {
		bind_res(&c->cb_vs[start + i], bufs ? (Sw11Res *)bufs[i] : NULL);
		c->cb_vs_first[start + i] = first ? first[i] : 0;
		c->cb_vs_n[start + i] = count ? count[i] : 0;
	}
	unlock_dev(c->dev);
}
static void WINAPI Ctx_PSSetConstantBuffers1(ID3D11DeviceContext1 *this, UINT start, UINT n,
					       ID3D11Buffer *const *bufs, const UINT *first,
					       const UINT *count)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT i;
	lock_dev(c->dev);
	for (i = 0; i < n && start + i < 14; i++) {
		bind_res(&c->cb_ps[start + i], bufs ? (Sw11Res *)bufs[i] : NULL);
		c->cb_ps_first[start + i] = first ? first[i] : 0;
		c->cb_ps_n[start + i] = count ? count[i] : 0;
	}
	unlock_dev(c->dev);
}
static void WINAPI Ctx_PSSetShaderResources(ID3D11DeviceContext1 *this, UINT start, UINT n,
					      ID3D11ShaderResourceView *const *views)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT i;
	lock_dev(c->dev);
	for (i = 0; i < n && start + i < 16; i++)
		bind_view(&c->ps_srv[start + i], views ? (Sw11View *)views[i] : NULL);
	unlock_dev(c->dev);
}
static void WINAPI Ctx_PSSetShader(ID3D11DeviceContext1 *this, ID3D11PixelShader *sh,
				     ID3D11ClassInstance *const *ci, UINT nci)
{
	Sw11Context *c = (Sw11Context *)this;
	(void)ci;
	(void)nci;
	lock_dev(c->dev);
	if (c->ps)
		Sh_Release(c->ps);
	c->ps = (Sw11Shader *)sh;
	if (c->ps)
		Sh_AddRef(c->ps);
	unlock_dev(c->dev);
}
static void WINAPI Ctx_VSSetShader(ID3D11DeviceContext1 *this, ID3D11VertexShader *sh,
				     ID3D11ClassInstance *const *ci, UINT nci)
{
	Sw11Context *c = (Sw11Context *)this;
	(void)ci;
	(void)nci;
	lock_dev(c->dev);
	if (c->vs)
		Sh_Release(c->vs);
	c->vs = (Sw11Shader *)sh;
	if (c->vs)
		Sh_AddRef(c->vs);
	unlock_dev(c->dev);
}
static void WINAPI Ctx_PSSetSamplers(ID3D11DeviceContext1 *this, UINT start, UINT n,
				       ID3D11SamplerState *const *s)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT i;
	lock_dev(c->dev);
	for (i = 0; i < n && start + i < 16; i++) {
		if (c->ps_samp[start + i])
			Samp_Release(c->ps_samp[start + i]);
		c->ps_samp[start + i] = s ? (Sw11Samp *)s[i] : NULL;
		if (c->ps_samp[start + i])
			Samp_AddRef(c->ps_samp[start + i]);
	}
	unlock_dev(c->dev);
}
static void WINAPI Ctx_IASetInputLayout(ID3D11DeviceContext1 *this, ID3D11InputLayout *l)
{
	Sw11Context *c = (Sw11Context *)this;
	lock_dev(c->dev);
	if (c->layout)
		Lay_Release(c->layout);
	c->layout = (Sw11Layout *)l;
	if (c->layout)
		Lay_AddRef(c->layout);
	unlock_dev(c->dev);
}
static void WINAPI Ctx_IASetVertexBuffers(ID3D11DeviceContext1 *this, UINT start, UINT n,
					    ID3D11Buffer *const *bufs, const UINT *strides,
					    const UINT *offs)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT i;
	lock_dev(c->dev);
	for (i = 0; i < n && start + i < 16; i++) {
		bind_res(&c->vb[start + i], bufs ? (Sw11Res *)bufs[i] : NULL);
		c->vb_stride[start + i] = strides ? strides[i] : 0;
		c->vb_off[start + i] = offs ? offs[i] : 0;
	}
	unlock_dev(c->dev);
}
static void WINAPI Ctx_IASetIndexBuffer(ID3D11DeviceContext1 *this, ID3D11Buffer *buf,
					    DXGI_FORMAT fmt, UINT off)
{
	Sw11Context *c = (Sw11Context *)this;
	lock_dev(c->dev);
	bind_res(&c->ib, (Sw11Res *)buf);
	c->ib_fmt = fmt;
	c->ib_off = off;
	unlock_dev(c->dev);
}
static void WINAPI Ctx_IASetPrimitiveTopology(ID3D11DeviceContext1 *this,
						D3D11_PRIMITIVE_TOPOLOGY t)
{
	Sw11Context *c = (Sw11Context *)this;
	lock_dev(c->dev);
	c->topo = t;
	unlock_dev(c->dev);
}
static void WINAPI Ctx_OMSetRenderTargets(ID3D11DeviceContext1 *this, UINT n,
					     ID3D11RenderTargetView *const *rt,
					     ID3D11DepthStencilView *ds)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT i;
	lock_dev(c->dev);
	for (i = 0; i < 4; i++)
		bind_view(&c->rtv[i], (i < n && rt) ? (Sw11View *)rt[i] : NULL);
	bind_view(&c->dsv, (Sw11View *)ds);
	unlock_dev(c->dev);
}
static void WINAPI Ctx_OMSetRenderTargetsAndUAVs(ID3D11DeviceContext1 *this, UINT n,
						    ID3D11RenderTargetView *const *rt, ID3D11DepthStencilView *ds,
						    UINT uav_start, UINT nuav,
						    ID3D11UnorderedAccessView *const *uav,
						    const UINT *counts)
{
	(void)uav_start;
	(void)nuav;
	(void)uav;
	(void)counts;
	Ctx_OMSetRenderTargets(this, n, rt, ds);
}
static void WINAPI Ctx_OMSetBlendState(ID3D11DeviceContext1 *this, ID3D11BlendState *b,
					   const FLOAT factor[4], UINT mask)
{
	Sw11Context *c = (Sw11Context *)this;
	lock_dev(c->dev);
	if (c->blend)
		Blend_Release(c->blend);
	c->blend = (Sw11Blend *)b;
	if (c->blend)
		Blend_AddRef(c->blend);
	if (factor)
		memcpy(c->blend_factor, factor, 16);
	c->sample_mask = mask;
	unlock_dev(c->dev);
}
static void WINAPI Ctx_OMSetDepthStencilState(ID3D11DeviceContext1 *this, ID3D11DepthStencilState *ds,
					       UINT ref)
{
	Sw11Context *c = (Sw11Context *)this;
	lock_dev(c->dev);
	if (c->dss)
		Dss_Release(c->dss);
	c->dss = (Sw11DS *)ds;
	if (c->dss)
		Dss_AddRef(c->dss);
	c->stencil_ref = ref;
	unlock_dev(c->dev);
}
static void WINAPI Ctx_RSSetState(ID3D11DeviceContext1 *this, ID3D11RasterizerState *rs)
{
	Sw11Context *c = (Sw11Context *)this;
	lock_dev(c->dev);
	if (c->rs)
		Rast_Release(c->rs);
	c->rs = (Sw11Rast *)rs;
	if (c->rs)
		Rast_AddRef(c->rs);
	unlock_dev(c->dev);
}
static void WINAPI Ctx_RSSetViewports(ID3D11DeviceContext1 *this, UINT n, const D3D11_VIEWPORT *vp)
{
	Sw11Context *c = (Sw11Context *)this;
	lock_dev(c->dev);
	c->nvp = n > 8 ? 8 : n;
	if (vp && c->nvp)
		memcpy(c->vp, vp, c->nvp * sizeof(D3D11_VIEWPORT));
	unlock_dev(c->dev);
}
static void WINAPI Ctx_RSSetScissorRects(ID3D11DeviceContext1 *this, UINT n, const D3D11_RECT *rc)
{
	Sw11Context *c = (Sw11Context *)this;
	lock_dev(c->dev);
	c->nscissor = n > 8 ? 8 : n;
	if (rc && c->nscissor)
		memcpy(c->scissor, rc, c->nscissor * sizeof(D3D11_RECT));
	unlock_dev(c->dev);
}

static void WINAPI Ctx_Draw(ID3D11DeviceContext1 *this, UINT n, UINT start)
{
	ctx_draw((Sw11Context *)this, n, start, 0, 0, 1);
}
static void WINAPI Ctx_DrawIndexed(ID3D11DeviceContext1 *this, UINT n, UINT start, INT base)
{
	/* The largest batch the game has built so far.
	 *
	 * The crash is the game filling an index array past its end, so the size
	 * of the biggest batch it manages without dying is the useful number: it
	 * says how close ordinary play gets to the ceiling, and whether the fatal
	 * scene is an outlier or just the first one over. Logged on each new
	 * maximum rather than per draw, which makes it a short list of the scenes
	 * that pushed hardest instead of tens of thousands of lines. */
	static UINT most;

	if (n > most) {
		most = n;
		d11_log("geometry high-water: %u indices in one DrawIndexed (%u triangles) "
			"after %ld presents",
			n, n / 3, g_present_n);
	}
	ctx_draw((Sw11Context *)this, n, start, base, 1, 1);
}
static void WINAPI Ctx_DrawInstanced(ID3D11DeviceContext1 *this, UINT n, UINT inst, UINT start,
				       UINT start_inst)
{
	(void)start_inst;
	ctx_draw((Sw11Context *)this, n, start, 0, 0, inst ? inst : 1);
}
static void WINAPI Ctx_DrawIndexedInstanced(ID3D11DeviceContext1 *this, UINT n, UINT inst, UINT start,
					      INT base, UINT start_inst)
{
	(void)start_inst;
	ctx_draw((Sw11Context *)this, n, start, base, 1, inst ? inst : 1);
}

static HRESULT WINAPI Ctx_Map(ID3D11DeviceContext1 *this, ID3D11Resource *res, UINT sub,
			       D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *mapped)
{
	Sw11Context *c = (Sw11Context *)this;
	Sw11Res *r = res_from_unk(res);
	(void)sub;
	if (!mapped)
		return E_INVALIDARG;
	memset(mapped, 0, sizeof(*mapped));
	if (r && r->phantom) {
		/* No pixels of its own: hand back the shared scratch at the resource's
		 * real size, so an upload sized to the full description writes somewhere
		 * valid. The write is discarded on Unmap - the texture stays magenta. */
		unsigned char *s = phantom_scratch(r->cpu_size ? r->cpu_size : 4u);

		if (!s)
			return E_OUTOFMEMORY;
		mapped->pData = s;
		mapped->RowPitch = r->kind ? r->row_pitch : r->byte_width;
		mapped->DepthPitch = r->cpu_size;
		return S_OK;
	}
	res_reclaim_cpu(r);
	if (!r || !r->cpu) {
		/* A refused Map leaves pData null. A game that checks the HRESULT
		 * copes; one that does not gets a null base pointer, and any size
		 * it derives by subtracting from it comes out as an address rather
		 * than a length. That is worth knowing about even when nothing
		 * appears to go wrong immediately. */
		static LONG said;

		if (InterlockedIncrement(&said) <= 8)
			d11_log("Map REFUSED for %s #%d (kind=%d bind=%#x cpu=%p): the game "
				"gets a null pointer back",
				r ? (r->kind ? "texture" : "buffer") : "unknown resource",
				r ? r->id : -1, r ? r->kind : -1, r ? (unsigned)r->bind : 0u,
				r ? (void *)r->cpu : NULL);
		return E_INVALIDARG;
	}
	/* The one interaction scaling cannot correct, so it is reported instead of
	 * papered over. The game asked for a surface of virt_w x virt_h and is
	 * about to walk this memory using that width as its stride; the memory is
	 * really the smaller size. Nothing here can fix that - handing back a
	 * padded copy would still be the wrong picture, and the game may write
	 * through it. If this line appears, this title needs D3D11SW_SCALE_RT off
	 * or a narrower rule about which targets are eligible. */
	if (r->virt_w) {
		static LONG said;

		if (InterlockedIncrement(&said) == 1)
			d11_log("WARNING scaled Map: the game is mapping SCALED target #%d, "
				"which it believes is %ux%u and is really %ux%u. It will read "
				"or write this with the wrong stride and the result is NOT "
				"correctable from here - if anything looks wrong, turn "
				"D3D11SW_SCALE_RT off for this game",
				r->id, r->virt_w, r->virt_h, r->width, r->height);
	}
	if (c && c->dev)
		lock_dev(c->dev);
	swrast_flush_if_pending(r->pixels);
	mapped->pData = r->cpu;
	mapped->RowPitch = r->kind ? r->row_pitch : r->byte_width;
	mapped->DepthPitch = r->cpu_size;
	map_track(r->cpu, r->cpu_size, r->id, (int)r->bind, r->kind);
	mapprobe_clear(r); /* legitimate map: drop any guard so it is not miscounted */
	if (mapprobe_on()) {
		int isread = (type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE);
		int big = r->cpu_size >= 0x20000; /* >=128 KB = a page-aligned payload */

		InterlockedIncrement(&g_mc_calls);
		if (isread)
			InterlockedIncrement(&g_mc_read);
		if (big)
			InterlockedIncrement(&g_mc_big);
		if (big && isread)
			InterlockedIncrement(&g_mc_bigread);
		if (!((uintptr_t)r->cpu & 0xFFF))
			InterlockedIncrement(&g_mc_aligned);
	}
	if (r->kind == 1) {
		/* The readback question, answered permanently rather than under a
		 * trace flag that costs more than the rasterising it describes.
		 *
		 * A staging texture with CPU read access, mapped, is the game
		 * reading back what was drawn - and that is the one thing that
		 * would stop the GPU from ever being a disposable cache.
		 *
		 * It used to be broken as well: rasterising writes the render
		 * target's pixels plane, every copy and Map path reads the cpu
		 * plane, and nothing encoded one into the other, so a readback of
		 * rendered output returned a plane that was never written. The
		 * copy into staging now does that encoding, and the count below
		 * says whether the plane about to be read got it. */
		if (r->usage == D3D11_USAGE_STAGING &&
		    (r->cpu_access & D3D11_CPU_ACCESS_READ)) {
			static long seen;

			if (InterlockedIncrement(&seen) <= 8)
				d11_log("READBACK: the game mapped staging texture #%d "
					"%ux%u for READ (type=%u). %u readback copy(s) "
					"this session, %u served with rendered pixels%s",
					r->id, r->width, r->height, type, g_n_readback,
					g_n_readback_served,
					g_n_readback > g_n_readback_served
						? " <<< the shortfall is read as stale bytes"
						: "");
		}
		d11_trace("Map tex #%d %ux%u fmt=%d type=%u flags=%x", r->id, r->width, r->height,
			(int)r->format, type, flags);
	} else if (r->bind & D3D11_BIND_CONSTANT_BUFFER) {
		if (InterlockedIncrement(&g_cb_maps) <= 20)
			d11_trace("Map cb bytes=%u type=%u", r->byte_width, type);
	} else if (InterlockedIncrement(&g_buf_maps) <= 20)
		d11_trace("Map buf bytes=%u bind=%x type=%u", r->byte_width, r->bind, type);
	if (c && c->dev)
		unlock_dev(c->dev);
	return S_OK;
}

static void WINAPI Ctx_Unmap(ID3D11DeviceContext1 *this, ID3D11Resource *res, UINT sub)
{
	Sw11Context *c = (Sw11Context *)this;
	Sw11Res *r = res_from_unk(res);
	(void)sub;
	if (c && c->dev)
		lock_dev(c->dev);
	if (r && r->kind == 1 && r->pixels)
		res_decode_pixels_written(r);
	if (c && c->dev)
		unlock_dev(c->dev);
	/* Guard the payload now it is unmapped: any access from here until the next
	 * Map is either our raster (expected) or the game holding a raw pointer. */
	mapprobe_arm(r);
}

static void WINAPI Ctx_UpdateSubresource(ID3D11DeviceContext1 *this, ID3D11Resource *res, UINT sub,
					     const D3D11_BOX *box, const void *src, UINT pitch,
					     UINT depth_pitch)
{
	Sw11Context *c = (Sw11Context *)this;
	Sw11Res *r = res_from_unk(res);
	(void)sub;
	(void)depth_pitch;
	if (!r || !src)
		return;
	res_reclaim_cpu(r);
	faucet_make_resident(r); /* an evicted texture needs a plane to upload into */
	if (c && c->dev)
		lock_dev(c->dev);
	if (r->kind == 0) {
		UINT off = box ? box->left : 0;
		UINT n = box ? (box->right - box->left) : r->byte_width;
		if (off < r->byte_width) {
			if (n > r->byte_width - off)
				n = r->byte_width - off;
			memcpy(r->cpu + off, src, n);
		}
		if (c && c->dev)
			unlock_dev(c->dev);
		return;
	}
	if (r->cpu) {
		UINT x0 = 0, y0 = 0, tw, th, bpp, y, bc;
		UINT srcp = pitch ? pitch : r->row_pitch;

		/* Storage deliberately smaller than the texture the game believes
		 * it created. The incoming image is in the game's coordinates, so
		 * it has to be shrunk on the way in; copying it straight would
		 * write a full-size image into a fraction of the memory. */
		if (r->virt_w > r->width && r->virt_h > r->height && !fmt_bc_block(r->format) &&
		    fmt_stride(r->format) == 4) {
			UINT bx = box ? box->left : 0, by = box ? box->top : 0;
			UINT bw = box ? (box->right - box->left) : r->virt_w;
			UINT bh = box ? (box->bottom - box->top) : r->virt_h;
			/* Where this update lands once the texture is stored
			 * smaller. Rounding outwards on the far edge so a run of
			 * adjacent sub-rect updates tiles the destination with no
			 * unwritten seam between them. */
			UINT dx0 = (UINT)((unsigned long long)bx * r->width / r->virt_w);
			UINT dy0 = (UINT)((unsigned long long)by * r->height / r->virt_h);
			UINT dx1 = (UINT)(((unsigned long long)(bx + bw) * r->width +
					   r->virt_w - 1) /
					  r->virt_w);
			UINT dy1 = (UINT)(((unsigned long long)(by + bh) * r->height +
					   r->virt_h - 1) /
					  r->virt_h);

			if (dx1 > r->width)
				dx1 = r->width;
			if (dy1 > r->height)
				dy1 = r->height;
			if (dx1 > dx0 && dy1 > dy0)
				area_downscale_rgba(r->cpu + (size_t)dy0 * r->row_pitch +
							    (size_t)dx0 * 4,
						    r->row_pitch, dx1 - dx0, dy1 - dy0,
						    (const unsigned char *)src, srcp, bw, bh);
			if (r->pixels)
				res_decode_pixels_written(r);
			if (c && c->dev)
				unlock_dev(c->dev);
			return;
		}
		bc = fmt_bc_block(r->format);
		if (box) {
			x0 = box->left;
			y0 = box->top;
			tw = box->right > box->left ? box->right - box->left : 0;
			th = box->bottom > box->top ? box->bottom - box->top : 0;
		} else if (bc) {
			x0 = y0 = 0;
			tw = r->width;
			th = (r->height + 3) / 4;
		} else {
			x0 = y0 = 0;
			tw = r->width;
			th = r->height;
		}
		d11_trace("UpdateSubresource %ux%u fmt=%d box=%u,%u %ux%u", r->width, r->height,
			(int)r->format, x0, y0, tw, th);
		if (bc) {
			UINT rows = box ? (th + 3) / 4 : th;
			UINT rowb = r->row_pitch;
			for (y = 0; y < rows; y++) {
				UINT n = rowb < srcp ? rowb : srcp;
				size_t dest = (size_t)(y0 / 4 + y) * r->row_pitch;
				if (dest >= r->cpu_size)
					break;
				if (dest + n > r->cpu_size)
					n = (UINT)(r->cpu_size - dest);
				memcpy(r->cpu + dest, (const char *)src + (size_t)y * srcp, n);
			}
		} else {
			bpp = fmt_stride(r->format);
			if (!bpp)
				bpp = 1;
			if (x0 < r->width && y0 < r->height) {
				if (tw > r->width - x0)
					tw = r->width - x0;
				if (th > r->height - y0)
					th = r->height - y0;
				for (y = 0; y < th; y++) {
					UINT n = tw * bpp;
					size_t dest = (size_t)(y0 + y) * r->row_pitch + (size_t)x0 * bpp;
					if (dest >= r->cpu_size)
						break;
					if (dest + n > r->cpu_size)
						n = (UINT)(r->cpu_size - dest);
					memcpy(r->cpu + dest, (const char *)src + (size_t)y * srcp, n);
				}
			}
		}
		if (r->pixels)
			res_decode_pixels_written(r);
		faucet_bank_store(r); /* content changed: refresh the bank */
	}
	if (c && c->dev)
		unlock_dev(c->dev);
}

static void WINAPI Ctx_UpdateSubresource1(ID3D11DeviceContext1 *this, ID3D11Resource *res, UINT sub,
					      const D3D11_BOX *box, const void *src, UINT pitch,
					      UINT depth_pitch, UINT copy_flags)
{
	(void)copy_flags;
	Ctx_UpdateSubresource(this, res, sub, box, src, pitch, depth_pitch);
}

static void WINAPI Ctx_CopyResource(ID3D11DeviceContext1 *this, ID3D11Resource *dst,
				      ID3D11Resource *src)
{
	Sw11Res *d = res_from_unk(dst), *s = res_from_unk(src);
	(void)this;
	if (!d || !s)
		return;
	swrast_flush_if_pending(s->pixels);
	if (d->is_bb)
		g_bb_writes += d->width * d->height;
	/* The other half of the readback question: copying INTO a staging
	 * surface is how the data gets there before it is mapped. Naming the
	 * source says whether it is the backbuffer or a render target, which is
	 * what decides whether the GPU could ever be non-authoritative here. */
	if (d->usage == D3D11_USAGE_STAGING && (d->cpu_access & D3D11_CPU_ACCESS_READ) &&
	    (s->is_bb || (s->bind & D3D11_BIND_RENDER_TARGET))) {
		static long seen;
		/* At mode 3 the rasteriser did not draw this frame, so its plane is
		 * not the rendered image and copying it would hand back a stale or
		 * empty one. The adapter has the pixels; a staging copy is the only
		 * way to reach them, and it is affordable because the game asks for
		 * this a few times a session at scene transitions. */
		int ok = (gpu_mode() >= 3 && s->is_bb)
				 ? gpu_readback((uint32_t *)d->cpu, d->row_pitch,
						(int)d->width, (int)d->height)
				 : res_readback_pixels(d, s);

		if (InterlockedIncrement(&seen) <= 8)
			d11_log("READBACK: CopyResource into staging #%d %ux%u from #%d "
				"%ux%u (backbuffer=%d, render target=%d): %s",
				d->id, d->width, d->height, s->id, s->width, s->height,
				s->is_bb, (s->bind & D3D11_BIND_RENDER_TARGET) ? 1 : 0,
				ok ? "rendered pixels encoded into the staging plane"
				   : "NOT SERVED - the rendered plane could not be "
				     "encoded into this destination, so the game will "
				     "read whatever the cpu plane already held");
		g_n_readback++;
		if (ok) {
			/* The plane the map will read is now correct, and letting the
			 * ordinary copy run would put the stale one straight back. */
			g_n_readback_served++;
			return;
		}
	}
	if (d->kind == 1 && s->kind == 1) {
		d11_trace("CopyResource #%d %ux%u fmt=%d bb=%d <- #%d %ux%u fmt=%d", d->id, d->width,
			d->height, (int)d->format, d->is_bb, s->id, s->width, s->height,
			(int)s->format);
		/* A whole-resource copy between surfaces the game believes are the
		 * same size and that scaling has made different. Stretch, because
		 * the sizes disagreeing is our doing and not the game's. */
		if ((d->virt_w || s->virt_w) && (d->width != s->width || d->height != s->height)) {
			static LONG said;

			if (InterlockedIncrement(&said) == 1)
				d11_log("scaled copy: CopyResource #%d %ux%u <- #%d %ux%u is "
					"being STRETCHED because render-target scaling made "
					"them different sizes. Correct for a full-surface "
					"copy; if the image comes out soft or offset this is "
					"the first place to look",
					d->id, d->width, d->height, s->id, s->width, s->height);
			res_stretch_tex(d, s);
			return;
		}
		res_copy_tex_rect(d, 0, 0, s, 0, 0, s->width, s->height);
		return;
	}
	if (d->cpu && s->cpu && d->cpu_size && s->cpu_size) {
		UINT n = d->cpu_size < s->cpu_size ? d->cpu_size : s->cpu_size;
		memcpy(d->cpu, s->cpu, n);
	}
	if (d->pixels && s->pixels && d->width == s->width && d->height == s->height) {
		memcpy(d->pixels, s->pixels, (size_t)d->width * d->height * 4);
		/* The rasteriser-side planes were copied directly, so whatever
		 * the stored bytes now are, they already match. */
		d->cpu_dirty = 0;
	} else if (d->pixels) {
		res_after_cpu_copy(d, s);
	}
}

static void WINAPI Ctx_CopySubresourceRegion(ID3D11DeviceContext1 *this, ID3D11Resource *dst,
						 UINT dst_sub, UINT x, UINT y, UINT z,
						 ID3D11Resource *src, UINT src_sub,
						 const D3D11_BOX *box)
{
	Sw11Res *d = res_from_unk(dst), *s = res_from_unk(src);
	UINT sx = 0, sy = 0, tw, th;
	(void)dst_sub;
	(void)z;
	(void)src_sub;
	if (!d || !s)
		return;
	swrast_flush_if_pending(s->pixels);
	if (d->kind != 1 || s->kind != 1) {
		Ctx_CopyResource(this, dst, src);
		return;
	}
	/* A copy into downscaled storage would land at the wrong coordinates,
	 * and this game fills its atlases through UpdateSubresource instead. Say
	 * so rather than corrupting quietly if some title does copy into one. */
	if (d->virt_w > d->width) {
		static LONG said;

		if (InterlockedIncrement(&said) <= 4)
			d11_log("WARNING CopySubresourceRegion into downscaled #%d (%ux%u stored, "
				"game sees %ux%u) at %u,%u from #%d %ux%u - not scaled",
				d->id, d->width, d->height, d->virt_w, d->virt_h, x, y, s->id,
				s->width, s->height);
	}
	if (box) {
		sx = box->left;
		sy = box->top;
		tw = box->right > box->left ? box->right - box->left : 0;
		th = box->bottom > box->top ? box->bottom - box->top : 0;
	} else {
		tw = s->width;
		th = s->height;
	}
	/* Every coordinate here came from the game and is therefore in the size
	 * it was told each surface is. Source and destination scale
	 * independently, so they convert independently.
	 *
	 * This is the path most likely to be subtly wrong, because a region copy
	 * carries an intent - "this exact rectangle of pixels" - that a scale
	 * factor cannot always honour: a 1-pixel border stays 1 pixel wide in the
	 * game's arithmetic and becomes half a pixel in ours. Hence the log
	 * rather than silence. */
	if (s->virt_w || d->virt_w) {
		static LONG said;
		float sfx = rt_scale_x(s), sfy = rt_scale_y(s);
		float dfx = rt_scale_x(d), dfy = rt_scale_y(d);

		if (InterlockedIncrement(&said) == 1)
			d11_log("scaled copy: CopySubresourceRegion touches a scaled target "
				"(dst #%d scale %.3f, src #%d scale %.3f). Region coordinates "
				"are being converted. A region copy asks for an exact "
				"rectangle, which scaling cannot always honour - suspect this "
				"if something is a pixel out or a thin edge disappears",
				d->id, (double)dfx, s->id, (double)sfx);
		sx = (UINT)((float)sx * sfx);
		sy = (UINT)((float)sy * sfy);
		tw = (UINT)((float)tw * sfx + 0.5f);
		th = (UINT)((float)th * sfy + 0.5f);
		x = (UINT)((float)x * dfx);
		y = (UINT)((float)y * dfy);
	}
	d11_trace("CopySubresource #%d bb=%d %u,%u <- #%d %u,%u %ux%u", d->id, d->is_bb, x, y, s->id,
		sx, sy, tw, th);
	if (d->is_bb)
		g_bb_writes += tw * th;
	res_copy_tex_rect(d, x, y, s, sx, sy, tw, th);
}

static void WINAPI Ctx_CopySubresourceRegion1(ID3D11DeviceContext1 *this, ID3D11Resource *dst,
						  UINT dst_sub, UINT x, UINT y, UINT z,
						  ID3D11Resource *src, UINT src_sub,
						  const D3D11_BOX *box, UINT copy_flags)
{
	(void)copy_flags;
	Ctx_CopySubresourceRegion(this, dst, dst_sub, x, y, z, src, src_sub, box);
}

static void WINAPI Ctx_ClearRenderTargetView(ID3D11DeviceContext1 *this, ID3D11RenderTargetView *rtv,
					       const FLOAT col[4])
{
	Sw11Context *c = (Sw11Context *)this;
	Sw11View *v = (Sw11View *)rtv;
	uint32_t p;
	UINT i, n;
	if (!v || !v->res)
		return;
	/* Remembered rather than issued here: the adapter's frame is opened by the
	 * first draw, which is after this, and it must start from the same
	 * background the rasteriser started from. */
	if (col && v->res->is_bb)
		g_gpu_clear_argb = ((uint32_t)(col[3] * 255.0f + 0.5f) << 24) |
				   ((uint32_t)(col[0] * 255.0f + 0.5f) << 16) |
				   ((uint32_t)(col[1] * 255.0f + 0.5f) << 8) |
				   (uint32_t)(col[2] * 255.0f + 0.5f);
	if (c && c->dev)
		lock_dev(c->dev);
	d11_trace("ClearRTV #%d %ux%u bb=%d rgba=%.2f,%.2f,%.2f,%.2f", v->res->id, v->res->width,
		v->res->height, v->res->is_bb, col ? col[0] : 0.0f, col ? col[1] : 0.0f,
		col ? col[2] : 0.0f, col ? col[3] : 1.0f);
	if (!res_ensure_pixels(v->res) || !v->res->pixels) {
		if (c && c->dev)
			unlock_dev(c->dev);
		return;
	}
	swrast_flush_if_pending(v->res->pixels);
	p = pack_argb(col ? col[0] : 0, col ? col[1] : 0, col ? col[2] : 0, col ? col[3] : 1);
	n = v->res->width * v->res->height;
	for (i = 0; i < n; i++)
		v->res->pixels[i] = p;
	if (c && c->dev)
		unlock_dev(c->dev);
}

static void WINAPI Ctx_ClearDepthStencilView(ID3D11DeviceContext1 *this, ID3D11DepthStencilView *dsv,
					       UINT flags, FLOAT depth, UINT8 stencil)
{
	Sw11View *v = (Sw11View *)dsv;
	UINT i, n;
	(void)this;
	(void)stencil;
	if (!v || !v->res || !(flags & D3D11_CLEAR_DEPTH))
		return;
	res_ensure_pixels(v->res);
	if (!v->res->depth)
		return;
	n = v->res->width * v->res->height;
	for (i = 0; i < n; i++)
		v->res->depth[i] = depth;
}

static void WINAPI Ctx_ClearUAVUint(ID3D11DeviceContext1 *this, ID3D11UnorderedAccessView *u,
				      const UINT v[4])
{
	(void)this;
	(void)u;
	(void)v;
}
static void WINAPI Ctx_ClearUAVFloat(ID3D11DeviceContext1 *this, ID3D11UnorderedAccessView *u,
					const FLOAT v[4])
{
	(void)this;
	(void)u;
	(void)v;
}
static void WINAPI Ctx_GenerateMips(ID3D11DeviceContext1 *this, ID3D11ShaderResourceView *v)
{
	(void)this;
	(void)v;
}
static void WINAPI Ctx_SetResourceMinLOD(ID3D11DeviceContext1 *this, ID3D11Resource *r, FLOAT lod)
{
	(void)this;
	(void)r;
	(void)lod;
}
static FLOAT WINAPI Ctx_GetResourceMinLOD(ID3D11DeviceContext1 *this, ID3D11Resource *r)
{
	(void)this;
	(void)r;
	return 0;
}
static void WINAPI Ctx_ResolveSubresource(ID3D11DeviceContext1 *this, ID3D11Resource *dst, UINT di,
					     ID3D11Resource *src, UINT si, DXGI_FORMAT fmt)
{
	(void)di;
	(void)si;
	(void)fmt;
	Ctx_CopyResource(this, dst, src);
}
static void WINAPI Ctx_ExecuteCommandList(ID3D11DeviceContext1 *this, ID3D11CommandList *list,
					      BOOL restore)
{
	(void)this;
	(void)list;
	(void)restore;
}
static void WINAPI Ctx_ClearState(ID3D11DeviceContext1 *this)
{
	Sw11Context *c = (Sw11Context *)this;
	int i;
	lock_dev(c->dev);
	for (i = 0; i < 16; i++) {
		bind_res(&c->vb[i], NULL);
		bind_view(&c->ps_srv[i], NULL);
		if (c->ps_samp[i]) {
			Samp_Release(c->ps_samp[i]);
			c->ps_samp[i] = NULL;
		}
	}
	for (i = 0; i < 14; i++) {
		bind_res(&c->cb_vs[i], NULL);
		bind_res(&c->cb_ps[i], NULL);
	}
	bind_res(&c->ib, NULL);
	if (c->vs)
		Sh_Release(c->vs);
	c->vs = NULL;
	if (c->ps)
		Sh_Release(c->ps);
	c->ps = NULL;
	if (c->layout)
		Lay_Release(c->layout);
	c->layout = NULL;
	for (i = 0; i < 4; i++)
		bind_view(&c->rtv[i], NULL);
	bind_view(&c->dsv, NULL);
	c->topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	c->nvp = 0;
	unlock_dev(c->dev);
}
static void WINAPI Ctx_Flush(ID3D11DeviceContext1 *this)
{
	(void)this;
	swrast_flush();
}
static D3D11_DEVICE_CONTEXT_TYPE WINAPI Ctx_GetType(ID3D11DeviceContext1 *this)
{
	return ((Sw11Context *)this)->deferred ? D3D11_DEVICE_CONTEXT_DEFERRED
						 : D3D11_DEVICE_CONTEXT_IMMEDIATE;
}
static UINT WINAPI Ctx_GetContextFlags(ID3D11DeviceContext1 *this)
{
	(void)this;
	return 0;
}
static HRESULT WINAPI Ctx_FinishCommandList(ID3D11DeviceContext1 *this, BOOL restore,
					       ID3D11CommandList **out)
{
	(void)this;
	(void)restore;
	if (out)
		*out = NULL;
	return DXGI_ERROR_INVALID_CALL;
}

static void WINAPI Ctx_VSGetConstantBuffers(ID3D11DeviceContext1 *this, UINT start, UINT n,
						ID3D11Buffer **out)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT i;
	if (!out)
		return;
	lock_dev(c->dev);
	for (i = 0; i < n; i++) {
		Sw11Res *r = (start + i < 14) ? c->cb_vs[start + i] : NULL;
		out[i] = r ? &r->iface.buf : NULL;
		if (r)
			Res_AddRef(r);
	}
	unlock_dev(c->dev);
}
static void WINAPI Ctx_PSGetConstantBuffers(ID3D11DeviceContext1 *this, UINT start, UINT n,
						ID3D11Buffer **out)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT i;
	if (!out)
		return;
	lock_dev(c->dev);
	for (i = 0; i < n; i++) {
		Sw11Res *r = (start + i < 14) ? c->cb_ps[start + i] : NULL;
		out[i] = r ? &r->iface.buf : NULL;
		if (r)
			Res_AddRef(r);
	}
	unlock_dev(c->dev);
}
static void WINAPI Ctx_PSGetShaderResources(ID3D11DeviceContext1 *this, UINT start, UINT n,
					       ID3D11ShaderResourceView **out)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT i;
	if (!out)
		return;
	lock_dev(c->dev);
	for (i = 0; i < n; i++) {
		Sw11View *v = (start + i < 16) ? c->ps_srv[start + i] : NULL;
		out[i] = v ? &v->iface.srv : NULL;
		if (v)
			InterlockedIncrement(&v->ref);
	}
	unlock_dev(c->dev);
}
static void WINAPI Ctx_PSGetShader(ID3D11DeviceContext1 *this, ID3D11PixelShader **sh,
				      ID3D11ClassInstance **ci, UINT *nci)
{
	Sw11Context *c = (Sw11Context *)this;
	if (nci)
		*nci = 0;
	if (ci)
		*ci = NULL;
	lock_dev(c->dev);
	if (sh) {
		*sh = c->ps ? (ID3D11PixelShader *)&c->ps->iface : NULL;
		if (c->ps)
			Sh_AddRef(c->ps);
	}
	unlock_dev(c->dev);
}
static void WINAPI Ctx_VSGetShader(ID3D11DeviceContext1 *this, ID3D11VertexShader **sh,
				      ID3D11ClassInstance **ci, UINT *nci)
{
	Sw11Context *c = (Sw11Context *)this;
	if (nci)
		*nci = 0;
	if (ci)
		*ci = NULL;
	lock_dev(c->dev);
	if (sh) {
		*sh = c->vs ? &c->vs->iface : NULL;
		if (c->vs)
			Sh_AddRef(c->vs);
	}
	unlock_dev(c->dev);
}
static void WINAPI Ctx_PSGetSamplers(ID3D11DeviceContext1 *this, UINT start, UINT n,
					ID3D11SamplerState **out)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT i;
	if (!out)
		return;
	lock_dev(c->dev);
	for (i = 0; i < n; i++) {
		Sw11Samp *s = (start + i < 16) ? c->ps_samp[start + i] : NULL;
		out[i] = s ? &s->iface : NULL;
		if (s)
			Samp_AddRef(s);
	}
	unlock_dev(c->dev);
}
static void WINAPI Ctx_IAGetInputLayout(ID3D11DeviceContext1 *this, ID3D11InputLayout **out)
{
	Sw11Context *c = (Sw11Context *)this;
	if (!out)
		return;
	lock_dev(c->dev);
	*out = c->layout ? &c->layout->iface : NULL;
	if (c->layout)
		Lay_AddRef(c->layout);
	unlock_dev(c->dev);
}
static void WINAPI Ctx_IAGetVertexBuffers(ID3D11DeviceContext1 *this, UINT start, UINT n,
					    ID3D11Buffer **bufs, UINT *strides, UINT *offs)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT i;
	lock_dev(c->dev);
	for (i = 0; i < n; i++) {
		UINT s = start + i;
		Sw11Res *r = (s < 16) ? c->vb[s] : NULL;
		if (bufs) {
			bufs[i] = r ? &r->iface.buf : NULL;
			if (r)
				Res_AddRef(r);
		}
		if (strides)
			strides[i] = (s < 16) ? c->vb_stride[s] : 0;
		if (offs)
			offs[i] = (s < 16) ? c->vb_off[s] : 0;
	}
	unlock_dev(c->dev);
}
static void WINAPI Ctx_IAGetIndexBuffer(ID3D11DeviceContext1 *this, ID3D11Buffer **buf,
					     DXGI_FORMAT *fmt, UINT *off)
{
	Sw11Context *c = (Sw11Context *)this;
	lock_dev(c->dev);
	if (buf) {
		*buf = c->ib ? &c->ib->iface.buf : NULL;
		if (c->ib)
			Res_AddRef(c->ib);
	}
	if (fmt)
		*fmt = c->ib_fmt;
	if (off)
		*off = c->ib_off;
	unlock_dev(c->dev);
}
static void WINAPI Ctx_IAGetPrimitiveTopology(ID3D11DeviceContext1 *this,
						 D3D11_PRIMITIVE_TOPOLOGY *t)
{
	if (t)
		*t = ((Sw11Context *)this)->topo;
}
static void WINAPI Ctx_OMGetRenderTargets(ID3D11DeviceContext1 *this, UINT n,
					      ID3D11RenderTargetView **rt, ID3D11DepthStencilView **ds)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT i;
	lock_dev(c->dev);
	if (rt) {
		for (i = 0; i < n; i++) {
			Sw11View *v = (i < 4) ? c->rtv[i] : NULL;
			rt[i] = v ? &v->iface.rtv : NULL;
			if (v)
				InterlockedIncrement(&v->ref);
		}
	}
	if (ds) {
		*ds = c->dsv ? &c->dsv->iface.dsv : NULL;
		if (c->dsv)
			InterlockedIncrement(&c->dsv->ref);
	}
	unlock_dev(c->dev);
}
static void WINAPI Ctx_OMGetRenderTargetsAndUAVs(ID3D11DeviceContext1 *this, UINT nrt,
						     ID3D11RenderTargetView **rt, ID3D11DepthStencilView **ds,
						     UINT uav_start, UINT nuav,
						     ID3D11UnorderedAccessView **uav)
{
	UINT i;
	Ctx_OMGetRenderTargets(this, nrt, rt, ds);
	(void)uav_start;
	if (uav)
		for (i = 0; i < nuav; i++)
			uav[i] = NULL;
}

static void WINAPI Ctx_OMGetBlendState(ID3D11DeviceContext1 *this, ID3D11BlendState **b,
					    FLOAT factor[4], UINT *mask)
{
	Sw11Context *c = (Sw11Context *)this;
	lock_dev(c->dev);
	if (b) {
		*b = c->blend ? &c->blend->iface : NULL;
		if (c->blend)
			Blend_AddRef(c->blend);
	}
	if (factor)
		memcpy(factor, c->blend_factor, 16);
	if (mask)
		*mask = c->sample_mask;
	unlock_dev(c->dev);
}
static void WINAPI Ctx_OMGetDepthStencilState(ID3D11DeviceContext1 *this,
						 ID3D11DepthStencilState **ds, UINT *ref)
{
	Sw11Context *c = (Sw11Context *)this;
	lock_dev(c->dev);
	if (ds) {
		*ds = c->dss ? &c->dss->iface : NULL;
		if (c->dss)
			Dss_AddRef(c->dss);
	}
	if (ref)
		*ref = c->stencil_ref;
	unlock_dev(c->dev);
}
static void WINAPI Ctx_RSGetState(ID3D11DeviceContext1 *this, ID3D11RasterizerState **rs)
{
	Sw11Context *c = (Sw11Context *)this;
	if (!rs)
		return;
	lock_dev(c->dev);
	*rs = c->rs ? &c->rs->iface : NULL;
	if (c->rs)
		Rast_AddRef(c->rs);
	unlock_dev(c->dev);
}
static void WINAPI Ctx_RSGetViewports(ID3D11DeviceContext1 *this, UINT *n, D3D11_VIEWPORT *vp)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT want;
	if (!n)
		return;
	lock_dev(c->dev);
	if (!vp) {
		*n = c->nvp;
		unlock_dev(c->dev);
		return;
	}
	want = *n;
	if (want > c->nvp)
		want = c->nvp;
	if (want)
		memcpy(vp, c->vp, want * sizeof(D3D11_VIEWPORT));
	*n = c->nvp;
	unlock_dev(c->dev);
}
static void WINAPI Ctx_RSGetScissorRects(ID3D11DeviceContext1 *this, UINT *n, D3D11_RECT *rc)
{
	Sw11Context *c = (Sw11Context *)this;
	UINT want;
	if (!n)
		return;
	lock_dev(c->dev);
	if (!rc) {
		*n = c->nscissor;
		unlock_dev(c->dev);
		return;
	}
	want = *n;
	if (want > c->nscissor)
		want = c->nscissor;
	if (want)
		memcpy(rc, c->scissor, want * sizeof(D3D11_RECT));
	*n = c->nscissor;
	unlock_dev(c->dev);
}
static void WINAPI Ctx_GSGetShader(ID3D11DeviceContext1 *this, ID3D11GeometryShader **sh,
				      ID3D11ClassInstance **ci, UINT *nci)
{
	if (sh)
		*sh = NULL;
	if (nci)
		*nci = 0;
	(void)this;
	(void)ci;
}
static void WINAPI Ctx_GetPredication(ID3D11DeviceContext1 *this, ID3D11Predicate **p, BOOL *v)
{
	if (p)
		*p = NULL;
	if (v)
		*v = FALSE;
	(void)this;
}

static void WINAPI Ctx_Begin(ID3D11DeviceContext1 *this, ID3D11Asynchronous *a)
{
	(void)this;
	(void)a;
}
static void WINAPI Ctx_End(ID3D11DeviceContext1 *this, ID3D11Asynchronous *a)
{
	(void)this;
	(void)a;
}
static HRESULT WINAPI Ctx_GetData(ID3D11DeviceContext1 *this, ID3D11Asynchronous *a, void *data,
				     UINT size, UINT flags)
{
	(void)this;
	(void)a;
	(void)flags;
	if (data && size)
		memset(data, 0, size);
	return S_OK;
}
static void WINAPI Ctx_SetPredication(ID3D11DeviceContext1 *this, ID3D11Predicate *p, BOOL v)
{
	(void)this;
	(void)p;
	(void)v;
}

static void WINAPI Ctx_GSSetShader(ID3D11DeviceContext1 *this, ID3D11GeometryShader *s,
				     ID3D11ClassInstance *const *ci, UINT n)
{
	(void)this;
	(void)s;
	(void)ci;
	(void)n;
}
static void WINAPI Ctx_GSSetConstantBuffers(ID3D11DeviceContext1 *this, UINT a, UINT b,
					      ID3D11Buffer *const *c)
{
	(void)this;
	(void)a;
	(void)b;
	(void)c;
}
static void WINAPI Ctx_SOSetTargets(ID3D11DeviceContext1 *this, UINT n, ID3D11Buffer *const *b,
				      const UINT *o)
{
	(void)this;
	(void)n;
	(void)b;
	(void)o;
}
static void WINAPI Ctx_DrawAuto(ID3D11DeviceContext1 *this)
{
	(void)this;
}
static void WINAPI Ctx_Dispatch(ID3D11DeviceContext1 *this, UINT x, UINT y, UINT z)
{
	(void)this;
	(void)x;
	(void)y;
	(void)z;
}

static void load_fmt(float out[4], const unsigned char *p, DXGI_FORMAT fmt)
{
	out[0] = 0;
	out[1] = 0;
	out[2] = 0;
	out[3] = 1;
	if (!p)
		return;
	switch (fmt) {
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
		memcpy(out, p, 16);
		break;
	case DXGI_FORMAT_R32G32B32_FLOAT:
		memcpy(out, p, 12);
		break;
	case DXGI_FORMAT_R32G32_FLOAT:
		memcpy(out, p, 8);
		break;
	case DXGI_FORMAT_R32_FLOAT:
		memcpy(out, p, 4);
		break;
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		out[0] = p[0] / 255.0f;
		out[1] = p[1] / 255.0f;
		out[2] = p[2] / 255.0f;
		out[3] = p[3] / 255.0f;
		break;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		out[0] = p[2] / 255.0f;
		out[1] = p[1] / 255.0f;
		out[2] = p[0] / 255.0f;
		out[3] = p[3] / 255.0f;
		break;
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	default:
		if (fmt_stride(fmt) >= 4 && fmt != DXGI_FORMAT_R8G8B8A8_UNORM)
			memcpy(out, p, fmt_stride(fmt) > 16 ? 16 : fmt_stride(fmt));
		break;
	}
}

static uint32_t color_from_vec(const float c[4])
{
	return pack_argb(c[0], c[1], c[2], c[3]);
}

static void ctx_draw_inner(Sw11Context *c, UINT count, UINT start, INT base, int indexed,
			   UINT inst);

static void ctx_draw(Sw11Context *c, UINT count, UINT start, INT base, int indexed, UINT inst)
{
	LARGE_INTEGER a, b;
	LONGLONG t0 = (LONGLONG)__builtin_readcyclecounter();
	double r0 = swrast_prof_peek_raster();
	QueryPerformanceCounter(&a);
	ctx_draw_inner(c, count, start, base, indexed, inst);
	QueryPerformanceCounter(&b);
	g_zone[ZONE_DRAW] += b.QuadPart - a.QuadPart;
	g_tsc_draw += (LONGLONG)__builtin_readcyclecounter() - t0;
	g_raster_nested_ms += swrast_prof_peek_raster() - r0;
}

static void ctx_draw_inner(Sw11Context *c, UINT count, UINT start, INT base, int indexed,
			   UINT inst)
{
	SwRast rast;
	SwState st;
	SwTex tex;
	Sw11Res *rt;
	Sw11Res *src_res = NULL;
	int a8_font = 0;
	SwTri *batch;
	UINT prims, i, ii;
	D3D11_VIEWPORT vp;
	const void *cb[14];
	unsigned cb_bytes[14];
	int pos_reg = -1, uv_reg = -1, col_reg = -1;
	float first_x = 0, first_y = 0;
	float first_pos[4] = { 0, 0, 0, 0 };
	int vs_ran = 0;
	unsigned dbg_loaded = 0, first_loaded = 0;
	float umin = 1.0e30f, umax = -1.0e30f, vmin = 1.0e30f, vmax = -1.0e30f;
	unsigned dbg_nosem = 0, first_nosem = 0;
	float first_in0[4] = { 0, 0, 0, 0 };
	float first_in1[4] = { 0, 0, 0, 0 };
	int elem_reg[32];
	UINT elem_need[32];
	int nelem, in_rows = 32, out_rows = 32;
	LONGLONG tsc_b0;

	if (!c->dev || count == 0)
		return;
	d11_trace("draw%s count=%u start=%u inst=%u topo=%d vs=%d", indexed ? "Indexed" : "", count,
		start, inst, (int)c->topo, c->vs && c->vs->parsed);
	lock_dev(c->dev);
	if (c->topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
	    c->topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) {
		unlock_dev(c->dev);
		return;
	}
	rt = (c->rtv[0] && c->rtv[0]->res) ? c->rtv[0]->res : NULL;
	if (!rt || !res_ensure_pixels(rt)) {
		unlock_dev(c->dev);
		return;
	}
	memset(&rast, 0, sizeof(rast));
	rast.color = rt->pixels;
	rast.width = (int)rt->width;
	rast.height = (int)rt->height;
	if (c->dsv && c->dsv->res && res_ensure_pixels(c->dsv->res))
		rast.depth = c->dsv->res->depth;
	swrast_state_defaults(&st);
	st.cull = D3DCULL_NONE;
	st.blend_enable = 0;
	st.src_blend = D3DBLEND_ONE;
	st.dst_blend = D3DBLEND_ZERO;
	st.blend_op = D3DBLENDOP_ADD;
	if (c->blend) {
		D3D11_RENDER_TARGET_BLEND_DESC *b = &c->blend->desc.RenderTarget[0];
		st.blend_enable = b->BlendEnable;
		st.src_blend = (int)b->SrcBlend;
		st.dst_blend = (int)b->DestBlend;
		st.blend_op = (int)b->BlendOp;
		/* Honour the colour write mask, which was being dropped entirely.
		 *
		 * The rasterizer has always had the mask; nothing ever filled it in
		 * from the blend state, so every draw wrote all four channels no
		 * matter what the game asked for. A mask of zero is a normal way to
		 * say "run this draw but do not let it touch the target", and
		 * ignoring it turns work the game wanted invisible into pixels on
		 * screen. */
		st.write_mask = 0;
		if (b->RenderTargetWriteMask & D3D11_COLOR_WRITE_ENABLE_RED)
			st.write_mask |= 0x00FF0000u;
		if (b->RenderTargetWriteMask & D3D11_COLOR_WRITE_ENABLE_GREEN)
			st.write_mask |= 0x0000FF00u;
		if (b->RenderTargetWriteMask & D3D11_COLOR_WRITE_ENABLE_BLUE)
			st.write_mask |= 0x000000FFu;
		if (b->RenderTargetWriteMask & D3D11_COLOR_WRITE_ENABLE_ALPHA)
			st.write_mask |= 0xFF000000u;
		if (b->RenderTargetWriteMask != 0xF) {
			static int said;

			if (said < 4) {
				said++;
				d11_log("NOTE colour write mask %X (not all channels) is now "
					"honoured; before this it was ignored and the draw "
					"wrote every channel regardless",
					b->RenderTargetWriteMask);
			}
		}
	}
	if (st.blend_enable) {
		/* A multiply blend scales the destination by the source, so the
		 * source has to be white where it means "leave this alone". We
		 * approximate pixel shaders with a texture modulate, which hands
		 * back the raw texel, and a transparent texel is black: under a
		 * multiply that erases the destination instead of preserving it.
		 * The rasterizer folds alpha into the colour to restore the
		 * identity. Confined to multiply because it is the only blend where
		 * black and "no contribution" differ. */
		int mul = st.src_blend == D3DBLEND_ZERO || st.dst_blend == D3DBLEND_SRCCOLOR ||
			  st.dst_blend == D3DBLEND_INVSRCCOLOR;

		if (mul && mul_identity())
			st.mul_identity = 1;
	}
	if (break_blend() && st.blend_enable) {
		int mul = st.src_blend == D3DBLEND_ZERO || st.dst_blend == D3DBLEND_SRCCOLOR ||
			  st.dst_blend == D3DBLEND_INVSRCCOLOR;

		switch (break_blend()) {
		case 1:
			st.src_blend = D3DBLEND_ONE;
			st.dst_blend = D3DBLEND_ONE;
			st.blend_op = D3DBLENDOP_ADD;
			break;
		case 2:
			if (mul)
				st.write_mask = 0;
			break;
		case 3:
			st.write_mask = 0;
			break;
		default:
			break;
		}
	}
	if (g_exp_transp == 1 && st.blend_enable)
		return;
	if (g_exp_transp == 2)
		st.blend_enable = 0;
	st.z_enable = 0;
	if (c->dss && c->dss->desc.DepthEnable && rast.depth) {
		st.z_enable = 1;
		st.z_write = c->dss->desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
		st.z_func = (int)c->dss->desc.DepthFunc;
	}
	if (c->rs && c->rs->desc.ScissorEnable && c->nscissor) {
		/* Scaled into real space first, for the same reason as the
		 * viewport and before the size heuristic below, which compares
		 * against rt->width. */
		float fx = rt->virt_w ? rt_scale_x(rt) : 1.0f;
		float fy = rt->virt_w ? rt_scale_y(rt) : 1.0f;
		int x0 = (int)((float)c->scissor[0].left * fx);
		int y0 = (int)((float)c->scissor[0].top * fy);
		int x1 = (int)((float)c->scissor[0].right * fx + 0.5f);
		int y1 = (int)((float)c->scissor[0].bottom * fy + 0.5f);
		int sw = x1 - x0;
		int sh = y1 - y0;
		/* D3D11SW_SCISSOR=0 restores an old workaround: honour the scissor
		 * only when it covers at least half the target on both axes.
		 *
		 * It was written for a Unity title that sets a 640x360 canvas
		 * scissor against a 2560x1440 backbuffer, which clipped away every
		 * scaled glyph. As a general rule it is wrong, because "small" is a
		 * perfectly ordinary thing for a scissor to be: a menu clipped to a
		 * panel in the corner of the screen is exactly that, and discarding
		 * it paints that menu over the whole frame with its content
		 * otherwise intact. Guessing at intent from a rectangle's size is
		 * not something this layer should be doing, so the default is now
		 * to clip where the game said to clip. */
		if (scissor_honour() || (sw * 2 >= (int)rt->width && sh * 2 >= (int)rt->height)) {
			st.scissor_enable = 1;
			st.scissor_x0 = x0;
			st.scissor_y0 = y0;
			st.scissor_x1 = x1;
			st.scissor_y1 = y1;
		} else {
			static int said;

			if (said < 4) {
				said++;
				d11_log("NOTE dropping a %dx%d scissor at %d,%d on a %ux%u target "
					"because D3D11SW_SCISSOR=0. Geometry the game wanted "
					"clipped to that rectangle will cover the frame instead",
					sw, sh, x0, y0, rt->width, rt->height);
			}
			d11_trace("ignore scissor %d,%d %dx%d rt=%ux%u", (int)c->scissor[0].left,
				(int)c->scissor[0].top, sw, sh, rt->width, rt->height);
		}
	}
	memset(&tex, 0, sizeof(tex));
	{
		int si;
		a8_font = 0;
		for (si = 0; si < 4; si++) {
			/* Faucet: an evicted (phantom-with-bank) texture is painted back into
			 * the arena here, on the draw thread, before it is sampled. If it fits
			 * the window it becomes a real texture below; if not it stays magenta. */
			if (c->ps_srv[si] && c->ps_srv[si]->res &&
			    c->ps_srv[si]->res->phantom && faucet_on())
				faucet_fault_in(c->ps_srv[si]->res);
			if (c->ps_srv[si] && c->ps_srv[si]->res &&
			    c->ps_srv[si]->res->phantom) {
				/* A capped texture: sample the magenta placeholder at 16x16
				 * with clamp addressing, so the sampler stays in bounds no
				 * matter the texture's real dimensions. Visible, never a
				 * crash. First phantom wins, like the real-texture case. */
				tex.pixels = g_phantom_px;
				tex.width = PHANTOM_DIM;
				tex.height = PHANTOM_DIM;
				src_res = c->ps_srv[si]->res;
				st.bilinear = 0;
				st.addr_u = D3DTADDRESS_CLAMP;
				st.addr_v = D3DTADDRESS_CLAMP;
				break;
			}
			if (c->ps_srv[si] && c->ps_srv[si]->res && res_ensure_pixels(c->ps_srv[si]->res) &&
			    c->ps_srv[si]->res->pixels) {
				Sw11Res *tr = c->ps_srv[si]->res;

				faucet_touch(tr);
				pal_maybe_take(tr);
				int a8 = (tr->format == DXGI_FORMAT_A8_UNORM ||
					   tr->format == DXGI_FORMAT_R8_UNORM ||
					   tr->format == DXGI_FORMAT_R8_UINT);
				if (a8 && !tr->has_texels) {
					/* Once per resource: there are only a handful of
					 * atlases, and knowing which never receive texels
					 * names the upload path we are still missing. */
					if (!tr->warned_empty) {
						tr->warned_empty = 1;
						d11_log("A8/R8 #%d %ux%u sampled with no texels",
							tr->id, tr->width, tr->height);
					}
					if (!c->blend || !c->blend->desc.RenderTarget[0].BlendEnable) {
						st.blend_enable = 1;
						st.src_blend = D3DBLEND_SRCALPHA;
						st.dst_blend = D3DBLEND_INVSRCALPHA;
					}
					break;
				}
				tex.pixels = tr->pixels;
				tex.width = (int)tr->width;
				tex.height = (int)tr->height;
				tex.gen = tr->gpu_gen;
				src_res = tr;
				st.bilinear = 1;
				st.addr_u = D3DTADDRESS_CLAMP;
				st.addr_v = D3DTADDRESS_CLAMP;
				if (c->ps_samp[si]) {
					st.bilinear = c->ps_samp[si]->desc.Filter != D3D11_FILTER_MIN_MAG_MIP_POINT;
					st.addr_u = (int)c->ps_samp[si]->desc.AddressU;
					st.addr_v = (int)c->ps_samp[si]->desc.AddressV;
				}
				if (a8) {
					/* A single-channel font atlas is either coverage or a
					 * signed distance field, and both put the glyph edge at
					 * 0.5. Cutting at 16 keeps the whole distance ramp, which
					 * fills small glyphs solid -- the quad becomes a block
					 * once the glyph is only a few pixels across.
					 *
					 * Cutting at 128 instead put the edge in the right place
					 * but kept the decision binary, so a glyph gained or lost
					 * whole pixels wherever its outline fell between samples,
					 * which is the chewed look. The ramp either side of 0.5
					 * is the coverage information needed to avoid that, so it
					 * is interpolated and steepened rather than thresholded;
					 * the slope is set once the magnification is known.
					 *
					 * Steepening is what makes this safe. Interpolating the
					 * ramp and then blending it as-is is strictly worse than
					 * the hard cut - it is the soft blob the comment above
					 * describes - so if sharpening is switched off the cut
					 * has to come back with it. */
					if (text_sharpen() > 0.0f) {
						st.bilinear = 1;
						st.alpha_test = 0;
						a8_font = 1;
					} else {
						st.bilinear = 0;
						st.alpha_test = 1;
						st.alpha_ref = 128;
						st.alpha_func = D3DCMP_GREATEREQUAL;
					}
					if (!c->blend || !c->blend->desc.RenderTarget[0].BlendEnable) {
						st.blend_enable = 1;
						st.src_blend = D3DBLEND_SRCALPHA;
						st.dst_blend = D3DBLEND_INVSRCALPHA;
					}
				}
				break;
			}
		}
	}
	if (c->nvp) {
		vp = c->vp[0];
		/* Converted here rather than in RSSetViewports so that what the
		 * game stored is what RSGetViewports hands back. The clamps just
		 * below are against rt->width/height, i.e. real space, so this has
		 * to happen before them. */
		if (rt->virt_w) {
			float fx = rt_scale_x(rt), fy = rt_scale_y(rt);

			vp.TopLeftX *= fx;
			vp.Width *= fx;
			vp.TopLeftY *= fy;
			vp.Height *= fy;
		}
	} else {
		memset(&vp, 0, sizeof(vp));
		vp.Width = (float)rt->width;
		vp.Height = (float)rt->height;
		vp.MaxDepth = 1.0f;
	}
	if (vp.Width < 1.0f)
		vp.Width = 1.0f;
	if (vp.Height < 1.0f)
		vp.Height = 1.0f;
	if (vp.TopLeftX < 0.0f)
		vp.TopLeftX = 0.0f;
	if (vp.TopLeftY < 0.0f)
		vp.TopLeftY = 0.0f;
	if (vp.TopLeftX >= (float)rt->width)
		vp.TopLeftX = 0.0f;
	if (vp.TopLeftY >= (float)rt->height)
		vp.TopLeftY = 0.0f;
	if (vp.TopLeftX + vp.Width > (float)rt->width)
		vp.Width = (float)rt->width - vp.TopLeftX;
	if (vp.TopLeftY + vp.Height > (float)rt->height)
		vp.Height = (float)rt->height - vp.TopLeftY;
	memset(cb, 0, sizeof(cb));
	memset(cb_bytes, 0, sizeof(cb_bytes));
	for (i = 0; i < 14; i++) {
		if (c->cb_vs[i] && c->cb_vs[i]->cpu) {
			UINT off = c->cb_vs_first[i] * 16u;
			UINT nbytes = c->cb_vs[i]->byte_width;
			if (off >= nbytes)
				continue;
			cb[i] = c->cb_vs[i]->cpu + off;
			cb_bytes[i] = nbytes - off;
			if (c->cb_vs_n[i]) {
				UINT slice = c->cb_vs_n[i] * 16u;
				if (slice < cb_bytes[i])
					cb_bytes[i] = slice;
			}
		}
	}
	if (c->vs && c->vs->parsed) {
		pos_reg = dxbc_find_sysval_pos(c->vs->dxbc.osgn, c->vs->dxbc.n_osgn);
		if (pos_reg < 0)
			pos_reg = dxbc_find_sem_any(c->vs->dxbc.osgn, c->vs->dxbc.n_osgn,
						    "SV_Position");
		if (pos_reg < 0)
			pos_reg = dxbc_find_sem_any(c->vs->dxbc.osgn, c->vs->dxbc.n_osgn, "POSITION");
		uv_reg = dxbc_find_sem_any(c->vs->dxbc.osgn, c->vs->dxbc.n_osgn, "TEXCOORD");
		col_reg = dxbc_find_sem_any(c->vs->dxbc.osgn, c->vs->dxbc.n_osgn, "COLOR");
		if (pos_reg >= 0)
			pos_reg = (int)c->vs->dxbc.osgn[pos_reg].reg;
		else
			pos_reg = 0;
		if (uv_reg >= 0)
			uv_reg = (int)c->vs->dxbc.osgn[uv_reg].reg;
		if (col_reg >= 0)
			col_reg = (int)c->vs->dxbc.osgn[col_reg].reg;
	}
	/* An element's shader input register, and the byte width its format
	 * needs, are fixed for the whole draw - but both were being worked out
	 * per vertex, and working out the register means string-comparing the
	 * element's semantic against every entry of the shader's input
	 * signature. That put tens of strcmps on each of tens of thousands of
	 * vertices per frame. D3D11 caps a layout at 32 elements, so the map
	 * fits a fixed array. */
	nelem = c->layout ? (int)c->layout->n : 0;
	if (nelem > 32)
		nelem = 32;
	for (i = 0; i < (UINT)nelem; i++) {
		D3D11_INPUT_ELEMENT_DESC *e = &c->layout->elems[i];

		elem_need[i] = fmt_stride(e->Format);
		if (!elem_need[i])
			elem_need[i] = 4;
		elem_reg[i] = 0;
		if (c->vs && c->vs->parsed && e->SemanticName) {
			int si = dxbc_find_sem(c->vs->dxbc.isgn, c->vs->dxbc.n_isgn,
					       e->SemanticName, e->SemanticIndex);
			/* Negative marks an attribute the shader never declared. */
			elem_reg[i] = (si >= 0) ? (int)c->vs->dxbc.isgn[si].reg : -1;
		}
	}
	/* Only the registers the signatures actually name can be read or
	 * written, so the kilobyte of scratch cleared per vertex only has to be
	 * cleared that far. */
	if (c->vs && c->vs->parsed) {
		int mi = -1, mo = -1;
		unsigned q;

		for (q = 0; q < c->vs->dxbc.n_isgn; q++)
			if ((int)c->vs->dxbc.isgn[q].reg > mi)
				mi = (int)c->vs->dxbc.isgn[q].reg;
		for (q = 0; q < c->vs->dxbc.n_osgn; q++)
			if ((int)c->vs->dxbc.osgn[q].reg > mo)
				mo = (int)c->vs->dxbc.osgn[q].reg;
		if (mi >= 0 && mi < 31)
			in_rows = mi + 1;
		if (mo >= 0 && mo < 31)
			out_rows = mo + 1;
	}
	if (c->topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
		prims = count / 3;
	else
		prims = count > 2 ? count - 2 : 0;
	batch = (SwTri *)malloc(sizeof(SwTri) * (prims ? prims : 1) * (inst ? inst : 1));
	if (!batch) {
		unlock_dev(c->dev);
		return;
	}
	tsc_b0 = (LONGLONG)__builtin_readcyclecounter();
	{
		UINT ntri = 0;
		for (ii = 0; ii < (inst ? inst : 1); ii++) {
			for (i = 0; i < prims; i++) {
				UINT vid[3];
				SwVert sv[3];
				int v;
				if (c->topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
					vid[0] = i * 3;
					vid[1] = i * 3 + 1;
					vid[2] = i * 3 + 2;
				} else {
					vid[0] = i;
					vid[1] = i + 1;
					vid[2] = i + 2;
					if (i & 1) {
						UINT t = vid[1];
						vid[1] = vid[2];
						vid[2] = t;
					}
				}
				for (v = 0; v < 3; v++) {
					UINT geom, k;
					float in[32][4], out[32][4];
					float px, py, pz, pw, rhw;
					memset(in, 0, (size_t)in_rows * sizeof(in[0]));
					memset(out, 0, (size_t)out_rows * sizeof(out[0]));
					g_n_verts++;
					if (indexed && c->ib && c->ib->cpu) {
						UINT idx = start + vid[v];
						UINT esz = (c->ib_fmt == DXGI_FORMAT_R32_UINT) ? 4u : 2u;
						UINT avail = (c->ib_off < c->ib->cpu_size)
								     ? (c->ib->cpu_size - c->ib_off)
								     : 0;
						if (esz == 0 || idx >= avail / esz)
							geom = 0;
						else {
							const unsigned char *ip = c->ib->cpu + c->ib_off;
							if (c->ib_fmt == DXGI_FORMAT_R32_UINT)
								geom = ((const UINT *)ip)[idx] + (UINT)base;
							else
								geom = ((const USHORT *)ip)[idx] + (UINT)base;
						}
					} else
						geom = start + vid[v];
					dbg_loaded = 0;
					dbg_nosem = 0;
					if (c->layout) {
						for (k = 0; k < (UINT)nelem; k++) {
							D3D11_INPUT_ELEMENT_DESC *e = &c->layout->elems[k];
							UINT slot = e->InputSlot;
							Sw11Res *vb;
							const unsigned char *vp;
							UINT stride, off, idx;
							int ireg;
							if (slot >= 16)
								continue;
							vb = c->vb[slot];
							if (!vb || !vb->cpu)
								continue;
							stride = c->vb_stride[slot];
							off = c->vb_off[slot];
							idx = (e->InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA)
								      ? ii
								      : geom;
							if (!stride)
								continue;
							{
								UINT need = elem_need[k];
								UINT voff;
								if (idx && stride > (0xffffffffu - e->AlignedByteOffset) / idx) {
									continue;
								}
								voff = off + idx * stride + e->AlignedByteOffset;
								if (voff + need > vb->cpu_size)
									continue;
								vp = vb->cpu + voff;
							}
							ireg = elem_reg[k];
							if (ireg < 0) {
								/* No matching input register: this
								 * attribute is unused by the shader.
								 * Loading it into r0 would overwrite
								 * whatever really belongs there. */
								dbg_nosem++;
								continue;
							}
							if (ireg >= 0 && ireg < 32) {
								load_fmt(in[ireg], vp, e->Format);
								dbg_loaded++;
							}
						}
					}
					if (i == 0 && v == 0 && ii == 0) {
						first_loaded = dbg_loaded;
						first_nosem = dbg_nosem;
						memcpy(first_in0, in[0], sizeof(first_in0));
						memcpy(first_in1, in[1], sizeof(first_in1));
					}
					if (c->vs && c->vs->parsed)
						vs_ran = dxbc_exec_vs(&c->vs->dxbc, in, cb, cb_bytes,
								      out);
					if (!vs_ran)
						memcpy(out, in,
						       (size_t)(in_rows < out_rows ? in_rows
										   : out_rows) *
							       sizeof(in[0]));
					{
						float *pos = out[pos_reg >= 0 ? pos_reg : 0];
						pw = pos[3] != 0.0f ? pos[3] : 1.0f;
						rhw = 1.0f / pw;
						px = pos[0] * rhw;
						py = pos[1] * rhw;
						pz = pos[2] * rhw;
						sv[v].x = (px * 0.5f + 0.5f) * vp.Width + vp.TopLeftX;
						sv[v].y = (0.5f - py * 0.5f) * vp.Height + vp.TopLeftY;
						sv[v].z = pz * (vp.MaxDepth - vp.MinDepth) + vp.MinDepth;
						sv[v].rhw = rhw;
						/* Only when no shader ran can a raw pixel-space vertex
						 * reach here untransformed. If the shader did run its
						 * clip-space output is authoritative, and reinterpreting
						 * off-screen geometry as pixels would invent triangles. */
						if (!vs_ran &&
						    (px > 2.0f || px < -2.0f || py > 2.0f || py < -2.0f)) {
							sv[v].x = pos[0] + vp.TopLeftX;
							sv[v].y = vp.Height - pos[1] + vp.TopLeftY;
							sv[v].z = 0.5f;
							sv[v].rhw = 1.0f;
						}
						sv[v].color = 0xffffffffu;
						sv[v].u = 0;
						sv[v].v = 0;
						if (uv_reg >= 0) {
							sv[v].u = out[uv_reg][0];
							sv[v].v = out[uv_reg][1];
						}
						if (sv[v].u < umin)
							umin = sv[v].u;
						if (sv[v].u > umax)
							umax = sv[v].u;
						if (sv[v].v < vmin)
							vmin = sv[v].v;
						if (sv[v].v > vmax)
							vmax = sv[v].v;
						/* No COLOR output means the pixel shader supplies the
						 * colour itself; white is the neutral modulator for
						 * the texture. Otherwise take the shader's value as
						 * it stands: forcing black to white or alpha to
						 * opaque was hiding the real output. */
						if (col_reg >= 0)
							sv[v].color = color_from_vec(out[col_reg]);
						if (i == 0 && v == 0 && ii == 0) {
							first_x = sv[v].x;
							first_y = sv[v].y;
							memcpy(first_pos, pos, sizeof(first_pos));
						}
					}
				}
				batch[ntri].a = sv[0];
				batch[ntri].b = sv[1];
				batch[ntri].c = sv[2];
				ntri++;
			}
		}
		g_tsc_batch += (LONGLONG)__builtin_readcyclecounter() - tsc_b0;
		g_n_draws++;
		g_n_tris_in += prims * (inst ? inst : 1);
		g_n_tris_out += ntri;
		/* The AVX2 span kernel only accepts WRAP addressing, and Unity
		 * samples sprites and UI with CLAMP, so the vector path was being
		 * declined for practically every draw in the frame. The two modes
		 * are indistinguishable when no sampled coordinate reaches an edge
		 * of the texture, which is the normal case for an atlased quad.
		 * Point sampling never reads past floor(u*w), so u in [0,1] is
		 * enough; bilinear also touches the next texel, so it needs a
		 * half-texel of margin at both ends. */
		if (ntri && tex.pixels && tex.width > 0 && tex.height > 0) {
			float lo_u = 0.0f, hi_u = 1.0f, lo_v = 0.0f, hi_v = 1.0f;
			if (st.bilinear) {
				lo_u = 0.5f / (float)tex.width;
				hi_u = 1.0f - 1.0f / (float)tex.width;
				lo_v = 0.5f / (float)tex.height;
				hi_v = 1.0f - 1.0f / (float)tex.height;
			}
			if (umin >= lo_u && umax <= hi_u && vmin >= lo_v && vmax <= hi_v) {
				st.uv_in_bounds = 1;
				st.addr_u = D3DTADDRESS_WRAP;
				st.addr_v = D3DTADDRESS_WRAP;
			} else if (umin >= 0.0f && umax <= 1.0f && vmin >= 0.0f &&
				   vmax <= 1.0f) {
				/* Inside the texture but touching an edge, so the margin
				 * above cannot prove the bilinear tap stays in range. A
				 * draw that never leaves [0,1] is not tiling, though, and
				 * wrap only differs from clamp on that one tap: wrap
				 * fetches the opposite edge, clamp repeats the near one.
				 * Calling it clamp costs a texel of border and buys the
				 * vector path, which indexes any dimensions when it does
				 * not have to wrap. Both samplers are told, so they still
				 * agree with each other. */
				st.addr_u = D3DTADDRESS_CLAMP;
				st.addr_v = D3DTADDRESS_CLAMP;
				g_n_npot_clamped++;
			}
		}
		/* A magnifying draw fetches four texels and weights them to
		 * reconstruct detail that is not in the source. The upscale blit
		 * this game ends its frame with is an exact 2x of pixel art, where
		 * that costs four gathers per pixel to produce a blurrier result
		 * than the single gather point sampling needs. Opt-in, because it
		 * is a visible change wherever the magnification is not integral. */
		if (ntri && st.bilinear && tex.pixels && point_mag_on()) {
			float du = (umax - umin) * (float)tex.width;
			float dv = (vmax - vmin) * (float)tex.height;
			float sx = 0.0f, sy = 0.0f;
			unsigned t;
			for (t = 0; t < ntri; t++) {
				float x0 = fminf(batch[t].a.x, fminf(batch[t].b.x, batch[t].c.x));
				float x1 = fmaxf(batch[t].a.x, fmaxf(batch[t].b.x, batch[t].c.x));
				float y0 = fminf(batch[t].a.y, fminf(batch[t].b.y, batch[t].c.y));
				float y1 = fmaxf(batch[t].a.y, fmaxf(batch[t].b.y, batch[t].c.y));
				if (x1 - x0 > sx)
					sx = x1 - x0;
				if (y1 - y0 > sy)
					sy = y1 - y0;
			}
			if (du > 0.5f && dv > 0.5f && sx >= du * 1.8f && sy >= dv * 1.8f)
				st.bilinear = 0;
		}
		/* The ramp is fixed in the atlas and spans several texels, so the
		 * slope that lands its transition inside one screen pixel is that
		 * spread times the magnification. Magnification alone is not it:
		 * text is usually drawn smaller than its atlas cell, which gives a
		 * factor of one, no sharpening, and a solid block per glyph.
		 *
		 * Floored well above one so the edge is always crisper than the
		 * raw ramp, and capped so a badly scaled quad degrades to the hard
		 * cut rather than something worse. */
		if (ntri && a8_font && tex.pixels && tex.width > 0 && tex.height > 0) {
			float du = (umax - umin) * (float)tex.width;
			float dv = (vmax - vmin) * (float)tex.height;
			float sx = 0.0f, sy = 0.0f, mag, k;
			unsigned t;
			for (t = 0; t < ntri; t++) {
				float x0 = fminf(batch[t].a.x, fminf(batch[t].b.x, batch[t].c.x));
				float x1 = fmaxf(batch[t].a.x, fmaxf(batch[t].b.x, batch[t].c.x));
				float y0 = fminf(batch[t].a.y, fminf(batch[t].b.y, batch[t].c.y));
				float y1 = fmaxf(batch[t].a.y, fmaxf(batch[t].b.y, batch[t].c.y));
				if (x1 - x0 > sx)
					sx = x1 - x0;
				if (y1 - y0 > sy)
					sy = y1 - y0;
			}
			mag = 0.0f;
			if (du > 0.5f)
				mag = sx / du;
			if (dv > 0.5f && sy / dv > mag)
				mag = sy / dv;
			if (mag <= 0.0f)
				mag = 1.0f;
			k = text_sharpen() * mag;
			if (k < 2.0f)
				k = 2.0f;
			else if (k > 64.0f)
				k = 64.0f;
			st.alpha_sharpen = (int)(256.0f * k);
		}
		/* A draw that samples a retired texture is dropped rather than drawn.
		 *
		 * Retirement keeps the header and releases the texels, so a stale
		 * pointer finds an object instead of freed memory - which is what stopped
		 * the crash in PresentFrame. But it leaves pixels NULL, and NULL reaching
		 * the call below means "untextured", not "transparent": the quad came out
		 * flat-shaded and opaque. That is the field of solid white rectangles
		 * after a restore, and it is expensive as well as wrong, because an
		 * opaque quad shades and blends every pixel where the sprite it replaced
		 * would have discarded most of them.
		 *
		 * Dropping it is the honest answer either way. The texture the game is
		 * asking for no longer exists, so there is nothing correct to put on
		 * screen, and a missing sprite reads as a glitch while a white block
		 * reads as a broken renderer. */
		/* Only when the texels are actually gone. A retired object whose payload
		 * was kept for the snapshot still has exactly the pixels the game is
		 * asking for, and dropping that draw would throw away the sprite this
		 * whole mechanism exists to preserve. */
		if (ntri && src_res && src_res->retired && !src_res->pixels) {
			static LONG said;
			if (InterlockedIncrement(&said) == 1)
				d11_log("dropping draws that sample retired textures whose pixels "
					"were NOT kept (first was #%d %ux%u); these sprites will be "
					"missing. Either the release happened with no savestate "
					"live, or the retain budget was full",
					src_res->id, src_res->width, src_res->height);
			ntri = 0;
		}
		if (ntri) {
			/* At mode 2 the rasteriser stays authoritative and the adapter
			 * draws the same thing beside it, so a declined draw costs a
			 * count rather than a torn frame. At mode 3 the adapter owns
			 * the frame: a draw it accepts is not rasterised at all, which
			 * is what makes letting go of the texture's CPU bytes safe,
			 * and a draw it declines is a sprite that will be missing. The
			 * decline count is therefore a correctness figure at mode 3
			 * and merely a coverage one at mode 2. */
			int on_gpu = 0;

			if (gpu_mode() >= 2 && rt->is_bb && src_res &&
			    (tex.pixels || src_res->gpu_released))
				on_gpu = gpu_shadow_draw(rt, batch, (int)ntri, src_res, &tex,
							 &st);
			if (!(on_gpu && gpu_mode() >= 3)) {
				swrast_triangles(&rast, batch, (int)ntri,
						 tex.pixels ? &tex : NULL, &st);
				/* The rasteriser has just changed this surface, and
				 * if it is an intermediate target the game will
				 * sample it next. Only the decode path bumps this
				 * otherwise, so without it a target composed in
				 * software uploads once and is never refreshed -
				 * which is a mirror holding the empty first frame
				 * forever. */
				if (rt && !rt->is_bb)
					rt->gpu_gen++;
			}
			census_draw(rt, batch, (int)ntri, src_res, &st);
		frame_note_draw(rt, ntri);
			perf_note_area(rt, batch, ntri, src_res, tex.width, tex.height,
				       st.blend_enable ? (st.src_blend | (st.dst_blend << 8)) : 0);
			if (rt->is_bb)
				g_bb_writes += ntri;
		}
		d11_trace("draw done ntri=%u rt=#%d %ux%u bb=%d xy=%.0f,%.0f pos=%.3f,%.3f,%.3f,%.3f "
			"regs=p%d/u%d/c%d srv=#%d %dx%d fmt=%d t00=%08x vcol=%08x | nlay=%u ld=%u "
			"ns=%u in0=%.1f,%.1f,%.1f,%.1f blend=%d/%d/%d",
			ntri, rt->id, rt->width, rt->height, rt->is_bb, first_x, first_y,
			first_pos[0], first_pos[1], first_pos[2], first_pos[3], pos_reg, uv_reg,
			col_reg, src_res ? src_res->id : -1, tex.width, tex.height,
			src_res ? (int)src_res->format : -1,
			/* Not just "has a pointer": a released or retired texture can
			 * still be carrying the address its payload used to live at,
			 * and with tracing on that is a page the adapter owns now. */
			(src_res && src_res->pixels && !src_res->gpu_released &&
			 !src_res->retired)
				? src_res->pixels[0]
				: 0,
			ntri ? batch[0].a.color : 0, c->layout ? c->layout->n : 0, first_loaded,
			first_nosem, first_in0[0], first_in0[1], first_in0[2], first_in0[3],
			st.blend_enable, st.src_blend, st.dst_blend);
		free(batch);
	}
	unlock_dev(c->dev);
}

static Sw11Context *ctx_new(Sw11Device *dev, int deferred)
{
	Sw11Context *c = deferred ? (Sw11Context *)calloc(1, sizeof(*c))
				  : (Sw11Context *)pin_calloc(sizeof(*c), "immediate context", PK_SINGLE);
	if (!c)
		return NULL;
	c->iface.lpVtbl = &kCtxVtbl;
	c->ref = 1;
	c->dev = dev;
	c->deferred = deferred;
	c->sample_mask = 0xffffffffu;
	c->blend_factor[0] = c->blend_factor[1] = c->blend_factor[2] = c->blend_factor[3] = 1.0f;
	return c;
}

static HRESULT sw11_create_device(IDXGIAdapter *adapter, UINT flags, D3D_FEATURE_LEVEL *level,
				  Sw11Device **out)
{
	Sw11Device *d;
	Sw11Factory *f = NULL;
	Sw11Adapter *a = NULL;
	ensure_vtbls();
	*out = NULL;
	d = (Sw11Device *)pin_calloc(sizeof(*d), "device", PK_SINGLE);
	if (!d)
		return E_OUTOFMEMORY;
	d->iface.lpVtbl = &kDevVtbl;
	d->dxgi.lpVtbl = &kDxgiDevVtbl;
	d->ref = 1;
	d->flags = flags;
	d->level = D3D_FEATURE_LEVEL_11_0;
	d->latency = 3;
	InitializeCriticalSection(&d->lock);
	if (adapter) {
		IDXGIAdapter1 *a1 = NULL;
		if (SUCCEEDED(adapter->lpVtbl->QueryInterface(adapter, &IID_IDXGIAdapter1,
								 (void **)&a1)) &&
		    a1 && ((Sw11Adapter *)a1)->iface.lpVtbl == &kAdpVtbl) {
			a = (Sw11Adapter *)a1;
			f = a->factory;
			f->iface.lpVtbl->AddRef(&f->iface);
		} else {
			if (a1)
				a1->lpVtbl->Release(a1);
			a = NULL;
		}
	}
	if (!f) {
		f = factory_new();
		if (!f) {
			obj_free(d);
			return E_OUTOFMEMORY;
		}
		if (FAILED(Adp_create(f, (IDXGIAdapter1 **)&a))) {
			f->iface.lpVtbl->Release(&f->iface);
			obj_free(d);
			return E_OUTOFMEMORY;
		}
	}
	d->factory = f;
	d->adapter = a;
	d->imm = ctx_new(d, 0);
	if (!d->imm) {
		Dev_Release(&d->iface);
		return E_OUTOFMEMORY;
	}
	if (level)
		*level = d->level;
	*out = d;
	d11_log("D3D11CreateDevice fl=11.0 flags=%u", flags);
	return S_OK;
}

HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
				 const D3D_FEATURE_LEVEL *levels, UINT nlevels, UINT sdk,
				 ID3D11Device **device, D3D_FEATURE_LEVEL *got,
				 ID3D11DeviceContext **ctx)
{
	Sw11Device *d;
	HRESULT hr;
	(void)type;
	(void)sw;
	(void)levels;
	(void)nlevels;
	(void)sdk;
	savestate_hooks_install();
	ensure_vtbls();
	if (device)
		*device = NULL;
	if (ctx)
		*ctx = NULL;
	hr = sw11_create_device(adapter, flags, got, &d);
	if (FAILED(hr))
		return hr;
	if (device) {
		*device = (ID3D11Device *)&d->iface;
		Dev_AddRef(&d->iface);
	}
	if (ctx)
		Dev_GetImmediateContext(&d->iface, ctx);
	Dev_Release(&d->iface);
	return S_OK;
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE sw,
					      UINT flags, const D3D_FEATURE_LEVEL *levels, UINT nlevels,
					      UINT sdk, const DXGI_SWAP_CHAIN_DESC *sd,
					      IDXGISwapChain **swap, ID3D11Device **device,
					      D3D_FEATURE_LEVEL *got, ID3D11DeviceContext **ctx)
{
	ID3D11Device *dev = NULL;
	HRESULT hr;
	if (swap)
		*swap = NULL;
	hr = D3D11CreateDevice(adapter, type, sw, flags, levels, nlevels, sdk, &dev, got, ctx);
	if (FAILED(hr))
		return hr;
	if (sd && swap) {
		IDXGIDevice *dx = NULL;
		IDXGIAdapter *ad = NULL;
		IDXGIFactory *f = NULL;
		hr = dev->lpVtbl->QueryInterface(dev, &IID_IDXGIDevice, (void **)&dx);
		if (SUCCEEDED(hr) && dx) {
			hr = dx->lpVtbl->GetAdapter(dx, &ad);
			if (SUCCEEDED(hr) && ad) {
				hr = ad->lpVtbl->GetParent(ad, &IID_IDXGIFactory, (void **)&f);
				if (SUCCEEDED(hr) && f)
					hr = f->lpVtbl->CreateSwapChain(f, (IUnknown *)dev,
									(DXGI_SWAP_CHAIN_DESC *)sd, swap);
			}
		}
		if (f)
			f->lpVtbl->Release(f);
		if (ad)
			ad->lpVtbl->Release(ad);
		if (dx)
			dx->lpVtbl->Release(dx);
	}
	if (device)
		*device = dev;
	else if (dev)
		dev->lpVtbl->Release(dev);
	return hr;
}

static void ensure_vtbls(void)
{
	if (g_vtbl_ready)
		return;
	/* All of them, before anything real is written over the top. */
	VTBL_FILL(kAdpVtbl, IDXGIAdapter1Vtbl);
	VTBL_FILL(kOutVtbl, IDXGIOutputVtbl);
	VTBL_FILL(kDxgiDevVtbl, IDXGIDevice1Vtbl);
	VTBL_FILL(kTexVtbl, ID3D11Texture2DVtbl);
	VTBL_FILL(kBufVtbl, ID3D11BufferVtbl);
	VTBL_FILL(kRtvVtbl, ID3D11RenderTargetViewVtbl);
	VTBL_FILL(kSrvVtbl, ID3D11ShaderResourceViewVtbl);
	VTBL_FILL(kDsvVtbl, ID3D11DepthStencilViewVtbl);
	VTBL_FILL(kVSVtbl, ID3D11VertexShaderVtbl);
	VTBL_FILL(kPSVtbl, ID3D11PixelShaderVtbl);
	VTBL_FILL(kLayVtbl, ID3D11InputLayoutVtbl);
	VTBL_FILL(kBlendVtbl, ID3D11BlendStateVtbl);
	VTBL_FILL(kDSVtbl, ID3D11DepthStencilStateVtbl);
	VTBL_FILL(kRastVtbl, ID3D11RasterizerStateVtbl);
	VTBL_FILL(kSampVtbl, ID3D11SamplerStateVtbl);
	VTBL_FILL(kQueryVtbl, ID3D11QueryVtbl);
	/* The factory and the swap chain were never prefilled at all. */
	VTBL_FILL(kFactVtbl, IDXGIFactory2Vtbl);
	VTBL_FILL(kSwapVtbl, IDXGISwapChain1Vtbl);
	VTBL_FILL(kDevVtbl, ID3D11Device1Vtbl);
	kDevVtbl.QueryInterface = Dev_QI;
	kDevVtbl.AddRef = Dev_AddRef;
	kDevVtbl.Release = Dev_Release;
	kDevVtbl.CreateBuffer = Dev_CreateBuffer;
	kDevVtbl.CreateTexture1D = Dev_CreateTexture1D;
	kDevVtbl.CreateTexture2D = Dev_CreateTexture2D;
	kDevVtbl.CreateTexture3D = Dev_CreateTexture3D;
	kDevVtbl.CreateShaderResourceView = Dev_CreateSRV;
	kDevVtbl.CreateUnorderedAccessView = Dev_CreateUAV;
	kDevVtbl.CreateRenderTargetView = Dev_CreateRTV;
	kDevVtbl.CreateDepthStencilView = Dev_CreateDSV;
	kDevVtbl.CreateInputLayout = Dev_CreateInputLayout;
	kDevVtbl.CreateVertexShader = Dev_CreateVS;
	kDevVtbl.CreateGeometryShader = Dev_CreateGS;
	kDevVtbl.CreateGeometryShaderWithStreamOutput = Dev_CreateGSSO;
	kDevVtbl.CreatePixelShader = Dev_CreatePS;
	kDevVtbl.CreateHullShader = Dev_CreateHS;
	kDevVtbl.CreateDomainShader = Dev_CreateDS;
	kDevVtbl.CreateComputeShader = Dev_CreateCS;
	kDevVtbl.CreateClassLinkage = Dev_CreateClassLinkage;
	kDevVtbl.CreateBlendState = Dev_CreateBlendState;
	kDevVtbl.CreateDepthStencilState = Dev_CreateDSS;
	kDevVtbl.CreateRasterizerState = Dev_CreateRS;
	kDevVtbl.CreateSamplerState = Dev_CreateSampler;
	kDevVtbl.CreateQuery = Dev_CreateQuery;
	kDevVtbl.CreatePredicate = Dev_CreatePredicate;
	kDevVtbl.CreateCounter = Dev_CreateCounter;
	kDevVtbl.CreateDeferredContext = Dev_CreateDeferredContext;
	kDevVtbl.OpenSharedResource = Dev_OpenSharedResource;
	kDevVtbl.CheckFormatSupport = Dev_CheckFormatSupport;
	kDevVtbl.CheckMultisampleQualityLevels = Dev_CheckMultisampleQualityLevels;
	kDevVtbl.CheckCounterInfo = Dev_CheckCounterInfo;
	kDevVtbl.CheckCounter = Dev_CheckCounter;
	kDevVtbl.CheckFeatureSupport = Dev_CheckFeatureSupport;
	kDevVtbl.GetPrivateData = Dev_GetPD;
	kDevVtbl.SetPrivateData = Dev_SetPD;
	kDevVtbl.SetPrivateDataInterface = Dev_SetPDI;
	kDevVtbl.GetFeatureLevel = Dev_GetFeatureLevel;
	kDevVtbl.GetCreationFlags = Dev_GetCreationFlags;
	kDevVtbl.GetDeviceRemovedReason = Dev_GetDeviceRemovedReason;
	kDevVtbl.GetImmediateContext = Dev_GetImmediateContext;
	kDevVtbl.SetExceptionMode = Dev_SetExceptionMode;
	kDevVtbl.GetExceptionMode = Dev_GetExceptionMode;
	kDevVtbl.GetImmediateContext1 = Dev_GetImmediateContext1;
	kDevVtbl.CreateDeferredContext1 = Dev_CreateDeferredContext1;
	kDevVtbl.CreateBlendState1 = Dev_CreateBlendState1;
	kDevVtbl.CreateRasterizerState1 = Dev_CreateRasterizerState1;
	kDevVtbl.CreateDeviceContextState = Dev_CreateDeviceContextState;
	kDevVtbl.OpenSharedResource1 = Dev_OpenSharedResource1;
	kDevVtbl.OpenSharedResourceByName = Dev_OpenSharedResourceByName;

	VTBL_FILL(kCtxVtbl, ID3D11DeviceContext1Vtbl);
	kCtxVtbl.QueryInterface = Ctx_QI;
	kCtxVtbl.AddRef = Ctx_AddRef;
	kCtxVtbl.Release = Ctx_Release;
	kCtxVtbl.GetDevice = Ctx_GetDevice;
	kCtxVtbl.GetPrivateData = Ctx_GetPD;
	kCtxVtbl.SetPrivateData = Ctx_SetPD;
	kCtxVtbl.SetPrivateDataInterface = Ctx_SetPDI;
	kCtxVtbl.VSSetConstantBuffers = Ctx_VSSetConstantBuffers;
	kCtxVtbl.PSSetShaderResources = Ctx_PSSetShaderResources;
	kCtxVtbl.PSSetShader = Ctx_PSSetShader;
	kCtxVtbl.PSSetSamplers = Ctx_PSSetSamplers;
	kCtxVtbl.VSSetShader = Ctx_VSSetShader;
	kCtxVtbl.DrawIndexed = Ctx_DrawIndexed;
	kCtxVtbl.Draw = Ctx_Draw;
	kCtxVtbl.Map = Ctx_Map;
	kCtxVtbl.Unmap = Ctx_Unmap;
	kCtxVtbl.PSSetConstantBuffers = Ctx_PSSetConstantBuffers;
	kCtxVtbl.IASetInputLayout = Ctx_IASetInputLayout;
	kCtxVtbl.IASetVertexBuffers = Ctx_IASetVertexBuffers;
	kCtxVtbl.IASetIndexBuffer = Ctx_IASetIndexBuffer;
	kCtxVtbl.DrawIndexedInstanced = Ctx_DrawIndexedInstanced;
	kCtxVtbl.DrawInstanced = Ctx_DrawInstanced;
	kCtxVtbl.GSSetConstantBuffers = Ctx_GSSetConstantBuffers;
	kCtxVtbl.GSSetShader = Ctx_GSSetShader;
	kCtxVtbl.IASetPrimitiveTopology = Ctx_IASetPrimitiveTopology;
	kCtxVtbl.OMSetRenderTargets = Ctx_OMSetRenderTargets;
	kCtxVtbl.OMSetRenderTargetsAndUnorderedAccessViews = Ctx_OMSetRenderTargetsAndUAVs;
	kCtxVtbl.OMSetBlendState = Ctx_OMSetBlendState;
	kCtxVtbl.OMSetDepthStencilState = Ctx_OMSetDepthStencilState;
	kCtxVtbl.SOSetTargets = Ctx_SOSetTargets;
	kCtxVtbl.DrawAuto = Ctx_DrawAuto;
	kCtxVtbl.Dispatch = Ctx_Dispatch;
	kCtxVtbl.RSSetState = Ctx_RSSetState;
	kCtxVtbl.RSSetViewports = Ctx_RSSetViewports;
	kCtxVtbl.RSSetScissorRects = Ctx_RSSetScissorRects;
	kCtxVtbl.CopySubresourceRegion = Ctx_CopySubresourceRegion;
	kCtxVtbl.CopySubresourceRegion1 = Ctx_CopySubresourceRegion1;
	kCtxVtbl.CopyResource = Ctx_CopyResource;
	kCtxVtbl.UpdateSubresource = Ctx_UpdateSubresource;
	kCtxVtbl.UpdateSubresource1 = Ctx_UpdateSubresource1;
	kCtxVtbl.ClearRenderTargetView = Ctx_ClearRenderTargetView;
	kCtxVtbl.ClearUnorderedAccessViewUint = Ctx_ClearUAVUint;
	kCtxVtbl.ClearUnorderedAccessViewFloat = Ctx_ClearUAVFloat;
	kCtxVtbl.ClearDepthStencilView = Ctx_ClearDepthStencilView;
	kCtxVtbl.GenerateMips = Ctx_GenerateMips;
	kCtxVtbl.SetResourceMinLOD = Ctx_SetResourceMinLOD;
	kCtxVtbl.GetResourceMinLOD = Ctx_GetResourceMinLOD;
	kCtxVtbl.ResolveSubresource = Ctx_ResolveSubresource;
	kCtxVtbl.ExecuteCommandList = Ctx_ExecuteCommandList;
	kCtxVtbl.Begin = Ctx_Begin;
	kCtxVtbl.End = Ctx_End;
	kCtxVtbl.GetData = Ctx_GetData;
	kCtxVtbl.SetPredication = Ctx_SetPredication;
	kCtxVtbl.ClearState = Ctx_ClearState;
	kCtxVtbl.Flush = Ctx_Flush;
	kCtxVtbl.GetType = Ctx_GetType;
	kCtxVtbl.GetContextFlags = Ctx_GetContextFlags;
	kCtxVtbl.FinishCommandList = Ctx_FinishCommandList;
	kCtxVtbl.VSGetConstantBuffers = Ctx_VSGetConstantBuffers;
	kCtxVtbl.PSGetShaderResources = Ctx_PSGetShaderResources;
	kCtxVtbl.PSGetShader = Ctx_PSGetShader;
	kCtxVtbl.PSGetSamplers = Ctx_PSGetSamplers;
	kCtxVtbl.VSGetShader = Ctx_VSGetShader;
	kCtxVtbl.PSGetConstantBuffers = Ctx_PSGetConstantBuffers;
	kCtxVtbl.IAGetInputLayout = Ctx_IAGetInputLayout;
	kCtxVtbl.IAGetVertexBuffers = Ctx_IAGetVertexBuffers;
	kCtxVtbl.IAGetIndexBuffer = Ctx_IAGetIndexBuffer;
	kCtxVtbl.IAGetPrimitiveTopology = Ctx_IAGetPrimitiveTopology;
	kCtxVtbl.GSGetShader = Ctx_GSGetShader;
	kCtxVtbl.OMGetRenderTargets = Ctx_OMGetRenderTargets;
	kCtxVtbl.OMGetRenderTargetsAndUnorderedAccessViews = Ctx_OMGetRenderTargetsAndUAVs;
	kCtxVtbl.OMGetBlendState = Ctx_OMGetBlendState;
	kCtxVtbl.OMGetDepthStencilState = Ctx_OMGetDepthStencilState;
	kCtxVtbl.RSGetState = Ctx_RSGetState;
	kCtxVtbl.RSGetViewports = Ctx_RSGetViewports;
	kCtxVtbl.RSGetScissorRects = Ctx_RSGetScissorRects;
	kCtxVtbl.GetPredication = Ctx_GetPredication;
	kCtxVtbl.VSSetConstantBuffers1 = Ctx_VSSetConstantBuffers1;
	kCtxVtbl.PSSetConstantBuffers1 = Ctx_PSSetConstantBuffers1;

	/* VTBL_FILL, not a loop of hr_ni.
	 *
	 * hr_ni is declared WINAPI with one parameter, so it pops four bytes. The
	 * loop this replaces put it in every slot of the factory table, including
	 * the IDXGIFactory2 methods that take five and six arguments -
	 * CreateSwapChainForCoreWindow, RegisterOcclusionStatusWindow and the rest.
	 * On x86 stdcall the callee cleans the stack, so calling one of those would
	 * have unwound sixteen or twenty bytes too few and corrupted the caller's
	 * frame - a crash somewhere else entirely, with nothing pointing back here.
	 *
	 * kNiStub has an entry per argument count and kArity_IDXGIFactory2Vtbl
	 * already existed; this site simply was not using either. Every implemented
	 * method below still overwrites its slot, so nothing changes for the paths
	 * the game actually takes. */
	VTBL_FILL(kFactVtbl, IDXGIFactory2Vtbl);
	kFactVtbl.QueryInterface = Fact_QI;
	kFactVtbl.AddRef = Fact_AddRef;
	kFactVtbl.Release = Fact_Release;
	kFactVtbl.SetPrivateData = Fact_SetPrivateData;
	kFactVtbl.SetPrivateDataInterface = Fact_SetPrivateDataInterface;
	kFactVtbl.GetPrivateData = Fact_GetPrivateData;
	kFactVtbl.GetParent = Fact_GetParent;
	kFactVtbl.EnumAdapters = Fact_EnumAdapters;
	kFactVtbl.MakeWindowAssociation = Fact_MakeWindowAssociation;
	kFactVtbl.GetWindowAssociation = Fact_GetWindowAssociation;
	kFactVtbl.CreateSwapChain = Fact_CreateSwapChain;
	kFactVtbl.CreateSoftwareAdapter = Fact_CreateSoftwareAdapter;
	kFactVtbl.EnumAdapters1 = Fact_EnumAdapters1;
	kFactVtbl.IsCurrent = Fact_IsCurrent;
	kFactVtbl.IsWindowedStereoEnabled = Fact_IsWindowedStereoEnabled;
	kFactVtbl.CreateSwapChainForHwnd = Fact_CreateSwapChainForHwnd;

	memset(&kAdpVtbl, 0, sizeof(kAdpVtbl));
	kAdpVtbl.QueryInterface = Adp_QI;
	kAdpVtbl.AddRef = Adp_AddRef;
	kAdpVtbl.Release = Adp_Release;
	kAdpVtbl.SetPrivateData = Adp_SetPD;
	kAdpVtbl.SetPrivateDataInterface = Adp_SetPDI;
	kAdpVtbl.GetPrivateData = Adp_GetPD;
	kAdpVtbl.GetParent = Adp_GetParent;
	kAdpVtbl.EnumOutputs = Adp_EnumOutputs;
	kAdpVtbl.GetDesc = Adp_GetDesc;
	kAdpVtbl.CheckInterfaceSupport = Adp_CheckInterfaceSupport;
	kAdpVtbl.GetDesc1 = Adp_GetDesc1;

	memset(&kOutVtbl, 0, sizeof(kOutVtbl));
	kOutVtbl.QueryInterface = Out_QI;
	kOutVtbl.AddRef = Out_AddRef;
	kOutVtbl.Release = Out_Release;
	kOutVtbl.SetPrivateData = Out_SetPD;
	kOutVtbl.SetPrivateDataInterface = Out_SetPDI;
	kOutVtbl.GetPrivateData = Out_GetPD;
	kOutVtbl.GetParent = Out_GetParent;
	kOutVtbl.GetDesc = Out_GetDesc;
	kOutVtbl.GetDisplayModeList = Out_GetDisplayModeList;
	kOutVtbl.FindClosestMatchingMode = Out_FindClosestMatchingMode;
	kOutVtbl.WaitForVBlank = Out_WaitForVBlank;
	kOutVtbl.TakeOwnership = Out_TakeOwnership;
	kOutVtbl.ReleaseOwnership = Out_ReleaseOwnership;
	kOutVtbl.GetGammaControlCapabilities = Out_GetGammaControlCapabilities;
	kOutVtbl.SetGammaControl = Out_SetGammaControl;
	kOutVtbl.GetGammaControl = Out_GetGammaControl;
	kOutVtbl.SetDisplaySurface = Out_SetDisplaySurface;
	kOutVtbl.GetDisplaySurfaceData = Out_GetDisplaySurfaceData;
	kOutVtbl.GetFrameStatistics = Out_GetFrameStatistics;

	/* Same correction as the factory table above: arity-correct stubs rather
	 * than a one-argument one in every slot. */
	VTBL_FILL(kSwapVtbl, IDXGISwapChain1Vtbl);
	kSwapVtbl.QueryInterface = Swap_QI;
	kSwapVtbl.AddRef = Swap_AddRef;
	kSwapVtbl.Release = Swap_Release;
	kSwapVtbl.SetPrivateData = Swap_SetPD;
	kSwapVtbl.SetPrivateDataInterface = Swap_SetPDI;
	kSwapVtbl.GetPrivateData = Swap_GetPD;
	kSwapVtbl.GetParent = Swap_GetParent;
	kSwapVtbl.GetDevice = Swap_GetDevice;
	kSwapVtbl.Present = Swap_Present;
	kSwapVtbl.GetBuffer = Swap_GetBuffer;
	kSwapVtbl.SetFullscreenState = Swap_SetFullscreenState;
	kSwapVtbl.GetFullscreenState = Swap_GetFullscreenState;
	kSwapVtbl.GetDesc = Swap_GetDesc;
	kSwapVtbl.ResizeBuffers = Swap_ResizeBuffers;
	kSwapVtbl.ResizeTarget = Swap_ResizeTarget;
	kSwapVtbl.GetContainingOutput = Swap_GetContainingOutput;
	kSwapVtbl.GetFrameStatistics = Swap_GetFrameStatistics;
	kSwapVtbl.GetLastPresentCount = Swap_GetLastPresentCount;
	kSwapVtbl.GetDesc1 = Swap_GetDesc1;
	kSwapVtbl.GetFullscreenDesc = Swap_GetFullscreenDesc;
	kSwapVtbl.GetHwnd = Swap_GetHwnd;
	kSwapVtbl.GetCoreWindow = Swap_GetCoreWindow;
	kSwapVtbl.Present1 = Swap_Present1;
	kSwapVtbl.IsTemporaryMonoSupported = Swap_IsTemporaryMonoSupported;
	kSwapVtbl.GetRestrictToOutput = Swap_GetRestrictToOutput;
	kSwapVtbl.SetBackgroundColor = Swap_SetBackgroundColor;
	kSwapVtbl.GetBackgroundColor = Swap_GetBackgroundColor;
	kSwapVtbl.SetRotation = Swap_SetRotation;
	kSwapVtbl.GetRotation = Swap_GetRotation;

	kDxgiDevVtbl.QueryInterface = DxgiDev_QI;
	kDxgiDevVtbl.AddRef = DxgiDev_AddRef;
	kDxgiDevVtbl.Release = DxgiDev_Release;
	kDxgiDevVtbl.SetPrivateData = DxgiDev_SetPD;
	kDxgiDevVtbl.SetPrivateDataInterface = DxgiDev_SetPDI;
	kDxgiDevVtbl.GetPrivateData = DxgiDev_GetPD;
	kDxgiDevVtbl.GetParent = DxgiDev_GetParent;
	kDxgiDevVtbl.GetAdapter = DxgiDev_GetAdapter;
	kDxgiDevVtbl.CreateSurface = DxgiDev_CreateSurface;
	kDxgiDevVtbl.QueryResourceResidency = DxgiDev_QueryResourceResidency;
	kDxgiDevVtbl.SetGPUThreadPriority = DxgiDev_SetGPUThreadPriority;
	kDxgiDevVtbl.GetGPUThreadPriority = DxgiDev_GetGPUThreadPriority;
	kDxgiDevVtbl.SetMaximumFrameLatency = DxgiDev_SetMaximumFrameLatency;
	kDxgiDevVtbl.GetMaximumFrameLatency = DxgiDev_GetMaximumFrameLatency;

	kTexVtbl.QueryInterface = Tex_QI;
	kTexVtbl.AddRef = Tex_AddRef;
	kTexVtbl.Release = Tex_Release;
	kTexVtbl.GetDevice = Tex_GetDevice;
	kTexVtbl.GetPrivateData = Tex_GetPD;
	kTexVtbl.SetPrivateData = Tex_SetPD;
	kTexVtbl.SetPrivateDataInterface = Tex_SetPDI;
	kTexVtbl.GetType = Tex_GetType;
	kTexVtbl.SetEvictionPriority = Tex_SetEvict;
	kTexVtbl.GetEvictionPriority = Tex_GetEvict;
	kTexVtbl.GetDesc = Tex_GetDesc;

	kBufVtbl.QueryInterface = Buf_QI;
	kBufVtbl.AddRef = Buf_AddRef;
	kBufVtbl.Release = Buf_Release;
	kBufVtbl.GetDevice = Buf_GetDevice;
	kBufVtbl.GetPrivateData = Buf_GetPD;
	kBufVtbl.SetPrivateData = Buf_SetPD;
	kBufVtbl.SetPrivateDataInterface = Buf_SetPDI;
	kBufVtbl.GetType = Buf_GetType;
	kBufVtbl.SetEvictionPriority = Buf_SetEvict;
	kBufVtbl.GetEvictionPriority = Buf_GetEvict;
	kBufVtbl.GetDesc = Buf_GetDesc;

	kRtvVtbl.QueryInterface = (void *)Rtv_QI;
	kRtvVtbl.AddRef = (void *)View_AddRef;
	kRtvVtbl.Release = (void *)View_Release;
	kRtvVtbl.GetDevice = (void *)View_GetDevice;
	kRtvVtbl.GetPrivateData = (void *)View_GetPD;
	kRtvVtbl.SetPrivateData = (void *)View_SetPD;
	kRtvVtbl.SetPrivateDataInterface = (void *)View_SetPDI;
	kRtvVtbl.GetResource = (void *)View_GetResource;
	kRtvVtbl.GetDesc = Rtv_GetDesc;

	kSrvVtbl.QueryInterface = (void *)Srv_QI;
	kSrvVtbl.AddRef = (void *)View_AddRef;
	kSrvVtbl.Release = (void *)View_Release;
	kSrvVtbl.GetDevice = (void *)View_GetDevice;
	kSrvVtbl.GetPrivateData = (void *)View_GetPD;
	kSrvVtbl.SetPrivateData = (void *)View_SetPD;
	kSrvVtbl.SetPrivateDataInterface = (void *)View_SetPDI;
	kSrvVtbl.GetResource = (void *)View_GetResource;
	kSrvVtbl.GetDesc = Srv_GetDesc;

	kDsvVtbl.QueryInterface = (void *)Dsv_QI;
	kDsvVtbl.AddRef = (void *)View_AddRef;
	kDsvVtbl.Release = (void *)View_Release;
	kDsvVtbl.GetDevice = (void *)View_GetDevice;
	kDsvVtbl.GetPrivateData = (void *)View_GetPD;
	kDsvVtbl.SetPrivateData = (void *)View_SetPD;
	kDsvVtbl.SetPrivateDataInterface = (void *)View_SetPDI;
	kDsvVtbl.GetResource = (void *)View_GetResource;
	kDsvVtbl.GetDesc = Dsv_GetDesc;

	kVSVtbl.QueryInterface = (void *)Sh_QI;
	kVSVtbl.AddRef = (void *)Sh_AddRef;
	kVSVtbl.Release = (void *)Sh_Release;
	kVSVtbl.GetDevice = (void *)Sh_GetDevice;
	kVSVtbl.GetPrivateData = (void *)Sh_GetPD;
	kVSVtbl.SetPrivateData = (void *)Sh_SetPD;
	kVSVtbl.SetPrivateDataInterface = (void *)Sh_SetPDI;

	kPSVtbl.QueryInterface = (void *)Sh_QI;
	kPSVtbl.AddRef = (void *)Sh_AddRef;
	kPSVtbl.Release = (void *)Sh_Release;
	kPSVtbl.GetDevice = (void *)Sh_GetDevice;
	kPSVtbl.GetPrivateData = (void *)Sh_GetPD;
	kPSVtbl.SetPrivateData = (void *)Sh_SetPD;
	kPSVtbl.SetPrivateDataInterface = (void *)Sh_SetPDI;

	kLayVtbl.QueryInterface = (void *)Lay_QI;
	kLayVtbl.AddRef = (void *)Lay_AddRef;
	kLayVtbl.Release = (void *)Lay_Release;
	kLayVtbl.GetDevice = (void *)Lay_GetDevice;
	kLayVtbl.GetPrivateData = (void *)Lay_GetPD;
	kLayVtbl.SetPrivateData = (void *)Lay_SetPD;
	kLayVtbl.SetPrivateDataInterface = (void *)Lay_SetPDI;

	kBlendVtbl.QueryInterface = (void *)Blend_QI;
	kBlendVtbl.AddRef = (void *)Blend_AddRef;
	kBlendVtbl.Release = (void *)Blend_Release;
	kBlendVtbl.GetDevice = (void *)Blend_GetDevice;
	kBlendVtbl.GetPrivateData = (void *)Blend_GetPD;
	kBlendVtbl.SetPrivateData = (void *)Blend_SetPD;
	kBlendVtbl.SetPrivateDataInterface = (void *)Blend_SetPDI;
	kBlendVtbl.GetDesc = Blend_GetDesc;

	kDSVtbl.QueryInterface = (void *)Dss_QI;
	kDSVtbl.AddRef = (void *)Dss_AddRef;
	kDSVtbl.Release = (void *)Dss_Release;
	kDSVtbl.GetDevice = (void *)Dss_GetDevice;
	kDSVtbl.GetPrivateData = (void *)Dss_GetPD;
	kDSVtbl.SetPrivateData = (void *)Dss_SetPD;
	kDSVtbl.SetPrivateDataInterface = (void *)Dss_SetPDI;
	kDSVtbl.GetDesc = Dss_GetDesc;

	kRastVtbl.QueryInterface = (void *)Rast_QI;
	kRastVtbl.AddRef = (void *)Rast_AddRef;
	kRastVtbl.Release = (void *)Rast_Release;
	kRastVtbl.GetDevice = (void *)Rast_GetDevice;
	kRastVtbl.GetPrivateData = (void *)Rast_GetPD;
	kRastVtbl.SetPrivateData = (void *)Rast_SetPD;
	kRastVtbl.SetPrivateDataInterface = (void *)Rast_SetPDI;
	kRastVtbl.GetDesc = Rast_GetDesc;

	kSampVtbl.QueryInterface = (void *)Samp_QI;
	kSampVtbl.AddRef = (void *)Samp_AddRef;
	kSampVtbl.Release = (void *)Samp_Release;
	kSampVtbl.GetDevice = (void *)Samp_GetDevice;
	kSampVtbl.GetPrivateData = (void *)Samp_GetPD;
	kSampVtbl.SetPrivateData = (void *)Samp_SetPD;
	kSampVtbl.SetPrivateDataInterface = (void *)Samp_SetPDI;
	kSampVtbl.GetDesc = Samp_GetDesc;

	kQueryVtbl.QueryInterface = (void *)Query_QI;
	kQueryVtbl.AddRef = (void *)Query_AddRef;
	kQueryVtbl.Release = (void *)Query_Release;
	kQueryVtbl.GetDevice = (void *)Query_GetDevice;
	kQueryVtbl.GetPrivateData = (void *)Query_GetPD;
	kQueryVtbl.SetPrivateData = (void *)Query_SetPD;
	kQueryVtbl.SetPrivateDataInterface = (void *)Query_SetPDI;
	kQueryVtbl.GetDataSize = Query_GetDataSize;
	kQueryVtbl.GetDesc = Query_GetDesc;

	g_vtbl_ready = 1;
}

static const struct vtbl_name g_vtbl_names[] = {
	{ "ID3D11Device", &kDevVtbl },        { "ID3D11DeviceContext", &kCtxVtbl },
	{ "IDXGIFactory", &kFactVtbl },       { "IDXGIAdapter", &kAdpVtbl },
	{ "IDXGIOutput", &kOutVtbl },         { "IDXGISwapChain", &kSwapVtbl },
	{ "IDXGIDevice", &kDxgiDevVtbl },     { "ID3D11Texture2D", &kTexVtbl },
	{ "ID3D11Buffer", &kBufVtbl },        { "ID3D11RenderTargetView", &kRtvVtbl },
	{ "ID3D11ShaderResourceView", &kSrvVtbl },
	{ "ID3D11DepthStencilView", &kDsvVtbl },
	{ "ID3D11VertexShader", &kVSVtbl },   { "ID3D11PixelShader", &kPSVtbl },
	{ "ID3D11InputLayout", &kLayVtbl },   { "ID3D11BlendState", &kBlendVtbl },
	{ "ID3D11DepthStencilState", &kDSVtbl },
	{ "ID3D11RasterizerState", &kRastVtbl },
	{ "ID3D11SamplerState", &kSampVtbl }, { "ID3D11Query", &kQueryVtbl },
	{ NULL, NULL },
};

