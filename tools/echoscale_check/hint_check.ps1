#requires -Version 5.1
<#
.SYNOPSIS
    The echogram's scale hint in the running app: shown for a moment after a zoom by the user, with the scale
    across, the history the screen holds and the range; not shown after a restore of the scale.

.DESCRIPTION
    One run of KoggerApp over a recording (KOGGER_PROFILE, so the user's settings are not touched), driven by
    the edit script (KOGGER_ECHOEDIT_SCRIPT): hscale (as a restore sets the scale), hzoom (the settings panel's
    step), vzoom (Ctrl + wheel), hint (what each echogram's hint says, logged with the numbers behind it), grab.
    Checked from the log:
      restore   after hscale 1 the hint is hidden
      across    after hzoom -2 (x1/5 since the develop merge): shown, the scale row bright, it says x1/5 and the
                pings the screen holds (the canvas' columns times 5, less up to a column's worth)
      range     after vzoom (two notches in): shown, the range row bright, it says the range shown (two channels:
                +-half of it)
      gone      2 s later: hidden
      end       hzoom past the end: x10, and the hint shown although the scale did not change
    Exit code 0 when every check holds. The grabs (across.png, range.png) are kept in -Out to be looked at.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\echoscale_check\hint_check.ps1 -Exe <build>\KoggerApp.exe -File D:\Temp\perf\00014_03.05.2025.KLF -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [string] $Profile = 'perf',
    [string] $PathPrefix,
    [string] $Out = "$env:TEMP\kogger_echoscale_hint",
    [int]    $TimeoutS = 400
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$times = [string][char]0xD7       # the multiplication sign of the hint
$pm = [string][char]0xB1          # plus-minus
$dots = [string][char]0x2026      # the ellipsis between two ends of a range
$g = $Out -replace '\\', '/'

$steps = @(
    '{"op": "settle", "quietMs": 3000, "timeoutMs": 180000}',
    '{"op": "view", "epochFrac": 1.0}',
    '{"op": "hscale", "scale": 1}', '{"op": "wait", "ms": 600}', '{"op": "hint"}',
    '{"op": "hzoom", "steps": -2}', '{"op": "wait", "ms": 400}', '{"op": "hint"}',
    ('{{"op": "grab", "path": "{0}/across.png"}}' -f $g),
    '{"op": "vzoom", "delta": -240}', '{"op": "wait", "ms": 400}', '{"op": "hint"}',
    ('{{"op": "grab", "path": "{0}/range.png"}}' -f $g),
    '{"op": "wait", "ms": 2000}', '{"op": "hint"}',
    '{"op": "hzoom", "steps": 10}', '{"op": "wait", "ms": 2000}',
    '{"op": "hzoom", "steps": 1}', '{"op": "wait", "ms": 400}', '{"op": "hint"}',
    '{"op": "hscale", "scale": 1}',
    '{"op": "quit"}'
)
$scriptPath = Join-Path $Out 'script.json'
('{"steps": [' + ($steps -join ",`n") + ']}') | Set-Content -LiteralPath $scriptPath -Encoding ASCII

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
$psi.Arguments = '"' + $File + '"'
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
$psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $Out            # gives the probe its window: "grab" needs it
$app = Start-TestDesktopProcess $psi
if (-not $app.WaitForExit($TimeoutS * 1000)) { $app.Kill(); throw "the app did not end in $TimeoutS s" }
$lines = Get-Content -LiteralPath $log -Encoding UTF8
$start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
$mine = @($lines[($start - 1)..($lines.Count - 1)])
$mine | Set-Content -LiteralPath (Join-Path $Out 'kogger.log') -Encoding UTF8

# The first echogram's hint line after each "hint" step, in order.
$re = 'echoedit script: hint across (\d) opacity ([0-9.]+) \[(.*)\] \[(.*)\]; scale (\S+) pings (\d+) columns (\d+) from (\S+) to (\S+)'
$hints = New-Object System.Collections.Generic.List[object]
$take = $false
foreach ($l in $mine) {
    if ($l -match 'echoedit script: hint done') { $take = $false; continue }
    if ($l -match 'echoedit script: hint$|echoedit script: hint across') {
        if ($l -match $re -and -not $take) {
            $inv = [Globalization.CultureInfo]::InvariantCulture
            $hints.Add([pscustomobject]@{
                Across = $Matches[1] -eq '1'; Opacity = [double]::Parse($Matches[2], $inv)
                Text1 = $Matches[3]; Text2 = $Matches[4]; Scale = [double]::Parse($Matches[5], $inv)
                Pings = [int]$Matches[6]; Columns = [int]$Matches[7]
                From = [double]::Parse($Matches[8], $inv); To = [double]::Parse($Matches[9], $inv) })
            $take = $true
        }
    }
}
if ($hints.Count -ne 5) {
    $mine | Where-Object { $_ -match 'echoedit script' } | ForEach-Object { Write-Host ('    ' + ($_ -replace '^\S+ \S+ ', '')) }
    throw "expected 5 hint lines, found $($hints.Count) (log: $log)"
}

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
function Fmt([double] $v, [int] $digits) { return $v.ToString('F' + $digits, [Globalization.CultureInfo]::InvariantCulture) }

$h = $hints[0]
Expect ($h.Opacity -eq 0) 'restore' ("hidden after a restore of the scale: opacity {0}" -f $h.Opacity)

$h = $hints[1]
$full = 5 * $h.Columns
$pingsOk = $h.Pings -le $full -and $h.Pings -ge $full - 6
Expect ($h.Across -and $h.Opacity -ge 0.99 -and [Math]::Abs($h.Scale - 0.2) -lt 1e-4 -and $h.Text1.Contains($times + '1/5') -and
        $h.Text1.Contains([string]$h.Pings) -and $pingsOk) 'across' (
    "[{0}] / [{1}]; opacity {2}, scale {3}, {4} pings for {5} columns" -f $h.Text1, $h.Text2, $h.Opacity,
    $h.Scale, $h.Pings, $h.Columns)

$h = $hints[2]
$lo = [Math]::Min($h.From, $h.To); $hi = [Math]::Max($h.From, $h.To)
$digits = 2
$want = if ($lo -lt 0 -and [Math]::Abs($lo + $hi) -lt 0.05) { $pm + (Fmt $hi $digits) } else { (Fmt $lo $digits) + ' ' + $dots + ' ' + (Fmt $hi $digits) }
Expect ((-not $h.Across) -and $h.Opacity -ge 0.99 -and $h.Text2.Contains($want)) 'range' (
    "[{0}] / [{1}]; opacity {2}, from {3} to {4}, wanted '{5}'" -f $h.Text1, $h.Text2, $h.Opacity, $h.From, $h.To, $want)

$h = $hints[3]
Expect ($h.Opacity -eq 0) 'gone' ("hidden 2 s after the last zoom: opacity {0}" -f $h.Opacity)

$h = $hints[4]
Expect ($h.Across -and $h.Opacity -ge 0.99 -and $h.Scale -eq 10 -and $h.Text1.Contains($times + '10') -and
        -not $h.Text1.Contains($times + '10' + '0')) 'end' ("a step past the end shows x10: [{0}], opacity {1}" -f $h.Text1, $h.Opacity)

"echoscale hint: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"grabs: {0}, {1}" -f (Join-Path $Out 'across.png'), (Join-Path $Out 'range.png')
exit ([int]($failed -gt 0))
