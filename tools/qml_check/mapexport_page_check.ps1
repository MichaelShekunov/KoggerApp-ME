<#
.SYNOPSIS
  Offline check of the "Map for sharing" page (qml/app/MapExportSettingsTab.qml, docs/map-export.md).

.DESCRIPTION
  Runs the real page over the kqml_types sources with a mock exporter (the C++ MapExporter's properties and
  calls) and a mock core:
    1. every setting has its row;
    2. "Save the map" starts the export into the Export group's folder, or the default one when it is empty;
    3. while it runs: the button cancels, the stage and the percentage show;
    4. after it: the result shows; "Show in folder" opens the first file; after a failure it is not offered;
       "Share the map" (Android) hands the .kmap to the share sheet, only when one was saved;
    5. the licence note follows the licence, and the "not for navigation" notice is always there;
    6. the switches and the lists write the exporter's settings;
    7. "In the file": the isobaths, then the mosaic; with neither, nothing can be saved and the page says so;
       the isobaths go as the app draws them, so there is no edge limit or step of the export's own.
  -Picture saves the page. Exit code 0 when all pass, 1 when a check fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\mapexport_page_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin] [-Picture page.png]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    [string]$Picture = ''
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "mapexport_page_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
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
    width: 440; height: 1400
    color: AppPalette.bg

    QtObject {
        id: store
        property string exportFolderSource: "C:/maps"
    }
    QtObject {
        id: coreMock
        property string revealed: ""
        property var shared: []
        function defaultExportDirectory() { return "C:/Documents/KoggerApp/exports" }
        function revealInFolder(p) { revealed = p }
        function shareFile(p, title) { shared = [p, title]; return true }
    }
    QtObject {
        id: ex
        property string title: "Lake"
        property string authorName: ""
        property string authorContact: ""
        property string licenseId: "all-rights-reserved"
        property var licenseOptions: [
            { id: "all-rights-reserved", name: "All rights reserved", url: "" },
            { id: "CC-BY-4.0", name: "CC BY 4.0", url: "https://creativecommons.org/licenses/by/4.0/" },
            { id: "CC0-1.0", name: "CC0", url: "https://creativecommons.org/publicdomain/zero/1.0/" } ]
        property int detailZoom: 5
        property var detailOptions: [ { zoom: 2, name: "2 cm" }, { zoom: 3, name: "4 cm" }, { zoom: 4, name: "8 cm" },
                                      { zoom: 5, name: "16 cm" }, { zoom: 6, name: "32 cm" } ]
        property double draftM: 0.0
        property bool writeKmap: true
        property bool writeKmz: false
        property bool writeGeoTiff: false
        property bool includeMosaic: true
        property bool includeDepth: true
        property bool hideTrackAndTime: true
        property bool busy: false
        property double progress: 0
        property string stage: ""
        property bool resultOk: false
        property string resultText: ""
        property var resultFiles: []
        property var started: []
        property int cancels: 0
        function start(folder) { started = started.concat([folder]) }
        function cancel() { cancels += 1 }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: page.height + 20
        MapExportSettingsTab {
            id: page
            x: 10; y: 10
            width: root.width - 20
            store: store
            exporter: ex
            coreApi: coreMock
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
    function textItem(s) {
        var r = all(page, function(c) { return c.hasOwnProperty("text") && typeof c.text === "string" && c.text === s && c.hasOwnProperty("wrapMode") })
        return r.length ? r[0] : null
    }
    function textContaining(s) {
        return all(page, function(c) { return c.hasOwnProperty("text") && typeof c.text === "string" && c.text.indexOf(s) >= 0 && c.hasOwnProperty("wrapMode") && shown(c) })
    }
    function button(s) {
        var r = all(page, function(c) { return c.hasOwnProperty("normalBg") && c.text === s })
        return r.length ? r[0] : null
    }
    function row(label) {
        var r = all(page, function(c) { return c.hasOwnProperty("chevron") && c.label === label })
        return r.length ? r[0] : null
    }
    function inRow(label, pred) {
        var r = all(row(label), pred)
        return r.length ? r[0] : null
    }

    TestCase {
        name: "MapExportPage"
        when: windowShown

        function test_1_rows() {
            var labels = ["Title", "Author", "Contact", "Licence", "Side-scan mosaic", "Mosaic detail", "Isobaths",
                          "Transducer depth, m",
                          "KOGGER map (.kmap)", "Google Earth (KMZ)", "GIS (GeoTIFF)", "Without the track and times"]
            for (var i = 0; i < labels.length; ++i)
                verify(row(labels[i]) !== null, "a row for " + labels[i])
        }

        function test_2_save_into_the_folder() {
            ex.started = []
            var b = button("Save the map")
            verify(b !== null && shown(b))
            mouseClick(b)
            compare(ex.started.length, 1)
            compare(ex.started[0], "C:/maps", "the Export group's folder")
            store.exportFolderSource = ""
            mouseClick(b)
            compare(ex.started[1], "C:/Documents/KoggerApp/exports", "the default folder when none is set")
            store.exportFolderSource = "C:/maps"
        }

        function test_3_while_running() {
            ex.busy = true; ex.progress = 0.4; ex.stage = "Painting the mosaic"
            var b = button("Cancel")
            verify(b !== null && shown(b), "the button cancels while it runs")
            verify(textContaining("Painting the mosaic").length > 0 && textContaining("40 %").length > 0, "stage and percentage")
            mouseClick(b)
            compare(ex.cancels, 1)
            ex.busy = false
        }

        function test_4_result() {
            ex.resultOk = true; ex.resultText = "Saved to C:/maps: lake.kmz."; ex.resultFiles = ["C:/maps/lake.kmz"]
            verify(textContaining("Saved to C:/maps").length > 0, "the result shows")
            var show = button("Show in folder")
            verify(show !== null && shown(show))
            mouseClick(show)
            compare(coreMock.revealed, "C:/maps/lake.kmz")
            ex.resultOk = false; ex.resultText = "Not saved: no survey is open."; ex.resultFiles = []
            verify(textContaining("Not saved").length > 0, "a failure shows")
            verify(!shown(button("Show in folder")), "nothing to show after a failure")
        }

        function test_4b_share() {
            verify(!page.canShare, "no Share on the desktop")
            ex.resultOk = true; ex.resultText = "Saved."; ex.resultFiles = ["C:/maps/lake.kmz", "C:/maps/lake.kmap"]
            verify(!shown(button("Share the map")), "not offered off Android")
            page.canShare = true
            var share = button("Share the map")
            verify(share !== null && shown(share), "offered on Android once a .kmap is saved")
            mouseClick(share)
            compare(coreMock.shared[0], "C:/maps/lake.kmap", "the .kmap goes out, not the KMZ")
            compare(coreMock.shared[1], "Send the map", "the share sheet's title")
            ex.resultFiles = ["C:/maps/lake.kmz"]
            verify(!shown(button("Share the map")), "nothing to share without a .kmap")
            ex.resultFiles = ["C:/maps/lake.kmap"]; ex.busy = true
            verify(!shown(button("Share the map")), "not while an export runs")
            ex.busy = false; ex.resultOk = false; ex.resultText = "Not saved: no survey is open."
            verify(!shown(button("Share the map")), "not after a failure")
            ex.resultFiles = []
            page.canShare = false
        }

        function test_5_licence_note() {
            ex.licenseId = "all-rights-reserved"
            verify(textContaining("needs your permission").length > 0)
            ex.licenseId = "CC-BY-4.0"
            verify(textContaining("if they name you as the author").length > 0)
            ex.licenseId = "CC0-1.0"
            verify(textContaining("without naming you").length > 0)
            verify(textContaining("NOT FOR NAVIGATION").length > 0, "the notice, whatever the licence")
            ex.licenseId = "all-rights-reserved"
        }

        function test_6_settings_written() {
            mouseClick(row("Without the track and times"))
            compare(ex.hideTrackAndTime, false, "the privacy switch")
            mouseClick(row("Without the track and times"))
            compare(ex.hideTrackAndTime, true)
            mouseClick(row("KOGGER map (.kmap)"))
            compare(ex.writeKmap, false, "the .kmap switch")
            mouseClick(row("KOGGER map (.kmap)"))
            mouseClick(row("Google Earth (KMZ)"))
            compare(ex.writeKmz, true, "KMZ for other programs")
            mouseClick(row("Google Earth (KMZ)"))
            mouseClick(row("Isobaths"))
            compare(ex.includeDepth, false)
            verify(!shown(row("Transducer depth, m")) || row("Transducer depth, m").open === false, "the depth rows fold away")
            mouseClick(row("Isobaths"))
            var combo = inRow("Licence", function(c) { return c.hasOwnProperty("swatchFor") })
            verify(combo !== null)
            combo.activated(1)
            compare(ex.licenseId, "CC-BY-4.0", "the licence list")
            compare(combo.currentIndex, 1)
            var detail = inRow("Mosaic detail", function(c) { return c.hasOwnProperty("swatchFor") })
            detail.activated(2)
            compare(ex.detailZoom, 4, "the detail list")
            // The isobaths go as the view draws them (owner, 2026-10-18): no edge limit or step of the export's own.
            compare(row("Join soundings up to, m"), null, "no edge limit of the export's own")
            compare(row("Isobaths every"), null, "no isobath step of the export's own")
            ex.licenseId = "all-rights-reserved"
        }

        function test_6b_in_the_file() {
            verify(textItem("In the file") !== null || all(page, function(c) { return c.text === "In the file" }).length > 0,
                   "the heading says what goes into the file")
            verify(row("Isobaths").mapToItem(page, 0, 0).y < row("Side-scan mosaic").mapToItem(page, 0, 0).y, "the isobaths first, then the mosaic")
            var b = button("Save the map")
            ex.includeDepth = true; ex.includeMosaic = true
            verify(b.enabled, "both: Save")
            ex.includeDepth = false
            verify(b.enabled, "the mosaic alone: Save")
            ex.includeDepth = true; ex.includeMosaic = false
            verify(b.enabled, "the isobaths alone: Save")
            compare(textContaining("there is nothing to save").length, 0, "no warning while one is chosen")
            ex.includeDepth = false
            verify(!b.enabled, "neither: no Save")
            compare(textContaining("there is nothing to save").length, 1, "neither: the page says so")
            ex.includeDepth = true; ex.includeMosaic = true
        }

        function test_7_picture() {
            if (!"%PICTURE%".length)
                return
            ex.resultOk = true; ex.resultText = "Saved to C:/maps: Lake_20261018-124424.kmz, Lake_20261018-124424_mosaic.tif, Lake_20261018-124424_depth.tif."
            ex.resultFiles = ["C:/maps/Lake_20261018-124424.kmz"]
            wait(100)
            grabImage(root).save("%PICTURE%")
        }
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('mapexport_page_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    New-ModuleDir $work
    Copy-Item (Join-Path $repo 'qml\app\MapExportSettingsTab.qml') (Join-Path $work 'MapExportSettingsTab.qml')
    $path = Join-Path $work 'tst_mapexport_page.qml'
    $pic = if ($Picture) { [System.IO.Path]::GetFullPath($Picture).Replace('\', '/') } else { '' }
    Write-Utf8 $path $test.Replace('%PICTURE%', $pic)
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
        Write-Host 'mapexport_page_check: failed'
        $code = 1
    } else {
        Write-Host 'mapexport_page_check: all passed'
        $code = 0
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
exit $code
