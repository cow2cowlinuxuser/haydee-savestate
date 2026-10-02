/* glsl.c - software GLSL interpreter. See glsl.h.
 *
 * Tree-walking: lex -> parse to an AST -> evaluate. Values are GlslVal (scalar,
 * vecN, or matN, column-major). Scope is a flat symbol table (globals for
 * uniforms/in/out/const, plus a locals stack for function bodies). Enough of GLSL
 * 1.40/3.30 to run real game shaders; unsupported constructs report an error
 * rather than miscompute. Built and validated in glsl_harness before it touches
 * the renderer. */
#include "glsl.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ---------------------------------------------------------------- lexer ---- */
enum { T_EOF, T_ID, T_NUM, T_PUNC };
typedef struct { int kind; const char *s; int len; float num; } Tok;

typedef struct {
	const char *src;
	Tok *t;
	int n, cap;
} Lex;

static void lx_push(Lex *L, int kind, const char *s, int len, float num)
{
	if (L->n >= L->cap) {
		L->cap = L->cap ? L->cap * 2 : 256;
		L->t = (Tok *)realloc(L->t, (size_t)L->cap * sizeof(Tok));
	}
	L->t[L->n].kind = kind;
	L->t[L->n].s = s;
	L->t[L->n].len = len;
	L->t[L->n].num = num;
	L->n++;
}

static int is_id0(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_id(int c) { return is_id0(c) || (c >= '0' && c <= '9'); }

static void lex(Lex *L, const char *src)
{
	const char *p = src;
	L->src = src;
	while (*p) {
		if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; continue; }
		if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
		if (p[0] == '/' && p[1] == '*') {
			p += 2;
			while (*p && !(p[0] == '*' && p[1] == '/')) p++;
			if (*p) p += 2;
			continue;
		}
		if (p[0] == '#') { while (*p && *p != '\n') p++; continue; } /* #version etc. */
		if (is_id0(*p)) {
			const char *s = p;
			while (is_id(*p)) p++;
			lx_push(L, T_ID, s, (int)(p - s), 0);
			continue;
		}
		if ((*p >= '0' && *p <= '9') || (*p == '.' && p[1] >= '0' && p[1] <= '9')) {
			char *end;
			float v = (float)strtod(p, &end);
			lx_push(L, T_NUM, p, (int)(end - p), v);
			p = end;
			if (*p == 'f' || *p == 'F') p++; /* 1.0f */
			continue;
		}
		{
			/* Multi-char operators first. */
			static const char *ops[] = { "==", "!=", "<=", ">=", "&&", "||",
						     "+=", "-=", "*=", "/=", "++", "--", 0 };
			int i, m = 0;
			for (i = 0; ops[i]; i++)
				if (p[0] == ops[i][0] && p[1] == ops[i][1]) { m = 2; break; }
			if (!m) m = 1;
			lx_push(L, T_PUNC, p, m, 0);
			p += m;
		}
	}
	lx_push(L, T_EOF, p, 0, 0);
}

/* --------------------------------------------------------------- ast ------- */
enum {
	N_NUM, N_VAR, N_BIN, N_UN, N_ASSIGN, N_SWIZ, N_INDEX, N_CALL, N_COND,
	N_DECL, N_IF, N_FOR, N_RET, N_BLOCK, N_EXPR, N_DISCARD, N_ARRAY
};
typedef struct Node Node;
struct Node {
	int kind;
	float num;
	char name[64];   /* var/call/decl/op text, or swizzle chars */
	int op;          /* punctuation op as a small code */
	Node *a, *b, *c, *d;
	Node **list;     /* call args / block stmts */
	int nlist;
	int vsize;       /* declared type component count (for constructors/decls) */
	int vcols;       /* matrix cols (0 = not a matrix type) */
};

typedef struct Func {
	char name[64];
	char pname[8][64];
	unsigned char pout[8]; /* 1 if param is out/inout: mutated value flows back */
	int nparam;
	Node *body;
} Func;

typedef struct Global {
	char name[64];
	GlslVal val;
	int is_sampler;
	int is_shadow;    /* sampler*Shadow: texture() returns a scalar compare, not rgba */
	int sampler_unit;
	int size;   /* declared components */
	struct Node *init;  /* initializer expr for a const/global (evaluated at compile end) */
	/* array globals (uniform arrays, const tables): arr holds arr_len elements,
	 * each arr_rows x arr_cols floats. NULL when this global is not an array. */
	int is_array;
	int arr_len;
	unsigned char arr_rows, arr_cols;
	float *arr;
	int qual;       /* GLSL_Q_* storage qualifier */
	int location;   /* layout(location = N), -1 if none */
} Global;

struct GlslProg {
	char *src_copy;
	Lex lex;
	Node **pool;
	int npool, cpool;
	Func funcs[64];
	int nfunc;
	Global glob[128];
	int nglob;
	int stage;
	char *err;
	int errcap;
	/* runtime */
	GlslSampleFn sample;
	void *sctx;
	int failed;
	/* Distinct per compile, so a clone can tell its source was freed and a
	 * new program compiled at the same address. */
	unsigned serial;
	/* Shares the source's tree, functions and array storage; owns only the
	 * struct, so its glob[].val can run a vertex while the source runs another. */
	int is_clone;
};

static int glsl_debug(void)
{
	static int v = -1;
	if (v < 0)
		v = getenv("GLSL_DEBUG") ? 1 : 0;
	return v;
}

/* parser state */
typedef struct {
	GlslProg *p;
	int i;
} Par;

static Node *node(GlslProg *p, int kind)
{
	Node *n;
	if (p->npool >= p->cpool) {
		p->cpool = p->cpool ? p->cpool * 2 : 256;
		p->pool = (Node **)realloc(p->pool, (size_t)p->cpool * sizeof(Node *));
	}
	n = (Node *)calloc(1, sizeof(Node));
	n->kind = kind;
	p->pool[p->npool++] = n;
	return n;
}

static Tok *pk(Par *ps) { return &ps->p->lex.t[ps->i]; }
static Tok *nx(Par *ps) { return &ps->p->lex.t[ps->i++]; }
static int is_p(Par *ps, const char *s)
{
	Tok *t = pk(ps);
	int n = (int)strlen(s);
	return t->kind == T_PUNC && t->len == n && strncmp(t->s, s, (size_t)n) == 0;
}
static int is_kw(Tok *t, const char *s)
{
	int n = (int)strlen(s);
	return t->kind == T_ID && t->len == n && strncmp(t->s, s, (size_t)n) == 0;
}
static int eat_p(Par *ps, const char *s) { if (is_p(ps, s)) { ps->i++; return 1; } return 0; }

static void tok_name(Tok *t, char *out, int cap)
{
	int n = t->len < cap - 1 ? t->len : cap - 1;
	memcpy(out, t->s, (size_t)n);
	out[n] = 0;
}

