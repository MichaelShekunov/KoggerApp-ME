#requires -Version 5.1
<#
.SYNOPSIS
    Translated words that do not fit: the texts a language cuts (elided "…" or past their line count) where English
    does not.

.DESCRIPTION
    For each language of -Codes and for English, the app (the test desktop, a test profile of its own written fresh,
    the toolset "Maximum", the window at the test desktop's size) opens the settings pane at each group of -Groups in
    turn and the UI probe
    dumps the settings page's item tree (the edit script's "probe" step; ui_probe.cpp writes a Text's words and its
    "truncated"). A
    text counts when it is on screen and cut. The same item in two languages is the same path of child indexes in the
    tree (the same QML builds the same tree), so a text cut in a language and not in English is cut because of the
    translation: listed, and the check fails. Texts cut in English too are by design (a long path, a name) and only
    counted.
    Not covered: pages that need a device (the device settings), popups, the 3D pane's menus.
    Exit code 0 when no translation cuts a text, 1 when one does.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\qml_check\lang_fit_check.ps1 -Exe <build>\KoggerApp.exe [-Codes es,hi]
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string[]] $Codes = @('ru', 'zh', 'es', 'hi'),
    # One group is enough today: a folded group's content stays in the tree (clipped, "visible"), so one dump holds
    # every shown group's texts (measured 2026-10-23: 397 texts in each of 13 dumps). More: a page that builds its
    # content only when unfolded.
    [string[]] $Groups = @('app.preference'),
    [string] $Work = (Join-Path ([System.IO.Path]::GetTempPath()) 'kogger_lang_fit'),
    [string] $Profile = 'langfit',
    [int]    $TimeoutS = 240
)
$ErrorActionPreference = 'Stop'
$Codes = @($Codes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Groups = @($Groups | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
$utf8 = New-Object System.Text.UTF8Encoding $false
$ini = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile.ini"
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$failed = 0

# The texts on screen and cut in one dump: path -> text.
function Cut-Texts([string] $json) {
    $doc = [System.IO.File]::ReadAllText($json, $utf8) | ConvertFrom-Json
    $out = @{}
    $stack = New-Object System.Collections.Stack
    $stack.Push(@($doc.root, ''))
    while ($stack.Count) {
        $pair = $stack.Pop(); $n = $pair[0]; $path = $pair[1]
        if ($n.visible -and $n.truncated -eq $true -and $n.text) { $out[$path] = $n.text }
        if ($n.children) {
            for ($i = 0; $i -lt $n.children.Count; $i++) { $stack.Push(@($n.children[$i], "$path/$i")) }
        }
    }
    return $out
}

function Run-Language([string] $code) {
    $dir = Join-Path $Work $code
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    # the toolset "Maximum" (main/ui/instrumentsGradeList 2, MainWindow's startupSettings): every settings group
    # shown, not the fish finder's few
    $seed = @('[General]', 'schemaVersion=8', '', '[main]', 'ui\welcomeShown=true', 'ui\instrumentsGradeList=2',
              "appLanguageCode=$code", '')
    if (Test-Path -LiteralPath $ini) { Remove-Item -LiteralPath $ini -Force }
    [System.IO.File]::WriteAllText($ini, ($seed -join "`r`n"), $utf8)
    $steps = @('{"op": "wait", "ms": 4000}')
    foreach ($g in $Groups) {
        $steps += '{"op": "pin", "mode": true, "group": "' + $g + '"}'
        $steps += '{"op": "wait", "ms": 1200}'
        $steps += '{"op": "probe", "tag": "' + $g + '", "root": "AppSettingsPage"}'
    }
    $steps += @('{"op": "pin", "mode": false}', '{"op": "quit"}')
    $sp = Join-Path ([System.IO.Path]::GetTempPath()) "kogger_langfit_steps.json"
    [System.IO.File]::WriteAllText($sp, ('{"live": true, "steps": [' + ($steps -join ', ') + ']}'), $utf8)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
    $psi.EnvironmentVariables['PATH'] = 'D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin;' + $env:PATH
    $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
    $psi.EnvironmentVariables['KOGGER_SETTINGS_INI'] = '1'
    $psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $sp
    $psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $dir
    $psi.EnvironmentVariables['KOGGER_UI_PROBE_DELAY_MS'] = '600000'   # no dump of its own: only the steps'
    $app = Start-TestDesktopProcess $psi
    if (-not $app.WaitForExit($TimeoutS * 1000)) { $app.Kill(); throw "the app did not end in $TimeoutS s ($code)" }
    $cut = @{}
    foreach ($g in $Groups) {
        $json = Join-Path $dir "ui-$g.json"
        if (-not (Test-Path $json)) { Write-Host "FAIL $code ${g}: no dump"; $script:failed++; continue }
        $cut[$g] = Cut-Texts $json
    }
    return $cut
}

$base = Run-Language 'en'
$byDesign = 0
foreach ($g in $Groups) { if ($base[$g]) { $byDesign += $base[$g].Count } }
"en: $byDesign texts cut by design (cut in English too)"
foreach ($code in $Codes) {
    $cut = Run-Language $code
    $new = @()
    foreach ($g in $Groups) {
        if (-not $cut[$g]) { continue }
        foreach ($path in $cut[$g].Keys) {
            if (-not ($base[$g] -and $base[$g].ContainsKey($path))) { $new += "$g | $($cut[$g][$path])" }
        }
    }
    if ($new.Count) { $failed++ }
    "{0} {1}: {2} texts cut by the translation" -f ($(if ($new.Count) { 'FAIL' } else { 'ok  ' })), $code, $new.Count
    $new | Sort-Object | ForEach-Object { "      $_" }
}
Remove-Item -LiteralPath $ini -Force -ErrorAction SilentlyContinue
"lang fit check: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
exit ([int]($failed -gt 0))
