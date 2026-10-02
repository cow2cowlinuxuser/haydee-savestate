/* glsl_harness.c - validate the software GLSL interpreter against real shaders,
 * with no game and no renderer. Compiles Haydee's actual vertex/fragment shaders,
 * feeds known uniforms/inputs, runs them, and checks the outputs against
 * hand-computed values. This is the "prove the core before wiring it in" step. */
#include "glsl.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail;

static int approx(float a, float b) { return fabsf(a - b) < 1e-3f; }

static void check(const char *what, const float *got, const float *want, int n)
{
	int i, ok = 1;
	for (i = 0; i < n; i++) if (!approx(got[i], want[i])) ok = 0;
	printf("  %-28s got[", what);
	for (i = 0; i < n; i++) printf("%s%.3f", i ? "," : "", got[i]);
	printf("] want[");
	for (i = 0; i < n; i++) printf("%s%.3f", i ? "," : "", want[i]);
	printf("]  %s\n", ok ? "OK" : "*** FAIL");
	if (!ok) g_fail++;
}

/* A stub sampler: returns the coord in rg, 0.5 in b, 1 in a (deterministic). */
static int stub_sample(void *ctx, int unit, const float *coord, int nc, float *rgba)
{
	(void)ctx; (void)unit; (void)nc;
	rgba[0] = coord[0]; rgba[1] = coord[1]; rgba[2] = 0.5f; rgba[3] = 1.0f;
	return 1;
}

static void identity(float *m, int N)
{
	int i, j;
	for (i = 0; i < N; i++) for (j = 0; j < N; j++) m[i * N + j] = (i == j) ? 1.0f : 0.0f;
}

/* -------- Haydee's actual shaders (from gl_sw.log) -------- */
static const char *VS_UI =
	"#version 330 core\n"
	"uniform mat3 matrix;\n"
	"layout(location = 0) in vec2 v_Vertex;\n"
	"layout(location = 1) in vec2 v_TexCoord;\n"
	"layout(location = 2) in vec4 v_Color;\n"
	"out vec2 f_TexCoord;\n"
	"out vec4 f_Color;\n"
	"void main () {\n"
	"  vec3 pos = matrix * vec3 ( v_Vertex, 1.0 );\n"
	"  gl_Position = vec4 ( pos, 1.0 );\n"
	"  f_TexCoord = v_TexCoord;\n"
	"  f_Color = v_Color;\n"
	"}\n";

static const char *VS_MODEL =
	"#version 330 core\n"
	"uniform mat4 localView;\n"
	"uniform mat4 proj;\n"
	"layout(location = 0) in vec3 v_Vertex;\n"
	"layout(location = 1) in vec2 v_TexCoord;\n"
	"out vec2 f_TexCoord;\n"
	"void main () {\n"
	"  vec4 localViewPos = localView * vec4 ( v_Vertex, 1.0 );\n"
	"  gl_Position = proj * localViewPos;\n"
	"  f_TexCoord = v_TexCoord;\n"
	"}\n";

static const char *FS_MASK =
	"#version 330 core\n"
	"uniform sampler2D maskMap;\n"
	"in vec2 f_TexCoord;\n"
	"out vec4 o_Color;\n"
	"void main () { o_Color = texture ( maskMap, f_TexCoord ); }\n";

static const char *FS_MATH =
	"#version 330 core\n"
	"out vec4 o_Color;\n"
	"void main () {\n"
	"  vec3 a = vec3 ( 1.0, 0.0, 0.0 );\n"
	"  vec3 b = vec3 ( 0.0, 1.0, 0.0 );\n"
	"  vec3 c = cross ( a, b );\n"        /* -> (0,0,1) */
	"  float d = dot ( a, b );\n"          /* -> 0 */
	"  vec3 m = mix ( a, b, 0.25 );\n"     /* -> (0.75,0.25,0) */
	"  float cl = clamp ( 5.0, 0.0, 2.0 );\n" /* -> 2 */
	"  o_Color = vec4 ( c.z, d, m.x, cl );\n"  /* -> (1,0,0.75,2) */
	"}\n";

/* User functions with out / inout params (Haydee's hdGetCSMCoord uses out). */
static const char *FS_OUTP =
	"#version 330 core\n"
	"out vec4 o_Color;\n"
	"void addone ( in float x, out float y ) { y = x + 1.0; }\n"
	"void swap2 ( inout vec2 v ) { v = v.yx; }\n"
	"void main () {\n"
	"  float r;\n"
	"  addone ( 4.0, r );\n"                 /* r -> 5 */
	"  vec2 p = vec2 ( 2.0, 7.0 );\n"
	"  swap2 ( p );\n"                       /* p -> (7,2) */
	"  o_Color = vec4 ( r, p.x, p.y, 1.0 );\n" /* -> (5,7,2,1) */
	"}\n";

