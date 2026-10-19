<#
.SYNOPSIS
  Offline check of pinned settings: a settings widget's row (qml/app/SettingMirrorRow.qml) mirroring tagged
  settings rows (KPinTag, PinRegistry), and the pins of pin mode (KPinOverlay).

.DESCRIPTION
  The real SettingMirrorRow.qml runs offscreen in qmltestrunner over a fake settings page whose rows are tagged
  the way qml/app/AppSettingsPage.qml tags them -- a KIslandRow with a switch that owns its value, a compound row
  (spin box + switch), a RowLayout with a combo bound to a backend, another whose combo acts on activated(), a
  Column with tabs, a slider bound to a backend -- inside a group shaped like a SettingsGroup (_bodyShown, a
  disabled body while closed). Each control's handler counts its calls. Checked:
    found    every mirror finds its row; labels without the trailing colon, the group under them; a key with
             no row says so
    switch   the mirror follows the row; a use of the mirror runs the row's handler once; it still does with the
             group closed (the row disabled)
    spin     the compound row gives both controls; a step of the mirror runs the row's handler once
    combo    a pick on the mirror reaches a row acting on currentIndex and a row acting on activated()
    tabs     a pick on the mirror reaches the row; the mirror follows the row's bound value back
    slider   a move of the mirror reaches the row and leaves the row's binding to its backend in place
    pins     pin mode puts a pin by every tagged row; none by rows of a closed group
    device   a device parameter (DeviceParamCatalog) read from and written to the device of its type; a
             device of a type not connected leaves the row unusable and says so
  -Mutants also runs it against copies of SettingMirrorRow.qml with one fault each (a switch through click(), a
  slider value always written, a combo without activated()) and expects each to fail. -Picture saves the scene.
  Exit code 0 when all pass, 1 when a check fails or a mutant passes, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\pin_mirror_check.ps1 -Mutants
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    [string]$Picture = '',
    [switch]$Mutants
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "pin_mirror_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
    exit 2
}
$utf8 = New-Object System.Text.UTF8Encoding $false
function Read-Text([string]$path) { return [System.IO.File]::ReadAllText($path, $utf8) }
function Write-Utf8([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8) }

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
}

$test = @'
import QtQuick
import QtQuick.Layouts
import QtTest
import kqml_types 1.0

