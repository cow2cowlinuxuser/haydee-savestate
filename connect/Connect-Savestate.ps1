<#
.SYNOPSIS
  Parse savestate logs into a timeline of attempts (what each save and load
  decided, and why) and a connection graph (threads, heaps, modules, pointers).

.DESCRIPTION
  Reads d3d9_sw_savestate_*.txt, ss_*.txt, and prev_*.txt under the game folder.
  Every verdict, cause, node and edge is taken from a quoted log line - nothing
  is inferred.

  Attempts (attempts.html, the default view): one row per save, load, fault or
  stall, with the log lines that explain it. Lines are labelled by the rules in
  signatures.json: a failure that matches a known cause says what it means and
  what to check next; one that matches nothing is marked NEW. Each launch also
  shows the build and the cfg values that changed since the launch before it.

  Graph (connections.html): the connection graph, also linked from the attempts
  page.
    -Date       one session, or every session whose logged date matches the prefix
    -OverTime   collapse names across sessions (heap owner, park site, module)

  Build helper (Rabi-Ribi only):
    -Swap presettle|settle   copy that pair of DLLs, archive the live log, regenerate

.EXAMPLE
  .\connect\Connect-Savestate.ps1                      # Haydee, open attempts
  .\connect\Connect-Savestate.ps1 -Attempts            # print the last few launches here
  .\connect\Connect-Savestate.ps1 -Game rabiribi
  .\connect\Connect-Savestate.ps1 -Graph               # open the graph instead
  .\connect\Connect-Savestate.ps1 -List
  .\connect\Connect-Savestate.ps1 -Date 2026-09-11
  .\connect\Connect-Savestate.ps1 -Date "2026-09-11 17:43"
  .\connect\Connect-Savestate.ps1 -OverTime
  .\connect\Connect-Savestate.ps1 -Game rabiribi -Swap settle
#>
[CmdletBinding()]
param(
    [ValidateSet("haydee", "rabiribi")][string]$Game = "haydee",
    [string]$Date,
    [switch]$OverTime,
    [switch]$List,
    [switch]$Attempts,
    [int]$Last = 4,
    [switch]$Graph,
    [ValidateSet("presettle", "settle")][string]$Swap,
    [string]$GameRoot,
    [string]$Out,
    [switch]$NoOpen,
    [switch]$Json
)

Set-StrictMode -Version 2
$ErrorActionPreference = "Stop"

if (-not $GameRoot) {
    $common = "C:\Program Files (x86)\Steam\steamapps\common"
    $GameRoot = if ($Game -eq "rabiribi") { Join-Path $common "Rabi-Ribi" } else { Join-Path $common "Haydee" }
}

$script:Rules = @()
$rulesPath = Join-Path $PSScriptRoot "signatures.json"
if (Test-Path $rulesPath) {
    $script:Rules = @(Get-Content $rulesPath -Raw | ConvertFrom-Json | ForEach-Object {
        @{ id = $_.id; rx = [regex]::new($_.match); level = $_.level; meaning = $_.meaning; next = $_.next; status = $_.status }
    })
}

function Get-TagFromFile([string]$name) {
    if ($name -match '^d3d9_sw_savestate_[^.]+\.(.+)\.txt$') { return $Matches[1] }
    if ($name -match '^d3d9_sw_savestate_[^.]+_prev_(.+)\.txt$') { return "prev-" + $Matches[1] }
    if ($name -match '^ss_(.+)\.txt$') { return $Matches[1] }
    if ($name -match '^prev_(.+)\.txt$') { return "prev-" + $Matches[1] }
    if ($name -match '^d3d9_sw_savestate_[^.]+\.txt$') { return "live" }
    return [IO.Path]::GetFileNameWithoutExtension($name)
}

function Normalize-Owner([string]$raw) {
    if (-not $raw) { return "" }
    $s = $raw.Trim()
    $s = $s -replace "\s+", " "
    if ($s -match "game'?s runtime") { return "game runtime / process heap" }
    if ($s -match "wrapper") { return "wrapper heap" }
    if ($s -match "(?i)([A-Za-z0-9_\-]+\.(?:dll|exe|DLL|EXE))") { return $Matches[1] }
    return $s
}

function New-SessionBuilder([string]$when, [int]$procId, [string]$file, [string]$tag) {
    $id = ($when -replace "[^\d]", "") + "-" + $procId
    return @{
        id       = $id
        when     = $when
        pid      = $procId
        file     = $file
        tag      = $tag
        outcome  = "unknown"
        policy   = ""
        stats    = @{
            threads     = 0
            heaps       = 0
            coverageMb  = 0
            presentMb   = 0
            presentPct  = 0
            loads       = 0
            saves       = 0
            contradictionMb = 0
            pointerHazardWords = 0
            clobberReverted = 0
        }
        settings = @{}
        nodes    = @{}
        edges    = @{}
        heaps    = New-Object System.Collections.Generic.List[object]
        inPtrMod = $false
        inCross  = $false
        inUncap  = $false
        inInvHeap = $false
        build    = ""
        cfg      = [ordered]@{}
        attempts = New-Object System.Collections.Generic.List[object]
        win      = [ordered]@{}
        sink     = $null
        scratch  = $false
        crossNext = $false
        loadSlot = ""
        boundary = New-Object System.Collections.Generic.List[object]
        firstLine = 0
    }
}

# ---- boundary: the ##### BOUNDARY / ##### LIVE report a cross-launch load prints,
# and the BOUNDARY sample lines a save prints, one table per report

