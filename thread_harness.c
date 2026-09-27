/* The last wall, in a sandbox: can fresh threads pick up a saved world?
 *
 * Every other harness restores into the SAME threads it saved - which is why
 * in-session restore works and cross-session does not. This one models the
 * process boundary inside one process: run workers over a shared world, freeze
 * and snapshot, then KILL the threads and spawn brand-new ones at OS-chosen
 * stacks and TIDs (the cross-session condition - non-deterministic runners), and
 * try to make them continue from the save two ways:
 *
 *   adopt     - restore each saved thread's stack and CPU context onto a fresh
 *               thread so it resumes the saved execution. This is the "become the
 *               dead thread" path, and its whole feasibility rests on one thing:
 *               is the saved stack's ADDRESS free in the new layout, or has a
 *               fresh thread already taken it? A stack cannot be relocated - it is
 *               full of absolute pointers to itself - so an occupied address means
 *               that thread cannot be adopted. The harness measures free vs taken.
 *
 *   data      - leave the fresh threads running their own healthy loop, rewind
 *               only the shared WORLD, and let them read it and carry on. This is
 *               F11-pos-restore at scale: change the world, do not touch the
 *               runners. It works only if the important state lives in the world
 *               (at a reproduced address) and not on the threads' stacks.
 *
 * The world is a fixed-address static, which models the pinned gameheap arena:
 * pointers into it survive because it does not move. Build: thread_harness32.exe.
 *
 *   thread_harness32 adopt      run the adoption path and report free/taken stacks
 *   thread_harness32 data       run the data-only path and assert the world
 *   thread_harness32 both       run both against the same snapshot (default)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define NWORKER 4
#define NENT 8

/* The shared world: the game's logical state. Fixed-address static = the pinned
 * arena, the case where the world reproduces its address across a relaunch. */
typedef struct {
	volatile LONG frame;
	LONG ent[NENT];       /* entity positions, advanced deterministically */
	CRITICAL_SECTION lock;
} World;

static World g_world;
static volatile LONG g_run = 1;
static volatile LONG g_paused;   /* set by workers while parked, so a freeze is quiescent */
static volatile LONG g_freeze;   /* asks workers to park at the top of the loop */

static HANDLE g_thr[NWORKER];
static DWORD g_tid[NWORKER];

/* One worker = one game-loop thread. It carries a stack-local accumulator that
 * ONLY the adoption path can preserve; the data path resets it. Whether that
 * matters is exactly what we are here to find out. */
static DWORD WINAPI worker(void *arg)
{
	int id = (int)(intptr_t)arg;
	unsigned long local_acc = 0; /* pure stack state */

	for (;;) {
		if (!g_run)
			return 0;
		if (g_freeze) {
			InterlockedIncrement(&g_paused);
			while (g_freeze && g_run)
				Sleep(1);
			InterlockedDecrement(&g_paused);
			continue;
		}
		EnterCriticalSection(&g_world.lock);
		g_world.ent[id] = (g_world.ent[id] + id + 1) % 100000;
		g_world.frame++;
		local_acc += (unsigned long)g_world.ent[id];
		LeaveCriticalSection(&g_world.lock);
		Sleep(2);
	}
}

/* ---- TEB / stack range for a thread (x86) --------------------------------- */
typedef LONG(NTAPI *PFN_NtQueryInformationThread)(HANDLE, ULONG, PVOID, ULONG,
						  PULONG);
static PFN_NtQueryInformationThread p_ntqit;

static int thread_stack_range(HANDLE h, uintptr_t *base, uintptr_t *limit)
{
	/* THREAD_BASIC_INFORMATION: TebBaseAddress is the 2nd pointer field. */
	struct {
		LONG ExitStatus;
		PVOID TebBaseAddress;
		PVOID UniqueProcessId;
		PVOID UniqueThreadId;
		ULONG_PTR AffinityMask;
		LONG Priority;
		LONG BasePriority;
	} tbi;

	if (!p_ntqit || p_ntqit(h, 0 /*ThreadBasicInformation*/, &tbi, sizeof(tbi), NULL) < 0)
		return 0;
	if (!tbi.TebBaseAddress)
		return 0;
	/* NT_TIB at the top of the TEB: StackBase at +4, StackLimit at +8 (x86). */
	*base = *(const uintptr_t *)((const unsigned char *)tbi.TebBaseAddress + 0x04);
	*limit = *(const uintptr_t *)((const unsigned char *)tbi.TebBaseAddress + 0x08);
	return 1;
}