Item {
    id: scene
    width: 900; height: 700

    Rectangle { anchors.fill: parent; color: AppPalette.bg }

    QtObject {
        id: backend
        property bool sw: false
        property int spin: 5
        property int combo: 0
        property int combo2: 0
        property var tabs: 0
        property real slider: 20
        property int swCalls: 0
        property int spinCalls: 0
        property int comboCalls: 0
        property int combo2Calls: 0
        property int tabsCalls: 0
        property int sliderCalls: 0
    }

    // Shaped like a SettingsGroup: a title, a state key, a body disabled while the group is closed.
    Item {
        id: group
        property string stateKey: "test.group"
        property string title: "Test group"
        property bool groupOpen: true
        property bool _bodyShown: groupOpen
        x: 10; y: 10; width: 420; height: body.implicitHeight

        Column {
            id: body
            width: parent.width
            enabled: group._bodyShown
            spacing: 4

            KIslandRow {
                label: "Zeroing:"
                KPinTag { key: "app:test.switch" }
                KSwitch { id: tSwitch; flat: true; onToggled: { backend.swCalls++; backend.sw = checked } }
            }
            KIslandRow {
                label: "FBS"
                KPinTag { key: "app:test.compound" }
                Row {
                    spacing: 4
                    KSpinBox { id: tSpin; from: 0; to: 100; value: 5; onValueModified: function(v) { backend.spinCalls++; backend.spin = v } }
                    KSwitch { id: tSwitch2; flat: true }
                }
            }
            RowLayout {
                width: parent.width
                KPinTag { key: "app:test.combo"; label: "Theme:" }
                Text { text: "Theme:"; Layout.fillWidth: true }
                KCombo {
                    id: tCombo
                    Layout.preferredWidth: 150
                    model: ["A", "B", "C"]
                    currentIndex: backend.combo
                    onCurrentIndexChanged: if (backend.combo !== currentIndex) { backend.comboCalls++; backend.combo = currentIndex }
                }
            }
            RowLayout {
                width: parent.width
                KPinTag { key: "app:test.combo2"; label: "Source:" }
                Text { text: "Source:"; Layout.fillWidth: true }
                KCombo {
                    id: tCombo2
                    Layout.preferredWidth: 150
                    model: ["X", "Y", "Z"]
                    onActivated: function(index) { backend.combo2Calls++; backend.combo2 = index }
                }
            }
            Column {
                width: parent.width
                KPinTag { key: "app:test.tabs"; label: "Side:" }
                Text { text: "Side:" }
                KTabBar {
                    id: tTabs
                    width: 300
                    options: [{ label: "Left", value: 0 }, { label: "Right", value: 1 }]
                    currentValue: backend.tabs
                    onValueSelected: function(v) { backend.tabsCalls++; backend.tabs = v }
                }
            }
            KIslandRow {
                label: "Zoom"
                KPinTag { key: "app:test.slider" }
                KSlider {
                    id: tSlider
                    width: 200
                    from: 0; to: 100; stepSize: 1
                    value: backend.slider
                    onValueModified: function(v) { backend.sliderCalls++; backend.slider = v }
                }
            }
        }
    }

    KPinOverlay {
        id: overlay
        x: 0; y: 0; width: 440; height: 700
        root: scene
        pinX: 0
    }

    // A connected device, as DeviceParamCatalog reads one: "Horizontal smoothing" (spin), "Supply voltage" (a mask switch).
    QtObject {
        id: mockDev
        property string devName: "NanoSSS"
        property bool isBoardInited: true
        property bool isDSPSupport: true
        property bool isDatasetSupport: true
        property int dspHorSmooth: 2
        property int datasetVoltage: 0
    }
    QtObject { id: store; function settingsDeviceFor(n) { return n === "NanoSSS" ? mockDev : null } }

    Column {
        x: 460; y: 10; width: 420
        spacing: 6
        SettingMirrorRow { id: mSwitch;   store: store; item: ({ key: "app:test.switch",   label: "" });     availableWidth: 420 }
        SettingMirrorRow { id: mCompound; store: store; item: ({ key: "app:test.compound", label: "" });     availableWidth: 420 }
        SettingMirrorRow { id: mCombo;    store: store; item: ({ key: "app:test.combo",    label: "" });     availableWidth: 420 }
        SettingMirrorRow { id: mCombo2;   store: store; item: ({ key: "app:test.combo2",   label: "" });     availableWidth: 420 }
        SettingMirrorRow { id: mTabs;     store: store; item: ({ key: "app:test.tabs",     label: "" });     availableWidth: 420 }
        SettingMirrorRow { id: mSlider;   store: store; item: ({ key: "app:test.slider",   label: "" });     availableWidth: 420 }
        SettingMirrorRow { id: mGone;     store: store; item: ({ key: "app:test.gone",     label: "Gone" }); availableWidth: 420 }
        SettingMirrorRow { id: mDevSpin;   store: store; item: ({ key: "dev:NanoSSS:dsp.horSmooth",   label: "" }); availableWidth: 420 }
        SettingMirrorRow { id: mDevSwitch; store: store; item: ({ key: "dev:NanoSSS:dataset.voltage", label: "" }); availableWidth: 420 }
        SettingMirrorRow { id: mDevGone;   store: store; item: ({ key: "dev:Basic2D:dsp.horSmooth",   label: "" }); availableWidth: 420 }
    }

    TestCase {
        name: "PinMirror"
        when: windowShown

        function mirrors(row) { return PinRegistry.controlsOf(row) }

        function test_1_found() {
            compare(mSwitch.labelText, "Zeroing")
            compare(mSwitch.note, "Test group")
            compare(mCompound.labelText, "FBS")
            compare(mCombo.labelText, "Theme")
            compare(mTabs.labelText, "Side")
            compare(mSlider.labelText, "Zoom")
            verify(mGone.tag === null, "no row for a key no row has")
            compare(mGone.labelText, "Gone")
            compare(mGone.note, "not available")
            compare(mirrors(mCompound).length, 2)
        }

        function test_2_switch() {
            var m = mirrors(mSwitch)[0]
            compare(m.controlKind, "switch")
            compare(m.checked, false)
            tSwitch.toggle()                       // the row changed elsewhere
            compare(m.checked, true, "the mirror follows the row")
            compare(backend.swCalls, 0, "and runs nothing")
            m.click()                              // a hand on the mirror
            compare(tSwitch.checked, false)
            compare(backend.swCalls, 1, "the row's handler once")
            compare(backend.sw, false)
            group.groupOpen = false                // the group closed: the row is disabled
            verify(!tSwitch.enabled)
            m.click()
            compare(tSwitch.checked, true, "a closed group's row still changes")
            compare(backend.swCalls, 2)
            group.groupOpen = true
        }

        function test_3_spin() {
            var m = mirrors(mCompound)[0]
            compare(m.controlKind, "spin")
            compare(m.value, 5)
            m.increment()
            compare(tSpin.value, 6)
            compare(backend.spinCalls, 1)
            compare(backend.spin, 6)
            tSpin.value = 40
            compare(m.value, 40, "the mirror follows the row")
            compare(backend.spinCalls, 1, "and runs nothing")
        }

        function test_4_combo() {
            var m = mirrors(mCombo)[0]
            compare(m.currentIndex, 0)
            m.currentIndex = 2                     // what KCombo does on a pick, then activated()
            m.activated(2)
            compare(tCombo.currentIndex, 2)
            compare(backend.comboCalls, 1)
            compare(backend.combo, 2)

            var m2 = mirrors(mCombo2)[0]
            m2.currentIndex = 1
            m2.activated(1)
            compare(backend.combo2Calls, 1, "a row acting on activated()")
            compare(backend.combo2, 1)
            compare(tCombo2.currentIndex, 1)
        }

        function test_5_tabs() {
            var m = mirrors(mTabs)[0]
            compare(m.currentValue, 0)
            m.valueSelected(1)                     // what KTabBar does on a pick
            compare(backend.tabsCalls, 1)
            compare(backend.tabs, 1)
            compare(tTabs.currentValue, 1)
            compare(m.currentValue, 1, "the mirror follows the row's bound value")
        }

        function test_6_slider_keeps_binding() {
            var m = mirrors(mSlider)[0]
            compare(m.value, 20)
            m.value = 30                           // what a move does, then valueModified (KSlider.onMoved)
            m.valueModified(30)
            compare(backend.sliderCalls, 1)
            compare(tSlider.value, 30)
            backend.slider = 70                    // the backend moves on its own
            compare(tSlider.value, 70, "the row's binding to its backend is still there")
            compare(m.value, 70)
        }

        function test_7_pins() {
            PinRegistry.pinMode = true
            var n = 0
            for (var i = 0; i < PinRegistry.tags.length; ++i)
                if (overlay.spotOf(PinRegistry.tags[i]) !== null) ++n
            compare(n, 6, "a pin by every tagged row")
            group.groupOpen = false
            n = 0
            for (var j = 0; j < PinRegistry.tags.length; ++j)
                if (overlay.spotOf(PinRegistry.tags[j]) !== null) ++n
            compare(n, 0, "none in a closed group")
            group.groupOpen = true
        }

        function test_8_device() {
            compare(mDevSpin.labelText, "Horizontal smoothing")
            compare(mDevSpin.note, "NanoSSS")
            var sp = mirrors(mDevSpin)[0]
            compare(sp.value, 2)
            sp.increment()
            compare(mockDev.dspHorSmooth, 3, "a step writes the device's parameter")
            mockDev.dspHorSmooth = 6
            compare(sp.value, 6, "the mirror follows the device")

            var sw = mirrors(mDevSwitch)[0]
            compare(sw.checked, false)
            sw.click()
            compare(mockDev.datasetVoltage, 1, "the switch writes its mask")
            mockDev.datasetVoltage = 0
            compare(sw.checked, false, "the mirror follows the device")

            compare(mDevGone.note, "Basic2D is not connected")
            verify(!mirrors(mDevGone)[0].enabled, "no device, no use")
        }

        function test_9_picture() {
            if (!"%PICTURE%".length)
                return
            PinRegistry.pinMode = true
            wait(300)
            grabImage(scene).save("%PICTURE%")
        }
    }
}
'@