function Parse-Boundary($b, [string]$line, [int]$lineNo) {
    if ($line -notmatch 'BOUNDARY|##### LIVE' -and -not ($b.boundary.Count -and $line -match '^\s+saved in ')) { return }
    $cur = if ($b.boundary.Count) { $b.boundary[$b.boundary.Count - 1] } else { $null }
    if ($line -match '^\s+BOUNDARY sample (\d+): holder (\w+) -> target (\w+) in heap (\w+) \((.*?)\), \+(\w+) into it, pointer words mask (\w+)') {
        if ($Matches[1] -eq "1" -or -not $cur -or $cur.kind -ne "save") {
            $cur = @{ kind = "save"; line = $lineNo; rows = New-Object System.Collections.Generic.List[object]; accuracy = ""; live = New-Object System.Collections.Generic.List[object] }
            $b.boundary.Add($cur) | Out-Null
        }
        $cur.rows.Add(@{ n = $Matches[1]; holder = $Matches[2]; wants = $Matches[3]; heap = $Matches[4]; heapName = $Matches[5]; into = $Matches[6]; mask = $Matches[7] }) | Out-Null
        return
    }
    if ($line -match '^##### BOUNDARY: this slot carries no samples') {
        $b.boundary.Add(@{ kind = "load"; line = $lineNo; rows = @(); accuracy = "this slot carries no samples"; live = @() }) | Out-Null
        return
    }
    if ($line -match '^##### BOUNDARY (\d+)/(\d+)\s+heap: (\w+)\s+wants: (\w+)\s+actual: (\S+)\s+offset: (\S+)\s+\((.*)\)\s*$') {
        if ($Matches[1] -eq "1" -or -not $cur -or $cur.kind -ne "load") {
            $cur = @{ kind = "load"; line = $lineNo; rows = New-Object System.Collections.Generic.List[object]; accuracy = ""; live = New-Object System.Collections.Generic.List[object] }
            $b.boundary.Add($cur) | Out-Null
        }
        $cur.rows.Add(@{ n = $Matches[1]; heap = $Matches[3]; wants = $Matches[4]; actual = $Matches[5]; offset = $Matches[6]; note = $Matches[7]; detail = "" }) | Out-Null
        return
    }
    if ($cur -and $cur.kind -eq "load" -and $cur.rows.Count -and $line -match '^\s+saved in (.+)$') {
        $cur.rows[$cur.rows.Count - 1].detail = $Matches[1].Trim()
        return
    }
    if ($cur -and $line -match '^##### BOUNDARY ACCURACY: (.+)$') { $cur.accuracy = $Matches[1].Trim(); return }
    if ($cur -and $line -match '^##### LIVE (\d+)/\d+\s+holder: (\w+)\s+wants: (\w+)\s+live: (\S+)(?:\s+offset: (\S+))?\s*(.*)$') {
        $cur.live.Add(@{ n = $Matches[1]; holder = $Matches[2]; wants = $Matches[3]; live = $Matches[4]; offset = $Matches[5]; note = $Matches[6].Trim() }) | Out-Null
    }
}

# ---- attempts: what each save and load decided, with the lines that explain it

function Add-Hit($hits, $rule, [int]$lineNo, [string]$line) {
    if (-not $hits.Contains($rule.id)) {
        $hits[$rule.id] = @{ rule = $rule.id; level = $rule.level; count = 0; lines = New-Object System.Collections.Generic.List[string] }
    }
    $h = $hits[$rule.id]
    $h.count += 1
    if ($h.lines.Count -lt 4) { $h.lines.Add("L${lineNo}: " + (Clip-Line $line)) | Out-Null }
}

function New-Attempt($b, [string]$kind, [string]$verdict, [string]$slot, [int]$lineNo, [string]$line) {
    $a = @{
        kind    = $kind
        verdict = $verdict
        slot    = $slot
        line    = $lineNo
        text    = (Clip-Line $line)
        cross   = $false
        hits    = $b.win
        status  = ""
    }
    $b.win = [ordered]@{}
    $b.attempts.Add($a) | Out-Null
    return $a
}

function Parse-Attempt($b, [string]$line, [int]$lineNo) {
    if ($line -match '^savestate ready, \w+ build (\w+)') { $b.build = $Matches[1] }
    if ($line -match '^\s+((?:D3D9SW|GLSW)_\w+) = (.+?) \(from the cfg file\)') { $b.cfg[$Matches[1]] = $Matches[2] }

    # A new operation starts; anything after this belongs to it, not to the last fault.
    if ($line -match '^(save: after dsh_quiet|rollback:|load: )' -or $line -match 'slotfile: loaded a slot') { $b.sink = $null }

    $made = $null
    if ($line -match '^rollback: taking a scratch save before loading slot (\d+)(, which came from another process)?') {
        $b.scratch = $true; $b.loadSlot = $Matches[1]
        $b.crossNext = [bool]$Matches[2]
    } elseif ($line -match '^save: slot (\d+), \d+ regions') {
        $made = New-Attempt $b $(if ($b.scratch) { "scratch save" } else { "save" }) "ok" $Matches[1] $lineNo $line
        $b.scratch = $false
    } elseif ($line -match '^load: slot (\d+), \d+ restored') {
        $made = New-Attempt $b "load" "ok" $Matches[1] $lineNo $line
        $made.cross = $b.crossNext; $b.crossNext = $false
    } elseif ($line -match '^load: refused') {
        $made = New-Attempt $b "load" "refused" $b.loadSlot $lineNo $line
        $made.cross = $b.crossNext; $b.crossNext = $false
    } elseif ($line -match '^fault: ') {
        $made = New-Attempt $b "fault" "fault" "" $lineNo $line
        $b.sink = $made
    } elseif ($line -match '^stall: no frame') {
        $made = New-Attempt $b "hang" "stall" "" $lineNo $line
        $b.sink = $made
    }

    foreach ($r in $script:Rules) {
        if (-not $r.rx.IsMatch($line)) { continue }
        $target = if ($made) { $made.hits } elseif ($b.sink) { $b.sink.hits } else { $b.win }
        Add-Hit $target $r $lineNo $line
    }
}

