#requires -Version 5.1
<#
.SYNOPSIS
    A device's setting typed with the keyboard goes out once, on Enter or Tab, not a digit at a time; Esc sends nothing
    and gives the value back; the -/+ buttons still send at once. KSpinBox.applyWhileTyping false (the device pages).

.DESCRIPTION
    2026-11-03, the owner: typing 450 over 700 in the frequency field sent the sonar 40 (the field's lower limit), 45,
    then 450. One run of tools\guide_shots\guide_shots_live.ps1 -Typing (the app on the test desktop, its own profile,
    a UDP stand playing a nanoSSS 11.21 that acknowledges each setting and logs each TRANSC it gets with the time).
    The advanced section's "Frequency, kHz", 600 at first, is typed over by keys posted to the window as a keyboard's
    are (so the window's shortcuts see them too):
      typed    450, nothing else: the stand gets nothing, the field shows 450 (in the accent colour: typing_1_typed.png)
      return   Enter: the stand gets 450, once
      escape   800 typed, Esc: the stand gets nothing, the field shows 450 again
      tab      700 typed, Tab: the stand gets 700, once
      beside   650 typed, a click on the row's name beside the field -- what the stand got is told, not judged (a click
               on a place that takes no focus leaves the field holding it); then Enter: 650 by then
      plus     the "+": the stand gets 655 at once
    Then the simple view's "Frequency, kHz" (a field of its own, DeviceSettingsPage's DevSpin):
      simple typed   500, nothing else: the stand gets nothing, the field shows 500
      simple return  Enter: the stand gets 500, once
      digits   every frequency the stand got is one of 450, 700, 650, 655, 500
    Exit code 0 when every check holds. 2026-11-03: with either field applying as typed (a mutant each, one run), its
    checks failed.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\qml_check\typed_setting_live.ps1 -Exe <build>\KoggerApp.exe -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $PathPrefix,
    [string] $Out = "$env:TEMP\kogger_typed_setting"
)
$ErrorActionPreference = 'Stop'

if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Recurse -Force }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot '..\guide_shots\guide_shots_live.ps1') -Typing -Exe $Exe -PathPrefix $PathPrefix -Out $Out | Out-Null
if ($LASTEXITCODE -ne 0) { "typed setting live: the run failed (exit $LASTEXITCODE)"; exit 1 }

# what the stand got, when; when each stage's picture was taken (the app's log, the same clock)
$got = @(Get-Content -LiteralPath (Join-Path $Out 'transc.txt') | Where-Object { $_ -match '^(\S+) .* got TRANSC setting (\d+) kHz' } |
         ForEach-Object { [pscustomobject]@{ At = [TimeSpan]::Parse($Matches[1]); Freq = [int]$Matches[2] } })
$at = @{}
Get-Content -LiteralPath (Join-Path $Out 'kogger.log') -Encoding UTF8 | Where-Object { $_ -match '^\S+ (\S+) .*echoedit script: grab .*(typing_\d)_\w+\.png written' } |
    ForEach-Object { $at[$Matches[2]] = [TimeSpan]::Parse($Matches[1]) }
function Between([string] $from, [string] $to) {
    $a = if ($from) { $at[$from] } else { [TimeSpan]::Zero }
    @($got | Where-Object { $_.At -gt $a -and $_.At -le $at[$to] } | ForEach-Object { $_.Freq })
}
$label = [regex]::Unescape('"\u0427\u0430\u0441\u0442\u043e\u0442\u0430, \u043a\u0413\u0446"')
function Field([string] $stage, [switch] $Simple) {   # the frequency field's text: the advanced section's (the last row
    $f = Get-ChildItem -LiteralPath (Join-Path $Out 'items') -Filter "$stage*.txt" | Select-Object -First 1   # of that name)
    if (-not $f) { return '?' }                                                                               # or the simple view's
    $lines = @(Get-Content -LiteralPath $f.FullName -Encoding UTF8)
    $rows = @($lines | Select-String -SimpleMatch $label)
    $k = $(if ($Simple) { $rows | Select-Object -First 1 } else { $rows | Select-Object -Last 1 }).LineNumber
    if (-not $k) { return '?' }
    $row = $lines[$k..([Math]::Min($k + 15, $lines.Count - 1))] | Where-Object { $_ -match 'TextInput .*"(.*)"$' } | Select-Object -First 1
    if ($row -match '"(.*)"$') { $Matches[1] } else { '?' }
}

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
$missing = @(1..9 | ForEach-Object { "typing_$_" } | Where-Object { -not $at.ContainsKey($_) })
Expect ($missing.Count -eq 0) 'stages' ("pictures missing: {0}" -f $(if ($missing) { $missing -join ', ' } else { 'none' }))
$s = Between '' 'typing_1'; $f = Field 'typing_1'
Expect ($s.Count -eq 0 -and $f -eq '450') 'typed' ("the stand got [{0}], the field shows {1}" -f ($s -join ', '), $f)
$s = Between 'typing_1' 'typing_2'
Expect (($s -join ',') -eq '450') 'return' ("the stand got [{0}]" -f ($s -join ', '))
$s = Between 'typing_2' 'typing_3'; $f = Field 'typing_3'
Expect ($s.Count -eq 0 -and $f -eq '450') 'escape' ("the stand got [{0}], the field shows {1}" -f ($s -join ', '), $f)
$s = Between 'typing_3' 'typing_4'
Expect (($s -join ',') -eq '700') 'tab' ("the stand got [{0}]" -f ($s -join ', '))
"     beside: a click beside the field, the stand got [{0}]" -f ((Between 'typing_4' 'typing_5') -join ', ')
$s = Between 'typing_4' 'typing_6'
Expect (($s -join ',') -eq '650') 'beside, then Enter' ("the stand got [{0}]" -f ($s -join ', '))
$s = Between 'typing_6' 'typing_7'; $f = Field 'typing_7'
Expect (($s -join ',') -eq '655' -and $f -eq '655') 'plus' ("the stand got [{0}], the field shows {1}" -f ($s -join ', '), $f)
$s = Between 'typing_7' 'typing_8'; $f = Field 'typing_8' -Simple
Expect ($s.Count -eq 0 -and $f -eq '500') 'simple typed' ("the stand got [{0}], the field shows {1}" -f ($s -join ', '), $f)
$s = Between 'typing_8' 'typing_9'
Expect (($s -join ',') -eq '500') 'simple return' ("the stand got [{0}]" -f ($s -join ', '))
$odd = @($got | Where-Object { @(450, 700, 650, 655, 500) -notcontains $_.Freq } | ForEach-Object { $_.Freq })
Expect ($odd.Count -eq 0) 'digits' ("frequencies got: [{0}], none else expected" -f (($got | ForEach-Object { $_.Freq }) -join ', '))
"typed setting live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"logs: {0}" -f $Out
exit $(if ($failed -eq 0) { 0 } else { 1 })
