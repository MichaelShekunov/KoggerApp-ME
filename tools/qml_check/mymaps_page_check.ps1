<#
.SYNOPSIS
  Offline check of the "My maps" page (qml/app/MyMapsTab.qml, docs/map-export.md).

.DESCRIPTION
  Runs the real page over the kqml_types sources with a mock library (the C++ MapLibrary's properties and
  calls) and a mock layer (MapLayer):
    1. with no maps: the empty state and "Save my map" (opens the export page);
    2. with two maps: a card each, newest first, with the title, the author or "Author not given", the licence,
       the size and the depths;
    3. a card's switch hides that map; "Show on the map" asks to fly to it; "Remove" asks first, removes on
       the second press;
    4. the depth under the boat by the map shows when there is one;
    5. the layer switches (the maps, the depths, the mosaic) write the layer;
    7. how to open a map: the desktop's ways, or on Android from the messenger.
  -Picture saves the page. -Preview <png> gives the cards a picture (e.g. a .kmap's preview.png).
  Exit code 0 when all pass, 1 when a check fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\mymaps_page_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin] [-Picture page.png] [-Preview preview.png]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    [string]$Picture = '',
    [string]$Preview = ''
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "mymaps_page_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
    exit 2
}

$utf8 = New-Object System.Text.UTF8Encoding $false
function Write-Utf8([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8) }

# kqml_types from the sources, with a qmldir of its own (the built one prefers the app's resources).
function New-ModuleDir([string]$work) {
    $mod = Join-Path $work 'imports\kqml_types'
    New-Item -ItemType Directory -Force $mod | Out-Null
    $lines = @('module kqml_types')
    $src = Join-Path $repo 'qml\kqml_types'
    foreach ($f in Get-ChildItem $src -File | Where-Object { $_.Extension -in '.qml', '.js' } | Sort-Object Name) {
        $s = [System.IO.File]::ReadAllText($f.FullName, $utf8)
        $single = $f.Extension -eq '.qml' -and $s -match '(?m)^\s*pragma\s+Singleton'
        if ($f.Name -in 'AppPalette.qml', 'Tokens.qml') {
            $s = ([regex]'QtObject \{').Replace($s, "QtObject {`n    readonly property var theme: null", 1)
        }
        Write-Utf8 (Join-Path $mod $f.Name) $s
        $lines += ('{0}{1} 1.0 {2}' -f $(if ($single) { 'singleton ' } else { '' }), $f.BaseName, $f.Name)
    }
    Write-Utf8 (Join-Path $mod 'qmldir') (($lines -join "`n") + "`n")
}

$test = @'
import QtQuick
import QtTest
import kqml_types 1.0