/* type keyword -> component count + matrix cols; 0 if not a type */
static int type_of(Tok *t, int *cols)
{
	*cols = 0;
	if (is_kw(t, "float") || is_kw(t, "int") || is_kw(t, "bool") || is_kw(t, "uint")) return 1;
	if (is_kw(t, "vec2") || is_kw(t, "ivec2") || is_kw(t, "bvec2")) return 2;
	if (is_kw(t, "vec3") || is_kw(t, "ivec3") || is_kw(t, "bvec3")) return 3;
	if (is_kw(t, "vec4") || is_kw(t, "ivec4") || is_kw(t, "bvec4")) return 4;
	if (is_kw(t, "mat2")) { *cols = 2; return 2; }
	if (is_kw(t, "mat3")) { *cols = 3; return 3; }
	if (is_kw(t, "mat4")) { *cols = 4; return 4; }
	/* matCxR: C columns of R rows */
	if (t->kind == T_ID && t->len == 6 && strncmp(t->s, "mat", 3) == 0 && t->s[4] == 'x' &&
	    t->s[3] >= '2' && t->s[3] <= '4' && t->s[5] >= '2' && t->s[5] <= '4') {
		*cols = t->s[3] - '0';
		return t->s[5] - '0';
	}
	if (is_kw(t, "void")) return -1;
	return 0;
}
static int is_sampler_kw(Tok *t)
{
	return is_kw(t, "sampler2D") || is_kw(t, "sampler2DArray") ||
	       is_kw(t, "sampler2DShadow") || is_kw(t, "sampler2DArrayShadow") ||
	       is_kw(t, "samplerCube") || is_kw(t, "samplerCubeShadow") || is_kw(t, "sampler3D");
}

/* forward */
static Node *expr(Par *ps);
static Node *stmt(Par *ps);

static Node *primary(Par *ps)
{
	GlslProg *p = ps->p;
	Tok *t = pk(ps);
	if (t->kind == T_NUM) { Node *n = node(p, N_NUM); n->num = nx(ps)->num; return n; }
	if (eat_p(ps, "(")) { Node *n = expr(ps); eat_p(ps, ")"); return n; }
	if (t->kind == T_ID) {
		int cols;
		int ty = type_of(t, &cols);
		char nm[64];
		tok_name(t, nm, sizeof(nm));
		ps->i++;
		if (is_p(ps, "(")) {
			/* call or constructor */
			Node *n = node(p, N_CALL);
			strncpy(n->name, nm, 63);
			if (ty > 0) { n->vsize = ty; n->vcols = cols; } /* constructor */
			eat_p(ps, "(");
			while (!is_p(ps, ")") && pk(ps)->kind != T_EOF) {
				Node *a = expr(ps);
				n->list = (Node **)realloc(n->list, (size_t)(n->nlist + 1) * sizeof(Node *));
				n->list[n->nlist++] = a;
				if (!eat_p(ps, ",")) break;
			}
			eat_p(ps, ")");
			return n;
		}
		{ Node *n = node(p, N_VAR); strncpy(n->name, nm, 63); return n; }
	}
	/* unexpected */
	{ Node *n = node(p, N_NUM); n->num = 0; ps->i++; return n; }
}

static Node *postfix(Par *ps)
{
	Node *n = primary(ps);
	for (;;) {
		if (eat_p(ps, ".")) {
			Tok *t = nx(ps);
			Node *s = node(ps->p, N_SWIZ);
			tok_name(t, s->name, sizeof(s->name));
			s->a = n;
			n = s;
		} else if (eat_p(ps, "[")) {
			Node *ix = node(ps->p, N_INDEX);
			ix->a = n;
			ix->b = expr(ps);
			eat_p(ps, "]");
			n = ix;
		} else if (is_p(ps, "++") || is_p(ps, "--")) {
			/* postfix x++ / x-- -> compound assign x += 1 / x -= 1.
			 * (returns the new value, not the old; irrelevant for the loop
			 * increments and statement-level uses that real shaders make.) */
			Node *as = node(ps->p, N_ASSIGN);
			Node *one = node(ps->p, N_NUM);
			as->name[0] = pk(ps)->s[0]; as->name[1] = 0;
			as->a = n;
			one->num = 1;
			as->b = one;
			ps->i++; /* consume ++ / -- */
			n = as;
		} else break;
	}
	return n;
}

static Node *unary(Par *ps)
{
	if (is_p(ps, "++") || is_p(ps, "--")) {
		/* prefix ++x / --x -> x += 1 / x -= 1 */
		Node *as = node(ps->p, N_ASSIGN);
		Node *one = node(ps->p, N_NUM);
		as->name[0] = pk(ps)->s[0]; as->name[1] = 0;
		ps->i++;
		as->a = unary(ps);
		one->num = 1;
		as->b = one;
		return as;
	}
	if (is_p(ps, "-") || is_p(ps, "!") || is_p(ps, "+")) {
		Node *n = node(ps->p, N_UN);
		n->name[0] = pk(ps)->s[0];
		ps->i++;
		n->a = unary(ps);
		return n;
	}
	return postfix(ps);
}

/* precedence-climbing binary */
static int prec_of(Par *ps, char *op)
{
	Tok *t = pk(ps);
	if (t->kind != T_PUNC) return -1;
	op[0] = t->s[0]; op[1] = t->len > 1 ? t->s[1] : 0; op[2] = 0;
	if (t->len == 1) {
		switch (t->s[0]) {
		case '*': case '/': case '%': return 7;
		case '+': case '-': return 6;
		case '<': case '>': return 5;
		}
		return -1;
	}
	if (t->len == 2) {
		if ((op[0] == '=' && op[1] == '=') || (op[0] == '!' && op[1] == '=')) return 4;
		if ((op[0] == '<' && op[1] == '=') || (op[0] == '>' && op[1] == '=')) return 5;
		if (op[0] == '&' && op[1] == '&') return 3;
		if (op[0] == '|' && op[1] == '|') return 2;
	}
	return -1;
}

static Node *binrhs(Par *ps, int minp, Node *lhs)
{
	for (;;) {
		char op[3];
		int p = prec_of(ps, op);
		if (p < minp) return lhs;
		ps->i++;
		Node *rhs = unary(ps);
		for (;;) {
			char op2[3];
			int p2 = prec_of(ps, op2);
			if (p2 <= p) break;
			rhs = binrhs(ps, p + 1, rhs);
		}
		{
			Node *n = node(ps->p, N_BIN);
			n->name[0] = op[0]; n->name[1] = op[1]; n->name[2] = 0;
			n->a = lhs; n->b = rhs;
			lhs = n;
		}
	}
}

static Node *ternary(Par *ps)
{
	Node *c = binrhs(ps, 0, unary(ps));
	if (eat_p(ps, "?")) {
		Node *n = node(ps->p, N_COND);
		n->a = c;
		n->b = expr(ps);
		eat_p(ps, ":");
		n->c = expr(ps);
		return n;
	}
	return c;
}

static Node *expr(Par *ps)
{
	Node *lhs = ternary(ps);
	if (is_p(ps, "=") || is_p(ps, "+=") || is_p(ps, "-=") || is_p(ps, "*=") || is_p(ps, "/=")) {
		Node *n = node(ps->p, N_ASSIGN);
		tok_name(pk(ps), n->name, sizeof(n->name));
		ps->i++;
		n->a = lhs;
		n->b = expr(ps);
		return n;
	}
	return lhs;
}

/* Parse an array constructor: TYPE '[' [size] ']' '(' e0, e1, ... ')' -> N_ARRAY.
 * vsize/vcols carry the element shape; list holds the element expressions. */
