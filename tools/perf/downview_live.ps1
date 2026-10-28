#requires -Version 5.1
<#
.SYNOPSIS
    The down view (src/down_view.h) in the app on a two-sided recording: the echograms switched to each of the three
    by their channel names, a ping's values as the echogram draws them checked against the two sides, and a picture
    of each.

.DESCRIPTION
    One run of KoggerApp on the test desktop with -File opened (KOGGER_PROFILE reset from the transfer folder's
    KoggerApp-perf.reg) and a live edit script: settle, the timeline at -Epoch, then "downView" with each mode (the
    echograms on "<port>|<address>|down-<mode>", channel 2 none), the last back on the two sides; a window picture
    after each, and that ping's sides and down views as Epoch::chartTo draws them (down.json).
    Checked:
      names     each switch logged with its channel names
      sides     the ping's two sides came out, not empty and not the same
      energy    each sample the root mean square of the sides' amplitudes (bytes 15 a doubling), worked out here
      mean      each sample the sides' mean, a half up
      min       each sample the smaller side
      pictures  the four pictures written and different from one another
    Exit code 0 when every check holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\perf\downview_live.ps1 -Exe <build>\KoggerApp.exe -File D:\Temp\perf\00014_03.05.2025.KLF -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [string] $PathPrefix,
    [int]    $Epoch = 24600,            # 00014: something under the boat at 4-8 m that both sides see
    [string] $Profile = 'perf',
    [int]    $TimeoutS = 240,
    [string] $Out = "$env:TEMP\kogger_downview_live",
    [string] $PerfReg = 'D:\YandexDisk\DelphiProjects\KoggerApp_myEdition_transfer\KoggerApp-perf.reg'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
New-Item -ItemType Directory -Force -Path $Out | Out-Null
if (Test-Path -LiteralPath $PerfReg) {
    # reg.exe reports success on stderr; under 'Stop' PowerShell 5.1 would take that for a failure.
    cmd.exe /c "reg import `"$PerfReg`" >nul 2>&1"
}
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$dump = Join-Path $Out 'down.json'
Remove-Item -LiteralPath $dump -ErrorAction SilentlyContinue
$modes = @('min', 'energy', 'mean')
$steps = @(@{ op = 'settle'; quietMs = 3000 }, @{ op = 'wait'; ms = 1000 }, @{ op = 'view'; epoch = $Epoch })
foreach ($m in $modes) {
    $step = @{ op = 'downView'; mode = $m }
    if ($m -eq $modes[0]) { $step.epoch = $Epoch; $step.path = $dump }
    $steps += $step, @{ op = 'wait'; ms = 1500 }, @{ op = 'grab'; path = (Join-Path $Out "$m.png") }
}
$steps += @{ op = 'downView' }, @{ op = 'wait'; ms = 1500 }, @{ op = 'grab'; path = (Join-Path $Out 'sides.png') }, @{ op = 'quit' }
$scriptPath = Join-Path $Out 'script.json'
(@{ live = $false; steps = $steps } | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath $scriptPath -Encoding ASCII

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
$psi.Arguments = '"' + $File + '"'
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
$psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $Out   # gives the probe its window: "grab" needs it
$before = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
$app = Start-TestDesktopProcess $psi
if (-not $app.WaitForExit($TimeoutS * 1000)) {
    $app.Kill()
    throw "the app did not end in $TimeoutS s"
}
$fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek($before, 'Begin')
$text = (New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd(); $fs.Close()
$mine = @($text -split "`r?`n")
$mine | Set-Content -LiteralPath (Join-Path $Out 'kogger.log') -Encoding UTF8

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
# "downView <ch1>, <ch2>" (the step's own line; the script's "downView done ..." after each step is not one)
$switches = @($mine | Where-Object { $_ -match 'echoedit script: downView [^ ]+\|[^ ]+, [^ ]+$' } |
    ForEach-Object { $_ -replace '^.*echoedit script: downView ', '' })
$named = @($modes | Where-Object { $m = $_; @($switches | Where-Object { $_ -match "\|down-$m, none$" }).Count -eq 1 })
Expect ($named.Count -eq $modes.Count -and $switches.Count -eq $modes.Count + 1) 'names' ($switches -join '; ')
# "downView lists: <combo class> <down views> of <items>; ...": the echogram's two lists offer the three
$lists = @($mine | Where-Object { $_ -match 'echoedit script: downView lists: ' } | Select-Object -First 1 |
    ForEach-Object { ($_ -replace '^.*downView lists: ', '') -split '; ' })
$withThree = @($lists | Where-Object { $_ -match ' 3 of \d+$' })
Expect ($withThree.Count -ge 2) 'lists' ($lists -join '; ')

if (-not (Test-Path -LiteralPath $dump)) {
    Expect $false 'sides' "no $dump"
} else {
    $d = Get-Content -LiteralPath $dump -Raw | ConvertFrom-Json
    $s0 = @($d.side0); $s1 = @($d.side1)
    $n = [Math]::Min($s0.Count, $s1.Count)
    $differ = @(0..($n - 1) | Where-Object { $s0[$_] -ne $s1[$_] }).Count
    Expect ($n -gt 100 -and $differ -gt $n / 10) 'sides' ("ping {0}: {1} samples, the sides differ in {2}, resolution {3} m" -f $d.epoch, $n, $differ, $d.resolution)
    $want = @{ 'down-energy' = New-Object int[] $n; 'down-mean' = New-Object int[] $n; 'down-min' = New-Object int[] $n }
    for ($i = 0; $i -lt $n; $i++) {
        $a = [int]$s0[$i]; $b = [int]$s1[$i]
        $rms = [Math]::Sqrt(([Math]::Pow(2.0, 2.0 * $a / 15.0) + [Math]::Pow(2.0, 2.0 * $b / 15.0)) / 2.0)
        $e = [int][Math]::Round(15.0 * [Math]::Log($rms, 2.0), [MidpointRounding]::AwayFromZero)
        $want['down-energy'][$i] = [Math]::Max(0, [Math]::Min(255, $e))
        $want['down-mean'][$i] = [int][Math]::Floor(($a + $b + 1) / 2)
        $want['down-min'][$i] = [Math]::Min($a, $b)
    }
    foreach ($k in 'down-energy', 'down-mean', 'down-min') {
        $got = @($d.$k)
        $bad = @(0..($n - 1) | Where-Object { [int]$got[$_] -ne $want[$k][$_] })
        $first = if ($bad.Count) { ", first at {0}: {1}, worked out {2}" -f $bad[0], $got[$bad[0]], $want[$k][$bad[0]] } else { '' }
        Expect ($got.Count -ge $n -and $bad.Count -eq 0) ($k -replace 'down-', '') ("{0} of {1} samples as worked out from the sides{2}" -f ($n - $bad.Count), $n, $first)
    }
}

$pics = @($modes + 'sides' | ForEach-Object { Join-Path $Out "$_.png" })
$there = @($pics | Where-Object { Test-Path -LiteralPath $_ })
$hashes = @($there | ForEach-Object { (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash } | Sort-Object -Unique)
Expect ($there.Count -eq 4 -and $hashes.Count -eq 4) 'pictures' ("{0} of 4 written, {1} different" -f $there.Count, $hashes.Count)
"down view live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"logs: {0}" -f $Out
exit $(if ($failed -eq 0) { 0 } else { 1 })
