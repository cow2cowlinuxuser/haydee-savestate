/* Giving the game a heap of its own.
 *
 * Every ownership heuristic in savestate.c exists for one reason: Rabi-Ribi
 * links ucrtbase, and ucrtbase does not create a heap. It calls GetProcessHeap
 * and allocates out of the same arena ntdll, GDI and combase are using. So the
 * game's state and Windows' state are interleaved block by block in one heap,
 * and every restore has to guess which is which. BLKOWNER, HELDVETO, VTABVETO,
 * the reachability closure and the recycled-block hazard are all consequences
 * of that single fact.
 *
 * DoDonPachi Resurrection is the control group, and it says what the other side
 * looks like. It links MSVCR100, which calls HeapCreate, so the game's runtime
 * heap has no other tenant. Its restore reported 208 regions, none by block, and
 * captured 1076 MB of 1089 - 98.8% of the process, with no ownership vote taken
 * anywhere. Rabi-Ribi's captures 59% and negotiates for the rest.
 *
 * We cannot change which runtime the game links. We can change where its
 * allocations land: create a heap, and redirect the executable's imports of
 * malloc and friends into it. What the game allocates from then on has no other
 * tenant, exactly as if it had linked MSVCR100.
 *
 * Three things make this safe rather than merely appealing.
 *
 * The redirect goes in after the process has started, so everything allocated
 * before it still lives on the process heap and will be freed through a pointer
 * we did not issue. free therefore dispatches: ours goes to our heap, everything
 * else goes to the runtime's own free, unchanged. The mixed period is correct by
 * construction rather than by timing.
 *
 * Ownership is not decided by address alone. A range table says which regions
 * our heap has been seen to occupy, and every block we issue carries an eight
 * byte header whose magic is keyed to the pointer it precedes. The range check
 * comes first because it cannot fault; the header is only read once the range
 * says the memory was ours, and it is what actually decides. A region that our
 * heap later hands back to the operating system can therefore never be mistaken
 * for ours if somebody else is given the same address.
 *
 * Allocations at or above GH_BIG stay with the runtime. A private heap satisfies
 * those with a dedicated VirtualAlloc that is released on free, which is the one
 * way a range in our table goes stale. They are also rare and long-lived, which
 * is to say they are not the state that fails to travel.
 *
 * Off unless D3D9SW_GAMEHEAP=1. This moves where the game's memory lives, and
 * nothing else in the engine has ever done that. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "savestate.h"

void savestate_log_line(const char *s);
int savestate_patch_iat(HMODULE mod, void *from, void *to);
int savestate_patch_iat_named(HMODULE mod, const char *dll, const char *fn, void *to,
			      void **prev);
void savestate_game_heap(HANDLE h);
void savestate_exclude(void *p, size_t bytes);
/* Defined further down with the note on why a fixed base is what makes a layout
 * reproducible. Declared here because the installer runs above it. */
static HANDLE gh_create_heap(void);
static void clock_probe_install(HMODULE exe);
static int gh_wholesale(void);
static void gh_make_selfcontained(HANDLE h);
/* Public: 1 when `base` is the start of a heap we own AND have made self-contained
 * for a wholesale (freelist-and-all) restore. The savestate engine asks this to
 * decide whether a rewound heap region may be copied whole instead of block by
 * block. Returns 0 unless D3D9SW_WHOLESALE is set. */
int gameheap_owned_heap(uintptr_t base);

static void ss_log(const char *fmt, ...)
{
	char b[512];
	va_list ap;

	va_start(ap, fmt);
	wvsprintfA(b, fmt, ap);
	va_end(ap);
	savestate_log_line(b);
}

/* Big enough that a private heap serves it from its own reservation rather than
 * out of a segment, which is the case where a freed block releases address space
 * that somebody else can then be given. */
#define GH_BIG (256u * 1024u)
#define GH_REG 512
#define GH_MAGIC ((uintptr_t)0x9E3779B9u)

typedef struct {
	uintptr_t magic;
	size_t size;
} GhHead;

static HANDLE g_heap;
static CRITICAL_SECTION g_cs;
static int g_ready;

static uintptr_t g_lo[GH_REG], g_hi[GH_REG];
static volatile LONG g_nreg;

/* D3D9SW_LAA_FALSE: a second region we own and the game does not know exists.
 * When on, the redirect and the floor serve blocks out of it instead of the
 * pinned arena, so the game runs on memory we control end to end. On a non-LAA
 * exe there is no space above 2 GB, so this competes with the game and is
 * EXPECTED to crash - that crash is the measurement. Reserved once and never
 * per-block freed, so it snapshots atomically like the arena. */
static HANDLE g_laa_heap;
static int g_laa_on;
static unsigned g_laa_mb;
static uintptr_t g_laa_lo, g_laa_hi;
/* The pinned arena's extent, set only on a successful fixed-base pin - a growable
 * HeapCreate fallback has no bounded extent and cannot be wholesale-restored. */
static uintptr_t g_heap_lo, g_heap_hi;
static int g_laa_high;			/* landed in the >2 GB band (exe really is LAA) */
static unsigned long g_laa_fell;	/* times the region was full and we fell back */
static void gh_create_laa_region(void);

static unsigned long g_alloc, g_freed_ours, g_freed_theirs, g_toobig, g_regfull;
static unsigned long g_stale, g_fellback;

/* Offsets the operator asked to watch, parsed at install. See gh_peek_one. */
#define GH_PEEK_MAX 8
static unsigned g_peek_off[GH_PEEK_MAX];
static int g_peek_n;
static void gh_peek_free(void *u);
static void gh_peek_parse(void);
static void gh_vorbis_free(const void *u, unsigned size);
static void gh_watch_parse(void);
static void gh_watch(const void *u, size_t n, unsigned site, unsigned ord,
		     const char *how);

/* -------------------------------------------------- the allocation trace
 *
 * D3D9SW_GHTRACE=N records the first N allocator operations - what kind, how
 * big, and where in the heap the block landed - and writes them to gh_trace.txt
 * beside the log.
 *
 * The reason to have it is an observation that is currently resting on two data
 * points. The player entity has been found at heap base + 0x7F4B8 in two
 * separate sessions whose heap bases differed (0DFB0000 and 0DC70000), which
 * says the heap's internal placement is a function of the allocation sequence
 * and nothing else - the base moves, the contents do not. If that holds over
 * tens of thousands of operations rather than one lucky pointer, then a
 * snapshot is relocatable and an address in it can be derived from a single
 * anchor. If it does not hold, everything built on it falls over, and it is far
 * cheaper to find that out from a diff of two traces than from a restore that
 * faults.
 *
 * Two runs, diff the offset column. Identical means deterministic.
 *
 * Constraints this sits under: it runs inside the game's malloc, on every call,
 * so it must not allocate, must not lock, and must not touch a file. A slot is
 * claimed with one interlocked increment into a buffer reserved up front, and
 * the whole thing is written out later from the census, which already runs on a
 * frame boundary. A lock here would change the timing of the very thing being
 * measured.
 */
#define GH_TR_ALLOC 'A'  /* served from our heap */
#define GH_TR_CALLOC 'C' /* served from our heap, zeroed */
#define GH_TR_REALLOC 'R'
#define GH_TR_FREE 'F'
#define GH_TR_BIG 'B'	 /* over GH_BIG, handed to the runtime */
#define GH_TR_OTHER 'O'	 /* freed something that was never ours */

typedef struct {
	unsigned op;
	unsigned size;
	unsigned off; /* from the heap base, or ~0 when it is not in our heap */
	unsigned site; /* the code that asked, as an RVA, or ~0 */
	unsigned ord;  /* how many that site had already asked for, or ~0 */
} GhTr;

static GhTr *g_tr;
static volatile LONG g_tr_n;
static LONG g_tr_cap;

static void gh_trace_at(unsigned op, size_t n, const void *u, unsigned site, unsigned ord)
{
	LONG i;

	if (!g_tr)
		return;
	i = InterlockedIncrement(&g_tr_n) - 1;
	if (i >= g_tr_cap)
		return;
	g_tr[i].op = op;
	g_tr[i].size = (unsigned)n;
	/* A Windows heap handle is the address of its first segment, so the handle
	 * doubles as the base the offsets are measured from. */
	g_tr[i].off = (u && g_heap && (uintptr_t)u > (uintptr_t)g_heap)
			      ? (unsigned)((uintptr_t)u - (uintptr_t)g_heap)
			      : 0xFFFFFFFFu;
	g_tr[i].site = site;
	g_tr[i].ord = ord;
}

static void gh_trace(unsigned op, size_t n, const void *u)
{
	gh_trace_at(op, n, u, 0xFFFFFFFFu, 0xFFFFFFFFu);
}

/* ---------------------------------------------------------------- block tags
 *
 * An address says where a block landed, not which block it is. Two sessions put
 * 96% of blocks at matching offsets under the stock heap, but a matching offset
 * is not evidence by itself: with a thousand blocks in a bounded arena some of
 * them coincide, and nothing in an address distinguishes the coincidences from
 * the real matches.
 *
 * So a block is named when it is asked for rather than recognised afterwards.
 * The name is the call site that asked plus how many times that site has asked
 * for that size already - the n-th block down this chute. The site is stable
 * because it is a code offset in an executable that does not change between
 * launches; the count is stable for as long as the game takes the same path,
 * which is the property actually under test.
 *
 * Size belongs in the key, not just the payload. A call site reached from two
 * paths allocating two different things would otherwise interleave its counts
 * and produce an ordinal that means nothing in either.
 *
 * The tags live beside the heap in a table keyed by address, NOT in GhHead.
 * Widening that header would push every block along and destroy the offset
 * agreement these tags exist to measure - the measurement would change the
 * thing being measured.
 *
 * Same constraints as the trace: this runs inside the game's malloc, on every
 * call, so it must not allocate, must not lock and must not touch a file. Both
 * tables are reserved up front and reached with interlocked operations only.
 */
/* Sized for a long run rather than for the live set. Retired entries are
 * tombstoned so that probe chains stay walkable, and although an insert reuses
 * the first tombstone it meets, a lookup has to walk past them - so what fills
 * this table is the total churn of a session, not its high-water mark. A
 * ten-minute run allocates tens of thousands of blocks against a live set of
 * about a thousand. */
#define GH_TAG_SLOTS 262144u
#define GH_TAG_MASK (GH_TAG_SLOTS - 1u)
#define GH_TAG_DEAD ((LONG)1) /* freed, but the probe chain must run through it */
/* Far larger than the ~1,200 blocks live at once, because retired slots are
 * tombstoned and only reused opportunistically: the table has to absorb the
 * whole churn of a run, not the high-water mark. */
#define GH_SITE_SLOTS 16384u
#define GH_SITE_MASK (GH_SITE_SLOTS - 1u)
#define GH_PROBE 64

typedef struct {
	volatile LONG addr; /* 0 free, 1 tombstone, otherwise the user pointer */
	unsigned site;
	unsigned size;
	unsigned ord;
} GhTag;

typedef struct {
	volatile LONG key; /* 0 free, otherwise the mixed (site, size) */
	unsigned site;
	unsigned size;
	volatile LONG next; /* how many this chute has produced */
} GhSite;

static GhTag *g_tag;
static GhSite *g_site;
static uintptr_t g_exe;	    /* so a site is an RVA and survives relocation */
static volatile LONG g_tag_full, g_site_full, g_tag_congested;

static unsigned gh_mix(unsigned x)
{
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

/* One frame, via the compiler, rather than a stack walk: the game is built with
 * frame pointers omitted in places, so the EBP chain cannot be trusted, and one
 * frame is enough to name the chute. That frame is genuinely the game's own code
 * - it links its runtime statically and we detour its private copies, so there
 * is no runtime thunk sitting between the caller and here. */
static unsigned gh_site_of(void *ra)
{
	uintptr_t a = (uintptr_t)ra;

	if (!g_exe || a < g_exe)
		return 0xFFFFFFFFu;
	return (unsigned)(a - g_exe);
}

/* Matching on the mixed key alone, not on (site, size) as well, because the two
 * fields are written after the slot is claimed and a reader could otherwise see
 * them half set. A key collision would merge two chutes' counts; at 32 bits
 * mixed that is rare enough to accept in a diagnostic, and g_site_full reports
 * the case that actually matters, which is the table filling up. */
static unsigned gh_ordinal(unsigned site, unsigned size)
{
	unsigned key = gh_mix(site ^ (size * 2654435761u));
	unsigned h, i;

	if (!g_site)
		return 0xFFFFFFFFu;
	if (!key)
		key = 1;
	h = key & GH_SITE_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_SITE_MASK;
		LONG was = InterlockedCompareExchange(&g_site[s].key, (LONG)key, 0);

		if (was == 0) {
			g_site[s].site = site;
			g_site[s].size = size;
			return (unsigned)(InterlockedIncrement(&g_site[s].next) - 1);
		}
		if (was == (LONG)key)
			return (unsigned)(InterlockedIncrement(&g_site[s].next) - 1);
	}
	InterlockedIncrement(&g_site_full);
	return 0xFFFFFFFFu;
}

static void gh_tag_put(const void *u, unsigned site, unsigned size, unsigned ord)
{
	unsigned h, i;

	if (!g_tag || !u)
		return;
	h = gh_mix((unsigned)(uintptr_t)u) & GH_TAG_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_TAG_MASK;
		LONG was = InterlockedCompareExchange(&g_tag[s].addr,
						      (LONG)(uintptr_t)u, 0);

		/* A tombstone is reusable: an address is live at most once, so
		 * there cannot be an entry for this one further down the chain. */
		if (was != 0) {
			was = InterlockedCompareExchange(&g_tag[s].addr,
							 (LONG)(uintptr_t)u,
							 GH_TAG_DEAD);
			if (was != GH_TAG_DEAD)
				continue;
		}
		g_tag[s].site = site;
		g_tag[s].size = size;
		g_tag[s].ord = ord;
		return;
	}
	InterlockedIncrement(&g_tag_full);
}

/* Reads the tag and retires the slot in one go, because every caller is either
 * freeing the block or moving it. Leaving the entry behind would let the next
 * allocation to land on this address inherit a name that belongs to a block
 * that no longer exists, which is worse than having no name at all. */
static int gh_tag_take(const void *u, unsigned *site, unsigned *size, unsigned *ord)
{
	unsigned h, i;

	if (!g_tag || !u)
		return 0;
	h = gh_mix((unsigned)(uintptr_t)u) & GH_TAG_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_TAG_MASK;
		LONG a = g_tag[s].addr;

		if (a == 0)
			return 0; /* chain ended: this block was never tagged */
		if (a != (LONG)(uintptr_t)u)
			continue; /* including tombstones, which do not end it */
		*site = g_tag[s].site;
		*size = g_tag[s].size;
		*ord = g_tag[s].ord;
		InterlockedExchange(&g_tag[s].addr, GH_TAG_DEAD);
		return 1;
	}
	/* Reaching here means the probe ran out without meeting an empty slot,
	 * which is the table being too congested to search rather than the block
	 * being unknown - a different thing from the chain simply ending, and the
	 * one that means the numbers are degrading. Counted separately for that
	 * reason: walking off the end of a chain is normal and happens on every
	 * realloc that moved a block, since the tag was already taken. */
	InterlockedIncrement(&g_tag_congested);
	return 0;
}

/* Busy-block sidetable: heapwalk without invoking the heap.
 *
 * HeapWalk / HEAP_SEGMENT is a closed door on Wine. Scanning the saved
 * image for GhHead magic is a walk of the heap's bytes, not of the live
 * set: one session found 2 blocks against hundreds live, because the
 * scan jumped by aligned size and missed. We already intercept every
 * game malloc and free. The live (user, size) pairs are therefore a
 * thing we already know, kept in a table that is not itself on the heap.
 *
 * The live table is present-tense. A snapshot is taken after suspend_all
 * at save, packed into a second excluded array, and that is what restore
 * walks. After the copy, the live table is rewound to the snapshot so
 * free/realloc see the set the game believes is live.
 *
 * Same constraints as the tags: this runs inside the game's malloc, so
 * it must not allocate, must not lock, and must not touch a file. */
#define GH_BUSY_SLOTS 16384u
#define GH_BUSY_MASK (GH_BUSY_SLOTS - 1u)
#define GH_BUSY_DEAD ((LONG)1)

typedef struct {
	volatile LONG addr; /* 0 empty, 1 tomb, else the user pointer */
	size_t size;
} GhBusy;

typedef struct {
	void *head;
	size_t total;
} GhBusySnap;

static GhBusy *g_busy;
static volatile LONG g_busy_n, g_busy_full, g_busy_congested;
static GhBusySnap *g_busy_snap;
static unsigned g_busy_snap_n, g_busy_snap_cap;

static void gh_busy_put(const void *u, size_t n)
{
	unsigned h, i;

	if (!g_busy || !u)
		return;
	h = gh_mix((unsigned)(uintptr_t)u) & GH_BUSY_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_BUSY_MASK;
		LONG was = InterlockedCompareExchange(&g_busy[s].addr,
						      (LONG)(uintptr_t)u, 0);

		if (was != 0) {
			was = InterlockedCompareExchange(&g_busy[s].addr,
							 (LONG)(uintptr_t)u,
							 GH_BUSY_DEAD);
			if (was != GH_BUSY_DEAD)
				continue;
		}
		g_busy[s].size = n;
		InterlockedIncrement(&g_busy_n);
		return;
	}
	InterlockedIncrement(&g_busy_full);
}

static int gh_busy_take(const void *u)
{
	unsigned h, i;

	if (!g_busy || !u)
		return 0;
	h = gh_mix((unsigned)(uintptr_t)u) & GH_BUSY_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_BUSY_MASK;
		LONG a = g_busy[s].addr;

		if (a == 0)
			return 0;
		if (a != (LONG)(uintptr_t)u)
			continue;
		InterlockedExchange(&g_busy[s].addr, GH_BUSY_DEAD);
		InterlockedDecrement(&g_busy_n);
		return 1;
	}
	InterlockedIncrement(&g_busy_congested);
	return 0;
}