static Node *array_ctor(Par *ps)
{
	Node *n = node(ps->p, N_ARRAY);
	int cols = 0;
	n->vsize = type_of(pk(ps), &cols);
	n->vcols = cols;
	ps->i++; /* element type */
	eat_p(ps, "[");
	while (!is_p(ps, "]") && pk(ps)->kind != T_EOF) ps->i++;
	eat_p(ps, "]");
	eat_p(ps, "(");
	while (!is_p(ps, ")") && pk(ps)->kind != T_EOF) {
		Node *el = expr(ps);
		n->list = (Node **)realloc(n->list, (size_t)(n->nlist + 1) * sizeof(Node *));
		n->list[n->nlist++] = el;
		if (!eat_p(ps, ",")) break;
	}
	eat_p(ps, ")");
	return n;
}

static Node *block(Par *ps)
{
	Node *n = node(ps->p, N_BLOCK);
	eat_p(ps, "{");
	while (!is_p(ps, "}") && pk(ps)->kind != T_EOF) {
		Node *s = stmt(ps);
		if (s) {
			n->list = (Node **)realloc(n->list, (size_t)(n->nlist + 1) * sizeof(Node *));
			n->list[n->nlist++] = s;
		}
	}
	eat_p(ps, "}");
	return n;
}

static Node *stmt(Par *ps)
{
	GlslProg *p = ps->p;
	Tok *t = pk(ps);
	int cols, ty;
	if (is_p(ps, "{")) return block(ps);
	if (is_kw(t, "if")) {
		Node *n = node(p, N_IF);
		ps->i++; eat_p(ps, "(");
		n->a = expr(ps); eat_p(ps, ")");
		n->b = stmt(ps);
		if (is_kw(pk(ps), "else")) { ps->i++; n->c = stmt(ps); }
		return n;
	}
	if (is_kw(t, "for")) {
		Node *n = node(p, N_FOR);
		ps->i++; eat_p(ps, "(");
		n->a = stmt(ps);         /* init (decl or expr;) */
		n->b = expr(ps); eat_p(ps, ";");
		n->c = expr(ps); eat_p(ps, ")");
		n->d = stmt(ps);
		return n;
	}
	if (is_kw(t, "return")) {
		Node *n = node(p, N_RET);
		ps->i++;
		if (!is_p(ps, ";")) n->a = expr(ps);
		eat_p(ps, ";");
		return n;
	}
	if (is_kw(t, "discard")) { Node *n = node(p, N_DISCARD); ps->i++; eat_p(ps, ";"); return n; }
	if (is_kw(t, "const")) ps->i++;
	ty = type_of(t, &cols);
	if (ty != 0 && ty != -1) {
		/* local declaration: type name [= init] [, name2 ...]; */
		Node *first = 0, *last = 0;
		ps->i++;
		for (;;) {
			Node *d = node(p, N_DECL);
			d->vsize = ty; d->vcols = cols;
			tok_name(pk(ps), d->name, sizeof(d->name));
			ps->i++;
			if (eat_p(ps, "[")) { /* array decl - skip size, treat as scalar array unsupported */
				while (!is_p(ps, "]") && pk(ps)->kind != T_EOF) ps->i++;
				eat_p(ps, "]");
			}
			if (eat_p(ps, "=")) d->a = expr(ps);
			if (!first) first = last = d;
			else { last->b = d; last = d; } /* chain multiple decls via ->b */
			if (!eat_p(ps, ",")) break;
		}
		eat_p(ps, ";");
		return first;
	}
	/* expression statement */
	{
		Node *n = node(p, N_EXPR);
		n->a = expr(ps);
		eat_p(ps, ";");
		return n;
	}
}

/* top-level: qualifiers, uniforms, in/out, globals, functions */
static void toplevel(GlslProg *p)
{
	Par ps; ps.p = p; ps.i = 0;
	int qual = GLSL_Q_NONE, loc = -1;
	while (pk(&ps)->kind != T_EOF) {
		Tok *t = pk(&ps);
		int cols, ty;
		if (is_kw(t, "layout")) {
			ps.i++;
			if (eat_p(&ps, "(")) {
				int d = 1;
				while (d && pk(&ps)->kind != T_EOF) {
					if (is_kw(pk(&ps), "location") && ps.p->lex.t[ps.i + 1].kind == T_PUNC &&
					    ps.p->lex.t[ps.i + 1].s[0] == '=' && ps.p->lex.t[ps.i + 2].kind == T_NUM)
						loc = (int)ps.p->lex.t[ps.i + 2].num;
					if (is_p(&ps, "(")) d++; else if (is_p(&ps, ")")) d--;
					ps.i++;
				}
			}
			continue;
		}
		if (is_kw(t, "in")) { qual = GLSL_Q_IN; ps.i++; continue; }
		if (is_kw(t, "out")) { qual = GLSL_Q_OUT; ps.i++; continue; }
		if (is_kw(t, "uniform")) { qual = GLSL_Q_UNIFORM; ps.i++; continue; }
		if (is_kw(t, "const") || is_kw(t, "flat") || is_kw(t, "smooth") ||
		    is_kw(t, "noperspective") || is_kw(t, "centroid")) {
			ps.i++;
			continue;
		}
		if (is_p(&ps, ";")) { qual = GLSL_Q_NONE; loc = -1; ps.i++; continue; }
		if (is_sampler_kw(t)) {
			/* sampler uniform declaration: samplerX name; (or name[]) */
			int shadow = is_kw(t, "sampler2DShadow") || is_kw(t, "sampler2DArrayShadow") ||
				     is_kw(t, "samplerCubeShadow");
			ps.i++;
			if (p->nglob < 128) {
				Global *g = &p->glob[p->nglob++];
				tok_name(pk(&ps), g->name, sizeof(g->name));
				g->is_sampler = 1;
				g->is_shadow = shadow;
				g->sampler_unit = 0;
				g->qual = GLSL_Q_UNIFORM;
				g->location = loc;
			}
			qual = GLSL_Q_NONE;
			loc = -1;
			ps.i++;
			while (pk(&ps)->kind != T_EOF && !is_p(&ps, ";")) ps.i++;
			eat_p(&ps, ";");
			continue;
		}
		ty = type_of(t, &cols);
		if (ty != 0) {
			char nm[64];
			ps.i++; /* type */
			if (ty == -1) {
				/* void func or void var - expect function */
			}
			tok_name(pk(&ps), nm, sizeof(nm));
			ps.i++; /* name */
			if (is_p(&ps, "(")) {
				qual = GLSL_Q_NONE;
				loc = -1;
				/* function definition */
				Func *fn = &p->funcs[p->nfunc < 64 ? p->nfunc : 63];
				strncpy(fn->name, nm, 63);
				fn->nparam = 0;
				eat_p(&ps, "(");
				while (!is_p(&ps, ")") && pk(&ps)->kind != T_EOF) {
					int c2, pty, is_out = 0;
					/* in/out/inout qualifiers on params */
					while (is_kw(pk(&ps), "in") || is_kw(pk(&ps), "out") ||
					       is_kw(pk(&ps), "inout") || is_kw(pk(&ps), "const")) {
						if (is_kw(pk(&ps), "out") || is_kw(pk(&ps), "inout"))
							is_out = 1;
						ps.i++;
					}
					pty = type_of(pk(&ps), &c2);
					if (is_sampler_kw(pk(&ps))) pty = 1;
					if (pty == 0 && pk(&ps)->kind == T_ID) pty = 1;
					ps.i++; /* param type */
					if (pk(&ps)->kind == T_ID && fn->nparam < 8) {
						tok_name(pk(&ps), fn->pname[fn->nparam], 64);
						fn->pout[fn->nparam] = (unsigned char)is_out;
						fn->nparam++;
						ps.i++;
					}
					if (!eat_p(&ps, ",")) break;
				}
				eat_p(&ps, ")");
				if (is_p(&ps, "{")) {
					fn->body = block(&ps);
					if (p->nfunc < 64) p->nfunc++;
				} else {
					eat_p(&ps, ";"); /* prototype */
				}
				continue;
			}
			/* global variable (possibly with initializer) - store as global */
			{
				Global *g = (p->nglob < 128) ? &p->glob[p->nglob++] : 0;
				if (g) {
					strncpy(g->name, nm, 63);
					g->size = ty;
					g->qual = qual;
					g->location = loc;
					memset(&g->val, 0, sizeof(g->val));
					g->val.rows = (unsigned char)ty;
					g->val.cols = (unsigned char)(cols ? cols : 1);
				}
				qual = GLSL_Q_NONE;
				loc = -1;
				if (eat_p(&ps, "[")) {
					/* array global: capture length + element shape, allocate storage */
					int len = 0;
					if (pk(&ps)->kind == T_NUM) len = (int)pk(&ps)->num;
					while (!is_p(&ps, "]") && pk(&ps)->kind != T_EOF) ps.i++;
					eat_p(&ps, "]");
					if (g) {
						int ef = cols > 1 ? ty * cols : ty;
						g->is_array = 1;
						g->arr_len = len;
						g->arr_rows = (unsigned char)ty;
						g->arr_cols = (unsigned char)(cols ? cols : 1);
						if (len > 0 && ef > 0) g->arr = (float *)calloc((size_t)len * ef, sizeof(float));
					}
				}
				if (eat_p(&ps, "=")) {
					int c3;
					if (g && g->is_array && type_of(pk(&ps), &c3) != 0) {
						g->init = array_ctor(&ps);   /* T[](...) element list */
					} else {
						Node *ini = expr(&ps);        /* scalar/vector const initializer */
						if (g && !g->is_array) g->init = ini;
					}
				}
			}
			while (pk(&ps)->kind != T_EOF && !is_p(&ps, ";")) ps.i++;
			eat_p(&ps, ";");
			continue;
		}
		/* unknown token - skip to next ; or } to stay in sync */
		ps.i++;
	}
}

