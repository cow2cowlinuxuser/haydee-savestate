/* Forwarding GL calls to glhost64.exe: launch, the ring writer, and every
 * forwarded call that carries a pointer. The rest are generated into
 * gl_fwd_gen.c. See glhost.h for the design. */
#include "gl_fwd.h"
#include "glhost.h"
#include "glhost_ops.h"
#include "savestate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile LONG g_glfwd;
static GlhHeader *g_h;
static HANDLE g_map, g_ev, g_ev_done, g_proc;
static CRITICAL_SECTION g_lock;
static void *g_last_ctx;
static int g_sent_w, g_sent_h;
static uint32_t g_frames_sent;
static LONG g_rb_seq;
static DWORD g_launch_tick;

static void fw_fail(const char *why)
{
	if (!InterlockedExchange(&g_glfwd, 0))
		return;
	gl_fwd_log("glhost: %s - drawing on the CPU from here on (host says: %s)", why,
		   g_h ? g_h->msg : "-");
	if (g_h) {
		g_h->quit = 1;
		SetEvent(g_ev);
	}
}

static int host_alive(void)
{
	if (!g_proc || WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0) {
		DWORD code = 0;
		if (g_proc)
			GetExitCodeProcess(g_proc, &code);
		gl_fwd_log("glhost: host exit code %lu", (unsigned long)code);
		fw_fail("host process ended");
		return 0;
	}
	if (g_h->state == GLH_FAILED) {
		fw_fail("host failed");
		return 0;
	}
	return 1;
}

/* Waits for the host to move a counter, giving up after the host has been
 * silent for long enough - longer while it is still starting. */
static int host_wait(DWORD *t0)
{
	DWORD limit = g_h->state == GLH_READY ? 10000u : 30000u;
	WaitForSingleObject(g_ev_done, 5);
	if (!host_alive())
		return 0;
	if (GetTickCount() - *t0 > limit) {
		fw_fail(g_h->state == GLH_READY ? "host stopped answering" : "host never came up");
		return 0;
	}
	return 1;
}

static void ring_put(uint32_t pos, const void *src, uint32_t n)
{
	uint8_t *ring = (uint8_t *)g_h + g_h->ring_off;
	uint32_t mask = g_h->ring_bytes - 1, at = pos & mask, first = g_h->ring_bytes - at;

	if (!n)
		return;
	if (first > n)
		first = n;
	memcpy(ring + at, src, first);
	if (n > first)
		memcpy(ring, (const uint8_t *)src + first, n - first);
}

static int ring_reserve(uint32_t need)
{
	DWORD t0 = GetTickCount();
	int spins = 0;

	for (;;) {
		uint32_t used = g_h->ring_head - g_h->ring_tail;
		if (g_h->ring_bytes - used >= need)
			return 1;
		SetEvent(g_ev);
		if (spins++ < 64) {
			SwitchToThread();
			continue;
		}
		if (!host_wait(&t0))
			return 0;
	}
}

/* One frame, call by call (GLSW_FRAMEGRAPH=N). Every record the N-th frame
 * writes gets the time it was written and how long writing took (a full ring
 * shows up there), plus the two places the game blocks on the host: the
 * one-frame-ahead wait after present, and readbacks. The gap before each entry
 * is the game's own work plus this DLL's bookkeeping for that call. glhost
 * times the same records on its side; both count from the same present, so
 * seq lines up between the two files. */
#define FT_MAX 65536
#define FT_WAIT_PRESENT 0x10000u
#define FT_WAIT_READBACK 0x10001u
typedef struct FtRec {
	uint32_t op, bytes;
	uint64_t a0, a1;
	LONGLONG t0, t1;
} FtRec;
static FtRec *g_ft;
static int g_ft_n;
static uint32_t g_ft_frame;
static LONGLONG g_ft_start;

static int ft_on(uint32_t sent)
{
	return g_ft && sent + 1 == g_ft_frame && g_ft_n < FT_MAX;
}

static LONGLONG ft_now(void)
{
	LARGE_INTEGER q;
	QueryPerformanceCounter(&q);
	return q.QuadPart;
}

static void ft_add(uint32_t op, uint32_t bytes, uint64_t a0, uint64_t a1, LONGLONG t0)
{
	FtRec *e = &g_ft[g_ft_n++];
	e->op = op;
	e->bytes = bytes;
	e->a0 = a0;
	e->a1 = a1;
	e->t0 = t0;
	e->t1 = ft_now();
	if (!g_ft_start)
		g_ft_start = t0;
}

static const char *ft_name(uint32_t op)
{
	if (op == FT_WAIT_PRESENT)
		return "wait:host shows previous frame";
	if (op == FT_WAIT_READBACK)
		return "wait:readback";
	if (op < GLH_HAND_N)
		return glh_hand_names[op];
	if (op >= GLH_OP_GEN && op < GLH_OP_GEN_END)
		return glh_gen_names[op - GLH_OP_GEN];
	return "?";
}

/* The frame graph of the same frame: each draw, clear, blit, copy, mipmap
 * build and upload, with the objects it writes and reads. At the end they are
 * grouped into passes - a run of work into one target - and three things fall
 * out: objects the frame read before writing (state carried in from the last
 * frame, which a restore has to get right), targets drawn into without a clear
 * (they keep last frame's pixels), and passes nothing on screen or carried
 * depends on (work that could be dropped). Liveness is worked conservatively:
 * a pass called dead is dead; a pass called live may not be. */
#define FG_MAX 16384
#define FG_IO 12
#define FG_RES 2048
#define FG_SCREEN 0xFFFFFFFFu
/* Buffers written by transform feedback (Haydee skins that way) or compute.
 * Buffers are not followed, so this counts as always live. */
#define FG_BUFFERS 0xFFFFFFFEu
enum { FG_DRAW, FG_CLEAR, FG_BLIT, FG_COPY, FG_MIP, FG_UPLOAD };
typedef struct FgEv {
	uint8_t kind, nw, nr;
	GLuint tgt, prog;
	GLuint w[FG_IO], r[FG_IO];
	int rec;
} FgEv;
typedef struct FgRes {
	GLuint id;
	uint8_t written, carried, kept, live;
} FgRes;
static FgEv *g_fg;
static int g_fg_n;

/* An FBO's attachments (the window for 0). With a clear mask, only the kinds
 * it clears. */
static int fg_fbo_out(GLuint fbo, GLuint *w, GLbitfield mask)
{
	GLuint a[FG_IO];
	int nc, n, i, k = 0;

	if (!fbo) {
		w[0] = FG_SCREEN;
		return 1;
	}
	n = glsw_fbo_atts(fbo, a, FG_IO, &nc);
	for (i = 0; i < n; i++) {
		if (i < nc && !(mask & GL_COLOR_BUFFER_BIT))
			continue;
		if (i >= nc && !(mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)))
			continue;
		w[k++] = a[i];
	}
	return k;
}

static void fg_note(uint32_t op, const uint64_t *a, int na)
{
	const GLbitfield all = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;
	static int tf;
	FgEv *e;

	if (op == GLH_OP_glBeginTransformFeedback || op == GLH_OP_glEndTransformFeedback) {
		tf = op == GLH_OP_glBeginTransformFeedback;
		return;
	}
	if (!g_fg || g_fg_n >= FG_MAX || !g_ft_n)
		return;
	e = &g_fg[g_fg_n];
	memset(e, 0, sizeof(*e));
	switch (op) {
	case GLH_OP_DRAW:
	case GLH_OP_glDispatchCompute:
		e->kind = FG_DRAW;
		e->tgt = op == GLH_OP_DRAW ? glsw_bound_fbo(0) : FG_BUFFERS;
		if (op == GLH_OP_DRAW)
			e->nw = (uint8_t)fg_fbo_out(e->tgt, e->w, all);
		if (tf || op != GLH_OP_DRAW)
			e->w[e->nw++] = FG_BUFFERS;
		e->nr = (uint8_t)glsw_sampled(e->r, FG_IO);
		e->prog = glsw_cur_prog();
		break;
	case GLH_OP_glClear:
		e->kind = FG_CLEAR;
		e->tgt = glsw_bound_fbo(0);
		e->nw = (uint8_t)fg_fbo_out(e->tgt, e->w, na > 0 ? (GLbitfield)a[0] : all);
		break;
	case GLH_OP_glBlitFramebuffer:
		e->kind = FG_BLIT;
		e->tgt = glsw_bound_fbo(0);
		e->nw = (uint8_t)fg_fbo_out(e->tgt, e->w, na > 8 ? (GLbitfield)a[8] : all);
		e->nr = (uint8_t)fg_fbo_out(glsw_bound_fbo(1), e->r, na > 8 ? (GLbitfield)a[8] : all);
		break;
	case GLH_OP_glCopyTexSubImage2D:
		e->kind = FG_COPY;
		e->tgt = glsw_bound_tex();
		e->w[0] = e->tgt;
		e->nw = e->tgt ? 1 : 0;
		e->nr = (uint8_t)fg_fbo_out(glsw_bound_fbo(1), e->r, GL_COLOR_BUFFER_BIT);
		break;
	case GLH_OP_glGenerateMipmap:
		e->kind = FG_MIP;
		e->tgt = e->w[0] = e->r[0] = glsw_bound_tex();
		e->nw = e->nr = e->tgt ? 1 : 0;
		break;
	case GLH_OP_TEX_IMAGE:
		e->kind = FG_UPLOAD;
		e->tgt = e->w[0] = glsw_bound_tex();
		e->nw = e->tgt ? 1 : 0;
		break;
	default:
		return;
	}
	e->rec = g_ft_n - 1;
	g_fg_n++;
}