typedef void *(__cdecl *PFN_malloc)(size_t);
typedef void *(__cdecl *PFN_calloc)(size_t, size_t);
typedef void *(__cdecl *PFN_realloc)(void *, size_t);
typedef void(__cdecl *PFN_free)(void *);
typedef size_t(__cdecl *PFN_msize)(void *);
typedef void *(__cdecl *PFN_recalloc)(void *, size_t, size_t);
typedef void *(__cdecl *PFN_expand)(void *, size_t);

static PFN_malloc r_malloc;
static PFN_calloc r_calloc;
static PFN_realloc r_realloc;
static PFN_free r_free;
static PFN_msize r_msize;
static PFN_recalloc r_recalloc;
static PFN_expand r_expand;

/* Address only, and deliberately no dereference: this runs on every free in the
 * process, including pointers that were never ours. */
static int in_range(const void *p)
{
	uintptr_t a = (uintptr_t)p;
	LONG n = g_nreg;
	LONG i;

	for (i = 0; i < n; i++)
		if (a >= g_lo[i] && a < g_hi[i])
			return 1;
	return 0;
}

/* True while a pointer lives in the D3D9SW_LAA_FALSE region, so a realloc lands
 * on the heap that actually owns the block instead of the arena. */
static int in_laa(const void *p)
{
	uintptr_t a = (uintptr_t)p;

	return g_laa_hi && a >= g_laa_lo && a < g_laa_hi;
}

/* Where the redirect and the floor put a NEW block: the LAA_FALSE region when
 * that experiment is on, otherwise the pinned arena. A realloc must not use this
 * - it has to reuse the heap the existing block came from (see in_laa). */
static HANDLE gh_serving_heap(void)
{
	return (g_laa_on && g_laa_heap) ? g_laa_heap : g_heap;
}

/* The whole reservation the block sits in, so one lookup covers a segment rather
 * than a page. Only ever called when a new allocation lands outside everything
 * we already know about, which after warm-up is almost never. */
static void note_region(const void *p)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t base, end;
	LONG n;

	if (in_range(p))
		return;
	EnterCriticalSection(&g_cs);
	if (in_range(p) || VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) ||
	    !mbi.AllocationBase) {
		LeaveCriticalSection(&g_cs);
		return;
	}
	base = (uintptr_t)mbi.AllocationBase;
	end = base;
	for (;;) {
		if (VirtualQuery((void *)end, &mbi, sizeof(mbi)) != sizeof(mbi))
			break;
		if ((uintptr_t)mbi.AllocationBase != base)
			break;
		end += mbi.RegionSize;
	}
	n = g_nreg;
	if (n < GH_REG && end > base) {
		g_lo[n] = base;
		g_hi[n] = end;
		/* Published last, so a reader that sees the count sees the bounds
		 * that go with it. */
		InterlockedIncrement(&g_nreg);
	} else if (n >= GH_REG) {
		g_regfull++;
	}
	LeaveCriticalSection(&g_cs);
}

/* The header, or nothing. Read only after in_range has agreed, so the memory is
 * inside a reservation our heap took and is safe to touch. The magic is keyed to
 * the pointer it precedes, so a stale range cannot launder somebody else's
 * block into ours. */
static GhHead *head_of(void *u)
{
	GhHead *h = (GhHead *)((char *)u - sizeof(GhHead));

	if (h->magic != (GH_MAGIC ^ (uintptr_t)u))
		return NULL;
	return h;
}

/* Two ways to not be ours, and they could not be further apart.
 *
 * A pointer outside every reservation we took belongs to somebody else and must
 * go back to the runtime untouched. A pointer INSIDE one of our reservations
 * whose header does not check out is a block we issued whose header has since
 * been overwritten - and handing that to the runtime means calling RtlFreeHeap
 * with the process heap's handle and an address the process heap never issued,
 * which is how a heap gets corrupted rather than how one gets freed.
 *
 * That is not hypothetical. A restore rewinds our heap wholesale, so a block the
 * game allocated after the save has, the moment the copy lands, a header from
 * whatever older thing occupied those bytes. The game frees it a few hundred
 * frames later - tearing down audio it thinks is still playing - the magic does
 * not match, and the block goes out through the runtime to a heap that never
 * owned it. The fault lands inside ntdll's free path reading an uncommitted page
 * in OUR arena, which reads as a Windows bug until you know where the pointer
 * came from.
 *
 * g_stale has been counting exactly this case since the day it was written, and
 * the report has been calling it "a hole worth reading about". It was.
 *
 * The caller learns which kind of miss it had so it can refuse rather than
 * forward. Split out here rather than re-tested at each site because in_range is
 * a linear walk and this runs on every free in the process. */
static GhHead *ours_why(void *u, int *orphan)
{
	GhHead *h;

	*orphan = 0;
	if (!u || !g_heap || !in_range(u))
		return NULL;
	h = head_of(u);
	if (!h) {
		g_stale++;
		*orphan = 1;
	}
	return h;
}

static void *give(void *raw, size_t n)
{
	GhHead *h = (GhHead *)raw;
	void *u = (char *)raw + sizeof(GhHead);

	h->magic = GH_MAGIC ^ (uintptr_t)u;
	h->size = n;
	note_region(u);
	gh_busy_put(u, n);
	g_alloc++;
	return u;
}

/* The detour targets are the thin wrappers below. The work is in the _at forms
 * so that gameheap's own internal calls - realloc moving a block, recalloc -
 * can pass the site they were given instead of naming a line in this file. */
static void *gh_malloc_at(size_t n, unsigned site)
{
	void *raw;

	if (!g_ready || n >= GH_BIG) {
		void *u = r_malloc(n);

		g_toobig += n >= GH_BIG;
		gh_trace(GH_TR_BIG, n, NULL);
		gh_watch(u, n, site, 0xFFFFFFFFu, "malloc-big");
		return u;
	}
	raw = HeapAlloc(gh_serving_heap(), 0, n + sizeof(GhHead));
	if (!raw) {
		g_fellback++;
		if (g_laa_on)
			g_laa_fell++;
		return r_malloc(n);
	}
	{
		void *u = give(raw, n);
		unsigned ord = gh_ordinal(site, (unsigned)n);

		gh_tag_put(u, site, (unsigned)n, ord);
		gh_trace_at(GH_TR_ALLOC, n, u, site, ord);
		gh_watch(u, n, site, ord, g_laa_on ? "malloc-laa" : "malloc-arena");
		return u;
	}
}

static void *gh_malloc(size_t n)
{
	return gh_malloc_at(n, gh_site_of(__builtin_return_address(0)));
}

static void *gh_calloc_at(size_t c, size_t s, unsigned site)
{
	size_t n = c * s;
	void *raw;

	if (c && n / c != s)
		return NULL;
	if (!g_ready || n >= GH_BIG) {
		void *u = r_calloc(c, s);

		g_toobig += n >= GH_BIG;
		gh_trace(GH_TR_BIG, n, NULL);
		gh_watch(u, n, site, 0xFFFFFFFFu, "calloc-big");
		return u;
	}
	raw = HeapAlloc(gh_serving_heap(), HEAP_ZERO_MEMORY, n + sizeof(GhHead));
	if (!raw) {
		g_fellback++;
		if (g_laa_on)
			g_laa_fell++;
		return r_calloc(c, s);
	}
	{
		void *u = give(raw, n);
		unsigned ord = gh_ordinal(site, (unsigned)n);

		gh_tag_put(u, site, (unsigned)n, ord);
		gh_trace_at(GH_TR_CALLOC, n, u, site, ord);
		gh_watch(u, n, site, ord, g_laa_on ? "calloc-laa" : "calloc-arena");
		return u;
	}
}

static void *gh_calloc(size_t c, size_t s)
{
	return gh_calloc_at(c, s, gh_site_of(__builtin_return_address(0)));
}

/* Leaking a block is a bounded cost. Corrupting the process heap is not, and it
 * is not even paid by the code that caused it - the damage surfaces later, in an
 * unrelated allocation, as a fault with nothing in its stack to explain itself.
 * So an orphan is dropped on the floor and counted, here and at every other
 * route out. The memory stays reserved in our arena, which is where it already
 * was and where nothing else will be given it. */
static unsigned long g_orphan_dropped;

static void gh_orphan_note(const char *what, void *p)
{
	if (!g_orphan_dropped++)
		ss_log("gameheap: %s of %p refused - the address is inside our arena "
		       "but its header is gone, so it is a block of ours whose head "
		       "was overwritten (a restore rewinding memory the game allocated "
		       "after the save does exactly this). Passing it to the runtime "
		       "would free it against a heap that never issued it. Dropped "
		       "instead; this leaks the block and keeps the heap intact\n",
		       what, p);
}

/* Defined with the allocation floor below. A dedicated-VirtualAlloc big block is
 * released here (and in gh_heapfree) before the arena logic runs: it is not in
 * the arena and must go back via VirtualFree, not HeapFree. Returns its size if
 * it was one of ours (and, with take, releases it). */
static unsigned long gh_va_lookup(void *u, int take);

static void gh_free(void *p)
{
	int orphan;
	GhHead *h;

	if (gh_va_lookup(p, 1))
		return;
	h = ours_why(p, &orphan);

	if (!h) {
		if (orphan) {
			gh_orphan_note("free", p);
			return;
		}
		if (p)
			g_freed_theirs++;
		if (p)
			gh_trace(GH_TR_OTHER, 0, NULL);
		r_free(p);
		return;
	}
	h->magic = 0;
	g_freed_ours++;
	{
		unsigned site = 0xFFFFFFFFu, size = 0, ord = 0xFFFFFFFFu;

		if (!gh_tag_take(p, &site, &size, &ord))
			site = ord = 0xFFFFFFFFu;
		gh_trace_at(GH_TR_FREE, h->size, p, site, ord);
	}
	gh_peek_free(p);
	gh_vorbis_free(p, (unsigned)h->size);
	gh_busy_take(p);
	/* Free against the heap that ISSUED the block. Under LAA_FALSE a block can
	 * live in the region rather than the arena, and freeing it against the wrong
	 * heap corrupts that heap's free list. in_laa is always false when the
	 * experiment is off, so the normal path is unchanged. */
	HeapFree(in_laa(p) ? g_laa_heap : g_heap, 0, h);
}

static void *gh_realloc_at(void *p, size_t n, unsigned site)
{
	int orphan;
	GhHead *h = ours_why(p, &orphan);
	void *raw, *q;
	size_t old;
	/* A realloc does not create an object, it resizes one, so the block keeps
	 * the name it was given when it was first asked for. Minting a fresh tag
	 * here would make one logical block look like two different ones across
	 * sessions that happened to grow it at different moments. */
	unsigned osite = 0xFFFFFFFFu, osize = 0, oord = 0xFFFFFFFFu;
	int named = 0;

	if (!p)
		return gh_malloc_at(n, site);
	if (!h) {
		if (orphan) {
			/* No header means no size, so there is nothing to copy
			 * forward. A fresh block of the size asked for is the only
			 * answer available, and it is the same answer the texture
			 * path already gives for a payload it cannot read back:
			 * rebuild empty rather than invent contents. */
			gh_orphan_note("realloc", p);
			return gh_malloc_at(n, site);
		}
		/* Not ours, so it stays where it is. Growing it into our heap
		 * would mean copying a block whose true size only the runtime
		 * knows. */
		return r_realloc(p, n);
	}
	if (!n) {
		gh_free(p);
		return NULL;
	}
	old = h->size;
	named = gh_tag_take(p, &osite, &osize, &oord);
	if (n < GH_BIG) {
		/* Reuse the heap that issued this block, not the current serving heap:
		 * under LAA_FALSE an older arena block and a newer region block coexist,
		 * and reallocating one against the other's heap corrupts it. */
		raw = HeapReAlloc(in_laa(p) ? g_laa_heap : g_heap, 0, h,
				  n + sizeof(GhHead));
		if (raw)
			{
				void *u;

				gh_busy_take(p);
				u = give(raw, n);

				if (!named) {
					osite = site;
					oord = gh_ordinal(site, (unsigned)n);
				}
				gh_tag_put(u, osite, osize ? osize : (unsigned)n,
					   oord);
				gh_trace_at(GH_TR_REALLOC, n, u, osite, oord);
				gh_watch(u, n, osite, oord, "realloc-arena");
				return u;
			}
	}
	/* Either it outgrew what we keep, or the heap could not extend it. Move
	 * it out to the runtime rather than fail: a realloc that returns NULL
	 * without freeing is correct C and a leak in most callers. */
	q = n >= GH_BIG ? r_malloc(n) : gh_malloc_at(n, site);
	if (!q)
		return NULL;
	memcpy(q, p, old < n ? old : n);
	/* gh_malloc_at has just tagged q under this call site. Replace that with
	 * the name the block already had, so a move is invisible to identity. */
	if (named && n < GH_BIG) {
		unsigned d1, d2, d3;

		gh_tag_take(q, &d1, &d2, &d3);
		gh_tag_put(q, osite, osize, oord);
	}
	gh_free(p);
	return q;
}

static void *gh_realloc(void *p, size_t n)
{
	return gh_realloc_at(p, n, gh_site_of(__builtin_return_address(0)));
}

static size_t gh_msize(void *p)
{
	int orphan;
	GhHead *h = ours_why(p, &orphan);

	if (h)
		return h->size;
	if (orphan) {
		/* Asking the runtime would have it read a header in our arena as
		 * though it were one of its own. Zero is a poor answer and a safe
		 * one. */
		gh_orphan_note("_msize", p);
		return 0;
	}
	return r_msize(p);
}

static void *gh_recalloc(void *p, size_t c, size_t s)
{
	size_t n = c * s;
	int orphan;
	GhHead *h = ours_why(p, &orphan);
	size_t old;
	void *q;
	/* Taken here and passed down, so the tag names the game's call site rather
	 * than the line below that forwards to it. */
	unsigned site = gh_site_of(__builtin_return_address(0));

	if (c && n / c != s)
		return NULL;
	if (!p)
		return gh_calloc_at(c, s, site);
	if (!h) {
		if (orphan) {
			gh_orphan_note("_recalloc", p);
			return gh_calloc_at(c, s, site);
		}
		return r_recalloc(p, c, s);
	}
	old = h->size;
	q = gh_realloc_at(p, n, site);
	if (q && n > old)
		memset((char *)q + old, 0, n - old);
	return q;
}

static void *gh_expand(void *p, size_t n)
{
	int orphan;
	GhHead *h = ours_why(p, &orphan);
	void *raw;

	if (!h) {
		if (orphan) {
			/* _expand never moves a block, so refusing is a normal
			 * answer the runtime already documents. */
			gh_orphan_note("_expand", p);
			return NULL;
		}
		return r_expand(p, n);
	}
	if (n >= GH_BIG)
		return NULL;
	raw = HeapReAlloc(in_laa(p) ? g_laa_heap : g_heap, HEAP_REALLOC_IN_PLACE_ONLY,
			  h, n + sizeof(GhHead));
	if (!raw)
		return NULL;
	h->size = n;
	gh_busy_take(p);
	gh_busy_put(p, n);
	return p;
}

/* Where the allocator actually is.
 *
 * The premise above is wrong in one detail that turns out to decide the method.
 * Rabi-Ribi does not link ucrtbase; it links the runtime statically. Its import
 * list is KERNEL32, USER32, GDI32, SHELL32 and STEAM_API and nothing else, so
 * the ucrtbase in the process belongs to Windows and the game never calls it.
 * That is why redirecting imports found nothing to redirect: there is no import
 * to redirect. The conclusion the log drew - that the game's runtime heap comes
 * from ucrtbase - named the right heap for the wrong reason.
 *
 * The allocator is in the game's own .text, and Ghidra found the whole of it by
 * working back from the floor that every allocator has to reach. Five functions
 * touch the runtime's heap handle, and only five:
 *
 *   malloc   0036E6B2 -> HeapAlloc      54 callers
 *   calloc   0037EBF7 -> HeapAlloc       2 callers   (__calloc_impl)
 *   realloc  0036E744 -> HeapReAlloc    15 callers
 *   free     00369754 -> HeapFree      107 callers
 *   _msize   0037805A -> HeapSize        2 callers
 *
 * All five read the handle from one global at 0118E588, which is __acrt_heap. A
 * static UCRT sets it to GetProcessHeap(), and that single assignment is the
 * origin of every ownership heuristic in savestate.c.
 *
 * The three other free-shaped functions in the image were the reason to go
 * looking. None of them touches HeapFree; they tail-call this one. The set is
 * closed, which is the property that makes hooking it safe - a block allocated
 * through us and released through a routine we missed would go to HeapFree on a
 * heap it does not belong to.
 *
 * Nothing outside the runtime shares these. The four other Heap* callers in the
 * image ask GetProcessHeap for themselves and free what they themselves
 * allocated; they never see a pointer of ours.
 *
 * So we detour the game's private copies rather than redirecting an import.
 * That is better than the original plan, not a fallback from it: the copies
 * belong to the game alone, so the redirect cannot reach Windows even by
 * accident. Windows keeps ucrtbase; the game gets a heap with no other tenant.
 *
 * Tempting and rejected: write our heap handle into __acrt_heap and change no
 * code at all. Every allocation already live at that moment came from the
 * process heap, and the next free would hand it to HeapFree against a heap that
 * never issued it. The swap is only safe before the runtime initialises, and we
 * are loaded by LoadLibrary long after. Detours dispatch per pointer, so the
 * mixed period is correct by construction. */
#define GH_PRO 7