/* ---------------------------------------------------------- environment ---- */
typedef struct { char name[64]; GlslVal val; } Sym;
typedef struct {
	GlslProg *p;
	Sym loc[256];
	int nloc;
	int scope[32];  /* stack of loc counts to pop on block exit */
	int nscope;
	GlslVal ret;
	int returned;
	int err;
	GlslSampleFn sample;
	void *sctx;
} Env;

static GlslVal vscalar(float x) { GlslVal v; memset(&v, 0, sizeof v); v.f[0] = x; v.rows = 1; v.cols = 1; return v; }

/* Symbols are cleared as they are declared, so only the header needs zeroing;
 * clearing the whole 34 KB Env per vertex dominated small shaders. */
static void env_init(Env *e)
{
	e->p = 0;
	e->nloc = 0;
	e->nscope = 0;
	memset(&e->ret, 0, sizeof e->ret);
	e->returned = 0;
	e->err = 0;
	e->sample = 0;
	e->sctx = 0;
}

static Global *find_global(GlslProg *p, const char *name)
{
	int i;
	for (i = 0; i < p->nglob; i++)
		if (strcmp(p->glob[i].name, name) == 0) return &p->glob[i];
	return 0;
}
static Sym *find_local(Env *e, const char *name)
{
	int i;
	for (i = e->nloc - 1; i >= 0; i--)
		if (strcmp(e->loc[i].name, name) == 0) return &e->loc[i];
	return 0;
}
static Sym *decl_local(Env *e, const char *name)
{
	Sym *s;
	if (e->nloc >= 256) { e->err = 1; return &e->loc[255]; }
	s = &e->loc[e->nloc++];
	strncpy(s->name, name, 63); s->name[63] = 0;
	memset(&s->val, 0, sizeof(s->val));
	return s;
}
static void push_scope(Env *e) { if (e->nscope < 32) e->scope[e->nscope++] = e->nloc; }
static void pop_scope(Env *e) { if (e->nscope > 0) e->nloc = e->scope[--e->nscope]; }

static int swiz_index(char c)
{
	switch (c) {
	case 'x': case 'r': case 's': return 0;
	case 'y': case 'g': case 't': return 1;
	case 'z': case 'b': case 'p': return 2;
	case 'w': case 'a': case 'q': return 3;
	}
	return -1;
}

static GlslVal eval(Env *e, Node *n);

/* Matrices are column-major, R rows x C cols: element (row, col) at f[col*R + row]. */
static GlslVal mat_vec(GlslVal m, GlslVal v)
{
	GlslVal r; int row, k; int R = m.rows, C = m.cols;
	memset(&r, 0, sizeof r);
	r.rows = (unsigned char)R; r.cols = 1;
	for (row = 0; row < R; row++) {
		float s = 0;
		for (k = 0; k < C; k++) s += m.f[k * R + row] * v.f[k];
		r.f[row] = s;
	}
	return r;
}
static GlslVal vec_mat(GlslVal v, GlslVal m)
{
	GlslVal r; int col, k; int R = m.rows, C = m.cols;
	memset(&r, 0, sizeof r);
	r.rows = (unsigned char)C; r.cols = 1;
	for (col = 0; col < C; col++) {
		float s = 0;
		for (k = 0; k < R; k++) s += v.f[k] * m.f[col * R + k];
		r.f[col] = s;
	}
	return r;
}
static GlslVal mat_mat(GlslVal a, GlslVal b)
{
	GlslVal r; int col, row, k; int R = a.rows, K = a.cols, C = b.cols;
	memset(&r, 0, sizeof r);
	r.rows = (unsigned char)R; r.cols = (unsigned char)C;
	for (col = 0; col < C; col++)
		for (row = 0; row < R; row++) {
			float s = 0;
			for (k = 0; k < K; k++) s += a.f[k * R + row] * b.f[col * b.rows + k];
			r.f[col * R + row] = s;
		}
	return r;
}

static int ncomp(GlslVal v) { return v.cols > 1 ? v.rows * v.cols : v.rows; }

