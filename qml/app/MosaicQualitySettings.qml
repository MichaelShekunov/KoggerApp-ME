import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtCore
import kqml_types 1.0

// Mosaic quality (docs/mosaic-quality.md): the brightness the beam and the range put into the echogram,
// measured from the data and taken out; the crop by the angle below the sonar; which line a pixel shows
// where lines overlap. A change repaints the mosaic. A part of the Mosaic group of AppSettingsPage, apart
// so that tools/qml_check/mosaicquality_check.ps1 can run it over a mock controller.
ColumnLayout {
    id: root

    // MosaicViewControlMenuController in the app: setQuality(correction, nadirCropDeg, farCropDeg, overlap)
    // and beamState (DataProcessor::mosaicBeamState).
    property var controller: null
    property int ctrlW: Math.round(170 * AppPalette.scale)
    readonly property var beam: controller ? controller.beamState : null

    spacing: Tokens.spaceMd

    function send() {
        if (controller) {
            controller.setQuality(correction.checked, nadirCrop.value, farCrop.value, overlap.currentIndex)
        }
    }
    // After every control has its saved value (Settings restore before onCompleted runs).
    Component.onCompleted: Qt.callLater(send)

    KSwitch {
        id: correction
        objectName: "mosaicBeamCorrection"
        text: qsTr("Even out brightness")
        toolTipText: qsTr("Measures from the echograms how the brightness depends on the angle below the sonar (its beam) and on the range, and takes both out: the bright band along the track and the dark far edge even out, and neighbouring lines match")
        checked: true
        Layout.fillWidth: true
        onToggled: root.send()
        Settings { category: "scene3d/mosaic"; property alias mosaicBeamCorrection: correction.checked }
    }

    RowLayout {
        spacing: Tokens.spaceMd
        Text {
            text: qsTr("Crop near the track, °:")
            color: AppPalette.textStrong
            font.pixelSize: Tokens.fontLg
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        KSpinBox {
            id: nadirCrop
        objectName: "mosaicNadirCrop"
            toolTipText: qsTr("Samples nearer straight down than this angle are not painted: the stretched, glaring strip under the sonar. A neighbouring line fills it where lines overlap. 0: off")
            Layout.preferredWidth: root.ctrlW
            from: 0; to: 30; stepSize: 1; value: 0
            onValueModified: function(v) { root.send() }
            Settings { category: "scene3d/mosaic"; property alias mosaicNadirCrop: nadirCrop.value }
        }
    }

    RowLayout {
        spacing: Tokens.spaceMd
        Text {
            text: qsTr("Crop the far edge, °:")
            color: AppPalette.textStrong
            font.pixelSize: Tokens.fontLg
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        KSpinBox {
            id: farCrop
        objectName: "mosaicFarCrop"
            toolTipText: qsTr("Samples further out than this angle below the sonar are not painted: the edge of the beam, where there is mostly noise. The chart below shows where the beam falls off. 90: off")
            Layout.preferredWidth: root.ctrlW
            from: 60; to: 90; stepSize: 1; value: 90
            onValueModified: function(v) { root.send() }
            Settings { category: "scene3d/mosaic"; property alias mosaicFarCrop: farCrop.value }
        }
    }

    RowLayout {
        spacing: Tokens.spaceMd
        Text {
            text: qsTr("Overlaps:")
            color: AppPalette.textStrong
            font.pixelSize: Tokens.fontLg
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        KCombo {
            id: overlap
        objectName: "mosaicOverlap"
            Layout.preferredWidth: root.ctrlW
            model: [qsTr("Newest on top"), qsTr("Newest, edges under"), qsTr("Nearest the track")]
            currentIndex: 1
            toolTipText: qsTr("Where lines overlap: the newest line; the newest, but its strip under the sonar and its beam's edge only where no other line has better; or the line that saw the place nearest straight down")
            onActivated: function(index) { root.send() }
            Settings { category: "scene3d/mosaic"; property alias mosaicOverlap: overlap.currentIndex }
        }
    }

    // The measured angle part: dB by the angle below the sonar, left and right, with the crops.
    Canvas {
        id: chart
        objectName: "mosaicBeamChart"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.round(110 * AppPalette.scale)
        visible: correction.checked && !!root.beam && !!root.beam.valid
        readonly property real padX: 4
        readonly property real padTop: 4
        readonly property real dbMin: -20
        readonly property real dbMax: 2
        function xOf(a) { return padX + (width - 2 * padX) * a / 90 }
        function yOf(db) { return padTop + (height - 14 - padTop) * (dbMax - Math.max(dbMin, Math.min(dbMax, db))) / (dbMax - dbMin) }
        onWidthChanged: requestPaint()
        onVisibleChanged: requestPaint()
        Connections {
            target: root
            function onBeamChanged() { chart.requestPaint() }
        }
        Connections {
            target: nadirCrop
            function onValueChanged() { chart.requestPaint() }
        }
        Connections {
            target: farCrop
            function onValueChanged() { chart.requestPaint() }
        }
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const st = root.beam
            const w = width - 2 * padX, bottom = height - 14
            ctx.font = Math.round(9 * AppPalette.scale) + "px sans-serif"
            ctx.fillStyle = AppPalette.textMuted
            ctx.strokeStyle = AppPalette.separator
            ctx.lineWidth = 1
            for (const db of [0, -6, -12, -18]) {
                ctx.beginPath(); ctx.moveTo(padX, yOf(db)); ctx.lineTo(padX + w, yOf(db)); ctx.stroke()
                ctx.fillText(db + " dB", padX + 2, yOf(db) - 2)
            }
            for (const a of [0, 30, 60, 90]) {
                ctx.fillText(a + "°", Math.min(xOf(a), padX + w - 18), height - 2)
            }
            // crops
            ctx.strokeStyle = AppPalette.textMuted
            ctx.setLineDash([3, 3])
            for (const a of [nadirCrop.value, farCrop.value]) {
                if (a > 0 && a < 90) {
                    ctx.beginPath(); ctx.moveTo(xOf(a), padTop); ctx.lineTo(xOf(a), bottom); ctx.stroke()
                }
            }
            ctx.setLineDash([])
            const curves = [[st && st.left ? st.left.curve : null, AppPalette.accent, qsTr("Left")],
                            [st && st.right ? st.right.curve : null, AppPalette.linkIdleBorder, qsTr("Right")]]
            // the legend, padTop right
            ctx.textAlign = "right"
            let lx = padX + w - 2
            for (let n = curves.length - 1; n >= 0; --n) {
                if (!curves[n][0]) continue
                ctx.fillStyle = curves[n][1]
                ctx.fillText(curves[n][2], lx, padTop + 10)
                lx -= ctx.measureText(curves[n][2]).width + 10
            }
            ctx.textAlign = "left"
            ctx.lineWidth = 2
            for (const c of curves) {
                if (!c[0]) continue
                ctx.strokeStyle = c[1]
                ctx.beginPath()
                let pen = false
                for (let k = 0; k < c[0].length; ++k) {
                    const v = c[0][k]
                    if (v === undefined || v === null) { pen = false; continue }
                    const x = xOf(k + 0.5), y = yOf(v)
                    if (pen) ctx.lineTo(x, y); else ctx.moveTo(x, y)
                    pen = true
                }
                ctx.stroke()
            }
        }
    }

    Text {
        id: status
        objectName: "mosaicBeamStatus"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: AppPalette.textMuted
        font.pixelSize: Tokens.fontSm
        visible: correction.checked
        text: {
            const st = root.beam
            if (!st || st.correction === undefined) {
                return ""
            }
            if (st.holding) {
                return qsTr("Measuring the beam from the echograms…")
            }
            if (!st.valid) {
                return qsTr("Not measured yet: needs %1 pings with a bottom track, seen out to %2° from straight down")
                    .arg(st.minPings).arg(st.minCover)
            }
            const side = function(name, s) {
                if (!s || s.peak === undefined) {
                    return qsTr("%1: not measured").arg(name)
                }
                if (s.edge6 < 0) {   // the data ends before the beam falls off
                    return qsTr("%1: brightest at %2°, no 6 dB fall up to %3°").arg(name)
                        .arg(s.peak.toFixed(0)).arg(s.dataTo)
                }
                return qsTr("%1: brightest at %2°, down 6 dB at %3°").arg(name)
                    .arg(s.peak.toFixed(0)).arg(s.edge6.toFixed(0))
            }
            return qsTr("Measured on %1 pings. %2; %3. Near to far range: %4 dB evened out")
                .arg(st.epochs)
                .arg(side(qsTr("Left"), st.left)).arg(side(qsTr("Right"), st.right))
                .arg(Math.max(st.left && st.left.rangeSpan ? st.left.rangeSpan : 0,
                              st.right && st.right.rangeSpan ? st.right.rangeSpan : 0).toFixed(0))
        }
    }
}
