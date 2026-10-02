/* Launch a process inside a job that dies with this process.
 *
 *   pico_job.exe --selftest
 *   pico_job.exe <exe> [args...]
 *
 * The job is armed before the child is resumed:
 *   JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
 *   breakaway left disabled (neither BREAKAWAY_OK nor SILENT_BREAKAWAY_OK)
 *
 * There is no active-process limit. rabiribi.exe is a Steam stub: it
 * CreateProcess's the real game and exits 0x35. A limit of 1 refuses that
 * second process, so the game never appears, and waiting on the stub then
 * closing the job would kill the game the stub just started. Children stay
 * in the job because breakaway is off, and this process waits until the job
 * is empty. Closing this process still kills whatever is left in it.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

static int arm_job(HANDLE job)
{
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION got;

	memset(&lim, 0, sizeof(lim));
	lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim))) {
		fprintf(stderr, "SetInformationJobObject failed (%lu)\n", GetLastError());
		return 0;
	}
	memset(&got, 0, sizeof(got));
	if (!QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &got, sizeof(got), NULL)) {
		fprintf(stderr, "QueryInformationJobObject failed (%lu)\n", GetLastError());
		return 0;
	}
	if (!(got.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) ||
	    (got.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_BREAKAWAY_OK) ||
	    (got.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)) {
		fprintf(stderr, "job flags did not stick: %08lX\n",
			got.BasicLimitInformation.LimitFlags);
		return 0;
	}
	return 1;
}

/* Suspended, so nothing runs before AssignProcessToJobObject. */
static int spawn_suspended(const wchar_t *cmdline, const wchar_t *cwd, PROCESS_INFORMATION *pi)
{
	STARTUPINFOW si;
	wchar_t buf[32768];

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	memset(pi, 0, sizeof(*pi));
	lstrcpynW(buf, cmdline, 32768);
	return CreateProcessW(NULL, buf, NULL, NULL, FALSE,
			      CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
			      NULL, cwd, &si, pi);
}

/* The image base name for a pid, empty if it cannot be opened (already gone,
 * or ours to see). Uses QueryFullProcessImageNameW so no psapi link is needed. */
static void proc_name(DWORD pid, char *out, size_t cap)
{
	HANDLE h;
	wchar_t wpath[MAX_PATH];
	DWORD n = MAX_PATH;
	wchar_t *base;

	out[0] = 0;
	h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h)
		return;
	if (QueryFullProcessImageNameW(h, 0, wpath, &n)) {
		base = wcsrchr(wpath, L'\\');
		base = base ? base + 1 : wpath;
		WideCharToMultiByte(CP_ACP, 0, base, -1, out, (int)cap, NULL, NULL);
	}
	CloseHandle(h);
}

/* Block until nothing is left in the job, narrating what enters and leaves it.
 *
 * The point of the narration is the re-exec question: a Steam game not launched
 * by the client asks steam.exe to relaunch it, and steam.exe is not in this job,
 * so the real game spawns OUTSIDE the cage with a pid we never see. When that
 * happens this loop reports only the stub passing through and then an empty job,
 * while Task Manager still shows a live rabiribi.exe on a pid that appears
 * nowhere below. That mismatch is the whole diagnosis. */