Rectangle {
    id: root
    width: 440; height: 1000
    color: AppPalette.bg

    QtObject {
        id: store
        property int exportOpened: 0
        function openMapExportSettings() { exportOpened += 1 }
    }
    QtObject {
        id: lib
        property var maps: []
        property string lastMessage: ""
        property bool lastOk: true
        property bool boatDepthValid: false
        property double boatDepth: 0
        property string boatDepthMap: ""
        property var hidden: []
        property var flown: []
        property var removed: []
        function importFile(p) { lastMessage = "Opened: " + p; return true }
        function setVisible(id, v) { if (!v) hidden = hidden.concat([id]) }
        function showOnMap(id) { flown = flown.concat([id]) }
        function remove(id) { removed = removed.concat([id]) }
    }
    QtObject {
        id: lay
        property bool visible: true
        property bool depthVisible: true
        property bool mosaicVisible: false
    }

    readonly property var twoMaps: [
        { id: "Lake_b.kmap", title: "Yerevan lake, south bay", author: "", license: "All rights reserved", licenseId: "all-rights-reserved",
          visible: true, hasDepth: true, hasMosaic: true, minDepth: 0.37, maxDepth: 20.05, widthM: 490, heightM: 410, preview: "%PREVIEW%" },
        { id: "Sevan_a.kmap", title: "Sevan, Shorzha", author: "Ani P.", license: "CC BY 4.0", licenseId: "CC-BY-4.0",
          visible: true, hasDepth: true, hasMosaic: false, minDepth: 2.1, maxDepth: 31.4, widthM: 1830, heightM: 1210, preview: "%PREVIEW%" } ]

    Flickable {
        anchors.fill: parent
        contentHeight: page.height + 20
        MyMapsTab {
            id: page
            x: 10; y: 10
            width: root.width - 20
            store: store
            library: lib
            mapsLayer: lay
        }
    }

    function all(item, pred, out) {
        out = out || []
        if (!item) return out
        if (pred(item)) out.push(item)
        var kids = item.children || []
        for (var i = 0; i < kids.length; ++i) all(kids[i], pred, out)
        if (item.contentItem && item.contentItem !== item) all(item.contentItem, pred, out)
        return out
    }
    function shown(item) {
        for (var it = item; it; it = it.parent) {
            if (!it.visible || it.opacity === 0 || it.height === 0) return false
            if (it === page) return true
        }
        return true
    }
    function texts(s) {
        return all(page, function(c) { return c.hasOwnProperty("wrapMode") && typeof c.text === "string" && c.text.indexOf(s) >= 0 && shown(c) })
    }
    function buttons(s) {
        return all(page, function(c) { return c.hasOwnProperty("normalBg") && c.text === s && shown(c) })
    }
    function row(label) {
        var r = all(page, function(c) { return c.hasOwnProperty("chevron") && c.label === label })
        return r.length ? r[0] : null
    }
    function cardSwitches() {
        return all(page, function(c) { return c.hasOwnProperty("flat") && c.toolTipText === "Show this map" && shown(c) })
    }

    TestCase {
        name: "MyMapsPage"
        when: windowShown

        function test_1_empty() {
            lib.maps = []
            verify(texts("No maps yet").length > 0, "the empty state")
            var b = buttons("Save my map")
            verify(b.length === 1)
            mouseClick(b[0])
            compare(store.exportOpened, 1, "it opens the export page")
            verify(buttons("Open a map…").length === 1, "the open button")
        }

        function test_2_cards() {
            lib.maps = root.twoMaps
            verify(texts("No maps yet").length === 0, "no empty state with maps")
            verify(texts("Yerevan lake, south bay").length > 0 && texts("Sevan, Shorzha").length > 0, "a card per map")
            verify(texts("Author not given · All rights reserved").length > 0, "no author: said so, with the licence")
            verify(texts("Ani P. · CC BY 4.0").length > 0, "the author and the licence")
            verify(texts("490 × 410 m · 0.4–20.1 m deep").length > 0, "the size and the depths in metres")
            verify(texts("1.8 × 1.2 km · 2.1–31.4 m deep").length > 0, "kilometres past a kilometre")
        }

        function test_3_card_actions() {
            lib.maps = root.twoMaps
            var sw = cardSwitches()
            compare(sw.length, 2)
            mouseClick(sw[0])
            compare(lib.hidden.length, 1, "the switch hides a map")
            var show = buttons("Show on the map")
            mouseClick(show[1])
            compare(lib.flown[0], "Sevan_a.kmap", "fly to the second card's map")
            var rm = buttons("Remove")
            mouseClick(rm[0])
            compare(lib.removed.length, 0, "the first press only asks")
            var sure = buttons("Remove for good?")
            compare(sure.length, 1)
            mouseClick(sure[0])
            compare(lib.removed.length, 1, "the second press removes")
        }

        function test_4_boat_depth() {
            lib.boatDepthValid = false
            verify(texts("Under the boat by the map").length === 0)
            lib.boatDepth = 7.36; lib.boatDepthMap = "Yerevan lake, south bay"; lib.boatDepthValid = true
            verify(texts("Under the boat by the map: 7.4 m (Yerevan lake, south bay)").length > 0, "the depth under the boat")
        }

        function test_5_layer_switches() {
            mouseClick(row("Side-scan mosaic"))
            compare(lay.mosaicVisible, true, "the mosaic switch")
            mouseClick(row("Depths and isobaths"))
            compare(lay.depthVisible, false, "the depth switch")
            mouseClick(row("Show the maps"))
            compare(lay.visible, false, "the layer switch")
            mouseClick(row("Show the maps"))
            mouseClick(row("Depths and isobaths"))
            mouseClick(row("Side-scan mosaic"))
        }

        function test_7_how_to_open() {
            verify(texts("by dropping it on the window").length > 0, "the desktop's ways to open a map")
            page.isAndroid = true
            verify(texts("tap it in the messenger and choose KoggerApp").length > 0, "on Android: from the messenger")
            verify(texts("by dropping it on the window").length === 0, "no dropping on a phone")
            page.isAndroid = false
        }

        function test_6_picture() {
            if (!"%PICTURE%".length)
                return
            lib.maps = root.twoMaps
            lib.hidden = []
            lib.lastMessage = "Opened: Yerevan lake, south bay"
            lib.boatDepth = 7.36; lib.boatDepthMap = "Yerevan lake, south bay"; lib.boatDepthValid = true
            wait(200)
            grabImage(root).save("%PICTURE%")
        }
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('mymaps_page_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    New-ModuleDir $work
    Copy-Item (Join-Path $repo 'qml\app\MyMapsTab.qml') (Join-Path $work 'MyMapsTab.qml')
    $path = Join-Path $work 'tst_mymaps_page.qml'
    $pic = if ($Picture) { [System.IO.Path]::GetFullPath($Picture).Replace('\', '/') } else { '' }
    $prev = if ($Preview) { 'file:///' + [System.IO.Path]::GetFullPath($Preview).Replace('\', '/') } else { '' }
    Write-Utf8 $path $test.Replace('%PICTURE%', $pic).Replace('%PREVIEW%', $prev)
    $env:PATH = "$QtBin;$env:PATH"
    # The offscreen platform has no fonts of its own: the repo's Arial, as sysgui_check does.
    $fonts = Join-Path $work 'fonts'
    New-Item -ItemType Directory -Force $fonts | Out-Null
    Copy-Item (Join-Path $repo 'resources\fonts\arial.ttf') $fonts
    $env:QT_QPA_FONTDIR = $fonts
    $env:QT_QUICK_CONTROLS_STYLE = 'Basic'   # the app's controls are customised; the native style refuses that
    $results = Join-Path $work 'results.txt'
    $p = Start-Process -FilePath $runner -NoNewWindow -Wait -PassThru `
        -ArgumentList @('-platform', 'offscreen', '-import', "`"$(Join-Path $work 'imports')`"", '-input', "`"$path`"",
                        '-o', "`"$results,txt`"") `
        -RedirectStandardOutput (Join-Path $work 'run.out') -RedirectStandardError (Join-Path $work 'run.err')
    $out = @()
    if (Test-Path $results) { $out += @(Get-Content $results -Encoding UTF8) }
    $out | Where-Object { $_ -match '^(PASS|FAIL|Totals|   Actual|   Expected|   Loc|QWARN)' } | ForEach-Object { Write-Host $_ }
    if ($p.ExitCode -ne 0 -or -not (Test-Path $results)) {
        @(Get-Content (Join-Path $work 'run.err') -Encoding UTF8) | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
        Write-Host 'mymaps_page_check: failed'
        $code = 1
    } else {
        Write-Host 'mymaps_page_check: all passed'
        $code = 0
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
exit $code
