/* Same-folder steam_api.dll that records what Rabi-Ribi actually calls.
 *
 * The real Steam client still answers. Rename the game's steam_api.dll to
 * steam_api_real.dll and put this file in its place. SteamAPI_Init goes
 * through to the real DLL, which is what the running game needs in order to
 * stay a Steam process. Every other call is forwarded too, and written to
 * steam_shim.log beside this DLL.
 *
 * Startup refuses to open a window unless this file's size is in
 * [182373, 201570] bytes. The original DLL is 187472. After linking, pad
 * the file out to that size; a smaller or larger shim exits before the
 * title screen.
 *
 * The ten imports are not the surface. SteamUser() and the others return an
 * interface, and the game calls methods through its vtable. Those methods are
 * thiscall. Each slot in the replacement vtable logs its index and jumps to
 * the real method with ecx set back to the real object.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define NIFACE 5
#define NSLOT 128

struct wrap {
	void *vt;
	void *real;
	const char *name;
	int which;
};

extern void *thunk_table[];

static HMODULE real_dll;
static CRITICAL_SECTION cs;
static volatile LONG cs_state;
static unsigned short hits[NIFACE][NSLOT];
static unsigned run_n;
static int summary_written;
static volatile LONG crash_logged;

static void note(const char *line);

static void lock(void)
{
	LONG s = InterlockedCompareExchange(&cs_state, 1, 0);
	if (s == 0) {
		InitializeCriticalSection(&cs);
		InterlockedExchange(&cs_state, 2);
	} else {
		while (cs_state != 2)
			Sleep(0);
	}
	EnterCriticalSection(&cs);
}

static void unlock(void)
{
	LeaveCriticalSection(&cs);
}

static void note(const char *line)
{
	HMODULE self = NULL;
	wchar_t path[MAX_PATH], *slash;
	HANDLE f;
	DWORD n, wrote;
	char buf[640];
	SYSTEMTIME st;

	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCWSTR)note, &self);
	if (!self || !GetModuleFileNameW(self, path, MAX_PATH))
		return;
	slash = wcsrchr(path, L'\\');
	if (!slash)
		return;
	wcscpy(slash + 1, L"steam_shim.log");
	GetLocalTime(&st);
	n = wsprintfA(buf, "%02u:%02u:%02u.%03u %s\r\n", st.wHour, st.wMinute, st.wSecond,
		      st.wMilliseconds, line);
	f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	WriteFile(f, buf, n, &wrote, NULL);
	CloseHandle(f);
}

static LONG CALLBACK crash_veh(EXCEPTION_POINTERS *ep)
{
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	char buf[160];

	/* OutputDebugString and the MSVC thread-name exception are not crashes. */
	if (code == 0x40010006 || code == 0x4001000A || code == 0x406D1388)
		return EXCEPTION_CONTINUE_SEARCH;
	if (InterlockedExchange(&crash_logged, 1) == 0) {
		wsprintfA(buf, "exception %08lx at %p", code,
			  ep->ExceptionRecord->ExceptionAddress);
		note(buf);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

/* Early reservation recorder.
 *
 * This DLL is the one import the exe loads from its own folder, so its attach
 * runs before the Steam stub's entry point - before .text is even decrypted,
 * and before the C runtime or DxLib reserve anything. Everything the game
 * builds at startup (the DxLib handle-table block, the asset blocks, the heaps)
 * lands at a different base every launch, and d3d11.dll's hooks arrive too
 * late to see any of it. This records every reservation of 1 MB and up from
 * here on: base, size, time since attach and the raw return chain. The hook
 * only writes a table slot; names are resolved and written out at er_flush,
 * outside it. gameheap.c chains its own detour behind this one. Log-only. */
#define ER_CAP 4096
#define ER_FR 12
struct er_rec {
	uintptr_t base, size;
	DWORD ms;
	ULONG type, prot;
	USHORT nfr;
	unsigned map;
	void *fr[ER_FR];
	unsigned key, ord, req;
	signed char elig, placed; /* placed: 1 at its slot, -1 slot refused */
	volatile LONG ready;
};
/* Rabi-Ribi's current map id, a word in the exe's own data (RR_MAPID in
 * savestate.c): 0 during startup, 8 at the title and menus, the area number
 * in play. Read only to tag records, so a restore's log can say which part of
 * the game asked for each block. NULL if the image is too small for it. */
#define ER_MAPID_RVA 0xA908F8u
static volatile unsigned *er_map;
static unsigned er_map_last = ~0u;
static struct er_rec er_tab[ER_CAP];
static volatile LONG er_n;
static LONG er_flushed;
static DWORD er_t0;
static LONG (WINAPI *er_real)(HANDLE, PVOID *, ULONG_PTR, PSIZE_T, ULONG, ULONG);
static USHORT (NTAPI *er_bt)(ULONG, ULONG, PVOID *, PULONG);

static LONG (WINAPI *er_realx)(HANDLE, PVOID *, PSIZE_T, ULONG, ULONG, PVOID, ULONG);
static unsigned char *er_stub;

/* ---- placement: the game's large blocks at the same address every launch --
 *
 * Every big block the game builds - the DxLib handle table, the decoded asset
 * blocks - is one HeapAlloc from its own allocator, made before the title is
 * up, in the same order with the same sizes each launch. Only the address
 * moves, because Windows randomises where the first allocations go and freed
 * load buffers leave different holes. A save holds pointers into those blocks,
 * so a restore into another launch reads the wrong memory.
 *
 * A request is known by the exe frames that made it (their offsets, so the
 * image base does not matter), its size, and how many identical requests came
 * before it. PICO_PLACE=1 in d3d9_sw.cfg: a launch with no map records, and at
 * shutdown writes every such block still alive into pico_place.txt, packed
 * into a fixed window. Load buffers are freed by then, so they get no slot.
 * Every later launch replays: the slots are reserved as placeholders the
 * moment this DLL loads, and a matching request gets its placeholder released
 * and its own address handed to the heap's call. Anything else is left alone.
 * PICO_PLACE=2 records again over an old map. */
#define PL_BASE 0x28000000u  /* above the gameheap pin, free at startup */
#define PL_LIMIT 0x40000000u /* d3d11's texture arena starts here */
#define PL_SIG 8
#define PL_CAP 128
static int pl_mode; /* 0 off, 1 record, 2 replay */
static uintptr_t pl_img, pl_img_end, pl_sys[3], pl_own;
static DWORD pl_stamp, pl_isize;
static struct {
	unsigned key, req, ord;
	uintptr_t at, len;
	volatile LONG used;
	int held;
} pl_slot[PL_CAP];
static int pl_nslot;
static struct {
	unsigned key;
	LONG n;
} pl_ord[256];
static volatile LONG pl_spin;
static wchar_t pl_path[MAX_PATH];

static uintptr_t pl_abase(const void *a)
{
	MEMORY_BASIC_INFORMATION m;

	return VirtualQuery(a, &m, sizeof(m)) == sizeof(m) ? (uintptr_t)m.AllocationBase : 0;
}

/* The key: frames past our own hook, gameheap's (if it chained in front)
 * and the system DLLs, which must then start in the exe. Not the size: the
 * heap pads every large block by a random amount, so the same request asks
 * for a few KB more or less each launch. */
static int pl_sig(void **fr, int n, unsigned req, unsigned *key)
{
	uintptr_t front = 0;
	unsigned h = 2166136261u;
	int k = 0, used = 0, s;

	if (er_stub && er_stub[0] == 0xE9)
		front = pl_abase(er_stub + 5 + *(LONG *)(er_stub + 1));
	if (front == pl_own)
		front = 0;
	for (; k < n; k++) {
		uintptr_t ab = pl_abase(fr[k]);

		if (ab == pl_own || (front && ab == front && k == 0))
			continue;
		for (s = 0; s < 3; s++)
			if (ab && ab == pl_sys[s])
				break;
		if (s == 3)
			break;
	}
	if (k >= n || (uintptr_t)fr[k] < pl_img || (uintptr_t)fr[k] >= pl_img_end)
		return 0;
	for (; k < n && used < PL_SIG; k++) {
		uintptr_t a = (uintptr_t)fr[k];
		unsigned rva = (a >= pl_img && a < pl_img_end) ? (unsigned)(a - pl_img) : 0xFFFFFFFFu;
		int b;

		for (b = 0; b < 4; b++)
			h = (h ^ ((rva >> (8 * b)) & 0xFF)) * 16777619u;
		used++;
	}
	(void)req;
	*key = h | 1u;
	return 1;
}

static unsigned pl_ord_next(unsigned key)
{
	unsigned i = key & 255u, j;
	LONG n = 0;

	while (InterlockedCompareExchange(&pl_spin, 1, 0))
		YieldProcessor();
	for (j = 0; j < 256; j++, i = (i + 1) & 255u)
		if (pl_ord[i].key == key || !pl_ord[i].key) {
			pl_ord[i].key = key;
			n = pl_ord[i].n++;
			break;
		}
	InterlockedExchange(&pl_spin, 0);
	return (unsigned)n;
}

/* 1 placed (st set), -1 its slot could not be used, 0 no slot for it. */
static int pl_place(unsigned key, unsigned req, unsigned ord, PVOID *base, ULONG_PTR zb,
		    PSIZE_T size, ULONG type, ULONG prot, LONG *st)
{
	int i;

	for (i = 0; i < pl_nslot; i++) {
		if (pl_slot[i].key != key || pl_slot[i].ord != ord)
			continue;
		if (!pl_slot[i].held || req > pl_slot[i].len ||
		    InterlockedExchange(&pl_slot[i].used, 1))
			return -1;
		VirtualFree((void *)pl_slot[i].at, 0, MEM_RELEASE);
		*base = (PVOID)pl_slot[i].at;
		*st = er_real(GetCurrentProcess(), base, zb, size, type | MEM_RESERVE, prot);
		if (*st >= 0)
			return 1;
		*base = NULL;
		*size = req;
		return -1;
	}
	return 0;
}

/* Knob from d3d9_sw.cfg beside the exe, so it sits with the other switches. */
static int pl_knob(void)
{
	wchar_t path[MAX_PATH], *slash;
	char buf[65536], *p;
	HANDLE f;
	DWORD got = 0;

	GetModuleFileNameW(NULL, path, MAX_PATH);
	slash = wcsrchr(path, L'\\');
	if (!slash)
		return 0;
	wcscpy(slash + 1, L"pico_place.txt");
	lstrcpynW(pl_path, path, MAX_PATH);
	wcscpy(slash + 1, L"d3d9_sw.cfg");
	f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
	CloseHandle(f);
	buf[got] = 0;
	for (p = buf; (p = strstr(p, "PICO_PLACE=")) != NULL; p++)
		if (p == buf || p[-1] == '\n')
			return p[11] - '0';
	return 0;
}

/* Replay: read the map and reserve every slot before anything else can. */
static int pl_load(char *why)
{
	HANDLE f = CreateFileW(pl_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0,
			       NULL);
	static char buf[65536];
	char *p;
	DWORD got = 0;
	unsigned long stamp = 0, isize = 0;

	if (f == INVALID_HANDLE_VALUE) {
		lstrcpyA(why, "no pico_place.txt yet");
		return 0;
	}
	ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
	CloseHandle(f);
	buf[got] = 0;
	p = strstr(buf, "image ");
	if (!p || sscanf(p, "image %lx %lx", &stamp, &isize) != 2 || stamp != pl_stamp ||
	    isize != pl_isize) {
		wsprintfA(why, "pico_place.txt was recorded against another exe build "
			       "(%08lX/%08lX, this is %08lX/%08lX)",
			  stamp, isize, pl_stamp, pl_isize);
		return 0;
	}
	for (p = buf; (p = strstr(p, "\nslot ")) != NULL && pl_nslot < PL_CAP; p++) {
		unsigned long key, req, ord, off, len;

		if (sscanf(p + 1, "slot %lx %lx %lu %lx %lx", &key, &req, &ord, &off, &len) != 5)
			continue;
		pl_slot[pl_nslot].key = key;
		pl_slot[pl_nslot].req = req;
		pl_slot[pl_nslot].ord = ord;
		pl_slot[pl_nslot].at = PL_BASE + off;
		pl_slot[pl_nslot].len = len;
		pl_slot[pl_nslot].held =
			VirtualAlloc((void *)(PL_BASE + off), len, MEM_RESERVE, PAGE_NOACCESS) != NULL;
		pl_nslot++;
	}
	return pl_nslot > 0;
}

/* A new reservation is MEM_RESERVE, or MEM_COMMIT with no base: Windows
 * reserves implicitly then, and that is how the heap takes its large blocks -
 * every HeapAlloc of a MB or more arrives as type 0x1000 with base NULL. */
static __attribute__((noinline)) void er_add(PVOID in, PVOID *base, PSIZE_T size, ULONG type,
					     ULONG prot, ULONG ex, void **fr, USHORT nfr,
					     unsigned key, unsigned ord, int elig, int placed,
					     unsigned req)
{
	LONG i;

	if (!((type & MEM_RESERVE) || (!in && (type & MEM_COMMIT))) || !size ||
	    *size < (1u << 20) || !base || !*base)
		return;
	i = InterlockedIncrement(&er_n) - 1;
	if (i >= ER_CAP)
		return;
	er_tab[i].base = (uintptr_t)*base;
	er_tab[i].size = *size;
	er_tab[i].ms = GetTickCount() - er_t0;
	er_tab[i].type = type | (ex ? 0x80000000u : 0);
	er_tab[i].prot = prot;
	er_tab[i].map = er_map ? *er_map : ~0u;
	er_tab[i].key = key;
	er_tab[i].ord = ord;
	er_tab[i].req = req;
	er_tab[i].elig = (signed char)elig;
	er_tab[i].placed = (signed char)placed;
	if (fr) {
		memcpy(er_tab[i].fr, fr, nfr * sizeof(void *));
		er_tab[i].nfr = nfr;
	} else
		er_tab[i].nfr = er_bt ? er_bt(2, ER_FR, er_tab[i].fr, NULL) : 0;
	InterlockedExchange(&er_tab[i].ready, 1);
}

static LONG WINAPI er_ntav(HANDLE proc, PVOID *base, ULONG_PTR zb, PSIZE_T size,
			   ULONG type, ULONG prot)
{
	PVOID in = base ? *base : NULL;
	SIZE_T req = size ? *size : 0;
	int self = proc == (HANDLE)(LONG_PTR)-1;
	void *fr[ER_FR];
	USHORT nfr = 0;
	unsigned key = 0, ord = 0;
	int elig = 0, placed = 0;
	LONG st = 0;

	if (self && base && !in && req >= (1u << 20) && (type & (MEM_COMMIT | MEM_RESERVE))) {
		nfr = er_bt ? er_bt(1, ER_FR, fr, NULL) : 0;
		if (pl_mode && pl_sig(fr, nfr, (unsigned)req, &key)) {
			elig = 1;
			ord = pl_ord_next(key);
			if (pl_mode == 2)
				placed = pl_place(key, (unsigned)req, ord, base, zb, size, type,
						  prot, &st);
		}
	}
	if (placed != 1)
		st = er_real(proc, base, zb, size, type, prot);
	if (st >= 0 && self)
		er_add(in, base, size, type, prot, 0, nfr ? fr : NULL, nfr, key, ord, elig, placed,
		       (unsigned)req);
	return st;
}

static LONG WINAPI er_ntavx(HANDLE proc, PVOID *base, PSIZE_T size, ULONG type, ULONG prot,
			    PVOID params, ULONG nparams)
{
	PVOID in = base ? *base : NULL;
	LONG st = er_realx(proc, base, size, type, prot, params, nparams);

	if (st >= 0 && proc == (HANDLE)(LONG_PTR)-1)
		er_add(in, base, size, type, prot, 1, NULL, 0, 0, 0, 0, 0, 0);
	return st;
}

/* Same technique as gameheap.c: the WOW64 stub opens with mov eax, SSN (5
 * bytes), which moves into a trampoline intact. */
static void *er_detour(HMODULE nt, const char *name, void *hook)
{
	unsigned char *t = (unsigned char *)GetProcAddress(nt, name);
	unsigned char *tr;
	DWORD old;

	if (!t || t[0] != 0xB8)
		return NULL;
	tr = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
					   PAGE_EXECUTE_READWRITE);
	if (!tr)
		return NULL;
	memcpy(tr, t, 5);
	tr[5] = 0xE9;
	*(LONG *)(tr + 6) = (LONG)((t + 5) - (tr + 10));
	if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old))
		return NULL;
	t[0] = 0xE9;
	*(LONG *)(t + 1) = (LONG)((unsigned char *)hook - (t + 5));
	VirtualProtect(t, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), t, 5);
	return tr;
}

