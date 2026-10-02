/* glhost64.exe - replays the software OpenGL DLL's calls on a real OpenGL
 * context, outside the game's process. See glhost.h.
 *
 *   glhost64.exe <mapping name> <game pid>
 *
 * The game's contexts each get a real context here, all sharing objects with
 * one root context on a hidden window of our own. The game's default
 * framebuffer is an ordinary framebuffer object at the game's size; at each
 * present it is scaled into a texture shared with D3D11 (WGL_NV_DX_interop)
 * and shown through a flip-model swap chain on the game's window - the
 * cross-process arrangement gpuhost64.exe uses. Without the interop the frame
 * goes through system memory instead. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glhost.h"
#include "glhost_ops.h"

#ifndef WGL_ACCESS_WRITE_DISCARD_NV
#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002
#endif

static FILE *g_log;
static GlhHeader *g_h;
static HMODULE g_ogl;
static uint32_t g_cur_op;

static void say(const char *fmt, ...)
{
	char line[1024];
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

static PROC glh_proc(const char *n)
{
	PROC p = wglGetProcAddress(n);
	if (!p || (uintptr_t)p <= 3 || (intptr_t)p == -1)
		p = GetProcAddress(g_ogl, n);
	return p;
}

static float glh_f(uint64_t v)
{
	union {
		uint32_t u;
		float f;
	} x;
	x.u = (uint32_t)v;
	return x.f;
}

static double glh_d(uint64_t v)
{
	union {
		uint64_t u;
		double d;
	} x;
	x.u = v;
	return x.d;
}

static GLuint xl(int kind, GLuint id);
static GLint xl_loc(GLint loc);

#include "glhost_gen.c"

static const char *op_name(uint32_t op)
{
	if (op < GLH_HAND_N)
		return glh_hand_names[op];
	if (op >= GLH_OP_GEN && op < GLH_OP_GEN_END)
		return glh_gen_names[op - GLH_OP_GEN];
	return "?";
}

/* ---- contexts ---- */

typedef struct HCtx {
	uint64_t id;
	HGLRC rc;
	GLuint prog, draw_fbo, read_fbo, array_buf;
	GLuint dflt_fbo, pres_fbo;
	int dflt_ver, pres_ver;
} HCtx;

static HWND g_win;
static HDC g_dc;
static HGLRC g_root;
static HCtx g_ctx[64];
static int g_nctx;
static HCtx *g_cc;
static PFNWGLCREATECONTEXTATTRIBSARBPROC p_wglCreateContextAttribsARB;

static HGLRC ctx_make(HGLRC share)
{
	static const int vers[][2] = { { 4, 6 }, { 4, 5 }, { 4, 3 }, { 3, 3 } };
	HGLRC rc = NULL;
	int i;

	for (i = 0; p_wglCreateContextAttribsARB && !rc && i < 4; i++) {
		int attr[] = { WGL_CONTEXT_MAJOR_VERSION_ARB,
			       vers[i][0],
			       WGL_CONTEXT_MINOR_VERSION_ARB,
			       vers[i][1],
			       WGL_CONTEXT_PROFILE_MASK_ARB,
			       WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
			       0 };
		rc = p_wglCreateContextAttribsARB(g_dc, share, attr);
	}
	if (!rc) {
		rc = wglCreateContext(g_dc);
		if (rc && share)
			wglShareLists(share, rc);
	}
	return rc;
}

static HCtx *ctx_find(uint64_t id, int create)
{
	int i;
	for (i = 0; i < g_nctx; i++)
		if (g_ctx[i].id == id)
			return &g_ctx[i];
	if (!create || g_nctx >= 64)
		return NULL;
	memset(&g_ctx[g_nctx], 0, sizeof(g_ctx[0]));
	g_ctx[g_nctx].id = id;
	g_ctx[g_nctx].rc = ctx_make(g_root);
	if (!g_ctx[g_nctx].rc) {
		say("could not create a context for game context %llx", (unsigned long long)id);
		return NULL;
	}
	say("context for game context %llx", (unsigned long long)id);
	return &g_ctx[g_nctx++];
}

static void ctx_switch(uint64_t id)
{
	HCtx *c = ctx_find(id, 1);
	if (!c || c == g_cc)
		return;
	if (g_cc)
		glFlush();
	wglMakeCurrent(g_dc, c->rc);
	g_cc = c;
}

static void ctx_delete(uint64_t id)
{
	HCtx *c = ctx_find(id, 0);
	if (!c)
		return;
	if (c == g_cc) {
		wglMakeCurrent(g_dc, g_root);
		g_cc = NULL;
	}
	wglDeleteContext(c->rc);
	*c = g_ctx[--g_nctx];
	if (g_cc == &g_ctx[g_nctx])
		g_cc = c;
}

/* ---- name translation ---- */

typedef struct Xl {
	GLuint *v;
	uint32_t n;
} Xl;
static Xl g_xl[GLH_K_N];

static GLuint xl_get(int k, GLuint id)
{
	return id < g_xl[k].n ? g_xl[k].v[id] : 0;
}

static void xl_set_in(Xl *x, GLuint id, GLuint host)
{
	if (id >= x->n) {
		uint32_t n = x->n ? x->n : 1024;
		GLuint *v;
		while (n <= id)
			n *= 2;
		v = (GLuint *)realloc(x->v, n * sizeof(GLuint));
		if (!v)
			return;
		memset(v + x->n, 0, (n - x->n) * sizeof(GLuint));
		x->v = v;
		x->n = n;
	}
	x->v[id] = host;
}

static void xl_set(int k, GLuint id, GLuint host)
{
	xl_set_in(&g_xl[k], id, host);
}

static GLuint obj_new(int k)
{
	GLuint h = 0;
	switch (k) {
	case GLH_K_TEX:
		P_glGenTextures(1, &h);
		break;
	case GLH_K_BUF:
		P_glGenBuffers(1, &h);
		break;
	case GLH_K_VAO:
		P_glGenVertexArrays(1, &h);
		break;
	case GLH_K_FBO:
		P_glGenFramebuffers(1, &h);
		break;
	case GLH_K_RBO:
		P_glGenRenderbuffers(1, &h);
		break;
	case GLH_K_SAMP:
		if (P_glGenSamplers)
			P_glGenSamplers(1, &h);
		break;
	}
	return h;
}

static void obj_del(int k, GLuint h)
{
	if (!h)
		return;
	switch (k) {
	case GLH_K_TEX:
		P_glDeleteTextures(1, &h);
		break;
	case GLH_K_BUF:
		P_glDeleteBuffers(1, &h);
		break;
	case GLH_K_VAO:
		P_glDeleteVertexArrays(1, &h);
		break;
	case GLH_K_FBO:
		P_glDeleteFramebuffers(1, &h);
		break;
	case GLH_K_RBO:
		P_glDeleteRenderbuffers(1, &h);
		break;
	case GLH_K_SAMP:
		if (P_glDeleteSamplers)
			P_glDeleteSamplers(1, &h);
		break;
	}
}

/* ---- the game's default framebuffer ---- */

static GLuint g_dflt_tex, g_dflt_rb;
static int g_dflt_w, g_dflt_h, g_dflt_ver;
static int g_want_w = 1280, g_want_h = 720;

static void dflt_storage(void)
{
	GLint tex = 0, rb = 0;
	if (g_dflt_tex && g_dflt_w == g_want_w && g_dflt_h == g_want_h)
		return;
	P_glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
	P_glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
	if (!g_dflt_tex) {
		P_glGenTextures(1, &g_dflt_tex);
		P_glGenRenderbuffers(1, &g_dflt_rb);
	}
	P_glBindTexture(GL_TEXTURE_2D, g_dflt_tex);
	P_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_want_w, g_want_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
		       NULL);
	P_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	P_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	P_glBindRenderbuffer(GL_RENDERBUFFER, g_dflt_rb);
	P_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, g_want_w, g_want_h);
	P_glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
	P_glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)rb);
	g_dflt_w = g_want_w;
	g_dflt_h = g_want_h;
	g_dflt_ver++;
	say("default framebuffer %dx%d", g_dflt_w, g_dflt_h);
}

/* The current context's stand-in for framebuffer 0 (framebuffer objects are
 * per context), attached to the shared storage. */
static GLuint dflt_fbo(void)
{
	GLint d = 0, r = 0;
	if (!g_cc)
		return 0;
	dflt_storage();
	if (g_cc->dflt_fbo && g_cc->dflt_ver == g_dflt_ver)
		return g_cc->dflt_fbo;
	P_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &d);
	P_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &r);
	if (!g_cc->dflt_fbo)
		P_glGenFramebuffers(1, &g_cc->dflt_fbo);
	P_glBindFramebuffer(GL_FRAMEBUFFER, g_cc->dflt_fbo);
	P_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_dflt_tex, 0);
	P_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
				    g_dflt_rb);
	P_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)d == 0 ? g_cc->dflt_fbo : (GLuint)d);
	P_glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)r == 0 ? g_cc->dflt_fbo : (GLuint)r);
	g_cc->dflt_ver = g_dflt_ver;
	return g_cc->dflt_fbo;
}

