#requires -Version 5.1
<#
.SYNOPSIS
    The sonar acceptance window maximized and back, in the running app with the simulated sonar: every item must be
    where it was before. The owner, 2026-11-02: "after maximizing the window and restoring it, the whole layout
    drifts".

.DESCRIPTION
    One run of KoggerApp on the test desktop (as qa_live.ps1: profile reset from KoggerApp-perf.reg, the simulated
    sonar KOGGER_SONARQA_DEMO=fast, results in a folder of its own). A baseline of the simulated reference and a test
    of a good sonar fill the window with a result; then, at the top of the page and again scrolled down to the
    checks, the window's items are dumped (windowItems), and again after each change of the window's state
    (windowState: maximized, minimized, normal); the last dump of each round is compared with its first.
    Checked:
      runs      the baseline and the test were made (the result is on the page)
      resize    the window was maximized (larger) and came back to its size
      top       maximized and restored at the top of the page: every item where it was before
      min       minimized and restored, the same
      maxmin    maximized, minimized, maximized again (the taskbar) and restored, the same
      scrolled, scrolledmin   scrolled down to the checks: maximized and restored, and with a minimize between
      qml       no QML error in the log
    -FromRoot <results folder>: that folder (a copy) instead of the runs, the window as it opens on the stand.
    Pictures before / maximized / restored of each round are left in -Out to look at.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\sonarqa_check\qa_resize.ps1 -Exe <build>\KoggerApp.exe -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [string] $Out = "$env:TEMP\kogger_sonarqa_resize",
    [string] $PerfReg = 'D:\YandexDisk\DelphiProjects\KoggerApp_myEdition_transfer\KoggerApp-perf.reg',
    [int]    $RunMs = 30000,
    [int]    $SettleMs = 1500,
    [int]    $TimeoutS = 240,
    [string] $FromRoot,            # a results folder (the stand's) copied in and no runs made: the window as it opens
    [string] $Watch = 'sonarQaWindow'   # the window whose items are dumped: "main" is the main window (no objectName)
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$root = Join-Path $Out 'sonar_qa'
if ($FromRoot) { Copy-Item -LiteralPath $FromRoot -Destination $root -Recurse }
if (Test-Path -LiteralPath $PerfReg) {
    cmd.exe /c "reg import `"$PerfReg`" >nul 2>&1"   # reg.exe reports success on stderr
}
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$utf8 = New-Object System.Text.UTF8Encoding $false

function OutPath([string] $name) { (Join-Path $Out $name).Replace('\', '/') }
function Round([string] $tag, [string[]] $states) {
    # the window through $states, dumped (and grabbed, unless minimized) after each; the last state is "normal"
    $w = '"name": "sonarQaWindow"'
    $d = '"name": "' + $(if ($Watch -eq 'main') { '' } else { $Watch }) + '"'
    $ops = @(('{"op": "windowItems", ' + $d + ', "path": "' + (OutPath "$tag-before.txt") + '"}'),
             ('{"op": "grabWindow", ' + $w + ', "path": "' + (OutPath "$tag-before.png") + '"}'))
    for ($i = 0; $i -lt $states.Count; ++$i) {
        $name = "$tag-$($i + 1)-$($states[$i])"
        $ops += @(('{"op": "windowState", ' + $w + ', "state": "' + $states[$i] + '"}'), ('{"op": "wait", "ms": ' + $SettleMs + '}'),
                  ('{"op": "windowItems", ' + $d + ', "path": "' + (OutPath "$name.txt") + '"}'))
        if ($states[$i] -ne 'minimized') {
            $ops += ('{"op": "grabWindow", ' + $w + ', "path": "' + (OutPath "$name.png") + '"}')
        }
    }
    $ops
}
$rounds = [ordered]@{ 'top' = @('maximized', 'normal'); 'min' = @('minimized', 'normal');
                      'maxmin' = @('maximized', 'minimized', 'maximized', 'normal') }
$scrolled = [ordered]@{ 'scrolled' = @('maximized', 'normal'); 'scrolledmin' = @('maximized', 'minimized', 'normal') }
$steps = @('{"op": "wait", "ms": 3000}',
           '{"op": "storeCall", "method": "openDeveloperSettings"}', '{"op": "wait", "ms": 1000}',
           '{"op": "clickItem", "name": "openSonarQa"}', '{"op": "wait", "ms": 1500}',
           '{"op": "clickItem", "name": "qaDemoRef"}', '{"op": "wait", "ms": 1500}') +
         $(if ($FromRoot) { @() } else {
           @('{"op": "clickItem", "name": "qaStartBaseline"}', ('{"op": "wait", "ms": ' + $RunMs + '}'),
             '{"op": "clickItem", "name": "qaDemoGood"}', '{"op": "wait", "ms": 1500}',
             '{"op": "clickItem", "name": "qaStartUnit"}', ('{"op": "wait", "ms": ' + $RunMs + '}')) }) +
         @($rounds.Keys | ForEach-Object { Round $_ $rounds[$_] }) +
         @('{"op": "scrollTo", "name": "qaChecks"}', ('{"op": "wait", "ms": ' + $SettleMs + '}')) +
         @($scrolled.Keys | ForEach-Object { Round $_ $scrolled[$_] }) +
         @('{"op": "quit"}')
$scriptPath = Join-Path $Out 'script.json'
[System.IO.File]::WriteAllText($scriptPath, ('{"live": true, "steps": [' + ($steps -join ",`n") + ']}'), $utf8)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
$psi.EnvironmentVariables['KOGGER_SONARQA_DEMO'] = 'fast'
$psi.EnvironmentVariables['KOGGER_SONARQA_ROOT'] = $root
$before = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
$proc = Start-TestDesktopProcess $psi
if (-not $proc.WaitForExit($TimeoutS * 1000)) { $proc.Kill(); throw "the app did not end in $TimeoutS s" }

$fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek($before, 'Begin')
$text = (New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd(); $fs.Close()
$mine = @($text -split "`r?`n")
$mine | Set-Content -LiteralPath (Join-Path $Out 'kogger.log') -Encoding UTF8

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
function Dump([string] $name) {
    $p = Join-Path $Out $name
    if (Test-Path -LiteralPath $p) { @(Get-Content -LiteralPath $p) } else { @() }
}

$journal = Join-Path $root 'journal.csv'
$rows = if (Test-Path $journal) { @(Get-Content -LiteralPath $journal | Select-Object -Skip 1 | ForEach-Object { $c = $_ -split ';'; '{0}/{1}' -f $c[1], $c[4] }) } else { @() }
if (-not $FromRoot) {
    Expect (($rows -join ' ') -eq 'baseline/baseline unit/pass') 'runs' ("journal: {0}" -f ($rows -join ' '))
}

$all = [ordered]@{}; foreach ($k in $rounds.Keys) { $all[$k] = $rounds[$k] }; foreach ($k in $scrolled.Keys) { $all[$k] = $scrolled[$k] }
foreach ($tag in $all.Keys) {
    $states = $all[$tag]
    $b = Dump "$tag-before.txt"; $r = Dump "$tag-$($states.Count)-normal.txt"
    if ($tag -eq 'top' -and $Watch -ne 'main') {   # the main window keeps its size
        $m = Dump 'top-1-maximized.txt'
        Expect ($b.Count -gt 0 -and $m.Count -gt 0 -and $r.Count -gt 0 -and $m[0] -ne $b[0] -and $r[0] -eq $b[0]) 'resize' (
            "{0} -> {1} -> {2}" -f $(if ($b.Count) { $b[0] } else { '?' }), $(if ($m.Count) { $m[0] } else { '?' }), $(if ($r.Count) { $r[0] } else { '?' }))
    }
    $diff = @(); $n = [Math]::Max($b.Count, $r.Count)
    for ($i = 0; $i -lt $n; ++$i) {
        $x = if ($i -lt $b.Count) { $b[$i] } else { '(none)' }
        $y = if ($i -lt $r.Count) { $r[$i] } else { '(none)' }
        if ($x -ne $y) { $diff += "    before:   $x`n    restored: $y" }
    }
    Expect ($b.Count -gt 1 -and $diff.Count -eq 0) $tag ("{0}: {1} items, {2} moved" -f ($states -join ' > '), $b.Count, $diff.Count)
    $diff | Select-Object -First 8
}
$qml = @($mine | Where-Object { $_ -match '\.qml:\d+' -and $_ -match 'Error|TypeError|ReferenceError|Unable to assign|Cannot assign|is not defined' })
Expect ($qml.Count -eq 0) 'qml' ($(if ($qml.Count) { $qml[0] } else { 'no QML error' }))
"sonarqa resize: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"logs and pictures: {0}" -f $Out
exit ([int]($failed -gt 0))