function Invoke-Check([string]$mirrorText, [string]$pic) {
    $work = Join-Path ([System.IO.Path]::GetTempPath()) ('pin_mirror_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force $work | Out-Null
    try {
        New-ModuleDir $work
        Write-Utf8 (Join-Path $work 'SettingMirrorRow.qml') $mirrorText
        $path = Join-Path $work 'tst_pin_mirror.qml'
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
        $err = @(Get-Content (Join-Path $work 'run.err') -Encoding UTF8)
        return [pscustomobject]@{ Ok = ($p.ExitCode -eq 0 -and (Test-Path $results)); Lines = $out; Err = $err }
    }
    finally {
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    }
}

$mirror = Read-Text (Join-Path $repo 'qml\app\SettingMirrorRow.qml')
$pic = if ($Picture) { [System.IO.Path]::GetFullPath($Picture).Replace('\', '/') } else { '' }
$r = Invoke-Check $mirror $pic
$r.Lines | Where-Object { $_ -match '^(PASS|FAIL|Totals|   Actual|   Expected|   Loc)' } | ForEach-Object { Write-Host $_ }
if (-not $r.Ok) {
    $r.Err | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
    Write-Host 'pin_mirror_check: failed'
    exit 1
}
Write-Host 'pin_mirror_check: all passed'
if (-not $Mutants) { exit 0 }

# One fault each, in a copy of SettingMirrorRow.qml. The text to replace must be there: a mutant that changes
# nothing would pass and look like a gap in the check.
$faults = @(
    @{ Name = 'a switch through click()'; From = "                t.toggle()`n                t.toggled()"; To = '                t.click()' },
    @{ Name = 'a slider value always written'; From = "            if (t.value !== v)`n                t.value = v"; To = '            t.value = v' },
    @{ Name = 'a combo without activated()'; From = "                t.activated(index)`n"; To = "`n" },
    @{ Name = 'a spin stepped without the row'; From = "            if (!_in && t)`n                t.applyValue(v)"; To = '            if (!_in && t) {}' }
)
$norm = $mirror.Replace("`r`n", "`n")
$missed = 0
foreach ($m in $faults) {
    if (-not $norm.Contains($m.From)) { throw "mutant '$($m.Name)': its text is not in SettingMirrorRow.qml" }
    $rm = Invoke-Check ($norm.Replace($m.From, $m.To)) ''
    $failed = @($rm.Lines | Where-Object { $_ -like 'FAIL*' })
    if (-not $rm.Ok -and $failed.Count -gt 0) {
        Write-Host ("mutant ({0}): caught, first: {1}" -f $m.Name, $failed[0])
    } else {
        Write-Host ("mutant ({0}): MISSED" -f $m.Name)
        ++$missed
    }
}
Write-Host ("pin_mirror_check mutants: {0} of {1} caught" -f ($faults.Count - $missed), $faults.Count)
exit ([int]($missed -gt 0))