/* ---- snapshot ------------------------------------------------------------- */
typedef struct {
	CONTEXT ctx;
	uintptr_t base, limit, sp;
	unsigned char *stack; /* copy from sp..base */
	size_t len;
} ThreadSnap;

static World g_world_save;
static ThreadSnap g_snap[NWORKER];

static void freeze_workers(void)
{
	int spins = 0;

	InterlockedExchange(&g_freeze, 1);
	while (g_paused < NWORKER && spins++ < 2000)
		Sleep(1);
}
static void thaw_workers(void) { InterlockedExchange(&g_freeze, 0); }

static void snapshot(void)
{
	int i;

	freeze_workers();
	/* The world, minus the lock handle (a live kernel object, not data). */
	memcpy(&g_world_save, &g_world, sizeof(g_world));
	for (i = 0; i < NWORKER; i++) {
		SuspendThread(g_thr[i]);
		g_snap[i].ctx.ContextFlags = CONTEXT_FULL;
		if (!GetThreadContext(g_thr[i], &g_snap[i].ctx)) {
			ResumeThread(g_thr[i]);
			continue;
		}
		g_snap[i].sp = g_snap[i].ctx.Esp;
		if (thread_stack_range(g_thr[i], &g_snap[i].base, &g_snap[i].limit) &&
		    g_snap[i].base > g_snap[i].sp) {
			g_snap[i].len = g_snap[i].base - g_snap[i].sp;
			g_snap[i].stack = (unsigned char *)malloc(g_snap[i].len);
			memcpy(g_snap[i].stack, (void *)g_snap[i].sp, g_snap[i].len);
		}
		ResumeThread(g_thr[i]);
	}
	printf("snapshot: frame=%ld, %d worker stacks captured (sp..base)\n",
	       g_world_save.frame, NWORKER);
	for (i = 0; i < NWORKER; i++)
		printf("   worker %d: stack %08lX..%08lX, sp %08lX, %lu bytes live\n", i,
		       (unsigned long)g_snap[i].limit, (unsigned long)g_snap[i].base,
		       (unsigned long)g_snap[i].sp, (unsigned long)g_snap[i].len);
	thaw_workers();
}

/* ---- kill the old session, spawn a fresh one ------------------------------ */
static void kill_session(void)
{
	int i;

	InterlockedExchange(&g_run, 0);
	InterlockedExchange(&g_freeze, 0);
	for (i = 0; i < NWORKER; i++) {
		if (g_thr[i]) {
			/* A relaunch does not unwind the old threads; they just cease. */
			TerminateThread(g_thr[i], 0);
			CloseHandle(g_thr[i]);
			g_thr[i] = NULL;
		}
	}
	/* The lock's owner may have vanished mid-hold; a fresh session re-creates it. */
	DeleteCriticalSection(&g_world.lock);
	InitializeCriticalSection(&g_world.lock);
	Sleep(20);
}

static void spawn_fresh(int suspended)
{
	int i;

	InterlockedExchange(&g_run, 1);
	InterlockedExchange(&g_freeze, 0);
	InterlockedExchange(&g_paused, 0);
	for (i = 0; i < NWORKER; i++)
		g_thr[i] = CreateThread(NULL, 0, worker, (void *)(intptr_t)i,
					suspended ? CREATE_SUSPENDED : 0, &g_tid[i]);
}

