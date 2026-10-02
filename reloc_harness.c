/* Regression for RELOC matching modules by base name.
 *
 * Rabi-Ribi holds two d3d11.dll and two dxgi.dll: ours, pinned, and the system
 * copies the GPU probe loads. RELOC used to look each saved module up with
 * GetModuleHandleA(name), which only ever answers with the first one loaded, so
 * the second looked moved in a same-process restore and 3527 words of game
 * memory were shifted by -227 MB.
 *
 * This loads reloc_dup.dll from two folders, keeps a pointer into the SECOND
 * copy in ordinary rewound data, saves, restores in the same process with
 * D3D9SW_RELOC=1, and checks that the pointer came back unchanged and the log
 * names no shift and no tripwire. Under the old matcher the pointer moves.
 *
 * Usage: reloc_harness32.exe   (reloc_dup.dll next to it)
 * Exit 0 on pass, 1 on fail, 2 on setup failure. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "savestate.h"

typedef struct {
	int after_restore;
	uintptr_t expect;
} Held;

/* No rasteriser here, so none of its workers can be running - the same stub
 * every harness carries. */
void swrast_pool_shutdown(void)
{
}

static Held *g_held;
/* Rewound with the image, so a shift by RELOC shows up as a different value. */
static volatile uintptr_t g_ptr_into_b;

static int stage_copy(const char *dir, const char *src, char *out, size_t cap)
{
	_snprintf(out, cap, "%s\\reloc_dup.dll", dir);
	out[cap - 1] = 0;
	CreateDirectoryA(dir, NULL);
	return CopyFileA(src, out, FALSE) != 0;
}

/* The log lines written by the most recent load, from its provenance line on. */
static int check_log(void)
{
	char exe[MAX_PATH], name[MAX_PATH + 32], *base, *p, *buf, *from, *hit;
	HANDLE f;
	DWORD size, got = 0;
	int ok = 1;

	GetModuleFileNameA(NULL, exe, sizeof(exe));
	base = strrchr(exe, '\\');
	base = base ? base + 1 : exe;
	if ((p = strchr(base, '.')) != NULL)
		*p = 0;
	_snprintf(name, sizeof(name), "d3d9_sw_savestate_%s.txt", base);
	f = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		printf("FAIL: cannot open %s\n", name);
		return 0;
	}
	size = GetFileSize(f, NULL);
	buf = (char *)malloc(size + 1);
	if (!buf || !ReadFile(f, buf, size, &got, NULL)) {
		CloseHandle(f);
		printf("FAIL: cannot read %s\n", name);
		return 0;
	}
	CloseHandle(f);
	buf[got] = 0;
	from = NULL;
	for (hit = strstr(buf, "provenance:"); hit; hit = strstr(hit + 1, "provenance:"))
		from = hit;
	if (!from) {
		printf("FAIL: no provenance line - the load did not log where it stands\n");
		free(buf);
		return 0;
	}
	if (strstr(from, "TRIPWIRE")) {
		printf("FAIL: tripwire fired - identity matched the wrong module\n");
		ok = 0;
	}
	if (strstr(from, "shifted across")) {
		printf("FAIL: RELOC shifted words in a same-process restore\n");
		ok = 0;
	}
	if (!strstr(from, "no module base moved")) {
		printf("FAIL: RELOC did not report that nothing moved\n");
		ok = 0;
	}
	/* Checked on the last reloc line, which is read after the copy: provenance kept
	 * where the restore winds it back reads as unknown there, and then the
	 * tripwire cannot fire. */
	hit = NULL;
	for (p = strstr(from, "  reloc:"); p; p = strstr(p + 1, "  reloc:"))
		hit = p;
	if (hit && (p = strchr(hit, '\n')) != NULL)
		*p = 0;
	if (!hit || !strstr(hit, "same process")) {
		printf("FAIL: RELOC did not see this load as the same process - "
		       "provenance was lost across the copy\n");
		ok = 0;
	}
	free(buf);
	return ok;
}

int main(void)
{
	char exe[MAX_PATH], dir[MAX_PATH], src[MAX_PATH], da[MAX_PATH], db[MAX_PATH];
	char pa[MAX_PATH], pb[MAX_PATH], tmp[MAX_PATH], *slash;
	HMODULE a, b, byname;
	uintptr_t marker;

	setvbuf(stdout, NULL, _IONBF, 0);
	g_held = (Held *)VirtualAlloc(NULL, sizeof(Held), MEM_COMMIT | MEM_RESERVE,
				      PAGE_READWRITE);
	if (!g_held)
		return 2;
	SetEnvironmentVariableA("D3D9SW_RELOC", "1");
	savestate_exclude(g_held, sizeof(Held));
	savestate_hooks_install();

	GetModuleFileNameA(NULL, exe, sizeof(exe));
	lstrcpynA(dir, exe, sizeof(dir));
	slash = strrchr(dir, '\\');
	if (slash)
		*slash = 0;
	_snprintf(src, sizeof(src), "%s\\reloc_dup.dll", dir);
	if (!GetTempPathA(sizeof(tmp), tmp))
		lstrcpynA(tmp, dir, sizeof(tmp));
	_snprintf(da, sizeof(da), "%sreloc_a", tmp);
	_snprintf(db, sizeof(db), "%sreloc_b", tmp);
	if (!stage_copy(da, src, pa, sizeof(pa)) || !stage_copy(db, src, pb, sizeof(pb))) {
		printf("SETUP: cannot stage %s into two folders\n", src);
		return 2;
	}
	a = LoadLibraryA(pa);
	b = LoadLibraryA(pb);
	byname = GetModuleHandleA("reloc_dup.dll");
	if (!a || !b || a == b) {
		printf("SETUP: expected two distinct copies, got %p and %p\n", (void *)a,
		       (void *)b);
		return 2;
	}
	printf("two copies of reloc_dup.dll: %p and %p; by name the loader answers %p\n",
	       (void *)a, (void *)b, (void *)byname);
	if (byname != a)
		printf("  (by-name answer is not the first copy - the old bug would hit "
		       "the other one, the test still holds)\n");

	marker = (uintptr_t)GetProcAddress(b, "reloc_dup_marker");
	if (!marker) {
		printf("SETUP: no marker export in the second copy\n");
		return 2;
	}
	g_ptr_into_b = marker;
	g_held->expect = marker;

	if (!savestate_save(0)) {
		printf("SETUP: save refused\n");
		return 2;
	}
	if (!savestate_last_was_restore()) {
		g_held->after_restore = 1;
		savestate_load(0);
		printf("FAIL: savestate_load returned - the restore did not happen\n");
		return 1;
	}

	printf("restored: pointer into the second copy %p, expected %p\n",
	       (void *)g_ptr_into_b, (void *)g_held->expect);
	if (g_ptr_into_b != g_held->expect) {
		printf("FAIL: RELOC moved a pointer into a module that never moved\n");
		return 1;
	}
	if (!check_log())
		return 1;
	printf("PASS: same name from two folders, same-process restore shifted nothing\n");
	return 0;
}
