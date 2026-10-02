/* The command channel between the software OpenGL DLL and glhost64.exe.
 *
 * The DLL keeps doing everything it did before - object tables, uniforms,
 * CPU copies of textures, buffers and shader source - but stops rasterising.
 * Each GL call the game makes is also written into a byte ring in shared
 * memory, and a separate 64-bit process replays it on a real OpenGL context
 * with the game's own shaders, then presents the default framebuffer on the
 * game's window through a D3D11 flip-model swap chain (a cross-process swap
 * chain, as gpuhost64.exe does). The game's process never loads a driver.
 *
 * Names are the DLL's: every texture, buffer, program and so on travels as
 * the id the DLL gave the game, and the host keeps its own translation. The
 * same goes for uniform locations, which the DLL numbers as
 * (slot << GLH_UNI_ELEM_BITS) | element; the host resolves each by name.
 *
 * If the host goes away, the DLL simply resumes drawing on the CPU: the
 * state it kept all along is still there. */
#ifndef GLHOST_H
#define GLHOST_H

#include <stdint.h>

#define GLH_MAGIC 0x31484C47u /* "GLH1" */
#define GLH_VERSION 2
#define GLH_HEADER_BYTES 4096u
#define GLH_RING_BYTES (64u << 20)
/* Readback area: glReadPixels / glGetTexImage results land here. */
#define GLH_RB_BYTES (32u << 20)
/* Payloads larger than this travel ahead of their call as GLH_OP_BLOB
 * pieces, so no record needs more than a fraction of the ring. */
#define GLH_CHUNK_BYTES (4u << 20)
#define GLH_UNI_ELEM_BITS 10
#define GLH_UNI_ELEM_MASK ((1 << GLH_UNI_ELEM_BITS) - 1)

enum {
	GLH_STARTING = 0,
	GLH_READY = 1, /* GL context is up; records are being replayed */
	GLH_FAILED = 2,
	GLH_EXITED = 3
};

typedef struct GlhHeader {
	uint32_t magic, version, header_bytes, pad0;
	uint32_t game_pid, host_pid;
	uint32_t ring_off, ring_bytes, rb_off, rb_bytes;
	/* Written by the host. */
	volatile int32_t state;
	volatile int32_t error;
	volatile int32_t heartbeat;
	volatile int32_t presented; /* GLH_OP_PRESENT records completed */
	volatile int32_t rb_done;   /* sequence of the last readback served */
	volatile int32_t rb_ok;
	volatile int32_t sleeping;  /* host is (about to be) waiting on the event */
	volatile uint32_t ring_tail;
	/* Written by the DLL. */
	volatile int32_t quit;
	volatile uint32_t ring_head;
	/* GLSW_FRAMEGRAPH: the frame (1-based present count) both sides time call by
	 * call into glframe_<N>_game.csv and glframe_<N>_host.csv. 0 for none. */
	uint32_t time_frame;
	/* Host statistics, for the DLL's log. */
	volatile uint32_t bytes_frame; /* ring bytes consumed by the last frame */
	volatile uint32_t records_frame;
	volatile uint32_t gl_errors;
	char msg[256];
} GlhHeader;

/* Every record: this header, nargs 8-byte arguments, then paylen bytes of
 * payload, padded so bytes is a multiple of 8. */
typedef struct GlhRec {
	uint32_t op, bytes;
	uint16_t nargs, flags;
	uint32_t paylen;
} GlhRec;

/* The payload was sent ahead as GLH_OP_BLOB records; use the staged bytes. */
#define GLH_REC_STAGED 1

/* Hand-written operations. Generated per-function operations start at
 * GLH_OP_GEN (see glhost_ops.h). */