static FgRes *fg_res(FgRes *t, int *n, GLuint id)
{
	int i;
	for (i = 0; i < *n; i++)
		if (t[i].id == id)
			return &t[i];
	if (*n >= FG_RES)
		return NULL;
	memset(&t[*n], 0, sizeof(t[0]));
	t[*n].id = id;
	return &t[(*n)++];
}

static void fg_desc(GLuint id, char *buf, int cap)
{
	if (id == FG_SCREEN)
		lstrcpynA(buf, "screen", cap);
	else if (id == FG_BUFFERS)
		lstrcpynA(buf, "buffers", cap);
	else
		glsw_obj_desc(id, buf, cap);
}

/* Passes: consecutive draws, clears and blits into one framebuffer; copies and
 * mipmap builds per texture; consecutive uploads together. */
static int fg_same_pass(const FgEv *a, const FgEv *b)
{
	int fa = a->kind <= FG_BLIT, fb = b->kind <= FG_BLIT;
	if (fa || fb)
		return fa && fb && a->tgt == b->tgt;
	if (a->kind != b->kind)
		return 0;
	return a->kind == FG_UPLOAD || a->tgt == b->tgt;
}

static void fg_dump(double us)
{
	static FgRes res[FG_RES];
	static int p0[FG_MAX + 1], live[FG_MAX];
	int nres = 0, np = 0, i, j, k, p, ndead = 0;
	double dead_ms = 0;
	char path[64], d[48];
	FILE *out;

	if (!g_fg)
		return;
	for (i = 0; i < g_fg_n; i++)
		if (!i || !fg_same_pass(&g_fg[i - 1], &g_fg[i]))
			p0[np++] = i;
	p0[np] = g_fg_n;
	/* Forward: what each object held when the frame first touched it. */
	for (i = 0; i < g_fg_n; i++) {
		FgEv *e = &g_fg[i];
		for (j = 0; j < e->nr; j++) {
			FgRes *r = fg_res(res, &nres, e->r[j]);
			if (r && !r->written)
				r->carried = 1;
		}
		for (j = 0; j < e->nw; j++) {
			FgRes *r = fg_res(res, &nres, e->w[j]);
			if (r && !r->written && e->kind != FG_CLEAR && e->kind != FG_UPLOAD &&
			    e->kind != FG_BLIT && r->id != FG_BUFFERS)
				r->kept = 1;
			if (r)
				r->written = 1;
		}
	}
	/* Backward: the screen and everything the next frame inherits is live, and
	 * so is whatever a live pass reads. */
	for (k = 0; k < nres; k++)
		res[k].live = res[k].id == FG_SCREEN || res[k].id == FG_BUFFERS || res[k].carried ||
			      res[k].kept;
	for (p = np - 1; p >= 0; p--) {
		live[p] = 0;
		for (i = p0[p]; i < p0[p + 1] && !live[p]; i++)
			for (j = 0; j < g_fg[i].nw; j++) {
				FgRes *r = fg_res(res, &nres, g_fg[i].w[j]);
				if (r && r->live)
					live[p] = 1;
			}
		if (live[p])
			for (i = p0[p]; i < p0[p + 1]; i++)
				for (j = 0; j < g_fg[i].nr; j++) {
					FgRes *r = fg_res(res, &nres, g_fg[i].r[j]);
					if (r)
						r->live = 1;
				}
	}
	snprintf(path, sizeof(path), "glframe_%u_graph.txt", g_ft_frame);
	out = fopen(path, "w");
	for (p = 0; p < np; p++) {
		FgEv *e = &g_fg[p0[p]];
		int cnt[6] = { 0 }, nw = 0, nr = 0, nprog = 0;
		GLuint w[64], r[64], pr[8];
		LONGLONG t_end = g_ft[g_fg[p0[p + 1] - 1].rec].t1;
		LONGLONG t_beg = p ? g_ft[g_fg[p0[p] - 1].rec].t1 : g_ft_start;
		double ms = (double)(t_end - t_beg) * us / 1000.0;

		for (i = p0[p]; i < p0[p + 1]; i++) {
			FgEv *x = &g_fg[i];
			cnt[x->kind]++;
			for (j = 0; j < x->nw; j++) {
				for (k = 0; k < nw && w[k] != x->w[j]; k++)
					;
				if (k == nw && nw < 64)
					w[nw++] = x->w[j];
			}
			for (j = 0; j < x->nr; j++) {
				for (k = 0; k < nr && r[k] != x->r[j]; k++)
					;
				if (k == nr && nr < 64)
					r[nr++] = x->r[j];
			}
			if (x->prog) {
				for (k = 0; k < nprog && pr[k] != x->prog; k++)
					;
				if (k == nprog && nprog < 8)
					pr[nprog++] = x->prog;
			}
		}
		if (!live[p]) {
			ndead++;
			dead_ms += ms;
		}
		if (!out)
			continue;
		if (e->kind <= FG_BLIT)
			fprintf(out, "pass %d: into %s %u", p, e->tgt ? "fbo" : "the window", e->tgt);
		else if (e->kind == FG_UPLOAD)
			fprintf(out, "pass %d: uploads", p);
		else
			fprintf(out, "pass %d: %s of %u", p, e->kind == FG_COPY ? "copy" : "mipmaps",
				e->tgt);
		fprintf(out, " - %s, %.2f ms;", live[p] ? "live" : "DEAD", ms);
		{
			static const char *const kn[] = { "draw", "clear", "blit", "copy", "mipmap",
							  "upload" };
			for (k = 0; k < 6; k++)
				if (cnt[k])
					fprintf(out, " %d %s%s", cnt[k], kn[k], cnt[k] > 1 ? "s" : "");
		}
		if (nprog) {
			fprintf(out, "; programs");
			for (k = 0; k < nprog; k++)
				fprintf(out, " %u", pr[k]);
		}
		fprintf(out, "\n");
		for (k = 0; k < nw; k++) {
			FgRes *x = fg_res(res, &nres, w[k]);
			fg_desc(w[k], d, sizeof(d));
			fprintf(out, "    writes %-10u %-16s%s\n", w[k], d,
				x && x->kept ? " (first drawn into without a clear: keeps last frame's)" : "");
		}
		for (k = 0; k < nr; k++) {
			FgRes *x = fg_res(res, &nres, r[k]);
			fg_desc(r[k], d, sizeof(d));
			fprintf(out, "    reads  %-10u %-16s%s\n", r[k], d,
				x && x->carried ? " (from before this frame)" : "");
		}
	}
	if (out) {
		fprintf(out, "\ncarried in - read before this frame wrote them:\n");
		for (k = 0; k < nres; k++)
			if (res[k].carried) {
				fg_desc(res[k].id, d, sizeof(d));
				fprintf(out, "    %-10u %-16s%s\n", res[k].id, d,
					res[k].written ? " (rewritten later this frame)" : " (only read)");
			}
		fprintf(out, "\nkept - drawn into before any clear, so last frame's pixels survive:\n");
		for (k = 0; k < nres; k++)
			if (res[k].kept) {
				fg_desc(res[k].id, d, sizeof(d));
				fprintf(out, "    %-10u %s\n", res[k].id, d);
			}
		fclose(out);
	}
	{
		int ncar = 0, nkept = 0;
		for (k = 0; k < nres; k++) {
			ncar += res[k].carried;
			nkept += res[k].kept;
		}
		gl_fwd_log("framegraph %u: %d passes over %d objects; %d carried in from the last "
			   "frame, %d kept by drawing without a clear; %d dead pass(es) worth %.2f ms "
			   "-> %s",
			   g_ft_frame, np, nres, ncar, nkept, ndead, dead_ms, path);
	}
	VirtualFree(g_fg, 0, MEM_RELEASE);
	g_fg = NULL;
}