static GLuint xl(int k, GLuint id)
{
	GLuint h;
	if (!id)
		return k == GLH_K_FBO ? dflt_fbo() : 0;
	h = xl_get(k, id);
	if (!h && k != GLH_K_OBJ) {
		h = obj_new(k);
		xl_set(k, id, h);
	}
	return h;
}

/* ---- uniform locations ---- */

typedef struct UniEnt {
	GLint cl, hl;
	char *name;
} UniEnt;

typedef struct PInfo {
	UniEnt *e;
	int n, cap;
} PInfo;

static PInfo **g_pinfo;
static uint32_t g_npinfo;

static PInfo *pinfo(GLuint prog, int create)
{
	if (prog >= g_npinfo) {
		uint32_t n = g_npinfo ? g_npinfo : 1024;
		PInfo **v;
		if (!create)
			return NULL;
		while (n <= prog)
			n *= 2;
		v = (PInfo **)realloc(g_pinfo, n * sizeof(*v));
		if (!v)
			return NULL;
		memset(v + g_npinfo, 0, (n - g_npinfo) * sizeof(*v));
		g_pinfo = v;
		g_npinfo = n;
	}
	if (!g_pinfo[prog] && create)
		g_pinfo[prog] = (PInfo *)calloc(1, sizeof(PInfo));
	return g_pinfo[prog];
}

static void pinfo_free(GLuint prog)
{
	PInfo *p = pinfo(prog, 0);
	int i;
	if (!p)
		return;
	for (i = 0; i < p->n; i++)
		free(p->e[i].name);
	free(p->e);
	free(p);
	g_pinfo[prog] = NULL;
}

static GLint xl_loc(GLint loc)
{
	PInfo *p;
	int i;
	GLint base = loc & ~GLH_UNI_ELEM_MASK;
	if (loc < 0)
		return -1;
	p = g_cc ? pinfo(g_cc->prog, 0) : NULL;
	if (!p)
		return loc;
	for (i = 0; i < p->n; i++)
		if (p->e[i].cl == loc)
			return p->e[i].hl;
	for (i = 0; i < p->n; i++)
		if (p->e[i].cl == base)
			return p->e[i].hl < 0 ? -1 : p->e[i].hl + (loc & GLH_UNI_ELEM_MASK);
	return loc;
}

static const char *uni_name(GLint loc)
{
	PInfo *p = g_cc ? pinfo(g_cc->prog, 0) : NULL;
	int i;
	for (i = 0; p && i < p->n; i++)
		if (p->e[i].cl == loc || p->e[i].cl == (loc & ~GLH_UNI_ELEM_MASK))
			return p->e[i].name;
	return "?";
}

static void uni_loc(GLuint prog, GLint cl, const char *name)
{
	PInfo *p = pinfo(prog, 1);
	UniEnt *e;
	int i;
	if (!p)
		return;
	for (i = 0; i < p->n; i++)
		if (p->e[i].cl == cl)
			return;
	if (p->n == p->cap) {
		int cap = p->cap ? p->cap * 2 : 16;
		UniEnt *v = (UniEnt *)realloc(p->e, (size_t)cap * sizeof(UniEnt));
		if (!v)
			return;
		p->e = v;
		p->cap = cap;
	}
	e = &p->e[p->n++];
	e->cl = cl;
	e->name = _strdup(name);
	e->hl = P_glGetUniformLocation(xl_get(GLH_K_OBJ, prog), name);
}

/* ---- hooks for generated calls ---- */

static GLenum dflt_buf(GLenum b)
{
	switch (b) {
	case GL_BACK:
	case GL_BACK_LEFT:
	case GL_FRONT:
	case GL_FRONT_LEFT:
	case GL_LEFT:
	case GL_FRONT_AND_BACK:
		return GL_COLOR_ATTACHMENT0;
	default:
		return b;
	}
}

static void hk_glBindFramebuffer(const uint64_t *a)
{
	GLenum t = (GLenum)a[0];
	GLuint id = (GLuint)a[1];
	if (!g_cc)
		return;
	if (t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER)
		g_cc->draw_fbo = id;
	if (t == GL_FRAMEBUFFER || t == GL_READ_FRAMEBUFFER)
		g_cc->read_fbo = id;
	P_glBindFramebuffer(t, xl(GLH_K_FBO, id));
}

static int fb_is_dflt(GLenum target)
{
	if (!g_cc)
		return 0;
	return target == GL_READ_FRAMEBUFFER ? g_cc->read_fbo == 0 : g_cc->draw_fbo == 0;
}

static void hk_glDrawBuffer(const uint64_t *a)
{
	GLenum b = (GLenum)a[0];
	P_glDrawBuffer(g_cc && g_cc->draw_fbo == 0 ? dflt_buf(b) : b);
}

static void hk_glReadBuffer(const uint64_t *a)
{
	GLenum b = (GLenum)a[0];
	P_glReadBuffer(g_cc && g_cc->read_fbo == 0 ? dflt_buf(b) : b);
}

static void hk_glUseProgram(const uint64_t *a)
{
	if (g_cc)
		g_cc->prog = (GLuint)a[0];
	P_glUseProgram(xl(GLH_K_OBJ, (GLuint)a[0]));
}

static void hk_glBindBuffer(const uint64_t *a)
{
	GLenum t = (GLenum)a[0];
	if (g_cc && t == GL_ARRAY_BUFFER)
		g_cc->array_buf = (GLuint)a[1];
	P_glBindBuffer(t, xl(GLH_K_BUF, (GLuint)a[1]));
}

static int g_compile_fails, g_link_fails;

static void hk_glCompileShader(const uint64_t *a)
{
	GLuint h = xl(GLH_K_OBJ, (GLuint)a[0]);
	GLint ok = 0;
	P_glCompileShader(h);
	P_glGetShaderiv(h, GL_COMPILE_STATUS, &ok);
	if (!ok && g_compile_fails++ < 40) {
		char log[2048];
		GLsizei n = 0;
		P_glGetShaderInfoLog(h, sizeof(log), &n, log);
		say("shader %u failed to compile: %.*s", (unsigned)a[0], (int)n, log);
	}
}

static void hk_glLinkProgram(const uint64_t *a)
{
	GLuint id = (GLuint)a[0], h = xl(GLH_K_OBJ, id);
	GLint ok = 0;
	PInfo *p;
	int i;
	P_glLinkProgram(h);
	P_glGetProgramiv(h, GL_LINK_STATUS, &ok);
	if (!ok && g_link_fails++ < 40) {
		char log[2048];
		GLsizei n = 0;
		P_glGetProgramInfoLog(h, sizeof(log), &n, log);
		say("program %u failed to link: %.*s", id, (int)n, log);
	}
	p = pinfo(id, 0);
	for (i = 0; p && i < p->n; i++)
		p->e[i].hl = P_glGetUniformLocation(h, p->e[i].name);
}

static int obj_retire(int k, GLuint id);

static void hk_glDeleteShader(const uint64_t *a)
{
	obj_retire(GLH_K_OBJ, (GLuint)a[0]);
}

/* Uniform names stay with a program that is only put aside. */
static void hk_glDeleteProgram(const uint64_t *a)
{
	GLuint id = (GLuint)a[0];
	if (!obj_retire(GLH_K_OBJ, id))
		pinfo_free(id);
}

/* ---- presenting ---- */

typedef BOOL(WINAPI *T_wglDXSetResourceShareHandleNV)(void *, HANDLE);
typedef HANDLE(WINAPI *T_wglDXOpenDeviceNV)(void *);
typedef BOOL(WINAPI *T_wglDXCloseDeviceNV)(HANDLE);
typedef HANDLE(WINAPI *T_wglDXRegisterObjectNV)(HANDLE, void *, GLuint, GLenum, GLenum);
typedef BOOL(WINAPI *T_wglDXUnregisterObjectNV)(HANDLE, HANDLE);
typedef BOOL(WINAPI *T_wglDXLockObjectsNV)(HANDLE, GLint, HANDLE *);
typedef BOOL(WINAPI *T_wglDXUnlockObjectsNV)(HANDLE, GLint, HANDLE *);

static T_wglDXOpenDeviceNV p_dxOpen;
static T_wglDXCloseDeviceNV p_dxClose;
static T_wglDXRegisterObjectNV p_dxRegister;
static T_wglDXUnregisterObjectNV p_dxUnregister;
static T_wglDXLockObjectsNV p_dxLock;
static T_wglDXUnlockObjectsNV p_dxUnlock;