/* Seven bytes because five is the jump and the eighth would split an
 * instruction. Both shapes end on an instruction boundary and neither contains
 * a relative operand, so the bytes can be moved to a trampoline unchanged. */
static const unsigned char kProSaveEsi[GH_PRO] = { 0x55, 0x8B, 0xEC, 0x56,
						   0x8B, 0x75, 0x08 };
static const unsigned char kProTestArg[GH_PRO] = { 0x55, 0x8B, 0xEC, 0x83,
						   0x7D, 0x08, 0x00 };

struct Site {
	const char *name;
	unsigned rva; /* 0 where this target has no such function */
	const unsigned char *pro;
	void **real;
	void *ours;
};

/* Deliberately not the aligned family, and not _strdup.
 *
 * _aligned_malloc and _aligned_free are a closed pair: leave both alone and
 * every aligned block is allocated and released by the same allocator, which is
 * self-consistent. Hook one and not the other and it is not. Aligning by hand on
 * our own heap is possible but it is more surface than the state it would buy.
 *
 * _strdup allocates through malloc, so its blocks arrive here on their own.
 *
 * _recalloc and _expand carry no RVA because this executable does not contain
 * them. They stay in the table so the log says we looked rather than leaving the
 * reader to wonder, and so their handlers are not quietly dropped if a later
 * target does have them. */
static struct Site g_sites[] = {
	{ "malloc", 0x0036E6B2, kProSaveEsi, (void **)&r_malloc, NULL },
	{ "calloc", 0x0037EBF7, kProSaveEsi, (void **)&r_calloc, NULL },
	{ "realloc", 0x0036E744, kProTestArg, (void **)&r_realloc, NULL },
	{ "free", 0x00369754, kProTestArg, (void **)&r_free, NULL },
	{ "_msize", 0x0037805A, kProTestArg, (void **)&r_msize, NULL },
	{ "_recalloc", 0, NULL, (void **)&r_recalloc, NULL },
	{ "_expand", 0, NULL, (void **)&r_expand, NULL }
};

#define GH_SITES ((int)(sizeof(g_sites) / sizeof(g_sites[0])))

/* The bytes have to be the ones we were promised.
 *
 * An RVA is only meaningful against the build it was read from. Point it at a
 * patched executable, a different version, or a target that is not this game at
 * all, and five bytes of jump land in the middle of an instruction - which does
 * not fail, it misbehaves. So every site is checked against the prologue Ghidra
 * recorded, and one mismatch stops the whole install. Patching four of five is
 * worse than patching none. */
static int site_ok(HMODULE exe, const struct Site *s)
{
	const unsigned char *t = (const unsigned char *)exe + s->rva;
	MEMORY_BASIC_INFORMATION mbi;

	if (!VirtualQuery(t, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
		return 0;
	return memcmp(t, s->pro, GH_PRO) == 0;
}

/* The displaced bytes, then a jump back to what follows them. Built for every
 * site before any target is written, because the moment the first jump goes in
 * another thread can arrive in our free and needs the real one to forward to. */
static void site_tramp(HMODULE exe, struct Site *s, unsigned char *tr)
{
	unsigned char *t = (unsigned char *)exe + s->rva;

	memcpy(tr, t, GH_PRO);
	tr[GH_PRO] = 0xE9;
	*(LONG *)(tr + GH_PRO + 1) = (LONG)(t + GH_PRO - (tr + GH_PRO + 5));
	*s->real = tr;
}

static int site_wire(HMODULE exe, struct Site *s)
{
	unsigned char *t = (unsigned char *)exe + s->rva;
	DWORD old;

	if (!VirtualProtect(t, GH_PRO, PAGE_EXECUTE_READWRITE, &old))
		return 0;
	t[0] = 0xE9;
	*(LONG *)(t + 1) = (LONG)((unsigned char *)s->ours - (t + 5));
	/* The two bytes the jump does not cover. Never executed, because nothing
	 * branches between a function's entry and its fourth byte, but a decoder
	 * reading them should see one instruction rather than half of one. */
	t[5] = 0x90;
	t[6] = 0x90;
	VirtualProtect(t, GH_PRO, old, &old);
	FlushInstructionCache(GetCurrentProcess(), t, GH_PRO);
	return 1;
}

/* The floor, under everything.
 *
 * The five detours cover the runtime's allocator, and the runtime's allocator
 * is not the only thing in this executable that frees memory. Three other
 * functions call HeapFree against GetProcessHeap() directly - a custom pair and
 * a teardown path - and they are not allocators we can replace: they take no
 * pointer argument, they read globals and release what they computed. There is
 * no entry point to detour.
 *
 * Whether any of them can ever be handed a block of ours is a question two
 * design notes wanted settled before arming anything. It does not need
 * settling, because it can be made moot. Every one of them reaches HeapFree
 * through a single import slot, and so does the runtime's own free. One patch
 * there puts an ownership check underneath every freer in the process,
 * including the ones nobody has identified.
 *
 * A count of zero here across a long session is the measurement those notes
 * asked for. A count above zero is a wrong-heap free that would have been
 * silent corruption. Either answer is worth having; only one of them needed
 * code to prevent. */
static BOOL(WINAPI *r_heapfree)(HANDLE, DWORD, LPVOID);
static unsigned long g_caught;

/* Defined with the log-only allocation floor below; used here so a forwarded
 * free can subtract the block from the floor's live-footprint tally. */
static int g_floor_on;
static int floor_live_take(void *p);

static BOOL WINAPI gh_heapfree(HANDLE heap, DWORD flags, LPVOID p)
{
	GhHead *h;

	int orphan = 0;

	if (gh_va_lookup(p, 1))
		return TRUE;
	/* Our own HeapFree calls do not come back through here - those go direct
	 * to the real one - so this only ever sees the game's. */
	if (g_ready && (h = ours_why(p, &orphan)) != NULL) {
		h->magic = 0;
		g_caught++;
		g_freed_ours++;
		gh_peek_free(p);
		gh_vorbis_free(p, (unsigned)h->size);
		gh_busy_take(p);
		return HeapFree(in_laa(p) ? g_laa_heap : g_heap, flags, h);
	}
	/* The floor has to hold for the orphan case too, and this is the route the
	 * observed crash actually took: RtlFreeHeap called with the process heap's
	 * handle and an address inside our arena. */
	if (orphan) {
		gh_orphan_note("HeapFree", p);
		return TRUE;
	}
	if (g_floor_on)
		floor_live_take(p);
	return r_heapfree(heap, flags, p);
}

/* ---- allocation floor, log-only (D3D9SW_GHFLOOR=1) --------------------------
 *
 * The seven detours above own the executable's static CRT, so the game's own
 * malloc/free live in the pinned arena and rewind with it. Anything the game
 * reaches by another route - a direct HeapAlloc, or the system CRT that
 * ucrtbase routes to GetProcessHeap - lands on the process heap, which we do
 * not own. Those are the blocks a restore cannot keep in step: the game's
 * pointer to one rewinds to the save while the process-heap block has moved on,
 * and the next free walks metadata that no longer matches. The RtlFreeHeap
 * fault a few hundred frames after a BGM swap is exactly this.
 *
 * Before deciding what to redirect, name what escapes. This forwards HeapAlloc
 * and HeapReAlloc to the real functions UNCHANGED - it never redirects, so it
 * cannot move the baseline - and tallies, by the game call site, every result
 * that lands outside the arena. The report prints the finite list. That outline
 * is the whole point: it says whether owning the rest is a short list or a long
 * one, and it is the same divergence set a cross-session restore would have to
 * reconstruct. */
#define GH_FLOOR_SITES 256

static struct {
	void *ret;           /* the game call site that allocated off-arena */
	volatile LONG count;
	unsigned long bytes; /* cumulative requested; summed without a lock */
} g_floor[GH_FLOOR_SITES];
static volatile LONG g_floor_n;
static volatile LONG g_floor_over; /* escapes seen after the table filled */
static volatile LONG g_floor_a_calls, g_floor_r_calls; /* HeapAlloc vs HeapReAlloc */
static int g_floor_redirect; /* GHFLOOR>=2: serve escapes from the arena, not just log */
static int g_floor_va;       /* GHFLOOR>=3: big blocks on dedicated VirtualAlloc.
                              * EXPERIMENTAL and known-broken across >1 save: a freed
                              * VA reservation's address is reused, which the snapshot
                              * cannot capture atomically the way it does the arena. */

static LPVOID(WINAPI *r_floor_halloc)(HANDLE, DWORD, SIZE_T);
static LPVOID(WINAPI *r_floor_ralloc)(HANDLE, DWORD, LPVOID, SIZE_T);

/* Live-footprint map: address -> KB, so PEAK concurrent off-arena bytes can be
 * reported. That number, not the cumulative sum, decides whether owning this
 * site fits the 64 MB arena or needs it grown. Open-addressed with a tombstone
 * so deletions do not break probe chains; the live set stays small when the site
 * is a decode treadmill, which is the case we expect. Held in this DLL's data,
 * which is left in the present, so it is not disturbed by a rewind. */
#define GH_FLOOR_MAP 16384
#define GH_FLOOR_TOMB ((void *)~(uintptr_t)0)
static CRITICAL_SECTION g_floor_cs;
static int g_floor_cs_ready;
static struct {
	void *p;
	unsigned long kb;
} g_floor_map[GH_FLOOR_MAP];
static unsigned long g_floor_live_kb, g_floor_peak_kb;
static unsigned g_pin_mb; /* the pinned arena size, set when the heap is created */

static unsigned long gh_kb(size_t n)
{
	return (unsigned long)((n + 1023) >> 10);
}

static void floor_live_add(void *p, size_t n)
{
	unsigned h, i;

	if (!g_floor_cs_ready || !p)
		return;
	EnterCriticalSection(&g_floor_cs);
	h = (unsigned)(((uintptr_t)p >> 4) & (GH_FLOOR_MAP - 1));
	for (i = 0; i < GH_FLOOR_MAP; i++) {
		unsigned s = (h + i) & (GH_FLOOR_MAP - 1);

		if (!g_floor_map[s].p || g_floor_map[s].p == GH_FLOOR_TOMB) {
			g_floor_map[s].p = p;
			g_floor_map[s].kb = gh_kb(n);
			g_floor_live_kb += g_floor_map[s].kb;
			if (g_floor_live_kb > g_floor_peak_kb)
				g_floor_peak_kb = g_floor_live_kb;
			break;
		}
	}
	LeaveCriticalSection(&g_floor_cs);
}

/* Nonzero if the address was one of ours, so a forwarded free knows whether it
 * was a floor block. */
static int floor_live_take(void *p)
{
	unsigned h, i;
	int found = 0;

	if (!g_floor_cs_ready || !p)
		return 0;
	EnterCriticalSection(&g_floor_cs);
	h = (unsigned)(((uintptr_t)p >> 4) & (GH_FLOOR_MAP - 1));
	for (i = 0; i < GH_FLOOR_MAP; i++) {
		unsigned s = (h + i) & (GH_FLOOR_MAP - 1);

		if (!g_floor_map[s].p)
			break; /* empty slot ends the probe: not ours */
		if (g_floor_map[s].p == p) {
			if (g_floor_live_kb >= g_floor_map[s].kb)
				g_floor_live_kb -= g_floor_map[s].kb;
			g_floor_map[s].p = GH_FLOOR_TOMB;
			g_floor_map[s].kb = 0;
			found = 1;
			break;
		}
	}
	LeaveCriticalSection(&g_floor_cs);
	return found;
}

/* ---- dedicated VirtualAlloc for big floor blocks ---------------------------
 *
 * gh_floor_arena sends anything >= GH_BIG here instead of into the shared arena.
 * Each big block is its own VirtualAlloc reservation, so it never fragments the
 * fixed arena and never needs it grown - the arena then only ever holds small
 * blocks. The block is committed private memory, so a save captures it and it
 * rewinds with the game like any owned block; on free it is released with
 * VirtualFree, which - unlike HeapFree - reads no heap metadata and so cannot
 * fault on a block whose contents a restore rewound. Identified at free time by
 * a small present-tense registry rather than by the arena's in_range table, so
 * releasing one never leaves a stale range that could launder a later address
 * into ours. Blocks the game orphans across a restore leak their reservation -
 * a bounded, later-reclaimable cost, not a crash. */
#define GH_VA_MAP 4096
#define GH_VA_TOMB ((void *)~(uintptr_t)0)
static struct {
	void *p;
	unsigned long bytes;
} g_va_map[GH_VA_MAP];
static volatile LONG g_va_n; /* live big VA blocks */
static unsigned long g_va_total, g_va_live_kb, g_va_peak_kb;

static void gh_va_put(void *u, size_t bytes)
{
	unsigned hh, i;

	if (!g_floor_cs_ready || !u)
		return;
	EnterCriticalSection(&g_floor_cs);
	hh = (unsigned)(((uintptr_t)u >> 4) & (GH_VA_MAP - 1));
	for (i = 0; i < GH_VA_MAP; i++) {
		unsigned s = (hh + i) & (GH_VA_MAP - 1);

		if (!g_va_map[s].p || g_va_map[s].p == GH_VA_TOMB) {
			g_va_map[s].p = u;
			g_va_map[s].bytes = (unsigned long)bytes;
			InterlockedIncrement(&g_va_n);
			g_va_total++;
			g_va_live_kb += (unsigned long)((bytes + 1023) >> 10);
			if (g_va_live_kb > g_va_peak_kb)
				g_va_peak_kb = g_va_live_kb;
			break;
		}
	}
	LeaveCriticalSection(&g_floor_cs);
}

static unsigned long gh_va_lookup(void *u, int take)
{
	unsigned hh, i;
	unsigned long bytes = 0;

	if (!g_floor_cs_ready || !u || !g_va_n)
		return 0; /* no big blocks live: the common free pays nothing */
	EnterCriticalSection(&g_floor_cs);
	hh = (unsigned)(((uintptr_t)u >> 4) & (GH_VA_MAP - 1));
	for (i = 0; i < GH_VA_MAP; i++) {
		unsigned s = (hh + i) & (GH_VA_MAP - 1);

		if (!g_va_map[s].p)
			break; /* empty slot ends the probe: not one of ours */
		if (g_va_map[s].p == u) {
			bytes = g_va_map[s].bytes;
			if (take) {
				unsigned long kb = (bytes + 1023) >> 10;

				g_va_map[s].p = GH_VA_TOMB;
				g_va_map[s].bytes = 0;
				if (g_va_live_kb >= kb)
					g_va_live_kb -= kb;
				InterlockedDecrement(&g_va_n);
				VirtualFree((char *)u - sizeof(GhHead), 0, MEM_RELEASE);
			}
			break;
		}
	}
	LeaveCriticalSection(&g_floor_cs);
	return bytes;
}

static void *gh_va_alloc(size_t n, unsigned site)
{
	void *base = VirtualAlloc(NULL, sizeof(GhHead) + n, MEM_COMMIT | MEM_RESERVE,
				 PAGE_READWRITE);
	GhHead *h;
	void *u;

	(void)site;
	if (!base) {
		g_fellback++;
		return r_malloc(n); /* rare; escapes, which the caller then records */
	}
	h = (GhHead *)base;
	u = (char *)base + sizeof(GhHead);
	h->magic = GH_MAGIC ^ (uintptr_t)u; /* so a stray ours_why still reads sane */
	h->size = n;
	gh_va_put(u, n); /* VirtualAlloc already zeroed the pages, so zero-init is free */
	return u;
}

/* Per-site tally, plus feeding the live map. Keyed by call site, not by block,
 * so the table stays tiny; a race on first sight can duplicate a row, which the
 * report sums correctly either way. */
static void gh_floor_note(void *ret, void *p, size_t n)
{
	LONG i, seen;

	if (!p || in_range(p))
		return; /* already owned - not an escape */
	floor_live_add(p, n);
	seen = g_floor_n;
	for (i = 0; i < seen && i < GH_FLOOR_SITES; i++) {
		if (g_floor[i].ret == ret) {
			InterlockedIncrement(&g_floor[i].count);
			g_floor[i].bytes += (unsigned long)n;
			return;
		}
	}
	i = InterlockedIncrement(&g_floor_n) - 1;
	if (i >= GH_FLOOR_SITES) {
		InterlockedIncrement(&g_floor_over);
		return;
	}
	g_floor[i].ret = ret;
	g_floor[i].count = 1;
	g_floor[i].bytes = (unsigned long)n;
}

/* Arena allocation for the floor, WITHOUT the GH_BIG cap. gh_malloc_at sends
 * anything >= 256 KB back to the runtime, which for this site would leave the big
 * decode buffers - the very ones freed across a restore - on the process heap,
 * i.e. unfixed. The floor's point is to own them, so it takes them into the arena
 * at any size, falling back to the runtime only if the arena is genuinely out of
 * room, which the report then shows as a remaining escape (the signal to grow
 * D3D9SW_GHPIN_MB). give() stamps the same header the free paths already read, so
 * both HeapFree and static-CRT free reclaim these exactly like the game's own. */
static void *gh_floor_arena(size_t n, int zero, unsigned site)
{
	void *raw;

	if (!g_ready)
		return zero ? r_calloc(1, n) : r_malloc(n);
	/* Big blocks CAN get their own VirtualAlloc, released on free, so they neither
	 * fragment the fixed arena nor need it grown - but only under GHFLOOR>=3,
	 * because that path is not snapshot-safe across more than one save (a freed
	 * reservation's address is reused and the snapshot cannot track it). Default
	 * (GHFLOOR=2) keeps everything in the atomically-captured arena. */
	if (g_floor_va && n >= GH_BIG)
		return gh_va_alloc(n, site);
	raw = HeapAlloc(gh_serving_heap(), zero ? HEAP_ZERO_MEMORY : 0,
			n + sizeof(GhHead));
	if (!raw) {
		g_fellback++;
		if (g_laa_on)
			g_laa_fell++;
		return zero ? r_calloc(1, n) : r_malloc(n);
	}
	{
		void *u = give(raw, n);
		unsigned ord = gh_ordinal(site, (unsigned)n);

		gh_tag_put(u, site, (unsigned)n, ord);
		gh_trace_at(zero ? GH_TR_CALLOC : GH_TR_ALLOC, n, u, site, ord);
		gh_watch(u, n, site, ord, g_laa_on ? "floor-laa" : "floor-arena");
		return u;
	}
}

static LPVOID WINAPI gh_floor_heapalloc(HANDLE heap, DWORD flags, SIZE_T n)
{
	void *ret = __builtin_return_address(0);
	LPVOID p;

	/* Redirect (GHFLOOR>=2): serve from the pinned arena so the block rewinds
	 * with the game and its pointer stays in step across a restore. Reuses the
	 * same header/tag path as the game's own malloc, so both free routes - the
	 * HeapFree floor and the static-CRT free - already reclaim it. HEAP_ZERO_MEMORY
	 * is honoured. If the arena cannot take it (>= GH_BIG, or full) gh_*_at fall
	 * back to the runtime and the block escapes again, which the tally still
	 * records - so a resize need shows up instead of silently corrupting. */
	if (g_floor_redirect && g_ready) {
		unsigned site = gh_site_of(ret);

		p = gh_floor_arena(n, (flags & HEAP_ZERO_MEMORY) != 0, site);
		if (g_floor_on) {
			InterlockedIncrement(&g_floor_a_calls);
			/* Only a result that reached neither the arena nor a VA block is a
			 * real escape worth recording; owned blocks are not escapes. */
			if (p && !in_range(p) && !gh_va_lookup(p, 0))
				gh_floor_note(ret, p, n);
		}
		return p;
	}
	p = r_floor_halloc ? r_floor_halloc(heap, flags, n) : HeapAlloc(heap, flags, n);
	/* The block lands on whatever heap the game passed - foreign to us. This is
	 * the leaf where the 0ED6BEB0-class decoder object is born when it is NOT
	 * redirected, so the watch has to see it here, not just on the arena paths. */
	gh_watch(p, n, gh_site_of(ret), 0xFFFFFFFFu, "heap-fwd");
	if (g_floor_on) {
		InterlockedIncrement(&g_floor_a_calls);
		gh_floor_note(ret, p, n);
	}
	return p;
}

static LPVOID WINAPI gh_floor_heaprealloc(HANDLE heap, DWORD flags, LPVOID q, SIZE_T n)
{
	void *ret = __builtin_return_address(0);
	LPVOID p;

	/* A realloc of a dedicated VA block: this game never reallocs the floor's
	 * blocks, but never hand a VA pointer to the runtime's realloc either. Grow
	 * by hand - allocate the new size, copy, release the old reservation. */
	if (g_floor_va && q) {
		unsigned long vb = gh_va_lookup(q, 0);

		if (vb) {
			p = gh_floor_arena(n, 0, gh_site_of(ret));
			if (p) {
				CopyMemory(p, q, (size_t)vb < n ? (size_t)vb : n);
				gh_va_lookup(q, 1); /* release the old VA */
			}
			if (g_floor_on)
				InterlockedIncrement(&g_floor_r_calls);
			return p;
		}
	}
	/* Never hand an arena pointer to the runtime; in redirect mode serve a fresh
	 * block from the arena too. An arena q always goes through gh_realloc_at,
	 * whatever the mode, because forwarding it to the real heap would corrupt. */
	if (g_ready && ((q && in_range(q)) || (g_floor_redirect && !q))) {
		p = gh_realloc_at(q, n, gh_site_of(ret));
		if (g_floor_on) {
			InterlockedIncrement(&g_floor_r_calls);
			gh_floor_note(ret, p, n);
		}
		return p;
	}
	p = r_floor_ralloc ? r_floor_ralloc(heap, flags, q, n) : HeapReAlloc(heap, flags, q, n);
	gh_watch(p, n, gh_site_of(ret), 0xFFFFFFFFu, "heaprealloc-fwd");
	if (g_floor_on) {
		InterlockedIncrement(&g_floor_r_calls);
		/* Only on success: a failed realloc leaves the old block live. */
		if (p) {
			if (q)
				floor_live_take(q);
			gh_floor_note(ret, p, n);
		}
	}
	return p;
}

static int on(void)
{
	char v[8];
	DWORD n = savestate_getenv("D3D9SW_GAMEHEAP", v, sizeof(v));

	return n > 0 && n < sizeof(v) && v[0] == '1';
}

/* ------------------------------------------------ VirtualAlloc placement log
 *
 * The cross-session wall is not pointers - the module reloc pass shifts those -
 * and not threads, which the restore reports zero newer than the save. It is the
 * game's own large VirtualAlloc reservations: a dozen MEM_PRIVATE/RW regions,
 * ~340 MB, that no heap of ours claims and that Windows scatters to different
 * addresses every launch. A byte snapshot of one only restores if it comes back
 * at the same base - the very property the pinned arena gives the CRT heap, and
 * the reason a heap at a fixed base reproduces its whole layout.
 *
 * Before pinning any of them we have to know the placement is worth pinning:
 * that the sequence of big reservations - which call site, in what order, at
 * what size - is the SAME up to the save point across two fresh launches. If it
 * is, a deterministic (module, call site, ordinal) key can hand each region a
 * stable slot in a reserved pool. If it is not, this log is what says so, and
 * says what to key on instead.
 *
 * This changes nothing: every call is forwarded to the real VirtualAlloc. It
 * only records, and only for reservations at or above the threshold. Off unless
 * D3D9SW_VALOG=1; D3D9SW_VALOG_KB overrides the 1 MB floor. The return address
 * is resolved to its module so a reservation made from ucrtbase or the game's
 * mod hook is named by its own code, not misread as an executable offset. The
 * offset is module-relative, so it is the same across launches despite ASLR -
 * which is exactly the key a later pinning pass would use. */
static LPVOID (WINAPI *r_valloc)(LPVOID, SIZE_T, DWORD, DWORD);
static int g_valog;
static SIZE_T g_valog_min = 1024u * 1024u;
static volatile LONG g_valog_seq;
/* Reentrancy guard for the LOGGING path only, shared by both hooks. It is a
 * global interlocked try-lock, not a per-thread __thread latch: these hooks run
 * on early and loader threads that do not yet have this DLL's thread-local
 * storage set up, and touching a __thread there dereferences a null TLS array
 * and faults (a mov ecx,[ecx+eax*4] with ecx=0). The allocation itself is always
 * forwarded; only the logging - GetModuleFileNameA, ss_log - is fenced, and a
 * re-entrant or concurrent caller that finds the lock held simply forwards and
 * records nothing, which costs at most a missed log line. */
static volatile LONG g_hooklog;

static LPVOID WINAPI gh_valog(LPVOID addr, SIZE_T size, DWORD type, DWORD prot)
{
	void *ret = __builtin_return_address(0);
	LPVOID (WINAPI *real)(LPVOID, SIZE_T, DWORD, DWORD) =
		r_valloc ? r_valloc : VirtualAlloc;
	LPVOID p = real(addr, size, type, prot);

	if (g_valog && p && size >= g_valog_min && (type & MEM_RESERVE) &&
	    InterlockedCompareExchange(&g_hooklog, 1, 0) == 0) {
		HMODULE m = NULL;
		uintptr_t mb = 0, off = (uintptr_t)ret;
		char path[MAX_PATH], *name = (char *)"?";
		unsigned ord;
		long seq = InterlockedIncrement(&g_valog_seq) - 1;

		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				       (LPCSTR)ret, &m) && m) {
			char *s, *q;

			mb = (uintptr_t)m;
			off = (uintptr_t)ret - mb;
			if (GetModuleFileNameA(m, path, sizeof(path))) {
				for (s = path, q = path; *q; q++)
					if (*q == '\\' || *q == '/')
						s = q + 1;
				name = s;
			}
		}
		/* Keyed on (module-relative site, size), both stable across launches,
		 * so the same site produces the same ordinal run in two sessions - the
		 * thing a determinism diff compares. */
		ord = gh_ordinal((unsigned)off, (unsigned)size);
		ss_log("valog: #%ld  %-22s +%08lX ord %u  size %lu KB  "
		       "type 0x%X prot 0x%X  -> base %08lX%s\n",
		       seq, name, (unsigned long)off, ord,
		       (unsigned long)(size >> 10), (unsigned)type, (unsigned)prot,
		       (unsigned long)(uintptr_t)p,
		       addr ? " (caller-fixed base)" : "");
		InterlockedExchange(&g_hooklog, 0);
	}
	return p;
}