static int er_arm(void)
{
	HMODULE nt = GetModuleHandleA("ntdll.dll");

	if (!nt)
		return 0;
	er_bt = (USHORT(NTAPI *)(ULONG, ULONG, PVOID *, PULONG))GetProcAddress(
		nt, "RtlCaptureStackBackTrace");
	er_stub = (unsigned char *)GetProcAddress(nt, "NtAllocateVirtualMemory");
	er_realx = (LONG(WINAPI *)(HANDLE, PVOID *, PSIZE_T, ULONG, ULONG, PVOID, ULONG))
		er_detour(nt, "NtAllocateVirtualMemoryEx", (void *)er_ntavx);
	er_real = (LONG(WINAPI *)(HANDLE, PVOID *, ULONG_PTR, PSIZE_T, ULONG, ULONG))
		er_detour(nt, "NtAllocateVirtualMemory", (void *)er_ntav);
	return (er_real ? 1 : 0) | (er_realx ? 2 : 0);
}

/* Once d3d11.dll chains its own detour in front of ours, every record's
 * nearest frame is that hook rather than the caller. It is whatever module
 * the stub now jumps into. */
static HMODULE er_front(void)
{
	HMODULE m = NULL;

	if (!er_stub || er_stub[0] != 0xE9)
		return NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCSTR)(er_stub + 5 + *(LONG *)(er_stub + 1)), &m);
	return m;
}

