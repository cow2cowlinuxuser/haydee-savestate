# Compare two persisted save slots with savediff.exe.
#
# The slotfiles (D3D9SW_SLOTFILE=1) are per-slot and do not overwrite each other,
# so save the SAME game point to slot A in one session and slot B in another, then
# run this to see whether the two are the same content at shifted addresses.
#
#   ./savediff_slots.ps1            # slot 0 vs slot 1 (the default pair)
#   ./savediff_slots.ps1 0 2        # any two slots
#
# Keys, with D3D9SW_SLOT_VK=0x31: slot 0 saves on '1', slot 1 on '3', slot 2 on
# '5', slot 3 on '7' (load is the next key up: '2','4','6','8').
param([int]$a = 0, [int]$b = 1)
$ErrorActionPreference = "Stop"
$rr  = "C:\Program Files (x86)\Steam\steamapps\common\Rabi-Ribi"
$exe = Join-Path $PSScriptRoot "savediff.exe"
$ra  = Join-Path $rr "d3d9sw_slot$a.regions"
$rb  = Join-Path $rr "d3d9sw_slot$b.regions"
foreach ($p in @($ra, $rb)) {
  if (-not (Test-Path $p)) { Write-Error "missing $p - save to that slot first (D3D9SW_SLOTFILE=1)"; }
}
& $exe diff $ra $rb