static void ft_dump(void)
{
	LARGE_INTEGER f;
	double us;
	char path[64];
	FILE *out;
	LONGLONG prev = g_ft_start, end = g_ft_n ? g_ft[g_ft_n - 1].t1 : g_ft_start;
	LONGLONG write = 0, wpres = 0, wrb = 0, gap = 0;
	static LONGLONG by_gap[256], by_time[256];
	static uint32_t by_n[256];
	int i, seq = 0, k;

	QueryPerformanceFrequency(&f);
	us = 1e6 / (double)f.QuadPart;
	snprintf(path, sizeof(path), "glframe_%u_game.csv", g_ft_frame);
	out = fopen(path, "w");
	if (out)
		fprintf(out, "seq,op,a0,a1,bytes,start_us,gap_before_us,dur_us\n");
	for (i = 0; i < g_ft_n; i++) {
		FtRec *e = &g_ft[i];
		LONGLONG g = e->t0 - prev, d = e->t1 - e->t0;
		int slot = e->op == FT_WAIT_PRESENT ? 254 : e->op == FT_WAIT_READBACK ? 255
				: e->op < 254 ? (int)e->op : 253;
		gap += g;
		by_gap[slot] += g;
		by_time[slot] += d;
		by_n[slot]++;
		if (e->op == FT_WAIT_PRESENT)
			wpres += d;
		else if (e->op == FT_WAIT_READBACK)
			wrb += d;
		else
			write += d;
		if (out) {
			if (e->op < FT_WAIT_PRESENT)
				fprintf(out, "%d,", seq++);
			else
				fprintf(out, ",");
			fprintf(out, "%s,%llX,%llX,%u,%.1f,%.1f,%.1f\n", ft_name(e->op),
				(unsigned long long)e->a0, (unsigned long long)e->a1, e->bytes,
				(double)(e->t0 - g_ft_start) * us, (double)g * us, (double)d * us);
		}
		prev = e->t1;
	}
	if (out)
		fclose(out);
	gl_fwd_log("frametime %u: %.2f ms, %d records; %.2f ms in the game and this DLL "
		   "between records, %.2f ms writing records, %.2f ms waiting for the host to "
		   "show the previous frame, %.2f ms waiting on readbacks -> %s",
		   g_ft_frame, (double)(end - g_ft_start) * us / 1000.0, seq,
		   (double)gap * us / 1000.0, (double)write * us / 1000.0,
		   (double)wpres * us / 1000.0, (double)wrb * us / 1000.0, path);
	for (k = 0; k < 10; k++) {
		int best = -1, j;
		for (j = 0; j < 256; j++)
			if (by_n[j] && (best < 0 || by_gap[j] + by_time[j] > by_gap[best] + by_time[best]))
				best = j;
		if (best < 0)
			break;
		gl_fwd_log("frametime %u:   %-32s x%-5u %.2f ms before it, %.2f ms in it", g_ft_frame,
			   best == 254 ? ft_name(FT_WAIT_PRESENT) : best == 255 ? ft_name(FT_WAIT_READBACK)
				   : best == 253 ? "?" : ft_name((uint32_t)best),
			   by_n[best], (double)by_gap[best] * us / 1000.0,
			   (double)by_time[best] * us / 1000.0);
		by_n[best] = 0;
	}
	fg_dump(us);
	VirtualFree(g_ft, 0, MEM_RELEASE);
	g_ft = NULL;
}

/* Under g_lock. */
static void rec_write(uint32_t op, uint16_t flags, const uint64_t *a, int na, const void *p1,
		      uint32_t n1, const void *p2, uint32_t n2)
{
	GlhRec r;
	uint32_t pos, bytes = (uint32_t)((sizeof(r) + (size_t)na * 8 + n1 + n2 + 7) & ~(size_t)7);
	int ft = ft_on(g_frames_sent);
	LONGLONG t0 = ft ? ft_now() : 0;

	if (!g_glfwd || !ring_reserve(bytes))
		return;
	r.op = op;
	r.bytes = bytes;
	r.nargs = (uint16_t)na;
	r.flags = flags;
	r.paylen = n1 + n2;
	pos = g_h->ring_head;
	ring_put(pos, &r, sizeof(r));
	pos += sizeof(r);
	ring_put(pos, a, (uint32_t)na * 8);
	pos += (uint32_t)na * 8;
	ring_put(pos, p1, n1);
	pos += n1;
	ring_put(pos, p2, n2);
	MemoryBarrier();
	InterlockedExchange((volatile LONG *)&g_h->ring_head, (LONG)(g_h->ring_head + bytes));
	if (g_h->sleeping)
		SetEvent(g_ev);
	if (ft)
		ft_add(op, bytes, op == GLH_OP_DRAW && na > 6 ? a[3] : na > 0 ? a[0] : 0,
		       op == GLH_OP_DRAW && na > 6 ? a[6] : na > 1 ? a[1] : 0, t0);
}

/* One call with a payload in up to two pieces. Payloads over a chunk go
 * ahead as BLOB records and the call refers to the staged bytes. */
static void fw_call2(uint32_t op, const uint64_t *a, int na, const void *p1, uint32_t n1,
		     const void *p2, uint32_t n2)
{
	void *ctx = glsw_ctx();
	int w, h;

	if (!g_glfwd || !ctx)
		return;
	EnterCriticalSection(&g_lock);
	if (ctx != g_last_ctx) {
		uint64_t c = (uint64_t)(uintptr_t)ctx;
		rec_write(GLH_OP_CTX, 0, &c, 1, NULL, 0, NULL, 0);
		g_last_ctx = ctx;
	}
	glsw_win_size(&w, &h);
	if (w > 0 && h > 0 && (w != g_sent_w || h != g_sent_h)) {
		uint64_t s[2] = { (uint64_t)w, (uint64_t)h };
		rec_write(GLH_OP_DFLT_SIZE, 0, s, 2, NULL, 0, NULL, 0);
		g_sent_w = w;
		g_sent_h = h;
	}
	if ((uint64_t)n1 + n2 <= GLH_CHUNK_BYTES) {
		rec_write(op, 0, a, na, p1, n1, p2, n2);
	} else {
		const uint8_t *src[2] = { (const uint8_t *)p1, (const uint8_t *)p2 };
		uint32_t len[2] = { n1, n2 };
		int k;
		for (k = 0; k < 2; k++) {
			uint32_t off;
			for (off = 0; off < len[k]; off += GLH_CHUNK_BYTES) {
				uint32_t n = len[k] - off;
				if (n > GLH_CHUNK_BYTES)
					n = GLH_CHUNK_BYTES;
				rec_write(GLH_OP_BLOB, 0, NULL, 0, src[k] + off, n, NULL, 0);
			}
		}
		rec_write(op, GLH_REC_STAGED, a, na, NULL, 0, NULL, 0);
	}
	if (g_fg && ft_on(g_frames_sent))
		fg_note(op, a, na);
	LeaveCriticalSection(&g_lock);
}

void fw_call(uint32_t op, const uint64_t *a, int na, const void *pay, uint32_t paylen)
{
	fw_call2(op, a, na, pay, paylen, NULL, 0);
}