function Close-Attempts($b) {
    if ($b.win.Count -gt 0) {
        $noisy = $false
        foreach ($k in $b.win.Keys) { if ($b.win[$k].level -in @("warn", "cause")) { $noisy = $true } }
        if ($noisy) {
            $a = New-Attempt $b "after the last attempt" "end" "" 0 "lines after the last save, load or fault"
        }
    }
    foreach ($a in $b.attempts) {
        $levels = @($a.hits.Values | ForEach-Object { $_.level })
        if ($a.verdict -eq "ok") {
            $a.status = if ($levels -contains "warn") { "ok, warnings" } else { "ok" }
        } elseif ($a.verdict -eq "end") {
            $a.status = if ($levels -contains "cause") { "known" } else { "warnings" }
        } elseif ($levels -contains "ignore") {
            $a.status = "ignored"
        } elseif ($levels -contains "cause") {
            $a.status = "known"
        } else {
            $a.status = "NEW"
        }
    }
}

function Add-Node($b, [string]$id, [string]$kind, [string]$label, [hashtable]$extra) {
    if (-not $id) { return }
    if ($b.nodes.ContainsKey($id)) {
        $n = $b.nodes[$id]
        if ($extra) {
            foreach ($k in $extra.Keys) {
                $v = $extra[$k]
                if ($null -eq $v -or $v -eq "") { continue }
                if (-not $n.ContainsKey($k) -or $null -eq $n[$k] -or $n[$k] -eq "") {
                    $n[$k] = $v
                } elseif ($k -in @("votes", "hits", "weight") -and ($v -as [double]) -gt ($n[$k] -as [double])) {
                    $n[$k] = $v
                }
            }
        }
        return
    }
    $n = @{ id = $id; kind = $kind; label = $label }
    if ($extra) { foreach ($k in $extra.Keys) { $n[$k] = $extra[$k] } }
    $b.nodes[$id] = $n
}

function Add-Edge($b, [string]$from, [string]$to, [string]$kind, [string]$evidence, [int]$weight = 1, [bool]$hazard = $false) {
    if (-not $from -or -not $to -or $from -eq $to) { return }
    $key = "$from|$kind|$to"
    if ($b.edges.ContainsKey($key)) {
        $e = $b.edges[$key]
        $e.weight += $weight
        if ($evidence -and $e.evidence.Count -lt 3 -and $e.evidence -notcontains $evidence) {
            $e.evidence += $evidence
        }
        if ($hazard) { $e.hazard = $true }
        return
    }
    $ev = @()
    if ($evidence) { $ev = @($evidence) }
    $b.edges[$key] = @{
        from     = $from
        to       = $to
        kind     = $kind
        weight   = $weight
        hazard   = $hazard
        evidence = $ev
    }
}

function Clip-Line([string]$line) {
    $t = $line.Trim()
    if ($t.Length -gt 220) { return $t.Substring(0, 217) + "..." }
    return $t
}

function Find-HeapId($b, [string]$addrHex) {
    if (-not $addrHex) { return $null }
    $n = [int64]("0x" + $addrHex)
    $best = $null
    $bestBase = -1
    foreach ($h in $b.heaps) {
        $base = [int64]("0x" + $h.addr)
        if ($base -le $n -and $base -gt $bestBase) {
            $bestBase = $base
            $best = $h
        }
    }
    if ($best) { return "heap:" + $best.addr }
    return $null
}

function Remember-Heap($b, [string]$addr, [string]$ownerRaw, [string]$policy, [int]$votes, [string]$line) {
    $owner = Normalize-Owner $ownerRaw
    $id = "heap:$addr"
    Add-Node $b $id "heap" $addr @{ owner = $owner; policy = $policy; votes = $votes }
    $found = $false
    foreach ($h in $b.heaps) { if ($h.addr -eq $addr) { $found = $true; $h.owner = $owner; $h.policy = $policy; break } }
    if (-not $found) { $b.heaps.Add(@{ addr = $addr; owner = $owner; policy = $policy }) | Out-Null }
    if ($owner) {
        $mid = "module:$owner"
        if ($owner -notmatch "heap|runtime|process") {
            Add-Node $b $mid "module" $owner @{ name = $owner; policy = $policy }
            Add-Edge $b $id $mid "owns" (Clip-Line $line) 1 $false
        } else {
            Add-Node $b "name:$owner" "name" $owner @{}
            Add-Edge $b $id "name:$owner" "owns" (Clip-Line $line) 1 $false
        }
    }
}

function Finish-Session($b) {
    Close-Attempts $b
    $parts = @()
    $loadsOk = @($b.attempts | Where-Object { $_.kind -eq "load" -and $_.verdict -eq "ok" })
    $loadsNo = @($b.attempts | Where-Object { $_.kind -eq "load" -and $_.verdict -eq "refused" })
    if ($loadsOk.Count) {
        $parts += $(if (@($loadsOk | Where-Object { $_.cross }).Count) { "restored (cross-launch)" } else { "restored" })
    }
    if ($loadsNo.Count) { $parts += "refused x$($loadsNo.Count)" }
    if (-not $parts.Count -and @($b.attempts | Where-Object { $_.kind -like "*save" }).Count) { $parts += "saved" }
    if (@($b.attempts | Where-Object { $_.kind -eq "fault" -and $_.status -ne "ignored" }).Count) { $parts += "fault" }
    if (@($b.attempts | Where-Object { $_.kind -eq "hang" }).Count) { $parts += "stall" }
    if (@($b.attempts | Where-Object { $_.status -eq "NEW" }).Count) { $parts += "NEW" }
    $b.outcome = if ($parts.Count) { $parts -join ", " } else { "nothing attempted" }
    $nodeList = New-Object System.Collections.Generic.List[object]
    foreach ($k in $b.nodes.Keys) { $nodeList.Add($b.nodes[$k]) | Out-Null }
    $edgeList = New-Object System.Collections.Generic.List[object]
    foreach ($k in $b.edges.Keys) { $edgeList.Add($b.edges[$k]) | Out-Null }
    return @{
        id       = $b.id
        when     = $b.when
        pid      = $b.pid
        file     = $b.file
        tag      = $b.tag
        outcome  = $b.outcome
        policy   = $b.policy
        stats    = $b.stats
        settings = $b.settings
        nodes    = $nodeList
        edges    = $edgeList
        build    = $b.build
        cfg      = $b.cfg
        attempts = $b.attempts
        boundary = $b.boundary
        firstLine = $b.firstLine
        logText  = ""
        logFile  = ""
    }
}