static GlslVal binop(const char *op, GlslVal a, GlslVal b, Env *e)
{
	(void)e; /* reserved for runtime error reporting */
	/* matrix products */
	if (op[1] == 0 && op[0] == '*') {
		if (a.cols > 1 && b.cols > 1 && a.cols == b.rows) return mat_mat(a, b);
		if (a.cols > 1 && b.cols == 1 && b.rows == a.cols) return mat_vec(a, b);
		if (b.cols > 1 && a.cols == 1 && a.rows == b.rows) return vec_mat(a, b);
	}
	{
		/* component-wise with scalar broadcast; comparisons -> scalar bool */
		GlslVal r; int i, na = ncomp(a), nb = ncomp(b), N = na > nb ? na : nb;
		int as = (na == 1), bs = (nb == 1);
		memset(&r, 0, sizeof r);
		char o0 = op[0], o1 = op[1];
		if ((o0 == '=' && o1 == '=') || (o0 == '!' && o1 == '=') ||
		    (o0 == '<' ) || (o0 == '>') || (o0 == '&' && o1=='&') || (o0=='|'&&o1=='|')) {
			int eq = 1; float x = a.f[0], y = b.f[0];
			for (i = 0; i < N; i++) if (a.f[as?0:i] != b.f[bs?0:i]) eq = 0;
			float res = 0;
			if (o1 == '=') { if (o0=='=') res = eq; else if (o0=='!') res = !eq; else if (o0=='<') res = x<=y; else if (o0=='>') res = x>=y; }
			else if (o0 == '<') res = x < y;
			else if (o0 == '>') res = x > y;
			else if (o0 == '&') res = (x!=0) && (y!=0);
			else if (o0 == '|') res = (x!=0) || (y!=0);
			return vscalar(res);
		}
		r.rows = (unsigned char)(as ? b.rows : a.rows);
		r.cols = (unsigned char)(as ? b.cols : a.cols);
		for (i = 0; i < N; i++) {
			float x = a.f[as ? 0 : i], y = b.f[bs ? 0 : i];
			switch (o0) {
			case '+': r.f[i] = x + y; break;
			case '-': r.f[i] = x - y; break;
			case '*': r.f[i] = x * y; break;
			case '/': r.f[i] = y != 0 ? x / y : 0; break;
			case '%': r.f[i] = (float)fmod(x, y); break;
			default: r.f[i] = x; break;
			}
		}
		return r;
	}
}

/* pack constructor args into an N (xM) value */
static GlslVal construct(int size, int cols, GlslVal *args, int nargs)
{
	GlslVal r; int total = cols > 1 ? size * cols : size, i, w = 0;
	memset(&r, 0, sizeof r);
	r.rows = (unsigned char)size;
	r.cols = (unsigned char)(cols ? cols : 1);
	if (cols > 1 && nargs == 1 && ncomp(args[0]) == 1) {
		/* matN(scalar) -> diagonal */
		int k; for (k = 0; k < size && k < cols; k++) r.f[k * size + k] = args[0].f[0];
		return r;
	}
	if (cols > 1 && nargs == 1 && args[0].cols > 1) {
		/* matCxR(matC'xR') -> copy overlap, identity elsewhere */
		int c, rr, Mr = args[0].rows, Mc = args[0].cols;
		for (c = 0; c < cols; c++) for (rr = 0; rr < size; rr++)
			r.f[c*size+rr] = (c<Mc && rr<Mr) ? args[0].f[c*Mr+rr] : (c==rr?1.0f:0.0f);
		return r;
	}
	if (nargs == 1 && ncomp(args[0]) == 1 && total > 1) {
		/* vecN(scalar) -> broadcast */
		for (i = 0; i < total; i++) r.f[i] = args[0].f[0];
		return r;
	}
	for (i = 0; i < nargs && w < total; i++) {
		int c = ncomp(args[i]), k;
		for (k = 0; k < c && w < total; k++) r.f[w++] = args[i].f[k];
	}
	return r;
}

static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }

static GlslVal call_builtin(Env *e, const char *name, GlslVal *a, int n, int *handled);

static GlslVal call_user(Env *e, Func *fn, GlslVal *a, int n)
{
	Env sub; int i;
	GlslVal r = vscalar(0);
	env_init(&sub);
	sub.p = e->p;
	sub.sample = e->sample;
	sub.sctx = e->sctx;
	for (i = 0; i < fn->nparam && i < n; i++) {
		Sym *s = decl_local(&sub, fn->pname[i]);
		s->val = a[i];
	}
	eval(&sub, fn->body);
	if (sub.returned) r = sub.ret;
	/* Copy each param's final local value back into a[] so the caller can flow
	 * out/inout results into its argument lvalues (e.g. hdGetCSMCoord's out coord).
	 * Harmless for in-params: the caller only writes back the ones flagged out. */
	for (i = 0; i < fn->nparam && i < n; i++) {
		Sym *s = find_local(&sub, fn->pname[i]);
		if (s) a[i] = s->val;
	}
	if (sub.err) e->err = 1;
	return r;
}

/* assignment target: returns pointer to the storage float(s) + count via out */
static float *lvalue(Env *e, Node *n, int *count, int *stride_swiz, int swiz_idx[4])
{
	*stride_swiz = 0;
	if (n->kind == N_VAR) {
		Sym *s = find_local(e, n->name);
		if (s) { *count = ncomp(s->val); return s->val.f; }
		{ Global *g = find_global(e->p, n->name); if (g) { *count = ncomp(g->val); return g->val.f; } }
		/* implicit create as local scalar */
		{ Sym *ns = decl_local(e, n->name); ns->val = vscalar(0); *count = 1; return ns->val.f; }
	}
	if (n->kind == N_SWIZ) {
		int c2, ss; int idx[4];
		float *base = lvalue(e, n->a, &c2, &ss, idx);
		int i, k = (int)strlen(n->name);
		*stride_swiz = k;
		for (i = 0; i < k && i < 4; i++) swiz_idx[i] = swiz_index(n->name[i]);
		*count = k;
		return base;
	}
	if (n->kind == N_INDEX) {
		int c2, ss; int idx[4];
		float *base = lvalue(e, n->a, &c2, &ss, idx);
		GlslVal iv = eval(e, n->b);
		int i = (int)iv.f[0];
		GlslVal bv = eval(e, n->a);
		if (!base) return 0;
		if (bv.cols > 1) { /* matrix column */
			if (i < 0 || i >= bv.cols) i = 0;
			*count = bv.rows;
			return base + i * bv.rows;
		}
		if (i < 0 || i >= 16) i = 0;
		*count = 1;
		return base + i;
	}
	*count = 0;
	return 0;
}

/* Write a value into an lvalue node (used for out/inout param write-back). */
static void store_val(Env *e, Node *n, GlslVal v)
{
	int count, ss, idx[4], i, rc;
	float *dst;
	if (!n || (n->kind != N_VAR && n->kind != N_SWIZ && n->kind != N_INDEX))
		return; /* arg isn't an lvalue - nothing to write back */
	dst = lvalue(e, n, &count, &ss, idx);
	if (!dst) return;
	rc = ncomp(v);
	if (ss) for (i = 0; i < ss; i++) dst[idx[i]] = v.f[rc == 1 ? 0 : i];
	else for (i = 0; i < count; i++) dst[i] = v.f[rc == 1 ? 0 : i];
}

