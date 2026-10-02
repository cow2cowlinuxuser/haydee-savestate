/* The frame channel between the software renderer and gpuhost64.exe.
 *
 * The renderer finishes each frame on the CPU exactly as before, then instead
 * of blitting it with GDI copies it into shared memory and signals. A separate
 * 64-bit process owns the only GPU device, puts a flip-model swap chain on the
 * game's window - a cross-process swap chain, which DXGI allows for flip
 * discard - and presents the frame through a shader pass.
 *
 * The point of the second process is that the game's own never loads a
 * graphics driver. A driver's threads, mappings and kernel objects are exactly
 * what a savestate cannot rewind, so keeping them on the far side of a process
 * boundary leaves the snapshot as clean as it is with GDI.
 *
 * Frames travel through three slots handed back and forth by atomic exchange,
 * the classic lock-free triple buffer: the renderer owns one to write, the
 * host owns one to read, and the third sits in the middle holding the newest
 * finished frame. Neither side ever waits for the other, and neither can
 * touch a slot the other is using.
 *
 * Mode 2 (D3D11SW_GPUHOST=2) moves the drawing across as well. The renderer
 * stops rasterising backbuffer draws and instead writes them into a byte ring
 * after the slots: textures once per content change, then per frame a begin,
 * the draws as raw triangles with a compact state, and an end. The host
 * replays them on the GPU into an offscreen target of the game's size and
 * shows that through the same shader pass. Frames the renderer still draws
 * itself - before the host is up, or with nothing on the backbuffer - keep
 * travelling through the slots. */
#ifndef GPUHOST_H
#define GPUHOST_H

#include <stdint.h>

#define GH_MAGIC 0x31485352u /* "RSH1" */
#define GH_VERSION 2
#define GH_SLOTS 3
#define GH_HEADER_BYTES 4096u
/* Set in `middle` when the slot it names holds a frame the host has not
 * taken yet. */
#define GH_FRESH 0x100

enum {
	GH_STARTING = 0, /* host launched, not ready */
	GH_READY = 1,	 /* swap chain is up; frames will be shown */
	GH_FAILED = 2,	 /* host gave up; msg and error say why */
	GH_EXITED = 3	 /* host left cleanly */
};

typedef struct GhHeader {
	uint32_t magic, version, header_bytes, slot_bytes;
	uint32_t game_pid, host_pid;
	uint64_t hwnd;
	/* Written by the host. */
	volatile int32_t state;
	volatile int32_t error; /* HRESULT when state is GH_FAILED */
	volatile int32_t heartbeat;
	volatile int32_t shown;	    /* frames presented */
	volatile int32_t shader_user; /* 1 when gpuhost.hlsl was loaded */
	/* Written by the renderer. */
	volatile int32_t quit;
	volatile int32_t published; /* frames handed over */
	volatile int32_t middle;    /* slot index, | GH_FRESH when unread */
	volatile int32_t back;	    /* the renderer's slot, for reattaching */
	uint32_t slot_w[GH_SLOTS], slot_h[GH_SLOTS];
	uint32_t point, integer; /* scaling: nearest filter, whole multiples */
	char msg[256];
	/* Mode 2. Offsets are from the start of the mapping; ring_bytes is a
	 * power of two and head/tail are free-running byte counts. */
	uint32_t mode, halfpixel;
	uint32_t ring_off, ring_bytes, rb_off, rb_bytes;
	volatile uint32_t ring_head; /* written by the renderer */
	volatile uint32_t ring_tail; /* written by the host */
	volatile int32_t rb_done;    /* host: sequence of the last readback served */
	volatile int32_t rb_ok;	     /* host: 1 if that readback has pixels */
	volatile int32_t gpu_frames, gpu_draws, gpu_textures; /* host counters */
} GhHeader;

#define GH_SLOT(h, i) ((uint32_t *)((uint8_t *)(h) + (h)->header_bytes + (size_t)(i) * (h)->slot_bytes))

#define GH_RING_BYTES (16u << 20)
/* Upload rows travel in pieces no larger than this, so a big texture never
 * needs the whole ring at once. */
#define GH_CHUNK_BYTES (1u << 20)

enum {
	GH_OP_TEX_DEF = 1,  /* GhTexDef: create or resize texture id */
	GH_OP_TEX_ROWS,	    /* GhTexRows + rows * w * 4 bytes */
	GH_OP_TEX_DROP,	    /* GhTexDrop */
	GH_OP_FRAME_BEGIN,  /* GhFrameBegin */
	GH_OP_DRAW,	    /* GhDraw + ntri * GhTri */
	GH_OP_FRAME_END,    /* no payload: show the frame */
	GH_OP_READBACK	    /* GhReadback: copy the target into rb_off */
};

/* Every record starts with this; bytes includes it and is a multiple of 8. */
typedef struct GhRec {
	uint32_t op, bytes;
} GhRec;

typedef struct GhTexDef {
	uint32_t id, w, h, pad;
} GhTexDef;

typedef struct GhTexRows {
	uint32_t id, y0, rows, w;
} GhTexRows;

typedef struct GhTexDrop {
	uint32_t id, pad;
} GhTexDrop;

typedef struct GhFrameBegin {
	uint32_t w, h, clear, argb;
} GhFrameBegin;

enum { GH_BLEND_OFF, GH_BLEND_OVER, GH_BLEND_ADD, GH_BLEND_MUL, GH_BLEND_RSUB, GH_BLEND_N };

typedef struct GhDraw {
	uint32_t tex, ntri;
	uint8_t blend, bilinear, mul_identity, scissor;
	float alpha_ref; /* 0 = no test, else clip below this */
	float sharpen;	 /* alpha slope about 0.5, <= 1 = none */
	int32_t sx0, sy0, sx1, sy1;
	uint32_t pad;
} GhDraw;

/* SwVert as the renderer has it: x, y, z, rhw, colour 0xAARRGGBB, u, v. The
 * same 28 bytes in either bitness, which is why it is sent untouched. */
typedef struct GhVert {
	float x, y, z, rhw;
	uint32_t color;
	float u, v;
} GhVert;

typedef struct GhTri {
	GhVert a, b, c;
} GhTri;

typedef struct GhReadback {
	uint32_t w, h;
	int32_t seq, pad;
} GhReadback;

#endif