function Parse-Line($b, [string]$line) {
    if ($line -match '^\s*===== end inventory') {
        $b.inPtrMod = $false; $b.inCross = $false; $b.inUncap = $false; $b.inInvHeap = $false
        return
    }

    if ($line -match '^\s*EFFECTIVE thread policy:\s*(.+)$') {
        $b.policy = $Matches[1].Trim()
        Add-Node $b "name:thread-policy" "name" "thread policy" @{}
        Add-Node $b "name:threads" "name" "threads" @{}
        Add-Edge $b "name:thread-policy" "name:threads" "setting" (Clip-Line $line)
        return
    }

    if ($line -match '^\s*(D3D9SW_\w+)\s*=\s*(.+)$') {
        $name = $Matches[1]
        $val = $Matches[2].Trim()
        if ($val -notmatch '^\(unset') {
            $b.settings[$name] = $val
            Add-Node $b "name:$name" "name" $name @{}
            if ($name -eq "D3D9SW_REWIND_GAMEHEAP") {
                Add-Node $b "name:game runtime / process heap" "name" "game runtime / process heap" @{}
                Add-Edge $b "name:$name" "name:game runtime / process heap" "setting" (Clip-Line $line)
            } elseif ($name -eq "D3D9SW_REWIND_SWHEAP") {
                Add-Node $b "name:wrapper heap" "name" "wrapper heap" @{}
                Add-Edge $b "name:$name" "name:wrapper heap" "setting" (Clip-Line $line)
            } elseif ($name -match "THREAD") {
                Add-Node $b "name:threads" "name" "threads" @{}
                Add-Edge $b "name:$name" "name:threads" "setting" (Clip-Line $line)
            }
        }
        return
    }

    if ($line -match '^\s*game runtime heap ([0-9A-Fa-f]{8}) from ([^,]+),') {
        $addr = $Matches[1]; $from = $Matches[2].Trim()
        Remember-Heap $b $addr "game runtime / process heap" "" 0 $line
        Add-Node $b "module:$from" "module" $from @{ name = $from }
        Add-Edge $b "heap:$addr" "module:$from" "from" (Clip-Line $line)
        return
    }

    if ($line -match '^\s*heap ([0-9A-Fa-f]{8})\s+(.+?)\s+(rewound|left in the present)\s+\((\d+)\s+vote') {
        Remember-Heap $b $Matches[1] $Matches[2] $Matches[3] ([int]$Matches[4]) $line
        $b.stats.heaps = [Math]::Max($b.stats.heaps, $b.heaps.Count)
        return
    }

    if ($line -match '^\s*([0-9A-Fa-f]{8})\s+(REWOUND with the game|left in the present)\s+(\d+) segment\(s\),\s+([\d.]+)\s+MB\s+(.+)$') {
        $pol = if ($Matches[2] -like "REWOUND*") { "rewound" } else { "left in the present" }
        Remember-Heap $b $Matches[1] $Matches[5] $pol 0 $line
        return
    }

    if ($line -match '^\s*module (\S+)\s+(\d+) KB at ([0-9A-Fa-f]+), (rewound|left in the present)') {
        $name = $Matches[1]
        Add-Node $b "module:$name" "module" $name @{ name = $name; policy = $Matches[4]; base = $Matches[3]; kb = [int]$Matches[2] }
        return
    }

    if ($line -match '^\s*REWOUND:\s+(\S+)$') {
        Add-Node $b "module:$($Matches[1])" "module" $Matches[1] @{ name = $Matches[1]; policy = "rewound" }
        return
    }

    if ($line -match '^\s*thread (\d+) parked at ([0-9A-Fa-f]+) \(([^)]+)\)') {
        $tid = $Matches[1]; $park = $Matches[3]
        $mod = $park
        if ($park -match '^([^+\s]+)') { $mod = $Matches[1] }
        Add-Node $b "thread:$tid" "thread" $tid @{ park = $park; role = "parked" }
        Add-Node $b "module:$mod" "module" $mod @{ name = $mod }
        Add-Edge $b "thread:$tid" "module:$mod" "parked" (Clip-Line $line)
        return
    }

    if ($line -match 'new park site: thread (\d+) at ([0-9A-Fa-f]+) in (.+)$') {
        $tid = $Matches[1]
        $mod = [IO.Path]::GetFileName($Matches[3].Trim())
        Add-Node $b "thread:$tid" "thread" $tid @{ park = $mod; role = "system" }
        Add-Node $b "module:$mod" "module" $mod @{ name = $mod }
        Add-Edge $b "thread:$tid" "module:$mod" "parked" (Clip-Line $line)
        return
    }

    if ($line -match 'those threads belong to:\s*(.+)$') {
        foreach ($part in ($Matches[1] -split ",")) {
            if ($part -match '(\S+)\s+x(\d+)') {
                $mod = $Matches[1]; $n = [int]$Matches[2]
                Add-Node $b "module:$mod" "module" $mod @{ name = $mod }
                Add-Node $b "name:system-threads" "thread" "system threads" @{ belong = $mod; role = "system-set" }
                Add-Edge $b "name:system-threads" "module:$mod" "belong" (Clip-Line $line) $n
            }
        }
        return
    }

    if ($line -match 'system thread (\d+) holds ([0-9A-Fa-f]+) in its thread block, on (.+), which we rewind') {
        $tid = $Matches[1]; $ptr = $Matches[2]; $ownerRaw = $Matches[3]
        $owner = Normalize-Owner $ownerRaw
        Add-Node $b "thread:$tid" "thread" $tid @{ role = "system" }
        $hid = $null
        $bestBase = -1L
        $pn = [int64]("0x" + $ptr)
        foreach ($h in $b.heaps) {
            if ($h.owner -ne $owner) { continue }
            $base = [int64]("0x" + $h.addr)
            if ($base -le $pn -and $base -gt $bestBase) {
                $bestBase = $base
                $hid = "heap:" + $h.addr
            }
        }
        if (-not $hid) {
            foreach ($h in $b.heaps) {
                if ($h.owner -eq $owner) { $hid = "heap:" + $h.addr; break }
            }
        }
        if (-not $hid) {
            $hid = "name:" + $(if ($owner) { $owner } else { "rewound heap" })
            $kind = if ($owner -match '\.(dll|exe)$') { "module" } else { "name" }
            Add-Node $b $hid $kind $(if ($owner) { $owner } else { "rewound heap" }) @{ owner = $owner }
        }
        Add-Edge $b "thread:$tid" $hid "holds" (Clip-Line $line) 1 $true
        return
    }

    if ($line -match 'CLASH:.+hold ([0-9A-Fa-f]{8}).+belongs to heap ([0-9A-Fa-f]{8})') {
        $reg = $Matches[1]; $heap = $Matches[2]
        Add-Node $b "region:$reg" "region" $reg @{ role = "clash" }
        Add-Node $b "heap:$heap" "heap" $heap @{}
        Add-Edge $b "heap:$heap" "region:$reg" "clash" (Clip-Line $line) 1 $true
        return
    }

    if ($line -match 'heap contradiction:\s+([\d.]+) span.+first at ([0-9A-Fa-f]{8}) on heap ([0-9A-Fa-f]{8})') {
        $b.stats.contradictionMb = [double]$Matches[1]
        return
    }

    if ($line -match 'LFH: region \d+ at ([0-9A-Fa-f]{8}) is bookkeeping for heap ([0-9A-Fa-f]{8})') {
        $reg = $Matches[1]; $heap = $Matches[2]
        Add-Node $b "region:$reg" "region" $reg @{ role = "lfh" }
        Add-Node $b "heap:$heap" "heap" $heap @{}
        Add-Edge $b "region:$reg" "heap:$heap" "lfh" (Clip-Line $line)
        return
    }

    if ($line -match '^\s*crossings:\s+(\d+) import') {
        $b.inCross = $true; $b.inPtrMod = $false; $b.inUncap = $false
        Add-Node $b "name:rewound-imports" "name" "rewound imports" @{}
        return
    }
    if ($b.inCross -and $line -match '^\s+(\d+)\s+->\s+(\S+)$') {
        $mod = $Matches[2]
        Add-Node $b "module:$mod" "module" $mod @{ name = $mod }
        Add-Edge $b "name:rewound-imports" "module:$mod" "import" (Clip-Line $line) ([int]$Matches[1]) $true
        return
    }
    if ($b.inCross -and $line -notmatch '^\s+\d+\s+->') { $b.inCross = $false }

    if ($line -match 'hazard set:\s+(\d+) word') {
        $b.stats.pointerHazardWords = [int64]$Matches[1]
    }
    if ($line -match '^\s+(\d+)\s+(rewind with us|held but immutable|HELD and WRITABLE, in a heap left in the present|HELD and WRITABLE, in a module''s data section|HELD and WRITABLE, private memory)') {
        $hits = [int64]$Matches[1]
        $cls = switch -Regex ($Matches[2]) {
            "rewind with us" { "rewind with us" }
            "immutable" { "held immutable" }
            "heap left" { "held writable heap" }
            "module" { "held writable module data" }
            "private" { "held writable private" }
            default { $Matches[2] }
        }
        $hazard = $cls -like "held writable*"
        Add-Node $b "pointer:rewound-memory" "pointer" "rewound memory" @{}
        Add-Node $b "pointer:$cls" "pointer" $cls @{ hits = $hits }
        Add-Edge $b "pointer:rewound-memory" "pointer:$cls" "points" (Clip-Line $line) $hits $hazard
        if ($cls -eq "held writable private") { $b.inUncap = $true; $b.inPtrMod = $false }
        return
    }

    if ($line -match 'the uncaptured reservations those private pointers land in:') {
        $b.inUncap = $true; $b.inPtrMod = $false
        return
    }
    if ($b.inUncap -and $line -match '^\s+([0-9A-Fa-f]{8})\s+(\d+)\s+hit\(s\),\s+([\d.]+)\s+MB') {
        $addr = $Matches[1]; $hits = [int]$Matches[2]
        $role = if ($addr -eq $script:ArenaBase) { "swrast arena" } else { "private" }
        $lab = if ($role -eq "swrast arena") { "$addr swrast" } else { $addr }
        Add-Node $b "region:$addr" "region" $lab @{ role = $role; hits = $hits }
        Add-Node $b "pointer:held writable private" "pointer" "held writable private" @{}
        Add-Edge $b "pointer:held writable private" "region:$addr" "points" (Clip-Line $line) $hits $true
        return
    }
    if ($line -match '^\s+module\s+hits\s+per MB') {
        $b.inPtrMod = $true; $b.inUncap = $false
        return
    }
    if ($b.inPtrMod -and $line -match '^\s+(\S+\.(?:dll|DLL|exe))\s+(\d+)\s+') {
        $mod = $Matches[1]; $hits = [int]$Matches[2]
        Add-Node $b "module:$mod" "module" $mod @{ name = $mod; hits = $hits }
        Add-Node $b "pointer:held writable module data" "pointer" "held writable module data" @{}
        Add-Edge $b "pointer:held writable module data" "module:$mod" "ptrmod" (Clip-Line $line) $hits $true
        return
    }
    if ($b.inPtrMod -and $line -match '^\s+threads:') { $b.inPtrMod = $false }

    if ($line -match 'reach: (\d+) of (\d+) matched block\(s\) reachable from the game') {
        Add-Node $b "name:game-reach" "name" "game reach" @{}
        Add-Node $b "name:blocks" "name" "busy blocks" @{}
        Add-Edge $b "name:game-reach" "name:blocks" "reach" (Clip-Line $line) ([int]$Matches[1])
        return
    }
    if ($line -match 'system reach: (\d+) of (\d+) matched block\(s\) reachable from ntdll, (\d+) contested') {
        Add-Node $b "module:ntdll.dll" "module" "ntdll.dll" @{ name = "ntdll.dll" }
        Add-Node $b "name:blocks" "name" "busy blocks" @{}
        Add-Node $b "name:contested" "name" "contested blocks" @{}
        Add-Edge $b "module:ntdll.dll" "name:blocks" "reach" (Clip-Line $line) ([int]$Matches[1])
        Add-Edge $b "name:game-reach" "name:contested" "reach" (Clip-Line $line) ([int]$Matches[3]) $true
        return
    }

    if ($line -match 'held: (\d+) block\(s\).+surviving thread was pointed into them - (\d+) pointer\(s\) read from (\d+) thread') {
        Add-Node $b "name:surviving-threads" "thread" "surviving threads" @{ role = "surviving" }
        Add-Node $b "name:held-blocks" "name" "held blocks" @{}
        Add-Edge $b "name:surviving-threads" "name:held-blocks" "holds" (Clip-Line $line) ([int]$Matches[2]) $true
        return
    }

    if ($line -match 'witness: player at x=(\d+) y=(\d+), entity ([0-9A-Fa-f]+)') {
        Add-Node $b "name:player" "name" "player" @{}
        Add-Node $b "region:$($Matches[3])" "region" $Matches[3] @{ role = "entity" }
        $haz = $line -match "NOT IN ANY"
        Add-Edge $b "name:player" "region:$($Matches[3])" "witness" (Clip-Line $line) 1 $haz
        return
    }
    if ($line -match 'carry: .* from ([0-9A-Fa-f]+)') {
        Add-Node $b "name:player" "name" "player" @{}
        Add-Node $b "region:$($Matches[1])" "region" $Matches[1] @{ role = "entity" }
        Add-Edge $b "name:player" "region:$($Matches[1])" "witness" (Clip-Line $line)
        return
    }

    if ($line -match 'dsound: sound buffers are created by ([0-9A-Fa-f]+) \(([^)]+)\)') {
        Add-Node $b "name:CreateSoundBuffer-owner" "name" "buffer create" @{}
        Add-Node $b "module:rabiribi.exe" "module" "rabiribi.exe" @{ name = "rabiribi.exe" }
        Add-Node $b "name:dsound" "name" "dsound" @{}
        Add-Edge $b "name:CreateSoundBuffer-owner" "module:rabiribi.exe" "dsound" (Clip-Line $line)
        Add-Edge $b "name:dsound" "name:CreateSoundBuffer-owner" "dsound" (Clip-Line $line)
        return
    }
    if ($line -match 'dsound: the play cursor is read at ([0-9A-Fa-f]+) \(([^)]+)\), called from ([0-9A-Fa-f]+) \(([^)]+)\)') {
        Add-Node $b "name:GetCurrentPosition" "name" "play cursor read" @{}
        Add-Node $b "name:cursor-caller" "name" $Matches[4] @{}
        Add-Node $b "module:rabiribi.exe" "module" "rabiribi.exe" @{ name = "rabiribi.exe" }
        Add-Edge $b "name:GetCurrentPosition" "name:cursor-caller" "dsound" (Clip-Line $line)
        Add-Edge $b "name:GetCurrentPosition" "module:rabiribi.exe" "dsound" (Clip-Line $line)
        return
    }
    if ($line -match 'vtable (IDirectSound\S*)\s+at') {
        Add-Node $b "name:dsound" "name" "dsound" @{}
        Add-Node $b "name:$($Matches[1])" "name" $Matches[1] @{}
        Add-Edge $b "name:dsound" "name:$($Matches[1])" "dsound" (Clip-Line $line)
        return
    }

    if ($line -match '^fault: (\S+) at ([0-9A-Fa-f]+) in ([^,]+), thread (\d+)') {
        $b.outcome = "fault"
        $tid = $Matches[4]
        $site = $Matches[3]
        $mod = $site
        if ($site -match '^([^+\s]+)') { $mod = $Matches[1] }
        Add-Node $b "thread:$tid" "thread" $tid @{ role = "fault" }
        Add-Node $b "module:$mod" "module" $mod @{ name = $mod }
        Add-Edge $b "thread:$tid" "module:$mod" "fault" (Clip-Line $line) 1 $true
        return
    }
    if ($line -match '^\s+(e[a-z]{2}|esi|edi|esp|ebp)=\S+.+heap at ([0-9A-Fa-f]{8})') {
        $reg = $Matches[1]; $heap = $Matches[2]
        $tid = $null
        foreach ($k in $b.nodes.Keys) {
            if ($k -like "thread:*" -and $b.nodes[$k].role -eq "fault") { $tid = $k; break }
        }
        if (-not $tid) { $tid = "name:fault" }
        Add-Node $b $tid "thread" ($tid -replace "thread:", "") @{ role = "fault" }
        Add-Node $b "heap:$heap" "heap" $heap @{}
        Add-Edge $b $tid "heap:$heap" "fault" (Clip-Line $line) 1 $true
        return
    }

    if ($line -match '^save: slot') { $b.stats.saves += 1; if ($b.outcome -eq "unknown") { $b.outcome = "saved" } }
    if ($line -match '^load: slot') { $b.stats.loads += 1; if ($b.outcome -ne "fault") { $b.outcome = "restored" } }
    if ($line -match 'FROZEN: fault') { $b.outcome = "fault" }

    if ($line -match 'coverage: ([\d.]+) MB captured of ([\d.]+) MB writable, ([\d.]+) MB left in the present \(([\d.]+)%\)') {
        $b.stats.coverageMb = [double]$Matches[1]
        $b.stats.presentMb = [double]$Matches[3]
        $b.stats.presentPct = [double]$Matches[4]
        return
    }
    if ($line -match 'save: slot .+, (\d+) threads') { $b.stats.threads = [int]$Matches[1] }
    if ($line -match 'threads: (\d+) tracked') { if (-not $b.stats.threads) { $b.stats.threads = [int]$Matches[1] } }
    if ($line -match 'clobber: .* (\d+) REVERTED') { $b.stats.clobberReverted = [int]$Matches[1] }

    if ($line -match '^\s+iat\s+(\S+)') {
        $fn = $Matches[1]
        Add-Node $b "name:$fn" "name" $fn @{}
        if ($fn -match "Heap|RtlFree") {
            Add-Node $b "name:heaps" "name" "heaps" @{}
            Add-Edge $b "name:$fn" "name:heaps" "setting" (Clip-Line $line)
        }
        return
    }
}