/* "module+offset" for an address, or the bare address outside any module. */
static void er_where(const void *a, char *out)
{
	HMODULE m = NULL;
	char path[MAX_PATH], *b, *p;

	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)a, &m) ||
	    !GetModuleFileNameA(m, path, MAX_PATH)) {
		wsprintfA(out, "%p", a);
		return;
	}
	for (b = p = path; *p; p++)
		if (*p == '\\' || *p == '/')
			b = p + 1;
	wsprintfA(out, "%.24s+%lX", b, (unsigned long)((const char *)a - (const char *)m));
}

static int er_sys(const char *w)
{
	return !_strnicmp(w, "ntdll", 5) || !_strnicmp(w, "kernelbase", 10) ||
	       !_strnicmp(w, "kernel32", 8) || !_strnicmp(w, "steam_api.dll", 13);
}

static void er_flush(const char *when)
{
	char buf[640], w[48];
	LONG n, i;

	lock();
	n = er_flushed;
	while (n < ER_CAP && n < er_n && er_tab[n].ready)
		n++;
	if (er_flushed < n) {
		wsprintfA(buf, "early reservations (%s): #%ld-#%ld of %ld so far", when,
			  er_flushed, n - 1, er_n);
		note(buf);
	}
	for (i = er_flushed; i < n; i++) {
		struct er_rec *r = &er_tab[i];
		int k, len, first = -1, k0 = 0;
		HMODULE fm = NULL, front = er_front(), self = NULL;

		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				   (LPCSTR)er_flush, &self);
		if (r->nfr && front && front != self &&
		    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				       (LPCSTR)r->fr[0], &fm) &&
		    fm == front)
			k0 = 1;
		for (k = k0; k < r->nfr; k++) {
			er_where(r->fr[k], w);
			if (!er_sys(w)) {
				first = k;
				break;
			}
		}
		if (first >= 0)
			er_where(r->fr[first], w);
		else
			lstrcpyA(w, "?");
		len = wsprintfA(buf, "  er #%ld +%lums map %d %08lX %lu KB type %lX prot %lX%s origin %s |",
				i, r->ms, (int)r->map, (unsigned long)r->base,
				(unsigned long)(r->size >> 10),
				r->type & 0x7FFFFFFFu, r->prot,
				(r->type & 0x80000000u) ? " (Ex)" : "", w);
		if (r->elig)
			len += wsprintfA(buf + len, " [key %08X#%u%s]", r->key, r->ord,
					 r->placed == 1    ? " PLACED"
					 : r->placed == -1 ? " SLOT REFUSED"
					 : pl_mode == 2    ? " no slot"
							   : "");
		for (k = first < 0 ? 0 : first + 1; k < r->nfr && len < 560; k++) {
			er_where(r->fr[k], w);
			len += wsprintfA(buf + len, " %s", w);
		}
		note(buf);
	}
	er_flushed = n;
	unlock();
}

