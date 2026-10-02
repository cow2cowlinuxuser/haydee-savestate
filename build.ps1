$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$zig = "C:\zig\zig.exe"
$warn = @(
  "-O2",
  "-Wall",
  "-Wno-incompatible-function-pointer-types"
)

$src = @("d3d9_sw.c", "swrast.c", "savestate.c", "vsinterp.c", "trace.c", "tramp.c", "allocwatch.c", "dsoundhook.c", "ds_sw.c", "xa2_sw.c", "gameheap.c", "d3d9.def")

& $zig cc @warn -DD3D9SW_VARIANT=stock -target x86_64-windows-gnu -shared -o d3d9_sw.dll @src -lgdi32 -luser32 -lwinmm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $zig cc @warn -target x86_64-windows-gnu -o d3d9_sw_test.exe test.c -luser32 -lgdi32 -lwinmm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# The savestate harness drives savestate.c directly, with no wrapper and no game,
# so an invariant can be asserted after every restore instead of waiting to see
# whether something dies. Built from the same source the game loads - a harness
# against a copy of the engine would prove nothing about the engine.
& $zig cc @warn -target x86_64-windows-gnu -o ss_harness.exe ss_harness.c savestate.c dsoundhook.c ds_sw.c xa2_sw.c gameheap.c -luser32 -lwinmm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built ss_harness.exe (savestate engine, no game)"

# The same harness at 32 bits, because that is the width the engine actually
# runs at in the games it is aimed at - Rabi-Ribi and DDPR are both PE32. The
# x64 build alone left every pointer-width assumption in the engine untested
# outside the game itself, which is the worst place to find one.
& $zig cc @warn -target x86-windows-gnu -o ss_harness32.exe ss_harness.c savestate.c dsoundhook.c ds_sw.c xa2_sw.c gameheap.c -luser32 -lwinmm

# Harness-positive save/copy/restore rules against the real engine, no game.
& $zig cc @warn -target x86-windows-gnu -o port_harness32.exe port_harness.c savestate.c dsoundhook.c ds_sw.c xa2_sw.c gameheap.c -luser32 -lwinmm
if ($LASTEXITCODE -eq 0) { "built port_harness32.exe (engine port sitting, no game)" }

# One DLL loaded from two folders under one name, the way our d3d11/dxgi sit
# beside the system's, then a same-process restore with RELOC on: any shift or
# tripwire means module identity confused the two copies again.
& $zig cc @warn -target x86-windows-gnu -shared -o reloc_dup.dll reloc_dup.c
if ($LASTEXITCODE -eq 0) {
	& $zig cc @warn -target x86-windows-gnu -o reloc_harness32.exe reloc_harness.c savestate.c dsoundhook.c ds_sw.c xa2_sw.c gameheap.c -luser32 -lwinmm
	if ($LASTEXITCODE -eq 0) { "built reloc_harness32.exe (duplicate-name module identity, no game)" }
}

# A DirectSound streaming loop with no game attached, to find out whether the
# negative-length copy is a property of the arrangement or of how this game
# uses it. 32-bit to match the title.
& $zig cc @warn -target x86-windows-gnu -o ds_harness32.exe ds_harness.c savestate.c dsoundhook.c ds_sw.c xa2_sw.c gameheap.c -luser32 -lwinmm
if ($LASTEXITCODE -eq 0) { "built ds_harness32.exe (DirectSound rewind test, no game)" }

# All four of the game's conditions at once - shared-heap objects, present-time
# ntdll pool threads holding pointers across the rewind, callbacks and COM
# vtables, and enough bulk that the by-block path actually engages. The other
# two harnesses each cover at most two of those and both report "0 by block".
& $zig cc @warn -target x86-windows-gnu -o rr_harness32.exe rr_harness.c savestate.c dsoundhook.c ds_sw.c xa2_sw.c gameheap.c -luser32 -lwinmm
if ($LASTEXITCODE -eq 0) { "built rr_harness32.exe (all four conditions, no game)" }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Replays a recorded allocation trace against candidate allocators. Standalone -
# it links nothing from the engine, because the question it answers is about the
# allocator alone and a dependency on savestate.c would only make it slower to
# build and harder to trust. 32-bit on purpose: header sizes, alignment and the
# low-fragmentation heap's bucket boundaries are all width-dependent, so a
# 64-bit replay would answer a question nobody asked.
& $zig cc @warn -target x86-windows-gnu -o gh_replay32.exe gh_replay.c
if ($LASTEXITCODE -eq 0) { "built gh_replay32.exe (allocation trace replay, no game)" }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Prints what every monitor-size API reports, side by side. Built with no DPI
# manifest on purpose, so it sees the same invented coordinates the game does -
# a manifest here would hide the exact discrepancy it exists to show.
& $zig cc @warn -target x86-windows-gnu -o dispprobe32.exe dispprobe.c -lgdi32 -luser32
if ($LASTEXITCODE -eq 0) { "built dispprobe32.exe (monitor size and refresh probe)" }

