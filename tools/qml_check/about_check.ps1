<#
.SYNOPSIS
  Offline check of the third-party notices: the About page's licences and the interface icons' provenance
  (THIRD_PARTY_NOTICES.md, third_party/icons/README.md).

.DESCRIPTION
    1. Every SVG in resources/icons/ui is a Tabler icon (its class attribute has "icon-tabler") or is named in
       third_party/icons/README.md: an icon from anywhere else fails until its source and licence are written
       down there. The icons the README gives no known licence are listed.
    2. Every licence text the About page offers (qml/app/AboutPage.qml) is in resources/resources.qrc under
       licenses/, and its file is there; every file the notices' icon table names exists.
    3. The real AboutPage.qml and LicensePage.qml, run offscreen over a mock store and an appUtils that reads
       the licence texts through the qrc's aliases from the source tree: the third-party island lists the
       components and the icon sets, no label ends in a space, and each row opens a licence view whose text
       is the one meant (the GPL, the LGPLs, the FTL, MAVLink's MIT, Tabler's MIT with Pawel Kuna's
       copyright, Ionic's MIT, the CC BY-SA credit of the "zoom fit" icon).
  -Picture saves the About page and the Tabler licence view. Exit code 0 when all pass, 1 when a check
  fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\about_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin] [-Picture about.png]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    [string]$Picture = ''
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "about_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
    exit 2
}
$utf8 = New-Object System.Text.UTF8Encoding $false
function Read-Text([string]$path) { return [System.IO.File]::ReadAllText($path, $utf8) }
function Write-Utf8([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8) }

$fails = 0
function Check([bool]$ok, [string]$what, [string]$detail = '') {
    if ($ok) { Write-Host "PASS $what $(if ($detail) { "($detail)" })" } else { Write-Host "FAIL $what $(if ($detail) { ": $detail" })"; $script:fails++ }
}

# ---- 1. the icons' provenance ----
$readme = Read-Text (Join-Path $repo 'third_party\icons\README.md')
$iconDir = Join-Path $repo 'resources\icons\ui'
$tabler = 0
$named = @()
$unlisted = @()
foreach ($f in Get-ChildItem $iconDir -File -Filter '*.svg' | Sort-Object Name) {
    if ((Read-Text $f.FullName) -match 'icon-tabler') { $tabler++; continue }
    if ($readme.Contains('`' + $f.Name + '`')) { $named += $f.Name } else { $unlisted += $f.Name }
}
Check ($unlisted.Count -eq 0) 'every icon is Tabler or named in third_party/icons/README.md' `
    ("{0} Tabler, {1} named{2}" -f $tabler, $named.Count, $(if ($unlisted.Count) { '; not named: ' + ($unlisted -join ', ') } else { '' }))
$unknownRow = ($readme -split "`n" | Where-Object { $_ -match 'not known' }) -join ' '
$unknown = @([regex]::Matches($unknownRow, '`([^`]+\.svg)`') | ForEach-Object { $_.Groups[1].Value })
if ($unknown.Count) { Write-Host ("INFO icons whose licence is not known yet: {0}" -f ($unknown -join ', ')) }

# ---- 2. the licence texts: the About page's, the qrc's aliases, the notices' icon table ----
$qrcPath = Join-Path $repo 'resources\resources.qrc'
$qrc = Read-Text $qrcPath
$texts = @{}
foreach ($m in [regex]::Matches($qrc, '<file(?:\s+alias="licenses/(?<alias>[^"]+)")?>(?<src>[^<]+)</file>')) {
    $src = $m.Groups['src'].Value.Trim()
    $name = if ($m.Groups['alias'].Success) { $m.Groups['alias'].Value } elseif ($src -match '^licenses/(.+)$') { $Matches[1] } else { $null }
    if (-not $name) { continue }
    $file = Join-Path (Split-Path $qrcPath) $src
    $texts[$name] = if (Test-Path $file) { Read-Text $file } else { '' }
}
$about = Read-Text (Join-Path $repo 'qml\app\AboutPage.qml')
# The components' "file:" entries, and the application's own licence opened by name.
$offered = @([regex]::Matches($about, 'file:\s*"([^"]+)"|openLicenseView\([^,]+,\s*"([^"]+)"\)') |
             ForEach-Object { if ($_.Groups[1].Success) { $_.Groups[1].Value } else { $_.Groups[2].Value } })