/* Record launch, at shutdown: every eligible block still alive gets a slot,
 * packed into the window in the order the game asked for them. A base the
 * game freed and reused is judged by its latest record only, so a load buffer
 * whose address a permanent block took later does not get a slot too. */
static void pl_write(void)
{
	HANDLE f;
	char line[512], w[48];
	DWORD wr;
	uintptr_t off = 0;
	LONG i, j, n = er_n < ER_CAP ? er_n : ER_CAP;
	int nslot = 0, len;

	f = CreateFileW(pl_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		note("placement: could not write pico_place.txt");
		return;
	}
	len = wsprintfA(line,
			"# pico placement map, recorded by pid %lu. One slot per large block the\r\n"
			"# game's own code asked for and still held at shutdown.\r\n"
			"# slot <key> <request bytes seen> <ordinal> <offset from %08lX> <length>\r\n"
			"image %08lX %08lX\r\n",
			GetCurrentProcessId(), (unsigned long)PL_BASE, pl_stamp, pl_isize);
	WriteFile(f, line, len, &wr, NULL);
	for (i = 0; i < n; i++) {
		struct er_rec *r = &er_tab[i];
		MEMORY_BASIC_INFORMATION m;
		uintptr_t a, rl = 0;
		int k;

		if (!r->ready || !r->elig)
			continue;
		for (j = i + 1; j < n; j++)
			if (er_tab[j].ready && er_tab[j].base == r->base)
				break;
		if (j < n)
			continue;
		if (VirtualQuery((void *)r->base, &m, sizeof(m)) != sizeof(m) ||
		    (uintptr_t)m.AllocationBase != r->base || m.State == MEM_FREE)
			continue;
		for (a = r->base; VirtualQuery((void *)a, &m, sizeof(m)) == sizeof(m) &&
				  (uintptr_t)m.AllocationBase == r->base && m.State != MEM_FREE;
		     a += m.RegionSize)
			rl += m.RegionSize;
		/* 64 KB over what this launch took, for the heap's random padding. */
		rl = ((rl + 0xFFFFu) & ~(uintptr_t)0xFFFFu) + 0x10000u;
		if (PL_BASE + off + rl > PL_LIMIT) {
			note("placement: the window is full; later blocks get no slot");
			break;
		}
		len = wsprintfA(line, "slot %08X %08X %u %08lX %08lX  # %lu KB, map %d,", r->key,
				r->req, r->ord, (unsigned long)off, (unsigned long)rl,
				(unsigned long)(rl >> 10), (int)r->map);
		for (k = 0; k < r->nfr && len < 440; k++)
			if ((uintptr_t)r->fr[k] >= pl_img && (uintptr_t)r->fr[k] < pl_img_end) {
				er_where(r->fr[k], w);
				len += wsprintfA(line + len, " %s", w);
			}
		len += wsprintfA(line + len, "\r\n");
		WriteFile(f, line, len, &wr, NULL);
		off += rl;
		nslot++;
	}
	CloseHandle(f);
	wsprintfA(line, "placement: recorded %d slot(s), %lu KB of the window at %08lX, into "
			"pico_place.txt. The next launch places them.",
		  nslot, (unsigned long)(off >> 10), (unsigned long)PL_BASE);
	note(line);
}