# Asking the game where it landed costs a Steam launch and thirty-five seconds.
# Asking this costs a millisecond, and it runs where the game cannot.
& $zig cc @warn -target x86-windows-gnu -o baseprobe32.exe baseprobe.c
if ($LASTEXITCODE -eq 0) { "built baseprobe32.exe (ASLR image base probe)" }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Models the cross-session THREAD boundary in one process: run workers over a
# shared world, snapshot, kill them, spawn fresh threads at OS-chosen stacks, and
# try to continue two ways - adopt (restore saved stacks + contexts, gated on the
# saved stack address being free) vs data-only (rewind just the world, leave the
# fresh runners alone). Standalone; the question it answers is about threads, not
# the engine. 32-bit to match the titles.
& $zig cc @warn -target x86-windows-gnu -o thread_harness32.exe thread_harness.c
if ($LASTEXITCODE -eq 0) { "built thread_harness32.exe (cross-session thread rebuild, no game)" }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Reads the slotfiles the engine writes under D3D9SW_SLOTFILE=1 and says what a
# save contains aside from addresses, or whether two sessions' saves are the same
# content at shifted addresses. Standalone - it links nothing from the engine,
# because it only parses the .regions text and the .bin bytes on disk.
& $zig cc @warn -target x86_64-windows-gnu -o savediff.exe savediff.c
if ($LASTEXITCODE -eq 0) { "built savediff.exe (offline save content and diff, no game)" }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built ss_harness32.exe (savestate engine, PE32)"

# test_simd.exe only compares the scalar rasteriser against the vector one, so a
# convention error both share passes it. This one has no reference to compare
# against: it renders additively so each pixel counts how many triangles claimed
# it, and blits 1:1 from a texture whose every texel is distinct, so coverage
# gaps, double-hits and wrong texels are all named rather than eyeballed.
& $zig cc @warn -target x86_64-windows-gnu -o test_seam.exe test_seam.c swrast.c -luser32 -lgdi32 -lwinmm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built test_seam.exe (absolute coverage and sampling audit)"

# pixbench.exe - the per-pixel colour kernels (pack/unpack, B5G6R5 and BC1
# decode, red/blue swizzle, saturating add/subtract, source-over mix, modulate,
# and the YCbCr baseband transform) lifted out of d3d11_sw.c/swrast.c as scalar,
# AVX2 and AVX512 forms, each vector path checked bit-exact against scalar and
# then timed per 720p frame. No global -mavx flags on purpose: the baseline must
# be generic codegen so the scalar column is honest (the stock DLL ships without
# -march=haswell), and only the target-attributed kernels get AVX. The 512-bit
# encodings still need to exist in the object, which the per-function evex512
# attribute provides. -ffp-contract=off keeps the float pack path from being
# contracted into an FMA that would round differently than the scalar reference.
& $zig cc @warn -ffp-contract=off -target x86_64-windows-gnu -o pixbench.exe pixbench.c
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built pixbench.exe (pixel kernels scalar/AVX2/AVX512, per 720p frame)"

# pixbench_gpu.exe - the same colour math on a real GPU via a compute shader, to
# measure whether offloading it beats the CPU once the PCIe round-trip is paid.
# Resolves system d3d11/d3dcompiler at runtime (no import libs), so it links on a
# machine with neither and reports them absent. Run it from a folder WITHOUT this
# repo's software d3d11.dll/dxgi.dll beside it, or the loader shadows the real ones.
& $zig cc @warn -target x86_64-windows-gnu -o pixbench_gpu.exe pixbench_gpu.c
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built pixbench_gpu.exe (GPU compute offload vs CPU, per 720p frame)"