$missing = @($offered | Where-Object { -not $texts.ContainsKey($_) -or -not $texts[$_] })
Check ($offered.Count -ge 8 -and $missing.Count -eq 0) 'every licence the About page offers is in the resources, its file there' `
    ("{0} offered{1}" -f $offered.Count, $(if ($missing.Count) { '; missing: ' + ($missing -join ', ') } else { '' }))
$notices = Read-Text (Join-Path $repo 'THIRD_PARTY_NOTICES.md')
$iconTexts = @([regex]::Matches($notices, '\]\((third_party/icons/[^)]+)\)') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
$absent = @($iconTexts | Where-Object { -not (Test-Path (Join-Path $repo $_)) })
Check ($iconTexts.Count -ge 4 -and $absent.Count -eq 0) 'the notices name the icons'' provenance and licence files, all there' `
    ("{0} named{1}" -f $iconTexts.Count, $(if ($absent.Count) { '; absent: ' + ($absent -join ', ') } else { '' }))

# ---- 3. the pages, run ----
function New-ModuleDir([string]$work) {
    $mod = Join-Path $work 'imports\kqml_types'
    New-Item -ItemType Directory -Force $mod | Out-Null
    $lines = @('module kqml_types')
    $src = Join-Path $repo 'qml\kqml_types'
    foreach ($f in Get-ChildItem $src -File | Where-Object { $_.Extension -in '.qml', '.js' } | Sort-Object Name) {
        $s = Read-Text $f.FullName
        $single = $f.Extension -eq '.qml' -and $s -match '(?m)^\s*pragma\s+Singleton'
        if ($f.Name -in 'AppPalette.qml', 'Tokens.qml') {
            # the app's C++ context property "theme"; null here, so the files take their own fallbacks
            $s = ([regex]'QtObject \{').Replace($s, "QtObject {`n    readonly property var theme: null", 1)
        }
        Write-Utf8 (Join-Path $mod $f.Name) $s
        $lines += ('{0}{1} 1.0 {2}' -f $(if ($single) { 'singleton ' } else { '' }), $f.BaseName, $f.Name)
    }
    Write-Utf8 (Join-Path $mod 'qmldir') (($lines -join "`n") + "`n")
    # The app's C++ context property "appUtils": the licence texts through the qrc's aliases.
    $mocks = Join-Path $work 'imports\mocks'
    New-Item -ItemType Directory -Force $mocks | Out-Null
    $json = ($texts | ConvertTo-Json -Depth 3 -Compress)
    Write-Utf8 (Join-Path $mocks 'Mocks.qml') @"
pragma Singleton
import QtQuick
QtObject {
    property QtObject appUtils: QtObject {
        property string gitRevision: "0123456789"
        property var texts: $json
        function licenseText(name) { return texts[name] !== undefined ? texts[name] : "" }
    }
}
"@
    Write-Utf8 (Join-Path $mocks 'qmldir') "module mocks`nsingleton Mocks 1.0 Mocks.qml`n"
}
function Copy-Page([string]$name, [string]$work) {
    $s = Read-Text (Join-Path $repo "qml\app\$name")
    $s = $s -replace '\bappUtils\.', 'Mocks.appUtils.'
    $s = ([regex]'import kqml_types 1.0').Replace($s, "import kqml_types 1.0`nimport mocks 1.0", 1)
    Write-Utf8 (Join-Path $work $name) $s
}

$test = @'
import QtQuick
import QtTest
import kqml_types 1.0