static GlslVal eval(Env *e, Node *n)
{
	if (!n || e->returned || e->err) return vscalar(0);
	switch (n->kind) {
	case N_NUM: return vscalar(n->num);
	case N_VAR: {
		Sym *s = find_local(e, n->name);
		if (s) return s->val;
		{ Global *g = find_global(e->p, n->name); if (g) return g->val; }
		if (strcmp(n->name, "true") == 0) return vscalar(1);
		if (strcmp(n->name, "false") == 0) return vscalar(0);
		return vscalar(0);
	}
	case N_UN: {
		GlslVal a = eval(e, n->a); int i, c = ncomp(a);
		if (n->name[0] == '-') for (i = 0; i < c; i++) a.f[i] = -a.f[i];
		else if (n->name[0] == '!') a.f[0] = (a.f[0] != 0) ? 0 : 1;
		return a;
	}
	case N_BIN: {
		GlslVal a = eval(e, n->a), b = eval(e, n->b);
		return binop(n->name, a, b, e);
	}
	case N_COND: {
		GlslVal c = eval(e, n->a);
		return c.f[0] != 0 ? eval(e, n->b) : eval(e, n->c);
	}
	case N_SWIZ: {
		GlslVal a = eval(e, n->a), r; int k = (int)strlen(n->name), i;
		memset(&r, 0, sizeof r);
		for (i = 0; i < k && i < 4; i++) { int si = swiz_index(n->name[i]); r.f[i] = si >= 0 ? a.f[si] : 0; }
		r.rows = (unsigned char)k; r.cols = 1;
		return r;
	}
	case N_INDEX: {
		/* array-global element: arr[i] returns the i-th element (scalar/vec/mat) */
		if (n->a->kind == N_VAR && !find_local(e, n->a->name)) {
			Global *g = find_global(e->p, n->a->name);
			if (g && g->is_array && g->arr) {
				int idx = (int)eval(e, n->b).f[0];
				int ef = g->arr_cols > 1 ? g->arr_rows * g->arr_cols : g->arr_rows, k;
				GlslVal r; memset(&r, 0, sizeof r);
				if (idx < 0) idx = 0;
				if (idx >= g->arr_len) idx = g->arr_len > 0 ? g->arr_len - 1 : 0;
				for (k = 0; k < ef && k < 16; k++) r.f[k] = g->arr[idx * ef + k];
				r.rows = g->arr_rows; r.cols = g->arr_cols;
				return r;
			}
		}
		{
		GlslVal a = eval(e, n->a); GlslVal iv = eval(e, n->b); int i = (int)iv.f[0];
		if (a.cols > 1) { /* matrix column -> vector */
			GlslVal r; int N = a.rows, k; memset(&r, 0, sizeof r);
			r.rows = (unsigned char)N; r.cols = 1;
			for (k = 0; k < N; k++) r.f[k] = a.f[i * N + k];
			return r;
		}
		return vscalar(a.f[i]);
		}
	}
	case N_CALL: {
		GlslVal args[16]; int i, na = n->nlist < 16 ? n->nlist : 16;
		for (i = 0; i < na; i++) args[i] = eval(e, n->list[i]);
		if (n->vsize) return construct(n->vsize, n->vcols, args, na); /* constructor */
		{
			int handled = 0;
			GlslVal r = call_builtin(e, n->name, args, na, &handled);
			if (handled) return r;
		}
		{
			int i2;
			for (i2 = 0; i2 < e->p->nfunc; i2++)
				if (strcmp(e->p->funcs[i2].name, n->name) == 0) {
					Func *fn = &e->p->funcs[i2];
					GlslVal r = call_user(e, fn, args, na);
					int k;
					for (k = 0; k < fn->nparam && k < na; k++)
						if (fn->pout[k]) store_val(e, n->list[k], args[k]);
					return r;
				}
		}
		return vscalar(0);
	}
	case N_ASSIGN: {
		GlslVal rhs = eval(e, n->b);
		int count, ss, idx[4]; float *dst = lvalue(e, n->a, &count, &ss, idx);
		if (!dst) return rhs;
		if (n->name[0] != '=') {
			/* compound += etc: dst op= rhs, componentwise */
			GlslVal cur; int i; memset(&cur, 0, sizeof cur); cur.rows = (unsigned char)count; cur.cols = 1;
			if (ss) for (i = 0; i < ss; i++) cur.f[i] = dst[idx[i]]; else for (i = 0; i < count; i++) cur.f[i] = dst[i];
			char op[3]; op[0] = n->name[0]; op[1] = 0; op[2] = 0;
			rhs = binop(op, cur, rhs, e);
		}
		{
			int i, rc = ncomp(rhs);
			if (ss) { for (i = 0; i < ss; i++) dst[idx[i]] = rhs.f[rc == 1 ? 0 : i]; }
			else { for (i = 0; i < count; i++) dst[i] = rhs.f[rc == 1 ? 0 : i]; }
		}
		return rhs;
	}
	case N_DECL: {
		Node *d = n;
		while (d) {
			Sym *s = decl_local(e, d->name);
			if (d->a) {
				GlslVal v = eval(e, d->a);
				/* shape to declared type */
				if (d->vcols > 1) { v.rows = (unsigned char)d->vsize; v.cols = (unsigned char)d->vcols; }
				else if (d->vsize > 1 && v.cols <= 1) { v.rows = (unsigned char)d->vsize; v.cols = 1; }
				s->val = v;
			} else { memset(&s->val, 0, sizeof s->val); s->val.rows = (unsigned char)d->vsize; s->val.cols = (unsigned char)(d->vcols?d->vcols:1); }
			d = d->b; /* chained decls */
		}
		return vscalar(0);
	}
	case N_IF: {
		GlslVal c = eval(e, n->a);
		if (c.f[0] != 0) eval(e, n->b); else if (n->c) eval(e, n->c);
		return vscalar(0);
	}
	case N_FOR: {
		int guard = 0;
		push_scope(e);
		eval(e, n->a);
		while (!e->returned && !e->err) {
			GlslVal c = eval(e, n->b);
			if (c.f[0] == 0) break;
			eval(e, n->d);
			eval(e, n->c);
			if (++guard > 100000) { e->err = 1; break; }
		}
		pop_scope(e);
		return vscalar(0);
	}
	case N_RET:
		if (n->a) e->ret = eval(e, n->a);
		e->returned = 1;
		return vscalar(0);
	case N_BLOCK: {
		int i;
		push_scope(e);
		for (i = 0; i < n->nlist && !e->returned && !e->err; i++) eval(e, n->list[i]);
		pop_scope(e);
		return vscalar(0);
	}
	case N_EXPR: return eval(e, n->a);
	case N_DISCARD: e->returned = 1; return vscalar(0);
	}
	return vscalar(0);
}

/* ------------------------------------------------------------- builtins ---- */
static GlslVal vmap1(GlslVal a, float (*fn)(float))
{
	int i, c = ncomp(a); for (i = 0; i < c; i++) a.f[i] = fn(a.f[i]); return a;
}
static float b_fract(float x) { return x - floorf(x); }
static float b_sign(float x) { return x > 0 ? 1.0f : x < 0 ? -1.0f : 0.0f; }
static float b_radians(float d) { return d * 3.14159265358979f / 180.0f; }

