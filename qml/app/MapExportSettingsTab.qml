import QtQuick 2.15
import kqml_types 1.0

// Drill-in: the map for sharing (docs/map-export.md). Saves the side-scan mosaic and the depth map as
// a KMZ for Google Earth and GeoTIFFs for GIS, with the author, the licence and the "not for
// navigation" notice written into every file. The settings live in C++ (mapExport) and are kept
// between runs; the folder is the Export group's.
Column {
    id: page

    required property var store

    readonly property color _bright: AppPalette.isDark ? "#FFFFFF" : AppPalette.text
    // The C++ objects; set from outside by tools/qml_check/mapexport_page_check.ps1.
    property var exporter: typeof mapExport !== "undefined" ? mapExport : null
    property var coreApi: typeof core !== "undefined" ? core : null
    readonly property int fieldW: Math.round(Math.min(260 * AppPalette.scale, page.width * 0.58))

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

    // A text field in an island row: the value is written back on every edit.
    component FieldRow: KIslandRow {
        id: fieldRow
        property alias text: field.text
        property string placeholder: ""
        signal edited(string value)
        labelColor: page._bright

        Rectangle {
            width: page.fieldW
            height: Tokens.controlHMd
            radius: Tokens.radiusMd
            color: AppPalette.bg
            border.width: field.activeFocus ? 1 : Tokens.cardBorderWidth
            border.color: field.activeFocus ? AppPalette.accentBorder : AppPalette.border

            TextInput {
                id: field
                activeFocusOnTab: true
                anchors.fill: parent
                anchors.leftMargin: Tokens.spaceMd
                anchors.rightMargin: Tokens.spaceMd
                verticalAlignment: TextInput.AlignVCenter
                color: AppPalette.text
                font.pixelSize: Tokens.fontBase
                clip: true
                maximumLength: 120
                onTextEdited: fieldRow.edited(text)
                TapHandler { acceptedButtons: Qt.LeftButton; onDoubleTapped: field.selectAll() }

                Text {
                    visible: !field.text.length
                    text: fieldRow.placeholder
                    color: AppPalette.textMuted
                    font.pixelSize: Tokens.fontBase
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
    }

    function folder() {
        if (page.store && page.store.exportFolderSource && page.store.exportFolderSource.length)
            return page.store.exportFolderSource
        return page.coreApi ? page.coreApi.defaultExportDirectory() : ""
    }

    function licenseIndex() {
        if (!page.exporter) return 0
        var opts = page.exporter.licenseOptions
        for (var i = 0; i < opts.length; ++i)
            if (opts[i].id === page.exporter.licenseId) return i
        return 0
    }

    function detailIndex() {
        if (!page.exporter) return 0
        var opts = page.exporter.detailOptions
        for (var i = 0; i < opts.length; ++i)
            if (opts[i].zoom === page.exporter.detailZoom) return i
        return 0
    }

    // ── Description ───────────────────────────────────────────────────────
    Note {
        text: qsTr("Save the depth map and the side-scan mosaic of the open survey as a KOGGER map (.kmap) to share it: whoever gets the file opens it in KoggerApp and sees the map under their boat. For Google Earth and GIS a KMZ and GeoTIFFs can be saved too. The folder is set in the Export group.")
    }

    // ── Save + progress ───────────────────────────────────────────────────
    Spacer {}

    KButton {
        width: parent.width
        height: Tokens.controlHLg
        enabled: page.exporter !== null
        text: page.exporter && page.exporter.busy ? qsTr("Cancel") : qsTr("Save the map")
        onClicked: {
            if (!page.exporter) return
            if (page.exporter.busy) page.exporter.cancel()
            else page.exporter.start(page.folder())
        }
    }

    Column {
        width: parent.width
        spacing: Tokens.spaceXs
        visible: page.exporter !== null && page.exporter.busy

        Text {
            width: parent.width
            text: page.exporter ? page.exporter.stage + " — " + Math.round(page.exporter.progress * 100) + " %" : ""
            color: AppPalette.text
            font.pixelSize: Tokens.fontSm
            elide: Text.ElideRight
        }
        Rectangle {
            width: parent.width
            height: Math.round(6 * AppPalette.scale)
            radius: height / 2
            color: AppPalette.bg
            Rectangle {
                width: parent.width * (page.exporter ? Math.max(0, Math.min(1, page.exporter.progress)) : 0)
                height: parent.height
                radius: parent.radius
                color: AppPalette.accentBorder
            }
        }
    }

    Text {
        width: parent.width
        visible: page.exporter !== null && !page.exporter.busy && page.exporter.resultText.length > 0
        text: page.exporter ? page.exporter.resultText : ""
        wrapMode: Text.WordWrap
        color: page.exporter && page.exporter.resultOk ? AppPalette.text : AppPalette.dangerBorder
        font.pixelSize: Tokens.fontSm
    }

    KButton {
        visible: page.exporter !== null && !page.exporter.busy && page.exporter.resultOk && page.exporter.resultFiles.length > 0
                 && Qt.platform.os !== "android"
        text: qsTr("Show in folder")
        onClicked: if (page.coreApi) page.coreApi.revealInFolder(page.exporter.resultFiles[0])
    }

    // ── About the map ─────────────────────────────────────────────────────
    Spacer { size: Tokens.spaceXl }

    KIsland {
        FieldRow {
            label: qsTr("Title")
            placeholder: qsTr("Map")
            text: page.exporter ? page.exporter.title : ""
            onEdited: function(v) { if (page.exporter) page.exporter.title = v }
        }
        FieldRow {
            label: qsTr("Author")
            placeholder: qsTr("Name or nickname")
            toolTipText: qsTr("Written into the files as the author. Leave it empty to stay anonymous.")
            text: page.exporter ? page.exporter.authorName : ""
            onEdited: function(v) { if (page.exporter) page.exporter.authorName = v }
        }
        FieldRow {
            label: qsTr("Contact")
            placeholder: qsTr("E-mail or site, optional")
            toolTipText: qsTr("Written into the files: everyone who gets the map will see it.")
            text: page.exporter ? page.exporter.authorContact : ""
            onEdited: function(v) { if (page.exporter) page.exporter.authorContact = v }
        }
        KIslandRow {
            label: qsTr("Licence")
            labelColor: page._bright
            toolTipText: qsTr("What others may do with the map. The data is yours; the licence is your permission.")

            KCombo {
                width: page.fieldW
                model: page.exporter ? page.exporter.licenseOptions.map(function(o) { return o.name }) : []
                currentIndex: page.licenseIndex()
                onActivated: function(idx) {
                    if (page.exporter) page.exporter.licenseId = page.exporter.licenseOptions[idx].id
                }
            }
        }
    }

    Note {
        text: {
            var id = page.exporter ? page.exporter.licenseId : ""
            var what = id === "CC-BY-4.0"
                ? qsTr("Anyone may copy, change and use the map, also commercially, if they name you as the author.")
                : id === "CC0-1.0"
                  ? qsTr("Anyone may do anything with the map, without naming you. This cannot be taken back for copies already shared.")
                  : qsTr("Others may look at the map; copying, passing on or changing it needs your permission.")
            return what + " " + qsTr("Every file also says: NOT FOR NAVIGATION, depths from an amateur survey, use at your own risk.")
        }
    }

    // ── What to save ──────────────────────────────────────────────────────
    Spacer {}

    KIsland {
        KIslandRow {
            label: qsTr("Side-scan mosaic")
            labelColor: page._bright
            interactive: true
            onClicked: mosaicSwitch.click()
            KSwitch {
                id: mosaicSwitch
                flat: true
                checked: page.exporter ? page.exporter.includeMosaic : true
                onToggled: if (page.exporter) page.exporter.includeMosaic = checked
            }
        }
        KIslandRow {
            label: qsTr("Mosaic detail")
            labelColor: page._bright
            open: mosaicSwitch.checked
            toolTipText: qsTr("Finer detail makes larger files and takes longer.")
            KCombo {
                width: page.fieldW
                model: page.exporter ? page.exporter.detailOptions.map(function(o) { return o.name }) : []
                currentIndex: page.detailIndex()
                onActivated: function(idx) {
                    if (page.exporter) page.exporter.detailZoom = page.exporter.detailOptions[idx].zoom
                }
            }
        }
        KIslandRow {
            label: qsTr("Depths and isobaths")
            labelColor: page._bright
            interactive: true
            onClicked: depthSwitch.click()
            KSwitch {
                id: depthSwitch
                flat: true
                checked: page.exporter ? page.exporter.includeDepth : true
                onToggled: if (page.exporter) page.exporter.includeDepth = checked
            }
        }
        KIslandRow {
            label: qsTr("Join soundings up to, m")
            labelColor: page._bright
            open: depthSwitch.checked
            toolTipText: qsTr("The depth map is drawn between soundings no farther apart than this; wider gaps stay empty. More than the distance between survey lines joins them.")
            KSpinBox {
                width: Math.round(140 * AppPalette.scale)
                height: Tokens.controlHMd
                from: 5; to: 500; stepSize: 5
                value: page.exporter ? Math.round(page.exporter.fillGapsM) : 50
                onValueModified: function(v) { if (page.exporter) page.exporter.fillGapsM = v }
            }
        }
        KIslandRow {
            label: qsTr("Isobaths every")
            labelColor: page._bright
            open: depthSwitch.checked
            toolTipText: qsTr("Also the step of the depth colours. Auto: 10 to 25 isobaths over the depths of the survey.")
            KCombo {
                width: page.fieldW
                model: page.exporter ? page.exporter.isobathStepOptions.map(function(o) { return o.name }) : []
                currentIndex: {
                    if (!page.exporter) return 0
                    var opts = page.exporter.isobathStepOptions
                    for (var i = 0; i < opts.length; ++i)
                        if (Math.abs(opts[i].step - page.exporter.isobathStepM) < 1e-6) return i
                    return 0
                }
                onActivated: function(idx) {
                    if (page.exporter) page.exporter.isobathStepM = page.exporter.isobathStepOptions[idx].step
                }
            }
        }
        KIslandRow {
            label: qsTr("Transducer depth, m")
            labelColor: page._bright
            open: depthSwitch.checked
            toolTipText: qsTr("How deep the transducer sits below the waterline. Added to every depth, so the map counts from the water surface; 0: depths from the transducer.")
            KSpinBox {
                width: Math.round(140 * AppPalette.scale)
                height: Tokens.controlHMd
                from: 0; to: 500; stepSize: 5
                divisor: 100; decimals: 2
                value: page.exporter ? Math.round(page.exporter.draftM * 100) : 0
                onValueModified: function(v) { if (page.exporter) page.exporter.draftM = v / 100 }
            }
        }
    }

    // ── Formats ───────────────────────────────────────────────────────────
    Spacer {}

    KIsland {
        KIslandRow {
            label: qsTr("KOGGER map (.kmap)")
            labelColor: page._bright
            interactive: true
            toolTipText: qsTr("The file to share: it opens in KoggerApp as a layer under the boat.")
            onClicked: kmapSwitch.click()
            KSwitch {
                id: kmapSwitch
                flat: true
                checked: page.exporter ? page.exporter.writeKmap : true
                onToggled: if (page.exporter) page.exporter.writeKmap = checked
            }
        }
    }

    Text {
        width: page.width
        text: qsTr("For other programs")
        color: AppPalette.textMuted
        font.pixelSize: Tokens.fontSm
    }

    KIsland {
        KIslandRow {
            label: qsTr("Google Earth (KMZ)")
            labelColor: page._bright
            interactive: true
            onClicked: kmzSwitch.click()
            KSwitch {
                id: kmzSwitch
                flat: true
                checked: page.exporter ? page.exporter.writeKmz : true
                onToggled: if (page.exporter) page.exporter.writeKmz = checked
            }
        }
        KIslandRow {
            label: qsTr("GIS (GeoTIFF)")
            labelColor: page._bright
            interactive: true
            onClicked: tiffSwitch.click()
            KSwitch {
                id: tiffSwitch
                flat: true
                checked: page.exporter ? page.exporter.writeGeoTiff : true
                onToggled: if (page.exporter) page.exporter.writeGeoTiff = checked
            }
        }
    }

    // ── Privacy ───────────────────────────────────────────────────────────
    Spacer {}

    KIsland {
        KIslandRow {
            label: qsTr("Without the track and times")
            labelColor: page._bright
            interactive: true
            toolTipText: qsTr("The boat's track is left out and the survey's date is given as a month only.")
            onClicked: privacySwitch.click()
            KSwitch {
                id: privacySwitch
                flat: true
                checked: page.exporter ? page.exporter.hideTrackAndTime : true
                onToggled: if (page.exporter) page.exporter.hideTrackAndTime = checked
            }
        }
    }

    Note {
        text: qsTr("Even without the track, a map shows where you have been. Do not share surveys of places that must stay private, and check whether surveying and publishing depths is allowed in those waters.")
    }
}