# pixtune.exe - does hand tuning move the straight AVX512 kernels: non-temporal
# stores, multiple accumulators, and decode/modulate/blend fusion, each checked
# bit-exact against its baseline. Same -ffp-contract=off baseline as pixbench.
& $zig cc @warn -ffp-contract=off -target x86_64-windows-gnu -o pixtune.exe pixtune.c
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built pixtune.exe (hand-tuning levers vs AVX512 baseline, per 720p frame)"

# fpsharness.exe - a standalone window that asks what a stable 240 / 480 fps
# needs and shows the ways a 60 Hz world can be presented at those rates without
# the windmill effect. It attaches to nothing. It links no d3d11/dxgi by name and
# loads System32's copies by absolute path at runtime, on purpose: this repo's
# own software d3d11.dll/dxgi.dll sit beside the .exe, and a name import would let
# the wrapper inject itself into the harness (it faults in its own Swap_Present,
# since its patches are meant for rabiribi.exe). Scripted runs:
#   fpsharness.exe <targetfps> <vsync0|1> <seconds>   e.g. "480 0 5"
& $zig cc @warn -target x86_64-windows-gnu -o fpsharness.exe fpsharness.c `
	-ldxguid -luser32 -lwinmm "-Wl,--subsystem,windows"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built fpsharness.exe (fps pacing + logic/render decoupling demo, no game)"

# gpuark.exe - tests whether GPU-PRODUCED state (a rendertarget the GPU wrote and
# later frames read) can be reconstructed bit-exact from a CPU-side shadow taken
# at an idle boundary, after the D3D11 device and every object are destroyed. The
# question that decides whether the gpuhost/boundary-interposition approach can be
# pointed at an emulator's GPU. Same System32 absolute-path load as fpsharness so
# the repo's software d3d11.dll cannot shadow the real one. -DTEX=2048 for a
# realistic readback-bandwidth figure (the default 256 is latency-bound).
& $zig cc @warn -target x86_64-windows-gnu -o gpuark.exe gpuark.c -ldxguid -luser32
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built gpuark.exe (GPU-state reconstruct-from-CPU-shadow test, no game)"

# glsl_harness.exe - validates the software GLSL interpreter (glsl.c) against
# Haydee's real vertex/fragment shaders offline, before it is wired into gl_sw.c.
& $zig cc @warn -target x86_64-windows-gnu -o glsl_harness.exe glsl_harness.c glsl.c -lm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built glsl_harness.exe (GLSL interpreter validation, no game)"

New-Item -ItemType Directory -Force -Path "x86" | Out-Null
& $zig cc @warn -DD3D9SW_VARIANT=stock -target x86-windows-gnu -shared -o x86\d3d9.dll @src -lgdi32 -luser32 -lwinmm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item -Force x86\d3d9.dll x86\d3d9_sw.dll
& $zig cc @warn -target x86-windows-gnu -o x86\d3d9_sw_test.exe test.c -luser32 -lgdi32 -lwinmm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# A second 32-bit DLL, same name, different folder, so swapping it in is one
# file copy and the known-good one is never touched.
#
# What it changes is only what an environment variable cannot: code generation.
# -march=haswell lets the whole translation unit use AVX2 rather than just the
# hand-written kernels that carry a target attribute, which mainly helps the
# scalar fallback and the per-pixel blend paths the vector kernel declines.
#
# That also makes the binary refuse to start on anything older than Haswell,
# with an illegal instruction rather than a clean message. That is the entire
# reason it is a separate file: the stock DLL stays runnable everywhere.
#
# -ffp-contract=off is not optional here. Once FMA is available the compiler may
# contract the scalar span arithmetic into a single rounding while the vector
# kernel keeps two, and the two paths stop agreeing bit for bit. Both seam
# audits flag that divergence as dormant precisely because the stock build has
# no FMA to contract into; -march=haswell would wake it up.
$low = @(
  "-O3",
  "-Wall",
  "-Wno-incompatible-function-pointer-types",
  "-march=haswell",
  "-mtune=skylake",
  "-ffp-contract=off"
)
New-Item -ItemType Directory -Force -Path "x86-lowspec" | Out-Null
& $zig cc @low -DD3D9SW_VARIANT=lowspec -target x86-windows-gnu -shared -o x86-lowspec\d3d9.dll @src -lgdi32 -luser32 -lwinmm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item -Force x86-lowspec\d3d9.dll x86-lowspec\d3d9_sw.dll

$d3d11src = @("d3d11_sw.c", "dxbc.c", "savestate.c", "swrast.c", "trace.c", "dsoundhook.c", "ds_sw.c", "xa2_sw.c", "gameheap.c", "gpuprobe.c", "gpu.c", "d3d11.def")
# This backend rasterises far more per frame than the D3D9 title, so the pool
# scales past that build's four threads; but it shares the CPU with the game's
# own logic thread, and measurement put the crossover at one thread per physical
# core rather than per logical one. D3D9SW_THREADS still overrides at runtime.
& $zig cc @warn -DD3D9SW_VARIANT=d3d11 -DSWRAST_DEFAULT_THREADS=32 -DSWRAST_THREADS_PHYSICAL -target x86_64-windows-gnu -shared -o d3d11.dll @d3d11src -lgdi32 -luser32 -lwinmm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $zig cc @warn -target x86_64-windows-gnu -shared -o dxgi.dll dxgi_fwd.c dxgi.def
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# The same backend as PE32. Plenty of D3D11 titles are still 32-bit - Rabi-Ribi
# is - and the x64 build cannot load into them at all. Nothing in the source
# needed changing for this; savestate.c and swrast.c were already building
# 32-bit for the D3D9 wrapper, and the rest followed on a target triple alone.
& $zig cc @warn -DD3D9SW_VARIANT=d3d11 -DSWRAST_DEFAULT_THREADS=32 -DSWRAST_THREADS_PHYSICAL -target x86-windows-gnu -shared -o x86\d3d11.dll @d3d11src -lgdi32 -luser32 -lwinmm "-Wl,--image-base=0x60000000"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  & $zig cc @warn -target x86-windows-gnu -shared -o x86\dxgi.dll dxgi_fwd.c dxgi.def "-Wl,--image-base=0x61000000"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  # The out-of-process presenter for D3D11SW_GPUHOST=1. 64-bit whatever the
  # game is, since it never loads into the game; it ships beside the 32-bit DLL.
  & $zig cc @warn -target x86_64-windows-gnu -o x86\gpuhost64.exe gpuhost.c -ld3d11 -ldxgi -ldxguid -luser32 "-Wl,--subsystem,windows"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  # A same-folder dsound.dll so that Windows' never loads. Only the 32-bit one
  # matters for Rabi-Ribi, but building both keeps the pair symmetric with dxgi.
  & $zig cc @warn -target x86_64-windows-gnu -shared -o dsound.dll dsound_fwd.c dsound.def
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  & $zig cc @warn -target x86-windows-gnu -shared -o x86\dsound.dll dsound_fwd.c dsound.def
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  # Same-folder trampoline. The -xaudio2 log names xaudio2_9.DLL (not _7).
  # Steam does not preload it, so the game folder wins. Forwards XAudio2Create
  # into d3d11.dll and exports the rest of the real DLL's names so DxLib's
  # delay-load of X3DAudioInitialize is not a jump to NULL.
  & $zig cc @warn -target x86-windows-gnu -shared -o x86\xaudio2_9.dll xa2_fwd.c xaudio2_9.def -luser32 "-Wl,--image-base=0x62000000"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  # DxLib tries xinput1_4 first, so that is the name to own. Forwards to the
  # system DLL unless D3D9SW_XINPUT=0, which pins every pad absent; pad
  # recognition changes the game's RNG consumption and so its allocation stream.
  & $zig cc @warn -target x86-windows-gnu -shared -o x86\xinput1_4.dll xinput_sw.c xinput.def "-Wl,--image-base=0x63000000"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  # Each of the four now links at its own base; this makes the loader prefer
  # that base instead of randomising it. Every module we pin is one less delta
  # a cross-session restore has to reconcile.
  Write-Host "pinning wrapper image bases:"
  & (Join-Path $PSScriptRoot "tools\noaslr.ps1") -Path x86\d3d11.dll, x86\dxgi.dll, x86\xaudio2_9.dll, x86\xinput1_4.dll

$rabi = "C:\Program Files (x86)\Steam\steamapps\common\Rabi-Ribi"
if ((Test-Path $rabi) -and (Test-Path "x86\xaudio2_9.dll")) {
  Copy-Item -Force x86\xaudio2_9.dll (Join-Path $rabi "xaudio2_9.dll")
  Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $rabi "xaudio2_probe.txt")
  Write-Host "deployed x86\xaudio2_9.dll to Rabi-Ribi (software XAudio2 trampoline; launch with -xaudio2)"
}

# The wrapper is only testable in situ, so drop it next to the game and clear
# the previous log; a stale log is worse than none when reading a new run.
$osfe = "C:\Program Files (x86)\Steam\steamapps\common\One Step From Eden"
if (Test-Path $osfe) {
  Copy-Item -Force d3d11.dll (Join-Path $osfe "d3d11.dll")
  Copy-Item -Force dxgi.dll (Join-Path $osfe "dxgi.dll")
  # The PDB travels with the DLL it describes.
  #
  # A dump of a hang is only as useful as the symbols that match the binary in
  # it, and a rebuild replaces d3d11.pdb in this directory while the dump still
  # refers to the previous DLL. That happened once: a live deadlock, still
  # standing and dumpable, and every frame in our own wrapper resolved to the
  # nearest export because two rebuilds had moved the PDB on underneath it.
  # Deployed alongside, the debugger finds the right one from the module path.
  Copy-Item -Force d3d11.pdb (Join-Path $osfe "d3d11.pdb")
# Renamed rather than deleted. Deleting it destroyed the wrapper-side record of
# the session being investigated at the moment a fix for that very session was
# deployed - twice in one afternoon, during the sprite-loss investigation. The
# savestate log learned this already; this one had not.
$log = Join-Path $osfe "d3d11_sw.log"
if (Test-Path $log) {
    $keep = Join-Path $osfe ("d3d11_sw_prev_" + (Get-Item $log).LastWriteTime.ToString("yyyyMMdd_HHmmss") + ".log")
    Move-Item -Force $log $keep
    # Two is enough to compare against; more than that is clutter nobody reads.
    Get-ChildItem $osfe -Filter "d3d11_sw_prev_*.log" |
        Sort-Object LastWriteTime -Descending | Select-Object -Skip 3 |
        Remove-Item -Force -ErrorAction SilentlyContinue
    Write-Host "deployed d3d11.dll dxgi.dll d3d11.pdb to OSFE, kept previous log as $(Split-Path $keep -Leaf)"
} else {
    Write-Host "deployed d3d11.dll dxgi.dll d3d11.pdb to OSFE"
}
}

python "$PSScriptRoot\gen_gl.py"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$glsrc = @("gl_sw.c", "gl_fwd.c", "gl_fwd_gen.c", "gl_stubs.c", "glsl.c", "savestate.c", "swrast.c", "trace.c", "dsoundhook.c", "ds_sw.c", "xa2_sw.c", "gameheap.c", "opengl32.def")
New-Item -ItemType Directory -Force -Path "x86" | Out-Null
& $zig cc @warn -Wno-inconsistent-dllimport -DD3D9SW_VARIANT=gl -DSWRAST_DEFAULT_THREADS=8 -target x86-windows-gnu -shared -o x86\opengl32.dll @glsrc -lgdi32 -luser32 -lwinmm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# Replays the DLL's GL calls on a real context and presents on the game's
# window (glhost.h). Beside opengl32.dll it is used; without it the DLL draws
# on the CPU.
& $zig cc @warn -target x86_64-windows-gnu -o x86\glhost64.exe glhost.c -lopengl32 -ld3d11 -ldxgi -ldxguid -lgdi32 -luser32 "-Wl,--subsystem,windows"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$haydee = "C:\Program Files (x86)\Steam\steamapps\common\Haydee"
if (Test-Path $haydee) {
  Copy-Item -Force x86\opengl32.dll (Join-Path $haydee "opengl32.dll")
  Copy-Item -Force x86\glhost64.exe (Join-Path $haydee "glhost64.exe")
  Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $haydee "gl_sw.log")
  Write-Host "deployed x86 opengl32.dll to Haydee, cleared gl_sw.log"
}

Write-Host "built d3d9_sw.dll d3d9_sw_test.exe"
Write-Host "built x86\d3d9.dll x86\d3d9_sw_test.exe (PE32 Steam drop-in name)"
Write-Host "built x86-lowspec\d3d9.dll (PE32, -O3 -march=haswell, needs AVX2)"
Write-Host "built d3d11.dll dxgi.dll (x64 software D3D11, drop next to OSFE.exe)"
Write-Host "built x86\d3d11.dll x86\dxgi.dll (PE32 software D3D11, for 32-bit titles)"
Write-Host "built x86\xaudio2_9.dll (trampoline into the software XAudio2)"
Write-Host "built x86\opengl32.dll (PE32 software OpenGL, drop next to Haydee launcher.exe)"