function Parse-File([string]$path) {
    $file = [IO.Path]::GetFileName($path)
    $tag = Get-TagFromFile $file
    $builder = $null
    $out = New-Object System.Collections.Generic.List[object]
    $reader = [IO.StreamReader]::new($path)
    $lineNo = 0
    $sb = $null
    try {
        while ($null -ne ($line = $reader.ReadLine())) {
            $lineNo++
            if ($line -match '^===== session (\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}), pid (\d+) =====') {
                if ($builder) { $s = Finish-Session $builder; $s.logText = $sb.ToString(); $out.Add($s) | Out-Null }
                $builder = New-SessionBuilder $Matches[1] ([int]$Matches[2]) $file $tag
                $builder.firstLine = $lineNo
                $sb = [Text.StringBuilder]::new()
                $sb.AppendLine($line) | Out-Null
                continue
            }
            if ($builder) {
                $sb.AppendLine($line) | Out-Null
                Parse-Attempt $builder $line $lineNo
                Parse-Boundary $builder $line $lineNo
                Parse-Line $builder $line
            }
        }
    } finally { $reader.Close() }
    if ($builder) { $s = Finish-Session $builder; $s.logText = $sb.ToString(); $out.Add($s) | Out-Null }
    return $out
}

function ConvertTo-JsonObject($obj) {
    ConvertTo-Json -InputObject $obj -Depth 10 -Compress
}