/* One layer down: NtAllocateVirtualMemory, the choke point every private
 * reservation passes through - VirtualAlloc, VirtualAllocEx, and any direct Nt
 * call all funnel here. The VirtualAlloc import net over 21 modules saw nothing,
 * so the game's big regions either resolve VirtualAlloc via GetProcAddress or
 * call Nt directly; either way they cross this line. Inline-hooked (the import
 * table is no use - nobody imports the ntdll stub), log-only, so it names who
 * allocates the 55-70 MB regions and becomes the point a pinning steer later
 * plugs into. D3D9SW_NTLOG=1. */
static LONG (WINAPI *r_ntav)(HANDLE, PVOID *, ULONG_PTR, PSIZE_T, ULONG, ULONG);
static USHORT (NTAPI *r_backtrace)(ULONG, ULONG, PVOID *, PULONG);
static int g_ntlog;
static volatile LONG g_ntstack_left = 12; /* full call-chain dumps for amdxx32 */

/* Basename of the module an address lands in, and the offset within it, into
 * buf. "?" when the address is in no module (heap, stack, jitted). */
static const char *gh_mod_of(const void *addr, char *buf, unsigned cap, uintptr_t *off)
{
	HMODULE m = NULL;

	*off = (uintptr_t)addr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			       (LPCSTR)addr, &m) &&
	    m) {
		char path[MAX_PATH], *s, *q;

		*off = (uintptr_t)addr - (uintptr_t)m;
		if (GetModuleFileNameA(m, path, sizeof(path))) {
			for (s = path, q = path; *q; q++)
				if (*q == '\\' || *q == '/')
					s = q + 1;
			lstrcpynA(buf, s, (int)cap);
			return buf;
		}
	}
	return "?";
}

static int gh_is_sys(const char *n)
{
	return !lstrcmpiA(n, "ntdll.dll") || !lstrcmpiA(n, "kernelbase.dll") ||
	       !lstrcmpiA(n, "kernel32.dll");
}

/* ---- our own VirtualAlloc regions, for the software-exclude path -----------
 *
 * When D3D9SW_SWEXCLUDE arms the Nt hooks, every reservation whose originator is
 * THIS DLL - the software renderer's textures, surfaces, framebuffers and
 * GPU-present staging - is recorded here and dropped when freed, so
 * build_exclusions can hold the live set in the present and out of the snapshot.
 * They are ours and regenerable: the game re-creates its resources and
 * gpu_dxgi_reconcile re-uploads the framebuffer after a restore.
 *
 * Open-addressed with the base as the claim word via CAS, so add and remove need
 * no lock the loader could be holding. The table lives in this DLL's data, which
 * is held in the present, so it survives a restore intact - the regions it names
 * are held in the present too. */
#define GH_OURVA_MAX 8192
static volatile LONG g_ourva[GH_OURVA_MAX]; /* base, 0 = free slot */
static SIZE_T g_ourva_size[GH_OURVA_MAX];
static volatile LONG g_ourva_hw;   /* high-water mark for iteration */
static volatile LONG g_ourva_full; /* adds dropped because the table was full */
static HMODULE g_ourmod;           /* this DLL (d3d11.dll) */
static int g_swexcl;

static void ourva_add(uintptr_t base, SIZE_T size)
{
	int i;

	if (!base)
		return;
	for (i = 0; i < GH_OURVA_MAX; i++) {
		if (g_ourva[i] == 0 &&
		    InterlockedCompareExchange(&g_ourva[i], (LONG)base, 0) == 0) {
			LONG hw;

			g_ourva_size[i] = size;
			do {
				hw = g_ourva_hw;
				if (i < hw)
					break;
			} while (InterlockedCompareExchange(&g_ourva_hw, i + 1, hw) != hw);
			return;
		}
	}
	InterlockedIncrement(&g_ourva_full);
}

static void ourva_del(uintptr_t base)
{
	int i, hw = g_ourva_hw;

	if (!base)
		return;
	for (i = 0; i < hw; i++)
		if ((uintptr_t)(LONG)g_ourva[i] == base) {
			InterlockedExchange(&g_ourva[i], 0);
			return;
		}
}

/* A subsystem inside this DLL that must be REWOUND with the game rather than held
 * in the present - the software audio, whose voice and PCM buffers seam into a
 * malformed stream if they stay present-tense while the game's audio state winds
 * back - calls this right after its VirtualAlloc to drop the region the ORIGIN
 * hook just recorded. Renderer memory (the reason SWEXCLUDE exists) is not
 * unheld; only the caller's own regions are. */
void gameheap_va_unhold(void *base)
{
	ourva_del((uintptr_t)base);
}

int gameheap_own_va_ranges(uintptr_t *base, uintptr_t *size, int max)
{
	int i, hw = g_ourva_hw, n = 0;

	for (i = 0; i < hw && n < max; i++) {
		uintptr_t b = (uintptr_t)(LONG)g_ourva[i];

		if (b) {
			base[n] = b;
			size[n] = (uintptr_t)g_ourva_size[i];
			n++;
		}
	}
	return n;
}

static LONG WINAPI gh_ntav(HANDLE proc, PVOID *base, ULONG_PTR zb, PSIZE_T size,
			   ULONG type, ULONG prot)
{
	void *ret = __builtin_return_address(0);
	/* Forward first, always, on any thread - no thread-local touched here, so a
	 * loader thread without our TLS cannot fault (the __thread version did). */
	LONG st = r_ntav(proc, base, zb, size, type, prot);
	SIZE_T got = size ? *size : 0;
	void *rbase;
	void *bt[32];
	USHORT nfr = 0;
	char onm[64] = "?";
	uintptr_t ooff = 0;
	int ours = 0;

	/* Only a successful self reservation is ours to track or log. */
	if ((!g_ntlog && !g_swexcl) || st < 0 || proc != (HANDLE)(LONG_PTR)-1 ||
	    !(type & MEM_RESERVE))
		return st;
	rbase = base ? *base : NULL;

	/* Originator: walk past the system layers (this stub, ntdll, kernelbase's
	 * VirtualAllocEx, kernel32) to the first frame in real code. That module is
	 * whose allocation this is - our renderer (hold it in the present) or the
	 * game / Steam (leave it). Needed for both the log and the exclude, so it
	 * runs outside the log lock. The whole frame array is kept so the log can
	 * dump the full chain when it is hunting who pulls a driver in. */
	if (r_backtrace) {
		int fi;

		nfr = r_backtrace(1, 32, bt, NULL);
		for (fi = 0; fi < nfr; fi++) {
			char tn[64];
			uintptr_t to;
			HMODULE fm = NULL;
			const char *tnm = gh_mod_of(bt[fi], tn, sizeof(tn), &to);

			if (tnm[0] == '?' || gh_is_sys(tnm))
				continue;
			lstrcpynA(onm, tnm, sizeof(onm));
			ooff = to;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					   (LPCSTR)bt[fi], &fm);
			ours = (fm && fm == g_ourmod);
			break;
		}
	}

	/* Record ours for exclusion - any size, no scope floor, lock-free table. */
	if (g_swexcl && ours && rbase)
		ourva_add((uintptr_t)rbase, got);

	if (g_ntlog && got >= g_valog_min &&
	    InterlockedCompareExchange(&g_hooklog, 1, 0) == 0) {
		char inm[64];
		uintptr_t ioff;
		const char *iname = gh_mod_of(ret, inm, sizeof(inm), &ioff);
		long seq = InterlockedIncrement(&g_valog_seq) - 1;

		ss_log("ntlog: #%ld  via %s+%lX  ORIGIN %s+%lX  size %lu KB  type 0x%X "
		       "prot 0x%X  -> base %08lX%s\n",
		       seq, iname, (unsigned long)ioff, onm, (unsigned long)ooff,
		       (unsigned long)(got >> 10), (unsigned)type, (unsigned)prot,
		       (unsigned long)(uintptr_t)rbase, ours ? "  [OURS->held]" : "");
		/* The whole call chain for the driver we are hunting, a few times, so
		 * the frame ABOVE the amdxx32 frames names who actually pulled it in
		 * (our code, the game, DXGI, DWM). Capped so the log stays readable. */
		if (!lstrcmpiA(onm, "amdxx32.dll") && g_ntstack_left > 0 &&
		    InterlockedDecrement(&g_ntstack_left) >= 0) {
			int fi;

			ss_log("  ntstack #%ld - who pulled in amdxx32:\n", seq);
			for (fi = 0; fi < nfr; fi++) {
				char tn[80];
				uintptr_t to;
				const char *tnm = gh_mod_of(bt[fi], tn, sizeof(tn), &to);

				ss_log("      [%2d] %-22s +%lX\n", fi, tnm,
				       (unsigned long)to);
			}
		}
		InterlockedExchange(&g_hooklog, 0);
	}
	return st;
}

/* NtFreeVirtualMemory, so a released renderer region leaves the owned table and
 * cannot go stale - which would hold in the present an address the game may take
 * next. Base is read before the call, since MEM_RELEASE can clear it. Only self
 * releases matter; a decommit keeps the reservation. */
static LONG (WINAPI *r_ntfv)(HANDLE, PVOID *, PSIZE_T, ULONG);

