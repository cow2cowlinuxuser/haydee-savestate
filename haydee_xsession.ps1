# Unattended Haydee session for the cross-session savestate test.
#
#   .\haydee_xsession.ps1 -Knobs 'D3D9SW_SAVE_AT=900','D3D9SW_QUIT_AT=1300'
#   .\haydee_xsession.ps1 -Knobs 'D3D9SW_LOAD_AT=900','D3D9SW_QUIT_AT=2100','D3D9SW_EXCLFORCE=1'
#
# Launches through Steam (pico_job --adopt), so the game sits in a kill-on-close
# job; a hang past -TimeoutSec is ended by killing the job. The cfg is rebuilt
# from d3d9_sw.cfg.bak-xsession plus -Knobs every run, and put back afterwards.
# Prints the log lines this session added that matter for the test.
param(
	[string[]]$Knobs = @(),
	[int]$TimeoutSec = 150,
	[string]$Tag = 'run'
)

$ErrorActionPreference = 'Stop'
$h = 'C:\Program Files (x86)\Steam\steamapps\common\Haydee'
$pico = Join-Path $PSScriptRoot 'rabi-ribi-pico\pico_job.exe'
$cfg = Join-Path $h 'd3d9_sw.cfg'
$base = Join-Path $h 'd3d9_sw.cfg.bak-xsession'
$log = Join-Path $h 'd3d9_sw_savestate_launcher.txt'
$out = Join-Path $env:TEMP "haydee_xsession_$Tag.txt"
$game = 'launcher|haydee|glhost64'

if (Get-Process | Where-Object { $_.Name -match "^($game)$" }) { throw 'the game is already running' }
if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { throw 'Steam is not running' }

$test = 'D3D9SW_(SAVE_AT|LOAD_AT|QUIT_AT|EXCLFORCE|RELOC)\b'
$lines = Get-Content $base | Where-Object { $_ -notmatch "^\s*$test" }
Set-Content $cfg ($lines + "# haydee_xsession.ps1 ($Tag)" + $Knobs)

$before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }
$t0 = Get-Date
$p = Start-Process $pico -ArgumentList '--adopt', '530890', 'launcher.exe' -PassThru `
	-NoNewWindow -RedirectStandardOutput $out
$done = $p.WaitForExit($TimeoutSec * 1000)
if (-not $done) {
	Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
	Start-Sleep 2
	Get-Process | Where-Object { $_.Name -match "^($game)$" } | Stop-Process -Force -ErrorAction SilentlyContinue
}
Start-Sleep 2
Copy-Item $base $cfg -Force

$secs = [int]((Get-Date) - $t0).TotalSeconds
"== $Tag : $(if ($done) { 'exited by itself' } else { "TIMED OUT after $TimeoutSec s, killed" }) after $secs s; knobs: $($Knobs -join ' ')"
if (Test-Path $log) {
	$new = @(Get-Content $log | Select-Object -Skip $before)
	$keep = 'save-at|load-at|quit-at|slotfile|provenance|refused|EXCLFORCE|unrestorable|thread-set|modules: \d|' +
		'reloc:|transplant|resume: |load: |restored|verify at restore|^fault:|exit:|PINNED|is full|' +
		'savestate (saved|restored)|physx: (save|load|restored)|openal: after|locks: '
	$new | Where-Object { $_ -match $keep } | ForEach-Object { $_.Trim() } | Select-Object -First 80
	"-- $($new.Count) new log line(s); full session in $log from line $($before + 1)"
}