function Invoke-Swap([string]$which) {
    $src11 = Join-Path $GameRoot "d3d11.$which.dll"
    $srcGi = Join-Path $GameRoot "dxgi.$which.dll"
    if (-not (Test-Path $src11)) { throw "missing $src11" }
    if (-not (Test-Path $srcGi)) { throw "missing $srcGi" }
    Copy-Item $src11 (Join-Path $GameRoot "d3d11.dll") -Force
    Copy-Item $srcGi (Join-Path $GameRoot "dxgi.dll") -Force
    $log = Join-Path $GameRoot "d3d9_sw_savestate_rabiribi.txt"
    if (Test-Path $log) {
        $stamp = Get-Date -Format "HHmmss"
        $dest = Join-Path $GameRoot "d3d9_sw_savestate_rabiribi.$which.$stamp.txt"
        Move-Item $log $dest -Force
        Write-Host "archived log -> $([IO.Path]::GetFileName($dest))"
    }
    Write-Host "active build is now $which"
}

# --- arena alias from the D3D11 wrapper log (same process, different file) ---
$script:ArenaBase = $null
Get-ChildItem -Path $GameRoot -Filter "d3d11_sw*.log" -ErrorAction SilentlyContinue | ForEach-Object {
    $m = Select-String -Path $_.FullName -Pattern 'arena: reserved \d+ MB at ([0-9A-Fa-f]+)' | Select-Object -Last 1
    if ($m) { $script:ArenaBase = $m.Matches[0].Groups[1].Value }
}