typedef struct Pres {
	HWND hwnd;
	ID3D11Device *dev;
	ID3D11DeviceContext *ctx;
	IDXGISwapChain1 *sc;
	UINT cw, ch;
	ID3D11Texture2D *tex; /* what GL fills, copied to the back buffer */
	HANDLE idev, iobj;    /* interop, when there is one */
	GLuint gltex;
	int ver;
	int interop;
	uint8_t *rows; /* readback path */
	int failed;
} Pres;
static Pres g_p;

#define REL(p) \
	do { \
		if (p) \
			IUnknown_Release((IUnknown *)(p)); \
		(p) = NULL; \
	} while (0)

static void pres_target_free(void)
{
	if (g_p.iobj) {
		p_dxUnregister(g_p.idev, g_p.iobj);
		g_p.iobj = NULL;
	}
	if (g_p.gltex) {
		P_glDeleteTextures(1, &g_p.gltex);
		g_p.gltex = 0;
	}
	REL(g_p.tex);
	free(g_p.rows);
	g_p.rows = NULL;
}

static void pres_free(void)
{
	pres_target_free();
	if (g_p.idev)
		p_dxClose(g_p.idev);
	g_p.idev = NULL;
	REL(g_p.sc);
	if (g_p.ctx)
		ID3D11DeviceContext_ClearState(g_p.ctx);
	REL(g_p.ctx);
	REL(g_p.dev);
	memset(&g_p, 0, sizeof(g_p));
}

static int pres_target(void)
{
	D3D11_TEXTURE2D_DESC td;
	GLint tex = 0;

	pres_target_free();
	memset(&td, 0, sizeof(td));
	td.Width = g_p.cw;
	td.Height = g_p.ch;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(ID3D11Device_CreateTexture2D(g_p.dev, &td, NULL, &g_p.tex)))
		return 0;
	P_glGenTextures(1, &g_p.gltex);
	if (g_p.interop) {
		g_p.iobj = p_dxRegister(g_p.idev, g_p.tex, g_p.gltex, GL_TEXTURE_2D,
					WGL_ACCESS_WRITE_DISCARD_NV);
		if (!g_p.iobj) {
			say("wglDXRegisterObjectNV failed (%lu); frames go through system memory",
			    GetLastError());
			g_p.interop = 0;
			P_glDeleteTextures(1, &g_p.gltex);
			P_glGenTextures(1, &g_p.gltex);
		}
	}
	if (!g_p.interop) {
		P_glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
		P_glBindTexture(GL_TEXTURE_2D, g_p.gltex);
		P_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)g_p.cw, (GLsizei)g_p.ch, 0, GL_RGBA,
			       GL_UNSIGNED_BYTE, NULL);
		P_glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
		g_p.rows = (uint8_t *)malloc((size_t)g_p.cw * g_p.ch * 4);
	}
	g_p.ver++;
	return 1;
}

static int pres_init(HWND hwnd)
{
	IDXGIDevice *xd = NULL;
	IDXGIAdapter *ad = NULL;
	IDXGIFactory2 *fac = NULL;
	IDXGIDevice1 *xd1 = NULL;
	DXGI_SWAP_CHAIN_DESC1 d;
	RECT rc;
	HRESULT hr;

	pres_free();
	g_p.hwnd = hwnd;
	hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			       NULL, 0, D3D11_SDK_VERSION, &g_p.dev, NULL, &g_p.ctx);
	if (FAILED(hr)) {
		say("D3D11CreateDevice failed %08lx", (unsigned long)hr);
		return 0;
	}
	ID3D11Device_QueryInterface(g_p.dev, &IID_IDXGIDevice, (void **)&xd);
	IDXGIDevice_GetAdapter(xd, &ad);
	IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
	IDXGIAdapter_Release(ad);
	IDXGIDevice_Release(xd);
	GetClientRect(hwnd, &rc);
	g_p.cw = rc.right > rc.left ? (UINT)(rc.right - rc.left) : 1;
	g_p.ch = rc.bottom > rc.top ? (UINT)(rc.bottom - rc.top) : 1;
	memset(&d, 0, sizeof(d));
	d.Width = g_p.cw;
	d.Height = g_p.ch;
	d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	d.SampleDesc.Count = 1;
	d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	d.BufferCount = 2;
	d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)g_p.dev, hwnd, &d, NULL, NULL,
						  &g_p.sc);
	if (SUCCEEDED(hr))
		IDXGIFactory2_MakeWindowAssociation(fac, hwnd,
						    DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
	IDXGIFactory2_Release(fac);
	if (FAILED(hr)) {
		say("CreateSwapChainForHwnd on the game window failed %08lx", (unsigned long)hr);
		return 0;
	}
	if (SUCCEEDED(ID3D11Device_QueryInterface(g_p.dev, &IID_IDXGIDevice1, (void **)&xd1))) {
		IDXGIDevice1_SetMaximumFrameLatency(xd1, 1);
		IDXGIDevice1_Release(xd1);
	}
	p_dxOpen = (T_wglDXOpenDeviceNV)(void *)glh_proc("wglDXOpenDeviceNV");
	p_dxClose = (T_wglDXCloseDeviceNV)(void *)glh_proc("wglDXCloseDeviceNV");
	p_dxRegister = (T_wglDXRegisterObjectNV)(void *)glh_proc("wglDXRegisterObjectNV");
	p_dxUnregister = (T_wglDXUnregisterObjectNV)(void *)glh_proc("wglDXUnregisterObjectNV");
	p_dxLock = (T_wglDXLockObjectsNV)(void *)glh_proc("wglDXLockObjectsNV");
	p_dxUnlock = (T_wglDXUnlockObjectsNV)(void *)glh_proc("wglDXUnlockObjectsNV");
	if (p_dxOpen && p_dxClose && p_dxRegister && p_dxUnregister && p_dxLock && p_dxUnlock)
		g_p.idev = p_dxOpen(g_p.dev);
	g_p.interop = g_p.idev != NULL;
	if (!g_p.interop)
		say("no WGL_NV_DX_interop; frames go through system memory");
	if (!pres_target())
		return 0;
	say("presenting on window %p, %ux%u client, %s", (void *)hwnd, g_p.cw, g_p.ch,
	    g_p.interop ? "GL/D3D11 shared texture" : "system memory copy");
	return 1;
}