/* Replay launch, at shutdown: what was placed, and every slot left unused. */
static void pl_report(void)
{
	char line[160];
	int i, used = 0;

	for (i = 0; i < pl_nslot; i++)
		if (pl_slot[i].used)
			used++;
		else {
			wsprintfA(line, "  placement: slot %08X#%u at %08lX (%lu KB) was never "
					"requested%s",
				  pl_slot[i].key, pl_slot[i].ord, (unsigned long)pl_slot[i].at,
				  (unsigned long)(pl_slot[i].len >> 10),
				  pl_slot[i].held ? "" : ", and its placeholder was refused at load");
			note(line);
		}
	wsprintfA(line, "placement: %d of %d slot(s) used this launch", used, pl_nslot);
	note(line);
}

/* Private allocations of 1 MB and up. At attach, the address space as the
 * loader leaves it, before any game code has run. Later, each one marked with
 * the record that saw it reserved, or NOT SEEN - which is a path neither hook
 * covers (a thread stack, which the kernel reserves, or something newer). */
static void er_census(const char *when, int mark)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t p = 0x10000, cur = 0, res = 0, com = 0;
	char buf[200], seen[40];
	int n = 0, unseen = 0;
	LONG nr = er_n < ER_CAP ? er_n : ER_CAP;

	wsprintfA(buf, "census %s: private allocations of 1 MB and up", when);
	note(buf);
	for (;;) {
		int more = VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == sizeof(mbi) &&
			   mbi.RegionSize && p + mbi.RegionSize > p;
		uintptr_t ab = more ? (uintptr_t)mbi.AllocationBase : 0;

		if (cur && (!more || mbi.Type != MEM_PRIVATE || ab != cur)) {
			if (res >= (1u << 20)) {
				LONG k;

				seen[0] = 0;
				if (mark) {
					lstrcpyA(seen, "  NOT SEEN");
					for (k = nr - 1; k >= 0; k--)
						if (er_tab[k].ready && er_tab[k].base == cur) {
							wsprintfA(seen, "  seen er #%ld", k);
							break;
						}
					if (k < 0)
						unseen++;
				}
				wsprintfA(buf, "  %s %08lX reserve %lu KB commit %lu KB%s", when,
					  (unsigned long)cur, (unsigned long)(res >> 10),
					  (unsigned long)(com >> 10), seen);
				note(buf);
				n++;
			}
			cur = 0;
		}
		if (!more)
			break;
		if (mbi.State != MEM_FREE && mbi.Type == MEM_PRIVATE) {
			if (!cur) {
				cur = ab;
				com = 0;
			}
			res = (uintptr_t)mbi.BaseAddress + mbi.RegionSize - cur;
			if (mbi.State == MEM_COMMIT)
				com += mbi.RegionSize;
		}
		p = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
	}
	wsprintfA(buf, "census %s: %d allocation(s), %d not seen by the recorder; exe at %p",
		  when, n, unseen, (void *)GetModuleHandleA(NULL));
	note(buf);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	char buf[80];

	(void)inst;
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		AddVectoredExceptionHandler(1, crash_veh);
		wsprintfA(buf, "attach pid=%lu", GetCurrentProcessId());
		note(buf);
		int armed;

		er_t0 = GetTickCount();
		{
			unsigned char *img = (unsigned char *)GetModuleHandleA(NULL);
			IMAGE_NT_HEADERS *nt =
				(IMAGE_NT_HEADERS *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);

			if (nt->OptionalHeader.SizeOfImage > ER_MAPID_RVA + 4)
				er_map = (volatile unsigned *)(img + ER_MAPID_RVA);
			pl_img = (uintptr_t)img;
			pl_img_end = pl_img + nt->OptionalHeader.SizeOfImage;
			pl_stamp = nt->FileHeader.TimeDateStamp;
			pl_isize = nt->OptionalHeader.SizeOfImage;
			pl_own = pl_abase((void *)er_ntav);
			pl_sys[0] = (uintptr_t)GetModuleHandleA("ntdll.dll");
			pl_sys[1] = (uintptr_t)GetModuleHandleA("kernel32.dll");
			pl_sys[2] = (uintptr_t)GetModuleHandleA("kernelbase.dll");
		}
		er_census("at-attach", 0);
		{
			int k = pl_knob();
			char why[200] = "";
			char line[300];

			if (k == 1 && pl_load(why)) {
				int i, held = 0;

				pl_mode = 2;
				for (i = 0; i < pl_nslot; i++)
					held += pl_slot[i].held;
				wsprintfA(line, "placement: REPLAY - %d slot(s) from pico_place.txt, %d "
						"placeholder(s) reserved in the window at %08lX",
					  pl_nslot, held, (unsigned long)PL_BASE);
				note(line);
			} else if (k == 1 || k == 2) {
				pl_mode = 1;
				wsprintfA(line, "placement: RECORD - %s; nothing is moved this launch, "
						"the map is written at shutdown",
					  k == 2 ? "PICO_PLACE=2 asks for a new map" : why);
				note(line);
			} else
				note("placement: off (PICO_PLACE is not set in d3d9_sw.cfg)");
		}
		armed = er_arm();
		wsprintfA(buf, "early reservation recorder: NtAllocateVirtualMemory %s, Ex %s",
			  (armed & 1) ? "armed" : "NOT armed", (armed & 2) ? "armed" : "NOT armed");
		note(buf);
	} else if (reason == DLL_PROCESS_DETACH) {
		er_flush("detach");
		wsprintfA(buf, "detach pid=%lu", GetCurrentProcessId());
		note(buf);
	}
	return TRUE;
}