static LONG WINAPI gh_ntfv(HANDLE proc, PVOID *base, PSIZE_T size, ULONG freetype)
{
	uintptr_t b = (g_swexcl && base && proc == (HANDLE)(LONG_PTR)-1 &&
		       (freetype & MEM_RELEASE))
			      ? (uintptr_t)*base
			      : 0;
	LONG st = r_ntfv(proc, base, size, freetype);

	if (b && st >= 0)
		ourva_del(b);
	return st;
}

/* Inline-detour one ntdll export. The x86 WOW64 stub opens with
 * `mov eax, <service number>` (B8 imm32, 5 bytes) - a whole, position-independent
 * instruction - so 5 bytes displace cleanly. Refuse anything else rather than
 * corrupt an unknown prologue. The displaced bytes plus a jump back become the
 * trampoline, returned in *realout for the hook to forward through. */
static int ntdll_inline_hook(const char *name, void *hookfn, void **realout)
{
	HMODULE nt = GetModuleHandleA("ntdll.dll");
	unsigned char *t = nt ? (unsigned char *)GetProcAddress(nt, name) : NULL;
	unsigned char *tr;
	DWORD old;

	*realout = NULL;
	if (!t) {
		ss_log("gameheap: NT hook - %s not found\n", name);
		return 0;
	}
	if (t[0] != 0xB8) {
		ss_log("gameheap: NT hook - %s stub at %p starts 0x%02X, not the expected "
		       "B8; not hooking\n",
		       name, (void *)t, t[0]);
		return 0;
	}
	tr = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
					   PAGE_EXECUTE_READWRITE);
	if (!tr) {
		ss_log("gameheap: NT hook - trampoline page refused for %s\n", name);
		return 0;
	}
	memcpy(tr, t, 5);     /* the displaced mov eax, SSN */
	tr[5] = 0xE9;         /* jmp back to the rest of the stub */
	*(LONG *)(tr + 6) = (LONG)((t + 5) - (tr + 10));
	*realout = tr;
	if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) {
		ss_log("gameheap: NT hook - could not make %s writable\n", name);
		VirtualFree(tr, 0, MEM_RELEASE);
		*realout = NULL;
		return 0;
	}
	t[0] = 0xE9; /* jmp hookfn */
	*(LONG *)(t + 1) = (LONG)((unsigned char *)hookfn - (t + 5));
	VirtualProtect(t, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), t, 5);
	ss_log("gameheap: NT hook - %s inline-detoured at %p, trampoline %p\n", name,
	       (void *)t, (void *)tr);
	return 1;
}

/* Arms the allocation hook (always) and, for the software-exclude path, the free
 * hook too. g_ntlog / g_swexcl must already be set so the detours behave the
 * moment they are wired. */
static int nt_hooks_install(int with_free)
{
	HMODULE nt = GetModuleHandleA("ntdll.dll");

	if (!nt)
		return 0;
	r_backtrace = (USHORT(NTAPI *)(ULONG, ULONG, PVOID *, PULONG))GetProcAddress(
		nt, "RtlCaptureStackBackTrace");
	/* This DLL, so the originator walk can tell our own reservations apart. */
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCSTR)&gh_ntav, &g_ourmod);
	if (!ntdll_inline_hook("NtAllocateVirtualMemory", (void *)gh_ntav,
			       (void **)&r_ntav)) {
		g_ntlog = g_swexcl = 0;
		return 0;
	}
	if (with_free) {
		if (!ntdll_inline_hook("NtFreeVirtualMemory", (void *)gh_ntfv,
				       (void **)&r_ntfv))
			ss_log("gameheap: SWEXCLUDE - free hook failed; held regions would "
			       "go stale on free, so exclusion is UNSAFE - disabling it\n");
		if (!r_ntfv)
			g_swexcl = 0; /* no free tracking -> do not record, to stay safe */
	}
	ss_log("gameheap: NT hooks armed - alloc%s, this DLL %p. %s\n",
	       (with_free && r_ntfv) ? "+free" : "", (void *)g_ourmod,
	       g_swexcl ? "SWEXCLUDE: renderer VirtualAlloc regions are held in the "
			  "present and rebuilt on restore"
		        : (g_ntlog ? "NTLOG: logging only" : "inactive"));
	return 1;
}

int gameheap_install(void)
{
	HMODULE exe = GetModuleHandleA(NULL);
	unsigned char *pool;
	int live = 0, bad = 0, wired = 0;
	int i;

	g_sites[0].ours = (void *)gh_malloc;
	g_sites[1].ours = (void *)gh_calloc;
	g_sites[2].ours = (void *)gh_realloc;
	g_sites[3].ours = (void *)gh_free;
	g_sites[4].ours = (void *)gh_msize;
	g_sites[5].ours = (void *)gh_recalloc;
	g_sites[6].ours = (void *)gh_expand;

	if (g_ready || !on())
		return 0;
	if (!exe)
		return 0;
	/* Before any allocation is served, because the free path reads the result
	 * and the block we most want to watch is freed during startup. */
	gh_peek_parse();
	gh_watch_parse();

	/* Look before touching anything. */
	for (i = 0; i < GH_SITES; i++) {
		if (!g_sites[i].rva)
			continue;
		live++;
		if (site_ok(exe, &g_sites[i]))
			continue;
		bad++;
		ss_log("gameheap: %-10s at +%08X does not begin with the bytes this "
		       "build was told to expect\n",
		       g_sites[i].name, g_sites[i].rva);
	}
	if (!live || bad) {
		ss_log("gameheap: %d of %d allocator site(s) did not match, so nothing "
		       "was touched. These offsets were read from one build of one "
		       "executable and mean nothing against another\n",
		       bad, live);
		return 0;
	}

	/* Executable, and ours, so nothing the game does can reclaim it. */
	pool = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
					     PAGE_EXECUTE_READWRITE);
	if (!pool) {
		ss_log("gameheap: no memory for trampolines, error %lu\n", GetLastError());
		return 0;
	}
	{
		char v[16];
		unsigned cap = savestate_getenv("D3D9SW_GHTRACE", v, sizeof(v));
		long want = 0;
		unsigned k;

		for (k = 0; k < cap && v[k] >= '0' && v[k] <= '9'; k++)
			want = want * 10 + (v[k] - '0');
		if (want > 0) {
			/* Reserved before the first allocation is served, because a
			 * trace that starts late starts after the layout it is meant
			 * to explain has already been decided. */
			g_tr = (GhTr *)VirtualAlloc(NULL, (SIZE_T)want * sizeof(GhTr),
						    MEM_COMMIT | MEM_RESERVE,
						    PAGE_READWRITE);
			g_tr_cap = g_tr ? (LONG)want : 0;
			ss_log("gameheap: tracing the first %ld allocator operation(s) "
			       "into gh_trace.txt%s\n",
			       want, g_tr ? "" : " - RESERVATION FAILED, tracing off");
		}
		/* Armed with the trace, and reserved just as early: a tag table
		 * that starts late cannot name the blocks allocated before it,
		 * and those are exactly the long-lived ones worth naming. */
		if (g_tr) {
			g_exe = (uintptr_t)GetModuleHandleA(NULL);
			g_tag = (GhTag *)VirtualAlloc(NULL,
						      GH_TAG_SLOTS * sizeof(GhTag),
						      MEM_COMMIT | MEM_RESERVE,
						      PAGE_READWRITE);
			g_site = (GhSite *)VirtualAlloc(NULL,
							GH_SITE_SLOTS * sizeof(GhSite),
							MEM_COMMIT | MEM_RESERVE,
							PAGE_READWRITE);
			ss_log("gameheap: naming blocks by (call site, ordinal) "
			       "against image base %08lX%s\n",
			       (unsigned long)g_exe,
			       (g_tag && g_site) ? ""
						 : " - TABLE RESERVATION FAILED, "
						   "blocks will be unnamed");
		}
	}
	g_heap = gh_create_heap();
	if (!g_heap) {
		ss_log("gameheap: HeapCreate failed, error %lu\n", GetLastError());
		VirtualFree(pool, 0, MEM_RELEASE);
		return 0;
	}
	gh_create_laa_region(); /* opt-in; no-op unless D3D9SW_LAA_FALSE is set */
	if (gh_wholesale()) {
		/* Before a single allocation, so the LFH never gets a foothold and every
		 * block this heap serves is placed by the standard front end whose state
		 * lives entirely inside the region we rewind. */
		gh_make_selfcontained(g_heap);
		if (g_laa_heap)
			gh_make_selfcontained(g_laa_heap);
		ss_log("gameheap: WHOLESALE - LFH disabled on the owned heap(s); the "
		       "savestate engine may now restore their whole region atomically, "
		       "freelist and all, instead of block by block\n");
	}
	InitializeCriticalSection(&g_cs);

	/* Always, not only under GHTRACE: the tag table names blocks for a
	 * diagnostic; this table IS the live set restore walks. Excluded so a
	 * rewind cannot take the journal away while it is being used to rewind
	 * the heap the journal describes. */
	g_busy = (GhBusy *)VirtualAlloc(NULL, GH_BUSY_SLOTS * sizeof(GhBusy),
					MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	g_busy_snap = (GhBusySnap *)VirtualAlloc(NULL,
						GH_BUSY_SLOTS * sizeof(GhBusySnap),
						MEM_COMMIT | MEM_RESERVE,
						PAGE_READWRITE);
	if (g_busy && g_busy_snap) {
		g_busy_snap_cap = GH_BUSY_SLOTS;
		savestate_exclude(g_busy, GH_BUSY_SLOTS * sizeof(GhBusy));
		savestate_exclude(g_busy_snap,
				  GH_BUSY_SLOTS * sizeof(GhBusySnap));
		ss_log("gameheap: busy-block sidetable at %p (%u slots), snapshot "
		       "at %p - Wine restore walks this instead of HeapWalk or a "
		       "linear GhHead scan\n",
		       (void *)g_busy, GH_BUSY_SLOTS, (void *)g_busy_snap);
	} else {
		if (g_busy) {
			VirtualFree(g_busy, 0, MEM_RELEASE);
			g_busy = NULL;
		}
		if (g_busy_snap) {
			VirtualFree(g_busy_snap, 0, MEM_RELEASE);
			g_busy_snap = NULL;
		}
		ss_log("gameheap: busy-block sidetable reservation failed - restore "
		       "falls back to scanning saved bytes for GhHead magic\n");
	}

	/* Every forwarding path complete before any jump exists, then ready, then
	 * the jumps. A call arriving midway through the last step reaches a
	 * handler that can already forward. */
	for (i = 0; i < GH_SITES; i++)
		if (g_sites[i].rva)
			site_tramp(exe, &g_sites[i], pool + (size_t)i * 16);
	g_ready = 1;
	for (i = 0; i < GH_SITES; i++) {
		if (!g_sites[i].rva)
			continue;
		if (site_wire(exe, &g_sites[i]))
			wired++;
		else
			ss_log("gameheap: %-10s could not be made writable\n",
			       g_sites[i].name);
	}
	{
		void *prev = NULL;
		/* By name. kernel32!HeapFree is a forwarder to ntdll!RtlFreeHeap,
		 * so the address GetProcAddress returns is not necessarily the one
		 * in the import slot - which is why matching by address found
		 * nothing and said so. */
		int n = savestate_patch_iat_named(exe, "KERNEL32.dll", "HeapFree",
						  (void *)gh_heapfree, &prev);

		if (!n)
			n = savestate_patch_iat_named(exe, NULL, "HeapFree",
						      (void *)gh_heapfree, &prev);
		r_heapfree = n ? (BOOL(WINAPI *)(HANDLE, DWORD, LPVOID))prev : NULL;
		ss_log("gameheap: HeapFree %d import slot(s) patched as a floor under "
		       "every freer in the executable, the three that are not "
		       "allocators included%s\n",
		       n, n ? "" : " - the census below cannot measure what it claims");
	}
	{
		char floorv[16];

		if (savestate_getenv("D3D9SW_GHFLOOR", floorv, sizeof(floorv)) > 0 &&
		    floorv[0] >= '1' && floorv[0] <= '9') {
			void *prev = NULL;
			int na, nr;

			InitializeCriticalSection(&g_floor_cs);
			g_floor_cs_ready = 1;
			g_floor_on = 1;
			g_floor_redirect = floorv[0] >= '2';
			g_floor_va = floorv[0] >= '3';
			na = savestate_patch_iat_named(exe, "KERNEL32.dll", "HeapAlloc",
						       (void *)gh_floor_heapalloc, &prev);
			if (!na)
				na = savestate_patch_iat_named(exe, NULL, "HeapAlloc",
							       (void *)gh_floor_heapalloc, &prev);
			r_floor_halloc = na ? (LPVOID(WINAPI *)(HANDLE, DWORD, SIZE_T))prev : NULL;
			prev = NULL;
			nr = savestate_patch_iat_named(exe, "KERNEL32.dll", "HeapReAlloc",
						       (void *)gh_floor_heaprealloc, &prev);
			if (!nr)
				nr = savestate_patch_iat_named(exe, NULL, "HeapReAlloc",
							       (void *)gh_floor_heaprealloc, &prev);
			r_floor_ralloc = nr ? (LPVOID(WINAPI *)(HANDLE, DWORD, LPVOID, SIZE_T))prev
					    : NULL;
			ss_log("gameheap: allocation floor %s%s - HeapAlloc %d slot(s), "
			       "HeapReAlloc %d slot(s) patched. %s\n",
			       g_floor_redirect ? "REDIRECT" : "LOG-ONLY",
			       g_floor_va ? " + VA-BIG (experimental, not >1-save safe)" : "",
			       na, nr,
			       g_floor_redirect
				       ? "off-arena HeapAlloc is served from the pinned arena; "
					 "escape count should fall to zero"
				       : "nothing is redirected; the outline prints in the report");
		}
	}
	{
		char vv[16];

		if (savestate_getenv("D3D9SW_VALOG", vv, sizeof(vv)) > 0 && vv[0] == '1') {
			/* Every loaded module's import of VirtualAlloc, not just the exe and
			 * the CRT: the first pass hooked those two and saw nothing, so the
			 * 55-70 MB regions a cross-session restore drops are placed from some
			 * other module (or below kernel32 entirely). Cast the net over all of
			 * them, minus our own shims, whose reservations we do not carry. If
			 * this still shows nothing, the path is GetProcAddress-resolved or a
			 * direct Nt call and the hook has to go one layer down. */
			static const char *const ours[] = {
				"d3d11.dll", "dxgi.dll", "xaudio2_9.dll", "xinput1_4.dll",
				"dsound.dll", "opengl32.dll", "d3d9.dll", "d3d9_sw.dll",
				/* The core allocators: hooking their VirtualAlloc import is
				 * both pointless (heap growth goes through
				 * NtAllocateVirtualMemory, not kernel32!VirtualAlloc, and
				 * these do not call their own export via an import) and
				 * hazardous - they run under the loader lock, where our
				 * logging path can deadlock. The game's own big allocations
				 * come from the exe and its libraries, never from here. */
				"ntdll.dll", "kernel32.dll", "kernelbase.dll"
			};
			char kb[16];
			int total = 0, hooked = 0, skipped = 0;
			HANDLE snap;

			if (savestate_getenv("D3D9SW_VALOG_KB", kb, sizeof(kb)) > 0) {
				unsigned k = 0, ci;

				for (ci = 0; kb[ci] >= '0' && kb[ci] <= '9'; ci++)
					k = k * 10u + (unsigned)(kb[ci] - '0');
				if (k)
					g_valog_min = (SIZE_T)k * 1024u;
			}
			snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
			if (snap != INVALID_HANDLE_VALUE) {
				MODULEENTRY32 me;

				me.dwSize = sizeof(me);
				if (Module32First(snap, &me)) {
					do {
						unsigned oi, mine = 0;
						void *prev = NULL;
						int n;

						for (oi = 0; oi < sizeof(ours) / sizeof(ours[0]); oi++)
							if (!lstrcmpiA(me.szModule, ours[oi])) {
								mine = 1;
								break;
							}
						if (mine) {
							skipped++;
							continue;
						}
						n = savestate_patch_iat_named((HMODULE)me.hModule,
									      "KERNEL32.dll",
									      "VirtualAlloc",
									      (void *)gh_valog, &prev);
						if (!n)
							n = savestate_patch_iat_named(
								(HMODULE)me.hModule, NULL,
								"VirtualAlloc", (void *)gh_valog,
								&prev);
						if (n && prev && !r_valloc)
							r_valloc = (LPVOID(WINAPI *)(
								LPVOID, SIZE_T, DWORD, DWORD))prev;
						if (n)
							hooked++;
						total += n;
					} while (Module32Next(snap, &me));
				}
				CloseHandle(snap);
			}
			g_valog = total > 0;
			ss_log("gameheap: VALOG %s - %d VirtualAlloc import slot(s) across %d "
			       "module(s) (%d of ours skipped), logging reservations >= %lu KB "
			       "by (module, call site, ordinal). Forwarded unchanged; only "
			       "records placement%s\n",
			       g_valog ? "ON" : "FAILED", total, hooked, skipped,
			       (unsigned long)(g_valog_min >> 10),
			       g_valog ? "" : " - no VirtualAlloc import found to patch");
		}
	}
	{
		char nl[8], sx[8];
		int ntlog = savestate_getenv("D3D9SW_NTLOG", nl, sizeof(nl)) > 0 &&
			    nl[0] == '1';
		int swx = savestate_getenv("D3D9SW_SWEXCLUDE", sx, sizeof(sx)) > 0 &&
			  sx[0] == '1';

		if (ntlog || swx) {
			g_ntlog = ntlog;
			g_swexcl = swx;
			nt_hooks_install(swx); /* alloc always; free hook only for exclude */
		}
	}
	clock_probe_install(exe);
	savestate_game_heap(g_heap);
	ss_log("gameheap: %d of %d allocator site(s) in the game's own code now run "
	       "on a private heap at %p. The runtime is linked statically, so these "
	       "are the game's copies and nothing Windows uses passes through them. "
	       "Allocations under %lu KB from here on have no other tenant, which is "
	       "the arrangement DDPR gets from MSVCR100 for free\n",
	       wired, live, (void *)g_heap, (unsigned long)(GH_BIG >> 10));
	return wired;
}