static void present(HWND hwnd, int interval)
{
	RECT rc;
	UINT cw, ch, dw, dh;
	int x0, y0;
	GLint d = 0, r = 0, sc = 0, rd = 0, srgb = 0;
	GLboolean cm[4];
	GLfloat cc[4];
	ID3D11Texture2D *bb = NULL;
	HRESULT hr;

	if (g_p.failed || !g_cc)
		return;
	if (hwnd != g_p.hwnd && !pres_init(hwnd)) {
		g_p.failed = 1;
		pres_free();
		g_p.failed = 1;
		return;
	}
	GetClientRect(hwnd, &rc);
	cw = rc.right > rc.left ? (UINT)(rc.right - rc.left) : 1;
	ch = rc.bottom > rc.top ? (UINT)(rc.bottom - rc.top) : 1;
	if (cw != g_p.cw || ch != g_p.ch) {
		g_p.cw = cw;
		g_p.ch = ch;
		pres_target_free();
		hr = IDXGISwapChain1_ResizeBuffers(g_p.sc, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
		if (FAILED(hr) || !pres_target()) {
			say("resize to %ux%u failed %08lx", cw, ch, (unsigned long)hr);
			g_p.failed = 1;
			return;
		}
	}
	dflt_fbo();
	if (!g_cc->pres_fbo || g_cc->pres_ver != g_p.ver) {
		GLint dd = 0;
		P_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dd);
		if (!g_cc->pres_fbo)
			P_glGenFramebuffers(1, &g_cc->pres_fbo);
		if (g_p.interop)
			p_dxLock(g_p.idev, 1, &g_p.iobj);
		P_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_cc->pres_fbo);
		P_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
					 g_p.gltex, 0);
		if (g_p.interop)
			p_dxUnlock(g_p.idev, 1, &g_p.iobj);
		P_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dd);
		g_cc->pres_ver = g_p.ver;
	}

	/* Letterboxed fit of the game's frame into the client area. */
	dw = cw;
	dh = (UINT)(((unsigned long long)cw * (UINT)g_dflt_h) / (UINT)g_dflt_w);
	if (dh > ch) {
		dh = ch;
		dw = (UINT)(((unsigned long long)ch * (UINT)g_dflt_w) / (UINT)g_dflt_h);
	}
	x0 = ((int)cw - (int)dw) / 2;
	y0 = ((int)ch - (int)dh) / 2;

	P_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &d);
	P_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &r);
	sc = P_glIsEnabled(GL_SCISSOR_TEST);
	rd = P_glIsEnabled(GL_RASTERIZER_DISCARD);
	srgb = P_glIsEnabled(GL_FRAMEBUFFER_SRGB);
	P_glGetBooleanv(GL_COLOR_WRITEMASK, cm);
	P_glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
	if (g_p.interop)
		p_dxLock(g_p.idev, 1, &g_p.iobj);
	P_glDisable(GL_SCISSOR_TEST);
	P_glDisable(GL_RASTERIZER_DISCARD);
	P_glDisable(GL_FRAMEBUFFER_SRGB);
	P_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	P_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_cc->pres_fbo);
	P_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_cc->dflt_fbo);
	P_glReadBuffer(GL_COLOR_ATTACHMENT0);
	P_glDrawBuffer(GL_COLOR_ATTACHMENT0);
	P_glClearColor(0, 0, 0, 1);
	P_glClear(GL_COLOR_BUFFER_BIT);
	/* D3D's row 0 is the top, GL's the bottom: blit upside down. */
	P_glBlitFramebuffer(0, 0, g_dflt_w, g_dflt_h, x0, y0 + (int)dh, x0 + (int)dw, y0,
			    GL_COLOR_BUFFER_BIT, GL_LINEAR);
	if (!g_p.interop) {
		P_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_cc->pres_fbo);
		P_glPixelStorei(GL_PACK_ALIGNMENT, 4);
		P_glReadPixels(0, 0, (GLsizei)cw, (GLsizei)ch, GL_RGBA, GL_UNSIGNED_BYTE, g_p.rows);
	}
	if (g_p.interop)
		p_dxUnlock(g_p.idev, 1, &g_p.iobj);
	/* Put back everything the game can see. */
	P_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)d);
	P_glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)r);
	if (g_cc->draw_fbo == 0)
		P_glDrawBuffer(GL_COLOR_ATTACHMENT0);
	if (g_cc->read_fbo == 0)
		P_glReadBuffer(GL_COLOR_ATTACHMENT0);
	if (sc)
		P_glEnable(GL_SCISSOR_TEST);
	if (rd)
		P_glEnable(GL_RASTERIZER_DISCARD);
	if (srgb)
		P_glEnable(GL_FRAMEBUFFER_SRGB);
	P_glColorMask(cm[0], cm[1], cm[2], cm[3]);
	P_glClearColor(cc[0], cc[1], cc[2], cc[3]);

	if (!g_p.interop)
		ID3D11DeviceContext_UpdateSubresource(g_p.ctx, (ID3D11Resource *)g_p.tex, 0, NULL,
						      g_p.rows, cw * 4, 0);
	if (SUCCEEDED(IDXGISwapChain1_GetBuffer(g_p.sc, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
		ID3D11DeviceContext_CopyResource(g_p.ctx, (ID3D11Resource *)bb,
						 (ID3D11Resource *)g_p.tex);
		ID3D11Texture2D_Release(bb);
	}
	hr = IDXGISwapChain1_Present(g_p.sc, interval > 0 ? 1 : 0, 0);
	if (FAILED(hr)) {
		say("Present failed %08lx", (unsigned long)hr);
		g_p.failed = 1;
	}
}

/* ---- records ---- */

static uint8_t *g_stage;
static uint32_t g_stage_len, g_stage_cap;
static GLuint g_svb[16], g_sib;
static uint32_t g_frame_draws, g_frame_dflt_draws;
static int g_trace; /* log every record of one frame */
static PFNGLGENQUERIESPROC P_glGenQueries;
static PFNGLDELETEQUERIESPROC P_glDeleteQueries;
static PFNGLBEGINQUERYPROC P_glBeginQuery;
static PFNGLENDQUERYPROC P_glEndQuery;
static PFNGLGETQUERYOBJECTUIVPROC P_glGetQueryObjectuiv;
static PFNGLNAMEDBUFFERSUBDATAPROC p_glNamedBufferSubData;

static void stage_add(const uint8_t *p, uint32_t n)
{
	if (g_stage_len + n > g_stage_cap) {
		uint32_t cap = g_stage_cap ? g_stage_cap : (8u << 20);
		uint8_t *s;
		while (cap < g_stage_len + n)
			cap *= 2;
		s = (uint8_t *)realloc(g_stage, cap);
		if (!s)
			return;
		g_stage = s;
		g_stage_cap = cap;
	}
	memcpy(g_stage + g_stage_len, p, n);
	g_stage_len += n;
}

static void op_tex_image(const uint64_t *a, const uint8_t *p, uint32_t n)
{
	int fn = (int)a[0];
	GLenum target = (GLenum)a[1];
	GLint level = (GLint)(int64_t)a[2], internal = (GLint)(int64_t)a[3];
	GLsizei w = (GLsizei)(int64_t)a[4], h = (GLsizei)(int64_t)a[5], d = (GLsizei)(int64_t)a[6];
	GLint border = (GLint)(int64_t)a[7];
	GLenum format = (GLenum)a[8], type = (GLenum)a[9];
	GLint x = (GLint)(int64_t)a[10], y = (GLint)(int64_t)a[11];
	const void *data = a[12] == 1 ? (const void *)p : NULL;

	switch (fn) {
	case 0:
		P_glTexImage2D(target, level, internal, w, h, border, format, type, data);
		break;
	case 1:
		if (data)
			P_glTexSubImage2D(target, level, x, y, w, h, format, type, data);
		break;
	case 2:
		P_glTexImage3D(target, level, internal, w, h, d, border, format, type, data);
		break;
	case 3:
		P_glCompressedTexImage2D(target, level, (GLenum)internal, w, h, border,
					 data ? (GLsizei)n : 0, data);
		break;
	}
}

static void op_uniform(const uint64_t *a, const uint8_t *p, uint32_t n)
{
	int kind = (int)a[0], type = kind & 15, comps = (kind >> 4) & 255;
	int cols = (kind >> 12) & 15, rows = (kind >> 16) & 15;
	GLint loc = xl_loc((GLint)(int64_t)a[1]);
	GLsizei count = (GLsizei)a[2];
	GLboolean t = (GLboolean)a[3];
	const GLfloat *f = (const GLfloat *)p;
	const GLint *iv = (const GLint *)p;

	if (loc < 0 || count <= 0 || (size_t)count * (size_t)comps * 4 > n)
		return;
	if (type == GLH_UNI_F) {
		switch (comps) {
		case 1: P_glUniform1fv(loc, count, f); break;
		case 2: P_glUniform2fv(loc, count, f); break;
		case 3: P_glUniform3fv(loc, count, f); break;
		case 4: P_glUniform4fv(loc, count, f); break;
		}
	} else if (type == GLH_UNI_I) {
		switch (comps) {
		case 1: P_glUniform1iv(loc, count, iv); break;
		case 2: P_glUniform2iv(loc, count, iv); break;
		case 3: P_glUniform3iv(loc, count, iv); break;
		case 4: P_glUniform4iv(loc, count, iv); break;
		}
	} else if (type == GLH_UNI_U) {
		P_glUniform1uiv(loc, count, (const GLuint *)p);
	} else {
		switch (cols * 16 + rows) {
		case 0x22: P_glUniformMatrix2fv(loc, count, t, f); break;
		case 0x33: P_glUniformMatrix3fv(loc, count, t, f); break;
		case 0x44: P_glUniformMatrix4fv(loc, count, t, f); break;
		case 0x23: P_glUniformMatrix2x3fv(loc, count, t, f); break;
		case 0x32: P_glUniformMatrix3x2fv(loc, count, t, f); break;
		case 0x24: P_glUniformMatrix2x4fv(loc, count, t, f); break;
		case 0x42: P_glUniformMatrix4x2fv(loc, count, t, f); break;
		case 0x34: P_glUniformMatrix3x4fv(loc, count, t, f); break;
		case 0x43: P_glUniformMatrix4x3fv(loc, count, t, f); break;
		}
	}
}

static void op_draw(const uint64_t *a, const uint8_t *p, uint32_t n)
{
	int kind = (int)a[0];
	GLenum mode = (GLenum)a[1], itype = (GLenum)a[4];
	GLint first = (GLint)(int64_t)a[2], base = (GLint)(int64_t)a[7];
	GLsizei count = (GLsizei)(int64_t)a[3], inst = (GLsizei)(int64_t)a[6];
	uintptr_t ioff = (uintptr_t)a[5];
	int nca = (int)a[8], cidx = (int)a[9], k;
	uint32_t at = 0;
	unsigned isz = itype == GL_UNSIGNED_INT ? 4u : itype == GL_UNSIGNED_SHORT ? 2u : 1u;

	GLuint q = 0;
	g_frame_draws++;
	if (g_cc && g_cc->draw_fbo == 0)
		g_frame_dflt_draws++;
	if (g_trace && P_glGenQueries) {
		P_glGenQueries(1, &q);
		P_glBeginQuery(GL_SAMPLES_PASSED, q);
	}
	for (k = 0; k < nca && k < 16; k++) {
		const GlhClientAttr *h = (const GlhClientAttr *)(p + at);
		if (at + sizeof(*h) > n)
			return;
		at += sizeof(*h);
		if (!g_svb[k])
			P_glGenBuffers(1, &g_svb[k]);
		P_glBindBuffer(GL_ARRAY_BUFFER, g_svb[k]);
		P_glBufferData(GL_ARRAY_BUFFER, h->bytes ? h->bytes : 16, h->bytes ? p + at : NULL,
			       GL_STREAM_DRAW);
		if (h->integer)
			P_glVertexAttribIPointer(h->index, (GLint)h->size, h->type, (GLsizei)h->stride,
						 NULL);
		else
			P_glVertexAttribPointer(h->index, (GLint)h->size, h->type,
						(GLboolean)h->normalized, (GLsizei)h->stride, NULL);
		at += (h->bytes + 7) & ~7u;
	}
	if (nca)
		P_glBindBuffer(GL_ARRAY_BUFFER, xl(GLH_K_BUF, g_cc ? g_cc->array_buf : 0));
	if (cidx) {
		if (at + (size_t)count * isz > n)
			return;
		if (!g_sib)
			P_glGenBuffers(1, &g_sib);
		P_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_sib);
		P_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)count * isz, p + at,
			       GL_STREAM_DRAW);
		ioff = 0;
	}
	if (kind == GLH_DRAW_ARRAYS) {
		if (inst > 0)
			P_glDrawArraysInstanced(mode, first, count, inst);
		else
			P_glDrawArrays(mode, first, count);
	} else if (base) {
		if (inst > 0)
			P_glDrawElementsInstancedBaseVertex(mode, count, itype, (const void *)ioff, inst,
							    base);
		else
			P_glDrawElementsBaseVertex(mode, count, itype, (const void *)ioff, base);
	} else {
		if (inst > 0)
			P_glDrawElementsInstanced(mode, count, itype, (const void *)ioff, inst);
		else
			P_glDrawElements(mode, count, itype, (const void *)ioff);
	}
	if (cidx)
		P_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	if (q) {
		GLuint passed = 0;
		GLint vp[4] = { 0 }, cm[4] = { 0 }, hp = 0, df = 0;
		P_glEndQuery(GL_SAMPLES_PASSED);
		P_glGetQueryObjectuiv(q, GL_QUERY_RESULT, &passed);
		P_glDeleteQueries(1, &q);
		P_glGetIntegerv(GL_VIEWPORT, vp);
		P_glGetIntegerv(GL_COLOR_WRITEMASK, cm);
		P_glGetIntegerv(GL_CURRENT_PROGRAM, &hp);
		P_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &df);
		say("  draw: mode %u count %d, %u client arrays%s, program %u (host %d), fbo %u "
		    "(host %d), viewport %d,%d %dx%d, mask %d%d%d%d -> %u samples passed",
		    mode, count, nca, cidx ? " + client indices" : "", g_cc->prog, hp,
		    g_cc->draw_fbo, df, vp[0], vp[1], vp[2], vp[3], cm[0], cm[1], cm[2], cm[3],
		    passed);
	}
}