void glfwd_start(void)
{
	static LONG once;
	char b[16], path[MAX_PATH], name[64], cmd[MAX_PATH + 96], *slash;
	HMODULE self = NULL;
	DWORD total = GLH_HEADER_BYTES + GLH_RING_BYTES + GLH_RB_BYTES;
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;

	if (InterlockedExchange(&once, 1))
		return;
	if (savestate_getenv("GLSW_HOST", b, sizeof(b)) && b[0] == '0') {
		gl_fwd_log("glhost: off (GLSW_HOST=0), drawing on the CPU");
		return;
	}
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			       (LPCSTR)(void *)glfwd_start, &self) ||
	    !GetModuleFileNameA(self, path, sizeof(path)) || !(slash = strrchr(path, '\\')))
		return;
	lstrcpynA(slash + 1, "glhost64.exe", (int)(sizeof(path) - (size_t)(slash + 1 - path)));
	if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
		gl_fwd_log("glhost: no %s, drawing on the CPU", path);
		return;
	}
	InitializeCriticalSection(&g_lock);
	snprintf(name, sizeof(name), "Local\\glsw_host_%lu", (unsigned long)GetCurrentProcessId());
	g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, total, name);
	g_h = g_map ? (GlhHeader *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
	if (!g_h) {
		gl_fwd_log("glhost: could not map %lu bytes (%lu), drawing on the CPU",
			   (unsigned long)total, GetLastError());
		return;
	}
	memset(g_h, 0, sizeof(*g_h));
	g_h->version = GLH_VERSION;
	g_h->header_bytes = GLH_HEADER_BYTES;
	g_h->game_pid = GetCurrentProcessId();
	g_h->ring_off = GLH_HEADER_BYTES;
	g_h->ring_bytes = GLH_RING_BYTES;
	g_h->rb_off = GLH_HEADER_BYTES + GLH_RING_BYTES;
	g_h->rb_bytes = GLH_RB_BYTES;
	g_h->rb_done = 0;
	if (savestate_getenv("GLSW_FRAMEGRAPH", b, sizeof(b)))
		g_ft_frame = (uint32_t)strtoul(b, NULL, 10);
	if (g_ft_frame) {
		g_ft = (FtRec *)VirtualAlloc(NULL, FT_MAX * sizeof(FtRec), MEM_COMMIT | MEM_RESERVE,
					     PAGE_READWRITE);
		g_fg = (FgEv *)VirtualAlloc(NULL, FG_MAX * sizeof(FgEv), MEM_COMMIT | MEM_RESERVE,
					    PAGE_READWRITE);
		/* Timing of this session, and wherever Windows put it this launch. */
		if (g_ft)
			savestate_exclude(g_ft, FT_MAX * sizeof(FtRec));
		if (g_fg)
			savestate_exclude(g_fg, FG_MAX * sizeof(FgEv));
	}
	g_h->time_frame = g_ft ? g_ft_frame : 0;
	MemoryBarrier();
	g_h->magic = GLH_MAGIC;
	snprintf(cmd, sizeof(cmd), "%s_f", name);
	g_ev = CreateEventA(NULL, FALSE, FALSE, cmd);
	snprintf(cmd, sizeof(cmd), "%s_d", name);
	g_ev_done = CreateEventA(NULL, FALSE, FALSE, cmd);
	snprintf(cmd, sizeof(cmd), "\"%s\" %s %lu", path, name, (unsigned long)GetCurrentProcessId());
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	memset(&pi, 0, sizeof(pi));
	if (!g_ev || !g_ev_done ||
	    !CreateProcessA(path, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		gl_fwd_log("glhost: could not launch %s (%lu), drawing on the CPU", path,
			   GetLastError());
		return;
	}
	CloseHandle(pi.hThread);
	g_proc = pi.hProcess;
	g_launch_tick = GetTickCount();
	InterlockedExchange(&g_glfwd, 1);
	gl_fwd_log("glhost: launched pid %lu, %u MB ring - GL calls go to the GPU",
		   (unsigned long)pi.dwProcessId, GLH_RING_BYTES >> 20);
}

void glfwd_present(HWND hwnd, int w, int h, int interval)
{
	uint64_t a[4];
	DWORD t0 = GetTickCount();
	LONGLONG w0;
	int ft;

	a[0] = (uint64_t)(uintptr_t)hwnd;
	a[1] = (uint64_t)w;
	a[2] = (uint64_t)h;
	a[3] = (uint64_t)interval;
	fw_call(GLH_OP_PRESENT, a, 4, NULL, 0);
	if (!g_glfwd)
		return;
	SetEvent(g_ev);
	g_frames_sent++;
	if (g_frames_sent == 1 || g_frames_sent % 300 == 0)
		gl_fwd_log("glhost: frame %u, host shown %ld, last frame %u records / %u KB, "
			   "%u GL errors so far",
			   g_frames_sent, (long)g_h->presented, g_h->records_frame,
			   g_h->bytes_frame >> 10, g_h->gl_errors);
	/* One frame in flight: the game never runs more than a frame ahead of
	 * what is on screen. */
	ft = ft_on(g_frames_sent - 1);
	w0 = ft ? ft_now() : 0;
	while (g_glfwd && (int32_t)(g_frames_sent - (uint32_t)g_h->presented) > 1)
		if (!host_wait(&t0))
			break;
	if (ft) {
		ft_add(FT_WAIT_PRESENT, 0, 0, 0, w0);
		ft_dump();
	} else if (g_ft && g_frames_sent + 1 == g_ft_frame) {
		g_ft_start = ft_now();
	}
}

/* After a restore. These were wound back with the rest of our image, but the
 * host and the ring went on: resend the context and size before the next
 * record, count frames and readbacks from where the host actually is. */
void glfwd_restored(void)
{
	uint64_t next = glsw_next_id(), hold = 1;

	if (!g_h)
		return;
	g_last_ctx = NULL;
	g_sent_w = g_sent_h = 0;
	g_frames_sent = (uint32_t)g_h->presented;
	g_rb_seq = g_h->rb_done;
	if (next)
		fw_call(GLH_OP_RESTORED, &next, 1, NULL, 0);
	fw_call(GLH_OP_HOLD, &hold, 1, NULL, 0);
}

void glfwd_saved(void)
{
	uint64_t hold = 1;
	fw_call(GLH_OP_HOLD, &hold, 1, NULL, 0);
}

void glfwd_ctx_delete(void *ctx)
{
	uint64_t c = (uint64_t)(uintptr_t)ctx;
	if (!g_glfwd)
		return;
	EnterCriticalSection(&g_lock);
	rec_write(GLH_OP_CTX_DELETE, 0, &c, 1, NULL, 0, NULL, 0);
	if (g_last_ctx == ctx)
		g_last_ctx = NULL;
	LeaveCriticalSection(&g_lock);
}

/* Sends a readback request and copies the answer into dst. */
static void fw_readback(uint32_t op, uint64_t *a, int na, void *dst, size_t bytes)
{
	LONG seq = InterlockedIncrement(&g_rb_seq);
	DWORD t0 = GetTickCount();
	LONGLONG w0;
	int ft;

	a[0] = (uint64_t)seq;
	fw_call(op, a, na, NULL, 0);
	SetEvent(g_ev);
	ft = ft_on(g_frames_sent);
	w0 = ft ? ft_now() : 0;
	while (g_glfwd && g_h->rb_done != seq)
		if (!host_wait(&t0))
			break;
	if (ft)
		ft_add(FT_WAIT_READBACK, (uint32_t)bytes, op, 0, w0);
	if (g_glfwd && g_h->rb_done == seq && g_h->rb_ok && bytes <= g_h->rb_bytes)
		memcpy(dst, (const uint8_t *)g_h + g_h->rb_off, bytes);
	else
		memset(dst, 0, bytes);
}

/* ---- pixel store state and image sizes ---- */

typedef struct PixStore {
	int align, row_len, skip_px, skip_rows, img_h, skip_img;
} PixStore;

static PixStore g_unpack = { 4, 0, 0, 0, 0, 0 }, g_pack = { 4, 0, 0, 0, 0, 0 };

static int format_comps(GLenum f)
{
	switch (f) {
	case GL_RG:
	case GL_RG_INTEGER:
	case GL_LUMINANCE_ALPHA:
	case GL_DEPTH_STENCIL:
		return 2;
	case GL_RGB:
	case GL_BGR:
	case GL_RGB_INTEGER:
	case GL_BGR_INTEGER:
		return 3;
	case GL_RGBA:
	case GL_BGRA:
	case GL_RGBA_INTEGER:
	case GL_BGRA_INTEGER:
		return 4;
	default:
		return 1;
	}
}

/* Bytes per pixel, and the size of one component for the alignment rule. */
static int pixel_bytes(GLenum format, GLenum type, int *comp)
{
	switch (type) {
	case GL_UNSIGNED_BYTE_3_3_2:
	case GL_UNSIGNED_BYTE_2_3_3_REV:
		*comp = 1;
		return 1;
	case GL_UNSIGNED_SHORT_5_6_5:
	case GL_UNSIGNED_SHORT_5_6_5_REV:
	case GL_UNSIGNED_SHORT_4_4_4_4:
	case GL_UNSIGNED_SHORT_4_4_4_4_REV:
	case GL_UNSIGNED_SHORT_5_5_5_1:
	case GL_UNSIGNED_SHORT_1_5_5_5_REV:
		*comp = 2;
		return 2;
	case GL_UNSIGNED_INT_8_8_8_8:
	case GL_UNSIGNED_INT_8_8_8_8_REV:
	case GL_UNSIGNED_INT_10_10_10_2:
	case GL_UNSIGNED_INT_2_10_10_10_REV:
	case GL_UNSIGNED_INT_24_8:
	case GL_UNSIGNED_INT_10F_11F_11F_REV:
	case GL_UNSIGNED_INT_5_9_9_9_REV:
		*comp = 4;
		return 4;
	case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
		*comp = 4;
		return 8;
	case GL_UNSIGNED_BYTE:
	case GL_BYTE:
		*comp = 1;
		break;
	case GL_UNSIGNED_SHORT:
	case GL_SHORT:
	case GL_HALF_FLOAT:
		*comp = 2;
		break;
	default:
		*comp = 4;
		break;
	}
	return *comp * format_comps(format);
}

/* The bytes GL reads (or writes) for a w x h x d image under the given pixel
 * store state, the last row unpadded - so never past the caller's memory. */
static size_t img_bytes(const PixStore *ps, int w, int h, int d, GLenum format, GLenum type)
{
	int comp, bpp = pixel_bytes(format, type, &comp);
	size_t rl, row, img;

	if (w <= 0 || h <= 0 || d <= 0)
		return 0;
	rl = (size_t)(ps->row_len > 0 ? ps->row_len : w);
	row = rl * (size_t)bpp;
	if (comp < ps->align)
		row = (row + (size_t)ps->align - 1) / (size_t)ps->align * (size_t)ps->align;
	img = row * (size_t)(ps->img_h > 0 ? ps->img_h : h);
	return (size_t)(ps->skip_img + d - 1) * img + (size_t)(ps->skip_rows + h - 1) * row +
	       (size_t)(ps->skip_px + w) * (size_t)bpp;
}

static void pix_store(PixStore *u, PixStore *p, GLenum pname, GLint v)
{
	switch (pname) {
	case GL_UNPACK_ALIGNMENT:
		u->align = v > 0 ? v : 4;
		break;
	case GL_UNPACK_ROW_LENGTH:
		u->row_len = v;
		break;
	case GL_UNPACK_SKIP_PIXELS:
		u->skip_px = v;
		break;
	case GL_UNPACK_SKIP_ROWS:
		u->skip_rows = v;
		break;
	case GL_UNPACK_IMAGE_HEIGHT:
		u->img_h = v;
		break;
	case GL_UNPACK_SKIP_IMAGES:
		u->skip_img = v;
		break;
	case GL_PACK_ALIGNMENT:
		p->align = v > 0 ? v : 4;
		break;
	case GL_PACK_ROW_LENGTH:
		p->row_len = v;
		break;
	case GL_PACK_SKIP_PIXELS:
		p->skip_px = v;
		break;
	case GL_PACK_SKIP_ROWS:
		p->skip_rows = v;
		break;
	}
}

void APIENTRY glPixelStorei(GLenum pname, GLint param);
void APIENTRY fw_glPixelStorei(GLenum pname, GLint param)
{
	uint64_t a[2];
	glPixelStorei(pname, param);
	pix_store(&g_unpack, &g_pack, pname, param);
	if (!glfwd_on())
		return;
	a[0] = pname;
	a[1] = (uint64_t)(int64_t)param;
	fw_call(GLH_OP_PIXEL_STORE, a, 2, NULL, 0);
}

void APIENTRY fw_glPixelStoref(GLenum pname, GLfloat param)
{
	fw_glPixelStorei(pname, (GLint)param);
}

/* ---- textures ---- */

static void fw_tex(int fn, GLenum target, GLint level, GLint internal, int w, int h, int d,
		   GLint border, GLenum format, GLenum type, int x, int y, const void *pixels,
		   size_t bytes)
{
	uint64_t a[14];
	a[0] = (uint64_t)fn;
	a[1] = target;
	a[2] = (uint64_t)(int64_t)level;
	a[3] = (uint64_t)(int64_t)internal;
	a[4] = (uint64_t)(int64_t)w;
	a[5] = (uint64_t)(int64_t)h;
	a[6] = (uint64_t)(int64_t)d;
	a[7] = (uint64_t)(int64_t)border;
	a[8] = format;
	a[9] = type;
	a[10] = (uint64_t)(int64_t)x;
	a[11] = (uint64_t)(int64_t)y;
	a[12] = pixels && bytes ? 1 : 0;
	a[13] = 0;
	fw_call(GLH_OP_TEX_IMAGE, a, 14, pixels, pixels ? (uint32_t)bytes : 0);
}

void APIENTRY glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
			   const void *);
