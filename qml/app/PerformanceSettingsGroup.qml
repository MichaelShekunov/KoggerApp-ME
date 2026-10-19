import QtQuick 2.15
import QtQuick.Layouts 1.15
import kqml_types 1.0

// Settings, Performance (the owner, 2026-10-18): switches for the speed-ups, each on by default, so one
// can be turned off to see whether it is behind something that looks wrong. The values live in
// WorkspaceStore (performancePrefs), which applies them to core at start and on every change.
SettingsGroup {
    id: performanceGroup

    property var store: null
    readonly property color _bright: AppPalette.isDark ? "#FFFFFF" : AppPalette.text

    preferredWidth: width
    title: qsTr("Performance")
    description: qsTr("Speed-ups of the mosaic. Each is on by default; turn one off to see whether it is behind something that looks wrong.")
    stateStore: store
    stateKey: "app.performance"

    KIsland {
        KIslandRow {
            KPinTag { key: "app:performance.allLevels" }
            label: qsTr("Paint all zoom levels ahead")
            labelColor: performanceGroup._bright
            toolTipText: qsTr("Every zoom level of the mosaic is painted in the background, so a zoom shows it at once. On a large survey it takes a few hundred MB of memory and of temporary disk space more.")
            interactive: true
            onClicked: precomputeSwitch.click()
            KSwitch {
                id: precomputeSwitch
                flat: true
                checked: performanceGroup.store ? performanceGroup.store.mosaicPrecompute : true
                onToggled: if (performanceGroup.store) performanceGroup.store.mosaicPrecompute = checked
            }
        }

        KIslandRow {
            KPinTag { key: "app:performance.multiThread" }
            label: qsTr("Multi-threaded mosaic")
            labelColor: performanceGroup._bright
            toolTipText: qsTr("The mosaic is painted on several processor cores. The picture is the same either way.")
            interactive: true
            onClicked: threadsSwitch.click()
            KSwitch {
                id: threadsSwitch
                flat: true
                checked: performanceGroup.store ? performanceGroup.store.mosaicMultiThreaded : true
                onToggled: if (performanceGroup.store) performanceGroup.store.mosaicMultiThreaded = checked
            }
        }

        KIslandRow {
            KPinTag { key: "app:performance.keepOldLevel" }
            label: qsTr("Keep the old zoom level until the new one is drawn")
            labelColor: performanceGroup._bright
            toolTipText: qsTr("After a zoom the previous level stays under the new one until the new one is painted, instead of an empty screen.")
            interactive: true
            onClicked: fallbackSwitch.click()
            KSwitch {
                id: fallbackSwitch
                flat: true
                checked: performanceGroup.store ? performanceGroup.store.zoomFallback : true
                onToggled: if (performanceGroup.store) performanceGroup.store.zoomFallback = checked
            }
        }
    }
}