/* ------------------------------------------------- where the heap itself goes
 *
 * D3D9SW_GHPIN puts the private heap at an address we choose instead of one
 * Windows chooses.
 *
 * This is the change that makes a layout reproducible, and the evidence for it
 * is unusually direct. Two sessions were made to issue byte-identical
 * allocation sequences - same 2,133 operations, same order - and replayed
 * against four allocators. HeapCreate put 93% of the blocks at different
 * offsets both times. Disabling the low-fragmentation heap changed nothing,
 * which killed the obvious theory. RtlCreateHeap at a fixed base reproduced
 * every block, at every checkpoint, exactly.
 *
 * The only difference between those cases is the base, so the base is the
 * cause: HeapCreate has no address parameter, Windows picks one, and the
 * internal layout follows from it.
 *
 * Worth being clear about why an address matters when a snapshot could simply
 * record offsets. It is not the snapshot that needs the address, it is the
 * contents: the blocks are full of pointers to each other, and a pointer is
 * only meaningful against the layout it was written for. Restoring the bytes
 * faithfully into a heap that moved is how a restored pointer ends up aimed at
 * whatever now occupies that address.
 *
 * A caller-supplied base means the heap cannot grow - there is nothing after
 * the reservation it is entitled to take. That is deliberate. Peak live in this
 * heap has been measured at 0.47 MB and everything at or above 256 KB is handed
 * to the runtime anyway, so the reservation below is orders of magnitude clear;
 * and if it is ever wrong, gh_malloc already falls back to the runtime and
 * g_fellback counts it, so the failure is a report rather than a crash.
 */
/* 0x20000000, not 0x50000000: the startup probe (d3d11_sw arena_probe) measured
 * the largest contiguous free hole at each candidate base, and 0x20000000 has by
 * far the most room (~1.15 GB early vs ~0.4 GB at 0x50000000). At 128 MB either
 * base takes, but 0x50000000 could not fit a larger arena - the 512 MB attempt
 * there failed with error 487 and fell back to an OS-placed base, which is NOT
 * the same address next session and so breaks cross-session pointers. Pinning at
 * 0x20000000 keeps the fixed base for any arena size we are likely to want,
 * meaning the arena stays DETERMINISTIC across sessions and drops out of the
 * relocation problem entirely. The d3d11 texture arena is steered off this base
 * so the two do not fight for it. */
#define GH_PIN_BASE 0x20000000u

static PVOID(NTAPI *p_RtlCreateHeap)(ULONG, PVOID, SIZE_T, SIZE_T, PVOID, PVOID);

static unsigned gh_knob(const char *name, unsigned def)
{
	char v[24];
	unsigned n = savestate_getenv(name, v, sizeof(v));
	unsigned got = 0, k = 0;
	int hex = (n > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X'));

	if (!n)
		return def;
	for (k = hex ? 2 : 0; k < n; k++) {
		if (v[k] >= '0' && v[k] <= '9')
			got = got * (hex ? 16 : 10) + (unsigned)(v[k] - '0');
		else if (hex && v[k] >= 'a' && v[k] <= 'f')
			got = got * 16 + (unsigned)(v[k] - 'a') + 10;
		else if (hex && v[k] >= 'A' && v[k] <= 'F')
			got = got * 16 + (unsigned)(v[k] - 'A') + 10;
		else
			break;
	}
	return got;
}

static HANDLE gh_create_heap(void)
{
	unsigned want = gh_knob("D3D9SW_GHPIN", 0);
	unsigned mb = gh_knob("D3D9SW_GHPIN_MB", 64);
	uintptr_t base;
	SIZE_T size;
	void *res;
	HMODULE nt;
	HANDLE h;

	g_pin_mb = 0; /* set below only if the pin actually succeeds */
	if (!want)
		return HeapCreate(0, 1u << 20, 0);
	/* 1 means "pin it, you pick"; anything larger is an address. */
	base = (want == 1) ? GH_PIN_BASE : (uintptr_t)want;
	size = (SIZE_T)mb * 1024u * 1024u;

	nt = GetModuleHandleA("ntdll.dll");
	p_RtlCreateHeap = nt ? (PVOID(NTAPI *)(ULONG, PVOID, SIZE_T, SIZE_T, PVOID,
					       PVOID))GetProcAddress(nt, "RtlCreateHeap")
			     : NULL;
	if (!p_RtlCreateHeap) {
		ss_log("gameheap: RtlCreateHeap is not exported, so the heap cannot "
		       "be pinned - falling back to HeapCreate and a layout that "
		       "will not reproduce\n");
		return HeapCreate(0, 1u << 20, 0);
	}
	/* Reserved first and checked, rather than trusting RtlCreateHeap to honour
	 * the address: landing somewhere else silently would look like success and
	 * produce a session whose traces quietly do not compare. */
	res = VirtualAlloc((LPVOID)base, size, MEM_RESERVE, PAGE_READWRITE);
	if (!res || (uintptr_t)res != base) {
		DWORD err = GetLastError();

		if (res) {
			VirtualFree(res, 0, MEM_RELEASE);
			res = NULL;
		}
		/* The fixed base is routinely occupied - 0x50000000 loses to whatever ASLR
		 * or a module put there - and the old answer was a growable HeapCreate,
		 * which cannot be pinned OR wholesale-restored (its segments come and go
		 * outside any bounded region). An OS-placed reservation of the SAME size is
		 * just as ownable and just as wholesale-able; it only gives up the fixed
		 * base, which determinism wants but survival does not. So prefer it, and
		 * keep HeapCreate as the last resort only. This is what lets D3D9SW_WHOLESALE
		 * engage without D3D9SW_LAA_FALSE, since the arena is now always bounded. */
		res = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
		if (!res) {
			ss_log("gameheap: cannot reserve %u MB at %08lX (error %lu) nor "
			       "anywhere else (error %lu) - falling back to a growable "
			       "HeapCreate that cannot be wholesale-restored\n",
			       mb, (unsigned long)base, err, GetLastError());
			return HeapCreate(0, 1u << 20, 0);
		}
		ss_log("gameheap: could not pin %u MB at %08lX (error %lu); reserved it "
		       "OS-placed at %08lX instead - bounded and wholesale-able, just not "
		       "at the fixed base (determinism traces will not compare)\n",
		       mb, (unsigned long)base, err, (unsigned long)(uintptr_t)res);
	}
	h = (HANDLE)p_RtlCreateHeap(0, res, size, 1u << 20, NULL, NULL);
	if (!h) {
		ss_log("gameheap: RtlCreateHeap refused the base %08lX - falling back "
		       "to HeapCreate\n",
		       (unsigned long)base);
		VirtualFree(res, 0, MEM_RELEASE);
		return HeapCreate(0, 1u << 20, 0);
	}
	ss_log("gameheap: heap PINNED at %p, %u MB reserved, not growable. Two "
	       "sessions that issue the same allocations should now place every "
	       "block at the same address, not merely at the same set of "
	       "addresses\n",
	       (void *)h, mb);
	if ((uintptr_t)h != base)
		ss_log("gameheap: NOTE the handle %p is not the base %08lX we asked "
		       "for - offsets are measured from the handle\n",
		       (void *)h, (unsigned long)base);
	g_pin_mb = mb; /* the pin held; report the real, pinned size */
	g_heap_lo = (uintptr_t)h;
	g_heap_hi = (uintptr_t)h + size;
	return h;
}

/* D3D9SW_LAA_FALSE - the "own a region the game does not know about" experiment.
 *
 * Reserve a second block and, once on, the redirect and floor serve every new
 * allocation from it (gh_serving_heap), so the game runs out of memory we hold.
 * The point is to see whether a region we control, off to one side, can back the
 * game's own state without the game or Steam noticing - and, on a 32-bit non-LAA
 * exe, to find the wall: there is no space above 2 GB, so this competes with the
 * game's ~1.4 GB and is EXPECTED to hit an allocation failure or a crash. That is
 * the reading, not a bug. On an exe patched LAA (tools/laa.ps1) the >2 GB band
 * exists and we try to land there first, where the game never allocates.
 *
 * Called after gh_create_heap so RtlCreateHeap is resolved; it re-resolves if the
 * arena was a plain HeapCreate and never went down the pin path. */
static void gh_create_laa_region(void)
{
	unsigned mb = gh_knob("D3D9SW_LAA_MB", 512);
	SIZE_T size = (SIZE_T)mb * 1024u * 1024u;
	void *res;

	if (!gh_knob("D3D9SW_LAA_FALSE", 0))
		return;
	g_laa_mb = mb;
	if (!p_RtlCreateHeap) {
		HMODULE nt = GetModuleHandleA("ntdll.dll");

		p_RtlCreateHeap = nt ? (PVOID(NTAPI *)(ULONG, PVOID, SIZE_T, SIZE_T, PVOID,
						       PVOID))GetProcAddress(nt, "RtlCreateHeap")
				     : NULL;
	}
	if (!p_RtlCreateHeap) {
		ss_log("gameheap: LAA_FALSE asked for but RtlCreateHeap is unavailable\n");
		return;
	}
	/* The >2 GB band only exists if the exe is LAA; try it first so a patched exe
	 * puts our region where the game's own allocator never reaches. */
	res = VirtualAlloc((LPVOID)0x80000000u, size, MEM_RESERVE, PAGE_READWRITE);
	g_laa_high = (res != NULL);
	if (!res)
		res = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE); /* OS places it */
	if (!res) {
		ss_log("gameheap: LAA_FALSE could not reserve %u MB anywhere (error %lu) - "
		       "no hole this large exists; that is the 2 GB ceiling talking, and "
		       "is itself the answer\n",
		       mb, GetLastError());
		return;
	}
	g_laa_heap = (HANDLE)p_RtlCreateHeap(0, res, size, 1u << 20, NULL, NULL);
	if (!g_laa_heap) {
		ss_log("gameheap: LAA_FALSE reserved %u MB at %08lX but RtlCreateHeap "
		       "refused it (error %lu)\n",
		       mb, (unsigned long)(uintptr_t)res, GetLastError());
		VirtualFree(res, 0, MEM_RELEASE);
		return;
	}
	g_laa_lo = (uintptr_t)res;
	g_laa_hi = (uintptr_t)res + size;
	g_laa_on = 1;
	ss_log("gameheap: LAA_FALSE region PINNED at %08lX, %u MB, %s. Every "
	       "redirected allocation from here on comes out of it - memory we own "
	       "and the game does not know exists. Any crash past this line is the "
	       "experiment reporting, not a regression\n",
	       (unsigned long)(uintptr_t)res, mb,
	       g_laa_high ? "in the >2 GB band, so the exe really is LAA"
			  : "inside the shared 2 GB - it competes with the game");
}

/* D3D9SW_WHOLESALE - the owned-heap wholesale-restore probe.
 *
 * Block-by-block restore leaves the allocator's free lists alone on purpose,
 * because rewinding them on the process default heap desynchronises from the
 * allocator state that lives OUTSIDE the region (the LFH's per-processor buckets
 * above all) and kills the process. But a heap WE created can be made to keep all
 * of its bookkeeping inside its own region: turn the Low Fragmentation Heap off
 * and the free lists, headers and lookaside all live in the bytes we rewind. Then
 * the region can be restored whole - freelist and all - and a block the game
 * allocated after the save simply ceases to exist, which is the address-reuse
 * crash's actual cure. This is opt-in and separate from the working config. */
static int gh_wholesale(void)
{
	static int v = -1;

	if (v < 0)
		v = (int)gh_knob("D3D9SW_WHOLESALE", 0);
	return v;
}

static void gh_make_selfcontained(HANDLE h)
{
	ULONG std = 0; /* HeapCompatibilityInformation: 0 = standard heap, no LFH */

	if (h)
		HeapSetInformation(h, HeapCompatibilityInformation, &std, sizeof(std));
}

/* The owned-heap base anchors for a relocatable (cross-session) restore. A saved
 * pointer into the arena is base+offset with a STABLE offset (the whole point of
 * the pin), so cross-session it becomes new_base+offset - all the relocation pass
 * needs is the base recorded at save and the new base read here at restore.
 * Modules (the exe included) are anchored separately by the savestate module
 * table; this covers the non-module owned heaps. Stable order: arena, then the
 * LAA_FALSE region. Writes up to `max` {base,size} pairs, returns the count.
 * Ranges are reported ONLY when bounded (a pinned or OS-placed reservation) -
 * a growable HeapCreate fallback has no fixed extent to relocate against. */
int gameheap_anchor_ranges(uintptr_t *base, uintptr_t *size, int max)
{
	int n = 0;

	if (g_heap_lo && g_heap_hi && n < max) {
		base[n] = g_heap_lo;
		size[n] = g_heap_hi - g_heap_lo;
		n++;
	}
	if (g_laa_lo && g_laa_hi && n < max) {
		base[n] = g_laa_lo;
		size[n] = g_laa_hi - g_laa_lo;
		n++;
	}
	return n;
}

int gameheap_owned_heap(uintptr_t base)
{
	if (!gh_wholesale())
		return 0;
	/* Range, not equality: a heap's committed memory can surface as several
	 * VirtualQuery regions at different bases within its reservation, and every
	 * one of them must be restored whole for the freelist to come back intact. */
	if (g_heap_hi && base >= g_heap_lo && base < g_heap_hi)
		return 1;
	if (g_laa_hi && base >= g_laa_lo && base < g_laa_hi)
		return 1;
	return 0;
}

/* Written from the census rather than from the allocator, because this opens a
 * file and formats text and neither belongs on a path the game takes millions of
 * times. Rewritten in full each time rather than appended, so the file always
 * describes one run from its first allocation and two runs can be diffed
 * directly. */
static void gh_trace_write(const char *path)
{
	HANDLE f;
	char line[64];
	LONG n = g_tr_n, i;
	DWORD wrote = 0;

	if (!g_tr)
		return;
	if (n > g_tr_cap)
		n = g_tr_cap;
	f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		ss_log("gameheap: cannot write %s, error %lu\n", path, GetLastError());
		return;
	}
	/* The base goes in a comment rather than into the offsets, which are what a
	 * diff compares. Two runs with different bases and identical offset columns
	 * is the whole result. */
	wsprintfA(line, "# heap %08lX  %ld op(s)%s\r\n", (unsigned long)(uintptr_t)g_heap,
		  (long)n, (g_tr_n > g_tr_cap) ? "  TRUNCATED" : "");
	WriteFile(f, line, (DWORD)lstrlenA(line), &wrote, NULL);
	/* site is an RVA into the executable and ordinal is the count that site
	 * had already reached, so the pair names the block independently of where
	 * it landed. Readers that only want the first three columns still work:
	 * the extra two are appended, not inserted. */
	wsprintfA(line, "# image %08lX\r\n", (unsigned long)g_exe);
	WriteFile(f, line, (DWORD)lstrlenA(line), &wrote, NULL);
	WriteFile(f, "# op size offset site ordinal\r\n", 31, &wrote, NULL);
	for (i = 0; i < n; i++) {
		int k = wsprintfA(line, "%c %08lX %08lX %08lX %08lX\r\n",
				  (char)g_tr[i].op, (unsigned long)g_tr[i].size,
				  (unsigned long)g_tr[i].off,
				  (unsigned long)g_tr[i].site,
				  (unsigned long)g_tr[i].ord);
		WriteFile(f, line, (DWORD)k, &wrote, NULL);
	}
	CloseHandle(f);
	ss_log("gameheap: %s written, %ld of %ld operation(s)%s\n", path, (long)n,
	       (long)g_tr_n,
	       (g_tr_n > g_tr_cap) ? " - the buffer filled, raise D3D9SW_GHTRACE" : "");
	if (g_tag_full || g_site_full || g_tag_congested)
		ss_log("gameheap: %ld block(s) and %ld chute(s) went unnamed, %ld "
		       "lookup(s) gave up searching - the tag tables are too small "
		       "for a run this long, so names after that point are short\n",
		       (long)g_tag_full, (long)g_site_full, (long)g_tag_congested);
}

/* --------------------------------------------- where the session seed enters
 *
 * D3D9SW_CLOCKPROBE=1 reports every call the game makes to the three
 * calendar-time functions it imports, with the caller's RVA and - the part that
 * makes it useful - the allocator operation number at the time of the call.
 *
 * The question it answers. Two sessions from an identical save allocate
 * identically for 3,363 operations and then differ in the length of one string.
 * Something the game knows must therefore differ before that point while
 * leaving no trace in the heap until it is formatted, and a value read from the
 * calendar is the obvious candidate: it changes every run by construction, it
 * is usually stored in a fixed-size structure or a register rather than an
 * allocation, and it becomes visible exactly when somebody prints it.
 *
 * The operation number is what turns this from a list into evidence. Clock
 * calls and allocations are two streams with no common clock of their own; the
 * trace index is a counter both can be read against, so a call reported at
 * operation 3,300 is one that happened in the window where the divergence was
 * introduced, and a call at operation 7,000 is not.
 *
 * Only the calendar functions are hooked, not QueryPerformanceCounter or
 * timeGetTime. Those are read every frame and would bury the rare calls that
 * matter under thousands that do not - and a per-frame timer is a poor seed
 * anyway, being exactly what the game would use for animation.
 *
 * These are imports, so patching an import slot is enough. That is not true of
 * the allocator, which the game links statically and which had to be detoured
 * instruction by instruction. */
