#requires -Version 5.1
<#
.SYNOPSIS
    The UI languages: each one listed has a complete translation; the app starts in the one chosen and switches.

.DESCRIPTION
    1. Offline: the languages LanguageController lists (src/language_controller.h) against the translations
       CMakeLists.txt compiles (translations/translation_<code>.ts): one for each, none extra; the old index order
       (legacyCode) the same as the first languages'. The translations of -Complete (not en, the sources; not pl,
       kept as upstream left it, a part translated) through tools/ts_check: nothing unfinished, nothing broken.
    2. With -Exe, in the app (the test desktop, a test profile of its own written fresh), for each of -Codes: started
       with that language chosen, it is the one in use, the picker shows it and a text is shown as its .ts has it; the
       settings pane at the Interface group pictured into -Shots (for an eye: the script drawn, the names in the picker
       fit). Then: a profile with only the old index (main/appLanguage=3) starts in Chinese and keeps the choice as its
       code; a switch at run time from Russian to Hindi shows Hindi and keeps hi, the old index left at Russian.
    Exit code 0 when every check holds, 1 when one fails.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\qml_check\lang_check.ps1 [-Exe <build>\KoggerApp.exe] [-Shots <dir>]
#>
[CmdletBinding()]
param(
    [string]   $Exe = '',
    [string[]] $Codes = @('en', 'ru', 'zh', 'es', 'hi'),
    [string[]] $Complete = @('ru', 'zh', 'es', 'hi'),
    [string]   $Shots = '',
    [string]   $Profile = 'langcheck',
    [int]      $TimeoutS = 120
)
$ErrorActionPreference = 'Stop'
# powershell -File passes "ru,zh" as one string
$Codes = @($Codes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Complete = @($Complete | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

$repo =(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$utf8 = New-Object System.Text.UTF8Encoding $false
$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}

# --- 1. the languages and their translations, offline ---------------------------------------------------------------
$header = [System.IO.File]::ReadAllText((Join-Path $repo 'src\language_controller.h'), $utf8)
$listed = @([regex]::Matches($header, '\{ QStringLiteral\("([a-z]{2,3})"\), QStringLiteral\(') | ForEach-Object { $_.Groups[1].Value })
$legacy = @([regex]::Matches([regex]::Match($header, 'order\[\] = \{([^}]*)\}').Groups[1].Value, '"([a-z]+)"') | ForEach-Object { $_.Groups[1].Value })
Expect ($listed.Count -ge 2) 'the languages listed' ($listed -join ' ')
Expect ($legacy.Count -gt 0 -and (($listed[0..($legacy.Count - 1)]) -join ' ') -eq ($legacy -join ' ')) 'the old index order' `
    "legacyCode $($legacy -join ' '), the list starts $(($listed | Select-Object -First $legacy.Count) -join ' ')"

$cmake = [System.IO.File]::ReadAllText((Join-Path $repo 'CMakeLists.txt'), $utf8)
$compiled = @([regex]::Matches([regex]::Match($cmake, '(?s)TS_FILES(.*?)RESOURCE_PREFIX').Groups[1].Value, 'translation_([a-z]+)\.ts') | ForEach-Object { $_.Groups[1].Value })
$missing = @($listed | Where-Object { $compiled -notcontains $_ -or -not (Test-Path (Join-Path $repo "translations\translation_$_.ts")) })
$extra = @($compiled | Where-Object { $listed -notcontains $_ })
Expect ($missing.Count -eq 0 -and $extra.Count -eq 0) 'a translation compiled for each language, none extra' `
    "compiled $($compiled -join ' '); missing [$($missing -join ' ')], extra [$($extra -join ' ')]"

# The translations' contents: ts_check (unfinished messages, placeholders, tags, line breaks, edge whitespace).
$tsOut = @(& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'tools\ts_check\ts_check.ps1') @Complete)
$tsCode = $LASTEXITCODE
Expect ($tsCode -eq 0) "translations $($Complete -join ' ') complete (ts_check)" `
    ((@($tsOut | Where-Object { $_ -match 'messages' }) + @($tsOut | Where-Object { $_ -match '^  ' } | Select-Object -First 5)) -join '; ')

# A message's text as the app shows it in a language: its translation, or the source where that is empty (the .qm
# leaves the message out) or the language is the sources' own.
function Shown([string] $code, [string] $context, [string] $source) {
    if ($code -eq 'en') { return $source }
    $xml = New-Object System.Xml.XmlDocument
    $xml.Load((Join-Path $repo "translations\translation_$code.ts"))
    foreach ($ctx in $xml.SelectNodes('/TS/context')) {
        if ($ctx.SelectSingleNode('name').InnerText -ne $context) { continue }
        foreach ($m in $ctx.SelectNodes('message')) {
            if ($m.SelectSingleNode('source').InnerText -ne $source) { continue }
            $t = $m.SelectSingleNode('translation')
            if (-not $t -or $t.GetAttribute('type') -eq 'unfinished' -or $t.InnerText -eq '') { return $source }
            return $t.InnerText
        }
    }
    return $source
}

# --- 2. in the app ---------------------------------------------------------------------------------------------------
if ($Exe) {
    . (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
    $ini = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile.ini"
    $log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
    if ($Shots) { New-Item -ItemType Directory -Force -Path $Shots | Out-Null }
    $probe = 'AppSettingsPage', 'Interface'
    function Expected([string] $code) { Shown $code $probe[0] $probe[1] }
    function Tr-Step([string] $apply) {
        '{"op": "tr", "context": "' + $probe[0] + '", "source": "' + $probe[1] + '"' + $(if ($apply) { ', "apply": "' + $apply + '"' } else { '' }) + '}'
    }
    # One run: the profile written fresh with these [main] lines, the steps, the log of the run back.
    function Run-App([string[]] $main, [string[]] $steps) {
        $seed = @('[General]', 'schemaVersion=8', '', '[main]', 'ui\welcomeShown=true') + $main + @('')
        if (Test-Path -LiteralPath $ini) { Remove-Item -LiteralPath $ini -Force }
        [System.IO.File]::WriteAllText($ini, ($seed -join "`r`n"), $utf8)
        $sp = Join-Path ([System.IO.Path]::GetTempPath()) "kogger_lang_steps.json"
        $all = @('{"op": "wait", "ms": 4000}') + $steps + @('{"op": "quit"}')
        [System.IO.File]::WriteAllText($sp, ('{"live": true, "steps": [' + ($all -join ', ') + ']}'), $utf8)
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
        $psi.EnvironmentVariables['PATH'] = 'D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin;' + $env:PATH
        $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
        $psi.EnvironmentVariables['KOGGER_SETTINGS_INI'] = '1'
        $psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $sp
        if ($Shots) { $psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $Shots }
        $app = Start-TestDesktopProcess $psi
        if (-not $app.WaitForExit($TimeoutS * 1000)) { $app.Kill(); throw "the app did not end in $TimeoutS s" }
        $lines = @(Get-Content -LiteralPath $log -Encoding UTF8)
        $start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
        return $lines[($start - 1)..($lines.Count - 1)]
    }
    function Tr-Lines($lines) {
        @($lines | ForEach-Object { if ($_ -match 'echoedit script: tr (\S+) "(.*)" -> "(.*)"$') { [pscustomobject]@{ code = $Matches[1]; text = $Matches[3] } } })
    }
    function Ini-Main([string] $key) {
        $m = [regex]::Match([System.IO.File]::ReadAllText($ini, $utf8), "(?m)^$([regex]::Escape($key))=(.*?)\r?$")
        if ($m.Success) { $m.Groups[1].Value } else { '' }
    }

    foreach ($code in $Codes) {
        # The picker asked three times, 1.5 s apart: the first look at the settings in Chinese can take longer (the
        # Han font's first use; once "row none" at 1.5 s, 2026-10-23)
        $steps = @((Tr-Step ''), '{"op": "pin", "mode": true, "group": "app.preference"}')
        foreach ($k in 1..3) { $steps += @('{"op": "wait", "ms": 1500}', '{"op": "pinstate", "key": "app:interface.language"}') }
        if ($Shots) { $steps += '{"op": "grab", "path": "' + (Join-Path $Shots "lang_$code.png").Replace('\', '/') + '"}' }
        $steps += '{"op": "pin", "mode": false}'
        $lines = Run-App @("appLanguageCode=$code") $steps
        $tr = Tr-Lines $lines | Select-Object -First 1
        $want = Expected $code
        Expect ($tr -and $tr.code -eq $code -and $want -and $tr.text -eq $want) "started in $code" `
            $(if ($tr) { "in use $($tr.code), '$($probe[1])' shown as '$($tr.text)', the .ts '$want'" } else { "no tr line in $log" })
        $asked = @($lines | Where-Object { $_ -match 'echoedit script: pinstate app:interface.language' })
        $picker = $asked | Where-Object { $_ -match 'row found' } | Select-Object -First 1
        Expect ($picker -match "tabs row $code\b") "the picker on $code" `
            ("{0} of {1} asks found it; {2}" -f @($asked | Where-Object { $_ -match 'row found' }).Count, $asked.Count, "$(if ($picker) { $picker } else { $asked | Select-Object -Last 1 })".Trim())
        if ($Shots) {
            $shot = Join-Path $Shots "lang_$code.png"
            Expect ((Test-Path $shot) -and (Get-Item $shot).Length -gt 0) "picture $code" $shot
        }
    }

    # The old index only: Chinese (3 in the old order), kept as its code from then on.
    $lines = Run-App @('appLanguage=3') @((Tr-Step ''))
    $tr = Tr-Lines $lines | Select-Object -First 1
    Expect ($tr -and $tr.code -eq 'zh' -and $tr.text -eq (Expected 'zh') -and (Ini-Main 'appLanguageCode') -eq 'zh') 'the old index read' `
        "in use $($tr.code), shown '$($tr.text)'; kept as appLanguageCode=$(Ini-Main 'appLanguageCode')"

    # A switch at run time to a language the old index has no place for: shown at once, kept as the code, the index
    # left at the last language it has (a build that reads the index only opens in that one).
    $lines = Run-App @('appLanguageCode=ru', 'appLanguage=1') @((Tr-Step ''), (Tr-Step 'hi'))
    $trs = Tr-Lines $lines
    Expect ($trs.Count -eq 2 -and $trs[0].code -eq 'ru' -and $trs[1].code -eq 'hi' -and $trs[1].text -eq (Expected 'hi')) 'switched to hi' `
        (($trs | ForEach-Object { "$($_.code) '$($_.text)'" }) -join ' -> ')
    Expect ((Ini-Main 'appLanguageCode') -eq 'hi' -and (Ini-Main 'appLanguage') -eq '1') 'the switch kept' `
        "appLanguageCode=$(Ini-Main 'appLanguageCode'), appLanguage=$(Ini-Main 'appLanguage')"
    Remove-Item -LiteralPath $ini -Force -ErrorAction SilentlyContinue
}

"lang check: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
exit ([int]($failed -gt 0))