static HMODULE real_mod(void)
{
	wchar_t path[MAX_PATH], *slash;
	HMODULE self = NULL;

	if (real_dll)
		return real_dll;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCWSTR)real_mod, &self);
	if (!self || !GetModuleFileNameW(self, path, MAX_PATH))
		return NULL;
	slash = wcsrchr(path, L'\\');
	if (!slash)
		return NULL;
	wcscpy(slash + 1, L"steam_api_real.dll");
	real_dll = LoadLibraryW(path);
	if (!real_dll)
		note("steam_api_real.dll did not load; rename the original steam_api.dll to that");
	return real_dll;
}

static FARPROC real_proc(const char *name)
{
	HMODULE m = real_mod();
	FARPROC f = m ? GetProcAddress(m, name) : NULL;
	char buf[128];
	if (!f) {
		wsprintfA(buf, "missing export %s", name);
		note(buf);
	}
	return f;
}

/* Public callback ids. The number is what CCallbackBase stores; the name is
 * only so the log can be read without the SDK open. Unknown ids stay numeric. */
static const char *callback_name(int id)
{
	switch (id) {
	case 101: return "SteamServersConnected";
	case 102: return "SteamServerConnectFailure";
	case 103: return "SteamServersDisconnected";
	case 113: return "ClientGameServerDeny";
	case 117: return "IPCFailure";
	case 125: return "LicensesUpdated";
	case 143: return "ValidateAuthTicketResponse";
	case 152: return "MicroTxnAuthorizationResponse";
	case 163: return "GetAuthSessionTicketResponse";
	case 164: return "GameWebCallback";
	case 304: return "PersonaStateChange";
	case 331: return "GameOverlayActivated";
	case 332: return "GameServerChangeRequested";
	case 333: return "GameLobbyJoinRequested";
	case 334: return "AvatarImageLoaded";
	case 337: return "GameRichPresenceJoinRequested";
	case 701: return "IPCountry";
	case 702: return "LowBatteryPower";
	case 703: return "SteamAPICallCompleted";
	case 704: return "SteamShutdown";
	case 1005: return "DlcInstalled";
	case 1014: return "NewUrlLaunchParameters";
	case 1101: return "UserStatsReceived";
	case 1102: return "UserStatsStored";
	case 1103: return "UserAchievementStored";
	case 1104: return "LeaderboardFindResult";
	case 1105: return "LeaderboardScoresDownloaded";
	case 1106: return "LeaderboardScoreUploaded";
	case 1107: return "NumberOfCurrentPlayers";
	case 1112: return "GlobalStatsReceived";
	default: return NULL;
	}
}