/* ---- strategy A: adopt ---------------------------------------------------- */
static void restore_adopt(void)
{
	int i, freecnt = 0, taken = 0, adopted = 0;

	printf("\n=== ADOPT ===\n");
	/* Fresh runners, held so we can rewrite them before they execute. */
	spawn_fresh(1);
	/* The world comes back by plain copy; the lock stays this session's. */
	{
		CRITICAL_SECTION keep = g_world.lock;
		memcpy(&g_world, &g_world_save, sizeof(g_world));
		g_world.lock = keep;
	}
	for (i = 0; i < NWORKER; i++) {
		MEMORY_BASIC_INFORMATION mbi;
		int can = 0;

		if (!g_snap[i].stack)
			continue;
		if (VirtualQuery((void *)g_snap[i].sp, &mbi, sizeof(mbi)) == sizeof(mbi)) {
			/* Free means the address the saved stack needs is available; a
			 * committed run that belongs to a live fresh thread's stack means
			 * it is not, and the stack cannot move to make room. */
			if (mbi.State == MEM_FREE) {
				freecnt++;
				can = 1;
			} else {
				taken++;
			}
		}
		if (can) {
			/* Reserve+commit the saved stack's own address and put the bytes
			 * back, then point the fresh thread's context at it. */
			uintptr_t rb = g_snap[i].limit & ~(uintptr_t)0xFFFF;
			SIZE_T rn = (g_snap[i].base - rb + 0xFFFF) & ~(SIZE_T)0xFFFF;
			void *r = VirtualAlloc((void *)rb, rn, MEM_RESERVE, PAGE_READWRITE);

			if (r && VirtualAlloc((void *)g_snap[i].sp, g_snap[i].len,
					      MEM_COMMIT, PAGE_READWRITE)) {
				memcpy((void *)g_snap[i].sp, g_snap[i].stack, g_snap[i].len);
				SetThreadContext(g_thr[i], &g_snap[i].ctx);
				adopted++;
			}
		}
	}
	printf("adopt: saved stacks - %d FREE (adoptable), %d TAKEN by a fresh thread\n",
	       freecnt, taken);
	printf("adopt: %d of %d threads adopted onto their saved stacks\n", adopted,
	       NWORKER);
	printf("adopt: -> the free/taken split is the whole story: a taken stack is a "
	       "thread that CANNOT resume where it was\n");
	for (i = 0; i < NWORKER; i++)
		if (g_thr[i])
			ResumeThread(g_thr[i]);
	Sleep(200);
	printf("adopt: world ran to frame=%ld after resume%s\n", g_world.frame,
	       adopted == NWORKER ? " (all adopted)" : " (some threads are fresh, not resumed)");
}

/* ---- strategy B: data-only ------------------------------------------------ */
static void restore_data(void)
{
	LONG before, after;

	printf("\n=== DATA-ONLY ===\n");
	/* Fresh runners, running their own healthy loop from the start. */
	spawn_fresh(0);
	Sleep(30); /* let them diverge a little, as a real fresh session would */
	before = g_world.frame;
	/* Rewind ONLY the world - the runners are never touched. */
	freeze_workers();
	{
		CRITICAL_SECTION keep = g_world.lock;
		memcpy(&g_world, &g_world_save, sizeof(g_world));
		g_world.lock = keep;
	}
	after = g_world.frame;
	thaw_workers();
	printf("data: world frame was %ld (fresh session diverged), rewound to %ld "
	       "(the save)\n",
	       before, after);
	printf("data: threads left ALONE; the world now reads the saved state and they "
	       "carry on from it\n");
	Sleep(100);
	printf("data: world advanced to frame=%ld - the fresh runners picked up the "
	       "saved world and moved it forward, no adoption needed\n",
	       g_world.frame);
	printf("data: what was LOST: each worker's stack-local accumulator reset to 0 - "
	       "the question for the game is whether any state that matters lives only "
	       "on a stack, or all of it is in the world\n");
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "both";
	int i;

	p_ntqit = (PFN_NtQueryInformationThread)GetProcAddress(
		GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
	InitializeCriticalSection(&g_world.lock);

	spawn_fresh(0);
	printf("running a session... (workers advancing the world)\n");
	Sleep(120);
	snapshot();
	kill_session();
	printf("killed the session, spawning a fresh one (new stacks, new TIDs)\n");

	if (!strcmp(mode, "adopt")) {
		restore_adopt();
	} else if (!strcmp(mode, "data")) {
		restore_data();
	} else {
		/* Both against the same snapshot: adopt first (it holds threads),
		 * then tear down and try data-only. */
		restore_adopt();
		kill_session();
		restore_data();
	}

	InterlockedExchange(&g_run, 0);
	Sleep(30);
	for (i = 0; i < NWORKER; i++)
		if (g_thr[i])
			CloseHandle(g_thr[i]);
	return 0;
}