static void(WINAPI *r_GetLocalTime)(LPSYSTEMTIME);
static void(WINAPI *r_GetSystemTime)(LPSYSTEMTIME);
static void(WINAPI *r_GetSystemTimeAsFileTime)(LPFILETIME);
static volatile LONG g_clk_n;

static void clk_note(const char *what, void *ra)
{
	LONG i = InterlockedIncrement(&g_clk_n);
	uintptr_t a = (uintptr_t)ra;

	/* Bounded, because a call in a loop would otherwise fill the log with the
	 * one thing we already know. The first calls are the ones that can have
	 * seeded anything. */
	if (i > 48)
		return;
	if (g_exe && a >= g_exe)
		ss_log("clock: %s called from +%08lX at allocator operation %ld\n",
		       what, (unsigned long)(a - g_exe), (long)g_tr_n);
	else
		ss_log("clock: %s called from %p (outside the executable) at "
		       "allocator operation %ld\n",
		       what, ra, (long)g_tr_n);
}

static void WINAPI gh_GetLocalTime(LPSYSTEMTIME t)
{
	clk_note("GetLocalTime", __builtin_return_address(0));
	r_GetLocalTime(t);
}

static void WINAPI gh_GetSystemTime(LPSYSTEMTIME t)
{
	clk_note("GetSystemTime", __builtin_return_address(0));
	r_GetSystemTime(t);
}

static void WINAPI gh_GetSystemTimeAsFileTime(LPFILETIME t)
{
	clk_note("GetSystemTimeAsFileTime", __builtin_return_address(0));
	r_GetSystemTimeAsFileTime(t);
}

static void clock_probe_install(HMODULE exe)
{
	void *prev;
	int n = 0;

	if (!gh_knob("D3D9SW_CLOCKPROBE", 0))
		return;
	if (!g_exe)
		g_exe = (uintptr_t)exe;
	prev = NULL;
	if (savestate_patch_iat_named(exe, "KERNEL32.dll", "GetLocalTime",
				      (void *)gh_GetLocalTime, &prev) &&
	    prev) {
		r_GetLocalTime = (void(WINAPI *)(LPSYSTEMTIME))prev;
		n++;
	}
	prev = NULL;
	if (savestate_patch_iat_named(exe, "KERNEL32.dll", "GetSystemTime",
				      (void *)gh_GetSystemTime, &prev) &&
	    prev) {
		r_GetSystemTime = (void(WINAPI *)(LPSYSTEMTIME))prev;
		n++;
	}
	prev = NULL;
	if (savestate_patch_iat_named(exe, "KERNEL32.dll", "GetSystemTimeAsFileTime",
				      (void *)gh_GetSystemTimeAsFileTime, &prev) &&
	    prev) {
		r_GetSystemTimeAsFileTime = (void(WINAPI *)(LPFILETIME))prev;
		n++;
	}
	ss_log("clock: %d of 3 calendar import(s) patched - every call will be "
	       "reported with the allocator operation number it happened at, so "
	       "it can be lined up against where two runs part\n",
	       n);
}

/* D3D9SW_GHPEEK=off,off,... prints what is actually stored at those heap
 * offsets, as hex and as text.
 *
 * Added for one specific question. Two runs of the game, from an identical save
 * and visibly doing the same things, allocate identically for 3,363 operations
 * and then differ in the size of a single block: 0x25 bytes in one, 0x2E in the
 * other, from the same call site with the same ordinal at the same offset. That
 * 9-byte difference rounds up to an 8-byte shift and drags every later block
 * along with it, which is the whole of the disagreement between those runs.
 *
 * A size that changes between runs at a fixed point in the sequence is almost
 * always a string, and a string can be read. The trace can say a block changed
 * size; only its contents can say why.
 *
 * Reading is guarded by VirtualQuery rather than assumed safe: the offset comes
 * from a human typing into a config file, and a wrong one would otherwise take
 * the game down inside a diagnostic. */
static void gh_peek_one(unsigned off)
{
	MEMORY_BASIC_INFORMATION mbi;
	const unsigned char *p = (const unsigned char *)g_heap + off;
	char line[128];
	unsigned row;

	if (!g_heap)
		return;
	if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
	    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
		ss_log("gameheap: peek +%08lX is not readable\n", (unsigned long)off);
		return;
	}
	for (row = 0; row < 4; row++) {
		const unsigned char *r = p + row * 16;
		char *o = line;
		unsigned k;

		o += wsprintfA(o, "gameheap: peek +%08lX ", (unsigned long)(off + row * 16));
		for (k = 0; k < 16; k++)
			o += wsprintfA(o, "%02X ", r[k]);
		*o++ = '|';
		for (k = 0; k < 16; k++)
			*o++ = (r[k] >= 32 && r[k] < 127) ? (char)r[k] : '.';
		*o++ = '|';
		*o = 0;
		ss_log("%s\n", line);
	}
}

/* Parsed once at install rather than read at each use. The free path calls into
 * this, and reading a knob from there would put config machinery underneath
 * every free in the process. */
static void gh_peek_parse(void)
{
	char v[96];
	unsigned n = savestate_getenv("D3D9SW_GHPEEK", v, sizeof(v));
	unsigned k, cur = 0;
	int any = 0;

	g_peek_n = 0;
	if (!n || n >= sizeof(v))
		return;
	for (k = 0; k <= n; k++) {
		char c = (k < n) ? v[k] : 0;

		if (c >= '0' && c <= '9') {
			cur = cur * 16 + (unsigned)(c - '0');
			any = 1;
		} else if (c >= 'a' && c <= 'f') {
			cur = cur * 16 + (unsigned)(c - 'a') + 10;
			any = 1;
		} else if (c >= 'A' && c <= 'F') {
			cur = cur * 16 + (unsigned)(c - 'A') + 10;
			any = 1;
		} else if (c == 'x' || c == 'X') {
			cur = 0; /* tolerate a 0x prefix */
		} else {
			if (any && g_peek_n < GH_PEEK_MAX)
				g_peek_off[g_peek_n++] = cur;
			cur = 0;
			any = 0;
		}
	}
}

static void gh_peek(void)
{
	int i;

	for (i = 0; i < g_peek_n; i++)
		gh_peek_one(g_peek_off[i]);
}

/* The save is the wrong moment for a transient.
 *
 * The block this was built to read - the 37-vs-46 byte string at +0xB8610 that
 * splits two otherwise identical sessions - is allocated during startup and
 * freed long before any save. Peeking at report time found four saves' worth of
 * zeroes, which says only that nothing had reused the address yet.
 *
 * A freed block still holds what was written to it right up until the allocator
 * takes it back, so the last honest moment to read one is the instant before it
 * goes. That is here. */
/* -------------------------------------------------- the vorbis witness
 *
 * The block whose size differs between two runs turned out to be the Vorbis
 * vendor string: 0x25 holds "Xiph.Org libVorbis I 20150105 (????)" and 0x2E
 * holds the 20140122 one, whose codename is Latin-1 and so 45 bytes rather than
 * the 47 an ASCII transliteration of it would suggest. This comment named the
 * 20101101 build before the run that settled it, which is a trap worth leaving
 * marked: that build's vendor string is also 45 bytes, so the arithmetic alone
 * cannot tell the two apart and only the logged string can. The music was not
 * all encoded with the same
 * library version, so the allocator's stream depends on which track is being
 * decoded - that is game state, not chance. Naming the track at the operation
 * number it loads at turns the divergence from a nuisance into a coordinate.
 *
 * Reads only, on the free path, capped so a long session cannot bury the log. */
#define GH_VORB_LINES 96
static int g_vorb_on = -1;
static LONG g_vorb_said;

static void gh_vorb_text(char *out, int cap, const unsigned char *p, unsigned n)
{
	unsigned i;

	if ((unsigned)(cap - 1) < n)
		n = (unsigned)(cap - 1);
	for (i = 0; i < n; i++) {
		unsigned char c = p[i];
		if (!c)
			break;
		out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
	}
	out[i] = 0;
}

static void gh_vorbis_free(const void *u, unsigned size)
{
	const unsigned char *p = (const unsigned char *)u;
	char text[128];
	unsigned i;
	int tag = 0;

	if (g_vorb_on < 0)
		g_vorb_on = (int)gh_knob("D3D9SW_GHVORBIS", 0);
	if (!g_vorb_on || !u || size < 8 || size > 512)
		return;
	if (g_vorb_said >= GH_VORB_LINES)
		return;

	if (size >= 19 && !memcmp(p, "Xiph.Org libVorbis", 18)) {
		gh_vorb_text(text, sizeof(text), p, size);
		InterlockedIncrement(&g_vorb_said);
		ss_log("vorbis: comment header freed at allocator operation %ld - "
		       "%lu byte(s), 0x%lX, vendor \"%s\"\n",
		       (long)g_tr_n, (unsigned long)size, (unsigned long)size, text);
		return;
	}

	/* A comment is KEY=value in plain ASCII. TITLE is the one that would name
	 * the track; the others are kept because this encoder wrote very few and
	 * whichever it did write is the only handle on identity we have. */
	for (i = 0; i < size; i++) {
		unsigned char c = p[i];
		if (!c)
			break;
		if (c == '=' && i)
			tag = 1;
		else if (c < 0x20 || c >= 0x7F)
			return;
	}
	if (!tag || i < 3)
		return;
	gh_vorb_text(text, sizeof(text), p, size);
	InterlockedIncrement(&g_vorb_said);
	ss_log("vorbis: tag freed at allocator operation %ld - \"%s\"\n",
	       (long)g_tr_n, text);
}

static void gh_peek_free(void *u)
{
	unsigned off;
	int i;

	if (g_peek_n <= 0 || !u || !g_heap)
		return;
	off = (unsigned)((char *)u - (char *)g_heap);
	for (i = 0; i < g_peek_n; i++) {
		if (g_peek_off[i] != off)
			continue;
		ss_log("gameheap: the watched block at +%08lX is being freed - this is "
		       "what it held:\n",
		       (unsigned long)off);
		gh_peek_one(off);
		return;
	}
}

/* -------------------------------------------------- the allocation watch
 *
 * GHVORBIS names the moment a track loads; this names the block a track load
 * hands back. Set D3D9SW_GHWATCH to the size(s) of the block being chased - the
 * 0ED6BEB0-class Vorbis decoder object is ~3.6 KB - and/or D3D9SW_GHWATCHSITE to
 * a call-site RVA, and every matching allocation prints, at the instant it is
 * served: the allocator operation number it lands at (the SAME clock GHVORBIS
 * prints its landmark on, so the two line up directly), the call site as
 * rabiribi.exe+RVA, the returned address, and whether that address is in our
 * arena or on a heap we do not own.
 *
 * The point is provenance across a restore. The block that faults after a
 * restore-across-a-track-change is read through a pointer that came back wrong;
 * to fix it we first need to know which code allocates it and where in the load
 * sequence, so a second run can confirm the site is deterministic. If the watch
 * stays silent for a size known to crash, that is itself the answer: the block
 * is born on a path we never owned (foreign heap, no redirect, no tag), which is
 * exactly why a restore cannot keep it consistent.
 *
 * Sizes are matched exactly, because the trace has shown block sizes are
 * deterministic to the byte at a fixed point in the sequence. One ss_log per
 * match, capped; on a miss the only cost is the size compare, and every path
 * this sits on is already handing back a pointer - it allocates nothing. */
#define GH_WATCH_MAX 8
#define GH_WATCH_LINES 256
static unsigned g_watch_sz[GH_WATCH_MAX];
static int g_watch_sz_n;
static unsigned g_watch_site[GH_WATCH_MAX];
static int g_watch_site_n;
static LONG g_watch_said;

/* Comma/space separated, each token decimal or 0x-hex. Parsed once at install so
 * the hot path only ever reads the parsed array, never a config string. */
static int gh_watch_list(const char *knob, unsigned *out, int max)
{
	char v[96];
	unsigned n = savestate_getenv(knob, v, sizeof(v));
	unsigned k, cur = 0;
	int got = 0, any = 0, hex = 0;

	if (!n || n >= sizeof(v))
		return 0;
	for (k = 0; k <= n; k++) {
		char c = (k < n) ? v[k] : 0;

		if (c == 'x' || c == 'X') {
			hex = 1; /* a 0x prefix; drop the leading 0 already taken */
			cur = 0;
		} else if (c >= '0' && c <= '9') {
			cur = cur * (hex ? 16u : 10u) + (unsigned)(c - '0');
			any = 1;
		} else if (hex && c >= 'a' && c <= 'f') {
			cur = cur * 16u + (unsigned)(c - 'a') + 10u;
			any = 1;
		} else if (hex && c >= 'A' && c <= 'F') {
			cur = cur * 16u + (unsigned)(c - 'A') + 10u;
			any = 1;
		} else {
			if (any && got < max)
				out[got++] = cur;
			cur = 0;
			any = 0;
			hex = 0;
		}
	}
	return got;
}

static void gh_watch_parse(void)
{
	char line[192];
	char *o = line;
	int i;

	g_watch_sz_n = gh_watch_list("D3D9SW_GHWATCH", g_watch_sz, GH_WATCH_MAX);
	g_watch_site_n = gh_watch_list("D3D9SW_GHWATCHSITE", g_watch_site, GH_WATCH_MAX);
	if (!g_watch_sz_n && !g_watch_site_n)
		return;
	o += wsprintfA(o, "gameheap: watch armed -");
	for (i = 0; i < g_watch_sz_n; i++)
		o += wsprintfA(o, " size %lu(0x%lX)", (unsigned long)g_watch_sz[i],
			       (unsigned long)g_watch_sz[i]);
	for (i = 0; i < g_watch_site_n; i++)
		o += wsprintfA(o, " site+%08lX", (unsigned long)g_watch_site[i]);
	ss_log("%s\n", line);
}

static void gh_watch(const void *u, size_t n, unsigned site, unsigned ord,
		     const char *how)
{
	char base[64];
	int i, hit = 0;

	if ((g_watch_sz_n <= 0 && g_watch_site_n <= 0) || !u)
		return;
	for (i = 0; i < g_watch_sz_n && !hit; i++)
		if (g_watch_sz[i] == (unsigned)n)
			hit = 1;
	for (i = 0; i < g_watch_site_n && !hit; i++)
		if (site != 0xFFFFFFFFu && g_watch_site[i] == site)
			hit = 1;
	if (!hit)
		return;
	/* Cap like the vorbis witness so a size that recurs cannot bury the log. */
	if (InterlockedIncrement(&g_watch_said) > GH_WATCH_LINES)
		return;

	if (in_range(u)) {
		wsprintfA(base, "arena +%08lX",
			  (unsigned long)((uintptr_t)u - (uintptr_t)g_heap));
	} else {
		MEMORY_BASIC_INFORMATION mbi;

		/* Only on a match, so the VirtualQuery is off the hot path. Naming the
		 * allocation base tells arena from a foreign heap at a glance - the
		 * whole point being to catch the block born where we do not own it. */
		if (VirtualQuery(u, &mbi, sizeof(mbi)))
			wsprintfA(base, "foreign, base %08lX",
				  (unsigned long)(uintptr_t)mbi.AllocationBase);
		else
			wsprintfA(base, "foreign");
	}
	if (ord == 0xFFFFFFFFu)
		ss_log("gameheap: WATCH op %ld  %-16s  %lu byte(s) (0x%lX)  "
		       "site rabiribi.exe+%08lX  -> %08lX  [%s]\n",
		       (long)g_tr_n, how, (unsigned long)n, (unsigned long)n,
		       (unsigned long)site, (unsigned long)(uintptr_t)u, base);
	else
		ss_log("gameheap: WATCH op %ld  %-16s  %lu byte(s) (0x%lX)  "
		       "site rabiribi.exe+%08lX ord %lu  -> %08lX  [%s]\n",
		       (long)g_tr_n, how, (unsigned long)n, (unsigned long)n,
		       (unsigned long)site, (unsigned long)ord,
		       (unsigned long)(uintptr_t)u, base);
}

/* Numbered as well as named, because a run can now be cut at several frames and
 * the comparison is per cut: gh_trace2.txt from one session against gh_trace2.txt
 * from another. gh_trace.txt keeps holding the latest, so anything that already
 * reads it carries on working. */
static void gh_trace_dump(void)
{
	static int cut;
	char name[32];

	if (!g_tr)
		return;
	/* The frame-1 save lands in the same frame the heap is installed, so it
	 * would otherwise write an empty trace over gh_trace.txt and peek at a
	 * heap that has served nothing. An empty dump answers no question. */
	if (g_tr_n <= 0) {
		ss_log("gameheap: nothing traced yet, the dump is skipped so the "
		       "last real one survives\n");
		return;
	}
	cut++;
	wsprintfA(name, "gh_trace%d.txt", cut);
	gh_trace_write(name);
	gh_trace_write("gh_trace.txt");
	gh_peek();
}

/* Read-only sibling of gh_tag_take for the fault reporter: names the block an
 * address belongs to WITHOUT retiring the tag. The tag table lives in our own
 * image, not the game's heap, so a restore that rewinds the block to zeroes
 * leaves its birth certificate - call site, size, ordinal - intact. That is
 * exactly the object we want named: one the game allocated after a save and the
 * restore then zeroed, whose own contents can no longer say what it was.
 *
 * Returns 1 when p is a tagged block's start, 2 when p falls inside one (a field
 * pointer, not the head), 0 when it matches nothing we served. site is an exe
 * RVA, size is bytes, ord is the n-th block that call site produced, and base -
 * when asked - is the block's start. Safe from a fault handler: reads only, no
 * lock, and it rejects anything outside our regions before the wider scan. */
