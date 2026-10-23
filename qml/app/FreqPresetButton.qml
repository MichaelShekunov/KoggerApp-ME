import QtQuick 2.15
import kqml_types 1.0

// One of a device's two frequency buttons in its pill (HotActionsPanel). The circle shows the beam, not a number
// (the owner, 2026-10-23): slot 0 (700 kHz) the narrow one, slot 1 (450 kHz) the wide one; the frequency is in the
// tooltip. When the sonar keeps presets in its own flash (ID_PRESET, firmware 11.21+) and this button's slot is used:
// its name beside the circle, and a tap recalls the slot. Otherwise a tap sets `khz`, as before. On a sonar that
// keeps presets a long press (holdMs) asks to save the sonar's current settings into the slot (saveAsked); on any
// other a long press is a tap. The texts come from the panel, so its translations stay where they are.
KCircleIconButton {
    id: btn

    property var dev: null
    property int slot: 0
    property int khz: 700
    property real uiScale: 1
    property int holdMs: 600
    property string tipFixed: ""
    property string tipPresetFormat: "%1: %2 kHz"
    property bool labelLeft: true

    signal done()              // the pill closes
    signal saveAsked(int slot)

    readonly property var preset: {
        if (!dev || !dev.presetsKnown || !dev.presetSlots || dev.presetSlots.length <= slot) return null
        var s = dev.presetSlots[slot]
        return (s && s.used) ? s : null
    }
    // The preset in effect: its frequency and pulses as the sonar reports them.
    readonly property bool active: preset ? !!(dev && dev.transFreq === preset.freqKhz && dev.transPulse === preset.pulses)
                                          : !!(dev && dev.transFreq === khz)
    readonly property bool held: _held

    iconSource: slot === 0 ? "qrc:/icons/ui/beam-narrow.svg" : "qrc:/icons/ui/beam-wide.svg"
    iconTintColor: AppPalette.text
    iconPixelSize: Math.round(Math.min(width, height) * 0.66)   // a thin drawing: more of the circle than the usual 56 %
    fillColor:        active ? AppPalette.accentBgStrong : AppPalette.card
    fillHoverColor:   active ? AppPalette.accentBgHover : AppPalette.cardHover
    fillPressedColor: active ? AppPalette.accentBgPressed : AppPalette.bgDeep
    borderColor:      active ? AppPalette.accentBorder : AppPalette.border
    toolTipText: preset ? tipPresetFormat.arg(preset.name).arg(preset.freqKhz) : tipFixed

    property bool _held: false
    Timer {
        id: holdTimer
        interval: btn.holdMs
        onTriggered: {
            btn._held = true
            btn.saveAsked(btn.slot)
        }
    }
    onPressStarted: {
        _held = false
        if (dev && dev.presetsKnown)
            holdTimer.restart()
    }
    onPressEnded: holdTimer.stop()
    onPressCanceled: holdTimer.stop()
    onClicked: {
        if (_held) {          // the release after a long press: the save was asked, nothing else
            _held = false
            return
        }
        if (preset)
            dev.presetRecall(slot)
        else if (dev)
            dev.transFreq = khz
        done()
    }

    // The preset's name beside the button: the circle has room for the beam only.
    Rectangle {
        id: nameLabel
        objectName: "presetNameLabel"
        visible: !!btn.preset
        x: btn.labelLeft ? -width - Math.round(6 * btn.uiScale) : btn.width + Math.round(6 * btn.uiScale)
        anchors.verticalCenter: parent.verticalCenter
        width: nameText.implicitWidth + Math.round(14 * btn.uiScale)
        height: nameText.implicitHeight + Math.round(8 * btn.uiScale)
        radius: height / 2
        color: AppPalette.bg
        border.width: 1
        border.color: btn.active ? AppPalette.accentBorder : AppPalette.border
        Text {
            id: nameText
            objectName: "presetNameText"
            anchors.centerIn: parent
            text: btn.preset ? btn.preset.name : ""
            color: AppPalette.text
            font.pixelSize: Math.round(12 * btn.uiScale)
            font.bold: btn.active
        }
    }
}
