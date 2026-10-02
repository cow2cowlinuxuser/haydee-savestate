#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savestate.h"
#include "swalloc.h"
#include "swrast.h"
#include "glsl.h"
#include "gl_fwd.h"

#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

/* Software OpenGL drop-in so Haydee 1 can rewind the same way the D3D
 * devices do: every texture and target is process memory, SwapBuffers is
 * the Present seam. GLSL is stored, not executed; draws feed attrib 0
 * through the same clip-space path D3D11 already uses. Compute shaders
 * are accepted and ignored. PhysX is not our problem — this build of
 * Haydee already ships the CPU libraries. */

PROC gl_lookup_proc(const char *name);

#define GL_TEXUNITS 32
#define GL_ATTRS 16
#define GL_UBOS 16
#define GL_IMM_MAX 4096
#define GL_EXT_COUNT 14
#define GL_HOST_EXT "GL_EXT_texture_filter_anisotropic"

static const char *k_exts[GL_EXT_COUNT] = {
	"GL_ARB_framebuffer_object",
	"GL_ARB_vertex_buffer_object",
	"GL_ARB_vertex_array_object",
	"GL_ARB_shader_objects",
	"GL_ARB_shading_language_100",
	"GL_ARB_map_buffer_range",
	"GL_ARB_uniform_buffer_object",
	"GL_ARB_sampler_objects",
	"GL_ARB_instanced_arrays",
	"GL_ARB_draw_instanced",
	"GL_ARB_texture_storage",
	"GL_ARB_sync",
	"GL_ARB_compute_shader",
	"GL_EXT_texture_compression_s3tc",
};

typedef struct GlTex {
	GLuint id;
	GLenum target;
	int w, h;
	uint32_t *pixels;
	int has;
	float *z; /* depth values when attached as an FBO depth target */
	/* last level-0 upload, for the trace */
	const char *up_fn;
	GLenum up_int, up_fmt, up_type;
	int up_dropped; /* the data was in a layout we could not convert */
	/* Render scale: a window-sized render target stored at rs times the size
	 * the game asked for (vw x vh). 0 = stored as asked. */
	float rs;
	int vw, vh;
} GlTex;

typedef struct GlBuf {
	GLuint id;
	unsigned char *data;
	size_t size;
	int mapped;
} GlBuf;

typedef struct GlAttrib {
	int enabled;
	GLint size;
	GLenum type;
	GLsizei stride;
	GLsizeiptr offset;
	GLuint buf;
	int normalized;
	GLuint divisor;
} GlAttrib;

typedef struct GlVao {
	GLuint id;
	GlAttrib attr[GL_ATTRS];
	GLuint ebo;
} GlVao;

typedef struct GlShader {
	GLuint id;
	GLenum type;
	char *src;
	int doomed; /* glDeleteShader while attached: freed once nothing holds it */
} GlShader;

/* Uniform storage by name. A location encodes (slot << UNI_ELEM_BITS) | element,
 * so "joints[5]" lands on element 5 of "joints". Every element is kept as floats
 * (ints converted), nc floats per element, matrices column-major; the draw pushes
 * the whole array into the interpreter. m/is_mat4 mirror element 0 of a mat4 for
 * the forward-hack's pass detection. */
#define UNI_ELEM_BITS 10
#define UNI_ELEM_MASK ((1 << UNI_ELEM_BITS) - 1)
typedef struct GlUniform {
	char name[48];
	int set;      /* a value has been stored */
	int is_mat4;  /* m[16] is a 4x4 */
	float m[16];
	int ivalue;   /* element 0 as an int - a sampler's texture unit */
	float *v;
	int nc;       /* floats per element */
	int nelem;    /* elements stored */
} GlUniform;

#define PROG_MAX_IN 16
#define PROG_MAX_TF 8
typedef struct GlProg {
	GLuint id;
	int linked;
	GlUniform uni[96];
	int nuni;
	/* Attached shader ids (recorded by glAttachShader) and the interpreter
	 * programs compiled from their source at glLinkProgram - the shaded raster
	 * path runs these instead of the fixed-function transform. */
	GLuint vs_id, fs_id, gs_id;
	GlslProg *vs, *fs;
	int compile_failed;
	/* glBindAttribLocation, applied when inputs are resolved */
	char bind_name[PROG_MAX_IN][48];
	int bind_loc[PROG_MAX_IN];
	int nbind;
	/* Vertex inputs resolved to locations, and the outputs the fixed-function
	 * fragment stage consumes. Built lazily after link (io_ready). Names are
	 * owned by vs. */
	int io_ready;
	int nin;
	const char *in_name[PROG_MAX_IN];
	int in_loc[PROG_MAX_IN], in_n[PROG_MAX_IN];
	const char *out_uv, *out_col;
	int fs_unit_uni;   /* uniform slot of the FS's colour sampler, -1 if none */
	int fs_out_loc;    /* location of the FS's colour output (its attachment) */
	int fs_out_diffuse; /* that output is a G-buffer diffuse/albedo target */
	int fs_no_tex;      /* the FS declares no sampler: draw untextured */
	int fs_color_uni;   /* slot of a vec3/vec4 colour uniform the FS applies, -1 if none */
	/* glTransformFeedbackVaryings */
	char tf_name[PROG_MAX_TF][48];
	int ntf;
	GLenum tf_mode;
} GlProg;

typedef struct GlFbo {
	GLuint id;
	GLuint color;
	GLenum color_tgt;
	GLuint depth;
	GLenum depth_tgt;
	GLuint att[8]; /* COLOR_ATTACHMENTn */
	unsigned bufs, bufs_set; /* glDrawBuffers: bit n = COLOR_ATTACHMENTn drawn */
} GlFbo;

typedef struct GlRbo {
	GLuint id;
	int w, h;
	uint32_t *color;
	float *depth;
} GlRbo;

typedef struct GlSamp {
	GLuint id;
	int mag, min_f;
} GlSamp;

typedef struct GlShare {
	LONG ref;
	GlTex **tex;
	int ntex, ctex;
	GlBuf **buf;
	int nbuf, cbuf;
	GlVao **vao;
	int nvao, cvao;
	GlShader **sh;
	int nsh, csh;
	GlProg **prog;
	int nprog, cprog;
	GlFbo **fbo;
	int nfbo, cfbo;
	GlRbo **rbo;
	int nrbo, crbo;
	GlSamp **samp;
	int nsamp, csamp;
	GLuint next_id;
	GlVao *vao0;
} GlShare;

typedef struct GlCtx {
	HDC hdc;
	HWND hwnd;
	GlShare *share;
	SwRast fb;
	GLuint draw_fbo, read_fbo;
	GLuint vao, prog;
	GLuint texunit;
	GLuint tex2d[GL_TEXUNITS];
	GLuint buf_array, buf_element, buf_uniform[GL_UBOS];
	GLuint buf_tf, buf_copy_r, buf_copy_w, buf_misc;
	GLuint sampler[GL_TEXUNITS];
	/* transform feedback: indexed bindings, write cursors, active flag */
	GLuint tf_buf[4];
	size_t tf_base[4], tf_off[4];
	int tf_active;
	int vp[4], scissor[4];
	int en_blend, en_depth, en_cull, en_scissor, en_tex2d, en_discard;
	/* The current target stores rows bottom-up (an FBO texture, GL's layout)
	 * rather than top-down (the window). Set per draw. */
	int rt_up;
	/* Colour attachment a draw writes (the fragment shader's colour output);
	 * 0 outside a draw. */
	int out_att;
	/* glVertexAttrib*: the value a disabled attribute array supplies */
	float cur_attr[GL_ATTRS][4];
	/* render scale of the target bind_draw_target / bind_read_target last bound:
	 * multiplies the game's pixel coordinates into stored pixels */
	float rt_rs, read_rs;
	/* Stencil is not emulated; this only spots passes that exist to write it. */
	int en_stencil;
	int sten_writes[2]; /* front, back: some stencil op is not GL_KEEP */
	GLuint sten_mask;
	GLenum blend_src, blend_dst, blend_op;
	GLenum depth_func, cull_face, front_face;
	GLboolean depth_mask, color_mask[4];
	float clear_c[4];
	double clear_z;
	int unpack_align;
	int in_begin;
	GLenum begin_mode;
	float cur_u, cur_v;
	uint32_t cur_color;
	/* Forward-hack: MVP for the current draw when it is the 3D model geometry
	 * pass (proj * localView), and a flag saying so. Off for 2D/UI/deferred. */
	float model_mvp[16];
	int model_mvp_active;
	SwVert imm[GL_IMM_MAX];
	int nimm;
	GLenum err;
} GlCtx;

/* Current context is per-thread. Haydee spins a pile of 1x1 loader
 * contexts on worker threads; a process-global current made every
 * SwapBuffers present a 1x1 hidden window while the 2560x1334 one sat
 * idle. */
static __thread GlCtx *g_cur;
static GlCtx *g_all[64];
static int g_nctx;
static CRITICAL_SECTION g_ctx_lock;
static LONG g_lock_ready;
static LONG g_present_n;
static LONG g_makecurrent_n;
static LONG g_frame_draws;
static LONG g_frame_tris;
static LONG g_frame_skip;
static LONG g_frame_vs;
static LONGLONG g_frame_vs_qpc, g_frame_tf_qpc;
#define VS_CACHE 4096 /* per-draw shaded-vertex cache, power of two */
/* The size the game renders the window at (its full-window glViewport on the
 * default framebuffer). The window buffer takes this size and present scales it
 * to the client area, so the game's resolution is never second-guessed. */
static int g_win_w, g_win_h;
/* Render scale: the window buffer and window-sized render targets are stored
 * at GLSW_RENDER_H lines (default 720, 0 = native) while the game keeps
 * addressing them at full size. */
static int g_render_h = -1;
static float g_rs = 1.0f;
static void gl_log(const char *fmt, ...);
unsigned savestate_getenv(const char *name, char *buf, unsigned cap);

static void rs_update(int win_h)
{
	float rs;
	if (g_render_h < 0) {
		char e[16];
		unsigned n = savestate_getenv("GLSW_RENDER_H", e, sizeof(e));
		g_render_h = n && n < sizeof(e) ? atoi(e) : 720;
	}
	rs = (g_render_h > 0 && win_h > g_render_h) ? (float)g_render_h / (float)win_h : 1.0f;
	if (rs != g_rs)
		gl_log("render scale %.3f: %d-line window rendered at %d lines", rs, win_h,
		       (int)(win_h * rs + 0.5f));
	g_rs = rs;
}

static int rs_px(int v, float rs)
{
	return (int)((float)v * rs + (v >= 0 ? 0.5f : -0.5f));
}
/* One-frame trace state; see tr_frame_end. */
static int g_tr, g_tr_frames, g_tr_done;
static GLuint g_tr_tex[48];
static GLuint g_tr_samp[24]; /* textures sampled during the traced frame */
static int g_tr_nsamp;
static int g_tr_ntex;
static int g_swap_interval;
static GlShare *g_share;

static void ctx_lock_enter(void)
{
	if (g_lock_ready < 2) {
		if (InterlockedCompareExchange(&g_lock_ready, 1, 0) == 0) {
			InitializeCriticalSection(&g_ctx_lock);
			InterlockedExchange(&g_lock_ready, 2);
		} else {
			while (g_lock_ready < 2)
				Sleep(0);
		}
	}
	EnterCriticalSection(&g_ctx_lock);
}

static int trace_on(void)
{
	static int v = -1;
	if (v < 0) {
		const char *s = getenv("GLSW_TRACE");
		v = (s && *s && *s != '0') ? 1 : 0;
	}
	return v;
}

static void gl_vlog(const char *fmt, va_list ap)
{
	static volatile LONG n;
	char line[640];
	FILE *f;
	if (InterlockedIncrement(&n) > 20000)
		return;
	_vsnprintf(line, sizeof(line), fmt, ap);
	line[sizeof(line) - 1] = 0;
	OutputDebugStringA("[gl_sw] ");
	OutputDebugStringA(line);
	OutputDebugStringA("\n");
	f = fopen("gl_sw.log", "a");
	if (f) {
		fprintf(f, "%s\n", line);
		fflush(f);
		fclose(f);
	}
}

static void gl_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	gl_vlog(fmt, ap);
	va_end(ap);
}

/* Append a string to the log verbatim, uncapped and unformatted - for shader
 * source, which is far longer than gl_log's 640-char line buffer and would
 * otherwise be truncated. */
static void gl_log_raw(const char *s)
{
	FILE *f = fopen("gl_sw.log", "a");
	if (f) {
		fputs(s, f);
		fputc('\n', f);
		fclose(f);
	}
}

/* Trace line, only while a frame is being traced; uncapped like shader dumps. */
static void tr(const char *fmt, ...)
{
	char line[768];
	va_list ap;
	if (!g_tr)
		return;
	line[0] = 'T';
	line[1] = ' ';
	va_start(ap, fmt);
	_vsnprintf(line + 2, sizeof(line) - 2, fmt, ap);
	va_end(ap);
	line[sizeof(line) - 1] = 0;
	gl_log_raw(line);
}

static void tr_touch(GLuint tex)
{
	int i;
	if (!g_tr || !tex)
		return;
	for (i = 0; i < g_tr_ntex; i++)
		if (g_tr_tex[i] == tex)
			return;
	if (g_tr_ntex < (int)(sizeof(g_tr_tex) / sizeof(g_tr_tex[0])))
		g_tr_tex[g_tr_ntex++] = tex;
}

static void tr_sampled(GLuint tex)
{
	int i;
	if (!g_tr || !tex)
		return;
	for (i = 0; i < g_tr_nsamp; i++)
		if (g_tr_samp[i] == tex)
			return;
	if (g_tr_nsamp < (int)(sizeof(g_tr_samp) / sizeof(g_tr_samp[0])))
		g_tr_samp[g_tr_nsamp++] = tex;
}

/* Remember how a texture's level 0 arrived; log each new upload layout once. */
static void tex_note(GlTex *t, const char *fn, GLenum internal, GLenum fmt, GLenum type,
		     int dropped)
{
	static struct {
		const char *fn;
		GLenum i, f, t;
	} seen[64];
	static int nseen;
	int k;
	t->up_fn = fn;
	t->up_int = internal;
	t->up_fmt = fmt;
	t->up_type = type;
	t->up_dropped = dropped;
	for (k = 0; k < nseen; k++)
		if (seen[k].fn == fn && seen[k].i == internal && seen[k].f == fmt && seen[k].t == type)
			return;
	if (nseen < 64) {
		seen[nseen].fn = fn;
		seen[nseen].i = internal;
		seen[nseen].f = fmt;
		seen[nseen].t = type;
		nseen++;
		gl_log("texture upload layout: %s internal 0x%X format 0x%X type 0x%X (tex %u %dx%d)%s",
		       fn, internal, fmt, type, t->id, t->w, t->h, dropped ? " NOT CONVERTED" : "");
	}
}

/* 24-bit BMP of a colour buffer, every step-th pixel. up: memory rows run
 * bottom-up (FBO textures) rather than top-down (the window). */
static void bmp_write(const char *path, const uint32_t *px, int w, int h, int up, int step)
{
	BITMAPFILEHEADER fh;
	BITMAPINFOHEADER ih;
	int ow = w / step, oh = h / step, row = (ow * 3 + 3) & ~3, x, y;
	unsigned char *line;
	FILE *f;

	if (!px || ow < 1 || oh < 1)
		return;
	f = fopen(path, "wb");
	if (!f)
		return;
	line = (unsigned char *)calloc(1, (size_t)row);
	memset(&fh, 0, sizeof(fh));
	memset(&ih, 0, sizeof(ih));
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(ih);
	fh.bfSize = fh.bfOffBits + (DWORD)row * oh;
	ih.biSize = sizeof(ih);
	ih.biWidth = ow;
	ih.biHeight = oh;
	ih.biPlanes = 1;
	ih.biBitCount = 24;
	fwrite(&fh, sizeof(fh), 1, f);
	fwrite(&ih, sizeof(ih), 1, f);
	for (y = 0; line && y < oh; y++) {
		int sy = up ? y * step : h - 1 - y * step;
		const uint32_t *s = px + (size_t)sy * w;
		for (x = 0; x < ow; x++) {
			uint32_t p = s[x * step];
			line[x * 3 + 0] = (unsigned char)(p & 0xff);
			line[x * 3 + 1] = (unsigned char)((p >> 8) & 0xff);
			line[x * 3 + 2] = (unsigned char)((p >> 16) & 0xff);
		}
		fwrite(line, 1, (size_t)row, f);
	}
	free(line);
	fclose(f);
}

static void gl_trace(const char *fmt, ...)
{
	va_list ap;
	if (!trace_on())
		return;
	va_start(ap, fmt);
	gl_vlog(fmt, ap);
	va_end(ap);
}

void gl_ni(const char *name)
{
	static char seen[160][64];
	static int n;
	int i;
	for (i = 0; i < n; i++)
		if (strcmp(seen[i], name) == 0)
			return;
	if (n < 160) {
		strncpy(seen[n], name, 63);
		seen[n][63] = 0;
		n++;
	}
	gl_log("NI %s", name);
}

static GlCtx *cur(void)
{
	return g_cur;
}

static void ctx_lock_leave(void)
{
	LeaveCriticalSection(&g_ctx_lock);
}

static void ctx_register(GlCtx *c)
{
	ctx_lock_enter();
	if (g_nctx < (int)(sizeof(g_all) / sizeof(g_all[0])))
		g_all[g_nctx++] = c;
	ctx_lock_leave();
}

static void ctx_unregister(GlCtx *c)
{
	int i;
	ctx_lock_enter();
	for (i = 0; i < g_nctx; i++)
		if (g_all[i] == c) {
			g_all[i] = g_all[g_nctx - 1];
			g_nctx--;
			break;
		}
	ctx_lock_leave();
}

/* WindowFromDC, but only for a window of this process. A DC handle restored from
 * another launch can name some other process's DC, and presenting into that
 * window breaks the host's swapchain for good. */
static HWND own_window_from_dc(HDC hdc)
{
	HWND h = hdc ? WindowFromDC(hdc) : NULL;
	DWORD pid = 0;

	if (h && (!GetWindowThreadProcessId(h, &pid) || pid != GetCurrentProcessId()))
		return NULL;
	return h;
}

static BOOL CALLBACK own_visible_cb(HWND h, LPARAM lp)
{
	HWND *best = (HWND *)lp;
	DWORD pid = 0;
	RECT a, b;

	GetWindowThreadProcessId(h, &pid);
	if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || !GetClientRect(h, &a))
		return TRUE;
	if (!*best || (GetClientRect(*best, &b) &&
		       (a.right * a.bottom) > (b.right * b.bottom)))
		*best = h;
	return TRUE;
}

/* A window handle restored from another launch names nothing here (or something
 * else); this process's largest visible window is where the game is shown. */
static HWND own_live_window(HWND h)
{
	DWORD pid = 0;
	HWND best = NULL;

	if (h && IsWindow(h) && GetWindowThreadProcessId(h, &pid) &&
	    pid == GetCurrentProcessId())
		return h;
	EnumWindows(own_visible_cb, (LPARAM)&best);
	if (best) {
		static volatile LONG said;
		if (InterlockedIncrement(&said) <= 8)
			gl_log("present: window %p is gone, presenting to this process's window %p",
			       (void *)h, (void *)best);
	}
	return best ? best : h;
}

static GlCtx *ctx_for_hwnd(HWND hwnd)
{
	GlCtx *best = NULL;
	int i, area, best_area = 0;
	if (!hwnd)
		return NULL;
	ctx_lock_enter();
	for (i = 0; i < g_nctx; i++) {
		GlCtx *c = g_all[i];
		if (!c || c->hwnd != hwnd)
			continue;
		area = c->fb.width * c->fb.height;
		if (!best || area > best_area) {
			best = c;
			best_area = area;
		}
	}
	ctx_lock_leave();
	return best;
}

static GlCtx *ctx_largest(void)
{
	GlCtx *best = NULL;
	int i, area, best_area = 0;
	ctx_lock_enter();
	for (i = 0; i < g_nctx; i++) {
		GlCtx *c = g_all[i];
		int cw, ch;
		RECT rc;
		if (!c)
			continue;
		cw = c->fb.width;
		ch = c->fb.height;
		if (c->hwnd && GetClientRect(c->hwnd, &rc)) {
			int ww = rc.right - rc.left;
			int wh = rc.bottom - rc.top;
			if (ww * wh > cw * ch) {
				cw = ww;
				ch = wh;
			}
		}
		area = cw * ch;
		if (area > best_area) {
			best = c;
			best_area = area;
		}
	}
	ctx_lock_leave();
	return best;
}

static int hwnd_area(HWND hwnd)
{
	RECT rc;
	if (!hwnd || !GetClientRect(hwnd, &rc))
		return 0;
	return (rc.right - rc.left) * (rc.bottom - rc.top);
}

static void set_err(GLenum e)
{
	GlCtx *c = cur();
	if (c && !c->err)
		c->err = e;
}

static GLuint alloc_id(GlShare *s)
{
	if (!s->next_id)
		s->next_id = 1;
	return s->next_id++;
}

static void *grow(void **arr, int *n, int *cap, size_t elem)
{
	if (*n >= *cap) {
		int nc = *cap ? *cap * 2 : 32;
		void *p = realloc(*arr, (size_t)nc * elem);
		if (!p)
			return NULL;
		memset((char *)p + (size_t)*cap * elem, 0, (size_t)(nc - *cap) * elem);
		*arr = p;
		*cap = nc;
	}
	return (char *)*arr + (size_t)(*n) * elem;
}

static GlShare *share_new(void)
{
	GlShare *s = (GlShare *)calloc(1, sizeof(*s));
	if (s) {
		s->ref = 1;
		s->vao0 = (GlVao *)calloc(1, sizeof(GlVao));
	}
	return s;
}

static GlShare *share_global(void)
{
	if (!g_share)
		g_share = share_new();
	if (g_share)
		InterlockedIncrement(&g_share->ref);
	return g_share;
}

static void tex_free(GlTex *t)
{
	if (!t)
		return;
	free(t->pixels);
	free(t->z);
	free(t);
}

static void buf_free(GlBuf *b)
{
	if (!b)
		return;
	free(b->data);
	free(b);
}

static void share_release(GlShare *s)
{
	int i;
	if (!s || InterlockedDecrement(&s->ref) > 0)
		return;
	for (i = 0; i < s->ntex; i++)
		tex_free(s->tex[i]);
	for (i = 0; i < s->nbuf; i++)
		buf_free(s->buf[i]);
	for (i = 0; i < s->nvao; i++)
		free(s->vao[i]);
	for (i = 0; i < s->nsh; i++) {
		if (s->sh[i])
			free(s->sh[i]->src);
		free(s->sh[i]);
	}
	for (i = 0; i < s->nprog; i++)
		free(s->prog[i]);
	for (i = 0; i < s->nfbo; i++)
		free(s->fbo[i]);
	for (i = 0; i < s->nrbo; i++) {
		if (s->rbo[i]) {
			free(s->rbo[i]->color);
			free(s->rbo[i]->depth);
		}
		free(s->rbo[i]);
	}
	for (i = 0; i < s->nsamp; i++)
		free(s->samp[i]);
	free(s->tex);
	free(s->buf);
	free(s->vao);
	free(s->sh);
	free(s->prog);
	free(s->fbo);
	free(s->rbo);
	free(s->samp);
	free(s->vao0);
	if (g_share == s)
		g_share = NULL;
	free(s);
}

static GlTex *tex_get(GlShare *s, GLuint id)
{
	int i;
	if (!s || !id)
		return NULL;
	for (i = 0; i < s->ntex; i++)
		if (s->tex[i] && s->tex[i]->id == id)
			return s->tex[i];
	return NULL;
}

static GlBuf *buf_get(GlShare *s, GLuint id)
{
	int i;
	if (!s || !id)
		return NULL;
	for (i = 0; i < s->nbuf; i++)
		if (s->buf[i] && s->buf[i]->id == id)
			return s->buf[i];
	return NULL;
}

static GlVao *vao_get(GlShare *s, GLuint id)
{
	int i;
	if (!s)
		return NULL;
	if (id == 0)
		return s->vao0;
	for (i = 0; i < s->nvao; i++)
		if (s->vao[i] && s->vao[i]->id == id)
			return s->vao[i];
	return NULL;
}

static GlFbo *fbo_get(GlShare *s, GLuint id)
{
	int i;
	if (!s || !id)
		return NULL;
	for (i = 0; i < s->nfbo; i++)
		if (s->fbo[i] && s->fbo[i]->id == id)
			return s->fbo[i];
	return NULL;
}

static GlRbo *rbo_get(GlShare *s, GLuint id)
{
	int i;
	if (!s || !id)
		return NULL;
	for (i = 0; i < s->nrbo; i++)
		if (s->rbo[i] && s->rbo[i]->id == id)
			return s->rbo[i];
	return NULL;
}

static GlShader *sh_get(GlShare *s, GLuint id)
{
	int i;
	if (!s || !id)
		return NULL;
	for (i = 0; i < s->nsh; i++)
		if (s->sh[i] && s->sh[i]->id == id)
			return s->sh[i];
	return NULL;
}