static void log_callback(const char *what, void *cb, unsigned long long call)
{
	char buf[256];
	int id = 0;
	unsigned flags = 0;
	const char *name = NULL;

	if (cb && !IsBadReadPtr(cb, 12)) {
		flags = *(unsigned char *)((char *)cb + 4);
		id = *(int *)((char *)cb + 8);
		name = callback_name(id);
	}
	if (name)
		wsprintfA(buf, "%s cb=%p id=%d %s flags=%u call=%08x%08x", what, cb, id, name,
			  flags, (unsigned)(call >> 32), (unsigned)call);
	else
		wsprintfA(buf, "%s cb=%p id=%d flags=%u call=%08x%08x", what, cb, id, flags,
			  (unsigned)(call >> 32), (unsigned)call);
	note(buf);
}

static struct wrap wraps[NIFACE];
static int nwrap;

static void *wrap_iface(const char *name, int which, void *real)
{
	int i;
	char buf[128];

	if (!real)
		return NULL;
	for (i = 0; i < nwrap; i++)
		if (wraps[i].real == real)
			return &wraps[i];
	if (nwrap >= NIFACE) {
		note("too many steam interfaces; returning the real pointer");
		return real;
	}
	wraps[nwrap].vt = thunk_table;
	wraps[nwrap].real = real;
	wraps[nwrap].name = name;
	wraps[nwrap].which = which;
	wsprintfA(buf, "%s -> %p", name, real);
	note(buf);
	return &wraps[nwrap++];
}