/* ---- across a savestate restore ----
 * The game's process is rewound; this one is not. While a save exists,
 * objects the game deletes are only put aside (g_park), because a restore
 * can bring back a moment when they were in use. The DLL numbers objects from
 * one counter that only goes up, so at a restore every id from the rewound
 * counter on was made in the future that was left behind: those go, and
 * everything put aside below it comes back. */
static int g_hold;
static Xl g_park[GLH_K_N];
static PFNGLISPROGRAMPROC p_glIsProgram;

static void obj_del_any(int k, GLuint h)
{
	if (!h)
		return;
	if (k != GLH_K_OBJ)
		obj_del(k, h);
	else if (p_glIsProgram && p_glIsProgram(h))
		P_glDeleteProgram(h);
	else
		P_glDeleteShader(h);
}

/* An object the game deleted. Returns 1 when it was put aside. */
static int obj_retire(int k, GLuint id)
{
	GLuint h = xl_get(k, id);
	if (!h)
		return 0;
	xl_set(k, id, 0);
	if (g_hold && !(id < g_park[k].n && g_park[k].v[id])) {
		xl_set_in(&g_park[k], id, h);
		return 1;
	}
	obj_del_any(k, h);
	return 0;
}

static void op_restored(GLuint next)
{
	long gone = 0, back = 0;
	int k;
	uint32_t id;

	for (k = 0; k < GLH_K_N; k++) {
		for (id = next; id < g_xl[k].n; id++)
			if (g_xl[k].v[id]) {
				obj_del_any(k, g_xl[k].v[id]);
				g_xl[k].v[id] = 0;
				if (k == GLH_K_OBJ)
					pinfo_free(id);
				gone++;
			}
		for (id = 1; id < g_park[k].n; id++) {
			GLuint h = g_park[k].v[id];
			if (!h)
				continue;
			g_park[k].v[id] = 0;
			if (id < next && !xl_get(k, id)) {
				xl_set(k, id, h);
				back++;
			} else {
				obj_del_any(k, h);
				if (k == GLH_K_OBJ && id >= next)
					pinfo_free(id);
				gone++;
			}
		}
	}
	say("restore: %ld object(s) from after the save destroyed, %ld deleted since it "
	    "brought back (ids from %u on are new again)",
	    gone, back, next);
}

static void op_names(int del, int kind, const GLuint *ids, uint32_t n)
{
	uint32_t i;
	if (kind < 0 || kind >= GLH_K_N)
		return;
	for (i = 0; i < n; i++) {
		if (!ids[i])
			continue;
		if (del) {
			obj_retire(kind, ids[i]);
		} else if (!xl_get(kind, ids[i])) {
			xl_set(kind, ids[i], obj_new(kind));
		}
	}
}

static void op_readback(uint32_t op, const uint64_t *a)
{
	uint8_t *dst = (uint8_t *)g_h + g_h->rb_off;
	int ok = 0;
	if (op == GLH_OP_READ_PIXELS) {
		GLsizei w = (GLsizei)(int64_t)a[3], h = (GLsizei)(int64_t)a[4];
		if (w > 0 && h > 0 && (uint64_t)w * (uint64_t)h * 16 <= g_h->rb_bytes) {
			P_glReadPixels((GLint)(int64_t)a[1], (GLint)(int64_t)a[2], w, h, (GLenum)a[5],
				       (GLenum)a[6], dst);
			ok = 1;
		}
	} else {
		GLint w = 0, h = 0;
		GLenum target = (GLenum)a[1];
		GLint level = (GLint)(int64_t)a[2];
		P_glGetTexLevelParameteriv(target, level, GL_TEXTURE_WIDTH, &w);
		P_glGetTexLevelParameteriv(target, level, GL_TEXTURE_HEIGHT, &h);
		if (w > 0 && h > 0 && (uint64_t)w * (uint64_t)h * 16 <= g_h->rb_bytes) {
			P_glGetTexImage(target, level, (GLenum)a[3], (GLenum)a[4], dst);
			ok = 1;
		}
	}
	g_h->rb_ok = ok;
	MemoryBarrier();
	InterlockedExchange((volatile LONG *)&g_h->rb_done, (LONG)a[0]);
}

static HANDLE g_ev_done;
static uint32_t g_frame_bytes, g_frame_recs;
static char g_dir[MAX_PATH];

static void bmp_write(const char *name, const uint8_t *rgba, int w, int h)
{
	char path[MAX_PATH];
	BITMAPFILEHEADER fh;
	BITMAPINFOHEADER ih;
	FILE *f;
	int i, n = w * h;
	uint8_t *bgra = (uint8_t *)malloc((size_t)n * 4);
	if (!bgra)
		return;
	for (i = 0; i < n; i++) {
		bgra[i * 4 + 0] = rgba[i * 4 + 2];
		bgra[i * 4 + 1] = rgba[i * 4 + 1];
		bgra[i * 4 + 2] = rgba[i * 4 + 0];
		bgra[i * 4 + 3] = 255;
	}
	snprintf(path, sizeof(path), "%s\\%s", g_dir, name);
	f = fopen(path, "wb");
	if (f) {
		memset(&fh, 0, sizeof(fh));
		memset(&ih, 0, sizeof(ih));
		fh.bfType = 0x4D42;
		fh.bfOffBits = sizeof(fh) + sizeof(ih);
		fh.bfSize = fh.bfOffBits + (DWORD)n * 4;
		ih.biSize = sizeof(ih);
		ih.biWidth = w;
		ih.biHeight = h; /* bottom-up, as GL reads */
		ih.biPlanes = 1;
		ih.biBitCount = 32;
		fwrite(&fh, sizeof(fh), 1, f);
		fwrite(&ih, sizeof(ih), 1, f);
		fwrite(bgra, 4, (size_t)n, f);
		fclose(f);
	}
	free(bgra);
}