static GlProg *prog_get(GlShare *s, GLuint id)
{
	int i;
	if (!s || !id)
		return NULL;
	for (i = 0; i < s->nprog; i++)
		if (s->prog[i] && s->prog[i]->id == id)
			return s->prog[i];
	return NULL;
}

static int ensure_fb(GlCtx *c, int w, int h)
{
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	if (c->fb.color && c->fb.width == w && c->fb.height == h)
		return 1;
	swrast_resize(&c->fb, w, h);
	c->fb.hwnd = c->hwnd;
	return c->fb.color != NULL;
}

static GlCtx *display_ctx(GlCtx *c)
{
	if (c && c->fb.width * c->fb.height > 16)
		return c;
	{
		GlCtx *big = ctx_largest();
		if (big)
			return big;
	}
	return c;
}

static void size_from_dc(GlCtx *c, int *w, int *h)
{
	RECT rc;
	HWND hwnd = c->hwnd ? c->hwnd : own_window_from_dc(c->hdc);
	c->hwnd = hwnd;
	if (hwnd && GetClientRect(hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
		*w = rc.right - rc.left;
		*h = rc.bottom - rc.top;
		return;
	}
	*w = 1280;
	*h = 720;
}

/* The window buffer's size: what the game renders at once known, else the client area. */
static void win_size(GlCtx *c, int *w, int *h)
{
	size_from_dc(c, w, h);
	if (g_win_w > 0 && g_win_h > 0 && *w * *h > 16) {
		*w = g_win_w;
		*h = g_win_h;
	}
	if (g_render_h < 0)
		rs_update(*h);
	*w = rs_px(*w, g_rs);
	*h = rs_px(*h, g_rs);
}

/* A render target the game sizes to the window, which the render scale shrinks. */
static int is_window_sized(GlCtx *c, int w, int h)
{
	int cw, ch;
	if (g_win_w > 0 && w == g_win_w && h == g_win_h)
		return 1;
	size_from_dc(c, &cw, &ch);
	return w == cw && h == ch;
}

static void bind_draw_target(GlCtx *c, SwRast *out, SwTex *tex)
{
	GlFbo *f;
	GlTex *t;
	memset(out, 0, sizeof(*out));
	memset(tex, 0, sizeof(*tex));
	c->rt_rs = 1.0f;
	if (!c->draw_fbo) {
		GlCtx *d = display_ctx(c);
		int w, h;
		win_size(d, &w, &h);
		ensure_fb(d, w, h);
		*out = d->fb;
		c->rt_rs = g_rs;
		return;
	}
	f = fbo_get(c->share, c->draw_fbo);
	if (!f)
		return;
	t = tex_get(c->share, c->out_att > 0 && c->out_att < 8 && f->att[c->out_att]
				      ? f->att[c->out_att]
				      : f->color);
	if (t && t->pixels) {
		GlTex *d = tex_get(c->share, f->depth);
		if (t->rs > 0)
			c->rt_rs = t->rs;
		out->color = t->pixels;
		out->width = t->w;
		out->height = t->h;
		tex->pixels = t->pixels;
		tex->width = t->w;
		tex->height = t->h;
		if (d && d->w == t->w && d->h == t->h) {
			if (!d->z) {
				size_t i, n = (size_t)d->w * d->h;
				d->z = (float *)malloc(n * sizeof(float));
				for (i = 0; d->z && i < n; i++)
					d->z[i] = 1.0f;
			}
			out->depth = d->z;
		}
	}
}

static void bind_read_target(GlCtx *c, SwRast *out)
{
	SwTex tex;
	GLuint save = c->draw_fbo;
	float save_rs = c->rt_rs;
	c->draw_fbo = c->read_fbo;
	bind_draw_target(c, out, &tex);
	c->read_rs = c->rt_rs;
	c->rt_rs = save_rs;
	c->draw_fbo = save;
}

/* GL row y (0 = bottom) to a memory row: FBO textures are stored bottom-up like
 * GL, the window top-down for presentation. */
static int gl_row(const SwRast *rt, int up, int y)
{
	return up ? y : rt->height - 1 - y;
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

static int d3d_blend(GLenum f)
{
	switch (f) {
	case GL_ZERO:
		return 1; /* D3DBLEND_ZERO */
	case GL_ONE:
		return 2;
	case GL_SRC_COLOR:
		return 3;
	case GL_ONE_MINUS_SRC_COLOR:
		return 4;
	case GL_SRC_ALPHA:
		return 5;
	case GL_ONE_MINUS_SRC_ALPHA:
		return 6;
	case GL_DST_ALPHA:
		return 7;
	case GL_ONE_MINUS_DST_ALPHA:
		return 8;
	case GL_DST_COLOR:
		return 9;
	case GL_ONE_MINUS_DST_COLOR:
		return 10;
	default:
		return 5;
	}
}

static int d3d_zfunc(GLenum f)
{
	switch (f) {
	case GL_NEVER:
		return 1;
	case GL_LESS:
		return 2;
	case GL_EQUAL:
		return 3;
	case GL_LEQUAL:
		return 4;
	case GL_GREATER:
		return 5;
	case GL_NOTEQUAL:
		return 6;
	case GL_GEQUAL:
		return 7;
	case GL_ALWAYS:
		return 8;
	default:
		return 4;
	}
}

static void fill_state(GlCtx *c, SwState *st, int rt_h)
{
	swrast_state_defaults(st);
	st->blend_enable = c->en_blend;
	st->src_blend = d3d_blend(c->blend_src);
	st->dst_blend = d3d_blend(c->blend_dst);
	st->blend_op = 1; /* ADD */
	st->z_enable = c->en_depth;
	st->z_write = c->depth_mask;
	st->z_func = d3d_zfunc(c->depth_func);
	/* swrast discards area > 0 for 2 (D3DCULL_CW) and area < 0 for 3. On a
	 * bottom-up target the rows are GL window rows, so a GL counter-clockwise
	 * triangle has area > 0; a top-down target mirrors that. */
	st->cull = 1;
	if (c->en_cull && c->cull_face != GL_FRONT_AND_BACK) {
		int front_ccw = c->front_face != GL_CW;
		int cull_ccw = c->cull_face == GL_FRONT ? front_ccw : !front_ccw;
		if (!c->rt_up)
			cull_ccw = !cull_ccw;
		st->cull = cull_ccw ? 2 : 3;
	}
	st->scissor_enable = c->en_scissor;
	if (rt_h < 1)
		rt_h = c->fb.height;
	{
		float s = c->rt_rs > 0 ? c->rt_rs : 1.0f;
		int sx0 = rs_px(c->scissor[0], s), sy0 = rs_px(c->scissor[1], s);
		int sx1 = rs_px(c->scissor[0] + c->scissor[2], s);
		int sy1 = rs_px(c->scissor[1] + c->scissor[3], s);
		st->scissor_x0 = sx0;
		st->scissor_x1 = sx1;
		if (c->rt_up) {
			st->scissor_y0 = sy0;
			st->scissor_y1 = sy1;
		} else {
			st->scissor_y0 = rt_h - sy1;
			st->scissor_y1 = rt_h - sy0;
		}
	}
	st->bilinear = 1;
	st->addr_u = 1; /* WRAP */
	st->addr_v = 1;
	st->write_mask = 0xffffffffu;
	if (!c->color_mask[0])
		st->write_mask &= ~0x00ff0000u;
	if (!c->color_mask[1])
		st->write_mask &= ~0x0000ff00u;
	if (!c->color_mask[2])
		st->write_mask &= ~0x000000ffu;
	if (!c->color_mask[3])
		st->write_mask &= ~0xff000000u;
}

static unsigned type_bytes(GLenum type)
{
	switch (type) {
	case GL_UNSIGNED_BYTE:
	case GL_BYTE:
		return 1;
	case GL_UNSIGNED_SHORT:
	case GL_SHORT:
	case GL_HALF_FLOAT:
		return 2;
	case GL_FLOAT:
	case GL_UNSIGNED_INT:
	case GL_INT:
		return 4;
	default:
		return 4;
	}
}

static void load_attr(float out[4], const unsigned char *p, const GlAttrib *a)
{
	int i;
	out[0] = out[1] = out[2] = 0;
	out[3] = 1;
	if (!p)
		return;
	for (i = 0; i < a->size && i < 4; i++) {
		const unsigned char *c = p + (size_t)i * type_bytes(a->type);
		switch (a->type) {
		case GL_FLOAT:
			out[i] = *(const float *)c;
			break;
		case GL_UNSIGNED_BYTE:
			out[i] = a->normalized ? c[0] / 255.0f : (float)c[0];
			break;
		case GL_UNSIGNED_SHORT:
			out[i] = a->normalized ? (*(const GLushort *)c) / 65535.0f
					       : (float)(*(const GLushort *)c);
			break;
		case GL_UNSIGNED_INT:
			out[i] = (float)(*(const GLuint *)c);
			break;
		case GL_INT:
			out[i] = (float)(*(const GLint *)c);
			break;
		default:
			out[i] = *(const float *)c;
			break;
		}
	}
}

static const unsigned char *attr_ptr(GlCtx *c, const GlAttrib *a, unsigned idx)
{
	GlBuf *b;
	size_t stride, off;
	if (!a->enabled)
		return NULL;
	stride = a->stride ? (size_t)a->stride : (size_t)a->size * type_bytes(a->type);
	off = (size_t)a->offset + (size_t)idx * stride;
	if (a->buf) {
		b = buf_get(c->share, a->buf);
		if (!b || !b->data || off + type_bytes(a->type) > b->size)
			return NULL;
		return b->data + off;
	}
	return (const unsigned char *)(uintptr_t)a->offset + (size_t)idx * stride;
}

/* Column-major 4x4 (OpenGL layout: m[col*4 + row]). */
static void mat4_mul(float *out, const float *a, const float *b)
{
	float t[16];
	int col, row, k;

	for (col = 0; col < 4; col++)
		for (row = 0; row < 4; row++) {
			float s = 0;
			for (k = 0; k < 4; k++)
				s += a[k * 4 + row] * b[col * 4 + k];
			t[col * 4 + row] = s;
		}
	memcpy(out, t, sizeof(t));
}

static void mat4_vec(float *out, const float *m, const float *v)
{
	int row, k;

	for (row = 0; row < 4; row++) {
		float s = 0;
		for (k = 0; k < 4; k++)
			s += m[k * 4 + row] * v[k];
		out[row] = s;
	}
}

/* A named uniform in a program if it EXISTS (was queried), value or not. Used to
 * fingerprint a pass by which uniforms it declares - e.g. the deferred lighting
 * pass by its G-buffer samplers, which are set through glUniform1i and so may have
 * no stored value here but were certainly located. */
static const GlUniform *prog_uniform(const GlProg *p, const char *name)
{
	int i;

	if (!p)
		return NULL;
	for (i = 0; i < p->nuni; i++)
		if (strcmp(p->uni[i].name, name) == 0)
			return &p->uni[i];
	return NULL;
}

/* A uniform declared in the fragment shader source, whether or not the game has
 * looked it up yet. */
static int fs_declares(const GlProg *p, const char *name)
{
	GlslVar vars[64];
	int n, k;

	if (!p || !p->fs)
		return 0;
	n = glsl_vars(p->fs, vars, 64);
	for (k = 0; k < n; k++)
		if (vars[k].qual == GLSL_Q_UNIFORM && vars[k].name && !strcmp(vars[k].name, name))
			return 1;
	return 0;
}

static int prog_mat4(const GlProg *p, const char *name, float *out)
{
	const GlUniform *u = prog_uniform(p, name);

	if (u && u->is_mat4) {
		memcpy(out, u->m, 16 * sizeof(float));
		return 1;
	}
	return 0;
}

static void vert_from_attr(GlCtx *c, SwVert *v, unsigned idx, int vp_w, int vp_h, int vp_x,
			   int vp_y)
{
	GlVao *vao = vao_get(c->share, c->vao);
	const GlAttrib *pos = NULL, *uv = NULL, *col = NULL;
	float p[4], t[4], k[4];
	float x, y, z, w, rhw;
	int i;

	memset(v, 0, sizeof(*v));
	v->color = 0xffffffffu;
	v->rhw = 1;
	if (vao) {
		for (i = 0; i < GL_ATTRS; i++) {
			if (!vao->attr[i].enabled)
				continue;
			if (!pos)
				pos = &vao->attr[i];
			else if (!uv && vao->attr[i].size <= 2)
				uv = &vao->attr[i];
			else if (!col && vao->attr[i].size >= 3)
				col = &vao->attr[i];
		}
	}
	if (pos)
		load_attr(p, attr_ptr(c, pos, idx), pos);
	else {
		p[0] = p[1] = p[2] = 0;
		p[3] = 1;
	}
	/* Forward-hack: the model pass hands us MODEL-space vec3 positions, not clip
	 * space. Run the transform the game's vertex shader would have - proj *
	 * localView * vec4(pos,1) - so the geometry lands where it belongs. Everything
	 * below then treats p[] as the clip-space position it now is. */
	if (c->model_mvp_active) {
		float in[4], clip[4];

		in[0] = p[0];
		in[1] = p[1];
		in[2] = p[2];
		in[3] = 1.0f;
		mat4_vec(clip, c->model_mvp, in);
		if (clip[3] <= 1e-4f) {
			/* On or behind the near plane. Without near-plane clipping the
			 * perspective divide by a tiny/negative w throws the vertex to
			 * infinity - the screen-spanning "giant triangle". Flag it so
			 * draw_tris drops the whole triangle instead of rasterizing garbage.
			 */
			v->rhw = -1.0f;
			return;
		}
		p[0] = clip[0];
		p[1] = clip[1];
		p[2] = clip[2];
		p[3] = clip[3];
	}
	w = p[3] != 0.0f ? p[3] : 1.0f;
	rhw = 1.0f / w;
	x = p[0] * rhw;
	y = p[1] * rhw;
	z = p[2] * rhw;
	v->x = (x * 0.5f + 0.5f) * (float)vp_w + (float)vp_x;
	v->y = (c->rt_up ? 0.5f + y * 0.5f : 0.5f - y * 0.5f) * (float)vp_h + (float)vp_y;
	v->z = z * 0.5f + 0.5f;
	v->rhw = rhw;
	/* The raw-pixel escape hatch is only for untransformed 2D verts; a model MVP
	 * produces real clip coords, so never take it then. */
	if (!c->model_mvp_active &&
	    (!pos || (x > 2.0f || x < -2.0f || y > 2.0f || y < -2.0f))) {
		/* Same escape hatch as D3D11: raw pixel-space verts skip clip. */
		if (p[0] > 2.0f || p[0] < -2.0f || p[1] > 2.0f || p[1] < -2.0f) {
			float s = c->rt_rs > 0 ? c->rt_rs : 1.0f;
			v->x = p[0] * s + (float)vp_x;
			v->y = (c->rt_up ? p[1] * s : (float)vp_h - p[1] * s) + (float)vp_y;
			v->z = 0.5f;
			v->rhw = 1.0f;
		}
	}
	if (uv) {
		load_attr(t, attr_ptr(c, uv, idx), uv);
		v->u = t[0];
		v->v = t[1];
	}
	if (col) {
		load_attr(k, attr_ptr(c, col, idx), col);
		v->color = pack_argb(k[0], k[1], k[2], k[3]);
	}
}

static GlTex *bound_tex2d(GlCtx *c)
{
	return tex_get(c->share, c->tex2d[c->texunit]);
}

/* ---- programmable vertex stage ---- */

static int uni_slot(const GlProg *p, const char *name)
{
	int i;
	for (i = 0; i < p->nuni; i++)
		if (strcmp(p->uni[i].name, name) == 0)
			return i;
	return -1;
}

static int name_has(const char *s, const char *const *keys)
{
	for (; *keys; keys++)
		if (strstr(s, *keys))
			return 1;
	return 0;
}

/* Resolve vertex inputs to attribute locations (layout, then glBindAttribLocation,
 * then the lowest free location in declaration order), and pick the vertex outputs
 * the fixed-function fragment stage uses as texcoord and colour. */
static void prog_io(GlProg *p)
{
	static const char *const k_uv[] = { "uv", "UV", "Coord", "coord", "Tex", 0 };
	static const char *const k_col[] = { "olor", "olour", 0 };
	GlslVar vars[64];
	int n, i, next = 0;
	unsigned char used[GL_ATTRS];

	if (p->io_ready)
		return;
	p->io_ready = 1;
	p->nin = 0;
	p->out_uv = p->out_col = NULL;
	p->fs_unit_uni = -1;
	p->fs_no_tex = 0;
	p->fs_color_uni = -1;
	memset(used, 0, sizeof(used));
	if (p->vs) {
		n = glsl_vars(p->vs, vars, 64);
		for (i = 0; i < n && p->nin < PROG_MAX_IN; i++) {
			int loc = vars[i].location, b;
			if (vars[i].qual != GLSL_Q_IN)
				continue;
			for (b = 0; loc < 0 && b < p->nbind; b++)
				if (strcmp(p->bind_name[b], vars[i].name) == 0)
					loc = p->bind_loc[b];
			if (loc >= GL_ATTRS)
				loc = -1;
			p->in_name[p->nin] = vars[i].name;
			p->in_loc[p->nin] = loc;
			p->in_n[p->nin] = vars[i].rows > 4 ? 4 : vars[i].rows;
			if (loc >= 0)
				used[loc] = 1;
			p->nin++;
		}
		for (i = 0; i < p->nin; i++) {
			if (p->in_loc[i] >= 0)
				continue;
			while (next < GL_ATTRS && used[next])
				next++;
			if (next < GL_ATTRS) {
				p->in_loc[i] = next;
				used[next] = 1;
			}
		}
		for (i = 0; i < n; i++) {
			int sz = vars[i].rows * vars[i].cols;
			if (vars[i].qual != GLSL_Q_OUT || sz < 2 || sz > 4)
				continue;
			if (!p->out_uv && name_has(vars[i].name, k_uv))
				p->out_uv = vars[i].name;
			if (!p->out_col && sz >= 3 && name_has(vars[i].name, k_col))
				p->out_col = vars[i].name;
		}
		for (i = 0; !p->out_uv && i < n; i++)
			if (vars[i].qual == GLSL_Q_OUT && vars[i].rows == 2 && vars[i].cols == 1)
				p->out_uv = vars[i].name;
	}
	p->fs_out_loc = 0;
	p->fs_out_diffuse = 0;
	if (p->fs) {
		static const char *const k_tex[] = { "iffuse", "lbedo", "olor", "olour", 0 };
		static const char *const k_dif[] = { "iffuse", "lbedo", 0 };
		const char *samp = NULL;
		int best = -1;
		n = glsl_vars(p->fs, vars, 64);
		/* The fixed-function stage samples one texture and writes one target:
		 * prefer the diffuse/colour sampler and output over normals, gloss, etc. */
		for (i = 0; i < n; i++) {
			if (vars[i].is_sampler && (!samp || (!name_has(samp, k_tex) &&
							    name_has(vars[i].name, k_tex))))
				samp = vars[i].name;
			if (vars[i].qual == GLSL_Q_OUT) {
				int named = name_has(vars[i].name, k_tex);
				if (best < 0 || (named && !name_has(vars[best].name, k_tex)))
					best = i;
			}
		}
		if (samp)
			p->fs_unit_uni = uni_slot(p, samp);
		p->fs_no_tex = !samp;
		for (i = 0; i < n && p->fs_color_uni < 0; i++)
			if (vars[i].qual == GLSL_Q_UNIFORM && !vars[i].is_sampler &&
			    vars[i].cols == 1 && vars[i].rows >= 3 && vars[i].rows <= 4 &&
			    !vars[i].arr_len && name_has(vars[i].name, k_col))
				p->fs_color_uni = uni_slot(p, vars[i].name);
		if (best >= 0) {
			p->fs_out_loc = vars[best].location > 0 ? vars[best].location : 0;
			p->fs_out_diffuse = p->fs_out_loc > 0 && name_has(vars[best].name, k_dif);
		}
	}
}

static void push_uniforms(GlProg *p, GlslProg *g)
{
	int i;
	if (!g)
		return;
	for (i = 0; i < p->nuni; i++) {
		GlUniform *u = &p->uni[i];
		int k;
		if (!u->set || !u->v)
			continue;
		k = glsl_kind(g, u->name);
		if (k == 2)
			glsl_set_sampler(g, u->name, u->ivalue);
		else if (k == 1)
			glsl_set(g, u->name, u->v, u->nelem * u->nc);
	}
}

/* texture() for shaders: nearest, wrapped, from the unit's bound 2D texture. */
static int gl_sample(void *ctx, int unit, const float *coord, int nc, float *rgba)
{
	GlCtx *c = (GlCtx *)ctx;
	GlTex *t;
	uint32_t px;
	int x, y;

	if (unit < 0 || unit >= GL_TEXUNITS || nc < 2)
		return 0;
	t = tex_get(c->share, c->tex2d[unit]);
	if (!t || !t->pixels || t->w < 1 || t->h < 1)
		return 0;
	x = (int)((coord[0] - floorf(coord[0])) * (float)t->w);
	y = (int)((coord[1] - floorf(coord[1])) * (float)t->h);
	if (x >= t->w)
		x = t->w - 1;
	if (y >= t->h)
		y = t->h - 1;
	px = t->pixels[y * t->w + x];
	rgba[0] = (float)((px >> 16) & 0xff) / 255.0f;
	rgba[1] = (float)((px >> 8) & 0xff) / 255.0f;
	rgba[2] = (float)(px & 0xff) / 255.0f;
	rgba[3] = (float)(px >> 24) / 255.0f;
	return 1;
}

/* Feed one vertex's attributes to a vertex shader instance - p->vs or one of
 * its per-thread clones - and run it. */
static void vs_run_g(GlslProg *g, GlCtx *c, GlProg *p, GlVao *vao, unsigned idx)
{
	int i;
	for (i = 0; i < p->nin; i++) {
		float a[4] = { 0, 0, 0, 1 };
		int L = p->in_loc[i];
		if (L >= 0) {
			const GlAttrib *at = vao ? &vao->attr[L] : NULL;
			if (at && at->enabled)
				load_attr(a, attr_ptr(c, at, at->divisor ? 0 : idx), at);
			else
				memcpy(a, c->cur_attr[L], sizeof(a));
		}
		glsl_set(g, p->in_name[i], a, p->in_n[i]);
	}
	glsl_run(g, gl_sample, c);
}

static void vs_run(GlCtx *c, GlProg *p, GlVao *vao, unsigned idx)
{
	vs_run_g(p->vs, c, p, vao, idx);
}

/* A shaded vertex before projection: gl_Position plus the outputs the
 * fixed-function fragment stage uses. Kept in clip space so triangles can be
 * clipped against the near plane before the divide. */
typedef struct VsClip {
	float p[4];
	float u, v;
	float col[4];
} VsClip;

static void vs_clip_out_g(GlslProg *g, GlProg *p, VsClip *v)
{
	float o[16];
	int n;

	memset(v, 0, sizeof(*v));
	v->p[3] = 1;
	v->col[0] = v->col[1] = v->col[2] = v->col[3] = 1;
	glsl_get(g, "gl_Position", v->p, 4);
	if (p->out_uv && glsl_get(g, p->out_uv, o, 16) >= 2) {
		v->u = o[0];
		v->v = o[1];
	}
	if (p->out_col && (n = glsl_get(g, p->out_col, o, 16)) >= 3) {
		v->col[0] = o[0];
		v->col[1] = o[1];
		v->col[2] = o[2];
		v->col[3] = n >= 4 ? o[3] : 1.0f;
	}
}

static void vs_clip_out(GlProg *p, VsClip *v)
{
	vs_clip_out_g(p->vs, p, v);
}

/* ---- vertex shading across the rasteriser's pool ----
 * Each worker runs its own clone of the program, refreshed from p->vs (which
 * holds this draw's uniforms) the first time it takes a job of a new batch. */
#define VS_CHUNK 64
#define VS_PAR_MIN 256
#define VS_TF_STRIDE 17 /* per varying: component count, then up to 16 values */

static GlslProg *g_vs_clone[SWRAST_JOB_SLOTS];
static LONG g_vs_clone_seq[SWRAST_JOB_SLOTS];
static LONG g_vs_seq;

typedef struct VsJob {
	GlCtx *c;
	GlProg *p;
	GlVao *vao;
	const unsigned *geom;
	unsigned n;
	VsClip *clip;
	float *tf;
	LONG seq;
	volatile LONG failed;
} VsJob;

/* D3D9SW_VS_THREADS: 1 keeps vertex shading on the game thread; unset uses
 * the whole rasteriser pool. */
static int vs_threads(void)
{
	static int v = -1;
	if (v < 0) {
		char e[16];
		unsigned n = savestate_getenv("D3D9SW_VS_THREADS", e, sizeof(e));
		v = n && n < sizeof(e) && atoi(e) > 0 ? atoi(e) : SWRAST_JOB_SLOTS;
	}
	return v;
}

static void vs_job(void *arg, int w, int job)
{
	VsJob *j = (VsJob *)arg;
	unsigned i = (unsigned)job * VS_CHUNK, end = i + VS_CHUNK;
	GlslProg *g;

	if (g_vs_clone_seq[w] != j->seq) {
		if (!glsl_sync(&g_vs_clone[w], j->p->vs)) {
			InterlockedExchange(&j->failed, 1);
			return;
		}
		g_vs_clone_seq[w] = j->seq;
	}
	g = g_vs_clone[w];
	if (end > j->n)
		end = j->n;
	for (; i < end; i++) {
		vs_run_g(g, j->c, j->p, j->vao, j->geom[i]);
		if (j->clip) {
			vs_clip_out_g(g, j->p, &j->clip[i]);
		} else {
			float *o = j->tf + (size_t)i * (size_t)j->p->ntf * VS_TF_STRIDE;
			int k;
			for (k = 0; k < j->p->ntf; k++, o += VS_TF_STRIDE)
				o[0] = (float)glsl_get(g, j->p->tf_name[k], o + 1, 16);
		}
	}
}

/* Shades j->n vertices into j->clip or j->tf. 0 means it could not, and the
 * caller shades them itself. */
static int vs_parallel(VsJob *j)
{
	j->seq = ++g_vs_seq;
	j->failed = 0;
	swrast_parallel((int)((j->n + VS_CHUNK - 1) / VS_CHUNK), vs_threads(), vs_job, j);
	return !j->failed;
}

/* Perspective divide and viewport transform; w must be positive. */
static void clip_project(GlCtx *c, const VsClip *s, SwVert *v, int vp_w, int vp_h, int vp_x,
			 int vp_y)
{
	float rhw = 1.0f / s->p[3];
	v->x = (s->p[0] * rhw * 0.5f + 0.5f) * (float)vp_w + (float)vp_x;
	v->y = (c->rt_up ? 0.5f + s->p[1] * rhw * 0.5f : 0.5f - s->p[1] * rhw * 0.5f) *
		       (float)vp_h +
	       (float)vp_y;
	v->z = s->p[2] * rhw * 0.5f + 0.5f;
	v->rhw = rhw;
	v->u = s->u;
	v->v = s->v;
	v->color = pack_argb(s->col[0], s->col[1], s->col[2], s->col[3]);
}

static void clip_lerp(VsClip *d, const VsClip *a, const VsClip *b, float t)
{
	int k;
	for (k = 0; k < 4; k++) {
		d->p[k] = a->p[k] + (b->p[k] - a->p[k]) * t;
		d->col[k] = a->col[k] + (b->col[k] - a->col[k]) * t;
	}
	d->u = a->u + (b->u - a->u) * t;
	d->v = a->v + (b->v - a->v) * t;
}

/* Clip a triangle against the GL near plane (z >= -w) and append the visible
 * part, 0 to 2 triangles with the original winding, to out. */
static int clip_tri(GlCtx *c, const VsClip *in, SwTri *out, int vp_w, int vp_h, int vp_x,
		    int vp_y)
{
	VsClip poly[4];
	float d[3];
	int k, n = 0, t;

	for (k = 0; k < 3; k++)
		d[k] = in[k].p[2] + in[k].p[3];
	if (d[0] < 0 && d[1] < 0 && d[2] < 0)
		return 0;
	for (k = 0; k < 3; k++) {
		int j = (k + 1) % 3;
		if (d[k] >= 0)
			poly[n++] = in[k];
		if ((d[k] >= 0) != (d[j] >= 0))
			clip_lerp(&poly[n++], &in[k], &in[j], d[k] / (d[k] - d[j]));
	}
	for (k = 0; k < n; k++)
		if (poly[k].p[3] <= 1e-6f)
			return 0;
	for (t = 0; t + 2 < n; t++) {
		clip_project(c, &poly[0], &out[t].a, vp_w, vp_h, vp_x, vp_y);
		clip_project(c, &poly[t + 1], &out[t].b, vp_w, vp_h, vp_x, vp_y);
		clip_project(c, &poly[t + 2], &out[t].c, vp_w, vp_h, vp_x, vp_y);
	}
	return n >= 3 ? n - 2 : 0;
}

/* Trace: each vertex input's binding and the value read for vertex idx. */
static void tr_inputs(GlCtx *c, GlProg *p, GlVao *vao, unsigned idx)
{
	int i;
	if (!g_tr)
		return;
	for (i = 0; i < p->nin; i++) {
		float a[4] = { 0, 0, 0, 1 };
		int L = p->in_loc[i];
		const GlAttrib *at = vao && L >= 0 ? &vao->attr[L] : NULL;
		GlBuf *b = at ? buf_get(c->share, at->buf) : NULL;
		const unsigned char *src = at && at->enabled ? attr_ptr(c, at, at->divisor ? 0 : idx) : NULL;
		if (src)
			load_attr(a, src, at);
		else if (L >= 0 && !(at && at->enabled))
			memcpy(a, c->cur_attr[L], sizeof(a));
		tr("    in %-12s loc %2d %s buf %u (%s %lu bytes) size %d type 0x%X stride %d off %ld"
		   " -> (%.3f, %.3f, %.3f, %.3f)%s",
		   p->in_name[i], L, at && at->enabled ? "on " : "off", at ? at->buf : 0,
		   b ? (b->data ? "data" : "NO DATA") : "no buffer", b ? (unsigned long)b->size : 0,
		   at ? at->size : 0, at ? (unsigned)at->type : 0, at ? at->stride : 0,
		   at ? (long)at->offset : 0, a[0], a[1], a[2], a[3],
		   at && at->enabled && !src ? " READ FAILED" : "");
	}
}

static unsigned fetch_index(const unsigned char *ip, unsigned esz, unsigned i)
{
	if (esz == 4)
		return ((const GLuint *)ip)[i];
	if (esz == 2)
		return ((const GLushort *)ip)[i];
	return ip[i];
}

static void tf_write(GlCtx *c, GlBuf **b, int bi, const float *o, int n)
{
	size_t at;
	if (n <= 0 || bi >= 4 || !b[bi] || !b[bi]->data)
		return;
	at = c->tf_base[bi] + c->tf_off[bi];
	if (at + (size_t)n * sizeof(float) <= b[bi]->size)
		memcpy(b[bi]->data + at, o, (size_t)n * sizeof(float));
	c->tf_off[bi] += (size_t)n * sizeof(float);
}

/* Transform feedback: run the vertex shader for every vertex in order and append
 * the captured varyings to the bound buffers. Haydee skins on the GPU this way. */
static void tf_capture(GlCtx *c, GlProg *p, unsigned count, const unsigned char *ip, unsigned esz,
		       unsigned first, int basevertex)
{
	GlVao *vao = vao_get(c->share, c->vao);
	GlBuf *b[4];
	unsigned k;
	int j, sep = p->tf_mode == GL_SEPARATE_ATTRIBS;

	for (j = 0; j < 4; j++)
		b[j] = c->tf_buf[j] ? buf_get(c->share, c->tf_buf[j]) : NULL;
	if (g_tr) {
		int s = uni_slot(p, "joints");
		float mx = 0;
		if (s >= 0 && p->uni[s].v) {
			int e, n = p->uni[s].nelem * p->uni[s].nc;
			for (e = 0; e < n; e++)
				if (fabsf(p->uni[s].v[e]) > mx)
					mx = fabsf(p->uni[s].v[e]);
		}
		tr("  capture: buffer %u (%s, %lu bytes, base %lu), joints slot %d set %d elems %d x %d "
		   "floats, max |v| %.3f",
		   c->tf_buf[0], b[0] ? (b[0]->data ? "data" : "NO DATA") : "not found",
		   b[0] ? (unsigned long)b[0]->size : 0, (unsigned long)c->tf_base[0], s,
		   s >= 0 ? p->uni[s].set : 0, s >= 0 ? p->uni[s].nelem : 0, s >= 0 ? p->uni[s].nc : 0,
		   mx);
	}
	if (!g_tr && count >= VS_PAR_MIN && vs_threads() > 1) {
		VsJob jb;
		unsigned *geoms = (unsigned *)malloc((size_t)count * sizeof(unsigned));
		float *outs = (float *)malloc((size_t)count * (size_t)p->ntf * VS_TF_STRIDE *
					      sizeof(float));
		int ok = 0;

		if (geoms && outs) {
			for (k = 0; k < count; k++)
				geoms[k] = (ip && esz ? fetch_index(ip, esz, first + k) : first + k) +
					   (unsigned)basevertex;
			memset(&jb, 0, sizeof(jb));
			jb.c = c;
			jb.p = p;
			jb.vao = vao;
			jb.geom = geoms;
			jb.n = count;
			jb.tf = outs;
			ok = vs_parallel(&jb);
		}
		if (ok) {
			for (k = 0; k < count; k++) {
				const float *o = outs + (size_t)k * (size_t)p->ntf * VS_TF_STRIDE;
				for (j = 0; j < p->ntf; j++, o += VS_TF_STRIDE)
					tf_write(c, b, sep ? j : 0, o + 1, (int)o[0]);
			}
		}
		free(geoms);
		free(outs);
		if (ok)
			return;
	}
	for (k = 0; k < count; k++) {
		unsigned geom = ip && esz ? fetch_index(ip, esz, first + k) : first + k;
		vs_run(c, p, vao, geom + (unsigned)basevertex);
		if (k == 0 && g_tr) {
			float o[16] = { 0 };
			tr_inputs(c, p, vao, geom + (unsigned)basevertex);
			glsl_get(p->vs, p->ntf ? p->tf_name[0] : "gl_Position", o, 16);
			tr("    out %s = (%.3f, %.3f, %.3f)", p->ntf ? p->tf_name[0] : "-", o[0], o[1], o[2]);
		}
		for (j = 0; j < p->ntf; j++) {
			float o[16];
			int n = glsl_get(p->vs, p->tf_name[j], o, 16);
			tf_write(c, b, sep ? j : 0, o, n);
		}
	}
}

/* Stand-in for the deferred lighting pass: copy the G-buffer's diffuse target into
 * the texture lighting would have written, once per frame. */
static GLuint g_albedo_tex;
/* Lighting targets already filled this frame. A frame can light several scenes
 * (Haydee renders a half-size mirrored scene for floor reflections before the
 * main one), each once, however many lights it has. */
static GLuint g_lit_dst[8];
static int g_lit_n;

static void lighting_passthrough(GlCtx *c)
{
	GlFbo *f = fbo_get(c->share, c->draw_fbo);
	GlTex *dst = f ? tex_get(c->share, f->color) : NULL;
	GlTex *src = tex_get(c->share, g_albedo_tex);
	int k, done = 0;

	for (k = 0; dst && k < g_lit_n; k++)
		if (g_lit_dst[k] == dst->id)
			done = 1;
	if (done || !dst || !src || dst == src || !dst->pixels || !src->pixels ||
	    dst->w != src->w || dst->h != src->h) {
		tr("lighting stand-in: nothing copied (diffuse tex %u, target tex %u%s)",
		   g_albedo_tex, dst ? dst->id : 0, done ? ", already done this frame" : "");
		return;
	}
	swrast_flush();
	memcpy(dst->pixels, src->pixels, (size_t)dst->w * dst->h * sizeof(uint32_t));
	if (g_lit_n < (int)(sizeof(g_lit_dst) / sizeof(g_lit_dst[0])))
		g_lit_dst[g_lit_n++] = dst->id;
	tr_touch(dst->id);
	tr("lighting stand-in: copied diffuse tex %u into tex %u (%dx%d)", src->id, dst->id,
	   dst->w, dst->h);
}

static GlTex *draw_tex(GlCtx *c, GlProg *p);

static float blend_factor(GLenum f, int ch, const float *s, const float *d)
{
	switch (f) {
	case GL_ZERO:
		return 0;
	case GL_ONE:
		return 1;
	case GL_SRC_COLOR:
		return s[ch];
	case GL_ONE_MINUS_SRC_COLOR:
		return 1 - s[ch];
	case GL_DST_COLOR:
		return d[ch];
	case GL_ONE_MINUS_DST_COLOR:
		return 1 - d[ch];
	case GL_SRC_ALPHA:
		return s[3];
	case GL_ONE_MINUS_SRC_ALPHA:
		return 1 - s[3];
	case GL_DST_ALPHA:
		return d[3];
	case GL_ONE_MINUS_DST_ALPHA:
		return 1 - d[3];
	default:
		return ch == 3 ? 1 : s[3];
	}
}

/* Haydee's interaction outline: a full-screen pass over an id mask (the object
 * drawn alone into a cleared texture) that outputs vec4(color, |4 - n| / 4),
 * n being how many of the 8 neighbouring texels are non-zero, blended onto the
 * frame. Run natively: the fragment shader cannot be. */
static void outline_pass(GlCtx *c, GlProg *p)
{
	GlFbo *f = fbo_get(c->share, c->draw_fbo);
	GlTex *dst = f ? tex_get(c->share, f->color) : NULL;
	GlTex *m = draw_tex(c, p);
	const GlUniform *u = &p->uni[p->fs_color_uni];
	float col[3] = { 1, 1, 1 };
	int x, y, k;

	if (!dst || !m || dst == m || !dst->pixels || !m->pixels) {
		tr("outline pass: nothing drawn (mask tex %u, target tex %u)", m ? m->id : 0,
		   dst ? dst->id : 0);
		return;
	}
	for (k = 0; k < 3 && u->set && u->v && k < u->nc; k++)
		col[k] = u->v[k];
	swrast_flush();
	for (y = 0; y < dst->h; y++) {
		int my = (int)((long long)y * m->h / dst->h);
		for (x = 0; x < dst->w; x++) {
			int mx = (int)((long long)x * m->w / dst->w), n = 0, dx, dy;
			uint32_t *px = &dst->pixels[y * dst->w + x];
			float s[4], d[4], o[4];
			for (dy = -1; dy <= 1; dy++)
				for (dx = -1; dx <= 1; dx++) {
					int sx = mx + dx, sy = my + dy;
					if (!dx && !dy)
						continue;
					sx = sx < 0 ? 0 : sx >= m->w ? m->w - 1 : sx;
					sy = sy < 0 ? 0 : sy >= m->h ? m->h - 1 : sy;
					n += m->pixels[sy * m->w + sx] != 0;
				}
			s[0] = col[0];
			s[1] = col[1];
			s[2] = col[2];
			s[3] = (float)abs(4 - n) * 0.25f;
			if (!c->en_blend) {
				*px = pack_argb(s[0], s[1], s[2], s[3]);
				continue;
			}
			d[0] = (float)((*px >> 16) & 0xff) / 255.0f;
			d[1] = (float)((*px >> 8) & 0xff) / 255.0f;
			d[2] = (float)(*px & 0xff) / 255.0f;
			d[3] = (float)(*px >> 24) / 255.0f;
			for (k = 0; k < 4; k++) {
				o[k] = s[k] * blend_factor(c->blend_src, k, s, d) +
				       d[k] * blend_factor(c->blend_dst, k, s, d);
				o[k] = o[k] < 0 ? 0 : o[k] > 1 ? 1 : o[k];
			}
			*px = pack_argb(o[0], o[1], o[2], o[3]);
		}
	}
	tr_touch(dst->id);
	tr("outline pass: mask tex %u (%dx%d) onto tex %u (%dx%d), colour (%.2f, %.2f, %.2f), "
	   "blend %d 0x%X/0x%X",
	   m->id, m->w, m->h, dst->id, dst->w, dst->h, col[0], col[1], col[2], c->en_blend,
	   c->blend_src, c->blend_dst);
}

/* The fixed-function fragment stage samples one texture: the unit the fragment
 * shader's first sampler names (default 0), or the active unit with no shader. */
static GlTex *draw_tex(GlCtx *c, GlProg *p)
{
	int unit = (int)c->texunit;
	if (p && p->fs && p->fs_no_tex)
		return NULL;
	if (p && p->fs) {
		unit = 0;
		if (p->fs_unit_uni >= 0 && p->uni[p->fs_unit_uni].set)
			unit = p->uni[p->fs_unit_uni].ivalue;
		if (unit < 0 || unit >= GL_TEXUNITS)
			unit = 0;
	}
	return tex_get(c->share, c->tex2d[unit]);
}

static void draw_tris(GlCtx *c, GLenum mode, unsigned count, GLenum itype, const void *idxp,
		      unsigned first, int basevertex)
{
	SwRast rt;
	SwTex tex;
	SwState st;
	SwTri *batch;
	GlTex *gt;
	GlVao *vao;
	GlBuf *ebo;
	unsigned i, ntri, maxv, nv = 0;
	int vp_w, vp_h, vp_x, vp_y;
	const unsigned char *ip = NULL;
	unsigned esz = 0;
	GlProg *prog;
	int use_vs;
	GLuint rt_tex = 0;
	int probed = 0;
	unsigned *vkey = NULL;
	VsClip *vcache = NULL;
	unsigned *pre_slot = NULL, *pre_geom = NULL, pre_lo = 0, pre_range = 0;
	VsClip *pre_clip = NULL;
	float fcol[4] = { 1, 1, 1, 1 };
	int use_fcol = 0;

	if (!c || !count)
		return;
	if (mode == GL_POLYGON)
		mode = GL_TRIANGLE_FAN;

	vao = vao_get(c->share, c->vao);
	if (vao && vao->ebo) {
		ebo = buf_get(c->share, vao->ebo);
		ip = ebo && ebo->data ? ebo->data + (size_t)(uintptr_t)idxp : NULL;
		esz = (itype == GL_UNSIGNED_INT) ? 4u : (itype == GL_UNSIGNED_BYTE) ? 1u : 2u;
	} else if (idxp) {
		ip = (const unsigned char *)idxp;
		esz = (itype == GL_UNSIGNED_INT) ? 4u : (itype == GL_UNSIGNED_BYTE) ? 1u : 2u;
	}

	prog = prog_get(c->share, c->prog);
	if (prog)
		prog_io(prog);
	use_vs = prog && prog->vs;
	if (c->tf_active || c->en_discard) {
		tr("draw prog=%u count=%u: transform feedback=%d discard=%d varyings=%d%s", c->prog,
		   count, c->tf_active, c->en_discard, prog ? prog->ntf : 0,
		   c->tf_active && use_vs && prog->ntf ? " -> captured" : " -> dropped");
		if (c->tf_active && use_vs && prog->ntf) {
			static volatile LONG said;
			LARGE_INTEGER q0, q1;

			push_uniforms(prog, prog->vs);
			QueryPerformanceCounter(&q0);
			tf_capture(c, prog, count, ip, esz, first, basevertex);
			QueryPerformanceCounter(&q1);
			g_frame_tf_qpc += q1.QuadPart - q0.QuadPart;
			if (InterlockedIncrement(&said) <= 8)
				gl_log("transform feedback: prog %u ran %u vertices -> %u bytes in buffer %u",
				       c->prog, count, (unsigned)c->tf_off[0], c->tf_buf[0]);
		}
		return;
	}

	/* Passes that exist to write stencil (light volumes marking the pixels a
	 * light reaches): no depth write, stencil ops that change the buffer, or a
	 * fragment shader with no outputs. With no stencil buffer their only
	 * visible effect would be painting the volume over the frame. */
	{
		int no_out = 0;
		if (prog && prog->fs) {
			GlslVar vars[64];
			int n = glsl_vars(prog->fs, vars, 64), k;
			no_out = 1;
			for (k = 0; k < n; k++)
				if (vars[k].qual == GLSL_Q_OUT)
					no_out = 0;
		}
		if (no_out || (c->en_stencil && c->sten_mask && !c->depth_mask &&
			       (c->sten_writes[0] || c->sten_writes[1]))) {
			tr("draw prog=%u vs=%u fs=%u mode=0x%X count=%u fbo=%u: SKIPPED as stencil "
			   "pass (%s)",
			   c->prog, prog ? prog->vs_id : 0, prog ? prog->fs_id : 0, (unsigned)mode,
			   count, c->draw_fbo, no_out ? "no fragment outputs" : "stencil writes");
			return;
		}
	}
	if (g_tr && c->en_stencil)
		tr("  stencil test on: ops write %d/%d, mask 0x%X, depth write %d",
		   c->sten_writes[0], c->sten_writes[1], c->sten_mask, c->depth_mask);

	/* ---- forward-hack: recognise Haydee's deferred pipeline by uniform names ----
	 * Geometry fills the G-buffer through the vertex shader; the fixed-function
	 * fragment stage writes only the diffuse attachment. Lighting cannot run
	 * without fragment shaders, so its pass is replaced by a copy of diffuse into
	 * the lighting target - an unlit frame that then flows through the game's own
	 * post-process and present. Water and fog are skipped. Without a vertex
	 * shader the model pass falls back to a built MVP drawn to the window. */
	{
		GlProg *pr = prog;
		float lv[16], pj[16];

		c->model_mvp_active = 0;
		if (pr) {
			/* Skip deferred fullscreen/environment passes that read render
			 * targets we never fill - lighting, water, fog. Every one of these
			 * also carries localView+proj, which is why the broad detector drew
			 * the WATER PLANE as a flat-white "model" (the giant triangle). */
			int is_lighting = prog_uniform(pr, "diffuseSampler") &&
					  prog_uniform(pr, "normalSampler");
			int is_water = prog_uniform(pr, "reflectionSampler") ||
				       prog_uniform(pr, "refractionSampler") ||
				       prog_uniform(pr, "waterASampler");
			int is_fog = prog_uniform(pr, "fogParams01") ||
				     prog_uniform(pr, "fogParams02");
			/* SSAO reads the depth texture, whose depth lives in z rather than
			 * pixels, so it computes full occlusion; its blur then multiplies
			 * the lit frame by that and blacks it out. */
			int is_ssao = fs_declares(pr, "ssaoParams") ||
				      (fs_declares(pr, "blurSize") && fs_declares(pr, "inputSampler"));
			int is_outline = prog_uniform(pr, "idTexture") && pr->fs &&
					 pr->fs_color_uni >= 0;
			/* The character model: the ONLY geometry pass with a material map. */
			int is_model = prog_uniform(pr, "diffuseMap") &&
				       prog_mat4(pr, "localView", lv) &&
				       prog_mat4(pr, "proj", pj);

			if (is_lighting || is_water || is_fog || is_ssao) {
				static volatile LONG said;
				tr("draw prog=%u vs=%u fs=%u mode=0x%X count=%u fbo=%u: SKIPPED as %s",
				   c->prog, pr->vs_id, pr->fs_id, (unsigned)mode, count, c->draw_fbo,
				   is_water ? "water" : is_fog ? "fog" : is_ssao ? "ssao" : "lighting");
				if (InterlockedIncrement(&said) <= 12)
					gl_log("forward-hack: skipping %s pass (prog %u)",
					       is_water ? "WATER" : is_fog ? "FOG" : is_ssao ? "SSAO"
										: "LIGHTING",
					       c->prog);
				if (is_lighting)
					lighting_passthrough(c);
				return;
			}
			if (is_outline) {
				outline_pass(c, pr);
				return;
			}
			if (is_model && !use_vs) {
				static volatile LONG said;
				mat4_mul(c->model_mvp, pj, lv); /* MVP = proj * localView */
				c->model_mvp_active = 1;
				if (InterlockedIncrement(&said) <= 4)
					gl_log("forward-hack: MODEL pass (prog %u, has diffuseMap) "
					       "- MVP built, drawing to window",
					       c->prog);
			}
		}
	}
	{
		GLuint saved_fbo = c->draw_fbo;
		GlFbo *f;
		if (c->model_mvp_active)
			c->draw_fbo = 0; /* model goes to the window, not the G-buffer */
		c->out_att = prog && prog->fs ? prog->fs_out_loc : 0;
		bind_draw_target(c, &rt, &tex);
		c->rt_up = c->draw_fbo != 0;
		f = fbo_get(c->share, c->draw_fbo);
		if (f) {
			rt_tex = c->out_att > 0 && c->out_att < 8 && f->att[c->out_att]
					 ? f->att[c->out_att]
					 : f->color;
			if (prog && prog->fs_out_diffuse && rt.color)
				g_albedo_tex = rt_tex;
		}
		c->out_att = 0;
		c->draw_fbo = saved_fbo;
	}
	if (!rt.color || rt.width <= 0) {
		tr("draw prog=%u vs=%u fs=%u mode=0x%X count=%u fbo=%u: no colour target, skipped",
		   c->prog, prog ? prog->vs_id : 0, prog ? prog->fs_id : 0, (unsigned)mode, count,
		   c->draw_fbo);
		return;
	}
	tr_touch(rt_tex);
	g_frame_draws++;
	{
		float s = c->rt_rs > 0 ? c->rt_rs : 1.0f;
		int x0 = rs_px(c->vp[0], s), y0 = rs_px(c->vp[1], s);
		int sw = rs_px(c->vp[0] + c->vp[2], s) - x0;
		int sh = rs_px(c->vp[1] + c->vp[3], s) - y0;
		vp_x = x0;
		vp_y = c->rt_up ? y0 : rt.height - (y0 + sh);
		vp_w = c->vp[2] ? sw : rt.width;
		vp_h = c->vp[3] ? sh : rt.height;
		if (vp_h < 0) {
			vp_y = y0;
			vp_h = -vp_h;
		}
	}
	if (vp_w < 2 || vp_h < 2) {
		vp_x = 0;
		vp_y = 0;
		vp_w = rt.width;
		vp_h = rt.height;
	}
	fill_state(c, &st, rt.height);
	gt = draw_tex(c, prog);
	if (gt && gt->pixels) {
		tex.pixels = gt->pixels;
		tex.width = gt->w;
		tex.height = gt->h;
	} else if (!tex.pixels || (use_vs && prog->fs && prog->fs_no_tex))
		memset(&tex, 0, sizeof(tex));
	if (use_vs && prog->fs_color_uni >= 0) {
		const GlUniform *u = &prog->uni[prog->fs_color_uni];
		int k;
		if (u->set && u->v && u->nc >= 3) {
			for (k = 0; k < u->nc && k < 4; k++)
				fcol[k] = u->v[k];
			use_fcol = 1;
		}
	}
	if (use_vs) {
		push_uniforms(prog, prog->vs);
		vkey = (unsigned *)malloc(VS_CACHE * sizeof(unsigned));
		vcache = (VsClip *)malloc(VS_CACHE * sizeof(VsClip));
		if (!vkey || !vcache) {
			free(vkey);
			free(vcache);
			return;
		}
		memset(vkey, 0xff, VS_CACHE * sizeof(unsigned));
		/* Shade every vertex the draw references once, up front and in
		 * parallel; the triangle loop then only reads. Index ranges far wider
		 * than the draw (a few vertices out of a huge shared buffer) stay on
		 * the cache path below. */
		if (!g_tr && count >= VS_PAR_MIN && vs_threads() > 1) {
			unsigned lo = ~0u, hi = 0, k;
			LARGE_INTEGER q0, q1;

			QueryPerformanceCounter(&q0);
			for (k = 0; k < count; k++) {
				unsigned g = (ip && esz ? fetch_index(ip, esz, first + k) : first + k) +
					     (unsigned)basevertex;
				if (g < lo)
					lo = g;
				if (g > hi)
					hi = g;
			}
			if (hi - lo < count * 4u + 4096u) {
				unsigned range = hi - lo + 1, nu = 0;
				pre_slot = (unsigned *)malloc((size_t)range * sizeof(unsigned));
				pre_geom = (unsigned *)malloc((size_t)count * sizeof(unsigned));
				pre_clip = (VsClip *)malloc((size_t)count * sizeof(VsClip));
				if (pre_slot && pre_geom && pre_clip) {
					VsJob jb;
					memset(pre_slot, 0xff, (size_t)range * sizeof(unsigned));
					for (k = 0; k < count; k++) {
						unsigned g = (ip && esz ? fetch_index(ip, esz, first + k)
									: first + k) +
							     (unsigned)basevertex;
						if (pre_slot[g - lo] == ~0u) {
							pre_slot[g - lo] = nu;
							pre_geom[nu++] = g;
						}
					}
					memset(&jb, 0, sizeof(jb));
					jb.c = c;
					jb.p = prog;
					jb.vao = vao;
					jb.geom = pre_geom;
					jb.n = nu;
					jb.clip = pre_clip;
					if (vs_parallel(&jb)) {
						pre_lo = lo;
						pre_range = range;
						if (use_fcol)
							for (k = 0; k < nu; k++) {
								int q;
								for (q = 0; q < 4; q++)
									pre_clip[k].col[q] *= fcol[q];
							}
						g_frame_vs += (LONG)nu;
					}
				}
			}
			QueryPerformanceCounter(&q1);
			g_frame_vs_qpc += q1.QuadPart - q0.QuadPart;
		}
	}

	if (mode == GL_POINTS)
		ntri = count * 2;
	else if (mode == GL_TRIANGLES || mode == GL_PATCHES)
		ntri = count / 3;
	else if (mode == GL_TRIANGLE_STRIP)
		ntri = count >= 3 ? count - 2 : 0;
	else if (mode == GL_TRIANGLE_FAN)
		ntri = count >= 3 ? count - 2 : 0;
	else if (mode == GL_QUADS)
		ntri = (count / 4) * 2;
	else if (mode == GL_TRIANGLES_ADJACENCY)
		ntri = count / 6;
	else if (mode == GL_TRIANGLE_STRIP_ADJACENCY)
		ntri = count >= 6 ? (count / 2) - 2 : 0;
	else if (mode == GL_LINES)
		ntri = (count / 2) * 2;
	else if (mode == GL_LINE_STRIP || mode == GL_LINE_LOOP)
		ntri = count >= 2 ? (count - (mode == GL_LINE_LOOP ? 0 : 1)) * 2 : 0;
	else {
		char tag[40];
		_snprintf(tag, sizeof(tag), "draw mode 0x%x", (unsigned)mode);
		gl_ni(tag);
		g_frame_skip++;
		goto out;
	}
	if (!ntri)
		goto out;
	/* Always-on, capped: the full draw sequence so we can find which program
	 * paints the giant triangle. A fullscreen pass shows as count=3 or 6; the
	 * model shows as model=1. Paired with the shader dumps it names every pass. */
	{
		static volatile LONG dn;
		static GLuint seen[128];
		static int nseen;

		if (InterlockedIncrement(&dn) <= 2500)
			gl_log("draw #%ld prog=%u mode=0x%X count=%u ntri=%u model=%d "
			       "fbo=%u tex=%u(%dx%d) vp=%dx%d",
			       (long)dn, c->prog, (unsigned)mode, count, ntri,
			       c->model_mvp_active, c->draw_fbo, gt ? gt->id : 0,
			       gt ? gt->w : 0, gt ? gt->h : 0, vp_w, vp_h);
		/* First time we draw with a program, dump the uniform names it declared,
		 * so each pass gets a fingerprint we can recognise and route. */
		{
			GlProg *pp = prog_get(c->share, c->prog);
			int s, k;

			for (s = 0; s < nseen; s++)
				if (seen[s] == c->prog)
					break;
			if (s == nseen && pp && nseen < 128) {
				char names[512];
				int off = 0;

				seen[nseen++] = c->prog;
				for (k = 0; k < pp->nuni && off < 460; k++)
					off += _snprintf(names + off, sizeof(names) - off - 1,
							 "%s ", pp->uni[k].name);
				names[off > 0 ? off : 0] = 0;
				gl_log("  prog %u uniforms: %s", c->prog, names);
			}
		}
	}
	/* near-plane clipping can split each triangle in two */
	batch = (SwTri *)malloc((size_t)ntri * (use_vs ? 2 : 1) * sizeof(SwTri));
	if (!batch)
		goto out;
	maxv = count + first;
	for (i = 0; i < ntri; i++) {
		unsigned k;
		unsigned vid[3];
		SwVert sv[3];
		VsClip cv[3];
		if (mode == GL_TRIANGLES || mode == GL_PATCHES) {
			vid[0] = i * 3;
			vid[1] = i * 3 + 1;
			vid[2] = i * 3 + 2;
		} else if (mode == GL_TRIANGLES_ADJACENCY) {
			vid[0] = i * 6;
			vid[1] = i * 6 + 2;
			vid[2] = i * 6 + 4;
		} else if (mode == GL_TRIANGLE_STRIP_ADJACENCY) {
			vid[0] = 2 * i;
			vid[1] = 2 * i + 2;
			vid[2] = 2 * i + 4;
			if (i & 1) {
				unsigned t = vid[1];
				vid[1] = vid[2];
				vid[2] = t;
			}
		} else if (mode == GL_QUADS) {
			unsigned q = (i / 2) * 4;
			if ((i & 1) == 0) {
				vid[0] = q;
				vid[1] = q + 1;
				vid[2] = q + 2;
			} else {
				vid[0] = q;
				vid[1] = q + 2;
				vid[2] = q + 3;
			}
		} else if (mode == GL_TRIANGLE_STRIP) {
			vid[0] = i;
			vid[1] = i + 1;
			vid[2] = i + 2;
			if (i & 1) {
				unsigned t = vid[1];
				vid[1] = vid[2];
				vid[2] = t;
			}
		} else if (mode == GL_POINTS) {
			vid[0] = vid[1] = vid[2] = i / 2;
		} else if (mode == GL_LINES) {
			unsigned seg = i / 2;
			vid[0] = seg * 2;
			vid[1] = seg * 2 + 1;
			vid[2] = vid[1];
		} else if (mode == GL_LINE_STRIP || mode == GL_LINE_LOOP) {
			unsigned seg = i / 2;
			vid[0] = seg;
			vid[1] = seg + 1;
			if (mode == GL_LINE_LOOP && vid[1] >= count)
				vid[1] = 0;
			vid[2] = vid[1];
		} else {
			vid[0] = 0;
			vid[1] = i + 1;
			vid[2] = i + 2;
		}
		for (k = 0; k < 3; k++) {
			unsigned geom = first + vid[k];
			if (ip && esz)
				geom = fetch_index(ip, esz, first + vid[k]);
			geom += (unsigned)basevertex;
			if (use_vs && pre_range && geom - pre_lo < pre_range &&
			    pre_slot[geom - pre_lo] != ~0u) {
				cv[k] = pre_clip[pre_slot[geom - pre_lo]];
			} else if (use_vs) {
				unsigned slot = geom & (VS_CACHE - 1);
				if (vkey[slot] != geom) {
					LARGE_INTEGER q0, q1;

					QueryPerformanceCounter(&q0);
					vs_run(c, prog, vao, geom);
					if (g_tr && !probed) {
						float o[16] = { 0, 0, 0, 0 };
						probed = 1;
						tr("  first vertex %u of prog %u:", geom, c->prog);
						tr_inputs(c, prog, vao, geom);
						glsl_get(prog->vs, "gl_Position", o, 16);
						tr("    gl_Position = (%.3f, %.3f, %.3f, %.3f)", o[0], o[1],
						   o[2], o[3]);
					}
					vs_clip_out(prog, &vcache[slot]);
					QueryPerformanceCounter(&q1);
					g_frame_vs_qpc += q1.QuadPart - q0.QuadPart;
					if (use_fcol) {
						int q;
						for (q = 0; q < 4; q++)
							vcache[slot].col[q] *= fcol[q];
					}
					vkey[slot] = geom;
					g_frame_vs++;
				}
				cv[k] = vcache[slot];
			} else
				vert_from_attr(c, &sv[k], geom, vp_w, vp_h, vp_x, vp_y);
			(void)maxv;
		}
		if (use_vs) {
			if (mode != GL_POINTS && mode != GL_LINES && mode != GL_LINE_STRIP &&
			    mode != GL_LINE_LOOP) {
				nv += (unsigned)clip_tri(c, cv, &batch[nv], vp_w, vp_h, vp_x, vp_y);
				continue;
			}
			for (k = 0; k < 3; k++) {
				if (cv[k].p[3] <= 1e-5f || cv[k].p[2] < -cv[k].p[3])
					break;
				clip_project(c, &cv[k], &sv[k], vp_w, vp_h, vp_x, vp_y);
			}
			if (k < 3)
				continue;
		}
		if (mode == GL_POINTS) {
			static const float ox[2][3] = { { -1, 1, 1 }, { -1, 1, -1 } };
			static const float oy[2][3] = { { -1, -1, 1 }, { -1, 1, 1 } };
			int t = (int)(i & 1);
			for (k = 0; k < 3; k++) {
				sv[k].x += ox[t][k];
				sv[k].y += oy[t][k];
			}
		} else if (mode == GL_LINES || mode == GL_LINE_STRIP || mode == GL_LINE_LOOP) {
			float dx = sv[1].x - sv[0].x, dy = sv[1].y - sv[0].y;
			float len = (float)sqrt(dx * dx + dy * dy);
			float nx, ny;
			if (len < 1.0f)
				len = 1.0f;
			nx = -dy / len;
			ny = dx / len;
			if ((i & 1) == 0) {
				sv[2] = sv[1];
				sv[0].x += nx;
				sv[0].y += ny;
				sv[1].x += nx;
				sv[1].y += ny;
				sv[2].x -= nx;
				sv[2].y -= ny;
			} else {
				SwVert v0 = sv[0], v1 = sv[1];
				sv[0] = v0;
				sv[0].x += nx;
				sv[0].y += ny;
				sv[1] = v1;
				sv[1].x -= nx;
				sv[1].y -= ny;
				sv[2] = v0;
				sv[2].x -= nx;
				sv[2].y -= ny;
			}
		}
		/* Fixed-function model path: drop near-plane-crossing triangles (a
		 * vertex flagged rhw<0) so they don't rasterize as a giant triangle. */
		if (c->model_mvp_active &&
		    (sv[0].rhw < 0.0f || sv[1].rhw < 0.0f || sv[2].rhw < 0.0f))
			continue;
		batch[nv].a = sv[0];
		batch[nv].b = sv[1];
		batch[nv].c = sv[2];
		nv++;
	}
	g_frame_tris += (LONG)nv;
	gl_trace("draw mode=%u ntri=%u->%u rt=%dx%d fbo=%u tex=%ux%u", mode, ntri, nv,
		 rt.width, rt.height, c->draw_fbo, tex.width, tex.height);
	if (g_tr) {
		float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
		unsigned t;
		for (t = 0; t < nv; t++) {
			const SwVert *vv[3] = { &batch[t].a, &batch[t].b, &batch[t].c };
			int q;
			for (q = 0; q < 3; q++) {
				if (vv[q]->x < x0) x0 = vv[q]->x;
				if (vv[q]->x > x1) x1 = vv[q]->x;
				if (vv[q]->y < y0) y0 = vv[q]->y;
				if (vv[q]->y > y1) y1 = vv[q]->y;
			}
		}
		tr("draw prog=%u vs=%u fs=%u mode=0x%X count=%u fbo=%u%s target=%dx%d up=%d "
		   "vp=%d,%d %dx%d tex=%u(%dx%d %s int 0x%X fmt 0x%X type 0x%X%s) shader=%d uv=%s "
		   "col=%s blend=%d depth=%d/%d cull=%d tris %u->%u bbox (%.0f,%.0f)-(%.0f,%.0f)",
		   c->prog, prog ? prog->vs_id : 0, prog ? prog->fs_id : 0, (unsigned)mode, count,
		   c->draw_fbo, c->model_mvp_active ? " MODEL->window" : "", rt.width, rt.height,
		   c->rt_up, c->vp[0], c->vp[1], c->vp[2], c->vp[3], gt ? gt->id : 0,
		   gt ? gt->w : 0, gt ? gt->h : 0, gt && gt->up_fn ? gt->up_fn : "never uploaded",
		   gt ? gt->up_int : 0, gt ? gt->up_fmt : 0, gt ? gt->up_type : 0,
		   gt && gt->up_dropped ? " NOT CONVERTED" : "", use_vs,
		   use_vs && prog->out_uv ? prog->out_uv : "-",
		   use_vs && prog->out_col ? prog->out_col : "-", c->en_blend, c->en_depth,
		   c->depth_mask, st.cull, ntri, nv, x0, y0, x1, y1);
		if (gt)
			tr_sampled(gt->id);
	}
	swrast_triangles(&rt, batch, (int)nv, tex.pixels ? &tex : NULL, &st);
	if (g_tr) {
		size_t i, n = (size_t)rt.width * rt.height, lit = 0;
		swrast_flush();
		for (i = 0; i < n; i++)
			lit += (rt.color[i] & 0xFFFFFFu) != 0;
		tr("  -> into tex %u (%dx%d): %lu of %lu pixels non-black after this draw", rt_tex,
		   rt.width, rt.height, (unsigned long)lit, (unsigned long)n);
	}
	free(batch);
out:
	free(vkey);
	free(vcache);
	free(pre_slot);
	free(pre_geom);
	free(pre_clip);
}

/* One-frame trace. The 20th frame doing real 3D work (50+ draws) is logged call
 * by call ("T" lines in gl_sw.log), then the window and every colour target it
 * touched are saved as gltrace<N>_*.bmp beside the log, N counting traces. F11
 * traces again. */
static void tr_frame_end(GlCtx *c, LONG n)
{
	static int seq;
	char path[64];
	int i;

	if (g_tr) {
		g_tr = 0;
		g_tr_done = 1;
		seq++;
		_snprintf(path, sizeof(path), "gltrace%d_window.bmp", seq);
		bmp_write(path, c->fb.color, c->fb.width, c->fb.height, 0,
			  c->fb.width > 1280 ? 2 : 1);
		for (i = 0; i < g_tr_ntex; i++) {
			GlTex *t = tex_get(c->share, g_tr_tex[i]);
			if (!t || !t->pixels)
				continue;
			_snprintf(path, sizeof(path), "gltrace%d_tex%u_%dx%d.bmp", seq, t->id, t->w,
				  t->h);
			bmp_write(path, t->pixels, t->w, t->h, 1, t->w > 1280 ? 2 : 1);
		}
		for (i = 0; i < g_tr_nsamp; i++) {
			GlTex *t = tex_get(c->share, g_tr_samp[i]);
			if (!t || !t->pixels)
				continue;
			_snprintf(path, sizeof(path), "gltrace%d_samp%u_%dx%d.bmp", seq, t->id, t->w,
				  t->h);
			bmp_write(path, t->pixels, t->w, t->h, 1, t->w > 512 ? t->w / 512 : 1);
		}
		gl_log("frame trace %d done at SwapBuffers #%ld: window + %d colour target(s) "
		       "saved as gltrace%d_*.bmp",
		       seq, (long)n, g_tr_ntex, seq);
		return;
	}
	/* F11 traces the next frame, whatever is on screen (menus included). */
	{
		/* Bit 0 catches a tap made between two (slow) frames. */
		static int f11_was;
		SHORT ks = GetAsyncKeyState(VK_F11);
		int f11 = (ks & 0x8000) != 0;
		int now = (f11 && !f11_was) || (ks & 1);
		f11_was = f11;
		if (now) {
			gl_log("F11: tracing the next frame");
		} else if (g_tr_done || g_frame_draws < 50 || ++g_tr_frames < 20)
			return;
	}
	g_tr = 1;
	g_tr_ntex = 0;
	g_tr_nsamp = 0;
	gl_log("frame trace: tracing the frame after SwapBuffers #%ld (window renders at %dx%d)",
	       (long)n, g_win_w, g_win_h);
	tr("---- frame trace begins after SwapBuffers #%ld; framebuffers:", (long)n);
	for (i = 0; i < c->share->nfbo; i++) {
		GlFbo *f = c->share->fbo[i];
		GlTex *t0;
		if (!f)
			continue;
		t0 = tex_get(c->share, f->color);
		tr("  fbo %u: colour att0..3 = %u %u %u %u, depth %u, draws into tex %u (%dx%d)",
		   f->id, f->att[0], f->att[1], f->att[2], f->att[3], f->depth, f->color,
		   t0 ? t0->w : 0, t0 ? t0->h : 0);
	}
}

/* ---- WGL pixel format: GDI forwards Choose/Set/Swap here. ---- */

BOOL WINAPI wglSwapBuffers(HDC hdc);

static PIXELFORMATDESCRIPTOR g_pfd;
static int g_pfd_set;

int WINAPI wglChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR *pfd)
{
	(void)hdc;
	if (pfd)
		g_pfd = *pfd;
	g_pfd.nSize = sizeof(g_pfd);
	g_pfd.nVersion = 1;
	g_pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	g_pfd.iPixelType = PFD_TYPE_RGBA;
	g_pfd.cColorBits = 32;
	g_pfd.cDepthBits = 24;
	g_pfd.cStencilBits = 8;
	g_pfd.iLayerType = PFD_MAIN_PLANE;
	gl_log("wglChoosePixelFormat -> 1");
	return 1;
}

BOOL WINAPI wglSetPixelFormat(HDC hdc, int fmt, const PIXELFORMATDESCRIPTOR *pfd)
{
	(void)hdc;
	(void)fmt;
	if (pfd)
		g_pfd = *pfd;
	g_pfd_set = 1;
	gl_log("wglSetPixelFormat %d", fmt);
	return TRUE;
}

int WINAPI wglGetPixelFormat(HDC hdc)
{
	(void)hdc;
	return g_pfd_set ? 1 : 0;
}

int WINAPI wglDescribePixelFormat(HDC hdc, int fmt, UINT size, PIXELFORMATDESCRIPTOR *pfd)
{
	(void)hdc;
	(void)fmt;
	if (!pfd || size < sizeof(*pfd))
		return 1;
	memset(pfd, 0, sizeof(*pfd));
	pfd->nSize = sizeof(*pfd);
	pfd->nVersion = 1;
	pfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd->iPixelType = PFD_TYPE_RGBA;
	pfd->cColorBits = 32;
	pfd->cRedBits = pfd->cGreenBits = pfd->cBlueBits = pfd->cAlphaBits = 8;
	pfd->cDepthBits = 24;
	pfd->cStencilBits = 8;
	pfd->iLayerType = PFD_MAIN_PLANE;
	return 1;
}

static GlCtx *ctx_from_rc(HGLRC rc)
{
	return (GlCtx *)rc;
}

HGLRC WINAPI wglCreateContext(HDC hdc)
{
	GlCtx *c;
	int w, h;
	savestate_hooks_install();
	c = (GlCtx *)calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->hdc = hdc;
	c->hwnd = own_window_from_dc(hdc);
	c->share = share_global();
	c->blend_src = GL_SRC_ALPHA;
	c->blend_dst = GL_ONE_MINUS_SRC_ALPHA;
	c->blend_op = GL_FUNC_ADD;
	c->depth_func = GL_LESS;
	{
		int a;
		for (a = 0; a < GL_ATTRS; a++)
			c->cur_attr[a][3] = 1.0f;
	}
	c->sten_mask = 0xffffffffu;
	c->cull_face = GL_BACK;
	c->front_face = GL_CCW;
	c->depth_mask = GL_TRUE;
	c->color_mask[0] = c->color_mask[1] = c->color_mask[2] = c->color_mask[3] = GL_TRUE;
	c->clear_z = 1.0;
	c->unpack_align = 4;
	c->cur_color = 0xffffffffu;
	size_from_dc(c, &w, &h);
	c->vp[0] = 0;
	c->vp[1] = 0;
	c->vp[2] = w;
	c->vp[3] = h;
	c->scissor[0] = 0;
	c->scissor[1] = 0;
	c->scissor[2] = w;
	c->scissor[3] = h;
	swrast_init(&c->fb, c->hwnd, w, h);
	ctx_register(c);
	gl_log("wglCreateContext hdc=%p hwnd=%p %dx%d", hdc, c->hwnd, w, h);
	glfwd_start();
	return (HGLRC)c;
}

HGLRC WINAPI wglCreateContextAttribsARB(HDC hdc, HGLRC share, const int *attr)
{
	HGLRC rc = wglCreateContext(hdc);
	int major = 0, minor = 0, profile = 0, flags = 0;

	(void)share;
	/* What version/profile Haydee demands. If it asks for a core profile or a
	 * version we do not really honour and then checks, that is a boot-stopper we
	 * want named. Attributes are (token,value) pairs terminated by 0. */
	if (attr) {
		int i;
		for (i = 0; attr[i]; i += 2) {
			if (attr[i] == 0x2091) major = attr[i + 1];      /* MAJOR_VERSION */
			else if (attr[i] == 0x2092) minor = attr[i + 1]; /* MINOR_VERSION */
			else if (attr[i] == 0x9126) profile = attr[i + 1]; /* PROFILE_MASK */
			else if (attr[i] == 0x2094) flags = attr[i + 1]; /* CONTEXT_FLAGS */
		}
	}
	gl_log("wglCreateContextAttribsARB share=%p -> ctx=%p requested GL %d.%d "
	       "profile=0x%X flags=0x%X (0x1=core 0x2=compat)",
	       share, (void *)rc, major, minor, profile, flags);
	return rc;
}

BOOL WINAPI wglDeleteContext(HGLRC rc)
{
	GlCtx *c = ctx_from_rc(rc);
	if (!c)
		return FALSE;
	glfwd_ctx_delete(c);
	if (g_cur == c)
		g_cur = NULL;
	swrast_free(&c->fb);
	share_release(c->share);
	ctx_unregister(c);
	free(c);
	return TRUE;
}

BOOL WINAPI wglMakeCurrent(HDC hdc, HGLRC rc)
{
	GlCtx *c = ctx_from_rc(rc);
	LONG n;
	int w = 0, h = 0;
	if (!rc) {
		g_cur = NULL;
		return TRUE;
	}
	if (!c)
		return FALSE;
	c->hdc = hdc;
	c->hwnd = own_window_from_dc(hdc);
	size_from_dc(c, &w, &h);
	ensure_fb(c, w, h);
	g_cur = c;
	n = InterlockedIncrement(&g_makecurrent_n);
	if (n <= 80)
		gl_log("wglMakeCurrent #%ld hdc=%p hwnd=%p %dx%d", n, hdc, c->hwnd, w, h);
	return TRUE;
}

BOOL WINAPI wglShareLists(HGLRC a, HGLRC b)
{
	(void)a;
	(void)b;
	gl_log("wglShareLists");
	return TRUE;
}

HDC WINAPI wglGetCurrentDC(void)
{
	return g_cur ? g_cur->hdc : NULL;
}

HGLRC WINAPI wglGetCurrentContext(void)
{
	return (HGLRC)g_cur;
}

PROC WINAPI wglGetProcAddress(LPCSTR name)
{
	PROC p;
	if (!name)
		return NULL;
	p = gl_lookup_proc(name);
	if (!p)
		gl_log("wglGetProcAddress miss %s", name);
	else
		gl_trace("wglGetProcAddress %s", name);
	return p;
}

PROC WINAPI wglGetDefaultProcAddress(LPCSTR name)
{
	return wglGetProcAddress(name);
}

BOOL WINAPI wglCopyContext(HGLRC a, HGLRC b, UINT mask)
{
	(void)a;
	(void)b;
	(void)mask;
	gl_ni("wglCopyContext");
	return FALSE;
}

HGLRC WINAPI wglCreateLayerContext(HDC hdc, int layer)
{
	(void)layer;
	return wglCreateContext(hdc);
}

BOOL WINAPI wglDescribeLayerPlane(HDC hdc, int fmt, int layer, UINT size, LPLAYERPLANEDESCRIPTOR pd)
{
	(void)hdc;
	(void)fmt;
	(void)layer;
	(void)size;
	if (pd)
		memset(pd, 0, sizeof(*pd));
	return FALSE;
}

int WINAPI wglGetLayerPaletteEntries(HDC a, int b, int c, int d, COLORREF *e)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	return 0;
}

