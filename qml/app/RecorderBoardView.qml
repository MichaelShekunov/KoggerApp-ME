import QtQuick 2.15
import kqml_types 1.0

// A Recorder drawn as its board (docs/RECORDER_PORTS.md): each connector a block beside it with its contacts in board
// order and their live values; a tap on a block opens its function's menu below, and each contact of a universal
// connector its role's menu. Read-only until the Recorder's firmware takes settings (the port spec's phases 2 and 3);
// the settings the firmware does not have yet are shown as reserved.
Column {
    id: view

    property var dev: null
    property var store: null
    readonly property var buses: dev ? dev.portBuses : []
    property string selected: ""
    property int openPin: -1          // the contact whose menu is open; -1: the bus's first active one, -2: none

    spacing: Tokens.spaceMd

    // The drawing is laid out on 680 x 500 units. Below 560 px its rows would get shorter than the text: it keeps
    // 560 px and scrolls sideways instead.
    readonly property real drawingWidth: Math.max(width, 560)
    readonly property real s: drawingWidth / 680
    readonly property real fontPx: Math.max(10, 11.5 * s)

    // RECORDER Mini v4.0 as the port spec's ID_BUS V1 table gives it (pads and nets from the PcbDoc): the side, the place
    // along it going clockwise round the board, and the contacts in clockwise order, contact 1 first. A label not in it
    // gets a free place and its signal pins, GND and its supply. The device does not send V1 yet (reserved, not
    // scheduled); when it does, it replaces this table.
    readonly property var knownLayout: ({
        "GNSS":   { side: "top",   slot: 0, order: ["TX", "RX", "GND", "+5V"] },
        "TELEM":  { side: "top",   slot: 1, order: ["TX", "RX", "GND", "+5V"] },
        "IN1,2":  { side: "right", slot: 0, order: ["IN1", "IN2", "GND", "+5V"] },
        "OUTS":   { side: "right", slot: 1, order: ["O3", "O2", "GND", "O1"] },
        "SONAR2": { side: "right", slot: 2, order: ["TX", "RX", "GND", "VBAT"] },
        "SONAR1": { side: "left",  slot: 0, order: ["TX", "RX", "GND", "VBAT"] },
        "IN3,4":  { side: "left",  slot: 1, order: ["IN3", "IN4", "GND", "+5V"] },
        "I2C":    { side: "left",  slot: 2, order: ["SCL", "SDA", "GND", "+5V"] }
    })
    readonly property var places: [{ side: "top", slot: 0 }, { side: "top", slot: 1 }, { side: "right", slot: 0 },
                                   { side: "right", slot: 1 }, { side: "right", slot: 2 }, { side: "left", slot: 0 },
                                   { side: "left", slot: 1 }, { side: "left", slot: 2 }]

    // Clockwise slots: the top runs left to right, the right side top to bottom, the left side bottom to top.
    function geometry(side, slot) {
        if (side === "top")
            return { bx: slot === 0 ? 150 : 350, by: 6, bw: 180, cx: slot === 0 ? 270 : 354, cy: 122, cw: 56, ch: 16 }
        var row = side === "left" ? 2 - slot : slot
        var y = [118, 216, 314][row]
        var cy = [150, 220, 290][row]
        return side === "left" ? { bx: 10, by: y, bw: 200, cx: 252, cy: cy, cw: 16, ch: 48 }
                               : { bx: 470, by: y, bw: 200, cx: 412, cy: cy, cw: 16, ch: 48 }
    }

    readonly property var blocks: {
        var out = [], used = {}
        for (var i = 0; i < buses.length; ++i) {
            var k = knownLayout[buses[i].label]
            if (k)
                used[k.side + k.slot] = true
        }
        var free = places.filter(function(p) { return !used[p.side + p.slot] })
        for (var j = 0; j < buses.length && j < 8; ++j) {
            var bus = buses[j]
            var lay = knownLayout[bus.label] || free.shift()
            if (!lay)
                continue
            var supplyPin = bus.supply === 1 ? "VBAT" : bus.supply === 2 ? "+5V" : ""
            // a block lists its contacts from the top down: on the left side clockwise is bottom to top
            var order = lay.order ? (lay.side === "left" ? lay.order.slice().reverse() : lay.order) : []
            var contacts = [], seen = {}
            for (var o = 0; o < order.length; ++o) {
                var pin = null
                for (var q = 0; q < bus.pins.length; ++q)
                    if (bus.pins[q].label === order[o])
                        pin = bus.pins[q]
                if (pin) {
                    contacts.push({ name: pin.label, pin: pin })
                    seen[pin.id] = true
                } else {
                    contacts.push({ name: order[o], power: order[o] })
                }
            }
            for (var r = 0; r < bus.pins.length; ++r)
                if (!seen[bus.pins[r].id])
                    contacts.push({ name: bus.pins[r].label, pin: bus.pins[r] })
            if (order.length === 0) {
                contacts.push({ name: "GND", power: "GND" })
                if (supplyPin)
                    contacts.push({ name: supplyPin, power: supplyPin })
            }
            var g = geometry(lay.side, lay.slot)
            g.side = lay.side
            g.bus = bus
            g.contacts = contacts
            g.bh = 22 + contacts.length * 17 + 6
            out.push(g)
        }
        return out
    }

    readonly property var current: {
        for (var i = 0; i < buses.length; ++i)
            if (buses[i].label === selected)
                return buses[i]
        for (var j = 0; j < buses.length; ++j)
            if (buses[j].label === "SONAR1")
                return buses[j]
        return buses.length ? buses[0] : null
    }

    // ── words ──
    function functionShort(f) { return [qsTr("contacts"), qsTr("sonar A"), qsTr("sonar B"), qsTr("telemetry"), qsTr("navigation")][f] || ("#" + f) }
    function functionLong(f) {
        return [qsTr("Separate contacts"), qsTr("Sonar port A"), qsTr("Sonar port B"), qsTr("Telemetry (Wi-Fi bridge)"),
                qsTr("GNSS (navigation)")][f] || qsTr("Function %1").arg(f)
    }
    function uartName(u) { return ["", "USART1", "USART2", "USART3", "LPUART1"][u] || "" }
    function volts(mv) { return (mv / 1000).toFixed(2) + qsTr(" V") }
    function baudText(b) {
        if (!(b > 0))
            return qsTr("auto")
        return b >= 1000000 ? qsTr("%1 Mbaud").arg(Math.round(b / 100000) / 10) : qsTr("%1 baud").arg(b)
    }
    function blockHeader(bus) {
        var t = bus.label + " · " + functionShort(bus.function)
        if (bus.function !== 0)
            t += " · " + baudText(bus.baud)
        return t
    }
    function contactValue(bus, c) {
        if (c.power) {
            if (c.power === "VBAT")
                return dev && dev.voltage0Mv >= 0 ? volts(dev.voltage0Mv) : ""
            if (c.power === "+5V")
                return dev && dev.voltage1Mv >= 0 ? volts(dev.voltage1Mv) : ""
            return ""
        }
        var p = c.pin
        if (bus.function !== 0)
            return p.id === bus.txPin ? qsTr("transmit") : p.id === bus.rxPin ? qsTr("receive") : qsTr("UART")
        switch (p.role) {
        case 3: return qsTr("PWM OUT%1 · %2 µs").arg(p.param + 1).arg(p.value)
        case 4: return qsTr("input · %1").arg(p.value)
        case 5: return qsTr("output · %1").arg(p.param)
        case 6: return qsTr("ADC · %1").arg(volts(p.value))
        case 0: return qsTr("off")
        }
        return qsTr("role %1").arg(p.role)
    }
    function roleName(r) {
        return [qsTr("Off"), qsTr("UART"), qsTr("I2C"), qsTr("PWM (servo)"), qsTr("Digital input"), qsTr("Digital output"),
                qsTr("ADC (0–3.3 V)")][r] || qsTr("Role %1").arg(r)
    }
    function blockColor(bus) {
        if (bus.function !== 0)
            return AppPalette.accent
        for (var i = 0; i < bus.pins.length; ++i)
            if (bus.pins[i].role !== 0)
                return AppPalette.linkIdleBorder
        return AppPalette.border
    }
    function supplyText(s) {
        return s === 1 ? qsTr("battery voltage (VBAT), up to about 30 V") : s === 2 ? qsTr("+5.2 V") : qsTr("none")
    }
    function linkState(f) {        // a device port's source as the recorder's status reports it
        if (!dev || !dev.recorderStatusValid)
            return ""
        var bit = f === 1 ? 1 : f === 2 ? 2 : f === 4 ? 4 : 0
        if (!bit)
            return ""
        return (dev.recorderStatusFlags & bit) ? qsTr("data coming in")
             : (dev.recorderDegradedFlags & bit) ? qsTr("no data now") : qsTr("nothing connected yet")
    }
    function outSource(n) {        // what drives the logical output OUTn (ID_PWM_ROUTE, ID_SERVO_CONTROL)
        if (!dev)
            return ""
        var t = [dev.pwmRouteOut1, dev.pwmRouteOut2, dev.pwmRouteOut3][n]
        if (t !== 1)
            return qsTr("nothing")
        return dev.servoManual ? qsTr("the servo, held by hand") : dev.servoEnabled ? qsTr("the servo, scanning")
                                                                                    : qsTr("the servo, stopped")
    }
    function firstActivePin(bus) {
        for (var i = 0; i < bus.pins.length; ++i)
            if (bus.pins[i].role !== 0)
                return bus.pins[i].id
        return -2
    }
    readonly property int effectiveOpenPin: openPin === -1 && current ? firstActivePin(current) : openPin

    // ── the drawing ──
    Flickable {
        objectName: "boardDrawing"
        width: view.width
        height: drawing.height
        contentWidth: drawing.width
        contentHeight: drawing.height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentWidth > width
        clip: true

        Item {
            id: drawing
            width: view.drawingWidth
            height: 500 * view.s

            Canvas {
                id: wires
                anchors.fill: parent
                readonly property var blocksSeen: view.blocks
                onBlocksSeenChanged: requestPaint()
                onWidthChanged: requestPaint()
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    ctx.strokeStyle = AppPalette.textMuted
                    ctx.lineWidth = 1
                    var k = view.s
                    for (var i = 0; i < view.blocks.length; ++i) {
                        var b = view.blocks[i]
                        var ax = b.side === "left" ? b.bx + b.bw : b.side === "right" ? b.bx : b.bx + b.bw / 2
                        var ay = b.side === "top" ? b.by + b.bh : b.by + b.bh / 2
                        var tx = b.side === "left" ? b.cx : b.side === "right" ? b.cx + b.cw : b.cx + b.cw / 2
                        var ty = b.side === "top" ? b.cy : b.cy + b.ch / 2
                        ctx.beginPath()
                        ctx.moveTo(ax * k, ay * k)
                        ctx.lineTo(tx * k, ty * k)
                        ctx.stroke()
                    }
                    ctx.beginPath()
                    ctx.moveTo(340 * k, 386 * k)
                    ctx.lineTo(340 * k, 408 * k)
                    ctx.stroke()
                }
            }

            Rectangle {        // the board
                x: 250 * view.s; y: 120 * view.s; width: 180 * view.s; height: 250 * view.s
                radius: 14 * view.s
                color: Qt.tint(AppPalette.card, Qt.rgba(0.18, 0.55, 0.30, 0.30))
                border.color: AppPalette.border
            }
            Rectangle {
                x: 310 * view.s; y: 200 * view.s; width: 60 * view.s; height: 60 * view.s; radius: 4 * view.s
                color: AppPalette.controlRaised; border.color: AppPalette.border
                Text { anchors.centerIn: parent; text: "MCU"; color: AppPalette.textSecond; font.pixelSize: view.fontPx }
            }
            Rectangle {
                x: 322 * view.s; y: 282 * view.s; width: 36 * view.s; height: 14 * view.s; radius: 6 * view.s
                color: AppPalette.controlRaised; border.color: AppPalette.border
            }
            Text { x: 322 * view.s; y: 300 * view.s; width: 36 * view.s; horizontalAlignment: Text.AlignHCenter
                   text: "USB-C"; color: AppPalette.textSecond; font.pixelSize: view.fontPx * 0.9 }
            Rectangle {        // the power connector and its readings
                x: 312 * view.s; y: 356 * view.s; width: 56 * view.s; height: 30 * view.s; radius: 4 * view.s
                color: AppPalette.linkIdleBg; border.color: AppPalette.linkIdleBorder
            }
            Rectangle {
                objectName: "boardPower"
                x: 250 * view.s; y: 408 * view.s; width: 180 * view.s; height: 62 * view.s; radius: 6 * view.s
                color: AppPalette.card; border.color: AppPalette.linkIdleBorder; border.width: 1.5
                Column {
                    anchors.centerIn: parent
                    spacing: 2
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("POWER 5–30 V")
                           color: AppPalette.textStrong; font.pixelSize: view.fontPx; font.bold: true }
                    Text { anchors.horizontalCenter: parent.horizontalCenter
                           text: qsTr("+BAT %1 · GND").arg(view.dev && view.dev.voltage0Mv >= 0 ? view.volts(view.dev.voltage0Mv) : "—")
                           color: AppPalette.textSecond; font.pixelSize: view.fontPx }
                    Text { anchors.horizontalCenter: parent.horizontalCenter
                           text: qsTr("5 V rail: %1").arg(view.dev && view.dev.voltage1Mv >= 0 ? view.volts(view.dev.voltage1Mv) : "—")
                           color: AppPalette.textSecond; font.pixelSize: view.fontPx }
                }
            }

            Repeater {        // the connectors on the board
                model: view.blocks
                delegate: Rectangle {
                    required property var modelData
                    x: modelData.cx * view.s; y: modelData.cy * view.s
                    width: modelData.cw * view.s; height: modelData.ch * view.s; radius: 2
                    color: AppPalette.card
                    border.color: view.blockColor(modelData.bus)
                    border.width: modelData.bus.label === (view.current ? view.current.label : "") ? 3 : 1.5
                }
            }

            Repeater {        // a block a connector: its contacts and their live values
                model: view.blocks
                delegate: Item {
                    id: blk
                    required property var modelData
                    readonly property bool isSelected: modelData.bus.label === (view.current ? view.current.label : "")
                    objectName: "boardBlock_" + modelData.bus.label
                    signal clicked()
                    onClicked: { view.selected = modelData.bus.label; view.openPin = -1 }
                    x: modelData.bx * view.s; y: modelData.by * view.s
                    width: modelData.bw * view.s; height: modelData.bh * view.s

                    Rectangle {
                        anchors.fill: parent
                        radius: 6 * view.s
                        color: blk.isSelected ? AppPalette.cardHover : AppPalette.card
                        border.color: view.blockColor(blk.modelData.bus)
                        border.width: blk.isSelected ? 3 : 1.5
                    }
                    Text {
                        x: 8 * view.s; y: 5 * view.s; width: parent.width - 16 * view.s
                        text: view.blockHeader(blk.modelData.bus)
                        color: AppPalette.textStrong; font.pixelSize: view.fontPx; font.bold: true
                        // the app's font and longer languages run wider than the block: shrink before eliding
                        fontSizeMode: Text.HorizontalFit; minimumPixelSize: 7
                        elide: Text.ElideRight
                    }
                    Repeater {
                        model: blk.modelData.contacts
                        delegate: Item {
                            required property var modelData
                            required property int index
                            x: 8 * view.s; y: (22 + index * 17) * view.s
                            width: blk.width - 16 * view.s; height: 17 * view.s
                            Text { anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                                   text: modelData.name; color: AppPalette.textSecond; font.pixelSize: view.fontPx }
                            Text { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                   text: view.contactValue(blk.modelData.bus, modelData)
                                   color: AppPalette.textStrong; font.pixelSize: view.fontPx }
                        }
                    }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: blk.clicked() }
                }
            }
        }
    }

    // ── the selected connector's menu ──
    component InfoRow: Item {
        id: ir
        property string label: ""
        property string value: ""
        property bool reserved: false
        width: parent ? parent.width : 0
        height: Math.max(labelText.implicitHeight, valueText.implicitHeight) + Tokens.spaceSm * 2
        Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: AppPalette.separator }
        Row {
            id: labelRow
            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
            spacing: Tokens.spaceSm
            Text { id: labelText; text: ir.label; color: AppPalette.textStrong; font.pixelSize: Tokens.fontMd }
            Rectangle {
                visible: ir.reserved
                anchors.verticalCenter: parent.verticalCenter
                width: tag.implicitWidth + Tokens.spaceSm * 2; height: tag.implicitHeight + 2; radius: height / 2
                color: AppPalette.accentBg
                Text { id: tag; anchors.centerIn: parent; text: qsTr("reserved"); color: AppPalette.accentText; font.pixelSize: Tokens.fontXs }
            }
        }
        Text {
            id: valueText
            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, parent.width - labelRow.width - Tokens.spaceMd)
            horizontalAlignment: Text.AlignRight
            text: ir.value.length ? ir.value : "—"
            color: AppPalette.textSecond; font.pixelSize: Tokens.fontMd
            elide: Text.ElideRight
        }
    }

    component MenuSection: Rectangle {
        default property alias rows: sectionColumn.data
        property string title: ""
        width: parent ? parent.width : 0
        height: sectionColumn.implicitHeight + Tokens.spaceMd * 2
        radius: Tokens.radiusSm
        color: AppPalette.bgDeep
        Column {
            id: sectionColumn
            x: Tokens.spaceMd; y: Tokens.spaceMd; width: parent.width - Tokens.spaceMd * 2
            Text { text: parent.parent.title; color: AppPalette.textStrong; font.pixelSize: Tokens.fontMd; font.bold: true
                   bottomPadding: Tokens.spaceXs }
        }
    }

    Column {
        id: menu
        objectName: "boardMenu"
        visible: !!view.current
        width: view.width
        spacing: Tokens.spaceSm
        readonly property var bus: view.current

        Text {
            text: menu.bus ? menu.bus.label : ""
            color: AppPalette.textStrong; font.pixelSize: Tokens.fontLg; font.bold: true
        }
        Text {
            width: parent.width; wrapMode: Text.WordWrap
            text: menu.bus ? qsTr("Power on the connector: %1").arg(view.supplyText(menu.bus.supply)) : ""
            color: AppPalette.textSecond; font.pixelSize: Tokens.fontMd
        }
        Text {
            width: parent.width; wrapMode: Text.WordWrap
            text: qsTr("This firmware of the Recorder only reports its connectors; changing them comes with its next phases. Rows marked reserved wait for the firmware too.")
            color: AppPalette.textMuted; font.pixelSize: Tokens.fontSm
        }
        InfoRow { label: qsTr("Function"); value: menu.bus ? view.functionLong(menu.bus.function) : "" }
        InfoRow {
            visible: !!menu.bus && (menu.bus.functionSaved !== menu.bus.function || menu.bus.uartSaved !== menu.bus.uart)
            label: qsTr("After a reboot"); value: menu.bus ? view.functionLong(menu.bus.functionSaved) : ""
        }
        Text {
            visible: !!menu.bus && menu.bus.supply === 1 && (menu.bus.function === 3 || menu.bus.function === 4)
            width: parent.width; wrapMode: Text.WordWrap
            text: qsTr("This connector carries the battery voltage, up to about 30 V: a 5 V GNSS receiver or Wi-Fi bridge on it burns. Connect one only through a converter.")
            color: AppPalette.dangerText; font.pixelSize: Tokens.fontMd
        }

        MenuSection {
            objectName: "menuSonar"
            visible: !!menu.bus && (menu.bus.function === 1 || menu.bus.function === 2)
            title: qsTr("Sonar port")
            InfoRow { label: qsTr("Link"); value: menu.bus ? view.linkState(menu.bus.function) : "" }
            InfoRow { label: qsTr("Speed"); value: menu.bus ? view.baudText(menu.bus.baud) : "" }
            InfoRow { label: qsTr("UART"); value: menu.bus ? view.uartName(menu.bus.uart) : "" }
            InfoRow { label: qsTr("Protocol"); reserved: true }
            InfoRow { label: qsTr("Ping sync"); reserved: true }
            InfoRow { label: qsTr("Write to the log"); reserved: true }
            InfoRow { label: qsTr("Pass on to the host"); reserved: true }
            InfoRow { label: qsTr("Traffic and errors"); reserved: true }
        }
        MenuSection {
            objectName: "menuTelemetry"
            visible: !!menu.bus && menu.bus.function === 3
            title: qsTr("Telemetry")
            InfoRow { label: qsTr("Speed"); value: menu.bus ? view.baudText(menu.bus.baud) : "" }
            InfoRow { label: qsTr("UART"); value: menu.bus ? view.uartName(menu.bus.uart) : "" }
            InfoRow { label: qsTr("What goes to the host"); reserved: true }
            InfoRow { label: qsTr("Channel load"); reserved: true }
        }
        MenuSection {
            objectName: "menuNav"
            visible: !!menu.bus && menu.bus.function === 4
            title: qsTr("Navigation")
            InfoRow { label: qsTr("Link"); value: view.linkState(4) }
            InfoRow { label: qsTr("Speed"); value: menu.bus ? view.baudText(menu.bus.baud) : "" }
            InfoRow { label: qsTr("UART"); value: menu.bus ? view.uartName(menu.bus.uart) : "" }
            InfoRow { label: qsTr("Protocol"); reserved: true }
            InfoRow { label: qsTr("Rate"); reserved: true }
            // the GNSS connector has no PPS line: a PPS wire goes to a universal input (its "Use as" row)
            InfoRow { label: qsTr("PPS"); value: qsTr("on a universal input") }
            InfoRow { label: qsTr("Fix and satellites"); reserved: true }
        }

        Repeater {        // a universal connector: each contact, its role's menu open one at a time
            model: menu.bus && menu.bus.function === 0 ? menu.bus.pins : []
            delegate: Column {
                id: pinItem
                required property var modelData
                readonly property bool open: view.effectiveOpenPin === modelData.id
                width: menu.width
                spacing: Tokens.spaceXs

                Item {
                    objectName: "boardPin_" + pinItem.modelData.label
                    signal clicked()
                    onClicked: view.openPin = pinItem.open ? -2 : pinItem.modelData.id
                    width: parent.width
                    height: pinHead.implicitHeight + Tokens.spaceSm * 2
                    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: AppPalette.separator }
                    Text {
                        id: pinHead
                        anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                        text: (pinItem.open ? "▾  " : "▸  ") + pinItem.modelData.label + "   " + pinItem.modelData.mcuPin
                        color: AppPalette.textStrong; font.pixelSize: Tokens.fontMd
                    }
                    Text {
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        text: view.roleName(pinItem.modelData.role)
                              + (pinItem.modelData.roleSaved !== pinItem.modelData.role
                                 ? qsTr(" → %1 after a reboot").arg(view.roleName(pinItem.modelData.roleSaved)) : "")
                        color: AppPalette.textSecond; font.pixelSize: Tokens.fontMd
                    }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: parent.clicked() }
                }

                MenuSection {
                    objectName: "menuPwm_" + pinItem.modelData.label
                    visible: pinItem.open && pinItem.modelData.role === 3
                    title: qsTr("Servo on %1").arg(pinItem.modelData.label)
                    InfoRow { label: qsTr("Output"); value: "OUT" + (pinItem.modelData.param + 1) }
                    InfoRow { label: qsTr("What drives it"); value: view.outSource(pinItem.modelData.param) }
                    InfoRow { label: qsTr("Pulse now"); value: qsTr("%1 µs").arg(pinItem.modelData.value) }
                    InfoRow { label: qsTr("Timer"); value: pinItem.modelData.timer > 0 ? "TIM" + pinItem.modelData.timer : "" }
                    KButton {
                        visible: !!(view.store && view.store.developerMode)
                        text: qsTr("Open the servo panel")
                        onClicked: view.store.setServoPanelShown(true)
                    }
                }
                MenuSection {
                    objectName: "menuAdc_" + pinItem.modelData.label
                    visible: pinItem.open && pinItem.modelData.role === 6
                    title: qsTr("ADC on %1").arg(pinItem.modelData.label)
                    InfoRow { label: qsTr("Now"); value: view.volts(pinItem.modelData.value) }
                    InfoRow { label: qsTr("Name"); reserved: true }
                    InfoRow { label: qsTr("Divider, scale"); reserved: true }
                    InfoRow { label: qsTr("Units"); reserved: true }
                    InfoRow { label: qsTr("Alarm threshold"); reserved: true }
                    InfoRow { label: qsTr("Write to the log"); reserved: true }
                }
                MenuSection {
                    objectName: "menuInput_" + pinItem.modelData.label
                    visible: pinItem.open && pinItem.modelData.role === 4
                    title: qsTr("Input %1").arg(pinItem.modelData.label)
                    InfoRow { label: qsTr("Now"); value: String(pinItem.modelData.value) }
                    InfoRow { label: qsTr("Pull"); value: [qsTr("none"), qsTr("up"), qsTr("down")][pinItem.modelData.param] || "" }
                    InfoRow { label: qsTr("Event"); reserved: true }
                    InfoRow { label: qsTr("Debounce"); reserved: true }
                    InfoRow { label: qsTr("Pulse counter"); reserved: true }
                    InfoRow { label: qsTr("Mark in the log on an event"); reserved: true }
                    InfoRow { label: qsTr("Use as PPS or ping trigger"); reserved: true }
                }
                MenuSection {
                    objectName: "menuOutput_" + pinItem.modelData.label
                    visible: pinItem.open && pinItem.modelData.role === 5
                    title: qsTr("Output %1").arg(pinItem.modelData.label)
                    InfoRow { label: qsTr("Level"); value: String(pinItem.modelData.param) }
                    InfoRow { label: qsTr("Mode"); reserved: true }
                    InfoRow { label: qsTr("Pulse length"); reserved: true }
                    InfoRow { label: qsTr("Toggle, pulse now"); reserved: true }
                }
                Text {
                    visible: pinItem.open && pinItem.modelData.role === 0
                    text: qsTr("The contact is off.")
                    color: AppPalette.textSecond; font.pixelSize: Tokens.fontMd
                }
            }
        }
        Text {
            visible: !!menu.bus && menu.bus.label === "I2C" && menu.bus.function === 0
            width: parent.width; wrapMode: Text.WordWrap
            text: qsTr("SCL and SDA have 10 kOhm pull-ups to 3.3 V on the board.")
            color: AppPalette.textSecond; font.pixelSize: Tokens.fontSm
        }
    }
}