static GlslVal call_builtin(Env *e, const char *name, GlslVal *a, int n, int *handled)
{
	*handled = 1;
	if (!strcmp(name, "texture") || !strcmp(name, "texture2D") ||
	    !strcmp(name, "textureLod") || !strcmp(name, "texelFetch") ||
	    !strcmp(name, "textureProj")) {
		GlslVal r; float rgba[4] = { 0, 0, 0, 1 }; int unit = 0, i2, shadow = 0;
		/* a[0] is the sampler value: we stored its unit in val.f[0] */
		if (n >= 1) unit = (int)a[0].f[0];
		if (n >= 2 && e->sample) {
			float coord[4] = {0,0,0,0}; int c = ncomp(a[1]), i;
			for (i = 0; i < c && i < 4; i++) coord[i] = a[1].f[i];
			if (!strcmp(name, "textureProj") && c >= 2 && coord[c-1] != 0)
				{ int k; for (k = 0; k < c-1; k++) coord[k] /= coord[c-1]; }
			e->sample(e->sctx, unit, coord, c, rgba);
		}
		/* sampler*Shadow: texture() returns a single scalar comparison result */
		for (i2 = 0; i2 < e->p->nglob; i2++)
			if (e->p->glob[i2].is_sampler && e->p->glob[i2].is_shadow &&
			    e->p->glob[i2].sampler_unit == unit) { shadow = 1; break; }
		if (shadow) return vscalar(rgba[0]);
		memset(&r, 0, sizeof r);
		r.f[0] = rgba[0]; r.f[1] = rgba[1]; r.f[2] = rgba[2]; r.f[3] = rgba[3];
		r.rows = 4; r.cols = 1;
		return r;
	}
	if (!strcmp(name, "dot")) { int i, c = ncomp(a[0]); float s = 0; for (i=0;i<c;i++) s += a[0].f[i]*a[1].f[i]; return vscalar(s); }
	if (!strcmp(name, "cross")) { GlslVal r; memset(&r,0,sizeof r); r.rows=3; r.cols=1;
		r.f[0]=a[0].f[1]*a[1].f[2]-a[0].f[2]*a[1].f[1];
		r.f[1]=a[0].f[2]*a[1].f[0]-a[0].f[0]*a[1].f[2];
		r.f[2]=a[0].f[0]*a[1].f[1]-a[0].f[1]*a[1].f[0]; return r; }
	if (!strcmp(name, "length")) { int i,c=ncomp(a[0]); float s=0; for(i=0;i<c;i++) s+=a[0].f[i]*a[0].f[i]; return vscalar(sqrtf(s)); }
	if (!strcmp(name, "distance")) { int i,c=ncomp(a[0]); float s=0; for(i=0;i<c;i++){float d=a[0].f[i]-a[1].f[i]; s+=d*d;} return vscalar(sqrtf(s)); }
	if (!strcmp(name, "normalize")) { int i,c=ncomp(a[0]); float s=0; for(i=0;i<c;i++) s+=a[0].f[i]*a[0].f[i]; s=sqrtf(s); if(s>0) for(i=0;i<c;i++) a[0].f[i]/=s; return a[0]; }
	if (!strcmp(name, "pow")) { int i,c=ncomp(a[0]); GlslVal r=a[0]; for(i=0;i<c;i++) r.f[i]=powf(a[0].f[i], a[1].f[ncomp(a[1])==1?0:i]); return r; }
	if (!strcmp(name, "exp")) return vmap1(a[0], expf);
	if (!strcmp(name, "log")) return vmap1(a[0], logf);
	if (!strcmp(name, "exp2")) return vmap1(a[0], exp2f);
	if (!strcmp(name, "sqrt")) return vmap1(a[0], sqrtf);
	if (!strcmp(name, "inversesqrt")) { int i,c=ncomp(a[0]); for(i=0;i<c;i++) a[0].f[i]=1.0f/sqrtf(a[0].f[i]); return a[0]; }
	if (!strcmp(name, "abs")) return vmap1(a[0], fabsf);
	if (!strcmp(name, "floor")) return vmap1(a[0], floorf);
	if (!strcmp(name, "ceil")) return vmap1(a[0], ceilf);
	if (!strcmp(name, "fract")) return vmap1(a[0], b_fract);
	if (!strcmp(name, "sign")) return vmap1(a[0], b_sign);
	if (!strcmp(name, "sin")) return vmap1(a[0], sinf);
	if (!strcmp(name, "cos")) return vmap1(a[0], cosf);
	if (!strcmp(name, "tan")) return vmap1(a[0], tanf);
	if (!strcmp(name, "radians")) return vmap1(a[0], b_radians);
	if (!strcmp(name, "mod")) { int i,c=ncomp(a[0]); GlslVal r=a[0]; for(i=0;i<c;i++) r.f[i]=(float)fmod(a[0].f[i], a[1].f[ncomp(a[1])==1?0:i]); return r; }
	if (!strcmp(name, "min")) { int i,c=ncomp(a[0]); GlslVal r=a[0]; for(i=0;i<c;i++){float y=a[1].f[ncomp(a[1])==1?0:i]; r.f[i]=a[0].f[i]<y?a[0].f[i]:y;} return r; }
	if (!strcmp(name, "max")) { int i,c=ncomp(a[0]); GlslVal r=a[0]; for(i=0;i<c;i++){float y=a[1].f[ncomp(a[1])==1?0:i]; r.f[i]=a[0].f[i]>y?a[0].f[i]:y;} return r; }
	if (!strcmp(name, "clamp")) { int i,c=ncomp(a[0]); GlslVal r=a[0]; for(i=0;i<c;i++){float lo=a[1].f[ncomp(a[1])==1?0:i],hi=a[2].f[ncomp(a[2])==1?0:i]; r.f[i]=clampf(a[0].f[i],lo,hi);} return r; }
	if (!strcmp(name, "mix")) { int i,c=ncomp(a[0]); GlslVal r=a[0]; for(i=0;i<c;i++){float t=a[2].f[ncomp(a[2])==1?0:i]; r.f[i]=a[0].f[i]*(1-t)+a[1].f[i]*t;} return r; }
	if (!strcmp(name, "step")) { int i,c=ncomp(a[1]); GlslVal r=a[1]; for(i=0;i<c;i++){float ed=a[0].f[ncomp(a[0])==1?0:i]; r.f[i]=a[1].f[i]<ed?0.0f:1.0f;} return r; }
	if (!strcmp(name, "smoothstep")) { int i,c=ncomp(a[2]); GlslVal r=a[2]; for(i=0;i<c;i++){float e0=a[0].f[ncomp(a[0])==1?0:i],e1=a[1].f[ncomp(a[1])==1?0:i]; float t=clampf((a[2].f[i]-e0)/(e1-e0>0?e1-e0:1e-6f),0,1); r.f[i]=t*t*(3-2*t);} return r; }
	if (!strcmp(name, "reflect")) { int i,c=ncomp(a[0]); float d=0; GlslVal r=a[0]; for(i=0;i<c;i++) d+=a[0].f[i]*a[1].f[i]; for(i=0;i<c;i++) r.f[i]=a[0].f[i]-2*d*a[1].f[i]; return r; }
	if (!strcmp(name, "float")) return vscalar(a[0].f[0]);
	if (!strcmp(name, "int")) { GlslVal r = vscalar((float)(int)a[0].f[0]); r.is_int=1; return r; }
	*handled = 0;
	return vscalar(0);
}

