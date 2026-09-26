import QtQuick
import QtQuick.Controls
import kqml_types 1.0

// Boat mission panel for the 3D pane: the autopilot's state, the waypoint plan and the commands.
// It edits the plan held by `vehicle` (MavVehicle, context property "boat"). Points are added by
// tapping the map while addMode is on (Pane3DWindow routes view.mapTapped here) and moved by
// dragging their markers in BoatMissionLayer. Commands that set the boat moving ask for a second
// tap; HOLD never does, it is the way to stop.
Rectangle {
    id: panel

    property var vehicle: null
    property bool addMode: false
    property int selectedIndex: -1
    property real maxHeight: Math.round(600 * _s)
    signal closeRequested()

    readonly property real _s: AppPalette.scale
    readonly property real pad: Tokens.spaceMd
    readonly property bool linked: !!vehicle && vehicle.connected

    width: Math.round(310 * _s)
    height: Math.min(contentCol.implicitHeight + 2 * pad, maxHeight)
    radius: Tokens.radiusLg
    color: AppPalette.card
    border.width: 1
    border.color: AppPalette.border

    // Nothing under the panel may reach the scene (taps would pan it or add points).
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: function(wheel) { wheel.accepted = true }
    }

    function gpsText(fix) {
        switch (fix) {
        case 0: case 1: return qsTr("no fix")
        case 2: return "2D"
        case 3: return "3D"
        case 4: return "DGPS"
        case 5: return "RTK float"
        case 6: return "RTK fixed"
        }
        return String(fix)
    }
    function metres(m) {
        if (!isFinite(m) || m < 0)
            return "—"
        return m >= 1000 ? (m / 1000).toFixed(2) + " " + qsTr("km") : Math.round(m) + " " + qsTr("m")
    }

    component SectionTitle: Text {
        width: parent ? parent.width : implicitWidth
        color: AppPalette.textSecond
        font.pixelSize: Tokens.fontSm
        font.bold: true
        topPadding: Tokens.spaceXs
    }

    component StatusText: Text {
        color: AppPalette.text
        font.pixelSize: Tokens.fontSm
        elide: Text.ElideRight
    }

    component PanelButton: KButton {
        height: Tokens.controlHLg
        fontPixelSize: Tokens.fontMd
        normalBg: AppPalette.controlRaised
        hoverBg: Qt.lighter(AppPalette.controlRaised, 1.2)
    }

    // Two taps within three seconds: the first arms it, the second acts.
    component ConfirmButton: PanelButton {
        id: cb
        property string label: ""
        property bool pending: false
        signal confirmed()
        text: pending ? qsTr("Tap again") : label
        danger: pending
        onEnabledChanged: if (!enabled) pending = false
        onClicked: {
            if (pending) {
                pending = false
                confirmTimer.stop()
                cb.confirmed()
            } else {
                pending = true
                confirmTimer.restart()
            }
        }
        Timer { id: confirmTimer; interval: 3000; onTriggered: cb.pending = false }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: panel.pad
        contentHeight: contentCol.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height

        Column {
            id: contentCol
            width: flick.width
            spacing: Tokens.spaceSm

            // ── header ──────────────────────────────────────────────────────────
            Item {
                width: parent.width
                height: Tokens.controlHMd
                Image {
                    id: boatIcon
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    source: "qrc:/icons/ui/speedboat.svg"
                    width: Tokens.iconMd
                    height: Tokens.iconMd
                    sourceSize.width: width * 2
                    sourceSize.height: height * 2
                }
                Text {
                    id: titleText
                    anchors.left: boatIcon.right
                    anchors.leftMargin: Tokens.spaceSm
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Boat")
                    color: AppPalette.textStrong
                    font.pixelSize: Tokens.fontLg
                    font.bold: true
                }
                Rectangle {
                    id: linkChip
                    anchors.left: titleText.right
                    anchors.leftMargin: Tokens.spaceSm
                    anchors.verticalCenter: parent.verticalCenter
                    height: Math.round(22 * panel._s)
                    width: chipText.implicitWidth + 2 * Tokens.spaceMd
                    radius: height / 2
                    color: panel.linked ? AppPalette.linkOkBg : AppPalette.linkDownBg
                    border.width: 1
                    border.color: panel.linked ? AppPalette.linkOkBorder : AppPalette.linkDownBorder
                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        color: panel.linked ? AppPalette.linkOkText : AppPalette.linkDownText
                        font.pixelSize: Tokens.fontXs
                        font.bold: true
                        text: !panel.vehicle || !panel.vehicle.detected ? qsTr("no autopilot")
                             : panel.linked ? (panel.vehicle.modeName + (panel.vehicle.armed ? " · " + qsTr("ARMED") : ""))
                                            : qsTr("no link %1 s").arg(panel.vehicle.heartbeatAgeS)
                    }
                }
                KCircleIconButton {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: Tokens.controlHMd
                    height: Tokens.controlHMd
                    rounded: false
                    cornerRadius: Tokens.radiusMd
                    iconSource: "qrc:/icons/ui/x.svg"
                    iconTintColor: AppPalette.text
                    fillColor: "transparent"
                    fillHoverColor: AppPalette.cardHover
                    borderWidth: 0
                    toolTipText: qsTr("Close")
                    onClicked: panel.closeRequested()
                }
            }

            // ── status ──────────────────────────────────────────────────────────
            Grid {
                width: parent.width
                columns: 2
                columnSpacing: Tokens.spaceMd
                rowSpacing: Tokens.spaceXxs
                visible: !!panel.vehicle && panel.vehicle.detected
                readonly property real colW: (width - columnSpacing) / 2
                StatusText {
                    width: parent.colW
                    text: qsTr("GPS: %1, %2 sat").arg(panel.gpsText(panel.vehicle ? panel.vehicle.gpsFix : 0))
                                                 .arg(panel.vehicle && panel.vehicle.gpsSats >= 0 ? panel.vehicle.gpsSats : "—")
                    color: panel.vehicle && panel.vehicle.gpsFix >= 3 ? AppPalette.text : AppPalette.dangerText
                }
                StatusText {
                    width: parent.colW
                    text: qsTr("Battery: %1").arg(panel.vehicle && panel.vehicle.batteryVoltage > 0
                                                  ? panel.vehicle.batteryVoltage.toFixed(1) + " V"
                                                    + (panel.vehicle.batteryRemaining >= 0 ? ", " + panel.vehicle.batteryRemaining + "%" : "")
                                                  : "—")
                }
                StatusText {
                    width: parent.colW
                    text: qsTr("Speed: %1 m/s").arg(panel.vehicle ? panel.vehicle.groundSpeed.toFixed(1) : "—")
                }
                StatusText {
                    width: parent.colW
                    text: qsTr("Home: %1").arg(panel.vehicle && panel.vehicle.homeValid
                                               ? panel.metres(panel.vehicle.distanceM(panel.vehicle.latitude, panel.vehicle.longitude,
                                                                                      panel.vehicle.homeLatitude, panel.vehicle.homeLongitude))
                                               : qsTr("not set"))
                    color: panel.vehicle && panel.vehicle.homeValid ? AppPalette.text : AppPalette.dangerText
                }
                StatusText {
                    width: parent.colW
                    visible: !!panel.vehicle && panel.vehicle.currentPlanIndex >= 0 && panel.vehicle.mode === 10
                    text: qsTr("To point %1: %2").arg(panel.vehicle ? panel.vehicle.currentPlanIndex + 1 : 0)
                                                 .arg(panel.metres(panel.vehicle ? panel.vehicle.wpDistance : -1))
                }
            }
            StatusText {
                width: parent.width
                visible: !!panel.vehicle && panel.vehicle.detected
                color: AppPalette.textMuted
                font.pixelSize: Tokens.fontXs
                text: panel.vehicle ? (panel.vehicle.autopilotText + (panel.vehicle.firmwareText.length ? " " + panel.vehicle.firmwareText : "")) : ""
            }
            StatusText {
                width: parent.width
                visible: !panel.vehicle || !panel.vehicle.detected
                wrapMode: Text.WordWrap
                color: AppPalette.textSecond
                text: qsTr("No MAVLink autopilot heard yet. It appears here as soon as its heartbeat arrives over any open link.")
            }

            Rectangle { width: parent.width; height: 1; color: AppPalette.separator }

            // ── route ───────────────────────────────────────────────────────────
            SectionTitle {
                text: panel.vehicle && panel.vehicle.planCount > 0
                      ? qsTr("Route: %1 points, %2").arg(panel.vehicle.planCount).arg(panel.metres(panel.vehicle.planLengthM))
                      : qsTr("Route")
            }

            PanelButton {
                width: parent.width
                text: panel.addMode ? qsTr("Adding points: on") : qsTr("Add points")
                normalBg: panel.addMode ? AppPalette.accentBgStrong : AppPalette.controlRaised
                normalBorder: panel.addMode ? AppPalette.accentBorder : AppPalette.border
                borderWidth: panel.addMode ? 2 : Tokens.cardBorderWidth
                onClicked: panel.addMode = !panel.addMode
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                color: panel.addMode ? AppPalette.accentText : AppPalette.textMuted
                font.pixelSize: Tokens.fontSm
                text: panel.addMode
                      ? qsTr("Tap the map to add a point, drag a numbered point to move it. H is home, where the boat returns.")
                      : qsTr("Turn on \"Add points\" to place points by tapping the map.")
            }

            Column {
                width: parent.width
                spacing: Tokens.spaceXxs
                Repeater {
                    model: panel.vehicle ? panel.vehicle.plan : []
                    delegate: Rectangle {
                        id: row
                        required property int index
                        required property var modelData
                        readonly property bool selected: index === panel.selectedIndex
                        readonly property bool target: !!panel.vehicle && panel.vehicle.mode === 10
                                                       && panel.vehicle.currentPlanIndex === index
                        width: parent ? parent.width : 0
                        height: Tokens.controlHMd
                        radius: Tokens.radiusMd
                        color: selected ? AppPalette.accentBg : (rowMouse.containsMouse ? AppPalette.cardHover : "transparent")
                        border.width: target ? 2 : 0
                        border.color: AppPalette.accentBorder

                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: panel.selectedIndex = (panel.selectedIndex === row.index ? -1 : row.index)
                        }
                        Rectangle {
                            id: badge
                            anchors.left: parent.left
                            anchors.leftMargin: Tokens.spaceXs
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.round(22 * panel._s)
                            height: width
                            radius: width / 2
                            color: row.target ? AppPalette.accent : "#E61B1F24"
                            Text {
                                anchors.centerIn: parent
                                text: String(row.index + 1)
                                color: "#FFFFFF"
                                font.pixelSize: Tokens.fontXs
                                font.bold: true
                            }
                        }
                        Text {
                            anchors.left: badge.right
                            anchors.leftMargin: Tokens.spaceSm
                            anchors.right: delBtn.left
                            anchors.rightMargin: Tokens.spaceXs
                            anchors.verticalCenter: parent.verticalCenter
                            elide: Text.ElideRight
                            color: AppPalette.text
                            font.pixelSize: Tokens.fontSm
                            text: {
                                var v = panel.vehicle
                                if (!v) return ""
                                var d
                                if (row.index === 0)
                                    d = v.positionValid ? qsTr("%1 from the boat").arg(panel.metres(v.distanceM(v.latitude, v.longitude,
                                                                                                            row.modelData.lat, row.modelData.lon)))
                                                        : ""
                                else {
                                    var prev = v.plan[row.index - 1]
                                    d = qsTr("+%1").arg(panel.metres(v.distanceM(prev.lat, prev.lon, row.modelData.lat, row.modelData.lon)))
                                }
                                return row.modelData.lat.toFixed(6) + ", " + row.modelData.lon.toFixed(6) + (d.length ? "  " + d : "")
                            }
                        }
                        KCircleIconButton {
                            id: delBtn
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.round(26 * panel._s)
                            height: width
                            rounded: false
                            cornerRadius: Tokens.radiusSm
                            iconSource: "qrc:/icons/ui/x.svg"
                            iconTintColor: AppPalette.dangerText
                            fillColor: "transparent"
                            fillHoverColor: AppPalette.cardHover
                            borderWidth: 0
                            toolTipText: qsTr("Remove point")
                            onClicked: {
                                if (panel.selectedIndex === row.index) panel.selectedIndex = -1
                                panel.vehicle.removePoint(row.index)
                            }
                        }
                    }
                }
            }

            Text {
                width: parent.width
                text: qsTr("Speed on the route")
                color: AppPalette.textSecond
                font.pixelSize: Tokens.fontSm
            }
            KTabBar {
                width: parent.width
                fontPixelSize: Tokens.fontSm
                options: [
                    { label: qsTr("Boat"), value: 0 },
                    { label: "0.5", value: 0.5 },
                    { label: "1", value: 1 },
                    { label: "1.5", value: 1.5 },
                    { label: "2", value: 2 },
                    { label: "3", value: 3 }
                ]
                currentValue: panel.vehicle ? panel.vehicle.planSpeed : 0
                onValueSelected: function(v) { if (panel.vehicle) panel.vehicle.planSpeed = v }
            }

            KSwitch {
                width: parent.width
                rowHeight: Tokens.controlHLg
                fontPixelSize: Tokens.fontMd
                text: qsTr("Return home at the end")
                // a click replaces a `checked` binding, so the model value is pushed in explicitly
                readonly property bool want: !!panel.vehicle && panel.vehicle.planReturnHome
                onWantChanged: if (checked !== want) checked = want
                Component.onCompleted: checked = want
                onToggled: if (panel.vehicle) panel.vehicle.planReturnHome = checked
            }

            Row {
                width: parent.width
                spacing: Tokens.spaceSm
                readonly property real bw: (width - spacing) / 2
                PanelButton {
                    width: parent.bw
                    text: qsTr("Upload to boat")
                    enabled: panel.linked && panel.vehicle.planCount > 0
                             && panel.vehicle.transferState !== 1 && panel.vehicle.transferState !== 2
                    onClicked: panel.vehicle.uploadPlan()
                }
                ConfirmButton {
                    width: parent.bw
                    label: qsTr("Clear route")
                    enabled: !!panel.vehicle && panel.vehicle.planCount > 0
                    onConfirmed: { panel.selectedIndex = -1; panel.vehicle.clearPlan() }
                }
            }

            // transfer state
            Column {
                width: parent.width
                spacing: Tokens.spaceXxs
                visible: !!panel.vehicle && (panel.vehicle.transferState !== 0 || panel.vehicle.planOnVehicle)
                Rectangle {
                    width: parent.width
                    height: Math.max(3, Math.round(3 * panel._s))
                    radius: height / 2
                    color: AppPalette.trackOff
                    visible: panel.vehicle && (panel.vehicle.transferState === 1 || panel.vehicle.transferState === 2)
                    Rectangle {
                        width: parent.width * (panel.vehicle ? panel.vehicle.transferProgress : 0) / 100
                        height: parent.height
                        radius: parent.radius
                        color: AppPalette.accent
                    }
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: Tokens.fontSm
                    color: !panel.vehicle ? AppPalette.text
                         : panel.vehicle.transferState === 4 ? AppPalette.dangerText
                         : panel.vehicle.planOnVehicle ? AppPalette.linkOkText : AppPalette.text
                    text: !panel.vehicle ? ""
                         : (panel.vehicle.transferState === 0 && panel.vehicle.planOnVehicle) ? qsTr("The boat holds this route")
                         : panel.vehicle.transferText
                }
            }

            Rectangle { width: parent.width; height: 1; color: AppPalette.separator }

            // ── control ─────────────────────────────────────────────────────────
            SectionTitle { text: qsTr("Control") }

            // startBlocker() is a function: re-read it whenever something it depends on changes
            property int _blockerTick: 0
            Connections {
                target: panel.vehicle
                ignoreUnknownSignals: true
                function onLinkChanged() { contentCol._blockerTick++ }
                function onTransferChanged() { contentCol._blockerTick++ }
                function onGpsChanged() { contentCol._blockerTick++ }
                function onHomeChanged() { contentCol._blockerTick++ }
                function onBusyChanged() { contentCol._blockerTick++ }
                function onPlanChanged() { contentCol._blockerTick++ }
            }
            readonly property string startWhy: { _blockerTick; return panel.vehicle ? panel.vehicle.startBlocker() : qsTr("No autopilot") }

            Text {
                width: parent.width
                visible: contentCol.startWhy.length > 0
                wrapMode: Text.WordWrap
                color: AppPalette.textMuted
                font.pixelSize: Tokens.fontSm
                text: qsTr("Start: %1").arg(contentCol.startWhy)
            }

            Row {
                width: parent.width
                spacing: Tokens.spaceSm
                readonly property real bw: (width - spacing) / 2
                ConfirmButton {
                    width: parent.bw
                    label: qsTr("Start mission")
                    enabled: contentCol.startWhy.length === 0
                    normalBg: enabled ? AppPalette.accentBgStrong : AppPalette.controlRaised
                    normalBorder: AppPalette.accentBorder
                    onConfirmed: panel.vehicle.startMission()
                }
                PanelButton {
                    width: parent.bw
                    text: qsTr("HOLD")
                    enabled: panel.linked
                    bold: true
                    toolTipText: qsTr("Stop and hold position")
                    onClicked: panel.vehicle.hold()
                }
            }
            Row {
                width: parent.width
                spacing: Tokens.spaceSm
                readonly property real bw: (width - spacing) / 2
                ConfirmButton {
                    width: parent.bw
                    label: qsTr("Return home")
                    enabled: panel.linked && !!panel.vehicle && panel.vehicle.homeValid
                    onConfirmed: panel.vehicle.returnHome()
                }
                ConfirmButton {
                    width: parent.bw
                    label: panel.vehicle && panel.vehicle.armed ? qsTr("Disarm") : qsTr("Arm")
                    enabled: panel.linked
                    onConfirmed: {
                        if (panel.vehicle.armed) panel.vehicle.disarm()
                        else panel.vehicle.arm()
                    }
                }
            }
            Text {
                width: parent.width
                visible: !!panel.vehicle && panel.vehicle.busy
                color: AppPalette.accentText
                font.pixelSize: Tokens.fontSm
                text: panel.vehicle ? panel.vehicle.busyText + "…" : ""
            }

            KSwitch {
                width: parent.width
                rowHeight: Tokens.controlHLg
                fontPixelSize: Tokens.fontSm
                text: qsTr("Heartbeat to the boat (link-loss failsafe)")
                toolTipText: qsTr("While on, the autopilot counts this head unit as its ground station: losing it for FS_GCS_TIMEOUT + FS_TIMEOUT seconds starts the failsafe (RTL outside AUTO).")
                readonly property bool want: !!panel.vehicle && panel.vehicle.controlEnabled
                onWantChanged: if (checked !== want) checked = want
                Component.onCompleted: checked = want
                onToggled: if (panel.vehicle) panel.vehicle.controlEnabled = checked
            }

            // ── messages ────────────────────────────────────────────────────────
            SectionTitle {
                text: qsTr("Messages")
                visible: !!panel.vehicle && panel.vehicle.messages.length > 0
            }
            Repeater {
                model: panel.vehicle ? panel.vehicle.messages.slice(0, 5) : []
                delegate: Text {
                    required property var modelData
                    width: contentCol.width
                    elide: Text.ElideRight
                    color: AppPalette.textSecond
                    font.pixelSize: Tokens.fontXs
                    text: modelData
                }
            }
        }
    }
}