enum {
	GLH_OP_NOP = 0,
	GLH_OP_CTX,	    /* a0 = DLL context id: make its host context current */
	GLH_OP_CTX_DELETE,  /* a0 = DLL context id */
	GLH_OP_BLOB,	    /* payload appended to the staging buffer */
	GLH_OP_PRESENT,	    /* a0 hwnd, a1 w, a2 h, a3 swap interval */
	GLH_OP_DFLT_SIZE,   /* a0 w, a1 h: the default framebuffer's size */
	GLH_OP_UNI_LOC,	    /* a0 program, a1 location; payload name */
	GLH_OP_GEN_NAMES,   /* a0 kind; payload GLuint names */
	GLH_OP_DEL_NAMES,   /* a0 kind; payload GLuint names */
	GLH_OP_CREATE_SHADER, /* a0 id, a1 type */
	GLH_OP_CREATE_PROGRAM, /* a0 id */
	GLH_OP_SHADER_SOURCE, /* a0 shader; payload source */
	GLH_OP_BIND_ATTRIB,   /* a0 program, a1 index; payload name */
	GLH_OP_BIND_FRAGDATA, /* a0 program, a1 colour; payload name */
	GLH_OP_TF_VARYINGS,   /* a0 program, a1 count, a2 mode; payload names, each NUL-ended */
	GLH_OP_UNIFORM,	    /* a0 kind, a1 location, a2 count, a3 transpose; payload values */
	GLH_OP_TEX_IMAGE,   /* a0 fn, a1 target, a2 level, a3 internal, a4 w, a5 h, a6 depth,
			       a7 border, a8 format, a9 type, a10 x, a11 y, a12 has data
			       (0 none, 1 payload, 2 offset in a13 from the unpack buffer) */
	GLH_OP_TEX_PARAMV,  /* a0 target, a1 pname, a2 is float; payload 4 values */
	GLH_OP_BUFFER_DATA, /* a0 target, a1 size, a2 usage, a3 has data; payload */
	GLH_OP_BUFFER_SUB,  /* a0 target, a1 offset; payload */
	GLH_OP_BUFFER_PUT,  /* a0 buffer id, a1 offset; payload (an unmapped range) */
	GLH_OP_ATTRIB_PTR,  /* a0 index, a1 size, a2 type, a3 normalized, a4 stride,
			       a5 offset, a6 integer, a7 client memory (skip until draw) */
	GLH_OP_ATTRIBV,	    /* a0 index, a1 count, a2 normalized ubytes; payload */
	GLH_OP_DRAW,	    /* see GlhDraw */
	GLH_OP_DRAW_BUFFERS, /* payload GLenums */
	GLH_OP_STIPPLE,	    /* payload 128 bytes */
	GLH_OP_READ_PIXELS, /* a0 seq, a1 x, a2 y, a3 w, a4 h, a5 format, a6 type */
	GLH_OP_GET_TEX_IMAGE, /* a0 seq, a1 target, a2 level, a3 format, a4 type */
	GLH_OP_PIXEL_STORE, /* a0 pname, a1 value */
	GLH_OP_HOLD,	    /* a0 1: a save exists, so deleted objects are kept, not destroyed */
	GLH_OP_RESTORED,    /* a0 the DLL's next object id after a restore: everything made
			       from it on belongs to the future the restore left behind */
	GLH_OP_GEN = 64
};

static const char *const glh_hand_names[] = {
	"NOP", "CTX", "CTX_DELETE", "BLOB", "PRESENT", "DFLT_SIZE", "UNI_LOC", "GEN_NAMES",
	"DEL_NAMES", "CREATE_SHADER", "CREATE_PROGRAM", "SHADER_SOURCE", "BIND_ATTRIB",
	"BIND_FRAGDATA", "TF_VARYINGS", "UNIFORM", "TEX_IMAGE", "TEX_PARAMV", "BUFFER_DATA",
	"BUFFER_SUB", "BUFFER_PUT", "ATTRIB_PTR", "ATTRIBV", "DRAW", "DRAW_BUFFERS", "STIPPLE",
	"READ_PIXELS", "GET_TEX_IMAGE", "PIXEL_STORE", "HOLD", "RESTORED",
};
#define GLH_HAND_N ((uint32_t)(sizeof(glh_hand_names) / sizeof(glh_hand_names[0])))

/* GLH_OP_UNIFORM kinds: element type and components.
   a0 = type | components << 4 (8 bits) | columns << 12 | rows << 16. */
enum { GLH_UNI_F = 0, GLH_UNI_I = 1, GLH_UNI_U = 2, GLH_UNI_MAT = 3 };

/* Object kinds for GEN_NAMES / DEL_NAMES and the host's translation tables. */
enum {
	GLH_K_TEX = 0,
	GLH_K_BUF,
	GLH_K_VAO,
	GLH_K_FBO,
	GLH_K_RBO,
	GLH_K_OBJ, /* shaders and programs: one namespace in the DLL */
	GLH_K_SAMP,
	GLH_K_N
};

/* GLH_OP_DRAW arguments. */
enum {
	GLH_DRAW_ARRAYS = 0,
	GLH_DRAW_ELEMENTS = 1
};
/* a0 kind, a1 mode, a2 first, a3 count, a4 index type, a5 index offset,
 * a6 instances, a7 basevertex, a8 client attribute count, a9 client indices.
 * Payload: per client attribute a GlhClientAttr and its bytes (padded to 8),
 * then the indices when a9 is set. */
typedef struct GlhClientAttr {
	uint32_t index, size, type, normalized;
	uint32_t stride, integer, bytes, divisor;
} GlhClientAttr;

#endif
