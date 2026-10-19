#requires -Version 5.1
<#
.SYNOPSIS
    Pinned settings in the running app: a settings row pinned as a widget acts on the real setting, which the
    settings page saves, and the widget is back after a restart.

.DESCRIPTION
    Two runs of KoggerApp over a recording, in a test profile (KOGGER_PROFILE; its registry key is exported first
    and imported back at the end, so the profile is left as it was), driven by the edit script
    (KOGGER_ECHOEDIT_SCRIPT: pin, pinuse, pinstate, grab).
      run 1   "Bottom track zeroing" (Dataset) pinned; its row found and no widget row yet before, the mirror
              then showing the row's value; the mirror switch clicked: the row's switch flips and the mirror
              with it. "FBS forward / backward" pinned into the same widget: three controls mirrored; the
              mirror's first spin box stepped up: the row's goes up by one.
              Then from the profile's registry: the page saved both values (zeroing, FBS forward steps), and
              the widget definition holds both keys.
      run 2   the same profile again: the widget is back, both rows mirrored with the values run 1 left. Both
              pinned again: each comes out, and the widget, empty, is gone from the stored definitions.
    Exit code 0 when every check holds; the grabs (run1.png, run2.png) stay in -Out.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\qml_check\pin_live_check.ps1 -Exe <build>\KoggerApp.exe -File D:\Temp\perf\00014_03.05.2025.KLF -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [string] $Profile = 'perf',
    [string] $PathPrefix,
    [string] $Out = "$env:TEMP\kogger_pin_live",
    [int]    $TimeoutS = 400
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$regPath = "HKCU\Software\KOGGER\KoggerApp-$Profile"
$regPs = "HKCU:\Software\KOGGER\KoggerApp-$Profile"
$backup = Join-Path $Out 'profile_before.reg'
$g = $Out -replace '\\', '/'
$zero = 'app:dataset.bottomTrackZeroing'
$fbs = 'app:dataset.fbs'

function Run-App([string] $tag, [string[]] $steps) {
    $scriptPath = Join-Path $Out "$tag.json"
    ('{"steps": [' + ($steps -join ",`n") + ']}') | Set-Content -LiteralPath $scriptPath -Encoding ASCII
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
    $psi.Arguments = '"' + $File + '"'
    if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
    $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
    $psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
    $psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $Out            # gives the probe its window: "grab" needs it
    $app = [System.Diagnostics.Process]::Start($psi)
    if (-not $app.WaitForExit($TimeoutS * 1000)) { $app.Kill(); throw "run $tag did not end in $TimeoutS s" }
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    $start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
    $mine = @($lines[($start - 1)..($lines.Count - 1)])
    $mine | Set-Content -LiteralPath (Join-Path $Out "$tag.log") -Encoding UTF8
    return @($mine | Where-Object { $_ -match 'echoedit script: (pin|pinuse|pinstate) ' } | ForEach-Object { $_ -replace '^.*echoedit script: ', '' })
}

# "pinstate <key>: row found, mirror found; switch row on mirror on; ..." -> an object. A pinuse step logs one too,
# right after the use.
function Parse-State([string] $line) {
    if ($line -notmatch '^pinstate (\S+): row (\w+), mirror (\w+); ?(.*)$') { return $null }
    $key = $Matches[1]; $row = $Matches[2] -eq 'found'; $mirror = $Matches[3] -eq 'found'; $rest = $Matches[4]
    $pairs = @()
    if ($rest) {
        foreach ($p in ($rest -split '; ')) {      # each -match below writes $Matches again
            if ($p -match '^(\S+) row (\S+) mirror (\S+)$') { $pairs += [pscustomobject]@{ Kind = $Matches[1]; Row = $Matches[2]; Mirror = $Matches[3] } }
        }
    }
    return [pscustomobject]@{ Key = $key; Row = $row; Mirror = $mirror; Pairs = $pairs }
}

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
function Same([object] $st) { return ($st.Pairs.Count -gt 0) -and (@($st.Pairs | Where-Object { $_.Row -ne $_.Mirror }).Count -eq 0) }
function Show([object] $st) { return (($st.Pairs | ForEach-Object { '{0} {1}/{2}' -f $_.Kind, $_.Row, $_.Mirror }) -join ', ') }

