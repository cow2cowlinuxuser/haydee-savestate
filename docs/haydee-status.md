# Haydee savestate: where it stands (2026-10-02)

Cross-launch restore for Haydee (32-bit, OpenGL, PhysX 3) through the
`opengl32.dll` wrapper. It is a lot more stable than it was a day ago, but it is
**not solved**. A load from another launch can resume and play for minutes,
including room changes, and it can also crash in the first frame. Same-process
loads are routine.

## What works

- **Same-process save and load**, repeatedly.
- **Loads from another launch** ("same boot, new process"), with PhysX on.
  The best run played about 350 s after the load, through several room
  changes, before it was closed.
- **The death pit and achievements.** The game calls
  `hdSteamSetAchievement` on every death, and after a load that used to crash
  on a stale Steam context. That is fixed.
- **Saving again after a cross-launch load.** That used to crash in the heap
  check because of stale arena pointers; it is fixed too.

## Changes in this update

### Restore engine (`savestate.c`, `gameheap.c`)

- **Steam context.** `SteamInternal_ContextInit` is wrapped through the game's
  imports so the cached `CSteamAPIContext` refills after a load. Its counter is
  zeroed so steam_api re-reads the interfaces of this launch. Switch:
  `D3D9SW_STEAM_CTX`, default 1.
- **Arena pointers survive a load.** The wrapper's own bookkeeping arrays live
  outside every snapshot, but the pointers to them sit in our statics, which a
  load rewinds. The pointers are now held across the load and put back, and the
  sized derived arrays are cleared.
- **Config validator.** Each slot gets a `d3d9sw_slotN.cfg` holding every knob
  the wrapper read plus the wrapper build (size and timestamp). On load, each
  difference is logged as `config: NAME was "x" at the save, "y" now`. A fault
  report repeats the differences first, because a changed setting has been the
  cause before (PhysX was turned off between a save and its load).
- **PhysX workers are transplanted by default.** `D3D9SW_PHYSX_WORKERS=keep`
  restores the old behaviour.
- **Stand-in threads on demand.** Before the freeze, the load counts saved
  threads that have no live thread with the same entry point, and starts
  exactly that many stand-ins. The cap is 100; above it the load is refused,
  because that would mean a misread thread list, not a real game. Stand-ins
  nobody takes exit after every load, and stand-ins that were taken are ended
  if the load rolls back.
- **Our threads stay out of saves.** The watchdog and the stand-ins are
  excluded from the save census, so a second-generation save no longer carries
  threads the next launch has no partner for.
- **Thread pairing no longer requires the same owner.** The transplant used to
  require that the saved and live threads also have the same "owner", meaning
  the first module found on the stack. For a pool worker that only says what it
  was doing at that instant: mid-PhysX job at the save, idle now. So on the two
  GPU-host loads, 7 and then 16 workers went unpaired, and their saved state
  was restored with no thread behind it. Pairing now takes same-owner matches
  first, then any live thread with the same entry point.
- **The fault handler cannot loop on itself.** A fault inside the handler (it
  read a setting through `GetEnvironmentVariableA` mid-restore, and that call
  faulted) re-entered the handler until the stack ran out. That turned the
  real first fault into a `C00000FD` with no useful report. The handler now
  steps aside when it is already running on that thread, and the setting's
  "unset" answer is cached.

### GPU host (`glhost.c`, `gl_sw.c`)

- **No recovery after a load.** After a load with `GLSW_HOST=1`, the host
  logged `CreateSwapChainForHwnd on the game window failed 80070057`. The
  present call was handed the window from the saving launch, which is no
  longer valid. Two changes followed:
  - The host tore down its working swapchain to rebuild on that window, failed,
    and marked presentation failed for the rest of the session. It now keeps
    its current window when handed one that is no longer valid.
  - The DLL's GPU-host present path now picks this process's live window, as
    the CPU path already did.
- **Not yet tested after these changes.** GPU-host cross-launch loads also had
  the thread-pairing problem above, so the next GPU-host run tests both.

## Open problems, with the evidence

1. **PhysX crashes in the first frame after a cross-launch load into a
   different room.** With all 36 movable threads paired (pid 57920), the
   fault is `C0000005 at PhysX3_x86.dll+99FF2`, 0 frames after the load. An
   earlier trace saw a container that was `{233D82C0,2,2}` in the save get
   reset to `{FFFFFFFF,0,0}` in frame 0 and then freed. The next step is to log
   PhysX create and release calls around the load.
2. **Execution from a heap address.** On the GPU-host load (pid 40760) the main
   thread jumped to `233CBA98` (not code), 62 frames after the load. That is
   in the same area as the PhysX container above. 16 workers were unpaired on
   that load, which may be the whole cause; retest with the pairing fix.
3. **A failed restore that went on anyway.** On pid 1788 the post-restore check
   reported `9 WRONG (973 word(s)) <<< THE RESTORE DID NOT TAKE` and the session
   stopped logging after the lock pass. Most of the wrong regions are inside our
   own image (0x60100000 to 0x601B5000), so they may be our own statics moving
   during the write, not game damage. This needs a look before it is trusted
   either way.
4. **A 278-frame stall in the same process.** The main thread waits in
   `game.dll` through `gh_wfso` (`haydee+198251`).
5. **Audio.** 8 of 9 replaced sound buffers come back empty after a
   cross-launch load.
6. **Rendering.** A stretched triangle shows on the player model after a load,
   and SSAO and deferred lighting are still turned off on the CPU renderer.

## Running it

- **Config:** `d3d9_sw.cfg` beside the game. Current lines:
  - `GLSW_HOST=0` or `1`
  - `D3D9SW_GAMEHEAP=2`
  - `D3D9SW_PHYSX=1`
  - `D3D9SW_SLOTFILE=1`
  - `D3D9SW_REWIND_NEWTHREADS=run`
  - `D3D9SW_PHYSX_WORKERS=move`
  - `D3D9SW_DIPROXY=1`
  - `D3D9SW_SKIP_SMALL=64`
  - `D3D9SW_EXCLFORCE=1` is a leftover test line and can go.
- **Keys:** F5 saves, Shift+F5 loads.
- **Slot files:** `d3d9sw_slot0.bin`, `.meta`, `.regions` and `.cfg`.
- **Log:** `d3d9_sw_savestate_launcher.txt`. Lines worth searching for:
  - `provenance:` and `config:` say where the save came from and whether the
    settings match.
  - `threads: ... stand-in` and `transplant: N of M` say how the threads were
    handed over.
  - `fault:` marks a crash.
- **Build** (32-bit DLL):
  ```
  zig cc -O2 -Wall -Wno-inconsistent-dllimport -DD3D9SW_VARIANT=gl -DSWRAST_DEFAULT_THREADS=8 -target x86-windows-gnu -shared -o x86\opengl32.dll gl_sw.c gl_fwd.c gl_fwd_gen.c gl_stubs.c glsl.c savestate.c swrast.c trace.c dsoundhook.c ds_sw.c xa2_sw.c gameheap.c opengl32.def -lgdi32 -luser32 -lwinmm "-Wl,--image-base=0x60000000" "-Wl,--no-dynamicbase"
  ```
  The GPU host is built with the `glhost64.exe` line in `build.ps1`.

## What carries over to other games

Most of this update is not specific to Haydee:

- The config validator.
- Stand-in threads on demand.
- Keeping our own threads out of saves.
- Pairing threads by entry point first.
- The guarded fault handler.
- Holding arena pointers across a load.

The Steam context wrapper applies to any Steamworks game that caches the
context, which is most of them. The PhysX and OpenAL work is specific to games
that use those libraries.
