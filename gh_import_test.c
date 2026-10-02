/* Stand-in for a game that takes its C runtime from ucrtbase, for the import
 * mode of gameheap.c (D3D9SW_GAMEHEAP=2). Built twice: as gh_late.dll with
 * -DGH_LATE (loaded after the wrapper, so it exercises the load notification)
 * and as the exe. Run from a folder holding opengl32.dll; prints PASS/FAIL. */
#include <windows.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GH_LATE
static void *g_ctor_block;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID r)
{
	(void)inst;
	(void)r;
	if (reason == DLL_PROCESS_ATTACH)
		g_ctor_block = malloc(100);
	return TRUE;
}

__declspec(dllexport) void *late_ctor_block(void)
{
	return g_ctor_block;
}

__declspec(dllexport) void *late_malloc(size_t n)
{
	return malloc(n);
}

__declspec(dllexport) void late_free(void *p)
{
	free(p);
}
#else
static int g_fail;

#define CHECK(c, ...)                                                                              \
	do {                                                                                       \
		if (!(c)) {                                                                        \
			g_fail++;                                                                  \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                \
			printf(__VA_ARGS__);                                                       \
			printf("\n");                                                              \
		}                                                                                  \
	} while (0)

/* Ours means: not in any segment of the process heap. */
static int on_process_heap(void *p)
{
	PROCESS_HEAP_ENTRY e;
	HANDLE h = GetProcessHeap();
	int hit = 0;

	e.lpData = NULL;
	HeapLock(h);
	while (HeapWalk(h, &e))
		if ((e.wFlags & PROCESS_HEAP_ENTRY_BUSY) && (char *)p >= (char *)e.lpData &&
		    (char *)p < (char *)e.lpData + e.cbData) {
			hit = 1;
			break;
		}
	HeapUnlock(h);
	return hit;
}

static DWORD WINAPI churn(LPVOID arg)
{
	unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u;
	void *keep[64] = { 0 };
	int i;

	for (i = 0; i < 200000; i++) {
		int k;
		size_t n;

		seed = seed * 1103515245u + 12345u;
		k = (seed >> 8) & 63;
		n = (seed >> 16) & 1 ? (seed >> 4) % 4096 + 1
				     : ((seed >> 20) & 7) == 0 ? (seed % (3u << 20)) + (256u << 10)
								: (seed >> 3) % 200000 + 1;
		if (keep[k]) {
			if ((seed >> 24) & 1)
				keep[k] = realloc(keep[k], n);
			else {
				free(keep[k]);
				keep[k] = NULL;
			}
		} else {
			keep[k] = malloc(n);
			if (keep[k])
				memset(keep[k], k, n < 64 ? n : 64);
		}
	}
	for (i = 0; i < 64; i++)
		free(keep[i]);
	return 0;
}