void APIENTRY fw_glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
			      GLint border, GLenum format, GLenum type, const void *pixels)
{
	glTexImage2D(target, level, internal, w, h, border, format, type, pixels);
	if (glfwd_on())
		fw_tex(0, target, level, internal, w, h, 1, border, format, type, 0, 0, pixels,
		       pixels ? img_bytes(&g_unpack, w, h, 1, format, type) : 0);
}

void APIENTRY glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
			      const void *);
void APIENTRY fw_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w,
				 GLsizei h, GLenum format, GLenum type, const void *pixels)
{
	glTexSubImage2D(target, level, x, y, w, h, format, type, pixels);
	if (glfwd_on() && pixels)
		fw_tex(1, target, level, 0, w, h, 1, 0, format, type, x, y, pixels,
		       img_bytes(&g_unpack, w, h, 1, format, type));
}

void APIENTRY glTexImage3D(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum,
			   GLenum, const void *);
void APIENTRY fw_glTexImage3D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
			      GLsizei d, GLint border, GLenum format, GLenum type,
			      const void *pixels)
{
	glTexImage3D(target, level, internal, w, h, d, border, format, type, pixels);
	if (glfwd_on())
		fw_tex(2, target, level, internal, w, h, d, border, format, type, 0, 0, pixels,
		       pixels ? img_bytes(&g_unpack, w, h, d, format, type) : 0);
}

void APIENTRY glCompressedTexImage2D(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei,
				     const void *);
void APIENTRY fw_glCompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei w,
					GLsizei h, GLint border, GLsizei size, const void *data)
{
	glCompressedTexImage2D(target, level, internal, w, h, border, size, data);
	if (glfwd_on())
		fw_tex(3, target, level, (GLint)internal, w, h, 1, border, 0, 0, 0, 0, data,
		       data && size > 0 ? (size_t)size : 0);
}

static int tex_param_count(GLenum pname)
{
	return pname == GL_TEXTURE_BORDER_COLOR || pname == GL_TEXTURE_SWIZZLE_RGBA ? 4 : 1;
}

void APIENTRY glTexParameteriv(GLenum, GLenum, const GLint *);
void APIENTRY fw_glTexParameteriv(GLenum target, GLenum pname, const GLint *params)
{
	uint64_t a[3];
	glTexParameteriv(target, pname, params);
	if (!glfwd_on() || !params)
		return;
	a[0] = target;
	a[1] = pname;
	a[2] = 0;
	fw_call(GLH_OP_TEX_PARAMV, a, 3, params, (uint32_t)tex_param_count(pname) * 4);
}

void APIENTRY glTexParameterfv(GLenum, GLenum, const GLfloat *);
void APIENTRY fw_glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params)
{
	uint64_t a[3];
	glTexParameterfv(target, pname, params);
	if (!glfwd_on() || !params)
		return;
	a[0] = target;
	a[1] = pname;
	a[2] = 1;
	fw_call(GLH_OP_TEX_PARAMV, a, 3, params, (uint32_t)tex_param_count(pname) * 4);
}

/* ---- names ---- */

static void fw_names(uint32_t op, int kind, GLsizei n, const GLuint *ids)
{
	uint64_t a = (uint64_t)kind;
	if (glfwd_on() && ids && n > 0)
		fw_call(op, &a, 1, ids, (uint32_t)n * 4);
}

#define FW_GEN_DEL(Gen, Del, kind) \
	void APIENTRY Gen(GLsizei n, GLuint *ids); \
	void APIENTRY fw_##Gen(GLsizei n, GLuint *ids) \
	{ \
		Gen(n, ids); \
		fw_names(GLH_OP_GEN_NAMES, kind, n, ids); \
	} \
	void APIENTRY Del(GLsizei n, const GLuint *ids); \
	void APIENTRY fw_##Del(GLsizei n, const GLuint *ids) \
	{ \
		fw_names(GLH_OP_DEL_NAMES, kind, n, ids); \
		Del(n, ids); \
	}

