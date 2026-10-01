import QtQuick 2.15
import kqml_types 1.0

// The "System" widget cell (owner 2026-10-01): the computer the app runs on at a glance. `sys` is the SystemMonitor
// (context property systemMonitor) or a sample with its names (DataFieldCatalog.systemSample, the palette).
//   CPU 40 %  2.40 GHz        ▼ (52°)    the mean load, the fastest core; ▼ held below its maximum; the hottest zone
//   ▂▃▂▅ ▇▆▅▇                           each core: height its frequency of its maximum, colour its load
//   GPU  (48°)        12 % · 300 MHz     the GPU: the bar its load
//   ━━━━━──────────────────────────────
//   RAM                  3.1 / 15.6 GB
//   ━━━━━━─────────────────────────────
//   /  (eMMC ≤10 %)       8.7 / 11.7 GB  a volume; its disk's kind on the disk's first volume, coloured by its wear
//   ━━━━━━━━━━━━━━━━━━━━━━━━━━─────────
//                         up 3 h 12 min
// A wide cell puts the CPU and the GPU on the left, the rest on the right spread over the height. A cell too small
// for the CPU, the GPU, memory and the system volume under the cores (a 1 x 1, owner 2026-10-01: "CPU and GPU on one
// widget, RAM and eMMC with numbers") drops the header and the cores for a CPU line, and shows first the volume this
// app keeps its data on (the maps, its log; owner: "the partition where the data is saved"):
//   CPU (52°)  40%
//   GPU (48°)  12%
//   RAM   3.1/15.6
//   /data 0.1/16.5
// What does not fit is left out from the bottom up, and each line shows the fullest of its texts that fits.
Item {
    id: ov

    property var sys: null
    property real k: 1.0

    readonly property bool _ok: !!sys && sys.available === true

    // a small cell (1 x 1, 2 x 1) keeps less margin: the panel has its own around it
    readonly property real _pad: Math.round((Math.min(width, height) < 110 * k ? 3 : 6) * k)
    readonly property real _gapX: Math.round(10 * k)
    readonly property real _gapY: Math.max(2, Math.round(3 * k))
    readonly property real _innerW: Math.max(0, width - 2 * _pad)
    readonly property real _innerH: Math.max(0, height - 2 * _pad)
    readonly property bool _wide: _innerW >= _innerH * 1.7
    readonly property real _colW: _wide ? Math.floor((_innerW - _gapX) / 2) : _innerW
    // type follows the smaller side of a column, and stays readable in the smallest cell
    readonly property real _s: Math.min(_innerH, _colW)
    readonly property int _labelPx: Math.max(Math.round(9 * k), Math.round(_s * 0.085))
    readonly property int _valuePx: Math.max(Math.round(12 * k), Math.round(_s * 0.115))
    readonly property real _headH: Math.round(_valuePx * 1.3)
    readonly property real _lineH: Math.round(_labelPx * 1.3)
    readonly property real _barH: Math.max(3, Math.round(_labelPx * 0.36))
    readonly property real _rowH: _lineH + _barH
    readonly property color _track: Qt.rgba(AppPalette.textSecond.r, AppPalette.textSecond.g, AppPalette.textSecond.b, 0.18)

    function wearColor(w) {
        return w < 0 ? AppPalette.textMuted : AppPalette.systemLevelColors[w <= 70 ? 0 : w <= 90 ? 1 : 2]
    }
    function pct(f) { return Math.round(f * 100) + " %" }
    function pctTight(f) { return Math.round(f * 100) + "%" }
    // "8.7 / 11.7 GB", "8.7/11.7 GB", "8.7/11.7"
    function amounts(used, total) {
        var u = used.toFixed(1), t = total.toFixed(total >= 100 ? 0 : 1)
        return [DataFieldCatalog.gbPair(used, total), u + "/" + t + " " + qsTr("GB"), u + "/" + t]
    }
    // A line's texts, the fullest first: each value with the fullest chip, then the shortest value with each shorter
    // chip, then that chip's text without its frame (bare: coloured text, no margins), then without one.
    function lineTexts(label, chips, values) {
        var out = [], last = values[values.length - 1]
        var chip = chips.length ? chips[0] : ""
        for (var i = 0; i < values.length; ++i)
            out.push({ label: label, pill: chip, value: values[i] })
        for (var j = 1; j < chips.length; ++j)
            out.push({ label: label, pill: chips[j], value: last })
        if (chip.length) {
            out.push({ label: label, pill: chips[chips.length - 1], value: last, bare: true })
            out.push({ label: label, pill: "", value: last })
        }
        return out
    }

    readonly property var _cores: _ok && sys.coreLoad ? sys.coreLoad : []
    readonly property string _loadText: _ok && sys.cpuLoad >= 0 ? pct(sys.cpuLoad) : "—"
    readonly property string _ghzText: _ok && sys.cpuFreqMHz > 0 ? (sys.cpuFreqMHz / 1000).toFixed(2) + " " + qsTr("GHz") : ""
    readonly property bool _hasTemp: _ok && isFinite(sys.cpuTempC)

    // A line over a bar: { key, cands: [{ label, pill, value }] the fullest first, pillColor, pillTextColor, frac,
    // fillColor }.
    readonly property var _cpuRow: {
        if (!_ok) return null
        var load = sys.cpuLoad
        var t = isFinite(sys.cpuTempC)
        var held = sys.throttled === true
        var temp = t ? Math.round(sys.cpuTempC) + "°" : ""
        // held back: the mark in the chip, given up before the temperature
        var chips = held ? (t ? ["▼" + temp, temp] : ["▼"]) : (t ? [temp] : [])
        var level = t ? DataFieldCatalog.sysTempColor(sys.cpuTempC) : AppPalette.systemLevelColors[2]
        var values = load >= 0 ? (_ghzText ? [pct(load) + " · " + _ghzText] : []).concat([pct(load), pctTight(load)]) : ["—"]
        return { key: "cpu", cands: lineTexts("CPU", chips, values), pillColor: level, pillTextColor: level,
                 frac: Math.max(0, load), fillColor: DataFieldCatalog.sysLoadColor(Math.max(0, load)) }
    }
    readonly property var _gpuRow: {
        if (!_ok) return null
        var load = (typeof sys.gpuLoad === "number") ? sys.gpuLoad : -1
        var f = sys.gpuFreqMHz > 0 ? sys.gpuFreqMHz : 0
        var t = isFinite(sys.gpuTempC)
        if (f <= 0 && load < 0 && !t) return null
        var mhz = f > 0 ? f + " " + qsTr("MHz") : ""
        var values = load >= 0 ? (mhz ? [pct(load) + " · " + mhz] : []).concat([pct(load), pctTight(load)])
                               : [mhz || "—"]
        var chip = t ? Math.round(sys.gpuTempC) + "°" : ""
        var level = t ? DataFieldCatalog.sysTempColor(sys.gpuTempC) : AppPalette.textMuted
        // the bar its load; without one reported, its frequency of the top of its table, uncoloured
        return { key: "gpu", cands: lineTexts("GPU", chip ? [chip] : [], values), pillColor: level, pillTextColor: level,
                 frac: load >= 0 ? load : (f > 0 && sys.gpuMaxMHz > 0 ? Math.min(1, f / sys.gpuMaxMHz) : 0),
                 fillColor: load >= 0 ? DataFieldCatalog.sysLoadColor(load) : AppPalette.systemFill }
    }
    // memory, then the volumes
    readonly property var _rows: {
        if (!_ok) return []
        var out = []
        if (sys.memTotalGB > 0) {
            var m = Math.max(0, Math.min(1, sys.memUsedGB / sys.memTotalGB))
            var ma = amounts(sys.memUsedGB, sys.memTotalGB)
            out.push({ key: "ram", cands: lineTexts(qsTr("RAM"), [], ma.concat([pctTight(m)])),
                       pillColor: AppPalette.textMuted, pillTextColor: AppPalette.textSecond, frac: m,
                       fillColor: DataFieldCatalog.sysFillColor(m) })
        }
        var v = sys.volumes || []
        for (var i = 0; i < v.length; ++i) {
            var name = String(v[i].name)
            var kind = v[i].kind ? String(v[i].kind) : ""
            var w = (typeof v[i].wearPct === "number") ? v[i].wearPct : -1
            var wear = w > 100 ? qsTr("worn out") : w >= 0 ? "≤" + w + " %" : ""
            var fv = v[i].totalGB > 0 ? Math.max(0, Math.min(1, v[i].usedGB / v[i].totalGB)) : 0
            var a = amounts(v[i].usedGB, v[i].totalGB)
            var c = []
            if (kind) {
                // the name and the disk; then the disk alone as the name ("eMMC"); the numbers kept before the disk
                if (wear) c.push({ label: name, pill: kind + " " + wear, value: a[0] })
                c.push({ label: name, pill: kind, value: a[0] }, { label: name, pill: kind, value: a[1] },
                       { label: kind, pill: "", value: a[1] }, { label: kind, pill: "", value: a[2] },
                       { label: name, pill: "", value: a[2] }, { label: kind, pill: "", value: pctTight(fv) })
            } else {
                c = lineTexts(name, [], a.concat([pctTight(fv)]))
            }
            c.push({ label: name, pill: "", value: pctTight(fv) })
            out.push({ key: "vol" + i, data: v[i].data === true, cands: c, pillColor: wearColor(w),
                       pillTextColor: w >= 0 ? wearColor(w) : AppPalette.textSecond, frac: fv,
                       fillColor: DataFieldCatalog.sysFillColor(fv) })
        }
        return out
    }
    // the compact cell's: the volume this app keeps its data on (the maps, its log - /data on the head unit) right
    // after memory
    readonly property var _rowsDataFirst: {
        var r = _rows.slice(), at = -1
        for (var i = 0; i < r.length; ++i)
            if (r[i].data === true) { at = i; break }
        if (at < 0) return r
        var d = r.splice(at, 1)[0]
        r.splice(r.length && r[0].key === "ram" ? 1 : 0, 0, d)
        return r
    }
    readonly property string _upText: _ok && sys.uptimeS >= 0 ? qsTr("up %1").arg(DataFieldCatalog.uptimeText(sys.uptimeS)) : ""

    // What fits. One column: the header, the cores (a fifth of the height), the GPU, the rows, the footer. Wide: the
    // header, the cores and the GPU on the left; the rows and the footer on the right. Compact (the one column cannot
    // show the GPU, memory and the system volume): lines only, the CPU's first.
    readonly property bool _gpuLeft: _wide && _gpuRow !== null
    readonly property real _cores1H: Math.max(4, Math.round(_innerH * 0.2))
    readonly property real _oneRoom: _innerH - _headH - _gapY - _cores1H - _gapY
    readonly property int _essentials: (_gpuRow !== null ? 1 : 0) + (_ok && sys.memTotalGB > 0 ? 1 : 0)
                                       + (_ok && sys.volumes && sys.volumes.length > 0 ? 1 : 0)
    readonly property bool _compact: _ok && !_wide && Math.floor((_oneRoom + _gapY) / (_rowH + _gapY)) < _essentials
    readonly property var _infoRows: _compact ? [_cpuRow].concat(_gpuRow !== null ? [_gpuRow] : [], _rowsDataFirst)
                                   : (_wide || _gpuRow === null) ? _rows : [_gpuRow].concat(_rows)
    readonly property real _infoRoom: (_wide || _compact) ? _innerH : _oneRoom
    readonly property int _rowsShown: {
        var n = 0
        while (n < _infoRows.length && (n + 1) * _rowH + n * _gapY <= _infoRoom)
            ++n
        return n
    }
    readonly property real _rowsH: _rowsShown > 0 ? _rowsShown * _rowH + (_rowsShown - 1) * _gapY : 0
    readonly property bool _showFoot: _ok && _upText.length > 0 && _rowsShown === _infoRows.length
                                      && _infoRoom - _rowsH - (_rowsShown > 0 ? _gapY : 0) >= _lineH
    readonly property real _coresH: _wide ? Math.max(4, _innerH - _headH - _gapY - (_gpuLeft ? _gapY + _rowH : 0)) : _cores1H
    // wide and compact: the lines and the footer spread over the height
    readonly property real _infoSpacing: {
        var parts = _rowsShown + (_showFoot ? 1 : 0)
        if (!(_wide || _compact) || parts < 2) return _gapY
        return Math.max(_gapY, (_innerH - _rowsShown * _rowH - (_showFoot ? _lineH : 0)) / (parts - 1))
    }
    // tighter in the compact lines and a narrow column, so the numbers and the chips stay
    readonly property bool _tight: _compact || _colW < 110 * k
    readonly property real _sp: Math.round((_tight ? 3 : 5) * k)
    readonly property real _pillPad: Math.round((_tight ? 6 : 8) * k)

    // A line over a bar. Its sizes come in as properties: an inline component does not see this file's ids.
    component SysRow: Item {
        id: row
        property var line: null
        property real k: 1.0
        property int labelPx: 9
        property real lineH: 12
        property real barH: 3
        property real sp: 5
        property real pillPad: 8
        property color track: "transparent"

        readonly property bool _has: line !== null && line !== undefined
        readonly property var _cands: _has ? line.cands : []
        readonly property real frac: _has ? line.frac : 0
        height: lineH + barH
        FontMetrics { id: fmLabel; font.pixelSize: row.labelPx; font.bold: true }
        FontMetrics { id: fmPill; font.pixelSize: Math.max(7, Math.round(row.labelPx * 0.85)) }
        FontMetrics { id: fmValue; font.pixelSize: row.labelPx }
        // rounded up as a Text's implicit width is, and some room: a Text lays a bold label out wider than FontMetrics
        // measures it (seen with DejaVu Sans Bold and a synthetic bold Arial; the width sweep in sysgui_check)
        function need(c) {
            return Math.ceil(fmLabel.advanceWidth(c.label)) + Math.ceil(labelPx * 0.15)
                   + (c.pill.length ? sp + Math.ceil(fmPill.advanceWidth(c.pill)) + (c.bare ? 0 : pillPad) : 0)
                   + sp + Math.ceil(fmValue.advanceWidth(c.value))
        }
        // the first of the texts that fits (the fonts read here, so a change of size measures again)
        readonly property int form: {
            if (fmLabel.font.pixelSize + fmPill.font.pixelSize + fmValue.font.pixelSize <= 0 || _cands.length === 0)
                return 0
            for (var i = 0; i < _cands.length; ++i)
                if (need(_cands[i]) <= width)
                    return i
            return _cands.length - 1
        }
        readonly property var cand: _cands.length ? _cands[Math.min(form, _cands.length - 1)] : { label: "", pill: "", value: "" }
        readonly property bool bare: cand.bare === true
        Text {
            id: rowLabel
            objectName: "sysRowLabel"
            anchors.left: parent.left
            anchors.top: parent.top
            height: row.lineH
            width: Math.min(implicitWidth, Math.max(0, row.width - rowValue.implicitWidth - row.sp
                                                       - (pill.visible ? pill.width + row.sp : 0)))
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            text: row.cand.label
            color: AppPalette.textSecond
            font.pixelSize: row.labelPx
            font.bold: true
        }
        Rectangle {
            id: pill
            objectName: "sysRowPill"
            visible: row.cand.pill.length > 0
            readonly property color level: row._has ? row.line.pillColor : AppPalette.textMuted
            anchors.left: rowLabel.right
            anchors.leftMargin: row.sp
            anchors.verticalCenter: rowLabel.verticalCenter
            width: pillText.implicitWidth + (row.bare ? 0 : row.pillPad)
            height: Math.round(row.lineH * 0.86)
            radius: height / 2
            color: row.bare ? "transparent" : Qt.rgba(level.r, level.g, level.b, 0.16)
            border.width: row.bare ? 0 : 1
            border.color: level
            Text {
                id: pillText
                anchors.centerIn: parent
                text: row.cand.pill
                color: row._has ? row.line.pillTextColor : AppPalette.textSecond
                font.pixelSize: fmPill.font.pixelSize
            }
        }
        Text {
            id: rowValue
            objectName: "sysRowValue"
            anchors.right: parent.right
            anchors.top: parent.top
            height: row.lineH
            verticalAlignment: Text.AlignVCenter
            text: row.cand.value
            color: AppPalette.textStrong
            font.pixelSize: row.labelPx
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: row.barH
            radius: height / 2
            color: row.track
            Rectangle {
                objectName: "sysRowFill"
                width: Math.max(height, Math.round(parent.width * row.frac))
                height: parent.height
                radius: parent.radius
                color: row._has ? row.line.fillColor : "transparent"
            }
        }
    }

    // no data
    Rectangle {
        objectName: "sysNoData"
        visible: !ov._ok
        anchors.centerIn: parent
        width: Math.round(ov._valuePx * 0.85)
        height: Math.max(2, Math.round(ov._valuePx * 0.09))
        radius: height / 2
        color: AppPalette.textStrong
    }

    Item {
        id: body
        visible: ov._ok
        x: ov._pad
        y: ov._pad
        width: ov._innerW
        height: ov._innerH

        Column {
            id: cpuCol
            visible: !ov._compact
            width: ov._colW
            spacing: ov._gapY

            // CPU, the mean load, the fastest core; "held below its maximum" and the temperature on the right
            Item {
                id: head
                width: parent.width
                height: ov._headH
                readonly property real room: width - (tempChip.visible ? tempChip.width + ov._sp : 0)
                                                   - (throttleMark.visible ? throttleMark.implicitWidth + ov._sp : 0)
                TextMetrics { id: tmCap; font: cpuCap.font; text: cpuCap.text }
                TextMetrics { id: tmLoad; font: cpuLoad.font; text: ov._loadText }
                TextMetrics { id: tmGhz; font: cpuGhz.font; text: ov._ghzText }
                readonly property bool capFits: tmCap.advanceWidth + ov._sp + tmLoad.advanceWidth <= room
                readonly property bool ghzFits: ov._ghzText.length > 0
                    && (capFits ? tmCap.advanceWidth + ov._sp : 0) + tmLoad.advanceWidth + ov._sp + tmGhz.advanceWidth <= room
                Text {
                    id: cpuCap
                    objectName: "sysCpuCaption"
                    visible: head.capFits
                    anchors.left: parent.left
                    anchors.baseline: cpuLoad.baseline
                    text: "CPU"
                    color: AppPalette.textSecond
                    font.pixelSize: ov._labelPx
                    font.bold: true
                }
                Text {
                    id: cpuLoad
                    objectName: "sysCpu"
                    anchors.left: head.capFits ? cpuCap.right : parent.left
                    anchors.leftMargin: head.capFits ? ov._sp : 0
                    anchors.verticalCenter: parent.verticalCenter
                    // shrinks rather than cuts when the marks on the right leave too little
                    width: Math.min(tmLoad.advanceWidth + 1, Math.max(0, head.room - (head.capFits ? cpuCap.width + ov._sp : 0)))
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: Math.max(6, Math.round(ov._labelPx * 0.8))
                    elide: Text.ElideRight
                    text: ov._loadText
                    color: AppPalette.textStrong
                    font.pixelSize: ov._valuePx
                    font.bold: true
                }
                Text {
                    id: cpuGhz
                    objectName: "sysCpuGhz"
                    visible: head.ghzFits
                    anchors.left: cpuLoad.right
                    anchors.leftMargin: ov._sp
                    anchors.baseline: cpuLoad.baseline
                    text: ov._ghzText
                    color: AppPalette.textMuted
                    font.pixelSize: ov._labelPx
                }
                Text {
                    id: throttleMark
                    objectName: "sysThrottled"
                    visible: ov._ok && ov.sys.throttled === true
                    anchors.right: tempChip.visible ? tempChip.left : parent.right
                    anchors.rightMargin: tempChip.visible ? ov._sp : 0
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▼"
                    color: AppPalette.systemLevelColors[2]
                    font.pixelSize: ov._labelPx
                }
                Rectangle {
                    id: tempChip
                    objectName: "sysTemp"
                    visible: ov._hasTemp
                    readonly property color level: ov._hasTemp ? DataFieldCatalog.sysTempColor(ov.sys.cpuTempC) : AppPalette.textMuted
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: tempText.implicitWidth + Math.round(10 * ov.k)
                    height: Math.min(parent.height, Math.round(ov._labelPx * 1.45))
                    radius: height / 2
                    color: Qt.rgba(level.r, level.g, level.b, 0.20)
                    border.width: 1
                    border.color: level
                    Text {
                        id: tempText
                        anchors.centerIn: parent
                        text: ov._hasTemp ? Math.round(ov.sys.cpuTempC) + "°" : ""
                        color: tempChip.level
                        font.pixelSize: ov._labelPx
                        font.bold: true
                    }
                }
            }

            // each core: height its frequency of its maximum, colour its load; a wider gap where the cluster changes
            Item {
                id: coresBox
                objectName: "sysCores"
                width: parent.width
                height: ov._coresH
                readonly property int n: ov._cores.length
                function maxOf(i) { return (ov._ok && ov.sys.coreMaxMHz && i < ov.sys.coreMaxMHz.length) ? ov.sys.coreMaxMHz[i] : 0 }
                readonly property int clusters: {
                    var c = n > 0 ? 1 : 0
                    for (var i = 1; i < n; ++i)
                        if (maxOf(i) !== maxOf(i - 1)) ++c
                    return c
                }
                // gaps no wider than a quarter of a bar's share, so many cores still fit
                readonly property real gap: n > 0 ? Math.min(Math.max(1, Math.round(2 * ov.k)), Math.floor(width / (4 * n))) : 0
                readonly property real clusterGap: Math.max(gap, Math.min(Math.round(7 * ov.k), Math.floor(width / (4 * Math.max(1, clusters)))))
                readonly property real barW: n > 0 ? Math.max(0.5, (width - gap * (n - clusters) - clusterGap * Math.max(0, clusters - 1)) / n) : 0
                Row {
                    anchors.bottom: parent.bottom
                    Repeater {
                        model: coresBox.n
                        delegate: Item {
                            objectName: "sysCore"
                            required property int index
                            readonly property bool newCluster: index > 0 && coresBox.maxOf(index) !== coresBox.maxOf(index - 1)
                            readonly property real load: ov._cores[index]
                            readonly property real frac: {
                                var m = coresBox.maxOf(index)
                                var f = (ov.sys.coreFreqMHz && index < ov.sys.coreFreqMHz.length) ? ov.sys.coreFreqMHz[index] : 0
                                return m > 0 && f > 0 ? Math.min(1, f / m) : load
                            }
                            width: coresBox.barW + (index > 0 ? (newCluster ? coresBox.clusterGap : coresBox.gap) : 0)
                            height: coresBox.height
                            Rectangle {
                                id: coreTrack
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                width: coresBox.barW
                                height: parent.height
                                radius: Math.min(width / 2, Math.round(2 * ov.k))
                                color: ov._track
                            }
                            Rectangle {
                                objectName: "sysCoreFill"
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                width: coresBox.barW
                                height: Math.max(2, Math.round(coreTrack.height * parent.frac))
                                radius: coreTrack.radius
                                color: DataFieldCatalog.sysLoadColor(parent.load)
                            }
                        }
                    }
                }
            }

            // wide: the GPU under the cores
            SysRow {
                objectName: "sysGpuRow"
                visible: ov._gpuLeft
                width: parent.width
                line: ov._gpuRow
                k: ov.k
                labelPx: ov._labelPx
                lineH: ov._lineH
                barH: ov._barH
                sp: ov._sp
                pillPad: ov._pillPad
                track: ov._track
            }
        }

        // the lines: the CPU (compact) and the GPU (one column), memory and the volumes - the label (and its chip)
        // on the left, the amount on the right, the bar under them; then the uptime
        Column {
            id: infoCol
            x: ov._wide ? ov._colW + ov._gapX : 0
            y: (ov._wide || ov._compact) ? 0 : ov._headH + ov._gapY + ov._cores1H + ov._gapY
            width: ov._colW
            spacing: ov._infoSpacing

            Repeater {
                model: ov._infoRows
                delegate: SysRow {
                    objectName: "sysRow"
                    required property var modelData
                    required property int index
                    visible: index < ov._rowsShown
                    width: infoCol.width
                    line: modelData
                    k: ov.k
                    labelPx: ov._labelPx
                    lineH: ov._lineH
                    barH: ov._barH
                    sp: ov._sp
                    pillPad: ov._pillPad
                    track: ov._track
                }
            }

            Item {
                id: foot
                objectName: "sysFoot"
                visible: ov._showFoot
                width: infoCol.width
                height: ov._lineH
                Text {
                    objectName: "sysUptime"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, foot.width)
                    elide: Text.ElideRight
                    text: ov._upText
                    color: AppPalette.textSecond
                    font.pixelSize: ov._labelPx
                }
            }
        }
    }
}
