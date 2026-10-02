/* glsl.h - a small software GLSL interpreter.
 *
 * Haydee (and any modern GL game) transforms and shades through GLSL, so a CPU
 * renderer that wants a correct frame has to RUN those shaders, not skip them.
 * This is a tree-walking interpreter for the subset of GLSL 1.40/3.30 that real
 * game shaders use: float/int/bool, vecN, matN, samplers, swizzles, the common
 * built-ins, if/for/return, and user functions. It is deliberately independent of
 * gl_sw.c so it can be built and validated in a harness against real shader source
 * before it is wired into the pipeline.
 *
 * A value is up to a mat4 (16 floats), tagged with rows x cols: a scalar is 1x1,
 * vecN is Nx1, matN is NxN, matCxR is R rows x C cols, stored column-major to
 * match OpenGL. */
#ifndef GLSL_H
#define GLSL_H

typedef struct GlslVal {
	float f[16];
	unsigned char rows; /* components of a vector, or rows of a matrix */
	unsigned char cols; /* 1 for scalar/vector, C for matN / matCxR */
	unsigned char is_int; /* int/bool semantics (still stored as float) */
} GlslVal;

typedef struct GlslProg GlslProg;

/* Resolve texture(sampler, coord). The interpreter calls this for every texture
 * fetch: `unit` is the sampler's bound texture unit (glUniform1i value), `coord`
 * is up to 4 components, `out` receives RGBA. Return 0 to yield (0,0,0,1). The
 * host (gl_sw.c) supplies this; the harness supplies a stub or a checkerboard. */
typedef int (*GlslSampleFn)(void *ctx, int unit, const float *coord, int ncoord,
			    float *out_rgba);

/* Compile shader source into a runnable program. On failure returns NULL and
 * writes a message into errbuf. stage is 'v', 'f', or 'g'. */
GlslProg *glsl_compile(const char *src, int stage, char *errbuf, int errcap);
void glsl_free(GlslProg *p);
/* Brings *clone up to src's current globals - uniforms included - allocating
 * or re-basing it when it is new or src is a different compile. The clone
 * shares src's code and array uniforms, so it is only valid while src lives
 * and nobody calls glsl_set on src while the clone runs. glsl_run on two
 * clones of one program may proceed in parallel. Returns 0 on failure. */
int glsl_sync(GlslProg **clone, GlslProg *src);

/* Set a uniform or an input (attribute/varying) by name, n floats (1,2,3,4,9,16).
 * Unknown names are ignored, so a caller can set everything it has. */
void glsl_set(GlslProg *p, const char *name, const float *v, int n);
/* Bind a sampler uniform to a texture unit (the glUniform1i value). */
void glsl_set_sampler(GlslProg *p, const char *name, int unit);

/* Run void main(). Returns 1 on success, 0 on a runtime error (message in the
 * buffer from compile is reused). sample/ctx service texture() calls. */
int glsl_run(GlslProg *p, GlslSampleFn sample, void *ctx);

/* Read an output/builtin (out varying, gl_Position, or a fragment out) after a
 * run. Returns the component count, 0 if absent. */
int glsl_get(GlslProg *p, const char *name, float *out, int cap);

/* Names of the declared inputs/uniforms, for a caller that wants to enumerate
 * what to feed. Returns count; fills up to cap name pointers (owned by p). */
int glsl_inputs(GlslProg *p, const char **names, int *sizes, int cap);

/* Declared interface variables, with their storage qualifier and layout location. */
enum { GLSL_Q_NONE, GLSL_Q_IN, GLSL_Q_OUT, GLSL_Q_UNIFORM };
typedef struct GlslVar {
	const char *name; /* owned by the program */
	int qual;         /* GLSL_Q_* */
	int location;     /* layout(location = N), -1 if none */
	int rows, cols;   /* vecN: N x 1; matCxR: R x C */
	int is_sampler;
	int arr_len;      /* 0 if not an array */
} GlslVar;
int glsl_vars(GlslProg *p, GlslVar *out, int cap);

/* 0 if name is not a global of p, 1 for a value, 2 for a sampler. */
int glsl_kind(GlslProg *p, const char *name);

#endif