int WINAPI wglSetLayerPaletteEntries(HDC a, int b, int c, int d, const COLORREF *e)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	return 0;
}

BOOL WINAPI wglRealizeLayerPalette(HDC a, int b, BOOL c)
{
	(void)a;
	(void)b;
	(void)c;
	return FALSE;
}

BOOL WINAPI wglSwapLayerBuffers(HDC hdc, UINT planes)
{
	(void)planes;
	return wglSwapBuffers(hdc);
}

DWORD WINAPI wglSwapMultipleBuffers(UINT n, const WGLSWAP *bufs)
{
	UINT i;
	for (i = 0; i < n && bufs; i++)
		wglSwapBuffers(bufs[i].hdc);
	return n;
}

BOOL WINAPI wglUseFontBitmapsA(HDC a, DWORD b, DWORD c, DWORD d)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	gl_ni("wglUseFontBitmapsA");
	return FALSE;
}

BOOL WINAPI wglUseFontBitmapsW(HDC a, DWORD b, DWORD c, DWORD d)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	return FALSE;
}

BOOL WINAPI wglUseFontOutlinesA(HDC a, DWORD b, DWORD c, DWORD d, FLOAT e, FLOAT f, int g,
				LPGLYPHMETRICSFLOAT h)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	(void)f;
	(void)g;
	(void)h;
	return FALSE;
}

