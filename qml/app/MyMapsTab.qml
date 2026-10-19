import QtQuick 2.15
import QtQuick.Dialogs
import kqml_types 1.0

// Drill-in: My maps (docs/map-export.md). The KOGGER maps (.kmap) opened here, from the Open menu or
// dropped on the window: each is drawn under the boat in the 3D view, with its author and licence.
// The C++ objects are `mapLibrary` and `mapLayer`; set from outside by tools/qml_check/mymaps_page_check.ps1.
Column {
    id: page

    required property var store
    property var library: typeof mapLibrary !== "undefined" ? mapLibrary : null
    property var mapsLayer: typeof mapLayer !== "undefined" ? mapLayer : null   // not "layer": Item has one
    property string pendingRemove: ""
    // Android: a map from a messenger is opened from the messenger ("open with"), not dropped.
    property bool isAndroid: Qt.platform.os === "android"

    readonly property color _bright: AppPalette.isDark ? "#FFFFFF" : AppPalette.text

    width: parent ? parent.width : implicitWidth
    spacing: Tokens.spaceSm

    component Spacer: Item {
        property int size: Tokens.spaceMd
        width: page.width
        height: size
    }

    component Note: Text {
        width: page.width
        wrapMode: Text.WordWrap
        color: AppPalette.textMuted
        font.pixelSize: Tokens.fontSm
    }

    function sizeText(m) {
        var w = Math.round(m.widthM), h = Math.round(m.heightM)
        var size = w >= 1000 || h >= 1000
                ? (m.widthM / 1000).toFixed(1) + " × " + (m.heightM / 1000).toFixed(1) + " " + qsTr("km")
                : w + " × " + h + " " + qsTr("m")
        if (m.hasDepth)
            size += " · " + m.minDepth.toFixed(1) + "–" + m.maxDepth.toFixed(1) + " " + qsTr("m deep")
        return size
    }

    Note {
        text: page.isAndroid
              ? qsTr("Depth maps you saved or others sent you. They lie under the boat in the 3D view. To open a .kmap someone sent you, tap it in the messenger and choose KoggerApp, or use the button below.")
              : qsTr("Depth maps you saved or others sent you. They lie under the boat in the 3D view. Open a .kmap file with the button below, from the Open menu, or by dropping it on the window.")
    }

    Spacer {}

    KButton {
        width: parent.width
        height: Tokens.controlHLg
        enabled: page.library !== null
        text: qsTr("Open a map…")
        onClicked: openMapDialog.open()
    }

    FileDialog {
        id: openMapDialog
        title: qsTr("Open a KOGGER map")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("KOGGER maps") + " (*.kmap *.KMAP)"]
        onAccepted: if (page.library && selectedFile) page.library.importFile(selectedFile.toString())
    }

    Text {
        width: parent.width
        visible: page.library !== null && page.library.lastMessage.length > 0
        text: page.library ? page.library.lastMessage : ""
        wrapMode: Text.WordWrap
        color: page.library && page.library.lastOk ? AppPalette.text : AppPalette.dangerBorder
        font.pixelSize: Tokens.fontSm
    }

    Text {
        width: parent.width
        visible: page.library !== null && page.library.boatDepthValid
        text: page.library ? qsTr("Under the boat by the map: %1 m (%2)").arg(page.library.boatDepth.toFixed(1)).arg(page.library.boatDepthMap) : ""
        wrapMode: Text.WordWrap
        color: page._bright
        font.pixelSize: Tokens.fontBase
        font.bold: true
    }

    Spacer { size: Tokens.spaceXl }

    KIsland {
        KIslandRow {
            label: qsTr("Show the maps")
            labelColor: page._bright
            interactive: true
            onClicked: layerSwitch.click()
            KSwitch {
                id: layerSwitch
                flat: true
                checked: page.mapsLayer ? page.mapsLayer.visible : true
                onToggled: if (page.mapsLayer) page.mapsLayer.visible = checked
            }
        }
        KIslandRow {
            label: qsTr("Depths and isobaths")
            labelColor: page._bright
            interactive: true
            open: layerSwitch.checked
            onClicked: depthSwitch.click()
            KSwitch {
                id: depthSwitch
                flat: true
                checked: page.mapsLayer ? page.mapsLayer.depthVisible : true
                onToggled: if (page.mapsLayer) page.mapsLayer.depthVisible = checked
            }
        }
        KIslandRow {
            label: qsTr("Side-scan mosaic")
            labelColor: page._bright
            interactive: true
            open: layerSwitch.checked
            onClicked: mosaicSwitch.click()
            KSwitch {
                id: mosaicSwitch
                flat: true
                checked: page.mapsLayer ? page.mapsLayer.mosaicVisible : false
                onToggled: if (page.mapsLayer) page.mapsLayer.mosaicVisible = checked
            }
        }
    }

    Spacer {}

    Column {
        id: emptyState
        width: parent.width
        spacing: Tokens.spaceSm
        visible: page.library !== null && page.library.maps.length === 0
        Note {
            text: qsTr("No maps yet. Ask someone to send you a .kmap file, or save your own: Settings → Export → Map for sharing.")
        }
        KButton {
            text: qsTr("Save my map")
            onClicked: if (page.store) page.store.openMapExportSettings()
        }
    }

    Repeater {
        model: page.library ? page.library.maps : []
        delegate: Rectangle {
            id: card
            required property var modelData
            width: page.width
            height: cardRow.height + 2 * Tokens.spaceMd
            radius: Tokens.radiusLg
            color: AppPalette.card

            Row {
                id: cardRow
                x: Tokens.spaceMd
                y: Tokens.spaceMd
                width: parent.width - 2 * Tokens.spaceMd
                spacing: Tokens.spaceMd

                Rectangle {
                    width: Math.round(76 * AppPalette.scale)
                    height: width
                    radius: Tokens.radiusMd
                    color: AppPalette.bg
                    clip: true
                    Image {
                        anchors.fill: parent
                        anchors.margins: 2
                        source: card.modelData.preview
                        fillMode: Image.PreserveAspectFit
                        sourceSize.width: 160
                        sourceSize.height: 160
                        asynchronous: true
                    }
                }

                Column {
                    width: parent.width - Math.round(76 * AppPalette.scale) - parent.spacing
                    spacing: Tokens.spaceXs

                    Row {
                        width: parent.width
                        Text {
                            width: parent.width - visSwitch.width
                            text: card.modelData.title
                            color: page._bright
                            font.pixelSize: Tokens.fontLg
                            font.bold: true
                            elide: Text.ElideRight
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        KSwitch {
                            id: visSwitch
                            flat: true
                            checked: card.modelData.visible
                            toolTipText: qsTr("Show this map")
                            onToggled: if (page.library) page.library.setVisible(card.modelData.id, checked)
                        }
                    }
                    Text {
                        width: parent.width
                        text: (card.modelData.author.length ? card.modelData.author : qsTr("Author not given")) + " · " + card.modelData.license
                        color: AppPalette.textMuted
                        font.pixelSize: Tokens.fontSm
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        width: parent.width
                        text: page.sizeText(card.modelData)
                        color: AppPalette.textMuted
                        font.pixelSize: Tokens.fontSm
                        wrapMode: Text.WordWrap
                    }
                    Row {
                        spacing: Tokens.spaceSm
                        KButton {
                            text: qsTr("Show on the map")
                            onClicked: if (page.library) page.library.showOnMap(card.modelData.id)
                        }
                        KButton {
                            danger: page.pendingRemove === card.modelData.id
                            text: page.pendingRemove === card.modelData.id ? qsTr("Remove for good?") : qsTr("Remove")
                            onClicked: {
                                if (page.pendingRemove === card.modelData.id) {
                                    page.pendingRemove = ""
                                    if (page.library) page.library.remove(card.modelData.id)
                                } else {
                                    page.pendingRemove = card.modelData.id
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Spacer {}

    Note {
        text: qsTr("Maps from others are amateur surveys: NOT FOR NAVIGATION. Each map carries its own licence; what you may do with it is shown on its card.")
    }
}