Item {
    id: root
    width: 960; height: 1500

    Rectangle { anchors.fill: parent; color: AppPalette.bg }

    QtObject {
        id: store
        property bool developerMode: false
        property int developerUnlockTaps: 7
        property string licenseViewTitle: ""
        property string licenseViewFile: ""
        property int opened: 0
        function openLicenseView(title, file) { licenseViewTitle = title; licenseViewFile = file; ++opened }
    }

    AboutPage { id: about; x: 20; y: 20; width: 440; store: store }
    LicensePage { id: lic; x: 500; y: 20; width: 440; store: store }

    TestCase {
        name: "About"
        when: windowShown

        function rows(item, out) {
            for (var i = 0; i < item.children.length; ++i) {
                var c = item.children[i]
                if (c.label !== undefined && c.caption !== undefined && c.clicked !== undefined)
                    out.push(c)
                rows(c, out)
            }
            return out
        }

        // What each licence view must show: the file the row opens and a phrase of its text.
        readonly property var meant: [
            { label: "Qt 6.8.3",               caption: "GNU LGPL v3",            file: "COPYING.LGPLv3",           text: "GNU LESSER GENERAL PUBLIC LICENSE" },
            { label: "FFmpeg 7.1",             caption: "GNU LGPL v2.1 or later", file: "COPYING.LGPLv2.1",         text: "GNU LESSER GENERAL PUBLIC LICENSE" },
            { label: "FreeType 2.13.2",        caption: "",                       file: "FTL.TXT",                  text: "The FreeType Project" },
            { label: "MAVLink C library c1fd65e", caption: "MIT",                 file: "MAVLINK-MIT.txt",          text: "Permission is hereby granted" },
            { label: "Tabler Icons",           caption: "MIT",                    file: "TABLER-ICONS-MIT.txt",     text: "Copyright (c) 2020-2026 Paweł Kuna" },
            { label: "Ionicons 5",             caption: "MIT",                    file: "IONICONS-MIT.txt",         text: "Copyright (c) 2015-present Ionic" },
            { label: "",                       caption: "CC BY-SA 4.0",           file: "ZOOM-FIT-CC-BY-SA-4.0.txt", text: "creativecommons.org/licenses/by-sa/4.0" }
        ]

        function test_1_rows() {
            var r = rows(about, [])
            var labels = r.map(function (x) { return x.label })
            compare(r.length, 1 + meant.length, "the application's licence and the components: " + labels.join(" | "))
            for (var i = 0; i < r.length; ++i)
                verify(r[i].label.length > 0 && r[i].label[r[i].label.length - 1] !== " ", "no label ends in a space: '" + r[i].label + "'")
            for (var k = 0; k < meant.length; ++k) {
                var found = r.filter(function (x) {
                    return (meant[k].label === "" || x.label === meant[k].label)
                        && (meant[k].caption === "" || x.caption === meant[k].caption)
                })
                compare(found.length, 1, "one row for " + meant[k].file)
            }
            verify(labels.some(function (l) { return l.indexOf("Timothée Giet") >= 0 }), "the zoom-fit icon's author named")
        }

        function test_2_each_opens_its_text() {
            var r = rows(about, [])
            for (var k = 0; k < meant.length; ++k) {
                var row = r.filter(function (x) {
                    return (meant[k].label === "" || x.label === meant[k].label)
                        && (meant[k].caption === "" || x.caption === meant[k].caption)
                })[0]
                var before = store.opened
                row.clicked()
                compare(store.opened, before + 1, "the row opens a view: " + meant[k].file)
                compare(store.licenseViewFile, meant[k].file)
                compare(store.licenseViewTitle, row.label, "the view's title is the row's label")
                verify(lic.body.length > 200, "the text read: " + meant[k].file + " (" + lic.body.length + " characters)")
                verify(lic.body.indexOf(meant[k].text) >= 0, meant[k].file + " says \"" + meant[k].text + "\"")
            }
            var app = r.filter(function (x) { return x.caption === "GNU GPL v3" })[0]
            app.clicked()
            compare(store.licenseViewFile, "COPYING.GPLv3")
            verify(lic.body.indexOf("GNU GENERAL PUBLIC LICENSE") >= 0, "the application's own GPL v3")
        }

        function test_3_picture() {
            if (!"%PICTURE%".length)
                return
            var r = rows(about, [])
            r.filter(function (x) { return x.label === "Tabler Icons" })[0].clicked()
            wait(200)
            var img = grabImage(root)
            img.save("%PICTURE%")
        }
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('about_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    New-ModuleDir $work
    Copy-Page 'AboutPage.qml' $work
    Copy-Page 'LicensePage.qml' $work
    Copy-Page 'AppIdentityCard.qml' $work
    $pic = if ($Picture) { [System.IO.Path]::GetFullPath($Picture).Replace('\', '/') } else { '' }
    $path = Join-Path $work 'tst_about.qml'
    Write-Utf8 $path $test.Replace('%PICTURE%', $pic)
    $env:PATH = "$QtBin;$env:PATH"
    $fonts = Join-Path $work 'fonts'
    New-Item -ItemType Directory -Force $fonts | Out-Null
    Copy-Item (Join-Path $repo 'resources\fonts\arial.ttf') $fonts
    $env:QT_QPA_FONTDIR = $fonts
    $results = Join-Path $work 'results.txt'
    $p = Start-Process -FilePath $runner -NoNewWindow -Wait -PassThru `
        -ArgumentList @('-platform', 'offscreen', '-import', "`"$(Join-Path $work 'imports')`"", '-input', "`"$path`"",
                        '-o', "`"$results,txt`"") `
        -RedirectStandardOutput (Join-Path $work 'run.out') -RedirectStandardError (Join-Path $work 'run.err')
    $out = @()
    if (Test-Path $results) { $out += @(Get-Content $results -Encoding UTF8) }
    $out | Where-Object { $_ -match '^(PASS|FAIL|Totals|   Actual|   Expected|   Loc)' } | ForEach-Object { Write-Host $_ }
    if ($p.ExitCode -ne 0 -or -not (Test-Path $results)) {
        @(Get-Content (Join-Path $work 'run.err') -Encoding UTF8) | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
        $fails++
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
if ($fails -eq 0) { Write-Host 'about_check: all passed'; exit 0 }
Write-Host "about_check: $fails failed"
exit 1