FW_GEN_DEL(glGenTextures, glDeleteTextures, GLH_K_TEX)
FW_GEN_DEL(glGenBuffers, glDeleteBuffers, GLH_K_BUF)
FW_GEN_DEL(glGenVertexArrays, glDeleteVertexArrays, GLH_K_VAO)
FW_GEN_DEL(glGenFramebuffers, glDeleteFramebuffers, GLH_K_FBO)
FW_GEN_DEL(glGenRenderbuffers, glDeleteRenderbuffers, GLH_K_RBO)
FW_GEN_DEL(glGenSamplers, glDeleteSamplers, GLH_K_SAMP)

/* ---- buffers ---- */

void APIENTRY glBufferData(GLenum, GLsizeiptr, const void *, GLenum);
void APIENTRY fw_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	uint64_t a[4];
	glBufferData(target, size, data, usage);
	if (!glfwd_on() || size < 0)
		return;
	a[0] = target;
	a[1] = (uint64_t)size;
	a[2] = usage;
	a[3] = data ? 1 : 0;
	fw_call(GLH_OP_BUFFER_DATA, a, 4, data, data ? (uint32_t)size : 0);
}

void APIENTRY glBufferSubData(GLenum, GLintptr, GLsizeiptr, const void *);
void APIENTRY fw_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
				 const void *data)
{
	uint64_t a[2];
	glBufferSubData(target, offset, size, data);
	if (!glfwd_on() || !data || size <= 0)
		return;
	a[0] = target;
	a[1] = (uint64_t)offset;
	fw_call(GLH_OP_BUFFER_SUB, a, 2, data, (uint32_t)size);
}

/* Mapped ranges the game may write; sent whole when unmapped. */
typedef struct FwMap {
	GLuint buf;
	size_t off, len;
	int write;
} FwMap;
static FwMap g_maps[32];

static void map_note(GLenum target, size_t off, size_t len, int write)
{
	GLuint id = glsw_bound_buffer(target);
	int i, free_i = -1;
	if (!id)
		return;
	for (i = 0; i < 32; i++) {
		if (g_maps[i].buf == id) {
			free_i = i;
			break;
		}
		if (!g_maps[i].buf && free_i < 0)
			free_i = i;
	}
	if (free_i < 0)
		return;
	g_maps[free_i].buf = id;
	g_maps[free_i].off = off;
	g_maps[free_i].len = len;
	g_maps[free_i].write = write;
}

void *APIENTRY glMapBuffer(GLenum, GLenum);
void *APIENTRY fw_glMapBuffer(GLenum target, GLenum access)
{
	void *p = glMapBuffer(target, access);
	if (p && glfwd_on()) {
		size_t size = 0;
		glsw_buffer_data(glsw_bound_buffer(target), &size);
		map_note(target, 0, size, access != GL_READ_ONLY);
	}
	return p;
}

void *APIENTRY glMapBufferRange(GLenum, GLintptr, GLsizeiptr, GLbitfield);
void *APIENTRY fw_glMapBufferRange(GLenum target, GLintptr off, GLsizeiptr len, GLbitfield acc)
{
	void *p = glMapBufferRange(target, off, len, acc);
	if (p && glfwd_on())
		map_note(target, (size_t)off, (size_t)len, (acc & GL_MAP_WRITE_BIT) != 0);
	return p;
}

GLboolean APIENTRY glUnmapBuffer(GLenum);
GLboolean APIENTRY fw_glUnmapBuffer(GLenum target)
{
	GLuint id = glsw_bound_buffer(target);
	int i;

	for (i = 0; id && i < 32; i++) {
		size_t size = 0;
		const unsigned char *d;
		FwMap *m = &g_maps[i];
		if (m->buf != id)
			continue;
		d = glsw_buffer_data(id, &size);
		if (glfwd_on() && m->write && d && m->off < size) {
			uint64_t a[2];
			size_t len = m->len;
			if (len > size - m->off)
				len = size - m->off;
			a[0] = id;
			a[1] = (uint64_t)m->off;
			fw_call(GLH_OP_BUFFER_PUT, a, 2, d + m->off, (uint32_t)len);
		}
		m->buf = 0;
		break;
	}
	return glUnmapBuffer(target);
}

/* ---- vertex attributes ---- */

static void fw_attrib_ptr(GLuint idx, GLint size, GLenum type, GLboolean norm, GLsizei stride,
			  const void *ptr, int integer)
{
	uint64_t a[8];
	a[0] = idx;
	a[1] = (uint64_t)(int64_t)size;
	a[2] = type;
	a[3] = norm;
	a[4] = (uint64_t)(int64_t)stride;
	a[5] = (uint64_t)(uintptr_t)ptr;
	a[6] = (uint64_t)integer;
	a[7] = glsw_bound_buffer(GL_ARRAY_BUFFER) == 0;
	fw_call(GLH_OP_ATTRIB_PTR, a, 8, NULL, 0);
}

static unsigned char g_attr_int[16];

void APIENTRY glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
void APIENTRY fw_glVertexAttribPointer(GLuint idx, GLint size, GLenum type, GLboolean norm,
				       GLsizei stride, const void *ptr)
{
	glVertexAttribPointer(idx, size, type, norm, stride, ptr);
	if (idx < 16)
		g_attr_int[idx] = 0;
	if (glfwd_on())
		fw_attrib_ptr(idx, size, type, norm, stride, ptr, 0);
}

void APIENTRY glVertexAttribIPointer(GLuint, GLint, GLenum, GLsizei, const void *);
void APIENTRY fw_glVertexAttribIPointer(GLuint idx, GLint size, GLenum type, GLsizei stride,
					const void *ptr)
{
	glVertexAttribIPointer(idx, size, type, stride, ptr);
	if (idx < 16)
		g_attr_int[idx] = 1;
	if (glfwd_on())
		fw_attrib_ptr(idx, size, type, GL_FALSE, stride, ptr, 1);
}

static void fw_attribv(GLuint idx, int n, int ubyte, const void *v)
{
	uint64_t a[3];
	if (!glfwd_on() || !v)
		return;
	a[0] = idx;
	a[1] = (uint64_t)n;
	a[2] = (uint64_t)ubyte;
	fw_call(GLH_OP_ATTRIBV, a, 3, v, (uint32_t)(ubyte ? 4 : n * 4));
}

#define FW_ATTRIBV(N) \
	void APIENTRY glVertexAttrib##N##fv(GLuint idx, const GLfloat *v); \
	void APIENTRY fw_glVertexAttrib##N##fv(GLuint idx, const GLfloat *v) \
	{ \
		glVertexAttrib##N##fv(idx, v); \
		fw_attribv(idx, N, 0, v); \
	}
FW_ATTRIBV(1)
FW_ATTRIBV(2)
FW_ATTRIBV(3)
FW_ATTRIBV(4)

void APIENTRY glVertexAttrib4Nubv(GLuint, const GLubyte *);
void APIENTRY fw_glVertexAttrib4Nubv(GLuint idx, const GLubyte *v)
{
	glVertexAttrib4Nubv(idx, v);
	fw_attribv(idx, 4, 1, v);
}

/* ---- shaders and programs ---- */

static void fw_named(uint32_t op, GLuint prog, GLuint n, const GLchar *name)
{
	uint64_t a[2];
	if (!glfwd_on() || !name)
		return;
	a[0] = prog;
	a[1] = n;
	fw_call(op, a, 2, name, (uint32_t)strlen(name) + 1);
}

void APIENTRY glBindAttribLocation(GLuint, GLuint, const GLchar *);
void APIENTRY fw_glBindAttribLocation(GLuint prog, GLuint idx, const GLchar *name)
{
	glBindAttribLocation(prog, idx, name);
	fw_named(GLH_OP_BIND_ATTRIB, prog, idx, name);
}

void APIENTRY glBindFragDataLocation(GLuint, GLuint, const GLchar *);
void APIENTRY fw_glBindFragDataLocation(GLuint prog, GLuint color, const GLchar *name)
{
	glBindFragDataLocation(prog, color, name);
	fw_named(GLH_OP_BIND_FRAGDATA, prog, color, name);
}

GLuint APIENTRY glCreateShader(GLenum);
GLuint APIENTRY fw_glCreateShader(GLenum type)
{
	GLuint id = glCreateShader(type);
	if (id && glfwd_on()) {
		uint64_t a[2] = { id, type };
		fw_call(GLH_OP_CREATE_SHADER, a, 2, NULL, 0);
	}
	return id;
}