reg export $regPath $backup /y | Out-Null
try {
    # A clean start: no widgets in the profile, so the pin makes a new one.
    Set-ItemProperty "$regPs\main\workspace" -Name widgetsJson -Value '[]'

    $r1 = Run-App 'run1' @(
        '{"op": "settle", "quietMs": 3000, "timeoutMs": 180000}',
        ('{{"op": "pinstate", "key": "{0}"}}' -f $zero),
        ('{{"op": "pin", "key": "{0}", "label": "Bottom track zeroing"}}' -f $zero),
        '{"op": "wait", "ms": 800}',
        ('{{"op": "pinstate", "key": "{0}"}}' -f $zero),
        ('{{"op": "pinuse", "key": "{0}", "index": 0}}' -f $zero),
        '{"op": "wait", "ms": 300}',
        ('{{"op": "pin", "key": "{0}", "label": "FBS forward / backward"}}' -f $fbs),
        '{"op": "wait", "ms": 800}',
        ('{{"op": "pinstate", "key": "{0}"}}' -f $fbs),
        ('{{"op": "pinuse", "key": "{0}", "index": 0}}' -f $fbs),
        '{"op": "wait", "ms": 300}',
        ('{{"op": "grab", "path": "{0}/run1.png"}}' -f $g),
        '{"op": "quit"}')
    $st = @($r1 | ForEach-Object { Parse-State $_ } | Where-Object { $_ })
    if ($st.Count -ne 5) { $r1 | ForEach-Object { '    ' + $_ }; throw "run 1: expected 5 pinstate lines, found $($st.Count)" }

    Expect ($st[0].Row -and -not $st[0].Mirror -and $st[0].Pairs.Count -eq 1) 'run1 before' ("row found, no widget yet: {0}" -f (Show $st[0]))
    Expect ($st[1].Mirror -and (Same $st[1])) 'run1 pinned' ("the mirror shows the row: {0}" -f (Show $st[1]))
    $flipped = $st[2].Pairs.Count -eq 1 -and $st[1].Pairs.Count -eq 1 -and $st[2].Pairs[0].Row -ne $st[1].Pairs[0].Row
    Expect ($flipped -and (Same $st[2])) 'run1 used' ("the mirror's click flipped the row: {0} -> {1}" -f (Show $st[1]), (Show $st[2]))
    $kinds = ($st[3].Pairs | ForEach-Object { $_.Kind }) -join ','
    Expect ($st[3].Mirror -and $kinds -eq 'spin,spin,switch' -and (Same $st[3])) 'run1 fbs' ("three controls mirrored: {0}" -f (Show $st[3]))
    $stepped = $st[4].Pairs.Count -ge 1 -and [int]$st[4].Pairs[0].Row -eq [int]$st[3].Pairs[0].Row + 1
    Expect ($stepped -and (Same $st[4])) 'run1 fbs used' ("the mirror's step raised the row's: {0} -> {1}" -f (Show $st[3]), (Show $st[4]))

    # What the settings page saved, and the widget as stored.
    $zeroSaved = (Get-ItemProperty "$regPs\main\dataset" -ErrorAction SilentlyContinue).zeroingBottomTrackButtonChecked
    $fbsSaved = (Get-ItemProperty "$regPs\main\blackStripes" -ErrorAction SilentlyContinue).fixBlackStripesForwardStepsSpinBox
    $zeroFinal = $st[2].Pairs[0].Row -eq 'on'
    Expect ([string]$zeroSaved -eq ([string]$zeroFinal).ToLower()) 'run1 saved zeroing' ("registry {0}, row {1}" -f $zeroSaved, $st[2].Pairs[0].Row)
    Expect ([string]$fbsSaved -eq $st[4].Pairs[0].Row) 'run1 saved FBS' ("registry {0}, row {1}" -f $fbsSaved, $st[4].Pairs[0].Row)
    $wjson = (Get-ItemProperty "$regPs\main\workspace").widgetsJson
    $defs = $wjson | ConvertFrom-Json
    $sdefs = @(foreach ($d in $defs) { if ($d.kind -eq 'settings') { $d } })
    $keys = if ($sdefs.Count -eq 1) { (@($sdefs[0].items) | ForEach-Object { $_.key }) -join ',' } else { '' }
    Expect ($keys -eq "$zero,$fbs") 'run1 widget stored' ("settings widgets {0}, keys {1}" -f $sdefs.Count, $keys)

    $r2 = Run-App 'run2' @(
        '{"op": "settle", "quietMs": 3000, "timeoutMs": 180000}',
        '{"op": "wait", "ms": 1000}',
        ('{{"op": "pinstate", "key": "{0}"}}' -f $zero),
        ('{{"op": "pinstate", "key": "{0}"}}' -f $fbs),
        ('{{"op": "grab", "path": "{0}/run2.png"}}' -f $g),
        ('{{"op": "pin", "key": "{0}", "label": ""}}' -f $zero),
        ('{{"op": "pin", "key": "{0}", "label": ""}}' -f $fbs),
        '{"op": "wait", "ms": 500}',
        '{"op": "quit"}')
    $st2 = @($r2 | ForEach-Object { Parse-State $_ } | Where-Object { $_ })
    if ($st2.Count -ne 2) { $r2 | ForEach-Object { '    ' + $_ }; throw "run 2: expected 2 pinstate lines, found $($st2.Count)" }
    Expect ($st2[0].Mirror -and (Same $st2[0]) -and $st2[0].Pairs[0].Row -eq $st[2].Pairs[0].Row) 'run2 zeroing' ("back after a restart: {0}" -f (Show $st2[0]))
    Expect ($st2[1].Mirror -and (Same $st2[1]) -and $st2[1].Pairs[0].Row -eq $st[4].Pairs[0].Row) 'run2 fbs' ("back after a restart: {0}" -f (Show $st2[1]))
    # A pin again takes a setting out; the widget goes with its last setting.
    $unpins = @($r2 | Where-Object { $_ -match '^pin app:' })
    $allOut = ($unpins.Count -eq 2) -and (@($unpins | Where-Object { $_ -notmatch '-> widget $' }).Count -eq 0)
    Expect $allOut 'run2 unpinned' (($unpins | ForEach-Object { "[$_]" }) -join ' ')
    $left = @(foreach ($d in ((Get-ItemProperty "$regPs\main\workspace").widgetsJson | ConvertFrom-Json)) { if ($d.kind -eq 'settings') { $d } })
    Expect ($left.Count -eq 0) 'run2 widget gone' ("settings widgets stored: {0}" -f $left.Count)
}
finally {
    reg import $backup | Out-Null      # its "completed" line goes to stderr: not redirected, PS 5.1 takes it for an error
}
"pin live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"grabs: {0}, {1}" -f (Join-Path $Out 'run1.png'), (Join-Path $Out 'run2.png')
exit ([int]($failed -gt 0))