if ($Swap) {
    if ($Game -ne "rabiribi") { throw "-Swap copies Rabi-Ribi's d3d11/dxgi pair; use -Game rabiribi" }
    Invoke-Swap $Swap
}

$patterns = @(
    "d3d9_sw_savestate_*.txt",
    "ss_*.txt",
    "prev_*.txt"
)
$files = foreach ($p in $patterns) { Get-ChildItem -Path $GameRoot -File -Filter $p -ErrorAction SilentlyContinue }
$files = $files | Sort-Object LastWriteTime, Name -Unique

$all = New-Object System.Collections.Generic.List[object]
foreach ($f in $files) {
    try {
        foreach ($s in (Parse-File $f.FullName)) { $all.Add($s) | Out-Null }
    } catch {
        Write-Warning "failed $($f.Name): $($_.Exception.Message)"
    }
}

# One plain text file per run, so a run can be read on its own.
$logDir = Join-Path (Split-Path $PSScriptRoot -Parent) "logs\$Game"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
foreach ($s in $all) {
    $name = ($s.when -replace ':', '-' -replace ' ', '_') + "_pid$($s.pid).txt"
    $path = Join-Path $logDir $name
    $s.logFile = "../logs/$Game/$name"
    $fi = Get-Item $path -ErrorAction SilentlyContinue
    if (-not $fi -or $fi.Length -ne [Text.Encoding]::UTF8.GetByteCount($s.logText)) {
        [IO.File]::WriteAllText($path, $s.logText)
    }
    $s.logText = ""
}
Write-Host "per-run logs in $logDir"

# What changed since the launch before: the build and every cfg value. Done
# before -Date narrows the list, so the first launch shown still compares
# against the one that really came before it.
$prev = $null
foreach ($s in ($all | Sort-Object { $_.when })) {
    $ch = New-Object System.Collections.Generic.List[string]
    if ($prev) {
        if ($s.build -ne $prev.build) { $ch.Add("build $($prev.build) -> $($s.build)") | Out-Null }
        foreach ($k in $s.cfg.Keys) {
            if (-not $prev.cfg.Contains($k)) { $ch.Add("$k = $($s.cfg[$k]) (new)") | Out-Null }
            elseif ($prev.cfg[$k] -ne $s.cfg[$k]) { $ch.Add("$k $($prev.cfg[$k]) -> $($s.cfg[$k])") | Out-Null }
        }
        foreach ($k in $prev.cfg.Keys) { if (-not $s.cfg.Contains($k)) { $ch.Add("$k removed (was $($prev.cfg[$k]))") | Out-Null } }
    }
    $s.changes = $ch
    $prev = $s
}

if ($Date) {
    $keep = New-Object System.Collections.Generic.List[object]
    foreach ($s in $all) { if ($s.when.StartsWith($Date)) { $keep.Add($s) | Out-Null } }
    $all = $keep
}

