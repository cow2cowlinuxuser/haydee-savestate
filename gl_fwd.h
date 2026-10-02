/* Forwarding GL calls to glhost64.exe: the DLL side. See glhost.h. */
#ifndef GL_FWD_H
#define GL_FWD_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdint.h>
#include <stddef.h>

/* Nonzero while the host is taking records. */
extern volatile LONG g_glfwd;
#define glfwd_on() (g_glfwd != 0)

void fw_call(uint32_t op, const uint64_t *a, int na, const void *pay, uint32_t paylen);

static inline uint64_t fw_f(float f)
{
	union {
		float f;
		uint32_t u;
	} v;
	v.f = f;
	return v.u;
}

static inline uint64_t fw_d(double d)
{
	union {
		double d;
		uint64_t u;
	} v;
	v.d = d;
	return v.u;
}

/* gl_fwd.c, called from gl_sw.c. */
void glfwd_start(void);
void glfwd_present(HWND hwnd, int w, int h, int interval);
void glfwd_ctx_delete(void *ctx);
void glfwd_restored(void);
void glfwd_saved(void);

/* gl_sw.c state the forwarding layer reads. */
typedef struct GlswClientAttr {
	unsigned index;
	int size;
	GLenum type;
	int normalized;
	int stride;
	unsigned divisor;
	const void *ptr;
} GlswClientAttr;

void *glsw_ctx(void);
GLuint glsw_next_id(void);
void glsw_win_size(int *w, int *h);
GLuint glsw_bound_buffer(GLenum target);
GLuint glsw_vao_ebo(void);
const unsigned char *glsw_buffer_data(GLuint id, size_t *size);
int glsw_client_attrs(GlswClientAttr *out, int max);
int glsw_tex_level_size(GLenum target, int level, int *w, int *h);
GLuint glsw_bound_fbo(int read);
int glsw_fbo_atts(GLuint fbo, GLuint *out, int cap, int *ncolor);
GLuint glsw_bound_tex(void);
GLuint glsw_cur_prog(void);
int glsw_sampled(GLuint *out, int cap);
void glsw_obj_desc(GLuint id, char *buf, int cap);
void gl_fwd_log(const char *fmt, ...);

#endif