#define MAX_SEEN 64
static void wait_until_empty(HANDLE job, DWORD launched_pid, HANDLE launched_h)
{
	HANDLE port;
	JOBOBJECT_ASSOCIATE_COMPLETION_PORT assoc;
	DWORD seen_pid[MAX_SEEN];
	HANDLE seen_h[MAX_SEEN];
	int nseen = 0, i;
	DWORD start = GetTickCount();

	port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
	if (!port)
		return;
	assoc.CompletionKey = job;
	assoc.CompletionPort = port;
	if (!SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation,
				     &assoc, sizeof(assoc))) {
		CloseHandle(port);
		return;
	}
	for (;;) {
		DWORD bytes = 0;
		ULONG_PTR key = 0;
		LPOVERLAPPED ov = NULL;
		JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;

		memset(&acct, 0, sizeof(acct));
		if (QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
					      &acct, sizeof(acct), NULL) &&
		    acct.ActiveProcesses == 0)
			break;
		if (!GetQueuedCompletionStatus(port, &bytes, &key, &ov, 500))
			continue;
		if (bytes == JOB_OBJECT_MSG_NEW_PROCESS) {
			DWORD npid = (DWORD)(ULONG_PTR)ov;
			char name[MAX_PATH];

			proc_name(npid, name, sizeof(name));
			printf("  entered job: pid %lu %s%s\n", npid,
			       name[0] ? name : "(name?)",
			       npid == launched_pid ? "   <- the pid we launched" : "");
			if (nseen < MAX_SEEN) {
				seen_pid[nseen] = npid;
				seen_h[nseen] = OpenProcess(
					PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
					FALSE, npid);
				nseen++;
			}
			continue;
		}
		if (bytes == JOB_OBJECT_MSG_EXIT_PROCESS ||
		    bytes == JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS) {
			DWORD xpid = (DWORD)(ULONG_PTR)ov;
			DWORD code = 0;
			HANDLE h = NULL;

			if (xpid == launched_pid)
				h = launched_h;
			else
				for (i = 0; i < nseen; i++)
					if (seen_pid[i] == xpid) {
						h = seen_h[i];
						break;
					}
			if (h)
				GetExitCodeProcess(h, &code);
			printf("  left job:    pid %lu exit 0x%lX%s%s\n", xpid, code,
			       h ? "" : " (code unknown)",
			       bytes == JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS ? " (abnormal)"
									    : "");
			continue;
		}
		if (bytes == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO)
			break;
	}
	if (launched_pid) {
		printf("job drained after %lu ms. launched/held pid was %lu; "
		       "%d process(es) entered the job after association.\n",
		       GetTickCount() - start, launched_pid, nseen);
		printf("If Task Manager still shows a live rabiribi.exe whose pid is neither "
		       "%lu nor any 'entered job' pid above, the game re-exec'd out of the "
		       "cage: steam.exe spawned it, so it never joined the job.\n",
		       launched_pid);
	} else {
		printf("job drained after %lu ms; the cage is empty.\n",
		       GetTickCount() - start);
	}
	for (i = 0; i < nseen; i++)
		if (seen_h[i])
			CloseHandle(seen_h[i]);
	CloseHandle(port);
}