/* Loop counters (postfix ++), const-global init, and same-body overloads -
 * the three things the real 15KB deferred-lighting shader (fs id=28) needs. */
static const char *FS_LOOP =
	"#version 330 core\n"
	"out vec4 o_Color;\n"
	"const float K = 2.0;\n"
	"float sq ( float x ) { return x * x; }\n"
	"vec2 sq ( vec2 x ) { return x * x; }\n"
	"void main () {\n"
	"  float s = 0.0;\n"
	"  for ( int i = 0; i < 4; i ++ ) s += K;\n"   /* s -> 8 */
	"  vec2 v = sq ( vec2 ( 2.0, 3.0 ) );\n"        /* -> (4,9) */
	"  o_Color = vec4 ( s, sq ( 3.0 ), v.x, v.y );\n" /* -> (8,9,4,9) */
	"}\n";

/* Arrays: const float[]/vec2[] initializers, dynamic index, element swizzle,
 * and a mat4[] uniform indexed then multiplied - the CSM-shadow machinery. */
static const char *FS_ARR =
	"#version 330 core\n"
	"out vec4 o_Color;\n"
	"float tbl[4] = float[] ( 1.0, 2.0, 3.0, 4.0 );\n"
	"vec2 pts[3] = vec2[] ( vec2 ( 0.1, 0.2 ), vec2 ( 0.3, 0.4 ), vec2 ( 0.5, 0.6 ) );\n"
	"uniform mat4 mats[2];\n"
	"void main () {\n"
	"  int k = 3;\n"
	"  vec4 mv = mats[1] * vec4 ( 1.0, 2.0, 3.0, 1.0 );\n"
	"  o_Color = vec4 ( tbl[0] + tbl[k], pts[2].y, mv.x, mv.z );\n" /* (5, 0.6, 11, 33) */
	"}\n";

/* Haydee's 201_v.glsl verbatim: GPU skinning captured by transform feedback. */
static const char *VS_SKIN =
	"#version 330 core\n"
	"in vec3 xyz;\n"
	"in vec3 uv;\n"
	"in vec3 normal;\n"
	"in vec3 tangent;\n"
	"in vec3 binormal;\n"
	"in vec4 weight;\n"
	"in vec4 index;\n"
	"out vec3 xyzOut;\n"
	"out vec3 uvOut;\n"
	"out vec3 normalOut;\n"
	"out vec3 tangentOut;\n"
	"out vec3 binormalOut;\n"
	"uniform mat4x3 joints[ 96 ];\n"
	"void main ()\n"
	"{\n"
	"	mat4x3 boneMatrix = joints[ int(index.x) ] * weight.x;\n"
	"	boneMatrix += joints[ int(index.y) ] * weight.y;\n"
	"	boneMatrix += joints[ int(index.z) ] * weight.z;\n"
	"	boneMatrix += joints[ int(index.w) ] * weight.w;\n"
	"	xyzOut = ( boneMatrix * vec4 ( xyz, 1.0 ) ).xyz;\n"
	"	uvOut = uv;\n"
	"	normalOut = mat3 ( boneMatrix ) * normal;\n"
	"	tangentOut = mat3 ( boneMatrix ) * tangent;\n"
	"	binormalOut = mat3 ( boneMatrix ) * binormal;\n"
	"}\n";

static const char *VS_LOC =
	"#version 330 core\n"
	"uniform mat4 m;\n"
	"layout(location = 3) in vec4 a_Pos;\n"
	"layout ( location = 5 ) in vec2 a_Uv;\n"
	"void main () { gl_Position = a_Pos * m; }\n";

