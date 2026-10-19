import QtQuick 2.15
import QtQuick.Controls 2.15

Item {
    id: root

    readonly property string controlKind: "tabs"     // a pinned row's widget mirrors it (PinRegistry)
    property var options: []
    property var currentValue
    property int buttonHeight: Math.round(30 * AppPalette.scale)
    property int buttonSpacing: Math.round(6 * AppPalette.scale)
    property int horizontalPadding: Math.round(4 * AppPalette.scale)
    property int verticalPadding: Math.round(4 * AppPalette.scale)
    property int cornerRadius: Math.round(8 * AppPalette.scale)
    property int fontPixelSize: Tokens.fontLg
    property color trackColor: AppPalette.bg
    property bool dragSelectEnabled: true
    signal valueSelected(var value)

    readonly property int optionCount: Array.isArray(options) ? options.length : 0
    readonly property real innerWidth: Math.max(0, width - horizontalPadding * 2)
    property int labelPadding: Math.round(14 * AppPalette.scale)   // по 7 с каждой стороны ярлыка

    // Мерка для ярлыков: тем же начертанием, каким они рисуются, иначе доля
    // окажется уже надписи и подсветка съедет с текста.
    FontMetrics {
        id: tabFontMetrics
        font.pixelSize: root.fontPixelSize
        font.bold: true
    }

    /*
     * Доли нарезаны по ширине ярлыков, а не поровну: при равных долях длинное
     * слово вылезает за свою подсветку (было видно на «Compressed» среди пяти
     * положений). Что не разошлось по надписям, делится поровну; если надписи
     * не помещаются вовсе, доли ужимаются пропорционально, а ярлык обрезается
     * многоточием — он ограничен шириной своей доли.
     */
    readonly property var segmentWidths: {
        var n = optionCount
        var res = []
        if (n <= 0)
            return res
        var px = tabFontMetrics.font.pixelSize      // зависимость: пересчитать при смене шрифта
        var pad = labelPadding
        var avail = Math.max(0, innerWidth - (n - 1) * buttonSpacing)
        var need = []
        var total = 0
        for (var i = 0; i < n; ++i) {
            var w = Math.ceil(tabFontMetrics.advanceWidth(optionLabelAt(i))) + pad
            need.push(w)
            total += w
        }
        if (total <= avail) {
            var extra = (avail - total) / n
            for (i = 0; i < n; ++i)
                res.push(need[i] + extra)
        } else {
            for (i = 0; i < n; ++i)
                res.push(avail * need[i] / total)
        }
        return res
    }

    function segmentWidthAt(idx) {
        var w = segmentWidths
        return (idx >= 0 && idx < w.length) ? w[idx] : 0
    }

    function segmentXAt(idx) {
        var x = horizontalPadding
        var w = segmentWidths
        for (var i = 0; i < idx && i < w.length; ++i)
            x += w[i] + buttonSpacing
        return x
    }
    readonly property int selectedIndex: indexOfCurrentValue()
    property int dragPreviewIndex: -1
    readonly property int visualIndex: dragPreviewIndex >= 0 ? dragPreviewIndex : selectedIndex
    property bool syncingFromValue: false

    implicitWidth: {
        var px = tabFontMetrics.font.pixelSize
        var sum = 0
        for (var i = 0; i < optionCount; ++i)
            sum += Math.ceil(tabFontMetrics.advanceWidth(optionLabelAt(i))) + labelPadding
        return Math.max(Math.round(120 * AppPalette.scale),
                        sum + Math.max(0, optionCount - 1) * buttonSpacing + horizontalPadding * 2)
    }
    implicitHeight: buttonHeight + verticalPadding * 2

    function optionAt(idx) {
        if (!Array.isArray(options) || idx < 0 || idx >= options.length)
            return null
        return options[idx]
    }

    function optionLabelAt(idx) {
        var option = optionAt(idx)
        if (option && option.label !== undefined)
            return String(option.label)
        return String(option)
    }

    function optionValueAt(idx) {
        var option = optionAt(idx)
        if (option && option.value !== undefined)
            return option.value
        return option
    }

    // An option may carry `enabled: false`: shown dimmed, never selected (click, drag or keys). Options without the
    // key are enabled, as before.
    function optionEnabledAt(idx) {
        var option = optionAt(idx)
        return !(option && option.enabled === false)
    }

    function optionTipAt(idx) {
        var option = optionAt(idx)
        if (option && option.tip !== undefined)
            return String(option.tip)
        return ""
    }

    function indexOfCurrentValue() {
        if (!Array.isArray(options))
            return -1
        for (var i = 0; i < options.length; ++i) {
            if (optionValueAt(i) === currentValue)
                return i
        }
        return -1
    }

    function indexFromPosition(xPos) {
        if (optionCount <= 0)
            return -1

        var localX = xPos - horizontalPadding
        if (localX < 0)
            localX = 0
        if (localX > innerWidth)
            localX = innerWidth

        var bestIndex = 0
        var bestDistance = Number.MAX_VALUE
        var cursor = 0
        for (var i = 0; i < optionCount; ++i) {
            var w = segmentWidthAt(i)
            var centerX = cursor + w / 2
            var distance = Math.abs(localX - centerX)
            if (distance < bestDistance) {
                bestDistance = distance
                bestIndex = i
            }
            cursor += w + buttonSpacing
        }
        return bestIndex
    }

    function applyIndex(indexToApply) {
        if (indexToApply < 0 || indexToApply >= optionCount || !optionEnabledAt(indexToApply))
            return

        if (tabBar.currentIndex !== indexToApply) {
            syncingFromValue = true
            tabBar.currentIndex = indexToApply
            syncingFromValue = false
        }

        var nextValue = optionValueAt(indexToApply)
        if (nextValue !== currentValue)
            valueSelected(nextValue)
    }

    function syncFromCurrentValue() {
        var idx = selectedIndex
        if (idx < 0)
            idx = 0
        if (tabBar.currentIndex === idx)
            return
        syncingFromValue = true
        tabBar.currentIndex = idx
        syncingFromValue = false
    }

    onCurrentValueChanged: {
        syncFromCurrentValue()
        if (root.dragPreviewIndex >= 0 && root.dragPreviewIndex === root.selectedIndex)
            root.dragPreviewIndex = -1
    }
    onOptionsChanged: syncFromCurrentValue()
    Component.onCompleted: syncFromCurrentValue()

    activeFocusOnTab: true
    property bool _ringSuppressed: false
    onActiveFocusChanged: if (!activeFocus) _ringSuppressed = false
    Keys.onLeftPressed:  function(e) { root.applyIndex(Math.max(0, root.selectedIndex - 1)); e.accepted = true }
    Keys.onUpPressed:    function(e) { root.applyIndex(Math.max(0, root.selectedIndex - 1)); e.accepted = true }
    Keys.onRightPressed: function(e) { root.applyIndex(Math.min(root.optionCount - 1, root.selectedIndex + 1)); e.accepted = true }
    Keys.onDownPressed:  function(e) { root.applyIndex(Math.min(root.optionCount - 1, root.selectedIndex + 1)); e.accepted = true }

    Rectangle {
        anchors.fill: parent
        radius: root.cornerRadius
        color: root.trackColor
        border.width: (root.activeFocus && !root._ringSuppressed) ? 2 : Tokens.cardBorderWidth
        border.color: (root.activeFocus && !root._ringSuppressed) ? AppPalette.accentBorder : AppPalette.border
    }

    Rectangle {
        visible: tabMouseArea.hoverIndex >= 0 && tabMouseArea.hoverIndex !== root.visualIndex
                 && root.optionEnabledAt(tabMouseArea.hoverIndex)
        x: root.segmentXAt(tabMouseArea.hoverIndex)
        y: root.verticalPadding
        width: root.segmentWidthAt(tabMouseArea.hoverIndex)
        height: root.buttonHeight
        radius: Math.round(6 * AppPalette.scale)
        color: AppPalette.bgHover
        z: 0
    }

    Rectangle {
        visible: root.visualIndex >= 0 && root.optionCount > 0
        x: root.segmentXAt(root.visualIndex)
        y: root.verticalPadding
        width: root.segmentWidthAt(root.visualIndex)
        height: root.buttonHeight
        radius: Math.round(6 * AppPalette.scale)
        color: AppPalette.accentBg
        border.width: 1
        border.color: AppPalette.accentBorder
        z: 1

        Behavior on x {
            NumberAnimation {
                duration: 160
                easing.type: Easing.OutCubic
            }
        }

        Behavior on width {
            NumberAnimation {
                duration: 160
                easing.type: Easing.OutCubic
            }
        }
    }

    // Visual labels only — interaction handled by tabMouseArea below
    TabBar {
        id: tabBar

        x: root.horizontalPadding
        y: root.verticalPadding
        width: root.innerWidth
        height: root.buttonHeight
        spacing: root.buttonSpacing
        z: 2

        background: Item {}

        Repeater {
            model: root.optionCount

            delegate: TabButton {
                id: tabButton
                property int optionIndex: index
                readonly property bool selected: optionIndex === root.visualIndex

                width: root.segmentWidthAt(optionIndex)
                height: root.buttonHeight
                text: root.optionLabelAt(optionIndex)
                padding: 0
                topPadding: 0
                bottomPadding: 0
                leftPadding: 0
                rightPadding: 0
                focusPolicy: Qt.NoFocus

                background: Item {}

                contentItem: Item {
                    implicitWidth: label.implicitWidth
                    implicitHeight: tabButton.height

                    Text {
                        id: label
                        anchors.centerIn: parent
                        // Ограничен долей: иначе многоточие не сработает и
                        // надпись вылезет за подсветку.
                        width: Math.max(0, tabButton.width - Math.round(4 * AppPalette.scale))
                        horizontalAlignment: Text.AlignHCenter
                        text: tabButton.text
                        color: selected ? AppPalette.accentText
                               : !root.optionEnabledAt(tabButton.optionIndex)
                                 ? Qt.rgba(AppPalette.textStrong.r, AppPalette.textStrong.g, AppPalette.textStrong.b, 0.32)
                               : (tabButton.optionIndex === tabMouseArea.hoverIndex
                                  ? AppPalette.textStrong
                                  : Qt.rgba(AppPalette.textStrong.r, AppPalette.textStrong.g, AppPalette.textStrong.b, 0.78))
                        font.pixelSize: root.fontPixelSize
                        font.bold: true
                        elide: Text.ElideRight
                    }
                }

                KToolTip {
                    text: root.optionTipAt(tabButton.optionIndex)
                    shown: tabMouseArea.hoverIndex === tabButton.optionIndex
                           && !tabMouseArea.pressed
                }
            }
        }
    }

    MouseArea {
        id: tabMouseArea
        anchors.fill: parent
        hoverEnabled: true
        z: 3

        property real pressX: 0
        property bool dragging: false
        property int hoverIndex: -1

        onPressed: function(mouse) {
            pressX = mouse.x
            dragging = false
            root._ringSuppressed = true
            if (root.dragSelectEnabled) {
                var pressIdx = root.indexFromPosition(mouse.x)
                root.dragPreviewIndex = root.optionEnabledAt(pressIdx) ? pressIdx : -1
            }
        }

        onPositionChanged: function(mouse) {
            tabMouseArea.hoverIndex = root.indexFromPosition(mouse.x)
            if (!pressed || !root.dragSelectEnabled)
                return
            if (!dragging && Math.abs(mouse.x - pressX) > Math.round(6 * AppPalette.scale))
                dragging = true
            if (dragging) {
                var dragIdx = root.indexFromPosition(mouse.x)
                root.dragPreviewIndex = root.optionEnabledAt(dragIdx) ? dragIdx : -1
            }
        }

        onExited: tabMouseArea.hoverIndex = -1

        onReleased: function(mouse) {
            var idx = root.indexFromPosition(mouse.x)
            root.dragPreviewIndex = -1
            dragging = false
            root.forceActiveFocus()
            root.applyIndex(idx)
        }

        onCanceled: {
            root.dragPreviewIndex = -1
            dragging = false
        }
    }
}