/* Saves one framebuffer's colour attachment 0 and logs its average colour. */
static void dump_fbo(GLuint fbo, int w, int h, const char *name)
{
	GLint r = 0, pack = 4, rb = 0;
	uint8_t *px;
	double sum[3] = { 0, 0, 0 };
	int i, n = w * h;
	if (!fbo || w <= 0 || h <= 0 || !(px = (uint8_t *)malloc((size_t)n * 4)))
		return;
	P_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &r);
	P_glGetIntegerv(GL_READ_BUFFER, &rb);
	P_glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
	P_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	P_glReadBuffer(GL_COLOR_ATTACHMENT0);
	P_glPixelStorei(GL_PACK_ALIGNMENT, 4);
	P_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	P_glPixelStorei(GL_PACK_ALIGNMENT, pack);
	P_glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)r);
	P_glReadBuffer((GLenum)rb);
	for (i = 0; i < n; i++) {
		sum[0] += px[i * 4];
		sum[1] += px[i * 4 + 1];
		sum[2] += px[i * 4 + 2];
	}
	bmp_write(name, px, w, h);
	say("dumped %s %dx%d, average colour %.1f %.1f %.1f", name, w, h, sum[0] / n, sum[1] / n,
	    sum[2] / n);
	free(px);
}
/* Per-frame census of the game's framebuffer: how much of it is non-black. */
static void frame_stats(long frame)
{
	static uint8_t *px;
	static int cap;
	GLint r = 0, pack = 4, rb = 0;
	int i, w = g_dflt_w, h = g_dflt_h, n = w * h;
	uint32_t lit = 0, alpha = 0;
	double sum[4] = { 0, 0, 0, 0 };
	if (n <= 0)
		return;
	if (n > cap) {
		free(px);
		px = (uint8_t *)malloc((size_t)n * 4);
		cap = px ? n : 0;
		if (!px)
			return;
	}
	P_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &r);
	P_glGetIntegerv(GL_READ_BUFFER, &rb);
	P_glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
	P_glBindFramebuffer(GL_READ_FRAMEBUFFER, dflt_fbo());
	P_glReadBuffer(GL_COLOR_ATTACHMENT0);
	P_glPixelStorei(GL_PACK_ALIGNMENT, 4);
	P_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	P_glPixelStorei(GL_PACK_ALIGNMENT, pack);
	P_glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)r);
	P_glReadBuffer((GLenum)rb);
	for (i = 0; i < n; i++) {
		const uint8_t *q = px + (size_t)i * 4;
		if (q[3])
			alpha++;
		if (!(q[0] | q[1] | q[2]))
			continue;
		lit++;
		sum[0] += q[0];
		sum[1] += q[1];
		sum[2] += q[2];
		sum[3] += q[3];
	}
	say("frame %ld: %u of %d pixels non-black (%.2f%%), their average %.1f %.1f %.1f "
	    "alpha %.1f; %u with alpha; %u draws, %u to the screen",
	    frame, lit, n, 100.0 * lit / n, lit ? sum[0] / lit : 0, lit ? sum[1] / lit : 0,
	    lit ? sum[2] / lit : 0, lit ? sum[3] / lit : 0, alpha, g_frame_draws,
	    g_frame_dflt_draws);
}

static LARGE_INTEGER g_qpf;
static double g_replay_ms;
static int g_errs_logged;

/* One record. Returns 0 to carry on, -1 on a record that makes no sense. */
static int run(uint32_t op, uint16_t flags, const uint64_t *a, uint32_t na, const uint8_t *p,
	       uint32_t n)
{
	uint64_t z[16] = { 0 };

	if (flags & GLH_REC_STAGED) {
		p = g_stage;
		n = g_stage_len;
	}
	/* Short argument lists read as zeros past the end. */
	if (na < 16 && op < GLH_OP_GEN) {
		memcpy(z, a, na * 8);
		a = z;
	}
	g_cur_op = op;
	if (g_trace && op != GLH_OP_DRAW && op != GLH_OP_BLOB) {
		char line[256];
		int len = snprintf(line, sizeof(line), "  %s", op_name(op));
		uint32_t i;
		for (i = 0; i < na && i < 8 && len < 200; i++)
			len += snprintf(line + len, sizeof(line) - (size_t)len, " %llX",
					(unsigned long long)a[i]);
		if (op == GLH_OP_UNIFORM)
			len += snprintf(line + len, sizeof(line) - (size_t)len, " %s -> host loc %d",
					uni_name((GLint)(int64_t)a[1]),
					xl_loc((GLint)(int64_t)a[1]));
		else if (!strncmp(op_name(op), "glUniform", 9))
			len += snprintf(line + len, sizeof(line) - (size_t)len, " %s -> host loc %d",
					uni_name((GLint)(int64_t)a[0]),
					xl_loc((GLint)(int64_t)a[0]));
		if (op == GLH_OP_UNIFORM && n >= 4 && (a[0] & 15) == GLH_UNI_F)
			snprintf(line + len, sizeof(line) - (size_t)len, " [%g ...]",
				 *(const float *)p);
		else if (n && len < 200)
			snprintf(line + len, sizeof(line) - (size_t)len, " +%u bytes", n);
		say("%s", line);
	}
	if (op == GLH_OP_CTX) {
		ctx_switch(a[0]);
		return 0;
	}
	if (op == GLH_OP_CTX_DELETE) {
		ctx_delete(a[0]);
		return 0;
	}
	if (op == GLH_OP_BLOB) {
		stage_add(p, n);
		return 0;
	}
	if (!g_cc) {
		static int said;
		if (said++ < 10)
			say("%s with no context current; skipped", op_name(op));
		return 0;
	}
	switch (op) {
	case GLH_OP_NOP:
		break;
	case GLH_OP_PRESENT: {
		static int f11_was, dumps;
		SHORT ks = GetAsyncKeyState(VK_F11);
		int f11 = (ks & 0x8000) != 0;
		int dump = (f11 && !f11_was) || (ks & 1);
		f11_was = f11;
		if (dump) {
			frame_stats((long)g_h->presented + 1);
			char name[64];
			dumps++;
			say("frame %ld: %u draws, %u into the default framebuffer, draw fbo %u, "
			    "read fbo %u, program %u",
			    (long)g_h->presented + 1, g_frame_draws, g_frame_dflt_draws,
			    g_cc->draw_fbo, g_cc->read_fbo, g_cc->prog);
			snprintf(name, sizeof(name), "glhost%d_frame.bmp", dumps);
			dump_fbo(dflt_fbo(), g_dflt_w, g_dflt_h, name);
		}
		present((HWND)(uintptr_t)a[0], (int)a[3]);
		if (dump && g_cc->pres_fbo && !g_p.failed) {
			char name[64];
			snprintf(name, sizeof(name), "glhost%d_shown.bmp", dumps);
			if (g_p.interop)
				p_dxLock(g_p.idev, 1, &g_p.iobj);
			dump_fbo(g_cc->pres_fbo, (int)g_p.cw, (int)g_p.ch, name);
			if (g_p.interop)
				p_dxUnlock(g_p.idev, 1, &g_p.iobj);
		}
		g_frame_draws = g_frame_dflt_draws = 0;
		if (g_trace)
			say("---- end of traced frame");
		g_trace = dump;
		if (g_trace)
			say("---- tracing frame %ld", (long)g_h->presented + 1);
		break;
	}
	case GLH_OP_DFLT_SIZE:
		if ((int)a[0] > 0 && (int)a[1] > 0) {
			g_want_w = (int)a[0];
			g_want_h = (int)a[1];
			dflt_storage();
		}
		break;
	case GLH_OP_UNI_LOC:
		if (n)
			uni_loc((GLuint)a[0], (GLint)(int64_t)a[1], (const char *)p);
		break;
	case GLH_OP_GEN_NAMES:
	case GLH_OP_DEL_NAMES:
		op_names(op == GLH_OP_DEL_NAMES, (int)a[0], (const GLuint *)p, n / 4);
		break;
	case GLH_OP_CREATE_SHADER:
		xl_set(GLH_K_OBJ, (GLuint)a[0], P_glCreateShader((GLenum)a[1]));
		break;
	case GLH_OP_CREATE_PROGRAM:
		xl_set(GLH_K_OBJ, (GLuint)a[0], P_glCreateProgram());
		break;
	case GLH_OP_SHADER_SOURCE: {
		const GLchar *s = (const GLchar *)p;
		if (n)
			P_glShaderSource(xl(GLH_K_OBJ, (GLuint)a[0]), 1, &s, NULL);
		break;
	}
	case GLH_OP_BIND_ATTRIB:
		if (n)
			P_glBindAttribLocation(xl(GLH_K_OBJ, (GLuint)a[0]), (GLuint)a[1],
					       (const GLchar *)p);
		break;
	case GLH_OP_BIND_FRAGDATA:
		if (n && P_glBindFragDataLocation)
			P_glBindFragDataLocation(xl(GLH_K_OBJ, (GLuint)a[0]), (GLuint)a[1],
						 (const GLchar *)p);
		break;
	case GLH_OP_TF_VARYINGS: {
		const GLchar *names[16];
		uint32_t i, at = 0, cnt = (uint32_t)a[1];
		for (i = 0; i < cnt && i < 16 && at < n; i++) {
			names[i] = (const GLchar *)p + at;
			at += (uint32_t)strlen(names[i]) + 1;
		}
		P_glTransformFeedbackVaryings(xl(GLH_K_OBJ, (GLuint)a[0]), (GLsizei)i, names,
					      (GLenum)a[2]);
		break;
	}
	case GLH_OP_UNIFORM:
		op_uniform(a, p, n);
		break;
	case GLH_OP_TEX_IMAGE:
		op_tex_image(a, p, n);
		break;
	case GLH_OP_TEX_PARAMV: {
		uint32_t v[4] = { 0, 0, 0, 0 };
		memcpy(v, p, n > 16 ? 16 : n);
		if (a[2])
			P_glTexParameterfv((GLenum)a[0], (GLenum)a[1], (const GLfloat *)v);
		else
			P_glTexParameteriv((GLenum)a[0], (GLenum)a[1], (const GLint *)v);
		break;
	}
	case GLH_OP_BUFFER_DATA:
		P_glBufferData((GLenum)a[0], (GLsizeiptr)a[1], a[3] && n ? p : NULL, (GLenum)a[2]);
		break;
	case GLH_OP_BUFFER_SUB:
		P_glBufferSubData((GLenum)a[0], (GLintptr)a[1], (GLsizeiptr)n, p);
		break;
	case GLH_OP_BUFFER_PUT: {
		GLuint b = xl(GLH_K_BUF, (GLuint)a[0]);
		if (p_glNamedBufferSubData) {
			p_glNamedBufferSubData(b, (GLintptr)a[1], (GLsizeiptr)n, p);
		} else {
			GLint prev = 0;
			P_glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &prev);
			P_glBindBuffer(GL_COPY_WRITE_BUFFER, b);
			P_glBufferSubData(GL_COPY_WRITE_BUFFER, (GLintptr)a[1], (GLsizeiptr)n, p);
			P_glBindBuffer(GL_COPY_WRITE_BUFFER, (GLuint)prev);
		}
		break;
	}
	case GLH_OP_ATTRIB_PTR:
		if (a[7])
			break; /* client memory: the draw brings it */
		if (a[6])
			P_glVertexAttribIPointer((GLuint)a[0], (GLint)a[1], (GLenum)a[2], (GLsizei)a[4],
						 (const void *)(uintptr_t)a[5]);
		else
			P_glVertexAttribPointer((GLuint)a[0], (GLint)a[1], (GLenum)a[2],
						(GLboolean)a[3], (GLsizei)a[4],
						(const void *)(uintptr_t)a[5]);
		break;
	case GLH_OP_ATTRIBV: {
		float v[4] = { 0, 0, 0, 1 };
		if (a[2]) {
			if (n >= 4)
				P_glVertexAttrib4Nubv((GLuint)a[0], p);
			break;
		}
		memcpy(v, p, n > 16 ? 16 : n);
		switch ((int)a[1]) {
		case 1: P_glVertexAttrib1fv((GLuint)a[0], v); break;
		case 2: P_glVertexAttrib2fv((GLuint)a[0], v); break;
		case 3: P_glVertexAttrib3fv((GLuint)a[0], v); break;
		default: P_glVertexAttrib4fv((GLuint)a[0], v); break;
		}
		break;
	}
	case GLH_OP_DRAW:
		op_draw(a, p, n);
		break;
	case GLH_OP_DRAW_BUFFERS: {
		GLenum b[16];
		uint32_t i, cnt = n / 4;
		if (cnt > 16)
			cnt = 16;
		memcpy(b, p, cnt * 4);
		if (g_cc->draw_fbo == 0)
			for (i = 0; i < cnt; i++)
				b[i] = dflt_buf(b[i]);
		P_glDrawBuffers((GLsizei)cnt, b);
		break;
	}
	case GLH_OP_STIPPLE:
		if (n >= 128)
			P_glPolygonStipple(p);
		break;
	case GLH_OP_READ_PIXELS:
	case GLH_OP_GET_TEX_IMAGE:
		op_readback(op, a);
		SetEvent(g_ev_done);
		break;
	case GLH_OP_PIXEL_STORE:
		P_glPixelStorei((GLenum)a[0], (GLint)(int64_t)a[1]);
		break;
	case GLH_OP_HOLD:
		if (!g_hold && a[0])
			say("a save exists: deleted objects are kept from here on");
		g_hold = a[0] != 0;
		break;
	case GLH_OP_RESTORED:
		op_restored((GLuint)a[0]);
		break;
	default: {
		int r = op >= GLH_OP_GEN ? gen_dispatch(op, a, na) : 0;
		if (r <= 0) {
			say("record op %u (%s) with %u args not understood", op, op_name(op), na);
			return -1;
		}
		break;
	}
	}
	if (flags & GLH_REC_STAGED)
		g_stage_len = 0;
	{
		GLenum e = glGetError();
		if (e) {
			g_h->gl_errors++;
			if (g_errs_logged++ < 60)
				say("GL error 0x%04X after %s (0x%llX, 0x%llX, 0x%llX)", e, op_name(op),
				    (unsigned long long)(na > 0 ? a[0] : 0),
				    (unsigned long long)(na > 1 ? a[1] : 0),
				    (unsigned long long)(na > 2 ? a[2] : 0));
		}
	}
	return 0;
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

