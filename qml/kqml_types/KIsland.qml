import QtQuick 2.15

Item {
    id: island

    readonly property bool isKIsland: true

    property string title: ""
    property string footer: ""
    property color fillColor: AppPalette.card
    property color borderColor: AppPalette.border
    property color titleColor: AppPalette.textStrong
    property color footerColor: AppPalette.textSecond
    property color separatorColor: AppPalette.separator
    property real cornerRadius: Tokens.radiusLg
    property real rowPadding: Tokens.spaceLg
    property real titleInset: rowPadding
    property real separatorInset: rowPadding
    property real slotWidth: 0
    property real rowMinHeight: Tokens.rowH
    property int labelPixelSize: Tokens.fontLg
    property bool separatorsVisible: true
    // Folded: only the first visible row shows (a section's title row); the others keep their layout, clipped away
    // (they get no input there either), and the footer hides.
    property bool folded: false
    readonly property real _foldedHeight: {
        if (!folded)
            return 0
        var kids = rowColumn.children
        for (var i = 0; i < kids.length; ++i)
            if (kids[i].visible && kids[i].height > 0)
                return kids[i].y + kids[i].height
        return 0
    }

    default property alias rows: rowColumn.data

    width: parent ? parent.width : implicitWidth
    implicitWidth: Math.round(240 * AppPalette.scale)
    implicitHeight: stack.implicitHeight

    Column {
        id: stack
        width: parent.width
        spacing: Tokens.spaceSm

        Text {
            x: island.titleInset
            width: Math.max(0, parent.width - island.titleInset * 2)
            visible: island.title.length > 0
            text: island.title
            color: island.titleColor
            font.pixelSize: Tokens.fontBase
            elide: Text.ElideRight
        }

        Rectangle {
            width: parent.width
            radius: island.cornerRadius
            color: island.fillColor
            border.width: Tokens.cardBorderWidth
            border.color: island.borderColor
            implicitHeight: island.folded ? island._foldedHeight : rowColumn.implicitHeight
            clip: island.folded

            Column {
                id: rowColumn
                width: parent.width
                spacing: 0
            }
        }

        Text {
            x: island.rowPadding
            width: Math.max(0, parent.width - island.rowPadding * 2)
            visible: island.footer.length > 0 && !island.folded
            text: island.footer
            color: island.footerColor
            font.pixelSize: Tokens.fontSm
            wrapMode: Text.Wrap
        }
    }
}