BOOL WINAPI wglUseFontOutlinesW(HDC a, DWORD b, DWORD c, DWORD d, FLOAT e, FLOAT f, int g,
				LPGLYPHMETRICSFLOAT h)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	(void)f;
	(void)g;
	(void)h;
	return FALSE;
}

static int wgl_pfd_attrib(int attr)
{
	switch (attr) {
	case WGL_NUMBER_PIXEL_FORMATS_ARB:
		return 1;
	case WGL_DRAW_TO_WINDOW_ARB:
	case WGL_SUPPORT_OPENGL_ARB:
	case WGL_DOUBLE_BUFFER_ARB:
		return 1;
	case WGL_DRAW_TO_BITMAP_ARB:
	case WGL_NEED_PALETTE_ARB:
	case WGL_NEED_SYSTEM_PALETTE_ARB:
	case WGL_SWAP_LAYER_BUFFERS_ARB:
	case WGL_STEREO_ARB:
	case WGL_SUPPORT_GDI_ARB:
	case WGL_TRANSPARENT_ARB:
		return 0;
	case WGL_ACCELERATION_ARB:
		return WGL_FULL_ACCELERATION_ARB;
	case WGL_SWAP_METHOD_ARB:
		return WGL_SWAP_COPY_ARB;
	case WGL_PIXEL_TYPE_ARB:
		return WGL_TYPE_RGBA_ARB;
	case WGL_COLOR_BITS_ARB:
		return 32;
	case WGL_RED_BITS_ARB:
	case WGL_GREEN_BITS_ARB:
	case WGL_BLUE_BITS_ARB:
	case WGL_ALPHA_BITS_ARB:
		return 8;
	case WGL_RED_SHIFT_ARB:
		return 16;
	case WGL_GREEN_SHIFT_ARB:
		return 8;
	case WGL_BLUE_SHIFT_ARB:
		return 0;
	case WGL_ALPHA_SHIFT_ARB:
		return 24;
	case WGL_DEPTH_BITS_ARB:
		return 24;
	case WGL_STENCIL_BITS_ARB:
		return 8;
	case WGL_NUMBER_OVERLAYS_ARB:
	case WGL_NUMBER_UNDERLAYS_ARB:
	case WGL_AUX_BUFFERS_ARB:
	case WGL_ACCUM_BITS_ARB:
		return 0;
	default:
		return 0;
	}
}