/* The host half of GLSW_FRAMEGRAPH (see the DLL's ft_dump): every record of the
 * chosen frame, how long replaying it took and how long the host sat idle
 * waiting for the game before it. Rows line up with the DLL's seq. */
typedef struct HtRec {
	uint32_t op, bytes;
	LONGLONG t0, t1;
} HtRec;
#define HT_MAX 65536
static HtRec *g_ht;
static int g_ht_n, g_ht_done;
static LONGLONG g_ht_start, g_last_present;

static void ht_dump(uint32_t frame)
{
	double us = 1e6 / (double)g_qpf.QuadPart;
	char path[MAX_PATH];
	FILE *out;
	LONGLONG prev = g_ht_start, idle = 0, busy = 0, end = g_ht_n ? g_ht[g_ht_n - 1].t1 : 0;
	static LONGLONG by_time[256];
	static uint32_t by_n[256];
	int i, k;

	snprintf(path, sizeof(path), "%s\\glframe_%u_host.csv", g_dir, frame);
	out = fopen(path, "w");
	if (out)
		fprintf(out, "seq,op,bytes,start_us,idle_before_us,replay_us\n");
	for (i = 0; i < g_ht_n; i++) {
		HtRec *e = &g_ht[i];
		LONGLONG g = e->t0 - prev, d = e->t1 - e->t0;
		int slot = e->op < 255 ? (int)e->op : 255;
		idle += g;
		busy += d;
		by_time[slot] += d;
		by_n[slot]++;
		if (out)
			fprintf(out, "%d,%s,%u,%.1f,%.1f,%.1f\n", i, op_name(e->op), e->bytes,
				(double)(e->t0 - g_ht_start) * us, (double)g * us, (double)d * us);
		prev = e->t1;
	}
	if (out)
		fclose(out);
	say("frametime %u: %.2f ms, %d records; %.2f ms replaying, %.2f ms idle waiting for "
	    "the game -> %s",
	    frame, (double)(end - g_ht_start) * us / 1000.0, g_ht_n, (double)busy * us / 1000.0,
	    (double)idle * us / 1000.0, path);
	for (k = 0; k < 10; k++) {
		int best = -1, j;
		for (j = 0; j < 256; j++)
			if (by_n[j] && (best < 0 || by_time[j] > by_time[best]))
				best = j;
		if (best < 0)
			break;
		say("frametime %u:   %-32s x%-5u %.2f ms", frame, op_name((uint32_t)best), by_n[best],
		    (double)by_time[best] * us / 1000.0);
		by_n[best] = 0;
	}
	free(g_ht);
	g_ht = NULL;
	g_ht_done = 1;
}

