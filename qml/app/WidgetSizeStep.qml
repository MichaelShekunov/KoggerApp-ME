import QtQuick 2.15
import kqml_types 1.0

Column {
    id: step

    required property var store

    width: parent ? parent.width : implicitWidth
    spacing: Tokens.spaceLg

    Component.onDestruction: if (store) {
        store.widgetHoverCols = 0
        store.widgetHoverRows = 0
    }

    Text {
        width: parent.width
        wrapMode: Text.WordWrap
        text: qsTr("Choose the panel grid size. You will place widgets on the next step.")
        color: AppPalette.textMuted
        font.pixelSize: Tokens.fontSm
    }

    readonly property int _maxCols: store ? store.widgetMaxCols : 4
    readonly property int _maxRows: store ? store.widgetMaxRows : 4

    Grid {
        width: parent.width
        columns: step._maxCols
        spacing: Tokens.spaceMd

        Repeater {
            // Every size the panels allow, a row of cards per number of rows.
            model: {
                var out = []
                for (var r = 1; r <= step._maxRows; ++r)
                    for (var c = 1; c <= step._maxCols; ++c)
                        out.push({ c: c, r: r })
                return out
            }
            delegate: Rectangle {
                id: sizeCard
                required property var modelData
                readonly property bool hovered: cardMouse.containsMouse
                width: Math.floor((step.width - (step._maxCols - 1) * Tokens.spaceMd) / step._maxCols)
                height: Math.round(96 * AppPalette.scale)
                radius: Tokens.radiusLg
                color: hovered ? AppPalette.cardHover : AppPalette.card
                border.width: hovered ? Math.max(1, Math.round(1 * AppPalette.scale)) : Tokens.cardBorderWidth
                border.color: hovered ? AppPalette.borderHover : AppPalette.border

                Column {
                    anchors.centerIn: parent
                    spacing: Tokens.spaceSm

                    Item {
                        id: mini
                        width: Math.round(48 * AppPalette.scale)
                        height: Math.round(56 * AppPalette.scale)
                        anchors.horizontalCenter: parent.horizontalCenter
                        readonly property real gap: Math.round(3 * AppPalette.scale)
                        readonly property real cw: (width - (sizeCard.modelData.c - 1) * gap) / sizeCard.modelData.c
                        readonly property real ch: (height - (sizeCard.modelData.r - 1) * gap) / sizeCard.modelData.r

                        Repeater {
                            model: sizeCard.modelData.c * sizeCard.modelData.r
                            delegate: Rectangle {
                                required property int index
                                x: (index % sizeCard.modelData.c) * (mini.cw + mini.gap)
                                y: Math.floor(index / sizeCard.modelData.c) * (mini.ch + mini.gap)
                                width: mini.cw
                                height: mini.ch
                                radius: 2
                                color: "transparent"
                                border.width: 1
                                border.color: AppPalette.borderHover
                            }
                        }
                    }

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: sizeCard.modelData.c + " × " + sizeCard.modelData.r
                        color: AppPalette.text
                        font.pixelSize: Tokens.fontMd
                        font.bold: true
                    }
                }

                MouseArea {
                    id: cardMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onEntered: if (step.store) {
                        step.store.widgetHoverCols = sizeCard.modelData.c
                        step.store.widgetHoverRows = sizeCard.modelData.r
                    }
                    onClicked: {
                        if (!step.store) return
                        step.store.widgetHoverCols = 0
                        step.store.widgetHoverRows = 0
                        step.store.widgetDraftSetSize(sizeCard.modelData.c, sizeCard.modelData.r)
                        Qt.callLater(function() { step.store.widgetEditStep = 2 })
                    }
                }
            }
        }
    }
}