BOOL WINAPI wglGetPixelFormatAttribivARB(HDC hdc, int fmt, int layer, UINT n, const int *attr,
					 int *vals)
{
	UINT i;
	(void)hdc;
	(void)layer;
	if (!attr || !vals)
		return FALSE;
	if (fmt == 0) {
		for (i = 0; i < n; i++)
			vals[i] = (attr[i] == WGL_NUMBER_PIXEL_FORMATS_ARB) ? 1 : 0;
		return TRUE;
	}
	for (i = 0; i < n; i++)
		vals[i] = wgl_pfd_attrib(attr[i]);
	return TRUE;
}

BOOL WINAPI wglGetPixelFormatAttribfvARB(HDC hdc, int fmt, int layer, UINT n, const int *attr,
					 FLOAT *vals)
{
	UINT i;
	if (!vals)
		return FALSE;
	for (i = 0; i < n; i++) {
		int v;
		if (!wglGetPixelFormatAttribivARB(hdc, fmt, layer, 1, attr + i, &v))
			return FALSE;
		vals[i] = (FLOAT)v;
	}
	return TRUE;
}

BOOL WINAPI wglChoosePixelFormatARB(HDC hdc, const int *ia, const FLOAT *fa, UINT nmax, int *fmts,
				    UINT *nout)
{
	(void)ia;
	(void)fa;
	if (fmts && nmax)
		fmts[0] = 1;
	if (nout)
		*nout = 1;
	return wglChoosePixelFormat(hdc, NULL) != 0;
}

const char *WINAPI wglGetExtensionsStringARB(HDC hdc)
{
	(void)hdc;
	return "WGL_ARB_create_context WGL_ARB_create_context_profile "
	       "WGL_ARB_pixel_format WGL_EXT_swap_control WGL_ARB_extensions_string";
}

const char *WINAPI wglGetExtensionsStringEXT(void)
{
	return wglGetExtensionsStringARB(NULL);
}

BOOL WINAPI wglSwapIntervalEXT(int interval)
{
	g_swap_interval = interval;
	return TRUE;
}

int WINAPI wglGetSwapIntervalEXT(void)
{
	return g_swap_interval;
}

/* Resolved centrally, so this backend answers to the same keys as the
 * others and picks up D3D9SW_LOAD_VK. Also off GetAsyncKeyState's low bit,
 * which whoever polls first consumes - here that is usually the game. */
static void ss_hotkeys(void)
{
	int k, soak = savestate_soak_action();

	for (k = 0; k < SAVESTATE_SLOTS; k++) {
		int hk = savestate_hotkey(k);

		if (hk == SS_HOTKEY_NONE && k == 0 && soak != SS_SOAK_NOTHING)
			hk = soak == SS_SOAK_LOAD ? SS_HOTKEY_LOAD : SS_HOTKEY_SAVE;
		if (hk == SS_HOTKEY_NONE)
			continue;
		if (hk == SS_HOTKEY_LOAD) {
			if (savestate_load(k))
				gl_log("savestate restored slot %d in %.1f ms", k,
				       savestate_last_ms());
		} else if (savestate_save(k)) {
			/* See the same branch in d3d11_sw.c: a restored thread returns
			 * through the save it was taking, so this is where a working
			 * restore reports in. */
			if (savestate_last_was_restore()) {
				glfwd_restored();
				gl_log("savestate restored slot %d in %.1f ms, resumed "
				       "through the save it was taking",
				       k, savestate_last_ms());
			} else {
				glfwd_saved();
				gl_log("savestate saved slot %d, %.1f MB in %.1f ms", k,
				       savestate_last_mb(), savestate_last_ms());
			}
		}
	}
}

BOOL WINAPI wglSwapBuffers(HDC hdc)
{
	GlCtx *c = cur();
	HWND hwnd = own_window_from_dc(hdc);
	GlCtx *matched;
	SwRast r;
	int w, h;
	LONG n;

	if (hwnd) {
		matched = ctx_for_hwnd(hwnd);
		if (matched)
			c = matched;
	}
	if (!c || hwnd_area(hwnd) <= 4) {
		GlCtx *big = ctx_largest();
		if (big && (!c || big->fb.width * big->fb.height > c->fb.width * c->fb.height)) {
			c = big;
			if (hwnd_area(hwnd) <= 4)
				hwnd = big->hwnd;
		}
	}
	if (!c)
		return FALSE;
	if (glfwd_on()) {
		size_from_dc(c, &w, &h);
		if (g_win_w > 0 && g_win_h > 0 && w * h > 16) {
			w = g_win_w;
			h = g_win_h;
		}
		glfwd_present(hwnd && hwnd_area(hwnd) > 4 ? hwnd : c->hwnd, w, h, g_swap_interval);
		n = InterlockedIncrement(&g_present_n);
		if (n <= 5 || (n % 300) == 1)
			gl_log("SwapBuffers #%ld %dx%d on the GPU host", n, w, h);
		if (glfwd_on()) {
			/* The host's GL objects stay in the present across a restore. */
			savestate_guard();
			ss_hotkeys();
			return TRUE;
		}
	}
	swrast_flush();
	win_size(c, &w, &h);
	ensure_fb(c, w, h);
	savestate_guard();
	ss_hotkeys();
	memset(&r, 0, sizeof(r));
	r.color = c->fb.color;
	r.width = c->fb.width;
	r.height = c->fb.height;
	r.hwnd = own_live_window(hwnd ? hwnd : c->hwnd);
	if (r.hwnd != c->hwnd && c->hwnd && !IsWindow(c->hwnd))
		c->hwnd = r.hwnd;
	swrast_present(&r, r.hwnd);
	n = InterlockedIncrement(&g_present_n);
	{
		static DWORD last;
		DWORD now = GetTickCount();
		double raster_ms = 0;
		LARGE_INTEGER fq;

		/* Taken every frame so each line covers one frame, not thirty. */
		swrast_prof_take(&raster_ms, NULL, NULL, NULL);
		QueryPerformanceFrequency(&fq);
		if (n <= 5 || (n % 30) == 1)
			gl_log("SwapBuffers #%ld %dx%d hwnd=%p fbo=%u cur=%p draws=%ld tris=%ld "
			       "skip=%ld vs-verts=%ld frame=%lums vp=%dx%d vs=%.1fms skin=%.1fms "
			       "raster=%.1fms",
			       n, r.width, r.height, r.hwnd, c->draw_fbo, cur(), g_frame_draws,
			       g_frame_tris, g_frame_skip, g_frame_vs, last ? now - last : 0,
			       cur() ? cur()->vp[2] : 0, cur() ? cur()->vp[3] : 0,
			       (double)g_frame_vs_qpc * 1000.0 / (double)fq.QuadPart,
			       (double)g_frame_tf_qpc * 1000.0 / (double)fq.QuadPart, raster_ms);
		last = now;
	}
	tr_frame_end(c, n);
	g_frame_draws = g_frame_tris = g_frame_skip = g_frame_vs = 0;
	g_frame_vs_qpc = 0;
	g_frame_tf_qpc = 0;
	g_lit_n = 0;
	return TRUE;
}

/* ---- state the forwarding layer (gl_fwd.c) reads ---- */

static GLuint *buf_binding(GlCtx *c, GLenum target);

void *glsw_ctx(void)
{
	return cur();
}

GLuint glsw_next_id(void)
{
	return g_share ? g_share->next_id : 0;
}

void glsw_win_size(int *w, int *h)
{
	*w = g_win_w;
	*h = g_win_h;
}

GLuint glsw_bound_buffer(GLenum target)
{
	GlCtx *c = cur();
	return c ? *buf_binding(c, target) : 0;
}

GLuint glsw_vao_ebo(void)
{
	GlCtx *c = cur();
	GlVao *v = c ? vao_get(c->share, c->vao) : NULL;
	return v ? v->ebo : 0;
}

const unsigned char *glsw_buffer_data(GLuint id, size_t *size)
{
	GlCtx *c = cur();
	GlBuf *b = c ? buf_get(c->share, id) : NULL;
	*size = b ? b->size : 0;
	return b ? b->data : NULL;
}

int glsw_client_attrs(GlswClientAttr *out, int max)
{
	GlCtx *c = cur();
	GlVao *v = c ? vao_get(c->share, c->vao) : NULL;
	int i, n = 0;
	for (i = 0; v && i < GL_ATTRS && n < max; i++) {
		const GlAttrib *at = &v->attr[i];
		if (!at->enabled || at->buf)
			continue;
		out[n].index = (unsigned)i;
		out[n].size = at->size;
		out[n].type = at->type;
		out[n].normalized = at->normalized;
		out[n].stride = at->stride;
		out[n].divisor = at->divisor;
		out[n].ptr = (const void *)(uintptr_t)at->offset;
		n++;
	}
	return n;
}

int glsw_tex_level_size(GLenum target, int level, int *w, int *h)
{
	GlCtx *c = cur();
	GlTex *t;
	(void)target;
	if (!c || c->texunit >= GL_TEXUNITS)
		return 0;
	t = tex_get(c->share, c->tex2d[c->texunit]);
	if (!t)
		return 0;
	*w = (t->rs > 0 ? t->vw : t->w) >> level;
	*h = (t->rs > 0 ? t->vh : t->h) >> level;
	if (*w < 1)
		*w = 1;
	if (*h < 1)
		*h = 1;
	return 1;
}

/* For the frame graph (gl_fwd.c): what a call touches, by the ids the game
 * sees. Every kind of object draws from one id counter, so an id names one
 * object whatever it is. */
GLuint glsw_bound_fbo(int read)
{
	GlCtx *c = cur();
	return c ? (read ? c->read_fbo : c->draw_fbo) : 0;
}

/* Colour attachments first, then depth; *ncolor says where depth starts. */
int glsw_fbo_atts(GLuint fbo, GLuint *out, int cap, int *ncolor)
{
	GlCtx *c = cur();
	GlFbo *f = c ? fbo_get(c->share, fbo) : NULL;
	int i, j, n = 0;

	*ncolor = 0;
	if (!f)
		return 0;
	for (i = 0; i < 8 && n < cap; i++) {
		if (!f->att[i])
			continue;
		for (j = 0; j < n && out[j] != f->att[i]; j++)
			;
		if (j == n)
			out[n++] = f->att[i];
	}
	if (f->color && n < cap) {
		for (j = 0; j < n && out[j] != f->color; j++)
			;
		if (j == n)
			out[n++] = f->color;
	}
	*ncolor = n;
	if (f->depth && n < cap)
		out[n++] = f->depth;
	return n;
}

GLuint glsw_bound_tex(void)
{
	GlCtx *c = cur();
	return c && c->texunit < GL_TEXUNITS ? c->tex2d[c->texunit] : 0;
}

GLuint glsw_cur_prog(void)
{
	GlCtx *c = cur();
	return c ? c->prog : 0;
}

/* Textures behind the current program's samplers. A sampler never given a
 * unit reads unit 0, as in GL. */
int glsw_sampled(GLuint *out, int cap)
{
	GlCtx *c = cur();
	GlProg *p = c ? prog_get(c->share, c->prog) : NULL;
	GlslVar v[64];
	int s, i, j, nv, n = 0;

	if (!p)
		return 0;
	for (s = 0; s < 2; s++) {
		GlslProg *g = s ? p->fs : p->vs;
		nv = g ? glsl_vars(g, v, 64) : 0;
		for (i = 0; i < nv && n < cap; i++) {
			int slot, unit;
			GLuint t;
			if (!v[i].is_sampler)
				continue;
			slot = uni_slot(p, v[i].name);
			unit = slot >= 0 ? p->uni[slot].ivalue : 0;
			t = unit >= 0 && unit < GL_TEXUNITS ? c->tex2d[unit] : 0;
			if (!t)
				continue;
			for (j = 0; j < n && out[j] != t; j++)
				;
			if (j == n)
				out[n++] = t;
		}
	}
	return n;
}

void glsw_obj_desc(GLuint id, char *buf, int cap)
{
	GlCtx *c = cur();
	GlTex *t = c ? tex_get(c->share, id) : NULL;
	GlRbo *r = c && !t ? rbo_get(c->share, id) : NULL;

	if (t)
		_snprintf(buf, (size_t)cap, "tex %dx%d", t->rs > 0 ? t->vw : t->w,
			  t->rs > 0 ? t->vh : t->h);
	else if (r)
		_snprintf(buf, (size_t)cap, "rbo %dx%d", r->w, r->h);
	else
		_snprintf(buf, (size_t)cap, "?");
	buf[cap - 1] = 0;
}

void gl_fwd_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	gl_vlog(fmt, ap);
	va_end(ap);
}

int gameheap_install_imports(void);

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)inst;
	if (reason == DLL_PROCESS_ATTACH) {
		ctx_lock_enter();
		ctx_lock_leave();
		gl_log("process attach (software OpenGL)");
		/* Here, not at the first frame: the game's modules are bound but have
		 * not run their initialisation yet, so their first allocation can
		 * already be ours. Does nothing unless D3D9SW_GAMEHEAP=2. */
		{
			int n = gameheap_install_imports();

			if (n)
				gl_log("gameheap: %d allocator import slot(s) on the private heap", n);
		}
	}
	else if (reason == DLL_PROCESS_DETACH)
		gl_log("process detach (%s) after %ld swaps",
		       reserved ? "process exiting" : "FreeLibrary", g_present_n);
	return TRUE;
}

/* ---- queries ---- */

const GLubyte *APIENTRY glGetString(GLenum name)
{
	{
		/* Always logged (capped): what the game asks for at boot, and our answer,
		 * is a prime no-boot suspect - a version/extension it needs and rejects.
		 * No env var needed, since a Steam launch makes GLSW_TRACE awkward. */
		static volatile LONG q;
		const char *nm = name == GL_VENDOR ? "VENDOR"
				 : name == GL_RENDERER ? "RENDERER"
				 : name == GL_VERSION ? "VERSION"
				 : name == GL_EXTENSIONS ? "EXTENSIONS"
				 : name == GL_SHADING_LANGUAGE_VERSION ? "SHADING_LANG"
								       : "?";
		if (InterlockedIncrement(&q) <= 40)
			gl_log("glGetString(0x%X %s)", (unsigned)name, nm);
	}
	switch (name) {
	case GL_VENDOR:
		return (const GLubyte *)"swrast";
	case GL_RENDERER:
		return (const GLubyte *)"d3d9_sw OpenGL";
	case GL_VERSION:
		return (const GLubyte *)"4.5.0";
	case GL_SHADING_LANGUAGE_VERSION:
		return (const GLubyte *)"4.50";
	case GL_EXTENSIONS:
#define GL_EXT_STRING                                                                              \
	"GL_ARB_framebuffer_object GL_ARB_vertex_buffer_object "                                   \
	"GL_ARB_shader_objects GL_ARB_compute_shader "                                             \
	"GL_EXT_texture_compression_s3tc GL_ARB_map_buffer_range "                                 \
	"GL_ARB_uniform_buffer_object GL_ARB_sampler_objects "                                     \
	"GL_ARB_instanced_arrays GL_ARB_draw_instanced "                                           \
	"GL_ARB_texture_storage GL_ARB_sync "                                                      \
	"GL_ARB_vertex_array_object GL_ARB_shading_language_100"
		/* The GPU host filters for real, so offer what the game would get natively. */
		if (glfwd_on())
			return (const GLubyte *)GL_EXT_STRING " " GL_HOST_EXT;
		return (const GLubyte *)GL_EXT_STRING;
	default:
		return (const GLubyte *)"";
	}
}

const GLubyte *APIENTRY glGetStringi(GLenum name, GLuint i)
{
	if (name == GL_EXTENSIONS && i < GL_EXT_COUNT)
		return (const GLubyte *)k_exts[i];
	if (name == GL_EXTENSIONS && i == GL_EXT_COUNT && glfwd_on())
		return (const GLubyte *)GL_HOST_EXT;
	return (const GLubyte *)"";
}

GLenum APIENTRY glGetError(void)
{
	GlCtx *c = cur();
	GLenum e;
	if (!c)
		return GL_INVALID_OPERATION;
	e = c->err;
	c->err = 0;
	return e;
}

static void get_int(GLenum pname, GLint *p)
{
	GlCtx *c = cur();
	if (!p)
		return;
	*p = 0;
	if (!c)
		return;
	switch (pname) {
	case GL_MAJOR_VERSION:
		*p = 4;
		break;
	case GL_MINOR_VERSION:
		*p = 5;
		break;
	case GL_NUM_EXTENSIONS:
		*p = GL_EXT_COUNT + (glfwd_on() ? 1 : 0);
		break;
	case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT:
		*p = glfwd_on() ? 16 : 1;
		break;
	case GL_MAX_TEXTURE_SIZE:
	case GL_MAX_RENDERBUFFER_SIZE:
	case GL_MAX_CUBE_MAP_TEXTURE_SIZE:
		*p = 16384;
		break;
	case GL_MAX_VERTEX_ATTRIBS:
		*p = GL_ATTRS;
		break;
	case GL_MAX_VERTEX_UNIFORM_COMPONENTS:
		*p = 4096;
		break;
	case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS:
		*p = 2048;
		break;
	case GL_MAX_GEOMETRY_UNIFORM_COMPONENTS:
		*p = 2048;
		break;
	case GL_MAX_UNIFORM_BUFFER_BINDINGS:
		*p = GL_UBOS;
		break;
	case GL_MAX_DRAW_BUFFERS:
	case GL_MAX_COLOR_ATTACHMENTS:
		*p = 8;
		break;
	case GL_MAX_SAMPLES:
		*p = 1;
		break;
	case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS:
		*p = 128;
		break;
	case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS:
		*p = 4;
		break;
	case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS:
		*p = 4;
		break;
	case GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS:
		*p = 1024;
		break;
	case GL_MAX_TEXTURE_IMAGE_UNITS:
	case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
		*p = GL_TEXUNITS;
		break;
	case GL_FRAMEBUFFER_BINDING:
		*p = (GLint)c->draw_fbo;
		break;
	case GL_READ_FRAMEBUFFER_BINDING:
		*p = (GLint)c->read_fbo;
		break;
	case GL_CURRENT_PROGRAM:
		*p = (GLint)c->prog;
		break;
	case GL_VERTEX_ARRAY_BINDING:
		*p = (GLint)c->vao;
		break;
	case GL_ARRAY_BUFFER_BINDING:
		*p = (GLint)c->buf_array;
		break;
	case GL_ELEMENT_ARRAY_BUFFER_BINDING:
		*p = (GLint)c->buf_element;
		break;
	case GL_ACTIVE_TEXTURE:
		*p = (GLint)(GL_TEXTURE0 + c->texunit);
		break;
	case GL_TEXTURE_BINDING_2D:
		*p = (GLint)c->tex2d[c->texunit];
		break;
	case GL_VIEWPORT:
		p[0] = c->vp[0];
		p[1] = c->vp[1];
		p[2] = c->vp[2];
		p[3] = c->vp[3];
		break;
	case GL_SCISSOR_BOX:
		p[0] = c->scissor[0];
		p[1] = c->scissor[1];
		p[2] = c->scissor[2];
		p[3] = c->scissor[3];
		break;
	case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:
		*p = 32;
		break;
	case GL_CONTEXT_PROFILE_MASK:
		*p = GL_CONTEXT_CORE_PROFILE_BIT;
		break;
	default:
		*p = 0;
		break;
	}
}

void APIENTRY glGetIntegerv(GLenum pname, GLint *params)
{
	if (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX) {
		get_int(pname, params);
		return;
	}
	get_int(pname, params);
}

void APIENTRY glGetIntegeri_v(GLenum pname, GLuint i, GLint *params)
{
	if (!params)
		return;
	if (pname == GL_MAX_COMPUTE_WORK_GROUP_COUNT)
		*params = 65535;
	else if (pname == GL_MAX_COMPUTE_WORK_GROUP_SIZE)
		*params = (i == 0) ? 1024 : 1024;
	else
		*params = 0;
}

void APIENTRY glGetFloatv(GLenum pname, GLfloat *params)
{
	GLint i = 0;
	if (!params)
		return;
	glGetIntegerv(pname, &i);
	params[0] = (GLfloat)i;
}

void APIENTRY glGetBooleanv(GLenum pname, GLboolean *params)
{
	GlCtx *c = cur();
	if (!params)
		return;
	*params = GL_FALSE;
	if (!c)
		return;
	switch (pname) {
	case GL_BLEND:
		*params = (GLboolean)c->en_blend;
		break;
	case GL_DEPTH_TEST:
		*params = (GLboolean)c->en_depth;
		break;
	case GL_CULL_FACE:
		*params = (GLboolean)c->en_cull;
		break;
	case GL_SCISSOR_TEST:
		*params = (GLboolean)c->en_scissor;
		break;
	default:
		*params = GL_FALSE;
		break;
	}
}

void APIENTRY glGetDoublev(GLenum pname, GLdouble *params)
{
	GLfloat f = 0;
	if (!params)
		return;
	glGetFloatv(pname, &f);
	params[0] = f;
}