GLuint APIENTRY glCreateProgram(void);
GLuint APIENTRY fw_glCreateProgram(void)
{
	GLuint id = glCreateProgram();
	if (id && glfwd_on()) {
		uint64_t a = id;
		fw_call(GLH_OP_CREATE_PROGRAM, &a, 1, NULL, 0);
	}
	return id;
}

void APIENTRY glShaderSource(GLuint, GLsizei, const GLchar *const *, const GLint *);
void APIENTRY fw_glShaderSource(GLuint sh, GLsizei count, const GLchar *const *str,
				const GLint *len)
{
	size_t total = 0, at = 0;
	char *src;
	GLsizei i;
	uint64_t a;

	glShaderSource(sh, count, str, len);
	if (!glfwd_on() || !str || count <= 0)
		return;
	for (i = 0; i < count; i++)
		if (str[i])
			total += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
	src = (char *)malloc(total + 1);
	if (!src)
		return;
	for (i = 0; i < count; i++) {
		size_t n;
		if (!str[i])
			continue;
		n = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
		memcpy(src + at, str[i], n);
		at += n;
	}
	src[at] = 0;
	a = sh;
	fw_call(GLH_OP_SHADER_SOURCE, &a, 1, src, (uint32_t)at + 1);
	free(src);
}

/* (program, location) pairs the host has been told about. */
static uint64_t g_loc_seen[8192];

GLint APIENTRY glGetUniformLocation(GLuint, const GLchar *);
GLint APIENTRY fw_glGetUniformLocation(GLuint prog, const GLchar *name)
{
	GLint loc = glGetUniformLocation(prog, name);
	uint64_t key, a[2];
	unsigned h;

	if (loc < 0 || !name || !glfwd_on())
		return loc;
	key = ((uint64_t)prog << 32 | (uint32_t)loc) + 1;
	h = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 51);
	while (g_loc_seen[h] && g_loc_seen[h] != key)
		h = (h + 1) & 8191;
	if (g_loc_seen[h] == key)
		return loc;
	g_loc_seen[h] = key;
	a[0] = prog;
	a[1] = (uint64_t)(int64_t)loc;
	fw_call(GLH_OP_UNI_LOC, a, 2, name, (uint32_t)strlen(name) + 1);
	return loc;
}

static void fw_uniform(int type, int comps, int cols, int rows, GLint loc, GLsizei count,
		       GLboolean transpose, const void *v)
{
	uint64_t a[4];
	if (!glfwd_on() || !v || count <= 0 || loc < 0)
		return;
	a[0] = (uint64_t)(type | comps << 4 | cols << 12 | rows << 16);
	a[1] = (uint64_t)(int64_t)loc;
	a[2] = (uint64_t)count;
	a[3] = transpose;
	fw_call(GLH_OP_UNIFORM, a, 4, v, (uint32_t)count * (uint32_t)comps * 4);
}

#define FW_UNIV(Name, T, type, comps) \
	void APIENTRY Name(GLint loc, GLsizei count, const T *v); \
	void APIENTRY fw_##Name(GLint loc, GLsizei count, const T *v) \
	{ \
		Name(loc, count, v); \
		fw_uniform(type, comps, 0, 0, loc, count, 0, v); \
	}
FW_UNIV(glUniform1fv, GLfloat, GLH_UNI_F, 1)
FW_UNIV(glUniform2fv, GLfloat, GLH_UNI_F, 2)
FW_UNIV(glUniform3fv, GLfloat, GLH_UNI_F, 3)
FW_UNIV(glUniform4fv, GLfloat, GLH_UNI_F, 4)
FW_UNIV(glUniform1iv, GLint, GLH_UNI_I, 1)
FW_UNIV(glUniform2iv, GLint, GLH_UNI_I, 2)
FW_UNIV(glUniform3iv, GLint, GLH_UNI_I, 3)
FW_UNIV(glUniform4iv, GLint, GLH_UNI_I, 4)
FW_UNIV(glUniform1uiv, GLuint, GLH_UNI_U, 1)

#define FW_UNIM(Name, C, R) \
	void APIENTRY Name(GLint loc, GLsizei count, GLboolean t, const GLfloat *v); \
	void APIENTRY fw_##Name(GLint loc, GLsizei count, GLboolean t, const GLfloat *v) \
	{ \
		Name(loc, count, t, v); \
		fw_uniform(GLH_UNI_MAT, (C) * (R), C, R, loc, count, t, v); \
	}
FW_UNIM(glUniformMatrix2fv, 2, 2)
FW_UNIM(glUniformMatrix3fv, 3, 3)
FW_UNIM(glUniformMatrix4fv, 4, 4)
FW_UNIM(glUniformMatrix2x3fv, 2, 3)
FW_UNIM(glUniformMatrix3x2fv, 3, 2)
FW_UNIM(glUniformMatrix2x4fv, 2, 4)
FW_UNIM(glUniformMatrix4x2fv, 4, 2)
FW_UNIM(glUniformMatrix3x4fv, 3, 4)
FW_UNIM(glUniformMatrix4x3fv, 4, 3)

void APIENTRY glTransformFeedbackVaryings(GLuint, GLsizei, const GLchar *const *, GLenum);
void APIENTRY fw_glTransformFeedbackVaryings(GLuint prog, GLsizei count,
					     const GLchar *const *names, GLenum mode)
{
	char buf[2048];
	size_t at = 0;
	GLsizei i;
	uint64_t a[3];

	glTransformFeedbackVaryings(prog, count, names, mode);
	if (!glfwd_on() || !names || count <= 0)
		return;
	for (i = 0; i < count; i++) {
		size_t n = names[i] ? strlen(names[i]) : 0;
		if (at + n + 1 > sizeof(buf))
			break;
		memcpy(buf + at, names[i] ? names[i] : "", n);
		at += n;
		buf[at++] = 0;
	}
	a[0] = prog;
	a[1] = (uint64_t)i;
	a[2] = mode;
	fw_call(GLH_OP_TF_VARYINGS, a, 3, buf, (uint32_t)at);
}

/* ---- framebuffer ---- */

void APIENTRY glDrawBuffers(GLsizei, const GLenum *);
void APIENTRY fw_glDrawBuffers(GLsizei n, const GLenum *bufs)
{
	glDrawBuffers(n, bufs);
	if (glfwd_on() && bufs && n > 0)
		fw_call(GLH_OP_DRAW_BUFFERS, NULL, 0, bufs, (uint32_t)n * 4);
}

void APIENTRY glPolygonStipple(const GLubyte *);
void APIENTRY fw_glPolygonStipple(const GLubyte *mask)
{
	glPolygonStipple(mask);
	if (glfwd_on() && mask)
		fw_call(GLH_OP_STIPPLE, NULL, 0, mask, 128);
}

void APIENTRY fw_glVertex2f(GLfloat x, GLfloat y);
void APIENTRY fw_glVertex2fv(const GLfloat *v)
{
	if (v)
		fw_glVertex2f(v[0], v[1]);
}

void APIENTRY glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
void APIENTRY fw_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type,
			      void *pixels)
{
	uint64_t a[7];
	if (!glfwd_on()) {
		glReadPixels(x, y, w, h, format, type, pixels);
		return;
	}
	if (!pixels)
		return;
	a[1] = (uint64_t)(int64_t)x;
	a[2] = (uint64_t)(int64_t)y;
	a[3] = (uint64_t)(int64_t)w;
	a[4] = (uint64_t)(int64_t)h;
	a[5] = format;
	a[6] = type;
	fw_readback(GLH_OP_READ_PIXELS, a, 7, pixels, img_bytes(&g_pack, w, h, 1, format, type));
}

void APIENTRY glGetTexImage(GLenum, GLint, GLenum, GLenum, void *);
void APIENTRY fw_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type,
			       void *pixels)
{
	uint64_t a[5];
	int w = 0, h = 0;
	if (!glfwd_on()) {
		glGetTexImage(target, level, format, type, pixels);
		return;
	}
	if (!pixels || !glsw_tex_level_size(target, level, &w, &h))
		return;
	a[1] = target;
	a[2] = (uint64_t)(int64_t)level;
	a[3] = format;
	a[4] = type;
	fw_readback(GLH_OP_GET_TEX_IMAGE, a, 5, pixels, img_bytes(&g_pack, w, h, 1, format, type));
}

/* ---- draws ---- */