/* ----------------------------------------------------------- public API ---- */
GlslProg *glsl_compile(const char *src, int stage, char *errbuf, int errcap)
{
	static volatile long serial;
	GlslProg *p = (GlslProg *)calloc(1, sizeof(GlslProg));
	if (!p) return 0;
	p->serial = (unsigned)++serial;
	p->stage = stage;
	p->err = errbuf; p->errcap = errcap;
	if (errbuf && errcap) errbuf[0] = 0;
	{
		size_t n = strlen(src) + 1;
		p->src_copy = (char *)malloc(n);
		memcpy(p->src_copy, src, n);
	}
	lex(&p->lex, p->src_copy);
	toplevel(p);
	/* Builtin outputs are never declared, so register them as globals now, else an
	 * assignment to gl_Position lands in a lost local and glsl_get finds nothing. */
	{
		static const char *bi[] = { "gl_Position", "gl_FragCoord", "gl_FragColor",
					    "gl_FragData", 0 };
		int i;
		for (i = 0; bi[i]; i++)
			if (!find_global(p, bi[i]) && p->nglob < 128) {
				Global *g = &p->glob[p->nglob++];
				strncpy(g->name, bi[i], 63);
				memset(&g->val, 0, sizeof g->val);
				g->val.rows = 4;
				g->val.cols = 1;
			}
	}
	/* Evaluate scalar/vector global initializers now (e.g. const float PI = 3.14159;)
	 * so uses of them see the value instead of zero. In declaration order, so a const
	 * may reference an earlier one. */
	{
		Env ie; int gi;
		memset(&ie, 0, sizeof ie);
		ie.p = p;
		for (gi = 0; gi < p->nglob; gi++) {
			Global *g = &p->glob[gi];
			if (!g->init) continue;
			if (g->is_array && g->arr) {
				/* array constructor: eval each element into arr storage */
				Node *ac = g->init;
				int ef = g->arr_cols > 1 ? g->arr_rows * g->arr_cols : g->arr_rows, k;
				for (k = 0; k < ac->nlist && k < g->arr_len; k++) {
					GlslVal v = eval(&ie, ac->list[k]);
					int c = ncomp(v), j;
					for (j = 0; j < ef && j < c; j++) g->arr[k * ef + j] = v.f[j];
				}
			} else {
				g->val = eval(&ie, g->init);
			}
		}
	}
	{
		int i, has_main = 0, mainbody = -1;
		for (i = 0; i < p->nfunc; i++) if (!strcmp(p->funcs[i].name, "main")) { has_main = 1; mainbody = p->funcs[i].body ? p->funcs[i].body->nlist : -2; }
		if (glsl_debug()) {
			fprintf(stderr, "[glsl] ntok=%d nfunc=%d nglob=%d main.stmts=%d globals:",
				p->lex.n, p->nfunc, p->nglob, mainbody);
			for (i = 0; i < p->nglob; i++) fprintf(stderr, " %s", p->glob[i].name);
			fprintf(stderr, "\n");
		}
		if (!has_main) { if (errbuf && errcap) snprintf(errbuf, errcap, "no main()"); glsl_free(p); return 0; }
	}
	return p;
}

void glsl_free(GlslProg *p)
{
	int i;
	if (!p) return;
	if (p->is_clone) { free(p); return; }
	for (i = 0; i < p->nglob; i++) free(p->glob[i].arr);
	for (i = 0; i < p->npool; i++) { free(p->pool[i]->list); free(p->pool[i]); }
	free(p->pool);
	free(p->lex.t);
	free(p->src_copy);
	free(p);
}

int glsl_sync(GlslProg **clone, GlslProg *src)
{
	GlslProg *c = *clone;
	if (!src)
		return 0;
	if (!c || c->serial != src->serial) {
		if (!c)
			c = (GlslProg *)malloc(sizeof(GlslProg));
		if (!c)
			return 0;
		memcpy(c, src, sizeof(GlslProg));
		c->is_clone = 1;
		c->err = 0;
		c->errcap = 0;
		*clone = c;
		return 1;
	}
	c->nglob = src->nglob;
	memcpy(c->glob, src->glob, (size_t)src->nglob * sizeof(Global));
	return 1;
}

void glsl_set(GlslProg *p, const char *name, const float *v, int n)
{
	Global *g = find_global(p, name);
	int i;
	if (!g) {
		if (p->nglob >= 128) return;
		g = &p->glob[p->nglob++];
		strncpy(g->name, name, 63);
		memset(&g->val, 0, sizeof(g->val));
	}
	if (g->is_array && g->arr) {
		/* array uniform (e.g. glUniformMatrix4fv count>1): fill element storage */
		int ef = g->arr_cols > 1 ? g->arr_rows * g->arr_cols : g->arr_rows;
		int total = g->arr_len * ef;
		for (i = 0; i < n && i < total; i++) g->arr[i] = v[i];
		return;
	}
	for (i = 0; i < n && i < 16; i++) g->val.f[i] = v[i];
	if (g->size > 0) return; /* declared: keep its type's shape */
	if (n == 16) { g->val.rows = 4; g->val.cols = 4; }
	else if (n == 9) { g->val.rows = 3; g->val.cols = 3; }
	else if (n == 4 && g->val.cols <= 1) { g->val.rows = 4; g->val.cols = 1; }
	else { g->val.rows = (unsigned char)(n < 1 ? 1 : n); g->val.cols = 1; }
}

void glsl_set_sampler(GlslProg *p, const char *name, int unit)
{
	Global *g = find_global(p, name);
	if (!g) { if (p->nglob >= 128) return; g = &p->glob[p->nglob++]; strncpy(g->name, name, 63); memset(&g->val,0,sizeof g->val); }
	g->is_sampler = 1; g->sampler_unit = unit;
	g->val.f[0] = (float)unit; g->val.rows = 1; g->val.cols = 1;
}

int glsl_run(GlslProg *p, GlslSampleFn sample, void *ctx)
{
	Env e; int i;
	env_init(&e);
	e.p = p; e.sample = sample; e.sctx = ctx;
	for (i = 0; i < p->nfunc; i++)
		if (!strcmp(p->funcs[i].name, "main")) { eval(&e, p->funcs[i].body); break; }
	if (glsl_debug()) {
		fprintf(stderr, "[glsl] after run: err=%d returned=%d;", e.err, e.returned);
		for (i = 0; i < p->nglob; i++)
			fprintf(stderr, " %s=(%.3f,%.3f,%.3f,%.3f)", p->glob[i].name,
				p->glob[i].val.f[0], p->glob[i].val.f[1],
				p->glob[i].val.f[2], p->glob[i].val.f[3]);
		fprintf(stderr, "\n");
	}
	return !e.err;
}

int glsl_get(GlslProg *p, const char *name, float *out, int cap)
{
	Global *g = find_global(p, name);
	int i, c;
	if (!g) return 0;
	c = ncomp(g->val);
	for (i = 0; i < c && i < cap; i++) out[i] = g->val.f[i];
	return c;
}

int glsl_vars(GlslProg *p, GlslVar *out, int cap)
{
	int i, k = 0;
	for (i = 0; i < p->nglob && k < cap; i++) {
		Global *g = &p->glob[i];
		if (!g->qual)
			continue;
		out[k].name = g->name;
		out[k].qual = g->qual;
		out[k].location = g->location;
		out[k].rows = g->is_sampler ? 1 : g->val.rows;
		out[k].cols = g->is_sampler ? 1 : g->val.cols;
		out[k].is_sampler = g->is_sampler;
		out[k].arr_len = g->is_array ? g->arr_len : 0;
		k++;
	}
	return k;
}

int glsl_kind(GlslProg *p, const char *name)
{
	Global *g = find_global(p, name);
	if (!g) return 0;
	return g->is_sampler ? 2 : 1;
}

int glsl_inputs(GlslProg *p, const char **names, int *sizes, int cap)
{
	int i, k = 0;
	for (i = 0; i < p->nglob && k < cap; i++) {
		names[k] = p->glob[i].name;
		sizes[k] = p->glob[i].size ? p->glob[i].size : ncomp(p->glob[i].val);
		k++;
	}
	return k;
}