static int cap_slot(GLenum cap, int **slot)
{
	GlCtx *c = cur();
	if (!c)
		return 0;
	switch (cap) {
	case GL_BLEND:
		*slot = &c->en_blend;
		return 1;
	case GL_DEPTH_TEST:
		*slot = &c->en_depth;
		return 1;
	case GL_CULL_FACE:
		*slot = &c->en_cull;
		return 1;
	case GL_SCISSOR_TEST:
		*slot = &c->en_scissor;
		return 1;
	case GL_TEXTURE_2D:
		*slot = &c->en_tex2d;
		return 1;
	case GL_RASTERIZER_DISCARD:
		*slot = &c->en_discard;
		return 1;
	case GL_STENCIL_TEST:
		*slot = &c->en_stencil;
		return 1;
	default:
		return 0;
	}
}

void APIENTRY glEnable(GLenum cap)
{
	int *s;
	if (cap_slot(cap, &s))
		*s = 1;
}

void APIENTRY glDisable(GLenum cap)
{
	int *s;
	if (cap_slot(cap, &s))
		*s = 0;
}

GLboolean APIENTRY glIsEnabled(GLenum cap)
{
	int *s;
	if (!cap_slot(cap, &s))
		return GL_FALSE;
	return *s ? GL_TRUE : GL_FALSE;
}

void APIENTRY glEnablei(GLenum cap, GLuint i)
{
	(void)i;
	glEnable(cap);
}

void APIENTRY glDisablei(GLenum cap, GLuint i)
{
	(void)i;
	glDisable(cap);
}

void APIENTRY glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
	GlCtx *c = cur();
	if (!c)
		return;
	c->vp[0] = x;
	c->vp[1] = y;
	c->vp[2] = w;
	c->vp[3] = h;
	if (!c->draw_fbo && x == 0 && y == 0 && w >= 320 && h >= 200 &&
	    (w != g_win_w || h != g_win_h)) {
		gl_log("window renders at %dx%d (game viewport)", w, h);
		g_win_w = w;
		g_win_h = h;
		rs_update(h);
	}
	tr("viewport %d,%d %dx%d (fbo %u)", x, y, w, h, c->draw_fbo);
}

void APIENTRY glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
	GlCtx *c = cur();
	if (!c)
		return;
	c->scissor[0] = x;
	c->scissor[1] = y;
	c->scissor[2] = w;
	c->scissor[3] = h;
}

void APIENTRY glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
	GlCtx *c = cur();
	if (!c)
		return;
	c->clear_c[0] = r;
	c->clear_c[1] = g;
	c->clear_c[2] = b;
	c->clear_c[3] = a;
}

void APIENTRY glClearDepth(GLclampd z)
{
	GlCtx *c = cur();
	if (c)
		c->clear_z = z;
}

void APIENTRY glClearDepthf(GLfloat z)
{
	glClearDepth(z);
}

void APIENTRY glClearStencil(GLint s)
{
	(void)s;
}

void APIENTRY glClear(GLbitfield mask)
{
	GlCtx *c = cur();
	SwRast rt;
	SwTex tex;
	if (!c)
		return;
	bind_draw_target(c, &rt, &tex);
	if (g_tr) {
		GlFbo *f = fbo_get(c->share, c->draw_fbo);
		tr("clear fbo=%u mask=0x%X colour=(%.2f,%.2f,%.2f,%.2f) depth=%.2f target=%dx%d%s",
		   c->draw_fbo, (unsigned)mask, c->clear_c[0], c->clear_c[1], c->clear_c[2],
		   c->clear_c[3], c->clear_z, rt.width, rt.height, rt.color ? "" : " (no colour target)");
		if (f)
			tr_touch(f->color);
	}
	/* A depth texture is cleared even with no colour attached: games clear
	 * depth alone before binding their colour targets. */
	if ((mask & GL_DEPTH_BUFFER_BIT) && c->draw_fbo) {
		GlFbo *f = fbo_get(c->share, c->draw_fbo);
		GlTex *d = f ? tex_get(c->share, f->depth) : NULL;
		if (d && d->w > 0 && d->h > 0) {
			size_t i, n = (size_t)d->w * d->h;
			swrast_flush();
			if (!d->z)
				d->z = (float *)malloc(n * sizeof(float));
			for (i = 0; d->z && i < n; i++)
				d->z[i] = (float)c->clear_z;
			mask &= ~GL_DEPTH_BUFFER_BIT;
		}
	}
	if (!rt.color)
		return;
	if (mask & GL_COLOR_BUFFER_BIT) {
		uint32_t v = pack_argb(c->clear_c[0], c->clear_c[1], c->clear_c[2], c->clear_c[3]);
		GlFbo *f = c->draw_fbo ? fbo_get(c->share, c->draw_fbo) : NULL;
		int i;
		swrast_clear_color(&rt, v);
		for (i = 0; f && i < 8; i++) {
			GlTex *t = f->att[i] && f->att[i] != f->color ? tex_get(c->share, f->att[i]) : NULL;
			SwRast o;
			/* Only the attachments glDrawBuffers selected: Haydee leaves the
			 * G-buffer attached and clears just attachment 0 between passes. */
			if (f->bufs_set && !(f->bufs & (1u << i)))
				continue;
			if (!t || !t->pixels)
				continue;
			memset(&o, 0, sizeof(o));
			o.color = t->pixels;
			o.width = t->w;
			o.height = t->h;
			swrast_clear_color(&o, v);
		}
	}
	if (mask & GL_DEPTH_BUFFER_BIT)
		swrast_clear_depth(&rt, (float)c->clear_z);
}

void APIENTRY glFinish(void)
{
	swrast_flush();
}

void APIENTRY glFlush(void)
{
	swrast_flush();
}

void APIENTRY glHint(GLenum t, GLenum m)
{
	(void)t;
	(void)m;
}

void APIENTRY glPixelStorei(GLenum pname, GLint param)
{
	GlCtx *c = cur();
	if (c && pname == GL_UNPACK_ALIGNMENT)
		c->unpack_align = param > 0 ? param : 4;
}

void APIENTRY glPixelStoref(GLenum pname, GLfloat param)
{
	glPixelStorei(pname, (GLint)param);
}

void APIENTRY glBlendFunc(GLenum s, GLenum d)
{
	GlCtx *c = cur();
	if (!c)
		return;
	c->blend_src = s;
	c->blend_dst = d;
}

void APIENTRY glBlendFuncSeparate(GLenum rgb_s, GLenum rgb_d, GLenum a_s, GLenum a_d)
{
	(void)a_s;
	(void)a_d;
	glBlendFunc(rgb_s, rgb_d);
}

void APIENTRY glBlendEquation(GLenum mode)
{
	GlCtx *c = cur();
	if (c)
		c->blend_op = mode;
}

void APIENTRY glBlendEquationSeparate(GLenum rgb, GLenum a)
{
	(void)a;
	glBlendEquation(rgb);
}

void APIENTRY glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	GlCtx *c = cur();
	if (!c)
		return;
	c->color_mask[0] = r;
	c->color_mask[1] = g;
	c->color_mask[2] = b;
	c->color_mask[3] = a;
}

void APIENTRY glColorMaski(GLuint i, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	(void)i;
	glColorMask(r, g, b, a);
}

void APIENTRY glDepthFunc(GLenum f)
{
	GlCtx *c = cur();
	if (c)
		c->depth_func = f;
}

void APIENTRY glDepthMask(GLboolean f)
{
	GlCtx *c = cur();
	if (c)
		c->depth_mask = f;
}

void APIENTRY glDepthRange(GLclampd n, GLclampd f)
{
	(void)n;
	(void)f;
}

void APIENTRY glCullFace(GLenum m)
{
	GlCtx *c = cur();
	if (c)
		c->cull_face = m;
}

void APIENTRY glFrontFace(GLenum m)
{
	GlCtx *c = cur();
	if (c)
		c->front_face = m;
}

void APIENTRY glPolygonMode(GLenum face, GLenum mode)
{
	static volatile LONG said;
	(void)face;
	if (mode != GL_FILL && InterlockedIncrement(&said) <= 20)
		gl_log("glPolygonMode 0x%X (not emulated; polygons stay filled)", (unsigned)mode);
	tr("polygon mode 0x%X", (unsigned)mode);
}

void APIENTRY glPolygonOffset(GLfloat f, GLfloat u)
{
	(void)f;
	(void)u;
}

void APIENTRY glLineWidth(GLfloat w)
{
	tr("line width %.1f", w);
}

void APIENTRY glPointSize(GLfloat s)
{
	(void)s;
}

void APIENTRY glStencilFunc(GLenum f, GLint ref, GLuint mask)
{
	(void)f;
	(void)ref;
	(void)mask;
}

void APIENTRY glStencilOpSeparate(GLenum face, GLenum a, GLenum b, GLenum z)
{
	GlCtx *c = cur();
	int w = a != GL_KEEP || b != GL_KEEP || z != GL_KEEP;
	if (!c)
		return;
	if (face != GL_BACK)
		c->sten_writes[0] = w;
	if (face != GL_FRONT)
		c->sten_writes[1] = w;
}

void APIENTRY glStencilOp(GLenum a, GLenum b, GLenum z)
{
	glStencilOpSeparate(GL_FRONT_AND_BACK, a, b, z);
}

void APIENTRY glStencilFuncSeparate(GLenum face, GLenum f, GLint ref, GLuint mask)
{
	(void)face;
	glStencilFunc(f, ref, mask);
}

void APIENTRY glStencilMask(GLuint m)
{
	GlCtx *c = cur();
	if (c)
		c->sten_mask = m;
}

void APIENTRY glStencilMaskSeparate(GLenum face, GLuint m)
{
	(void)face;
	glStencilMask(m);
}

void APIENTRY glPolygonStipple(const GLubyte *m)
{
	(void)m;
}

void APIENTRY glReadBuffer(GLenum m)
{
	(void)m;
}

static void fbo_set_bufs(GlCtx *c, GLsizei n, const GLenum *bufs)
{
	GlFbo *f = c && c->draw_fbo ? fbo_get(c->share, c->draw_fbo) : NULL;
	int i;

	if (!f || !bufs)
		return;
	f->bufs = 0;
	for (i = 0; i < n; i++)
		if (bufs[i] >= 0x8CE0 && bufs[i] < 0x8CE8)
			f->bufs |= 1u << (bufs[i] - 0x8CE0);
	f->bufs_set = 1;
}

void APIENTRY glDrawBuffer(GLenum m)
{
	fbo_set_bufs(cur(), 1, &m);
}

void APIENTRY glDrawBuffers(GLsizei n, const GLenum *bufs)
{
	GlCtx *c = cur();
	fbo_set_bufs(c, n, bufs);
	if (g_tr && c && bufs) {
		char s[160];
		int i, off = 0;
		s[0] = 0;
		for (i = 0; i < n && i < 8 && off < 140; i++)
			off += _snprintf(s + off, sizeof(s) - off, "0x%X ", (unsigned)bufs[i]);
		tr("draw buffers (fbo %u): %s", c->draw_fbo, s);
	}
}

void APIENTRY glActiveTexture(GLenum t)
{
	GlCtx *c = cur();
	if (c && t >= GL_TEXTURE0 && t < GL_TEXTURE0 + GL_TEXUNITS)
		c->texunit = t - GL_TEXTURE0;
}

void APIENTRY glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type,
			   GLvoid *pixels)
{
	GlCtx *c = cur();
	SwRast rt;
	SwTex tex;
	int i, j;
	(void)format;
	(void)type;
	if (!c || !pixels || w <= 0 || h <= 0)
		return;
	(void)tex;
	bind_read_target(c, &rt);
	if (!rt.color)
		return;
	swrast_flush_if_pending(rt.color);
	for (j = 0; j < h; j++) {
		int sy = gl_row(&rt, c->read_fbo != 0, (int)((float)(y + j) * c->read_rs));
		unsigned char *dst = (unsigned char *)pixels + (size_t)j * w * 4;
		if (sy < 0 || sy >= rt.height)
			continue;
		for (i = 0; i < w; i++) {
			int sx = (int)((float)(x + i) * c->read_rs);
			uint32_t p = (sx >= 0 && sx < rt.width) ? rt.color[sy * rt.width + sx] : 0;
			dst[i * 4 + 0] = (unsigned char)((p >> 16) & 0xff);
			dst[i * 4 + 1] = (unsigned char)((p >> 8) & 0xff);
			dst[i * 4 + 2] = (unsigned char)(p & 0xff);
			dst[i * 4 + 3] = (unsigned char)((p >> 24) & 0xff);
		}
	}
}

/* ---- textures ---- */

void APIENTRY glGenTextures(GLsizei n, GLuint *ids)
{
	GlCtx *c = cur();
	int i;
	if (!c || n < 0)
		return;
	for (i = 0; i < n; i++) {
		GlTex **slot = (GlTex **)grow((void **)&c->share->tex, &c->share->ntex,
					     &c->share->ctex, sizeof(GlTex *));
		GlTex *t;
		if (!slot)
			return;
		t = (GlTex *)calloc(1, sizeof(*t));
		t->id = alloc_id(c->share);
		*slot = t;
		c->share->ntex++;
		if (ids)
			ids[i] = t->id;
	}
}

void APIENTRY glDeleteTextures(GLsizei n, const GLuint *ids)
{
	GlCtx *c = cur();
	int i, j;
	if (!c || !ids)
		return;
	for (i = 0; i < n; i++) {
		for (j = 0; j < c->share->ntex; j++) {
			if (c->share->tex[j] && c->share->tex[j]->id == ids[i]) {
				tex_free(c->share->tex[j]);
				c->share->tex[j] = c->share->tex[--c->share->ntex];
				break;
			}
		}
	}
}

void APIENTRY glBindTexture(GLenum target, GLuint id)
{
	GlCtx *c = cur();
	GlTex *t;
	if (!c)
		return;
	if (target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_2D_ARRAY)
		c->tex2d[c->texunit] = id;
	t = tex_get(c->share, id);
	if (t)
		t->target = target;
}

GLboolean APIENTRY glIsTexture(GLuint id)
{
	GlCtx *c = cur();
	return (c && tex_get(c->share, id)) ? GL_TRUE : GL_FALSE;
}

static void tex_alloc(GlTex *t, int w, int h)
{
	size_t n;
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	if (t->pixels && t->w == w && t->h == h)
		return;
	free(t->pixels);
	free(t->z);
	t->z = NULL;
	n = (size_t)w * (size_t)h;
	t->pixels = (uint32_t *)calloc(n, 4);
	t->w = w;
	t->h = h;
}

static void unpack_rgba(GlTex *t, GLenum format, GLenum type, const void *src, int unpack)
{
	int x, y;
	const unsigned char *s = (const unsigned char *)src;
	int bpp = 4;
	(void)unpack;
	if (!src || !t->pixels)
		return;
	if (type != GL_UNSIGNED_BYTE)
		return;
	if (format == GL_RGB || format == GL_BGR)
		bpp = 3;
	else if (format == GL_RED || format == GL_ALPHA || format == GL_LUMINANCE)
		bpp = 1;
	for (y = 0; y < t->h; y++) {
		const unsigned char *row = s + (size_t)y * t->w * bpp;
		uint32_t *d = t->pixels + (size_t)y * t->w;
		for (x = 0; x < t->w; x++) {
			const unsigned char *p = row + (size_t)x * bpp;
			unsigned r = 255, g = 255, b = 255, a = 255;
			if (format == GL_RGBA || format == 0x1908) {
				r = p[0];
				g = p[1];
				b = p[2];
				a = p[3];
			} else if (format == GL_BGRA) {
				b = p[0];
				g = p[1];
				r = p[2];
				a = p[3];
			} else if (format == GL_RGB) {
				r = p[0];
				g = p[1];
				b = p[2];
			} else if (format == GL_BGR) {
				b = p[0];
				g = p[1];
				r = p[2];
			} else if (bpp == 1) {
				r = g = b = p[0];
			}
			d[x] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	t->has = 1;
}

void APIENTRY glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
			   GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
	GlCtx *c = cur();
	GlTex *t;
	int win;
	(void)target;
	(void)internal;
	(void)border;
	if (!c || level != 0)
		return;
	t = tex_get(c->share, c->tex2d[c->texunit]);
	if (!t)
		return;
	win = !pixels && is_window_sized(c, w, h);
	if (win && g_render_h < 0)
		rs_update(h);
	if (win && g_rs < 1.0f) {
		t->rs = g_rs;
		t->vw = w;
		t->vh = h;
		tex_alloc(t, rs_px(w, g_rs), rs_px(h, g_rs));
	} else {
		t->rs = 0;
		tex_alloc(t, w, h);
	}
	if (pixels)
		unpack_rgba(t, format, type, pixels, c->unpack_align);
	tex_note(t, "TexImage2D", (GLenum)internal, format, type,
		 pixels && type != GL_UNSIGNED_BYTE);
	gl_trace("TexImage2D %ux%u fmt=%x", w, h, format);
}

void APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLsizei w,
			      GLsizei h, GLenum format, GLenum type, const GLvoid *pixels)
{
	GlCtx *c = cur();
	GlTex *t;
	int x, y, bpp = 4;
	const unsigned char *s = (const unsigned char *)pixels;
	(void)target;
	if (!c || level != 0 || !pixels)
		return;
	t = tex_get(c->share, c->tex2d[c->texunit]);
	if (!t || !t->pixels)
		return;
	if (format == GL_RGB || format == GL_BGR)
		bpp = 3;
	tex_note(t, "TexSubImage2D", t->up_int, format, type, type != GL_UNSIGNED_BYTE);
	if (type != GL_UNSIGNED_BYTE)
		return;
	for (y = 0; y < h; y++) {
		int dy = yoff + y;
		if (dy < 0 || dy >= t->h)
			continue;
		for (x = 0; x < w; x++) {
			int dx = xoff + x;
			const unsigned char *p;
			unsigned r, g, b, a = 255;
			if (dx < 0 || dx >= t->w)
				continue;
			p = s + ((size_t)y * w + x) * bpp;
			if (bpp == 4) {
				if (format == GL_BGRA) {
					b = p[0];
					g = p[1];
					r = p[2];
					a = p[3];
				} else {
					r = p[0];
					g = p[1];
					b = p[2];
					a = p[3];
				}
			} else {
				r = p[0];
				g = p[1];
				b = p[2];
			}
			t->pixels[dy * t->w + dx] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	t->has = 1;
}

void APIENTRY glTexImage3D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
			   GLsizei depth, GLint border, GLenum format, GLenum type,
			   const void *pixels)
{
	(void)depth;
	glTexImage2D(target, level, internal, w, h, border, format, type, pixels);
}

void APIENTRY glTexStorage2D(GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h)
{
	(void)levels;
	glTexImage2D(target, 0, (GLint)internal, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

void APIENTRY glCompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei w,
				     GLsizei h, GLint border, GLsizei imageSize, const void *data)
{
	GlCtx *c = cur();
	GlTex *t;
	int bc, bsize, bx, by;
	(void)target;
	(void)border;
	(void)imageSize;
	if (!c || level != 0 || !data)
		return;
	t = tex_get(c->share, c->tex2d[c->texunit]);
	if (!t)
		return;
	t->rs = 0;
	tex_alloc(t, w, h);
	switch (internal) {
	case 0x83F0: case 0x83F1: case 0x8C4C: case 0x8C4D: bc = 1; break; /* DXT1 (+sRGB) */
	case 0x83F2: case 0x8C4E: bc = 2; break;                           /* DXT3 */
	case 0x83F3: case 0x8C4F: bc = 3; break;                           /* DXT5 */
	case 0x8DBB: bc = 4; break;                                        /* RGTC1 */
	case 0x8DBD: bc = 5; break;                                        /* RGTC2 */
	case 0x8E8C: case 0x8E8D: bc = 7; break;                           /* BPTC */
	default: bc = 0; break;
	}
	tex_note(t, "CompressedTexImage2D", internal, 0, 0, bc == 0);
	if (!bc)
		return;
	bsize = (bc == 1 || bc == 4) ? 8 : 16;
	for (by = 0; by < (h + 3) / 4; by++) {
		for (bx = 0; bx < (w + 3) / 4; bx++) {
			const unsigned char *blk =
				(const unsigned char *)data +
				((size_t)by * ((w + 3) / 4) + bx) * (size_t)bsize;
			unsigned char rgba[16 * 4], ch[16 * 2];
			int px, py, k;
			if (bc == 1)
				bcdec_bc1(blk, rgba, 16);
			else if (bc == 2)
				bcdec_bc2(blk, rgba, 16);
			else if (bc == 3)
				bcdec_bc3(blk, rgba, 16);
			else if (bc == 7)
				bcdec_bc7(blk, rgba, 16);
			else if (bc == 4) {
				bcdec_bc4(blk, ch, 4);
				for (k = 0; k < 16; k++) {
					rgba[k * 4 + 0] = ch[k];
					rgba[k * 4 + 1] = rgba[k * 4 + 2] = 0;
					rgba[k * 4 + 3] = 255;
				}
			} else {
				bcdec_bc5(blk, ch, 8);
				for (k = 0; k < 16; k++) {
					rgba[k * 4 + 0] = ch[k * 2];
					rgba[k * 4 + 1] = ch[k * 2 + 1];
					rgba[k * 4 + 2] = 0;
					rgba[k * 4 + 3] = 255;
				}
			}
			for (py = 0; py < 4; py++)
				for (px = 0; px < 4; px++) {
					int x = bx * 4 + px, y = by * 4 + py;
					unsigned char *s;
					if (x >= w || y >= h)
						continue;
					s = rgba + (py * 4 + px) * 4;
					t->pixels[y * w + x] =
						((uint32_t)s[3] << 24) | ((uint32_t)s[0] << 16) |
						((uint32_t)s[1] << 8) | s[2];
				}
		}
	}
	t->has = 1;
}

void APIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param)
{
	static volatile LONG said;
	if (pname == GL_TEXTURE_MAX_ANISOTROPY_EXT && InterlockedIncrement(&said) <= 3)
		gl_log("glTexParameter anisotropy %d (target 0x%X)", param, (unsigned)target);
}

void APIENTRY glTexParameterf(GLenum t, GLenum p, GLfloat v)
{
	glTexParameteri(t, p, (GLint)v);
}

void APIENTRY glTexParameteriv(GLenum t, GLenum p, const GLint *v)
{
	if (v)
		glTexParameteri(t, p, v[0]);
}

void APIENTRY glTexParameterfv(GLenum t, GLenum p, const GLfloat *v)
{
	if (v)
		glTexParameterf(t, p, v[0]);
}

void APIENTRY glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels)
{
	GlCtx *c = cur();
	GlTex *t;
	int i;
	(void)target;
	(void)level;
	(void)format;
	(void)type;
	if (!c || !pixels)
		return;
	t = tex_get(c->share, c->tex2d[c->texunit]);
	if (!t || !t->pixels)
		return;
	for (i = 0; i < t->w * t->h; i++) {
		uint32_t p = t->pixels[i];
		unsigned char *d = (unsigned char *)pixels + (size_t)i * 4;
		d[0] = (unsigned char)((p >> 16) & 0xff);
		d[1] = (unsigned char)((p >> 8) & 0xff);
		d[2] = (unsigned char)(p & 0xff);
		d[3] = (unsigned char)((p >> 24) & 0xff);
	}
}

void APIENTRY glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params)
{
	GlCtx *c = cur();
	GlTex *t;
	(void)target;
	(void)level;
	if (!c || !params)
		return;
	t = tex_get(c->share, c->tex2d[c->texunit]);
	*params = 0;
	if (!t)
		return;
	if (pname == GL_TEXTURE_WIDTH)
		*params = t->rs > 0 ? t->vw : t->w;
	else if (pname == GL_TEXTURE_HEIGHT)
		*params = t->rs > 0 ? t->vh : t->h;
	else if (pname == GL_TEXTURE_INTERNAL_FORMAT)
		*params = GL_RGBA8;
}

void APIENTRY glGetTexParameteriv(GLenum t, GLenum p, GLint *params)
{
	(void)t;
	(void)p;
	if (params)
		*params = 0;
}

void APIENTRY glGenerateMipmap(GLenum target)
{
	(void)target;
}

void APIENTRY glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLint x,
				  GLint y, GLsizei w, GLsizei h)
{
	GlCtx *c = cur();
	GlTex *t;
	SwRast rt;
	SwTex tex;
	int i, j;
	(void)target;
	(void)level;
	if (!c)
		return;
	t = tex_get(c->share, c->tex2d[c->texunit]);
	tr("copy-tex fbo %u (%d,%d %dx%d) -> tex %u at %d,%d", c->read_fbo, x, y, w, h,
	   t ? t->id : 0, xoff, yoff);
	if (!t || !t->pixels)
		return;
	(void)tex;
	bind_read_target(c, &rt);
	if (!rt.color)
		return;
	swrast_flush_if_pending(rt.color);
	{
		float ds = t->rs > 0 ? t->rs : 1.0f;
		for (j = 0; j < h; j++) {
			int sy = gl_row(&rt, c->read_fbo != 0, (int)((float)(y + j) * c->read_rs));
			int dy = (int)((float)(yoff + j) * ds);
			if (sy < 0 || sy >= rt.height || dy < 0 || dy >= t->h)
				continue;
			for (i = 0; i < w; i++) {
				int sx = (int)((float)(x + i) * c->read_rs);
				int dx = (int)((float)(xoff + i) * ds);
				if (sx < 0 || sx >= rt.width || dx < 0 || dx >= t->w)
					continue;
				t->pixels[dy * t->w + dx] = rt.color[sy * rt.width + sx];
			}
		}
	}
}