static int selftest(void)
{
	wchar_t self_dir[MAX_PATH], sleeper[MAX_PATH], cmd[MAX_PATH * 2], result[MAX_PATH];
	wchar_t *slash;
	HANDLE job;
	PROCESS_INFORMATION a, b;
	DWORD wait, code;
	char text[64];
	DWORD nread;
	HANDLE rf;
	int pass = 1;

	GetModuleFileNameW(NULL, self_dir, MAX_PATH);
	slash = wcsrchr(self_dir, L'\\');
	if (!slash) {
		fprintf(stderr, "no directory for sleeper\n");
		return 1;
	}
	*slash = 0;
	wsprintfW(sleeper, L"%s\\pico_sleep.exe", self_dir);
	wsprintfW(result, L"%s\\pico_job_breakaway.txt", self_dir);

	job = CreateJobObjectW(NULL, NULL);
	if (!job || !arm_job(job))
		return 1;
	printf("job armed: kill-on-close, breakaway off\n");

	wsprintfW(cmd, L"\"%s\" --breakaway %s", sleeper, result);
	DeleteFileW(result);
	if (!spawn_suspended(cmd, NULL, &a)) {
		fprintf(stderr, "spawn sleeper failed (%lu)\n", GetLastError());
		CloseHandle(job);
		return 1;
	}
	if (!AssignProcessToJobObject(job, a.hProcess)) {
		fprintf(stderr, "AssignProcessToJobObject failed (%lu). "
			"The launcher is probably already inside a job that "
			"does not allow a nested one.\n",
			GetLastError());
		TerminateProcess(a.hProcess, 1);
		CloseHandle(a.hThread);
		CloseHandle(a.hProcess);
		CloseHandle(job);
		return 1;
	}
	ResumeThread(a.hThread);
	if (WaitForSingleObject(a.hProcess, 5000) != WAIT_OBJECT_0) {
		fprintf(stderr, "FAIL breakaway probe did not exit\n");
		pass = 0;
	} else {
		rf = CreateFileW(result, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		text[0] = 0;
		if (rf != INVALID_HANDLE_VALUE) {
			ReadFile(rf, text, sizeof(text) - 1, &nread, NULL);
			text[nread] = 0;
			CloseHandle(rf);
		}
		if (strcmp(text, "breakaway-refused\n") == 0)
			printf("PASS breakaway refused\n");
		else {
			printf("FAIL breakaway result: %s\n", text[0] ? text : "(no file)");
			pass = 0;
		}
	}
	CloseHandle(a.hThread);
	CloseHandle(a.hProcess);

	wsprintfW(cmd, L"\"%s\" --nest %s", sleeper, result);
	DeleteFileW(result);
	if (!spawn_suspended(cmd, NULL, &a) || !AssignProcessToJobObject(job, a.hProcess)) {
		fprintf(stderr, "FAIL could not place the sleeper in the job (%lu)\n", GetLastError());
		CloseHandle(job);
		return 1;
	}
	ResumeThread(a.hThread);
	{
		int spins;
		JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;
		DWORD child = 0;

		text[0] = 0;
		for (spins = 0; spins < 50 && !text[0]; spins++) {
			Sleep(100);
			rf = CreateFileW(result, GENERIC_READ, FILE_SHARE_READ, NULL,
					 OPEN_EXISTING, 0, NULL);
			if (rf == INVALID_HANDLE_VALUE)
				continue;
			ReadFile(rf, text, sizeof(text) - 1, &nread, NULL);
			text[nread] = 0;
			CloseHandle(rf);
		}
		if (sscanf(text, "child %lu", &child) != 1) {
			printf("FAIL nest did not report a child (%s)\n", text[0] ? text : "no file");
			pass = 0;
		}
		memset(&acct, 0, sizeof(acct));
		QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
					  &acct, sizeof(acct), NULL);
		if (acct.ActiveProcesses >= 2)
			printf("PASS child stayed in the job (%lu processes)\n", acct.ActiveProcesses);
		else {
			printf("FAIL job holds %lu process(es), wanted the parent and its child\n",
			       acct.ActiveProcesses);
			pass = 0;
		}
		CloseHandle(job);
		job = NULL;
		wait = WaitForSingleObject(a.hProcess, 3000);
		if (wait != WAIT_OBJECT_0) {
			printf("FAIL parent still alive after the job handle was closed\n");
			TerminateProcess(a.hProcess, 1);
			pass = 0;
		}
		if (child) {
			HANDLE cp = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child);
			if (!cp)
				printf("PASS child is gone\n");
			else {
				DWORD cwait = WaitForSingleObject(cp, 3000);
				CloseHandle(cp);
				if (cwait == WAIT_OBJECT_0)
					printf("PASS closing the job killed the child\n");
				else {
					printf("FAIL child %lu survived the job\n", child);
					pass = 0;
				}
			}
		}
	}
	CloseHandle(a.hThread);
	CloseHandle(a.hProcess);
	(void)b;
	(void)code;
	DeleteFileW(result);
	printf("%s\n", pass ? "SELFTEST PASS" : "SELFTEST FAIL");
	return pass ? 0 : 1;
}