int gameheap_whatis(const void *p, unsigned *site, unsigned *size, unsigned *ord,
		    uintptr_t *base)
{
	uintptr_t a = (uintptr_t)p;
	unsigned h, i;

	if (!g_tag || !p || !in_range(p))
		return 0;
	/* Exact first: one hashed probe chain, which is the common case when the
	 * faulting pointer is the object the game passed a method as `this`. */
	h = gh_mix((unsigned)a) & GH_TAG_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_TAG_MASK;
		LONG ad = g_tag[s].addr;

		if (ad == 0)
			break; /* chain ended without a hit; try the interior scan */
		if (ad == (LONG)a) {
			*site = g_tag[s].site;
			*size = g_tag[s].size;
			*ord = g_tag[s].ord;
			if (base)
				*base = a;
			return 1;
		}
	}
	/* Interior: the address is a field inside a block. There is no range index,
	 * so walk the table once and take the tightest block that contains it. This
	 * only ever runs from a one-shot fault report, never on the hot path. */
	{
		uintptr_t best_base = 0;
		unsigned best_size = 0, best_i = 0;
		int hit = 0;

		for (i = 0; i < GH_TAG_SLOTS; i++) {
			LONG ad = g_tag[i].addr;
			uintptr_t b;

			if (ad == 0 || ad == GH_TAG_DEAD)
				continue;
			b = (uintptr_t)ad;
			if (a >= b && a - b < g_tag[i].size &&
			    (!hit || g_tag[i].size < best_size)) {
				best_base = b;
				best_size = g_tag[i].size;
				best_i = i;
				hit = 1;
			}
		}
		if (hit) {
			*site = g_tag[best_i].site;
			*size = g_tag[best_i].size;
			*ord = g_tag[best_i].ord;
			if (base)
				*base = best_base;
			return 2;
		}
	}
	return 0;
}

void gameheap_report(void)
{
	gh_trace_dump();
	if (!g_ready) {
		if (on())
			ss_log("gameheap: asked for, but not installed\n");
		return;
	}
	ss_log("gameheap: %lu allocation(s) served, %lu freed to us, %lu passed back "
	       "to the runtime, %lu too big to take, %lu region(s) mapped, %ld "
	       "busy live (sidetable)%s\n",
	       g_alloc, g_freed_ours, g_freed_theirs, g_toobig, (unsigned long)g_nreg,
	       (long)g_busy_n, g_busy ? "" : " - NO TABLE");
	if (g_busy_full || g_busy_congested)
		ss_log("gameheap: %ld busy insert(s) missed the table, %ld take(s) "
		       "gave up searching - the sidetable is too small or too "
		       "full of tombstones\n",
		       (long)g_busy_full, (long)g_busy_congested);
	if (g_laa_on)
		ss_log("gameheap: LAA_FALSE region %08lX..%08lX (%u MB, %s) served the "
		       "game; %lu allocation(s) fell back because it was full (0 = it "
		       "held the whole session)\n",
		       (unsigned long)g_laa_lo, (unsigned long)g_laa_hi, g_laa_mb,
		       g_laa_high ? ">2 GB band" : "shared 2 GB", g_laa_fell);
	/* The number two design notes wanted before anything was armed. Zero says
	 * the runtime's allocator and the executable's other Heap callers never
	 * traded a block; anything else says they do, and says we caught it. */
	ss_log("gameheap: %lu block(s) of ours reached HeapFree by a route that is "
	       "not the runtime's free%s\n",
	       g_caught,
	       r_heapfree ? "" : " - UNMEASURED, the HeapFree floor is not in");
	if (g_orphan_dropped)
		ss_log("gameheap: %lu block(s) dropped rather than freed - inside our "
		       "arena with no header, so each one would have gone to a heap "
		       "that never issued it%s\n",
		       g_orphan_dropped,
		       " <<< every one of those was a heap corruption avoided");
	if (g_fellback || g_regfull || g_stale)
		ss_log("gameheap: %lu allocation(s) fell back to the runtime, %lu region(s) "
		       "past the table, %lu pointer(s) inside our range with no header - "
		       "any of the three is a hole worth reading about before trusting "
		       "this\n",
		       g_fellback, g_regfull, g_stale);
	if (g_floor_on) {
		HMODULE exe = GetModuleHandleA(NULL);
		uintptr_t base = (uintptr_t)exe;
		LONG i, n = g_floor_n;

		const char *fit;

		if (n > GH_FLOOR_SITES)
			n = GH_FLOOR_SITES;
		if (g_pin_mb && g_floor_peak_kb > (unsigned long)g_pin_mb * 1024)
			fit = " <<< PEAK EXCEEDS THE ARENA - it would need to grow to own this";
		else if (g_pin_mb)
			fit = " - fits the arena, so this site is redirectable as-is";
		else
			fit = "";
		ss_log("gameheap: allocation floor - %ld HeapAlloc + %ld HeapReAlloc "
		       "call(s) through the floor; OFF-ARENA live now %lu KB, PEAK %lu KB "
		       "(0 = every call owned) vs %u MB pinned arena%s\n",
		       (long)g_floor_a_calls, (long)g_floor_r_calls, g_floor_live_kb,
		       g_floor_peak_kb, g_pin_mb, fit);
		if (g_floor_live_kb)
			ss_log("gameheap: allocation floor - %lu KB still live at report: held "
			       "across, or freed by a route we do not intercept\n",
			       g_floor_live_kb);
		if (g_va_total)
			ss_log("gameheap: allocation floor - big blocks on dedicated "
			       "VirtualAlloc: %lu served, %ld live, %lu KB live, PEAK %lu KB. "
			       "Owned and freed on free, off the arena - so the arena can stay "
			       "small\n",
			       g_va_total, (long)g_va_n, g_va_live_kb, g_va_peak_kb);
		if (!n)
			ss_log("gameheap: allocation floor - no off-arena call site recorded\n");
		else {
			ss_log("gameheap: allocation floor - %ld game call site(s) outside the "
			       "arena%s:\n",
			       (long)n, g_floor_over ? ", plus more after the table filled" : "");
			for (i = 0; i < n; i++)
				ss_log("    rabiribi.exe+%08X  %ld alloc(s), %lu byte(s) cumulative\n",
				       (unsigned)((uintptr_t)g_floor[i].ret - base),
				       (long)g_floor[i].count, g_floor[i].bytes);
		}
	}
}

/* ---- heap census: the definitive size of the boundary we would own ---------
 *
 * The allocation floor above sees only what flows through the EXE's patched
 * HeapAlloc IAT. This measures the truth underneath: every heap the process has
 * (GetProcessHeaps), and for each its committed and live-busy bytes, block
 * count, biggest block, and a size histogram - walked directly with HeapWalk. It
 * answers the one question the "become the game's allocator" plan turns on: is
 * the game's live state a 50 MB boundary or a 1 GB one, and is it one heap or
 * many. The game's runtime (MSVCR100 via DxLib) makes its own heap with
 * HeapCreate, so it shows up here as a [game/other] heap distinct from our arena
 * and the process default.
 *
 * Read-only; forwarded to nobody. Must run while all threads are still LIVE, so
 * HeapLock can serialise each walk normally - walking a heap whose lock a
 * suspended thread holds would hang, which is why the save calls this BEFORE it
 * freezes. Each heap is guarded and skipped on failure so one uncooperative heap
 * never sinks the census, and the walk is bounded. 32-bit process, so every size
 * fits unsigned long as KB (wvsprintfA has no %llu). Off unless D3D9SW_HEAPCENSUS
 * is set - it walks every block, so it is not free. */
void gameheap_heap_census(void)
{
	char v[8], v2[8];
	DWORD wc = savestate_getenv("D3D9SW_HEAPCENSUS", v, sizeof(v));
	DWORD wa = savestate_getenv("D3D9SW_ASSETEXCL", v2, sizeof(v2));
	int census_on = (wc > 0 && wc < sizeof(v) && v[0] == '1');
	int asset_on = (wa > 0 && wa < sizeof(v2) && v2[0] == '1');
	HANDLE heaps[128];
	DWORD nheaps, i;
	HANDLE def = GetProcessHeap();
	unsigned long tot_commit_kb = 0, tot_busy_kb = 0, tot_blocks = 0;
	char exe_path[MAX_PATH], *exe_base = exe_path;
	DWORD el;

	/* The walk runs for either knob: the census logs, or ASSETEXCL needs the same
	 * per-heap profile to decide which heaps to hand savestate for holding. */
	if (!census_on && !asset_on)
		return;

	/* The game's own module name, so a busy block whose first word points into it
	 * is a game object (vtable/self-pointer) rather than a Windows one. */
	el = GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
	if (el && el < sizeof(exe_path)) {
		char *q;

		for (q = exe_path; *q; q++)
			if (*q == '\\' || *q == '/')
				exe_base = q + 1;
	} else {
		lstrcpynA(exe_path, "?", sizeof(exe_path));
	}

	nheaps = GetProcessHeaps(128, heaps);
	if (!nheaps) {
		ss_log("heapcensus: GetProcessHeaps returned 0 (err %lu)\n",
		       GetLastError());
		return;
	}
	ss_log("heapcensus: %lu process heap(s); default=%08lX ours-arena=%08lX "
	       "ours-laa=%08lX\n",
	       nheaps, (unsigned long)(uintptr_t)def, (unsigned long)(uintptr_t)g_heap,
	       (unsigned long)(uintptr_t)g_laa_heap);
	if (nheaps > 128)
		nheaps = 128;

	for (i = 0; i < nheaps; i++) {
		HANDLE h = heaps[i];
		PROCESS_HEAP_ENTRY e;
		unsigned long commit_kb = 0, busy_kb = 0, blocks = 0, regions = 0;
		unsigned long hist[6];        /* <256B <4K <64K <256K <1M >=1M */
		unsigned long maxblk = 0;
		unsigned long guard = 0;
		/* PROBE 1 - content owner attribution: which module the first word of each
		 * busy block points into. game = the EXE (a game object), ourmod = our
		 * shims, othermod = any other DLL (Windows/Steam), data = not a pointer.
		 * A heap dominated by game pointers is the game's private heap - a
		 * pin-candidate we can own wholesale; one dominated by othermod pointers is
		 * Windows'/Steam' and must stay in the present. Sampling is capped so 15k
		 * blocks do not turn into 15k loader-lock lookups. */
		unsigned long c_game = 0, c_ours = 0, c_mod = 0, c_data = 0, sampled = 0;
		/* PROBE 2 - content signature: a cheap rolling hash over every busy block
		 * (size + first word). Two census runs across a map transition can be
		 * diffed: a heap whose sig turns over wholesale while size holds is ASSETS
		 * the game reloaded (exclude, do not own); a stable sig is STATE (own). */
		unsigned long sig = 0;
		int locked, walked = 0, k;
		int ours = (h == g_heap) || (g_laa_heap && h == g_laa_heap);
		const char *tag = (h == g_heap) ? " [OURS arena]" :
				  (g_laa_heap && h == g_laa_heap) ? " [OURS laa]" :
				  (h == def) ? " [default]" : " [game/other]";

		for (k = 0; k < 6; k++)
			hist[k] = 0;

		locked = HeapLock(h) ? 1 : 0;
		memset(&e, 0, sizeof(e));
		while (HeapWalk(h, &e)) {
			walked = 1;
			if (e.wFlags & PROCESS_HEAP_REGION) {
				regions++;
				commit_kb += (unsigned long)((e.Region.dwCommittedSize +
							     1023) >> 10);
			} else if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
				unsigned long sz = (unsigned long)e.cbData;

				blocks++;
				busy_kb += (sz + 1023) >> 10;
				if (sz > maxblk)
					maxblk = sz;
				if (sz < 256) hist[0]++;
				else if (sz < 4096) hist[1]++;
				else if (sz < 65536) hist[2]++;
				else if (sz < 262144) hist[3]++;
				else if (sz < 1048576) hist[4]++;
				else hist[5]++;
				if (sz >= 4 && e.lpData) {
					unsigned long w =
						*(volatile unsigned long *)e.lpData;

					sig = sig * 1000003ul + w + sz;
					if (sampled < 512) {
						char mn[64];
						uintptr_t mo;
						const char *m = gh_mod_of(
							(const void *)(uintptr_t)w,
							mn, sizeof(mn), &mo);
						sampled++;
						if (m[0] == '?')
							c_data++;
						else if (!lstrcmpiA(m, exe_base))
							c_game++;
						else if (!lstrcmpiA(m, "d3d11.dll") ||
							 !lstrcmpiA(m, "dxgi.dll") ||
							 !lstrcmpiA(m, "xaudio2_9.dll") ||
							 !lstrcmpiA(m, "xinput1_4.dll"))
							c_ours++;
						else
							c_mod++;
					}
				}
			}
			if (++guard > 20000000ul)
				break;          /* bound the walk */
		}
		if (locked)
			HeapUnlock(h);

		if (!walked) {
			ss_log("heapcensus: heap %08lX%s - HeapWalk yielded nothing "
			       "(err %lu)\n",
			       (unsigned long)(uintptr_t)h, tag, GetLastError());
			continue;
		}
		ss_log("heapcensus: heap %08lX%s - committed %lu KB, busy %lu KB in %lu "
		       "block(s), %lu region(s), biggest %lu KB\n",
		       (unsigned long)(uintptr_t)h, tag, commit_kb, busy_kb, blocks,
		       regions, (maxblk + 1023) >> 10);
		ss_log("    sizes: <256B %lu, <4K %lu, <64K %lu, <256K %lu, <1M %lu, "
		       ">=1M %lu\n",
		       hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);
		ss_log("    owner (of %lu sampled): game %lu, ours %lu, othermod %lu, "
		       "data %lu; sig %08lX%s\n",
		       sampled, c_game, c_ours, c_mod, c_data, sig,
		       (!ours && sampled && c_game > c_mod && c_game >= c_data)
			       ? "  <<< GAME-dominated private heap - pin candidate"
			       : "");
		/* Option 3: a large, pure-data, non-default, non-ours HeapCreate heap with
		 * (almost) no code pointers is the game's asset heap - the one that
		 * ballooned across the room change. Hand it to savestate to hold in the
		 * present so the game reloads it, instead of rewinding it to a moved base.
		 * The ~2 game-pointer slack tolerates a stray vtable in an asset wrapper. */
		if (asset_on && !ours && h != def && commit_kb >= 1024 && sampled &&
		    c_data * 10 >= sampled * 9 && c_game <= 2) {
			savestate_note_asset_heap(h);
			ss_log("    -> ASSETEXCL: heap %08lX marked regenerable ASSET - it "
			       "will be held in the present and reloaded by the game\n",
			       (unsigned long)(uintptr_t)h);
		}
		if (!ours) {
			tot_commit_kb += commit_kb;
			tot_busy_kb += busy_kb;
			tot_blocks += blocks;
		}
	}
	ss_log("heapcensus: NON-OURS total - committed %lu KB (%lu MB), busy %lu KB "
	       "(%lu MB), %lu block(s). THIS is the boundary to own for cross-session "
	       "(arena is %u MB)\n",
	       tot_commit_kb, tot_commit_kb >> 10, tot_busy_kb, tot_busy_kb >> 10,
	       tot_blocks, g_pin_mb);
}

/* HEAPBLOCKS for this heap under Wine, without HeapWalk. Wine has no Windows
 * HEAP_SEGMENT, so the engine used to memcpy the whole region - lists, TLS
 * objects a LEFT RUNNING mmdevapi thread still holds, everything. The magic
 * is keyed to the user pointer, so a word that merely looks busy does not
 * match. */
int gameheap_saved_block(const void *saved, void *live_head, size_t remain, size_t *n)
{
	const GhHead *h;
	uintptr_t user;
	size_t total;

	if (!saved || !live_head || !n || remain < sizeof(GhHead))
		return 0;
	h = (const GhHead *)saved;
	user = (uintptr_t)live_head + sizeof(GhHead);
	if (h->magic != (GH_MAGIC ^ user))
		return 0;
	if (!h->size || h->size >= GH_BIG)
		return 0;
	total = sizeof(GhHead) + h->size;
	if (total > remain)
		return 0;
	*n = total;
	return 1;
}

unsigned gameheap_busy_snapshot(void)
{
	unsigned i, n = 0;

	g_busy_snap_n = 0;
	if (!g_busy || !g_busy_snap)
		return 0;
	for (i = 0; i < GH_BUSY_SLOTS; i++) {
		LONG a = g_busy[i].addr;
		size_t sz;

		if (a == 0 || a == GH_BUSY_DEAD)
			continue;
		sz = g_busy[i].size;
		if (!sz || sz >= GH_BIG)
			continue;
		if (n >= g_busy_snap_cap)
			break;
		g_busy_snap[n].head = (char *)(uintptr_t)a - sizeof(GhHead);
		g_busy_snap[n].total = sizeof(GhHead) + sz;
		n++;
	}
	g_busy_snap_n = n;
	return n;
}

unsigned gameheap_busy_saved_count(void)
{
	return g_busy_snap_n;
}

int gameheap_busy_saved_at(unsigned i, void **head, size_t *total)
{
	if (!head || !total || !g_busy_snap || i >= g_busy_snap_n)
		return 0;
	*head = g_busy_snap[i].head;
	*total = g_busy_snap[i].total;
	return 1;
}

void gameheap_busy_rewind(void)
{
	unsigned i;

	if (!g_busy || !g_busy_snap || !g_busy_snap_n)
		return;
	memset(g_busy, 0, GH_BUSY_SLOTS * sizeof(GhBusy));
	g_busy_n = 0;
	g_busy_full = 0;
	g_busy_congested = 0;
	for (i = 0; i < g_busy_snap_n; i++) {
		void *u = (char *)g_busy_snap[i].head + sizeof(GhHead);
		size_t sz = g_busy_snap[i].total - sizeof(GhHead);

		gh_busy_put(u, sz);
	}
}