/* ---- buffers / VAO ---- */

static GLuint *buf_binding(GlCtx *c, GLenum target)
{
	if (target == GL_ARRAY_BUFFER)
		return &c->buf_array;
	if (target == GL_ELEMENT_ARRAY_BUFFER)
		return &c->buf_element;
	if (target == GL_UNIFORM_BUFFER)
		return &c->buf_uniform[0];
	if (target == GL_TRANSFORM_FEEDBACK_BUFFER)
		return &c->buf_tf;
	if (target == GL_COPY_READ_BUFFER)
		return &c->buf_copy_r;
	if (target == GL_COPY_WRITE_BUFFER)
		return &c->buf_copy_w;
	return &c->buf_misc;
}

void APIENTRY glGenBuffers(GLsizei n, GLuint *ids)
{
	GlCtx *c = cur();
	int i;
	if (!c)
		return;
	for (i = 0; i < n; i++) {
		GlBuf **slot = (GlBuf **)grow((void **)&c->share->buf, &c->share->nbuf,
					     &c->share->cbuf, sizeof(GlBuf *));
		GlBuf *b;
		if (!slot)
			return;
		b = (GlBuf *)calloc(1, sizeof(*b));
		b->id = alloc_id(c->share);
		*slot = b;
		c->share->nbuf++;
		if (ids)
			ids[i] = b->id;
	}
}

void APIENTRY glDeleteBuffers(GLsizei n, const GLuint *ids)
{
	GlCtx *c = cur();
	int i, j;
	if (!c || !ids)
		return;
	for (i = 0; i < n; i++)
		for (j = 0; j < c->share->nbuf; j++)
			if (c->share->buf[j] && c->share->buf[j]->id == ids[i]) {
				buf_free(c->share->buf[j]);
				c->share->buf[j] = c->share->buf[--c->share->nbuf];
				break;
			}
}

void APIENTRY glBindBuffer(GLenum target, GLuint id)
{
	GlCtx *c = cur();
	GLuint *b;
	GlVao *vao;
	if (!c)
		return;
	b = buf_binding(c, target);
	*b = id;
	if (target == GL_ELEMENT_ARRAY_BUFFER) {
		vao = vao_get(c->share, c->vao);
		if (vao)
			vao->ebo = id;
	}
}

void APIENTRY glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	GlCtx *c = cur();
	GlBuf *b;
	(void)usage;
	if (!c || size < 0)
		return;
	b = buf_get(c->share, *buf_binding(c, target));
	if (!b)
		return;
	free(b->data);
	b->data = size ? (unsigned char *)calloc((size_t)size, 1) : NULL;
	b->size = (size_t)size;
	if (data && b->data)
		memcpy(b->data, data, (size_t)size);
}

void APIENTRY glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	GlCtx *c = cur();
	GlBuf *b;
	if (!c || !data || size < 0)
		return;
	b = buf_get(c->share, *buf_binding(c, target));
	if (!b || !b->data)
		return;
	if ((size_t)offset + (size_t)size > b->size)
		return;
	memcpy(b->data + offset, data, (size_t)size);
}

void *APIENTRY glMapBuffer(GLenum target, GLenum access)
{
	GlCtx *c = cur();
	GlBuf *b;
	(void)access;
	if (!c)
		return NULL;
	b = buf_get(c->share, *buf_binding(c, target));
	if (!b)
		return NULL;
	b->mapped = 1;
	return b->data;
}

void *APIENTRY glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield acc)
{
	unsigned char *p = (unsigned char *)glMapBuffer(target, 0);
	(void)acc;
	(void)length;
	return p ? p + offset : NULL;
}

GLboolean APIENTRY glUnmapBuffer(GLenum target)
{
	GlCtx *c = cur();
	GlBuf *b;
	if (!c)
		return GL_FALSE;
	b = buf_get(c->share, *buf_binding(c, target));
	if (!b)
		return GL_FALSE;
	b->mapped = 0;
	return GL_TRUE;
}

void APIENTRY glBindBufferBase(GLenum target, GLuint index, GLuint id)
{
	GlCtx *c = cur();
	if (!c)
		return;
	if (target == GL_UNIFORM_BUFFER && index < GL_UBOS)
		c->buf_uniform[index] = id;
	else if (target == GL_TRANSFORM_FEEDBACK_BUFFER && index < 4) {
		c->tf_buf[index] = id;
		c->tf_base[index] = 0;
		c->tf_off[index] = 0;
		c->buf_tf = id;
	} else
		glBindBuffer(target, id);
}

void APIENTRY glBindBufferRange(GLenum target, GLuint index, GLuint id, GLintptr off, GLsizeiptr sz)
{
	GlCtx *c = cur();
	(void)sz;
	glBindBufferBase(target, index, id);
	if (c && target == GL_TRANSFORM_FEEDBACK_BUFFER && index < 4)
		c->tf_base[index] = (size_t)off;
}

void APIENTRY glCopyBufferSubData(GLenum src, GLenum dst, GLintptr r, GLintptr w, GLsizeiptr n)
{
	GlCtx *c = cur();
	GlBuf *a, *b;
	if (!c)
		return;
	a = buf_get(c->share, *buf_binding(c, src));
	b = buf_get(c->share, *buf_binding(c, dst));
	if (!a || !b || !a->data || !b->data)
		return;
	if ((size_t)r + (size_t)n > a->size || (size_t)w + (size_t)n > b->size)
		return;
	memcpy(b->data + w, a->data + r, (size_t)n);
}

void APIENTRY glGenVertexArrays(GLsizei n, GLuint *ids)
{
	GlCtx *c = cur();
	int i;
	if (!c)
		return;
	for (i = 0; i < n; i++) {
		GlVao **slot = (GlVao **)grow((void **)&c->share->vao, &c->share->nvao,
					     &c->share->cvao, sizeof(GlVao *));
		GlVao *v;
		if (!slot)
			return;
		v = (GlVao *)calloc(1, sizeof(*v));
		v->id = alloc_id(c->share);
		*slot = v;
		c->share->nvao++;
		if (ids)
			ids[i] = v->id;
	}
}

void APIENTRY glDeleteVertexArrays(GLsizei n, const GLuint *ids)
{
	GlCtx *c = cur();
	int i, j;
	if (!c || !ids)
		return;
	for (i = 0; i < n; i++)
		for (j = 0; j < c->share->nvao; j++)
			if (c->share->vao[j] && c->share->vao[j]->id == ids[i]) {
				free(c->share->vao[j]);
				c->share->vao[j] = c->share->vao[--c->share->nvao];
				break;
			}
}

void APIENTRY glBindVertexArray(GLuint id)
{
	GlCtx *c = cur();
	if (c)
		c->vao = id;
}

void APIENTRY glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride,
				    const void *ptr)
{
	GlCtx *c = cur();
	GlVao *v;
	if (!c || i >= GL_ATTRS)
		return;
	v = vao_get(c->share, c->vao);
	if (!v)
		return;
	v->attr[i].size = size;
	v->attr[i].type = type;
	v->attr[i].normalized = norm;
	v->attr[i].stride = stride;
	v->attr[i].offset = (GLsizeiptr)(uintptr_t)ptr;
	v->attr[i].buf = c->buf_array;
}

void APIENTRY glVertexAttribIPointer(GLuint i, GLint size, GLenum type, GLsizei stride,
				     const void *ptr)
{
	glVertexAttribPointer(i, size, type, GL_FALSE, stride, ptr);
}

void APIENTRY glEnableVertexAttribArray(GLuint i)
{
	GlCtx *c = cur();
	GlVao *v;
	if (!c || i >= GL_ATTRS)
		return;
	v = vao_get(c->share, c->vao);
	if (v)
		v->attr[i].enabled = 1;
}

void APIENTRY glDisableVertexAttribArray(GLuint i)
{
	GlCtx *c = cur();
	GlVao *v;
	if (!c || i >= GL_ATTRS)
		return;
	v = vao_get(c->share, c->vao);
	if (v)
		v->attr[i].enabled = 0;
}

void APIENTRY glVertexAttribDivisor(GLuint i, GLuint d)
{
	GlCtx *c = cur();
	GlVao *v;
	if (!c || i >= GL_ATTRS)
		return;
	v = vao_get(c->share, c->vao);
	if (v)
		v->attr[i].divisor = d;
}

static void cur_attr_set(GLuint i, float x, float y, float z, float w)
{
	GlCtx *c = cur();
	if (!c || i >= GL_ATTRS)
		return;
	c->cur_attr[i][0] = x;
	c->cur_attr[i][1] = y;
	c->cur_attr[i][2] = z;
	c->cur_attr[i][3] = w;
}

void APIENTRY glVertexAttrib1f(GLuint i, GLfloat x)
{
	cur_attr_set(i, x, 0, 0, 1);
}
void APIENTRY glVertexAttrib2f(GLuint i, GLfloat x, GLfloat y)
{
	cur_attr_set(i, x, y, 0, 1);
}
void APIENTRY glVertexAttrib3f(GLuint i, GLfloat x, GLfloat y, GLfloat z)
{
	cur_attr_set(i, x, y, z, 1);
}
void APIENTRY glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
	cur_attr_set(i, x, y, z, w);
}
void APIENTRY glVertexAttrib1fv(GLuint i, const GLfloat *v)
{
	if (v)
		cur_attr_set(i, v[0], 0, 0, 1);
}
void APIENTRY glVertexAttrib2fv(GLuint i, const GLfloat *v)
{
	if (v)
		cur_attr_set(i, v[0], v[1], 0, 1);
}
void APIENTRY glVertexAttrib3fv(GLuint i, const GLfloat *v)
{
	if (v)
		cur_attr_set(i, v[0], v[1], v[2], 1);
}
void APIENTRY glVertexAttrib4fv(GLuint i, const GLfloat *v)
{
	if (v)
		cur_attr_set(i, v[0], v[1], v[2], v[3]);
}
void APIENTRY glVertexAttrib4Nub(GLuint i, GLubyte x, GLubyte y, GLubyte z, GLubyte w)
{
	cur_attr_set(i, x / 255.0f, y / 255.0f, z / 255.0f, w / 255.0f);
}
void APIENTRY glVertexAttrib4Nubv(GLuint i, const GLubyte *v)
{
	if (v)
		cur_attr_set(i, v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f);
}

void APIENTRY glBindAttribLocation(GLuint prog, GLuint index, const GLchar *name)
{
	GlCtx *c = cur();
	GlProg *p = c ? prog_get(c->share, prog) : NULL;
	int i;

	if (!p || !name || index >= GL_ATTRS)
		return;
	for (i = 0; i < p->nbind; i++)
		if (strcmp(p->bind_name[i], name) == 0)
			break;
	if (i == p->nbind) {
		if (p->nbind >= PROG_MAX_IN)
			return;
		p->nbind++;
	}
	strncpy(p->bind_name[i], name, 47);
	p->bind_name[i][47] = 0;
	p->bind_loc[i] = (int)index;
}

GLint APIENTRY glGetAttribLocation(GLuint prog, const GLchar *name)
{
	GlCtx *c = cur();
	GlProg *p = c ? prog_get(c->share, prog) : NULL;

	if (!name)
		return -1;
	if (p && p->vs) {
		int i;
		prog_io(p);
		for (i = 0; i < p->nin; i++)
			if (strcmp(p->in_name[i], name) == 0)
				return p->in_loc[i];
		return -1;
	}
	if (strstr(name, "Position") || strstr(name, "position") || strcmp(name, "in_Position") == 0)
		return 0;
	if (strstr(name, "Coord") || strstr(name, "uv") || strstr(name, "Tex"))
		return 1;
	if (strstr(name, "Color") || strstr(name, "colour"))
		return 2;
	return 0;
}

/* ---- shaders: accept, do not execute ---- */

GLuint APIENTRY glCreateShader(GLenum type)
{
	GlCtx *c = cur();
	GlShader **slot;
	GlShader *s;
	if (!c)
		return 0;
	slot = (GlShader **)grow((void **)&c->share->sh, &c->share->nsh, &c->share->csh,
				 sizeof(GlShader *));
	if (!slot)
		return 0;
	s = (GlShader *)calloc(1, sizeof(*s));
	s->id = alloc_id(c->share);
	s->type = type;
	*slot = s;
	c->share->nsh++;
	gl_log("CreateShader type=%x id=%u", type, s->id);
	return s->id;
}

void APIENTRY glShaderSource(GLuint id, GLsizei count, const GLchar *const *str, const GLint *len)
{
	GlCtx *c = cur();
	GlShader *s;
	int i;
	size_t n = 0;
	if (!c || !str)
		return;
	s = sh_get(c->share, id);
	if (!s)
		return;
	for (i = 0; i < count; i++) {
		size_t L = len && len[i] >= 0 ? (size_t)len[i] : (str[i] ? strlen(str[i]) : 0);
		n += L;
	}
	free(s->src);
	s->src = (char *)malloc(n + 1);
	if (!s->src)
		return;
	n = 0;
	for (i = 0; i < count; i++) {
		size_t L = len && len[i] >= 0 ? (size_t)len[i] : (str[i] ? strlen(str[i]) : 0);
		if (str[i] && L) {
			memcpy(s->src + n, str[i], L);
			n += L;
		}
	}
	s->src[n] = 0;
}

void APIENTRY glCompileShader(GLuint id)
{
	/* Not a compile - we do not run GLSL yet - but the one call where the whole
	 * source is guaranteed present. Dump it so we can see exactly what pipeline
	 * Haydee needs before deciding to approximate it or build a GLSL executor.
	 * Capped so a game that recompiles constantly cannot fill the disk. */
	static volatile LONG dumped;
	GlCtx *c = cur();
	GlShader *s = c ? sh_get(c->share, id) : NULL;

	if (s && s->src && InterlockedIncrement(&dumped) <= 1024) {
		const char *k = s->type == GL_VERTEX_SHADER	 ? "VERTEX"
				: s->type == GL_FRAGMENT_SHADER	 ? "FRAGMENT"
				: s->type == GL_GEOMETRY_SHADER	 ? "GEOMETRY"
				: s->type == GL_COMPUTE_SHADER	 ? "COMPUTE"
				: s->type == GL_TESS_CONTROL_SHADER    ? "TESS_CTRL"
				: s->type == GL_TESS_EVALUATION_SHADER ? "TESS_EVAL"
								       : "OTHER";

		gl_log(">>> SHADER id=%u type=0x%X %s  %u bytes >>>", id,
		       (unsigned)s->type, k, (unsigned)strlen(s->src));
		gl_log_raw(s->src);
		gl_log("<<< end shader id=%u <<<", id);
	}
}

static int sh_attached(GlShare *s, GLuint id)
{
	int j;
	for (j = 0; j < s->nprog; j++)
		if (s->prog[j] && (s->prog[j]->vs_id == id || s->prog[j]->fs_id == id ||
				   s->prog[j]->gs_id == id))
			return 1;
	return 0;
}

/* GL keeps a deleted shader alive while a program has it attached - the usual
 * create / attach / delete / link order depends on it, and freeing at once left
 * every link with no source to compile. */
static void sh_reap(GlShare *s)
{
	int j;
	for (j = 0; j < s->nsh; j++)
		if (s->sh[j] && s->sh[j]->doomed && !sh_attached(s, s->sh[j]->id)) {
			free(s->sh[j]->src);
			free(s->sh[j]);
			s->sh[j] = s->sh[--s->nsh];
			j--;
		}
}

void APIENTRY glDeleteShader(GLuint id)
{
	GlCtx *c = cur();
	GlShader *s = c ? sh_get(c->share, id) : NULL;
	if (!s)
		return;
	s->doomed = 1;
	sh_reap(c->share);
}

GLuint APIENTRY glCreateProgram(void)
{
	GlCtx *c = cur();
	GlProg **slot;
	GlProg *p;
	if (!c)
		return 0;
	slot = (GlProg **)grow((void **)&c->share->prog, &c->share->nprog, &c->share->cprog,
			      sizeof(GlProg *));
	if (!slot)
		return 0;
	p = (GlProg *)calloc(1, sizeof(*p));
	p->id = alloc_id(c->share);
	*slot = p;
	c->share->nprog++;
	return p->id;
}

void APIENTRY glAttachShader(GLuint prog, GLuint sh)
{
	GlCtx *c = cur();
	GlProg *p = c ? prog_get(c->share, prog) : NULL;
	GlShader *s = c ? sh_get(c->share, sh) : NULL;
	{
		static volatile LONG said;
		if (InterlockedIncrement(&said) <= 32)
			gl_log("glAttachShader prog=%u sh=%u p=%p s=%p type=0x%X share=%p",
			       prog, sh, (void *)p, (void *)s,
			       s ? (unsigned)s->type : 0u, (void *)(c ? c->share : NULL));
	}
	if (!p || !s)
		return;
	if (s->type == GL_VERTEX_SHADER)
		p->vs_id = sh;
	else if (s->type == GL_FRAGMENT_SHADER)
		p->fs_id = sh;
	else if (s->type == GL_GEOMETRY_SHADER)
		p->gs_id = sh;
}

void APIENTRY glDetachShader(GLuint prog, GLuint sh)
{
	GlCtx *c = cur();
	GlProg *p = c ? prog_get(c->share, prog) : NULL;
	if (!p)
		return;
	if (p->vs_id == sh)
		p->vs_id = 0;
	if (p->fs_id == sh)
		p->fs_id = 0;
	if (p->gs_id == sh)
		p->gs_id = 0;
	sh_reap(c->share);
}

void APIENTRY glLinkProgram(GLuint id)
{
	GlCtx *c = cur();
	GlProg *p = c ? prog_get(c->share, id) : NULL;
	GlShader *vs, *fs;
	char err[256];

	if (!p)
		return;
	p->linked = 1;

	/* Compile the attached VS + FS source through the software GLSL interpreter.
	 * This is the seam where the shaded raster path gets its runnable programs;
	 * a compile failure just leaves them NULL and the draw falls back. */
	if (p->vs)
		glsl_free(p->vs);
	if (p->fs)
		glsl_free(p->fs);
	p->vs = p->fs = NULL;
	p->compile_failed = 0;
	p->io_ready = 0;

	vs = sh_get(c->share, p->vs_id);
	fs = sh_get(c->share, p->fs_id);
	if (vs && vs->src) {
		p->vs = glsl_compile(vs->src, 'v', err, sizeof err);
		if (!p->vs) {
			p->compile_failed = 1;
			gl_log("glsl: prog %u VS compile FAIL: %s", id, err);
		}
	}
	if (fs && fs->src) {
		p->fs = glsl_compile(fs->src, 'f', err, sizeof err);
		if (!p->fs) {
			p->compile_failed = 1;
			gl_log("glsl: prog %u FS compile FAIL: %s", id, err);
		}
	}
	/* Games may hardcode uniform locations instead of asking for them (Haydee's
	 * skinning program writes its only uniform, joints, at location 0), so slots
	 * are assigned in declaration order, VS then FS, as a driver would. */
	{
		GlslProg *sp[2] = { p->vs, p->fs };
		GlslVar vars[64];
		int s, n, k;
		for (s = 0; s < 2; s++) {
			if (!sp[s])
				continue;
			n = glsl_vars(sp[s], vars, 64);
			for (k = 0; k < n; k++) {
				if (vars[k].qual != GLSL_Q_UNIFORM || uni_slot(p, vars[k].name) >= 0)
					continue;
				if (p->nuni >= (int)(sizeof(p->uni) / sizeof(p->uni[0])))
					break;
				memset(&p->uni[p->nuni], 0, sizeof(p->uni[0]));
				strncpy(p->uni[p->nuni].name, vars[k].name, 47);
				p->nuni++;
			}
		}
	}
	{
		static volatile LONG said;
		if (InterlockedIncrement(&said) <= 64)
			gl_log("glsl: prog %u linked vs=%s fs=%s (vs_id=%u fs_id=%u share=%p)", id,
			       p->vs ? "ok" : (vs ? "FAIL" : "none"),
			       p->fs ? "ok" : (fs ? "FAIL" : "none"),
			       p->vs_id, p->fs_id, (void *)c->share);
	}
}

void APIENTRY glUseProgram(GLuint id)
{
	GlCtx *c = cur();
	if (c)
		c->prog = id;
}

void APIENTRY glDeleteProgram(GLuint id)
{
	GlCtx *c = cur();
	int j;
	if (!c)
		return;
	for (j = 0; j < c->share->nprog; j++)
		if (c->share->prog[j] && c->share->prog[j]->id == id) {
			int k;
			glsl_free(c->share->prog[j]->vs);
			glsl_free(c->share->prog[j]->fs);
			for (k = 0; k < c->share->prog[j]->nuni; k++)
				free(c->share->prog[j]->uni[k].v);
			free(c->share->prog[j]);
			c->share->prog[j] = c->share->prog[--c->share->nprog];
			sh_reap(c->share);
			break;
		}
}

void APIENTRY glGetShaderiv(GLuint id, GLenum pname, GLint *params)
{
	(void)id;
	if (!params)
		return;
	if (pname == GL_COMPILE_STATUS)
		*params = GL_TRUE;
	else if (pname == GL_INFO_LOG_LENGTH)
		*params = 1;
	else
		*params = 0;
}

void APIENTRY glGetProgramiv(GLuint id, GLenum pname, GLint *params)
{
	(void)id;
	if (!params)
		return;
	if (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS)
		*params = GL_TRUE;
	else if (pname == GL_INFO_LOG_LENGTH)
		*params = 1;
	else if (pname == GL_ACTIVE_UNIFORMS || pname == GL_ACTIVE_ATTRIBUTES)
		*params = 0;
	else
		*params = 0;
}

void APIENTRY glGetShaderInfoLog(GLuint id, GLsizei n, GLsizei *len, GLchar *log)
{
	(void)id;
	if (len)
		*len = 0;
	if (log && n > 0)
		log[0] = 0;
}

void APIENTRY glGetProgramInfoLog(GLuint id, GLsizei n, GLsizei *len, GLchar *log)
{
	glGetShaderInfoLog(id, n, len, log);
}

/* Find-or-create a slot for this uniform, remembering the name so a draw can look
 * it up. "csmTransform[2]" returns the slot of csmTransform with element 2. */
GLint APIENTRY glGetUniformLocation(GLuint prog, const GLchar *name)
{
	GlCtx *c = cur();
	GlProg *p = c ? prog_get(c->share, prog) : NULL;
	char base[48];
	int i, elem = 0;

	if (!p || !name) {
		static volatile LONG said;
		if (InterlockedIncrement(&said) <= 50)
			gl_log("uniform lookup '%s' on prog %u: %s", name ? name : "(null)", prog,
			       p ? "no name" : "no such program");
		return -1;
	}
	for (i = 0; name[i] && name[i] != '[' && i < 47; i++)
		base[i] = name[i];
	base[i] = 0;
	if (name[i] == '[')
		elem = atoi(name + i + 1);
	if (elem < 0 || elem > UNI_ELEM_MASK)
		return -1;
	for (i = 0; i < p->nuni; i++)
		if (strcmp(p->uni[i].name, base) == 0)
			return (i << UNI_ELEM_BITS) | elem;
	if (p->nuni >= (int)(sizeof(p->uni) / sizeof(p->uni[0])))
		return -1;
	i = p->nuni++;
	memset(&p->uni[i], 0, sizeof(p->uni[i]));
	strncpy(p->uni[i].name, base, 47);
	{
		static volatile LONG said;
		if (InterlockedIncrement(&said) <= 3000)
			gl_log("uniform lookup prog %u '%s' -> slot %d", prog, name, i);
	}
	return (i << UNI_ELEM_BITS) | elem;
}

/* Store count elements of nc floats at a location in the current program. */
static void uni_store(GLint loc, int nc, int count, const float *v)
{
	GlCtx *c = cur();
	GlProg *p = c ? prog_get(c->share, c->prog) : NULL;
	GlUniform *u;
	int slot, elem, need;

	if (!p || loc < 0 || !v || count <= 0 || nc <= 0) {
		static volatile LONG said;
		if (loc != -1 && InterlockedIncrement(&said) <= 50)
			gl_log("uniform write dropped: loc %d, %d x %d floats, current prog %u%s", loc,
			       count, nc, c ? c->prog : 0, p ? "" : " (not found)");
		return;
	}
	slot = loc >> UNI_ELEM_BITS;
	elem = loc & UNI_ELEM_MASK;
	if (slot >= p->nuni) {
		static volatile LONG said;
		if (InterlockedIncrement(&said) <= 50)
			gl_log("uniform write dropped: loc %d is no slot of prog %u (%d slots)", loc,
			       c->prog, p->nuni);
		return;
	}
	u = &p->uni[slot];
	tr("uniform prog %u %s[%d] <- %d x %d floats (%.3f %.3f %.3f %.3f)", c->prog,
	   p->uni[slot].name, elem, count, nc, v[0], nc > 1 ? v[1] : 0.0f, nc > 2 ? v[2] : 0.0f,
	   nc > 3 ? v[3] : 0.0f);
	if (u->nc != nc) {
		free(u->v);
		u->v = NULL;
		u->nelem = 0;
		u->nc = nc;
	}
	need = elem + count;
	if (need > u->nelem) {
		float *nv = (float *)realloc(u->v, (size_t)need * nc * sizeof(float));
		if (!nv)
			return;
		memset(nv + (size_t)u->nelem * nc, 0, (size_t)(need - u->nelem) * nc * sizeof(float));
		u->v = nv;
		u->nelem = need;
	}
	memcpy(u->v + (size_t)elem * nc, v, (size_t)count * nc * sizeof(float));
	u->set = 1;
	u->ivalue = (int)u->v[0];
	if (nc == 16) {
		memcpy(u->m, u->v, 16 * sizeof(float));
		u->is_mat4 = 1;
	}
}