int main(void)
{
	void *(__cdecl * u_malloc)(size_t);
	void(__cdecl * u_free)(void *);
	void *(__cdecl * u_realloc)(void *, size_t);
	void *(__cdecl * u_amalloc)(size_t, size_t);
	void(__cdecl * u_afree)(void *);
	HMODULE ucrt = GetModuleHandleA("ucrtbase.dll"), gl, late;
	char *before, *p, *big, *q;
	void *a, *b;
	HANDLE th[4];
	int i;

	u_malloc = (void *(__cdecl *)(size_t))GetProcAddress(ucrt, "malloc");
	u_free = (void(__cdecl *)(void *))GetProcAddress(ucrt, "free");
	u_realloc = (void *(__cdecl *)(void *, size_t))GetProcAddress(ucrt, "realloc");
	u_amalloc = (void *(__cdecl *)(size_t, size_t))GetProcAddress(ucrt, "_aligned_malloc");
	u_afree = (void(__cdecl *)(void *))GetProcAddress(ucrt, "_aligned_free");

	before = malloc(40);
	strcpy(before, "allocated before the install");
	a = _aligned_malloc(100, 64);
	CHECK(on_process_heap(before), "pre-install block should be the runtime's");

	SetEnvironmentVariableA("D3D9SW_GAMEHEAP", "2");
	gl = LoadLibraryA("opengl32.dll");
	CHECK(gl != NULL, "opengl32.dll did not load (%lu)", GetLastError());

	p = malloc(100);
	CHECK(p && !on_process_heap(p), "small malloc %p is still on the process heap", p);
	big = malloc(5u << 20);
	CHECK(big && !on_process_heap(big), "5 MB malloc %p not ours", big);
	memset(big, 0xAB, 5u << 20);
	q = calloc(1, 300u << 10);
	CHECK(q && q[0] == 0 && q[(300u << 10) - 1] == 0, "big calloc not zeroed");
	free(q);

	/* migration on realloc */
	before = realloc(before, 4000);
	CHECK(before && !on_process_heap(before) && !strcmp(before, "allocated before the install"),
	      "pre-install block was not migrated intact");
	free(before);

	/* the runtime frees ours (floor), ours frees the runtime's */
	u_free(p);
	q = u_malloc(64);
	free(q);
	q = malloc(64);
	q = u_realloc(q, 100000);
	CHECK(q && !on_process_heap(q), "ucrt realloc of our block left our heap");
	q = u_realloc(q, 2u << 20);
	CHECK(q && !on_process_heap(q), "ucrt realloc to 2 MB left our heap");
	CHECK(_msize(q) == (2u << 20), "_msize after grow is %u", (unsigned)_msize(q));
	free(q);
	big = realloc(big, 9u << 20);
	CHECK(big && (unsigned char)big[(5u << 20) - 1] == 0xAB, "big realloc lost contents");
	free(big);

	/* aligned, both directions */
	b = _aligned_malloc(1000, 64);
	CHECK(b && ((uintptr_t)b & 63) == 0, "aligned block %p misaligned", b);
	CHECK(!on_process_heap(b), "aligned block not ours");
	u_afree(b);
	_aligned_free(a);
	a = u_amalloc(100, 32);
	_aligned_free(a);
	b = _aligned_malloc(1u << 20, 4096);
	CHECK(b && ((uintptr_t)b & 4095) == 0 && !on_process_heap(b), "big aligned wrong");
	_aligned_free(b);

	/* a module loaded later */
	late = LoadLibraryA("gh_late.dll");
	CHECK(late != NULL, "gh_late.dll did not load");
	if (late) {
		void *(*ctor)(void) = (void *(*)(void))GetProcAddress(late, "late_ctor_block");
		void *(*lm)(size_t) = (void *(*)(size_t))GetProcAddress(late, "late_malloc");
		void (*lf)(void *) = (void (*)(void *))GetProcAddress(late, "late_free");
		void *c = ctor(), *m = lm(77);

		CHECK(c && !on_process_heap(c), "late DLL's DllMain allocation %p not ours", c);
		CHECK(m && !on_process_heap(m), "late DLL's malloc not ours");
		free(m);
		m = malloc(55);
		lf(m);
	}

	for (i = 0; i < 4; i++)
		th[i] = CreateThread(NULL, 0, churn, (LPVOID)(uintptr_t)(i + 1), 0, NULL);
	WaitForMultipleObjects(4, th, TRUE, INFINITE);
	CHECK(HeapValidate(GetProcessHeap(), 0, NULL), "process heap corrupt after the run");
	{
		HANDLE hs[64];
		DWORD n = GetProcessHeaps(64, hs), k;

		for (k = 0; k < n; k++)
			CHECK(HeapValidate(hs[k], 0, NULL), "heap %p corrupt", hs[k]);
	}
	printf(g_fail ? "%d FAILURE(S)\n" : "PASS\n", g_fail);
	return g_fail != 0;
}
#endif