static int launch(int argc, wchar_t **argv)
{
	HANDLE job;
	PROCESS_INFORMATION pi;
	wchar_t cmd[32768];
	int i;
	size_t used = 0;

	cmd[0] = 0;
	for (i = 1; i < argc; i++) {
		size_t len = wcslen(argv[i]);
		int quote = wcschr(argv[i], L' ') != NULL;
		if (used + len + 4 >= 32768) {
			fprintf(stderr, "command line too long\n");
			return 1;
		}
		if (i > 1)
			cmd[used++] = L' ';
		if (quote)
			cmd[used++] = L'"';
		memcpy(cmd + used, argv[i], len * sizeof(wchar_t));
		used += len;
		if (quote)
			cmd[used++] = L'"';
		cmd[used] = 0;
	}

	job = CreateJobObjectW(NULL, NULL);
	if (!job || !arm_job(job))
		return 1;
	{
		wchar_t cwd[MAX_PATH];
		wchar_t *slash;
		const wchar_t *cwd_arg = NULL;

		lstrcpynW(cwd, argv[1], MAX_PATH);
		slash = wcsrchr(cwd, L'\\');
		if (!slash)
			slash = wcsrchr(cwd, L'/');
		if (slash) {
			*slash = 0;
			cwd_arg = cwd;
		}
		if (!spawn_suspended(cmd, cwd_arg, &pi)) {
			fprintf(stderr, "CreateProcess failed (%lu)\n", GetLastError());
			CloseHandle(job);
			return 1;
		}
	}
	if (!AssignProcessToJobObject(job, pi.hProcess)) {
		fprintf(stderr, "AssignProcessToJobObject failed (%lu)\n", GetLastError());
		TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		CloseHandle(job);
		return 1;
	}
	ResumeThread(pi.hThread);
	printf("launched pid %lu (the process this launcher created and holds a handle to).\n"
	       "Holding the job open; every process that enters or leaves it is reported below.\n"
	       "Closing this window kills whatever is still in the job.\n",
	       pi.dwProcessId);
	wait_until_empty(job, pi.dwProcessId, pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	CloseHandle(job);
	return 0;
}

/* Let Steam launch the game, then pull it into the cage.
 *
 * A SteamStub-DRM'd title cannot be held by spawning it ourselves: the stub
 * bounces through a steam.exe helper to the always-running client, which spawns
 * the real game OUTSIDE any job we made. So instead of owning the launch, we own
 * the game after it exists: kick steam://run/<appid>, watch for the game's image
 * name to appear, and AssignProcessToJobObject it. Nested jobs are allowed here,
 * so it joins our kill-on-close cage even though Steam already owns it.
 *
 * The stub is the same image name as the game and appears first, so we adopt
 * every match we see and wait for one to persist: the stub dies in ~0.3 s at
 * exit 0x35, the real game does not. Once an adopted process has lived a few
 * seconds it is the game, and we stop scanning and hold the job.
 */
static int is_alive(HANDLE h)
{
	return h && WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
}

/* Bring the caged game's window to the front.
 *
 * Steam starts the game while something else owns the foreground, and Windows
 * will not hand a window focus it did not ask for. A fullscreen game that
 * minimizes itself whenever it is deactivated - Haydee does - then starts
 * minimized and stays there. Joining the foreground window's input queue for a
 * moment is the standard way past that lock. */
typedef struct {
	const DWORD *pid;
	int npid;
	HWND best;
	LONG best_area;
} FindWin;

static BOOL CALLBACK find_win_one(HWND h, LPARAM lp)
{
	FindWin *f = (FindWin *)lp;
	DWORD pid = 0;
	RECT rc;
	LONG area;
	int i;

	if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER))
		return TRUE;
	GetWindowThreadProcessId(h, &pid);
	for (i = 0; i < f->npid && f->pid[i] != pid; i++)
		;
	if (i == f->npid)
		return TRUE;
	/* A minimized window reports a tiny rect; count it as the game anyway. */
	area = 1;
	if (!IsIconic(h) && GetWindowRect(h, &rc))
		area = (rc.right - rc.left) * (rc.bottom - rc.top);
	if (!f->best || area > f->best_area) {
		f->best = h;
		f->best_area = area;
	}
	return TRUE;
}

static void bring_to_front(const DWORD *pid, const HANDLE *ph, int n)
{
	DWORD live[MAX_SEEN];
	DWORD t0 = GetTickCount();
	FindWin f;
	int i, nlive = 0;

	for (i = 0; i < n; i++)
		if (is_alive(ph[i]))
			live[nlive++] = pid[i];
	if (!nlive)
		return;
	memset(&f, 0, sizeof(f));
	f.pid = live;
	f.npid = nlive;
	while (!f.best && GetTickCount() - t0 < 30000) {
		EnumWindows(find_win_one, (LPARAM)&f);
		if (!f.best)
			Sleep(250);
	}
	if (!f.best) {
		printf("no visible game window within 30s; left the foreground alone.\n");
		return;
	}
	{
		HWND fg = GetForegroundWindow();
		DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
		DWORD me = GetCurrentThreadId();
		int attached = fg_tid && fg_tid != me && AttachThreadInput(me, fg_tid, TRUE);

		if (IsIconic(f.best))
			ShowWindow(f.best, SW_RESTORE);
		BringWindowToTop(f.best);
		SetForegroundWindow(f.best);
		if (attached)
			AttachThreadInput(me, fg_tid, FALSE);
	}
	Sleep(200);
	printf("game window %p %s\n", (void *)f.best,
	       GetForegroundWindow() == f.best ? "brought to the front"
					       : "could NOT take the foreground - click it to restore");
}