static int attr_type_bytes(GLenum t)
{
	switch (t) {
	case GL_BYTE:
	case GL_UNSIGNED_BYTE:
		return 1;
	case GL_SHORT:
	case GL_UNSIGNED_SHORT:
	case GL_HALF_FLOAT:
		return 2;
	case GL_DOUBLE:
		return 8;
	default:
		return 4;
	}
}

static void fw_draw(int kind, GLenum mode, GLint first, GLsizei count, GLenum itype,
		    const void *indices, GLsizei inst, GLint basevertex)
{
	GlswClientAttr ca[16];
	int nca = glsw_client_attrs(ca, 16), k, client_idx = 0;
	unsigned isz = itype == GL_UNSIGNED_INT ? 4u : itype == GL_UNSIGNED_SHORT ? 2u : 1u;
	const unsigned char *ip = NULL;
	size_t total = 0, at = 0;
	uint32_t nverts = 0;
	unsigned char *buf = NULL;
	uint64_t a[10];

	if (count <= 0)
		return;
	if (kind == GLH_DRAW_ELEMENTS) {
		GLuint ebo = glsw_vao_ebo();
		if (ebo) {
			size_t sz = 0;
			const unsigned char *d = glsw_buffer_data(ebo, &sz);
			size_t off = (size_t)(uintptr_t)indices;
			if (d && off + (size_t)count * isz <= sz)
				ip = d + off;
		} else {
			ip = (const unsigned char *)indices;
			client_idx = ip != NULL;
		}
	}
	if (nca) {
		if (kind == GLH_DRAW_ARRAYS) {
			nverts = (uint32_t)(first + count);
		} else if (ip) {
			uint32_t mx = 0, v;
			GLsizei i;
			for (i = 0; i < count; i++) {
				v = isz == 4 ? ((const uint32_t *)ip)[i] :
				    isz == 2 ? ((const uint16_t *)ip)[i] : ip[i];
				if (v > mx)
					mx = v;
			}
			nverts = (uint32_t)((int64_t)mx + basevertex + 1);
		}
		for (k = 0; k < nca; k++) {
			int el = attr_type_bytes(ca[k].type) *
				 (ca[k].size == GL_BGRA ? 4 : ca[k].size);
			int stride = ca[k].stride ? ca[k].stride : el;
			uint32_t n = ca[k].divisor ? (uint32_t)((inst + ca[k].divisor - 1) / ca[k].divisor)
						   : nverts;
			size_t bytes = n ? (size_t)(n - 1) * (size_t)stride + (size_t)el : 0;
			total += sizeof(GlhClientAttr) + ((bytes + 7) & ~(size_t)7);
		}
	}
	if (client_idx)
		total += (size_t)count * isz;
	if (total) {
		buf = (unsigned char *)malloc(total);
		if (!buf)
			return;
		for (k = 0; k < nca; k++) {
			GlhClientAttr h;
			int el = attr_type_bytes(ca[k].type) *
				 (ca[k].size == GL_BGRA ? 4 : ca[k].size);
			int stride = ca[k].stride ? ca[k].stride : el;
			uint32_t n = ca[k].divisor ? (uint32_t)((inst + ca[k].divisor - 1) / ca[k].divisor)
						   : nverts;
			size_t bytes = n ? (size_t)(n - 1) * (size_t)stride + (size_t)el : 0;
			h.index = ca[k].index;
			h.size = (uint32_t)ca[k].size;
			h.type = ca[k].type;
			h.normalized = (uint32_t)ca[k].normalized;
			h.stride = (uint32_t)ca[k].stride;
			h.integer = ca[k].index < 16 ? g_attr_int[ca[k].index] : 0;
			h.bytes = (uint32_t)bytes;
			h.divisor = ca[k].divisor;
			memcpy(buf + at, &h, sizeof(h));
			at += sizeof(h);
			if (bytes && ca[k].ptr)
				memcpy(buf + at, ca[k].ptr, bytes);
			else if (bytes)
				memset(buf + at, 0, bytes);
			at += (bytes + 7) & ~(size_t)7;
		}
		if (client_idx) {
			memcpy(buf + at, ip, (size_t)count * isz);
			at += (size_t)count * isz;
		}
	}
	a[0] = (uint64_t)kind;
	a[1] = mode;
	a[2] = (uint64_t)(int64_t)first;
	a[3] = (uint64_t)(int64_t)count;
	a[4] = itype;
	a[5] = client_idx ? 0 : (uint64_t)(uintptr_t)indices;
	a[6] = (uint64_t)(int64_t)inst;
	a[7] = (uint64_t)(int64_t)basevertex;
	a[8] = (uint64_t)nca;
	a[9] = (uint64_t)client_idx;
	fw_call(GLH_OP_DRAW, a, 10, buf, (uint32_t)at);
	free(buf);
}

void APIENTRY glDrawArrays(GLenum, GLint, GLsizei);
void APIENTRY fw_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	if (!glfwd_on())
		glDrawArrays(mode, first, count);
	else
		fw_draw(GLH_DRAW_ARRAYS, mode, first, count, 0, NULL, 0, 0);
}

void APIENTRY glDrawArraysInstanced(GLenum, GLint, GLsizei, GLsizei);
void APIENTRY fw_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei n)
{
	if (!glfwd_on())
		glDrawArraysInstanced(mode, first, count, n);
	else
		fw_draw(GLH_DRAW_ARRAYS, mode, first, count, 0, NULL, n, 0);
}

void APIENTRY glDrawElements(GLenum, GLsizei, GLenum, const void *);
void APIENTRY fw_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx)
{
	if (!glfwd_on())
		glDrawElements(mode, count, type, idx);
	else
		fw_draw(GLH_DRAW_ELEMENTS, mode, 0, count, type, idx, 0, 0);
}

void APIENTRY glDrawRangeElements(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
void APIENTRY fw_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count,
				     GLenum type, const void *idx)
{
	if (!glfwd_on())
		glDrawRangeElements(mode, start, end, count, type, idx);
	else
		fw_draw(GLH_DRAW_ELEMENTS, mode, 0, count, type, idx, 0, 0);
}

void APIENTRY glDrawElementsInstanced(GLenum, GLsizei, GLenum, const void *, GLsizei);
void APIENTRY fw_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *idx,
					 GLsizei n)
{
	if (!glfwd_on())
		glDrawElementsInstanced(mode, count, type, idx, n);
	else
		fw_draw(GLH_DRAW_ELEMENTS, mode, 0, count, type, idx, n, 0);
}

void APIENTRY glDrawElementsBaseVertex(GLenum, GLsizei, GLenum, const void *, GLint);
void APIENTRY fw_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type,
					  const void *idx, GLint base)
{
	if (!glfwd_on())
		glDrawElementsBaseVertex(mode, count, type, idx, base);
	else
		fw_draw(GLH_DRAW_ELEMENTS, mode, 0, count, type, idx, 0, base);
}

void APIENTRY glDrawElementsInstancedBaseVertex(GLenum, GLsizei, GLenum, const void *, GLsizei,
						GLint);
void APIENTRY fw_glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type,
						   const void *idx, GLsizei n, GLint base)
{
	if (!glfwd_on())
		glDrawElementsInstancedBaseVertex(mode, count, type, idx, n, base);
	else
		fw_draw(GLH_DRAW_ELEMENTS, mode, 0, count, type, idx, n, base);
}

void APIENTRY glMultiDrawElements(GLenum, const GLsizei *, GLenum, const void *const *, GLsizei);
void APIENTRY fw_glMultiDrawElements(GLenum mode, const GLsizei *count, GLenum type,
				     const void *const *idx, GLsizei n)
{
	GLsizei i;
	if (!glfwd_on()) {
		glMultiDrawElements(mode, count, type, idx, n);
		return;
	}
	for (i = 0; count && idx && i < n; i++)
		fw_draw(GLH_DRAW_ELEMENTS, mode, 0, count[i], type, idx[i], 0, 0);
}

void APIENTRY glMultiDrawElementsBaseVertex(GLenum, const GLsizei *, GLenum, const void *const *,
					    GLsizei, const GLint *);
void APIENTRY fw_glMultiDrawElementsBaseVertex(GLenum mode, const GLsizei *count, GLenum type,
					       const void *const *idx, GLsizei n,
					       const GLint *base)
{
	GLsizei i;
	if (!glfwd_on()) {
		glMultiDrawElementsBaseVertex(mode, count, type, idx, n, base);
		return;
	}
	for (i = 0; count && idx && i < n; i++)
		fw_draw(GLH_DRAW_ELEMENTS, mode, 0, count[i], type, idx[i], 0, base ? base[i] : 0);
}