/* Runs every record published so far. Returns records run, or -1. */
static int ring_drain(uint8_t **scratch, uint32_t *scap)
{
	const uint8_t *ring = (const uint8_t *)g_h + g_h->ring_off;
	uint32_t tail = g_h->ring_tail, mask = g_h->ring_bytes - 1;
	int done = 0;

	for (;;) {
		uint32_t head = g_h->ring_head, body, pos;
		const uint8_t *p;
		GlhRec rec;
		LARGE_INTEGER t0, t1;

		MemoryBarrier();
		if (head == tail)
			break;
		ring_copy(&rec, tail, sizeof(rec));
		if (rec.bytes < sizeof(rec) || (rec.bytes & 7) || rec.bytes > head - tail ||
		    (uint64_t)sizeof(rec) + (uint64_t)rec.nargs * 8 + rec.paylen > rec.bytes) {
			say("ring record op %u bytes %u at %u (head %u) is corrupt", rec.op, rec.bytes,
			    tail, head);
			return -1;
		}
		body = rec.bytes - (uint32_t)sizeof(rec);
		pos = (tail + (uint32_t)sizeof(rec)) & mask;
		if (pos + body <= g_h->ring_bytes) {
			p = ring + pos;
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
		QueryPerformanceCounter(&t0);
		if (run(rec.op, rec.flags, (const uint64_t *)p, rec.nargs, p + (size_t)rec.nargs * 8,
			rec.paylen) < 0)
			return -1;
		QueryPerformanceCounter(&t1);
		g_replay_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
		g_frame_bytes += rec.bytes;
		g_frame_recs++;
		tail += rec.bytes;
		if (g_h->time_frame && !g_ht_done &&
		    (uint32_t)g_h->presented + 1 == g_h->time_frame) {
			if (!g_ht) {
				g_ht = (HtRec *)malloc(HT_MAX * sizeof(HtRec));
				g_ht_n = 0;
				g_ht_start = g_last_present ? g_last_present : t0.QuadPart;
			}
			if (g_ht && g_ht_n < HT_MAX) {
				g_ht[g_ht_n].op = rec.op;
				g_ht[g_ht_n].bytes = rec.bytes;
				g_ht[g_ht_n].t0 = t0.QuadPart;
				g_ht[g_ht_n].t1 = t1.QuadPart;
				g_ht_n++;
			}
		}
		if (rec.op == GLH_OP_PRESENT)
			g_last_present = t1.QuadPart;
		if (rec.op == GLH_OP_PRESENT && g_ht)
			ht_dump((uint32_t)g_h->presented + 1);
		if (rec.op == GLH_OP_PRESENT) {
			g_h->bytes_frame = g_frame_bytes;
			g_h->records_frame = g_frame_recs;
			InterlockedIncrement((volatile LONG *)&g_h->presented);
			SetEvent(g_ev_done);
			if (g_h->presented == 1 || g_h->presented % 300 == 0)
				say("frame %ld: %u records, %u KB, %.1f ms replaying, %u GL errors",
				    (long)g_h->presented, g_frame_recs, g_frame_bytes >> 10, g_replay_ms,
				    g_h->gl_errors);
			g_frame_bytes = g_frame_recs = 0;
			g_replay_ms = 0;
		}
		if (++done % 64 == 0 || rec.op == GLH_OP_PRESENT)
			InterlockedExchange((volatile LONG *)&g_h->ring_tail, (LONG)tail);
	}
	InterlockedExchange((volatile LONG *)&g_h->ring_tail, (LONG)tail);
	return done;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	return DefWindowProcA(h, m, w, l);
}

static int gl_init(void)
{
	WNDCLASSA wc;
	PIXELFORMATDESCRIPTOR pfd;
	HGLRC tmp;
	int fmt, miss;

	g_ogl = LoadLibraryA("opengl32.dll");
	memset(&wc, 0, sizeof(wc));
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = wndproc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "glhost";
	RegisterClassA(&wc);
	g_win = CreateWindowA("glhost", "glhost", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL,
			      wc.hInstance, NULL);
	g_dc = g_win ? GetDC(g_win) : NULL;
	if (!g_dc) {
		say("no window for the GL context");
		return 0;
	}
	memset(&pfd, 0, sizeof(pfd));
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	pfd.cDepthBits = 24;
	pfd.cStencilBits = 8;
	fmt = ChoosePixelFormat(g_dc, &pfd);
	if (!fmt || !SetPixelFormat(g_dc, fmt, &pfd)) {
		say("pixel format failed (%lu)", GetLastError());
		return 0;
	}
	tmp = wglCreateContext(g_dc);
	if (!tmp || !wglMakeCurrent(g_dc, tmp)) {
		say("wglCreateContext failed (%lu)", GetLastError());
		return 0;
	}
	p_wglCreateContextAttribsARB =
		(PFNWGLCREATECONTEXTATTRIBSARBPROC)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
	g_root = ctx_make(NULL);
	wglMakeCurrent(NULL, NULL);
	wglDeleteContext(tmp);
	if (!g_root || !wglMakeCurrent(g_dc, g_root)) {
		say("could not create the root context");
		return 0;
	}
	miss = gen_load();
	p_glNamedBufferSubData = (PFNGLNAMEDBUFFERSUBDATAPROC)(void *)glh_proc("glNamedBufferSubData");
	p_glIsProgram = (PFNGLISPROGRAMPROC)(void *)glh_proc("glIsProgram");
	P_glGenQueries = (PFNGLGENQUERIESPROC)(void *)glh_proc("glGenQueries");
	P_glDeleteQueries = (PFNGLDELETEQUERIESPROC)(void *)glh_proc("glDeleteQueries");
	P_glBeginQuery = (PFNGLBEGINQUERYPROC)(void *)glh_proc("glBeginQuery");
	P_glEndQuery = (PFNGLENDQUERYPROC)(void *)glh_proc("glEndQuery");
	P_glGetQueryObjectuiv = (PFNGLGETQUERYOBJECTUIVPROC)(void *)glh_proc("glGetQueryObjectuiv");
	say("GL %s | %s | %s", (const char *)glGetString(GL_VERSION),
	    (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VENDOR));
	if (miss)
		say("%d forwarded GL functions missing from the driver", miss);
	return 1;
}

int main(int argc, char **argv)
{
	char dir[MAX_PATH], path[MAX_PATH], evname[128], *slash;
	HANDLE map, ev, game;
	DWORD pid;
	uint8_t *scratch = NULL;
	uint32_t scap = 0;

	map = argc >= 3 ? OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, argv[1]) : NULL;
	g_h = map ? (GlhHeader *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
	if (g_h && (g_h->magic != GLH_MAGIC || g_h->version != GLH_VERSION))
		g_h = NULL;
	if (g_h) {
		g_h->host_pid = GetCurrentProcessId();
		InterlockedIncrement((volatile LONG *)&g_h->heartbeat);
	}
	/* Real pixel sizes for the game's window, not 96-DPI virtualised ones. */
	{
		typedef BOOL(WINAPI * T_dpi)(HANDLE);
		T_dpi f = (T_dpi)(void *)GetProcAddress(GetModuleHandleA("user32.dll"),
							 "SetProcessDpiAwarenessContext");
		if (f)
			f((HANDLE)-4); /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
	}
	GetModuleFileNameA(NULL, dir, sizeof(dir));
	slash = strrchr(dir, '\\');
	if (slash)
		*slash = 0;
	lstrcpynA(g_dir, dir, sizeof(g_dir));
	snprintf(path, sizeof(path), "%s\\glhost.log", dir);
	g_log = fopen(path, "w");
	if (!g_log) {
		char tmp[MAX_PATH];
		if (GetTempPathA(sizeof(tmp), tmp)) {
			snprintf(path, sizeof(path), "%sglhost.log", tmp);
			g_log = fopen(path, "w");
		}
	}
	QueryPerformanceFrequency(&g_qpf);
	if (argc < 3) {
		say("usage: glhost64.exe <mapping> <game pid>");
		return 2;
	}
	pid = (DWORD)strtoul(argv[2], NULL, 10);
	say("---- glhost %s for pid %lu", argv[1], (unsigned long)pid);
	if (!g_h) {
		say("mapping %s missing or not ours", argv[1]);
		return 3;
	}
	snprintf(evname, sizeof(evname), "%s_f", argv[1]);
	ev = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, evname);
	snprintf(evname, sizeof(evname), "%s_d", argv[1]);
	g_ev_done = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, evname);
	game = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (!ev || !g_ev_done || !game) {
		g_h->state = GLH_FAILED;
		say("events %p %p, game %p", (void *)ev, (void *)g_ev_done, (void *)game);
		return 4;
	}
	if (!gl_init()) {
		g_h->state = GLH_FAILED;
		return 5;
	}
	InterlockedExchange((volatile LONG *)&g_h->state, GLH_READY);
	say("ready");

	for (;;) {
		HANDLE w[2] = { ev, game };
		int n;
		MSG msg;

		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
			DispatchMessageA(&msg);
		InterlockedIncrement((volatile LONG *)&g_h->heartbeat);
		n = ring_drain(&scratch, &scap);
		if (n < 0) {
			InterlockedExchange((volatile LONG *)&g_h->state, GLH_FAILED);
			say("giving up after %ld frame(s)", (long)g_h->presented);
			break;
		}
		if (g_h->quit) {
			say("renderer asked us to quit");
			break;
		}
		if (n)
			continue;
		g_h->sleeping = 1;
		MemoryBarrier();
		if (g_h->ring_head == g_h->ring_tail &&
		    WaitForMultipleObjects(2, w, FALSE, 100) == WAIT_OBJECT_0 + 1) {
			say("game exited");
			break;
		}
		g_h->sleeping = 0;
	}
	pres_free();
	free(scratch);
	if (g_h->state != GLH_FAILED)
		InterlockedExchange((volatile LONG *)&g_h->state, GLH_EXITED);
	say("exited after %ld frame(s)", (long)g_h->presented);
	return 0;
}