static int adopt(const wchar_t *appid, const wchar_t *exe)
{
	HANDLE job;
	wchar_t url[64];
	DWORD self = GetCurrentProcessId();
	DWORD ad_pid[MAX_SEEN];
	HANDLE ad_h[MAX_SEEN];
	DWORD ad_t[MAX_SEEN];
	int nad = 0, i;
	DWORD start;

	job = CreateJobObjectW(NULL, NULL);
	if (!job || !arm_job(job))
		return 1;
	printf("job armed: kill-on-close, breakaway off\n");

	wsprintfW(url, L"steam://run/%s", appid);
	printf("asking Steam to launch %ls; adopting every %ls it spawns.\n", url, exe);
	ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);

	start = GetTickCount();
	for (;;) {
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		PROCESSENTRY32W pe;
		int settled = 0;

		if (snap != INVALID_HANDLE_VALUE) {
			pe.dwSize = sizeof(pe);
			if (Process32FirstW(snap, &pe)) {
				do {
					HANDLE h;

					if (pe.th32ProcessID == self)
						continue;
					if (lstrcmpiW(pe.szExeFile, exe) != 0)
						continue;
					for (i = 0; i < nad; i++)
						if (ad_pid[i] == pe.th32ProcessID)
							break;
					if (i < nad)
						continue; /* already adopted */
					h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE |
							PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
							FALSE, pe.th32ProcessID);
					if (!h)
						continue;
					if (AssignProcessToJobObject(job, h)) {
						printf("adopted pid %lu into the cage\n",
						       pe.th32ProcessID);
						if (nad < MAX_SEEN) {
							ad_pid[nad] = pe.th32ProcessID;
							ad_h[nad] = h;
							ad_t[nad] = GetTickCount();
							nad++;
						} else
							CloseHandle(h);
					} else {
						printf("could NOT adopt pid %lu (err %lu) - the OS "
						       "refused to nest it in our job.\n",
						       pe.th32ProcessID, GetLastError());
						CloseHandle(h);
					}
				} while (Process32NextW(snap, &pe));
			}
			CloseHandle(snap);
		}

		/* Settled once an adopted process has lived long enough to be the game
		 * rather than the stub. */
		for (i = 0; i < nad; i++)
			if (is_alive(ad_h[i]) && GetTickCount() - ad_t[i] > 4000)
				settled = 1;
		if (settled) {
			printf("a caged %ls has been alive >4s; it is the game, not the "
			       "stub. Holding the cage.\n",
			       exe);
			bring_to_front(ad_pid, ad_h, nad);
			break;
		}
		if (GetTickCount() - start > 45000) {
			int any_alive = 0;
			for (i = 0; i < nad; i++)
				any_alive |= is_alive(ad_h[i]);
			if (!nad) {
				printf("no %ls appeared in 45s. Is Steam running and %ls the "
				       "right appid?\n",
				       exe, appid);
				for (i = 0; i < nad; i++)
					CloseHandle(ad_h[i]);
				CloseHandle(job);
				return 1;
			}
			printf("stopped scanning at 45s (%d adopted, %s still alive).\n",
			       nad, any_alive ? "some" : "none");
			break;
		}
		Sleep(200);
	}

	for (i = 0; i < nad; i++)
		CloseHandle(ad_h[i]);
	printf("closing this window kills what is in the cage.\n");
	wait_until_empty(job, 0, NULL);
	CloseHandle(job);
	return 0;
}

int wmain(int argc, wchar_t **argv)
{
	if (argc >= 2 && wcscmp(argv[1], L"--selftest") == 0)
		return selftest();
	if (argc >= 3 && wcscmp(argv[1], L"--adopt") == 0)
		return adopt(argv[2], argc >= 4 ? argv[3] : L"rabiribi.exe");
	if (argc < 2) {
		fprintf(stderr, "usage: pico_job.exe --selftest\n"
				"       pico_job.exe --adopt <appid> [exe-name]\n"
				"       pico_job.exe <exe> [args...]\n");
		return 1;
	}
	return launch(argc, argv);
}