if ($all.Count -eq 0) {
    Write-Warning "no savestate sessions matched (Date='$Date')"
    return
}

Write-Host ("{0} session(s) from {1} file(s)" -f $all.Count, @($files).Count)

if ($List -or $PSBoundParameters.ContainsKey("List")) {
    $all | Sort-Object { $_.when } | ForEach-Object {
        [pscustomobject]@{
            When    = $_.when
            Pid     = $_.pid
            Tag     = $_.tag
            Outcome = $_.outcome
            Heaps   = $_.stats.heaps
            Threads = $_.stats.threads
            Nodes   = $_.nodes.Count
            Edges   = $_.edges.Count
            File    = $_.file
        }
    } | Format-Table -AutoSize
    if ($List -and -not $Json -and -not $Out) { return }
}

if ($Attempts) {
    $show = @($all | Sort-Object { $_.when })
    if (-not $Date -and $show.Count -gt $Last) { $show = $show[($show.Count - $Last)..($show.Count - 1)] }
    foreach ($s in $show) {
        Write-Host ""
        Write-Host ("== {0}  pid {1}  {2}  build {3}  -> {4}" -f $s.when, $s.pid, $s.file, $s.build, $s.outcome)
        foreach ($c in $s.changes) { Write-Host "   changed: $c" }
        foreach ($a in $s.attempts) {
            $what = $a.kind + $(if ($a.cross) { " (cross-launch)" } else { "" }) + $(if ($a.slot) { " slot $($a.slot)" } else { "" })
            $causes = @($a.hits.Values | Where-Object { $_.level -in @("cause", "warn", "ignore") } | ForEach-Object { "$($_.rule) x$($_.count)" })
            Write-Host ("   L{0,-6} {1,-30} {2,-13} {3}" -f $a.line, $what, $a.status, ($causes -join ", "))
            if ($a.status -notin @("ok", "ignored")) {
                Write-Host "            $($a.text)"
                foreach ($h in $a.hits.Values) {
                    if ($h.level -notin @("cause", "warn")) { continue }
                    Write-Host "            $($h.lines[0])"
                }
            }
        }
    }
    if (-not $Json -and -not $Out) { return }
}

# flatten Generic.List nodes/edges to object[] so the serializer emits arrays
$payloadSessions = foreach ($s in ($all | Sort-Object { $_.when })) {
    @{
        id       = $s.id
        when     = $s.when
        pid      = $s.pid
        file     = $s.file
        tag      = $s.tag
        outcome  = $s.outcome
        policy   = $s.policy
        stats    = $s.stats
        settings = $s.settings
        nodes    = @($s.nodes | ForEach-Object { $_ })
        edges    = @($s.edges | ForEach-Object {
            $ev = @($_.evidence)
            @{
                from     = $_.from
                to       = $_.to
                kind     = $_.kind
                weight   = [int]$_.weight
                hazard   = [bool]$_.hazard
                evidence = @($ev)
            }
        })
    }
}

$payload = @{
    generatedAt = (Get-Date).ToString("s")
    gameRoot    = $GameRoot
    arenaBase   = $script:ArenaBase
    defaultMode = $(if ($OverTime -or -not $Date) { if ($OverTime) { "time" } else { "date" } } else { "date" })
    sessions    = @($payloadSessions)
}

$jsonText = ConvertTo-JsonObject $payload

if ($Json) {
    $jsonPath = Join-Path $PSScriptRoot "connections.json"
    [IO.File]::WriteAllText($jsonPath, $jsonText)
    Write-Host "wrote $jsonPath"
}

$templatePath = Join-Path $PSScriptRoot "viz.template.html"
if (-not (Test-Path $templatePath)) { throw "missing template $templatePath" }
$template = [IO.File]::ReadAllText($templatePath)
$html = $template.Replace("__CONNECT_DATA__", $jsonText)

$graphOut = if ($Out) { $Out } else { Join-Path $PSScriptRoot "connections.html" }
[IO.File]::WriteAllText($graphOut, $html)
Write-Host "wrote $graphOut"

$attemptSessions = foreach ($s in ($all | Sort-Object { $_.when } -Descending)) {
    @{
        when     = $s.when
        pid      = $s.pid
        file     = $s.file
        build    = $s.build
        outcome  = $s.outcome
        changes  = @($s.changes)
        logFile  = $s.logFile
        firstLine = $s.firstLine
        boundary = @(foreach ($r in $s.boundary) {
            $rows = [object[]]@(foreach ($x in $r.rows) { $x })
            $live = [object[]]@(foreach ($x in $r.live) { $x })
            @{ kind = $r.kind; line = $r.line; accuracy = $r.accuracy; rows = $rows; live = $live }
        })
        attempts = @($s.attempts | ForEach-Object {
            @{
                kind    = $_.kind
                verdict = $_.verdict
                status  = $_.status
                slot    = $_.slot
                line    = $_.line
                text    = $_.text
                cross   = [bool]$_.cross
                hits    = @($_.hits.Values | ForEach-Object { @{ rule = $_.rule; level = $_.level; count = $_.count; lines = @($_.lines) } })
            }
        })
    }
}
$rulesOut = @($script:Rules | ForEach-Object { @{ id = $_.id; level = $_.level; meaning = $_.meaning; next = $_.next; status = $_.status } })
$attemptJson = ConvertTo-JsonObject @{ generatedAt = (Get-Date).ToString("s"); gameRoot = $GameRoot; rules = $rulesOut; sessions = @($attemptSessions) }
$attemptTemplate = [IO.File]::ReadAllText((Join-Path $PSScriptRoot "attempts.template.html"))
$attemptOut = Join-Path ([IO.Path]::GetDirectoryName($graphOut)) "attempts.html"
[IO.File]::WriteAllText($attemptOut, $attemptTemplate.Replace("__ATTEMPT_DATA__", $attemptJson))
Write-Host "wrote $attemptOut"

if (-not $NoOpen) { Start-Process $(if ($Graph) { $graphOut } else { $attemptOut }) }