static void uni_store_i(GLint loc, int nc, int count, const GLint *v)
{
	float tmp[64], *f = tmp;
	int i, n = nc * count;

	if (!v || n <= 0)
		return;
	if (n > 64 && !(f = (float *)malloc((size_t)n * sizeof(float))))
		return;
	for (i = 0; i < n; i++)
		f[i] = (float)v[i];
	uni_store(loc, nc, count, f);
	if (f != tmp)
		free(f);
}

static void uni_store_u(GLint loc, int nc, int count, const GLuint *v)
{
	float tmp[64], *f = tmp;
	int i, n = nc * count;

	if (!v || n <= 0)
		return;
	if (n > 64 && !(f = (float *)malloc((size_t)n * sizeof(float))))
		return;
	for (i = 0; i < n; i++)
		f[i] = (float)v[i];
	uni_store(loc, nc, count, f);
	if (f != tmp)
		free(f);
}

/* Matrices of cols x rows; transpose means v is row-major per element. */
static void uni_store_mat(GLint loc, int cols, int rows, int count, GLboolean t, const GLfloat *v)
{
	int nc = cols * rows, e, col, row;
	float *f;

	if (!v || count <= 0)
		return;
	if (!t) {
		uni_store(loc, nc, count, v);
		return;
	}
	f = (float *)malloc((size_t)count * nc * sizeof(float));
	if (!f)
		return;
	for (e = 0; e < count; e++)
		for (col = 0; col < cols; col++)
			for (row = 0; row < rows; row++)
				f[e * nc + col * rows + row] = v[e * nc + row * cols + col];
	uni_store(loc, nc, count, f);
	free(f);
}

GLuint APIENTRY glGetUniformBlockIndex(GLuint prog, const GLchar *name)
{
	(void)prog;
	(void)name;
	return 0;
}

void APIENTRY glUniformBlockBinding(GLuint p, GLuint i, GLuint b)
{
	(void)p;
	(void)i;
	(void)b;
}

void APIENTRY glBindFragDataLocation(GLuint p, GLuint c, const GLchar *n)
{
	(void)p;
	(void)c;
	(void)n;
}

void APIENTRY glUniform1i(GLint loc, GLint v)
{
	uni_store_i(loc, 1, 1, &v);
}
void APIENTRY glUniform2i(GLint loc, GLint a, GLint b)
{
	GLint v[2] = { a, b };
	uni_store_i(loc, 2, 1, v);
}
void APIENTRY glUniform3i(GLint loc, GLint a, GLint b, GLint c)
{
	GLint v[3] = { a, b, c };
	uni_store_i(loc, 3, 1, v);
}
void APIENTRY glUniform4i(GLint loc, GLint a, GLint b, GLint c, GLint d)
{
	GLint v[4] = { a, b, c, d };
	uni_store_i(loc, 4, 1, v);
}
void APIENTRY glUniform1ui(GLint loc, GLuint v)
{
	uni_store_u(loc, 1, 1, &v);
}
void APIENTRY glUniform1f(GLint loc, GLfloat v)
{
	uni_store(loc, 1, 1, &v);
}
void APIENTRY glUniform2f(GLint loc, GLfloat a, GLfloat b)
{
	GLfloat v[2] = { a, b };
	uni_store(loc, 2, 1, v);
}
void APIENTRY glUniform3f(GLint loc, GLfloat a, GLfloat b, GLfloat c)
{
	GLfloat v[3] = { a, b, c };
	uni_store(loc, 3, 1, v);
}
void APIENTRY glUniform4f(GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d)
{
	GLfloat v[4] = { a, b, c, d };
	uni_store(loc, 4, 1, v);
}
void APIENTRY glUniform1iv(GLint loc, GLsizei n, const GLint *v)
{
	uni_store_i(loc, 1, n, v);
}
void APIENTRY glUniform2iv(GLint loc, GLsizei n, const GLint *v)
{
	uni_store_i(loc, 2, n, v);
}
void APIENTRY glUniform3iv(GLint loc, GLsizei n, const GLint *v)
{
	uni_store_i(loc, 3, n, v);
}
void APIENTRY glUniform4iv(GLint loc, GLsizei n, const GLint *v)
{
	uni_store_i(loc, 4, n, v);
}
void APIENTRY glUniform1uiv(GLint loc, GLsizei n, const GLuint *v)
{
	uni_store_u(loc, 1, n, v);
}
void APIENTRY glUniform1fv(GLint loc, GLsizei n, const GLfloat *v)
{
	uni_store(loc, 1, n, v);
}
void APIENTRY glUniform2fv(GLint loc, GLsizei n, const GLfloat *v)
{
	uni_store(loc, 2, n, v);
}
void APIENTRY glUniform3fv(GLint loc, GLsizei n, const GLfloat *v)
{
	uni_store(loc, 3, n, v);
}
void APIENTRY glUniform4fv(GLint loc, GLsizei n, const GLfloat *v)
{
	uni_store(loc, 4, n, v);
}
void APIENTRY glUniformMatrix2fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v)
{
	uni_store_mat(loc, 2, 2, n, t, v);
}
void APIENTRY glUniformMatrix3fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v)
{
	uni_store_mat(loc, 3, 3, n, t, v);
}
void APIENTRY glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v)
{
	uni_store_mat(loc, 4, 4, n, t, v);
}
void APIENTRY glUniformMatrix2x3fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v)
{
	uni_store_mat(loc, 2, 3, n, t, v);
}
void APIENTRY glUniformMatrix3x2fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v)
{
	uni_store_mat(loc, 3, 2, n, t, v);
}
void APIENTRY glUniformMatrix2x4fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v)
{
	uni_store_mat(loc, 2, 4, n, t, v);
}
void APIENTRY glUniformMatrix4x2fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v)
{
	uni_store_mat(loc, 4, 2, n, t, v);
}
void APIENTRY glUniformMatrix3x4fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v)
{
	uni_store_mat(loc, 3, 4, n, t, v);
}
void APIENTRY glUniformMatrix4x3fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v)
{
	uni_store_mat(loc, 4, 3, n, t, v);
}

void APIENTRY glTransformFeedbackVaryings(GLuint program, GLsizei count, const GLchar *const *varyings,
					  GLenum bufferMode)
{
	GlCtx *c = cur();
	GlProg *p = c ? prog_get(c->share, program) : NULL;
	int i;

	if (!p || !varyings)
		return;
	p->ntf = 0;
	p->tf_mode = bufferMode;
	for (i = 0; i < count && i < PROG_MAX_TF; i++) {
		if (!varyings[i])
			continue;
		strncpy(p->tf_name[p->ntf], varyings[i], 47);
		p->tf_name[p->ntf][47] = 0;
		p->ntf++;
	}
	{
		static volatile LONG said;
		if (InterlockedIncrement(&said) <= 16)
			gl_log("transform feedback: prog %u captures %d varying(s) (%s), first '%s'",
			       program, p->ntf, bufferMode == GL_SEPARATE_ATTRIBS ? "separate" : "interleaved",
			       p->ntf ? p->tf_name[0] : "");
	}
}

void APIENTRY glBeginTransformFeedback(GLenum primitiveMode)
{
	GlCtx *c = cur();
	int i;
	(void)primitiveMode;
	if (!c)
		return;
	c->tf_active = 1;
	for (i = 0; i < 4; i++)
		c->tf_off[i] = 0;
}

void APIENTRY glEndTransformFeedback(void)
{
	GlCtx *c = cur();
	if (c)
		c->tf_active = 0;
}

/* ---- FBO ---- */

void APIENTRY glGenFramebuffers(GLsizei n, GLuint *ids)
{
	GlCtx *c = cur();
	int i;
	if (!c)
		return;
	for (i = 0; i < n; i++) {
		GlFbo **slot = (GlFbo **)grow((void **)&c->share->fbo, &c->share->nfbo,
					     &c->share->cfbo, sizeof(GlFbo *));
		GlFbo *f;
		if (!slot)
			return;
		f = (GlFbo *)calloc(1, sizeof(*f));
		f->id = alloc_id(c->share);
		*slot = f;
		c->share->nfbo++;
		if (ids)
			ids[i] = f->id;
	}
}

void APIENTRY glDeleteFramebuffers(GLsizei n, const GLuint *ids)
{
	GlCtx *c = cur();
	int i, j;
	if (!c || !ids)
		return;
	for (i = 0; i < n; i++)
		for (j = 0; j < c->share->nfbo; j++)
			if (c->share->fbo[j] && c->share->fbo[j]->id == ids[i]) {
				free(c->share->fbo[j]);
				c->share->fbo[j] = c->share->fbo[--c->share->nfbo];
				break;
			}
}

void APIENTRY glBindFramebuffer(GLenum target, GLuint id)
{
	GlCtx *c = cur();
	if (!c)
		return;
	if (target == GL_READ_FRAMEBUFFER)
		c->read_fbo = id;
	else if (target == GL_DRAW_FRAMEBUFFER)
		c->draw_fbo = id;
	else
		c->draw_fbo = c->read_fbo = id;
	tr("bind fbo %u (target 0x%X)", id, (unsigned)target);
	{
		static volatile LONG n;
		GlFbo *f = fbo_get(c->share, id);
		if (InterlockedIncrement(&n) <= 120)
			gl_log("bindFBO %u (target 0x%X) color-tex=%u depth-tex=%u", id,
			       (unsigned)target, f ? f->color : 0, f ? f->depth : 0);
	}
}

void APIENTRY glFramebufferTexture2D(GLenum target, GLenum att, GLenum textarget, GLuint tex,
				     GLint level)
{
	GlCtx *c = cur();
	GlFbo *f;
	GLuint id;
	(void)level;
	if (!c)
		return;
	id = target == GL_READ_FRAMEBUFFER ? c->read_fbo : c->draw_fbo;
	f = fbo_get(c->share, id);
	if (!f)
		return;
	if (att == GL_DEPTH_ATTACHMENT || att == GL_DEPTH_STENCIL_ATTACHMENT) {
		f->depth = tex;
		f->depth_tgt = textarget;
	} else if (att >= GL_COLOR_ATTACHMENT0 && att < GL_COLOR_ATTACHMENT0 + 8) {
		int i;
		f->att[att - GL_COLOR_ATTACHMENT0] = tex;
		/* One colour target until MRT: the lowest attachment, which is where
		 * a shader's first output goes. */
		f->color = 0;
		for (i = 0; i < 8 && !f->color; i++)
			f->color = f->att[i];
		f->color_tgt = textarget;
	}
	tr("attach fbo %u att 0x%X <- tex %u", id, (unsigned)att, tex);
}

void APIENTRY glFramebufferTexture(GLenum target, GLenum att, GLuint tex, GLint level)
{
	glFramebufferTexture2D(target, att, GL_TEXTURE_2D, tex, level);
}

void APIENTRY glFramebufferRenderbuffer(GLenum target, GLenum att, GLenum rbt, GLuint rb)
{
	(void)rbt;
	glFramebufferTexture2D(target, att, GL_TEXTURE_2D, rb, 0);
}

GLenum APIENTRY glCheckFramebufferStatus(GLenum target)
{
	(void)target;
	return GL_FRAMEBUFFER_COMPLETE;
}

void APIENTRY glGenRenderbuffers(GLsizei n, GLuint *ids)
{
	GlCtx *c = cur();
	int i;
	if (!c)
		return;
	for (i = 0; i < n; i++) {
		GlRbo **slot = (GlRbo **)grow((void **)&c->share->rbo, &c->share->nrbo,
					     &c->share->crbo, sizeof(GlRbo *));
		GlRbo *r;
		if (!slot)
			return;
		r = (GlRbo *)calloc(1, sizeof(*r));
		r->id = alloc_id(c->share);
		*slot = r;
		c->share->nrbo++;
		if (ids)
			ids[i] = r->id;
	}
}

void APIENTRY glDeleteRenderbuffers(GLsizei n, const GLuint *ids)
{
	GlCtx *c = cur();
	int i, j;
	if (!c || !ids)
		return;
	for (i = 0; i < n; i++)
		for (j = 0; j < c->share->nrbo; j++)
			if (c->share->rbo[j] && c->share->rbo[j]->id == ids[i]) {
				free(c->share->rbo[j]->color);
				free(c->share->rbo[j]->depth);
				free(c->share->rbo[j]);
				c->share->rbo[j] = c->share->rbo[--c->share->nrbo];
				break;
			}
}

void APIENTRY glBindRenderbuffer(GLenum target, GLuint id)
{
	(void)target;
	(void)id;
}

void APIENTRY glRenderbufferStorage(GLenum target, GLenum internal, GLsizei w, GLsizei h)
{
	(void)target;
	(void)internal;
	(void)w;
	(void)h;
}

void APIENTRY glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,
				GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)
{
	GlCtx *c = cur();
	SwRast src, dst;
	SwTex tex;
	int x, y, sw, sh, dw, dh;
	(void)filter;
	if (!c || !(mask & GL_COLOR_BUFFER_BIT))
		return;
	{
		static volatile LONG n;
		if (InterlockedIncrement(&n) <= 60)
			gl_log("blitFBO read-fbo %u -> draw-fbo %u  (%d,%d..%d,%d)->(%d,%d..%d,%d)",
			       c->read_fbo, c->draw_fbo, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1);
	}
	bind_read_target(c, &src);
	bind_draw_target(c, &dst, &tex);
	tr("blit fbo %u (%d,%d..%d,%d) -> fbo %u (%d,%d..%d,%d) mask=0x%X", c->read_fbo, sx0, sy0,
	   sx1, sy1, c->draw_fbo, dx0, dy0, dx1, dy1, (unsigned)mask);
	if (!src.color || !dst.color)
		return;
	sx0 = rs_px(sx0, c->read_rs);
	sy0 = rs_px(sy0, c->read_rs);
	sx1 = rs_px(sx1, c->read_rs);
	sy1 = rs_px(sy1, c->read_rs);
	dx0 = rs_px(dx0, c->rt_rs);
	dy0 = rs_px(dy0, c->rt_rs);
	dx1 = rs_px(dx1, c->rt_rs);
	dy1 = rs_px(dy1, c->rt_rs);
	/* A reversed destination range mirrors; normalise it onto the source. */
	if (dx1 < dx0) {
		GLint t = dx0;
		dx0 = dx1;
		dx1 = t;
		t = sx0;
		sx0 = sx1;
		sx1 = t;
	}
	if (dy1 < dy0) {
		GLint t = dy0;
		dy0 = dy1;
		dy1 = t;
		t = sy0;
		sy0 = sy1;
		sy1 = t;
	}
	sw = sx1 - sx0;
	sh = sy1 - sy0;
	dw = dx1 - dx0;
	dh = dy1 - dy0;
	if (sw == 0 || sh == 0 || dw < 1 || dh < 1)
		return;
	swrast_flush_if_pending(src.color);
	swrast_flush_if_pending(dst.color);
	for (y = 0; y < dh; y++) {
		int gsy, sy, dy = gl_row(&dst, c->draw_fbo != 0, dy0 + y);
		if (sh < 0)
			gsy = sy0 - 1 - (int)(((double)y + 0.5) * -sh / dh);
		else
			gsy = sy0 + (int)(((double)y + 0.5) * sh / dh);
		sy = gl_row(&src, c->read_fbo != 0, gsy);
		if (sy < 0 || sy >= src.height || dy < 0 || dy >= dst.height)
			continue;
		for (x = 0; x < dw; x++) {
			int sx = sw < 0 ? sx0 - 1 - (int)(((double)x + 0.5) * -sw / dw)
					: sx0 + (int)(((double)x + 0.5) * sw / dw);
			int dx = dx0 + x;
			if (sx < 0 || sx >= src.width || dx < 0 || dx >= dst.width)
				continue;
			dst.color[dy * dst.width + dx] = src.color[sy * src.width + sx];
		}
	}
}

/* ---- draw ---- */

void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	GlCtx *c = cur();
	if (c)
		draw_tris(c, mode, (unsigned)count, 0, NULL, (unsigned)first, 0);
}

void APIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
	GlCtx *c = cur();
	if (c)
		draw_tris(c, mode, (unsigned)count, type, indices, 0, 0);
}

void APIENTRY glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
				  const void *indices)
{
	(void)start;
	(void)end;
	glDrawElements(mode, count, type, indices);
}

void APIENTRY glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei prim)
{
	GLsizei i;
	for (i = 0; i < prim; i++)
		glDrawArrays(mode, first, count);
}

void APIENTRY glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *idx,
				      GLsizei prim)
{
	GLsizei i;
	for (i = 0; i < prim; i++)
		glDrawElements(mode, count, type, idx);
}

void APIENTRY glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices,
				       GLint basevertex)
{
	GlCtx *c = cur();
	if (c)
		draw_tris(c, mode, (unsigned)count, type, indices, 0, basevertex);
}

void APIENTRY glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type,
						const void *indices, GLsizei instancecount,
						GLint basevertex)
{
	GLsizei i;
	for (i = 0; i < instancecount; i++)
		glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
}

void APIENTRY glMultiDrawElements(GLenum mode, const GLsizei *count, GLenum type,
				  const void *const *indices, GLsizei drawcount)
{
	GLsizei i;
	if (!count || !indices)
		return;
	for (i = 0; i < drawcount; i++)
		glDrawElements(mode, count[i], type, indices[i]);
}

void APIENTRY glMultiDrawElementsBaseVertex(GLenum mode, const GLsizei *count, GLenum type,
					    const void *const *indices, GLsizei drawcount,
					    const GLint *basevertex)
{
	GLsizei i;
	if (!count || !indices)
		return;
	for (i = 0; i < drawcount; i++)
		glDrawElementsBaseVertex(mode, count[i], type, indices[i],
					 basevertex ? basevertex[i] : 0);
}

void APIENTRY glBegin(GLenum mode)
{
	GlCtx *c = cur();
	if (!c)
		return;
	c->in_begin = 1;
	c->begin_mode = mode;
	c->nimm = 0;
}

static void imm_push(GlCtx *c, float x, float y, float z)
{
	SwVert *v;
	if (!c || c->nimm >= GL_IMM_MAX)
		return;
	v = &c->imm[c->nimm++];
	v->x = x;
	v->y = y;
	v->z = z;
	v->rhw = 1;
	v->u = c->cur_u;
	v->v = c->cur_v;
	v->color = c->cur_color;
}

void APIENTRY glVertex2f(GLfloat x, GLfloat y)
{
	GlCtx *c = cur();
	if (c)
		imm_push(c, x, y, 0);
}

void APIENTRY glVertex2fv(const GLfloat *v)
{
	if (v)
		glVertex2f(v[0], v[1]);
}

void APIENTRY glVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
	GlCtx *c = cur();
	if (c)
		imm_push(c, x, y, z);
}

void APIENTRY glTexCoord2f(GLfloat s, GLfloat t)
{
	GlCtx *c = cur();
	if (c) {
		c->cur_u = s;
		c->cur_v = t;
	}
}

void APIENTRY glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
	GlCtx *c = cur();
	if (c)
		c->cur_color = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void APIENTRY glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	glColor4ub((GLubyte)(r * 255), (GLubyte)(g * 255), (GLubyte)(b * 255), (GLubyte)(a * 255));
}

void APIENTRY glEnd(void)
{
	GlCtx *c = cur();
	SwRast rt;
	SwTex tex;
	SwState st;
	SwTri *batch;
	GlTex *gt;
	int i, ntri, vp_w, vp_h;
	if (!c || !c->in_begin)
		return;
	c->in_begin = 0;
	bind_draw_target(c, &rt, &tex);
	c->rt_up = c->draw_fbo != 0;
	if (!rt.color || c->nimm < 3)
		return;
	vp_w = rt.width;
	vp_h = rt.height;
	for (i = 0; i < c->nimm; i++) {
		SwVert *v = &c->imm[i];
		float x = v->x, y = v->y;
		if (x <= 2.0f && x >= -2.0f && y <= 2.0f && y >= -2.0f) {
			v->x = (x * 0.5f + 0.5f) * (float)vp_w;
			v->y = (c->rt_up ? 0.5f + y * 0.5f : 0.5f - y * 0.5f) * (float)vp_h;
		} else {
			float s = c->rt_rs > 0 ? c->rt_rs : 1.0f;
			v->x = x * s;
			v->y = c->rt_up ? y * s : (float)vp_h - y * s;
		}
	}
	if (c->begin_mode == GL_QUADS)
		ntri = (c->nimm / 4) * 2;
	else if (c->begin_mode == GL_TRIANGLES)
		ntri = c->nimm / 3;
	else if (c->begin_mode == GL_TRIANGLE_STRIP)
		ntri = c->nimm - 2;
	else
		ntri = 0;
	if (ntri <= 0)
		return;
	batch = (SwTri *)malloc((size_t)ntri * sizeof(SwTri));
	if (!batch)
		return;
	if (c->begin_mode == GL_QUADS) {
		int q, t = 0;
		for (q = 0; q + 3 < c->nimm; q += 4) {
			batch[t].a = c->imm[q];
			batch[t].b = c->imm[q + 1];
			batch[t].c = c->imm[q + 2];
			t++;
			batch[t].a = c->imm[q];
			batch[t].b = c->imm[q + 2];
			batch[t].c = c->imm[q + 3];
			t++;
		}
		ntri = t;
	} else if (c->begin_mode == GL_TRIANGLES) {
		for (i = 0; i < ntri; i++) {
			batch[i].a = c->imm[i * 3];
			batch[i].b = c->imm[i * 3 + 1];
			batch[i].c = c->imm[i * 3 + 2];
		}
	} else {
		for (i = 0; i < ntri; i++) {
			batch[i].a = c->imm[i];
			batch[i].b = c->imm[i + 1];
			batch[i].c = c->imm[i + 2];
		}
	}
	fill_state(c, &st, rt.height);
	gt = bound_tex2d(c);
	if (gt && gt->pixels) {
		tex.pixels = gt->pixels;
		tex.width = gt->w;
		tex.height = gt->h;
	}
	swrast_triangles(&rt, batch, ntri, tex.pixels ? &tex : NULL, &st);
	free(batch);
}

void APIENTRY glDispatchCompute(GLuint x, GLuint y, GLuint z)
{
	gl_ni("glDispatchCompute");
	(void)x;
	(void)y;
	(void)z;
}

void APIENTRY glBindImageTexture(GLuint u, GLuint t, GLint l, GLboolean layered, GLint layer,
				 GLenum acc, GLenum fmt)
{
	(void)u;
	(void)t;
	(void)l;
	(void)layered;
	(void)layer;
	(void)acc;
	(void)fmt;
}

void APIENTRY glMemoryBarrier(GLbitfield b)
{
	(void)b;
}

GLsync APIENTRY glFenceSync(GLenum cond, GLbitfield flags)
{
	(void)cond;
	(void)flags;
	return (GLsync)(uintptr_t)1;
}

GLenum APIENTRY glClientWaitSync(GLsync s, GLbitfield f, GLuint64 t)
{
	(void)s;
	(void)f;
	(void)t;
	return GL_ALREADY_SIGNALED;
}

void APIENTRY glDeleteSync(GLsync s)
{
	(void)s;
}

void APIENTRY glWaitSync(GLsync s, GLbitfield f, GLuint64 t)
{
	(void)s;
	(void)f;
	(void)t;
}

void APIENTRY glGenSamplers(GLsizei n, GLuint *ids)
{
	GlCtx *c = cur();
	int i;
	if (!c)
		return;
	for (i = 0; i < n; i++) {
		GlSamp **slot = (GlSamp **)grow((void **)&c->share->samp, &c->share->nsamp,
					       &c->share->csamp, sizeof(GlSamp *));
		GlSamp *s;
		if (!slot)
			return;
		s = (GlSamp *)calloc(1, sizeof(*s));
		s->id = alloc_id(c->share);
		*slot = s;
		c->share->nsamp++;
		if (ids)
			ids[i] = s->id;
	}
}

void APIENTRY glDeleteSamplers(GLsizei n, const GLuint *ids)
{
	(void)n;
	(void)ids;
}

void APIENTRY glBindSampler(GLuint unit, GLuint id)
{
	GlCtx *c = cur();
	if (c && unit < GL_TEXUNITS)
		c->sampler[unit] = id;
}

void APIENTRY glSamplerParameteri(GLuint s, GLenum p, GLint v)
{
	static volatile LONG said;
	if (p == GL_TEXTURE_MAX_ANISOTROPY_EXT && InterlockedIncrement(&said) <= 3)
		gl_log("glSamplerParameter anisotropy %d (sampler %u)", v, s);
}

void APIENTRY glSamplerParameterf(GLuint s, GLenum p, GLfloat v)
{
	glSamplerParameteri(s, p, (GLint)v);
}