/* Called from steam_fwd.S with the wrap pointer and the vtable slot. */
void log_slot(struct wrap *w, int slot)
{
	char buf[160];

	if (!w || slot < 0 || slot >= NSLOT)
		return;
	lock();
	if (w->which >= 0 && w->which < NIFACE && hits[w->which][slot] < 0xffff)
		hits[w->which][slot]++;
	if (hits[w->which][slot] == 1) {
		wsprintfA(buf, "call %s slot %d", w->name, slot);
		note(buf);
	}
	unlock();
}

static void dump_hits(const char *why)
{
	char buf[160];
	int i, s, any = 0;

	if (summary_written && strcmp(why, "shutdown") != 0)
		return;
	lock();
	wsprintfA(buf, "summary (%s)", why);
	note(buf);
	for (i = 0; i < nwrap; i++) {
		for (s = 0; s < NSLOT; s++) {
			if (!hits[i][s])
				continue;
			any = 1;
			wsprintfA(buf, "  %s slot %d  x%u", wraps[i].name, s, hits[i][s]);
			note(buf);
		}
	}
	if (!any)
		note("  (no interface methods)");
	if (strcmp(why, "shutdown") == 0)
		summary_written = 1;
	unlock();
}

int SteamAPI_Init(void)
{
	typedef int (__cdecl *fn_t)(void);
	fn_t fn;
	int r;
	char buf[64];

	er_flush("before SteamAPI_Init");
	fn = (fn_t)real_proc("SteamAPI_Init");
	r = fn ? fn() : 0;
	wsprintfA(buf, "SteamAPI_Init -> %d", r);
	note(buf);
	er_flush("after SteamAPI_Init");
	return r;
}

void SteamAPI_Shutdown(void)
{
	typedef void (__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamAPI_Shutdown");
	note("SteamAPI_Shutdown");
	er_flush("shutdown");
	er_census("at-shutdown", 1);
	if (pl_mode == 1)
		pl_write();
	else if (pl_mode == 2)
		pl_report();
	dump_hits("shutdown");
	if (fn)
		fn();
}

void SteamAPI_RunCallbacks(void)
{
	typedef void (__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamAPI_RunCallbacks");
	run_n++;
	if (run_n == 1 || run_n == 100 || run_n == 1000)
		er_flush(run_n == 1 ? "first RunCallbacks" : "RunCallbacks");
	/* Once a frame. A map change flushes first, so the records before the
	 * boundary are written above its line and the ones after below it. */
	if (er_map && *er_map != er_map_last) {
		unsigned now = *er_map;
		char mb[96];

		er_flush("before map change");
		wsprintfA(mb, "map %d -> %u at +%lums", (int)er_map_last, now,
			  GetTickCount() - er_t0);
		note(mb);
		er_map_last = now;
	}
	if (run_n == 1 || (run_n % 1000) == 0) {
		char buf[64];
		wsprintfA(buf, "SteamAPI_RunCallbacks n=%u", run_n);
		note(buf);
	}
	if (run_n == 100)
		dump_hits("after 100 RunCallbacks");
	if (fn)
		fn();
}

void SteamAPI_RegisterCallResult(void *cb, unsigned long long call)
{
	typedef void (__cdecl *fn_t)(void *, unsigned long long);
	fn_t fn = (fn_t)real_proc("SteamAPI_RegisterCallResult");
	log_callback("RegisterCallResult", cb, call);
	if (fn)
		fn(cb, call);
}

void SteamAPI_UnregisterCallResult(void *cb, unsigned long long call)
{
	typedef void (__cdecl *fn_t)(void *, unsigned long long);
	fn_t fn = (fn_t)real_proc("SteamAPI_UnregisterCallResult");
	log_callback("UnregisterCallResult", cb, call);
	if (fn)
		fn(cb, call);
}

void *SteamApps(void)
{
	typedef void *(__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamApps");
	return wrap_iface("SteamApps", 0, fn ? fn() : NULL);
}

void *SteamFriends(void)
{
	typedef void *(__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamFriends");
	return wrap_iface("SteamFriends", 1, fn ? fn() : NULL);
}

void *SteamUser(void)
{
	typedef void *(__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamUser");
	return wrap_iface("SteamUser", 2, fn ? fn() : NULL);
}

void *SteamUserStats(void)
{
	typedef void *(__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamUserStats");
	return wrap_iface("SteamUserStats", 3, fn ? fn() : NULL);
}

void *SteamUtils(void)
{
	typedef void *(__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamUtils");
	return wrap_iface("SteamUtils", 4, fn ? fn() : NULL);
}
