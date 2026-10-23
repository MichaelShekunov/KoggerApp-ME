#requires -Version 5.1
<#
.SYNOPSIS
    The sonar presets (ID_PRESET) live, on a real nanoSSS with firmware 11.21 on a serial port, and the slot it writes
    emptied again: the table as it was.

.DESCRIPTION
    One run of KoggerApp on the test desktop (a test profile of its own, KOGGER_SETTINGS_INI, written fresh; the
    owner's settings are not touched) with KOGGER_AUTOTEST=<port> (the port connected with no click) and a live edit
    script: wait for the presets, log the table, picture the hot actions panel; save the sonar's current settings
    into slot 0 as a long press on the first frequency button does ("Narrow beam"), log the table, picture; recall
    slot 0 as a tap does; delete slot 0 (presetDelete, no button: this check's undo); log the table, picture; quit.
    The sonar's flash is written twice (the save and the delete): run it only with the owner's word.
    Checked from the app's log:
      answered   the sonar keeps presets (it answered GET 0x7C)
      before     the table as it was (all empty for a fresh 11.21: the buttons show 700 / 450)
      save       slot 0 written: its frame came back used, "Narrow beam", the sonar's settings in it
      recall     slot 0 recalled, not refused
      delete     slot 0 empty again, not refused
      after      the table as before
    Exit code 0 when every check holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\preset_check\preset_sonar.ps1 -Exe <build>\KoggerApp.exe -Port COM13
#>
[CmdletBinding()]
param(
    [string] $Exe,
    [string] $Port,
    [string] $Profile = 'presetsonar',
    [string] $Out = "$env:TEMP\kogger_preset_sonar",
    # the checks again on the log and dumps of an earlier run in $Out, no run: the sonar's flash is not written again
    [switch] $FromLog,
    [int]    $TimeoutS = 150
)

$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding $false
if ($FromLog) {
    $mine = @(Get-Content -LiteralPath (Join-Path $Out 'kogger.log') -Encoding UTF8)
} else {
if (-not $Exe -or -not $Port) { throw 'give -Exe and -Port (or -FromLog for an earlier run)' }
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$ini = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile.ini"
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"

$seed = @('[General]', 'schemaVersion=8', '', '[main]', 'ui\welcomeShown=true', 'ui\instrumentsGradeList=2', '')
if (Test-Path -LiteralPath $ini) { Remove-Item -LiteralPath $ini -Force }
New-Item -ItemType Directory -Force -Path (Split-Path $ini) | Out-Null
[System.IO.File]::WriteAllText($ini, ($seed -join "`r`n"), $utf8)
$steps = @(
    '{"op": "waitPresets", "timeoutMs": 40000}', '{"op": "presets"}',
    '{"op": "probe", "tag": "before", "root": "HotActionsPanel"}',
    '{"op": "presetSave", "slot": 0, "name": "Narrow beam"}', '{"op": "wait", "ms": 2500}', '{"op": "presets"}',
    '{"op": "probe", "tag": "saved", "root": "HotActionsPanel"}',
    '{"op": "presetRecall", "slot": 0}', '{"op": "wait", "ms": 2500}',
    '{"op": "presetDelete", "slot": 0}', '{"op": "wait", "ms": 2500}', '{"op": "presets"}',
    '{"op": "probe", "tag": "after", "root": "HotActionsPanel"}',
    '{"op": "quit"}')
$sp = Join-Path $Out 'steps.json'
[System.IO.File]::WriteAllText($sp, ('{"live": true, "steps": [' + ($steps -join ",`n") + ']}'), $utf8)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
$psi.EnvironmentVariables['PATH'] = 'D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin;' + $env:PATH
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_SETTINGS_INI'] = '1'
$psi.EnvironmentVariables['KOGGER_AUTOTEST'] = $Port
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $sp
$psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $Out
$psi.EnvironmentVariables['KOGGER_UI_PROBE_DELAY_MS'] = '600000'   # no dump of its own: only the steps'
$proc = Start-TestDesktopProcess $psi
if (-not $proc.WaitForExit($TimeoutS * 1000)) { $proc.Kill(); throw "the app did not end in $TimeoutS s" }

$lines = Get-Content -LiteralPath $log -Encoding UTF8
$start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
$mine = @($lines[($start - 1)..($lines.Count - 1)])
$mine | Set-Content -LiteralPath (Join-Path $Out 'kogger.log') -Encoding UTF8
}

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
function After([string] $pattern) {   # the index of the first line matching, -1 when none
    for ($i = 0; $i -lt $mine.Count; $i++) { if ($mine[$i] -match $pattern) { return $i } }
    return -1
}
$tables = @($mine | Where-Object { $_ -match 'echoedit script: presets of ' } | ForEach-Object { $_ -replace '^.*echoedit script: presets of ', '' })
$iSave = After 'preset: save the sonar''s settings into slot 0'
$iRecall = After 'preset: recall slot 0'
$iDelete = After 'preset: delete slot 0'
$slot0 = @($mine | Where-Object { $_ -match 'preset: slot 0: ' } | ForEach-Object { $_ -replace '^.*preset: slot 0: ', '' })
$refused = @($mine | Where-Object { $_ -match 'preset: SETTING v\d refused' })
# @() round the if: as an expression it would hand back a one-line array as a string
$savedLine = @(if ($iSave -ge 0) { $mine[$iSave..($mine.Count - 1)] | Where-Object { $_ -match 'preset: slot 0: \d+ kHz' } | Select-Object -First 1 })
$emptyAfter = @(if ($iDelete -ge 0) { $mine[$iDelete..($mine.Count - 1)] | Where-Object { $_ -match 'preset: slot 0: empty' } })

Expect ((After 'preset: the sonar keeps presets') -ge 0) 'answered' 'the sonar answered GET 0x7C'
Expect ($tables.Count -eq 3) 'before' ($(if ($tables.Count) { $tables[0] } else { 'no table logged' }))
Expect ($iSave -ge 0 -and $savedLine.Count -eq 1 -and $savedLine[0] -match '"Narrow beam"') 'save' ($(if ($savedLine.Count) { $savedLine[0] -replace '^.*preset: ', '' } else { 'no slot frame after the save' }))
Expect ($iRecall -gt $iSave -and -not ($refused | Where-Object { $_ -match 'v0 refused' })) 'recall' ($(if ($iRecall -ge 0) { 'sent, not refused' } else { 'not sent' }))
Expect ($iDelete -gt $iRecall -and $emptyAfter.Count -ge 1 -and -not ($refused | Where-Object { $_ -match 'v2 refused' })) 'delete' ($(if ($emptyAfter.Count) { 'slot 0 empty again' } else { 'slot 0 not reported empty' }))
Expect ($tables.Count -eq 3 -and $tables[2] -eq $tables[0]) 'after' ($(if ($tables.Count -eq 3) { $tables[2] } else { '' }))
Expect ($refused.Count -eq 0) 'no refusal' ($(if ($refused.Count) { $refused[0] -replace '^.*preset: ', '' } else { 'none' }))
# The buttons: a frequency button's glyph in the panel's dump, when the pill keeps its buttons in the tree
foreach ($tag in 'before', 'saved', 'after') {
    $json = Join-Path $Out "ui-$tag.json"
    $glyphs = @()
    if (Test-Path $json) {
        $doc = [System.IO.File]::ReadAllText($json, $utf8) | ConvertFrom-Json
        $stack = New-Object System.Collections.Stack
        $stack.Push($doc.root)
        while ($stack.Count) {
            $n = $stack.Pop()
            if ($n.type -match 'FreqPresetButton') {
                $t = New-Object System.Collections.Stack; $t.Push($n)
                while ($t.Count) { $m = $t.Pop(); if ($m.text) { $glyphs += [string]$m.text }; if ($m.children) { $m.children | ForEach-Object { $t.Push($_) } } }
            }
            if ($n.children) { $n.children | ForEach-Object { $stack.Push($_) } }
        }
    }
    "info buttons ${tag}: " + $(if ($glyphs.Count) { ($glyphs | Select-Object -Unique) -join ' | ' } else { 'not in the panel''s tree (the pill is closed)' })
}
"preset sonar: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"log: {0}" -f (Join-Path $Out 'kogger.log')
exit ([int]($failed -gt 0))