int main(void)
{
	char err[256];
	float out[16];

	printf("== VS_UI (2D, mat3) ==\n");
	{
		GlslProg *p = glsl_compile(VS_UI, 'v', err, sizeof err);
		if (!p) { printf("  compile FAIL: %s\n", err); g_fail++; }
		else {
			float m3[9]; float vv[2] = { 0.5f, 0.25f };
			identity(m3, 3);
			glsl_set(p, "matrix", m3, 9);
			glsl_set(p, "v_Vertex", vv, 2);
			glsl_run(p, stub_sample, 0);
			{ float want[4] = { 0.5f, 0.25f, 1.0f, 1.0f };
			  int n = glsl_get(p, "gl_Position", out, 16);
			  (void)n; check("gl_Position (identity)", out, want, 4); }
			/* a scaling matrix: mat3 with diag (2,3,1) */
			m3[0] = 2; m3[4] = 3; m3[8] = 1;
			glsl_set(p, "matrix", m3, 9);
			glsl_run(p, stub_sample, 0);
			{ float want[4] = { 1.0f, 0.75f, 1.0f, 1.0f };
			  glsl_get(p, "gl_Position", out, 16);
			  check("gl_Position (scale 2,3)", out, want, 4); }
			glsl_free(p);
		}
	}

	printf("== VS_MODEL (3D, mat4*mat4*vec) ==\n");
	{
		GlslProg *p = glsl_compile(VS_MODEL, 'v', err, sizeof err);
		if (!p) { printf("  compile FAIL: %s\n", err); g_fail++; }
		else {
			float lv[16], pj[16]; float vv[3] = { 1.0f, 2.0f, 3.0f };
			identity(lv, 4); identity(pj, 4);
			glsl_set(p, "localView", lv, 16);
			glsl_set(p, "proj", pj, 16);
			glsl_set(p, "v_Vertex", vv, 3);
			glsl_run(p, stub_sample, 0);
			{ float want[4] = { 1.0f, 2.0f, 3.0f, 1.0f };
			  glsl_get(p, "gl_Position", out, 16);
			  check("gl_Position (identity MVP)", out, want, 4); }
			/* translate by (10,20,30) in localView (column-major: col3) */
			lv[12] = 10; lv[13] = 20; lv[14] = 30;
			glsl_set(p, "localView", lv, 16);
			glsl_run(p, stub_sample, 0);
			{ float want[4] = { 11.0f, 22.0f, 33.0f, 1.0f };
			  glsl_get(p, "gl_Position", out, 16);
			  check("gl_Position (translate MVP)", out, want, 4); }
			glsl_free(p);
		}
	}

	printf("== FS_MASK (texture sample) ==\n");
	{
		GlslProg *p = glsl_compile(FS_MASK, 'f', err, sizeof err);
		if (!p) { printf("  compile FAIL: %s\n", err); g_fail++; }
		else {
			float uv[2] = { 0.3f, 0.7f };
			glsl_set(p, "f_TexCoord", uv, 2);
			glsl_set_sampler(p, "maskMap", 0);
			glsl_run(p, stub_sample, 0);
			{ float want[4] = { 0.3f, 0.7f, 0.5f, 1.0f };
			  glsl_get(p, "o_Color", out, 16);
			  check("o_Color = texture(uv)", out, want, 4); }
			glsl_free(p);
		}
	}

	printf("== FS_MATH (built-ins) ==\n");
	{
		GlslProg *p = glsl_compile(FS_MATH, 'f', err, sizeof err);
		if (!p) { printf("  compile FAIL: %s\n", err); g_fail++; }
		else {
			glsl_run(p, stub_sample, 0);
			{ float want[4] = { 1.0f, 0.0f, 0.75f, 2.0f };
			  glsl_get(p, "o_Color", out, 16);
			  check("cross/dot/mix/clamp", out, want, 4); }
			glsl_free(p);
		}
	}

	printf("== FS_OUTP (out/inout param write-back) ==\n");
	{
		GlslProg *p = glsl_compile(FS_OUTP, 'f', err, sizeof err);
		if (!p) { printf("  compile FAIL: %s\n", err); g_fail++; }
		else {
			glsl_run(p, stub_sample, 0);
			{ float want[4] = { 5.0f, 7.0f, 2.0f, 1.0f };
			  glsl_get(p, "o_Color", out, 16);
			  check("addone(out)/swap2(inout)", out, want, 4); }
			glsl_free(p);
		}
	}

	printf("== FS_LOOP (postfix++, const-init, overload) ==\n");
	{
		GlslProg *p = glsl_compile(FS_LOOP, 'f', err, sizeof err);
		if (!p) { printf("  compile FAIL: %s\n", err); g_fail++; }
		else {
			glsl_run(p, stub_sample, 0);
			{ float want[4] = { 8.0f, 9.0f, 4.0f, 9.0f };
			  glsl_get(p, "o_Color", out, 16);
			  check("for-sum / sq() / const K", out, want, 4); }
			glsl_free(p);
		}
	}

	printf("== FS_ARR (const arrays, index, mat4[] uniform) ==\n");
	{
		GlslProg *p = glsl_compile(FS_ARR, 'f', err, sizeof err);
		if (!p) { printf("  compile FAIL: %s\n", err); g_fail++; }
		else {
			float mats[32]; int b, e2;
			for (b = 0; b < 2; b++) for (e2 = 0; e2 < 16; e2++) mats[b*16+e2] = (e2%5==0)?1.0f:0.0f;
			mats[16+12] = 10; mats[16+13] = 20; mats[16+14] = 30; /* mats[1] translate */
			glsl_set(p, "mats", mats, 32);
			glsl_run(p, stub_sample, 0);
			{ float want[4] = { 5.0f, 0.6f, 11.0f, 33.0f };
			  glsl_get(p, "o_Color", out, 16);
			  check("tbl[]/pts[]/mats[1]*v", out, want, 4); }
			glsl_free(p);
		}
	}

	printf("== VS_SKIN (Haydee transform-feedback skinning, mat4x3 joints[]) ==\n");
	{
		GlslProg *p = glsl_compile(VS_SKIN, 'v', err, sizeof err);
		if (!p) { printf("  compile FAIL: %s\n", err); g_fail++; }
		else {
			/* joint 2: translate (10,20,30); joint 5: scale x2. Column-major 4 cols x 3 rows. */
			static float joints[96 * 12];
			float xyz[3] = { 1, 2, 3 }, nrm[3] = { 0, 1, 0 };
			float w[4] = { 0.5f, 0.5f, 0, 0 }, ix[4] = { 2, 5, 0, 0 };
			GlslVar vars[32];
			int j, nv, locs_ok = 1;
			for (j = 0; j < 96; j++) {
				float *m = joints + j * 12;
				m[0] = m[4] = m[8] = 1;
			}
			joints[2 * 12 + 9] = 10; joints[2 * 12 + 10] = 20; joints[2 * 12 + 11] = 30;
			joints[5 * 12 + 0] = 2; joints[5 * 12 + 4] = 2; joints[5 * 12 + 8] = 2;
			glsl_set(p, "joints", joints, 96 * 12);
			glsl_set(p, "xyz", xyz, 3);
			glsl_set(p, "normal", nrm, 3);
			glsl_set(p, "weight", w, 4);
			glsl_set(p, "index", ix, 4);
			glsl_run(p, stub_sample, 0);
			/* 0.5*(p + t) + 0.5*(2p) = 1.5p + 0.5t */
			{ float want[3] = { 6.5f, 13.0f, 19.5f };
			  glsl_get(p, "xyzOut", out, 16);
			  check("skinned xyzOut", out, want, 3); }
			{ float want[3] = { 0, 1.5f, 0 };
			  glsl_get(p, "normalOut", out, 16);
			  check("mat3(mat4x3) * normal", out, want, 3); }
			nv = glsl_vars(p, vars, 32);
			for (j = 0; j < nv; j++) {
				if (!strcmp(vars[j].name, "xyz") && vars[j].qual != GLSL_Q_IN) locs_ok = 0;
				if (!strcmp(vars[j].name, "xyzOut") && vars[j].qual != GLSL_Q_OUT) locs_ok = 0;
				if (!strcmp(vars[j].name, "joints") &&
				    (vars[j].qual != GLSL_Q_UNIFORM || vars[j].arr_len != 96 ||
				     vars[j].rows != 3 || vars[j].cols != 4)) locs_ok = 0;
			}
			{ float got = (float)locs_ok, want = 1;
			  check("qualifiers / mat4x3 shape", &got, &want, 1); }
			glsl_free(p);
		}
	}

	printf("== VS_LOC (layout locations, vec*mat) ==\n");
	{
		GlslProg *p = glsl_compile(VS_LOC, 'v', err, sizeof err);
		if (!p) { printf("  compile FAIL: %s\n", err); g_fail++; }
		else {
			GlslVar vars[16];
			int j, nv = glsl_vars(p, vars, 16);
			float got[2] = { -1, -1 }, want2[2] = { 3, 5 };
			float m[16] = { 1, 2, 0, 0, 3, 4, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
			float v[4] = { 1, 1, 0, 0 };
			for (j = 0; j < nv; j++) {
				if (!strcmp(vars[j].name, "a_Pos")) got[0] = (float)vars[j].location;
				if (!strcmp(vars[j].name, "a_Uv")) got[1] = (float)vars[j].location;
			}
			check("layout(location)", got, want2, 2);
			glsl_set(p, "m", m, 16);
			glsl_set(p, "a_Pos", v, 4);
			glsl_run(p, stub_sample, 0);
			/* row vector * column-major m: (1*1+1*2, 1*3+1*4) */
			{ float want[2] = { 3, 7 };
			  glsl_get(p, "gl_Position", out, 16);
			  check("vec4 * mat4", out, want, 2); }
			glsl_free(p);
		}
	}

	printf("\n%s (%d failure(s))\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
	return g_fail ? 1 : 0;
}
