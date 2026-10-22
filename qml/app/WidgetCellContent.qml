import QtQuick 2.15
import kqml_types 1.0

Item {
    id: content

    property string rep: "value"
    property string label: ""
    property string value: ""
    property real k: 1.0
    property real gap: 0
    // A stacked bar under the value (DataFieldCatalog.bars): [{ v, c }], each part as wide as its share of the sum.
    property var bars: []
    // The field and SystemMonitor (or its sample): "System" is drawn whole by SystemOverview, and a cell of a
    // system field keeps the monitor reading while it lives.
    property string field: ""
    property var sys: null
    readonly property bool _overview: content.field === "sysOverview"
    property var _subscribed: null
    function _syncSubscription() {
        var want = (DataFieldCatalog.usesSystem(content.field) && content.sys && typeof content.sys.subscribe === "function")
                   ? content.sys : null
        if (want === content._subscribed)
            return
        if (content._subscribed)
            content._subscribed.unsubscribe()
        content._subscribed = want
        if (want)
            want.subscribe()
    }
    onFieldChanged: _syncSubscription()
    onSysChanged: _syncSubscription()
    Component.onCompleted: _syncSubscription()
    Component.onDestruction: if (content._subscribed) content._subscribed.unsubscribe()

    SystemOverview {
        objectName: "sysOverview"
        visible: content._overview
        anchors.fill: parent
        sys: content.sys
        k: content.k
    }

    readonly property real _margin: Math.round(6 * k)
    readonly property real _minPx: Math.max(6, Math.round(7 * k))

    // The type follows the cell's height, but no further than its width: a cell is never taller than wide on a
    // floating panel; stretched along a side dock it is, and a reading the cell's height there is no reading.
    readonly property real _typeBox: Math.min(content.height, content.width)
    readonly property int _labelPx:    Math.max(_minPx, Math.round(_typeBox * 0.17))
    readonly property int _valuePx:    Math.max(_minPx, Math.round(_typeBox * 0.30))
    readonly property int _valueBigPx: Math.max(_minPx, Math.round(_typeBox * 0.45))

    readonly property bool _noData: content.value === "—"
    readonly property bool _barsShown: !!content.bars && content.bars.length > 0 && !content._noData
    readonly property real _barH: Math.max(3, Math.round(_typeBox * 0.12))
    // Room the value leaves at the bottom for the bar.
    readonly property real _barSpace: _barsShown ? _barH + Math.round(2 * k) : 0

    component ValueView : Item {
        id: vv
        property string text: ""
        property int pixelSize: 12
        property int minPx: 6
        property bool noData: false
        property int hAlign: Text.AlignHCenter

        Text {
            visible: !vv.noData
            anchors.fill: parent
            horizontalAlignment: vv.hAlign
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.NoWrap
            maximumLineCount: 2
            fontSizeMode: Text.Fit
            minimumPixelSize: vv.minPx
            font.pixelSize: vv.pixelSize
            font.bold: true
            elide: Text.ElideRight
            text: vv.text
            color: AppPalette.textStrong
        }
        Rectangle {
            visible: vv.noData
            width: Math.round(vv.pixelSize * 0.85)
            height: Math.max(2, Math.round(vv.pixelSize * 0.09))
            y: Math.round((vv.height - height) / 2)
            x: vv.hAlign === Text.AlignLeft ? 0
               : vv.hAlign === Text.AlignRight ? (vv.width - width)
               : Math.round((vv.width - width) / 2)
            radius: height / 2
            color: AppPalette.textStrong
        }
    }

    ValueView {
        visible: content.rep === "value" && !content._overview
        anchors.fill: parent
        anchors.margins: content._margin
        anchors.bottomMargin: content._margin + content._barSpace
        text: content.value
        pixelSize: content._valueBigPx
        minPx: content._minPx
        noData: content._noData
    }

    Item {
        visible: content.rep === "labelValueStacked" && !content._overview
        anchors.fill: parent
        anchors.margins: content._margin
        anchors.bottomMargin: content._margin + content._barSpace

        Text {
            id: stLabel
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: parent.height * 0.45
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            fontSizeMode: Text.Fit
            minimumPixelSize: content._minPx
            font.pixelSize: content._labelPx
            font.bold: true
            elide: Text.ElideRight
            text: content.label
            color: AppPalette.textSecond
        }
        ValueView {
            anchors.top: stLabel.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            text: content.value
            pixelSize: content._valuePx
            minPx: content._minPx
            noData: content._noData
        }
    }

    Item {
        visible: content.rep === "labelValueRow" && !content._overview
        anchors.fill: parent
        anchors.margins: content._margin
        anchors.bottomMargin: content._margin + content._barSpace

        Text {
            id: rowLabel
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: (parent.width - content.gap) / 2
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
            maximumLineCount: 3
            fontSizeMode: Text.Fit
            minimumPixelSize: content._minPx
            font.pixelSize: content._labelPx
            font.bold: true
            elide: Text.ElideRight
            text: content.label
            color: AppPalette.textSecond
        }
        ValueView {
            anchors.left: rowLabel.right
            anchors.leftMargin: content.gap
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            text: content.value
            pixelSize: content._valuePx
            minPx: content._minPx
            noData: content._noData
            hAlign: Text.AlignLeft
        }
    }

    Row {
        id: barRow
        visible: content._barsShown
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: content._margin
        height: content._barH
        readonly property real total: {
            var t = 0
            for (var i = 0; i < (content.bars ? content.bars.length : 0); ++i)
                t += Math.max(0, content.bars[i].v)
            return t
        }
        Repeater {
            model: content._barsShown ? content.bars : []
            delegate: Rectangle {
                required property var modelData
                height: barRow.height
                width: barRow.total > 0 ? barRow.width * Math.max(0, modelData.v) / barRow.total : 0
                color: modelData.c
            }
        }
    }
}
