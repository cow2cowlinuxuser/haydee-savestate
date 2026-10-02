/* A module that exists only to be loaded twice under one name, from two folders,
 * the way our d3d11.dll and the system's are. reloc_harness points at its marker. */
#include <windows.h>

__declspec(dllexport) int reloc_dup_marker = 0x5EED;

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
	(void)h;
	(void)reason;
	(void)reserved;
	return TRUE;
}
