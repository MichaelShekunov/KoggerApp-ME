#pragma once

// KOGGER_ECHOEDIT_SCRIPT=<file.json>: echogram edits made by a script on the recording opened from
// the command line, for the end-to-end check tools/perf/echoedit_e2e.ps1. Off unless set. The steps
// go one after another, each with a line "echoedit script: ..." in the log:
//
//   {"op": "settle", "quietMs": 3000, "timeoutMs": 180000}   the bottom track quiet that long
//   {"op": "wait", "ms": 8000}
//   {"op": "draw" | "erase" | "raise" | "lower", "from": e0, "to": e1, "depth": m | "depthRel": k}
//                                    as a stroke of the tool; depthRel: k times the stretch's median bottom
//   {"op": "mark" | "unmark", "from": e0, "to": e1}
//   {"op": "smooth", "from": e0, "to": e1, "passes": k}   {"op": "fillGaps", "from": e0, "to": e1}
//                                    a stroke of the brush over the stretch (smooth: k sweeps of it)
//   {"op": "mosaicOut", "from": e0, "to": e1, "side": 0 | 1}   one channel's side out of the mosaic
//   {"op": "despike"}   {"op": "turns"}   the palette's whole-recording buttons; the count is dumped
//   {"op": "spike", "from": e0, "to": e1 | "width": pings, "factor": k}
//                                    a test's spike: the bottom there k times deeper, as if the bottom
//                                    track had found it (outside the history, not undone)
//   {"op": "trimStart" | "trimEnd", "epoch": e}      {"op": "clearTrim"}   {"op": "resetAll"}
//   {"op": "undo"}   {"op": "redo"}   {"op": "undoAll"}   {"op": "redoAll"}   {"op": "flush"}
//   {"op": "undoTo", "steps": k}     undone until k steps are left to undo
//   {"op": "magnetProbe", "name": s, "from": e0, "to": e1, "offset": m, "radius": m}
//                                    the magnet started off the bottom track's bottom, where it lands
//   {"op": "exportCsv", "dir": path}
//   {"op": "dump", "path": file}    the edits, the bottom's digest, the probes, the export, as JSON,
//                                    and <file>.bottom.bin: the first channel's bottom, float32 an epoch,
//                                    and <file>.track.bin: what the turns are found from (see dump())
//   {"op": "view", "epoch": e, "tool": t, "palette": true}   the echograms at e, a tool, the palette
//   {"op": "hscale", "scale": s}     every echogram's horizontal scale (screen columns a ping, 1/16..8)
//   {"op": "hzoom", "steps": k}   {"op": "vzoom", "delta": d}   a zoom as the user makes it on the echograms
//                                    on screen (the settings panel's step; Ctrl + wheel), which shows their hint
//   {"op": "hint"}                   what each echogram's scale hint says, with the numbers it is made from
//   {"op": "pin", "key": k, "label": s}   a setting pinned as pin mode's pin does it (a settings widget)
//   {"op": "pin", "mode": true, "group": g}   pin mode on, the settings pane open at a group; "mode": false closes
//   {"op": "pinuse", "key": k, "index": i}   a settings widget's mirror control used as a hand would
//   {"op": "pinstate", "key": k}     the setting's row controls on the settings page and their mirrors
//   {"op": "plotState", "set": {"bottomTrackLine": false}}   each echogram's panel state (Plot2D.qml's echoViewState),
//                                    changed as its controls change it, and logged with what the plot says
//   {"op": "relayout", "times": n, "margin": px, "side": "right"|"left"|"bottom", "intervalMs": ms}
//                                    the workspace's margin on a side set and put back n times, ms apart (400 without):
//                                    written, its binding to the docks is gone for the rest of the run - put it last;
//                                    with "dock": {"widget": id | i, "side": s} instead, that widget docked there and
//                                    undocked (WorkspaceStore.dockWidget); with "collapse": side, that dock collapsed and
//                                    opened, as its tab does. Logged: the GUI thread's time in the change,
//                                    the sync of the frame that takes it (the GUI waits: the 3D view's new FBO, the
//                                    echograms painted anew), the time to that frame on screen; and every frame until the
//                                    next change (PaneFrame's animation, if the panes animate): how many, their syncs
//                                    together, when the last was on screen
//   {"op": "linkTrack", "points": n, "run": k}   the boat's track by link quality filled as a long session leaves
//                                    it (MavVehicle::fillLinkTrackForTest), from the plan's first point
//   {"op": "drag3d", "seconds": s, "hz": n, "radius": px, "turnSeconds": s}
//                                    a left-button drag of the 3D view through its triggers, as the QML pointer
//                                    handlers (a finger on the head unit) call them: the pointer circles about the
//                                    view's middle. Logged: the moves, the frames and the longest gap between them,
//                                    the time a move took on the GUI thread, how late the moves' timer fired
//   {"op": "dock", "widget": i | id, "side": s, "at": k}   a widget into the dock at a window edge
//                                    (WorkspaceStore.dockWidget); i: its place in the list of widgets, k: in the dock
//   {"op": "undock", "widget": i | id}   {"op": "dockScale", "side": s, "scale": k}
//   {"op": "dockCollapse", "side": s, "collapsed": true | false}   as the dock's tab does; without "collapsed": toggled
//   {"op": "layout", "index": i}     the layout applied, as its button does
//   {"op": "front", "widget": i | id}   the widget's panel over the others, as a touch on it does
//   {"op": "editMode", "on": true | false}   the layout edit mode (WorkspaceStore.editableMode)
//   {"op": "docks"}                  one line of JSON: the docks, their insets and slots, the scale steps, the widgets'
//                                    sizes at scale 1, the window and the workspace on screen, each widget's panel
//                                    (docked, visible, where)
//   {"op": "drag", "from"...: a point, "to"...: a point, "holdMs": ms, "ms": ms, "steps": n, "grab": file.png}
//                                    a left-button drag on the main window, as a hand makes it: pressed, held holdMs
//                                    (a long press), moved over ms in n moves, held a moment (the picture taken then),
//                                    let go. A point: [x, y]; "...Frac": [fx, fy] of the window; "...Widget": id (its
//                                    panel's middle, or where no other panel covers it, or "...Offset": [dx, dy] from
//                                    its top-left; none when others cover it all: the step is skipped); "...Tab": a dock's
//                                    side; "toEdge": side ("toAlong": share along it); "by": [dx, dy] from the start
//   {"op": "viewCenter"}             the geographic point in the middle of the 3D view, where it looks (on the water)
//   {"op": "tr", "context": name, "source": text, "apply": code}   the UI language in use and the text as the app
//                                    shows it in that language (the translators installed; the context is a QML
//                                    file's name); with "apply", that language chosen first, as the picker does
//   {"op": "grab", "path": file.png}                         the window, through the UI probe
//   {"op": "probe", "tag": name, "root": type}   the UI probe's dump into KOGGER_UI_PROBE's folder: ui-<name>.json
//                                    (the item tree from the first item of that QML type, else the window's; a
//                                    Text's words and whether they are cut; ui-<name>.png too with KOGGER_UI_PROBE_PNG=1)
// The missions (src/mission, src/autopilot; a live autopilot, e.g. KOGGER_AUTOTEST=TCP:5760 to ArduPilot SITL):
//   {"op": "waitAutopilot", "timeoutMs": 30000}   until an autopilot is bound (DeviceManager's heartbeat rule)
//   {"op": "missionPlan", "home": [lat, lon], "points": [[lat, lon], ...], "rally": [[lat, lon], ...], "fence": [[lat, lon], ...],
//    "speed": m/s, "end": endAction}   a new plan in the planner as its editor makes one (fence: one inclusion zone)
//   {"op": "missionUpload"} / {"op": "missionDownload"}   the plan to the autopilot / the autopilot's mission
//                                    into the planner (the editor's buttons)
//   {"op": "waitMissionTransfer", "timeoutMs": 60000}   until the transfer started by the last step has ended;
//                                    every end is logged: "mission transfer: upload|download ok|failed result type"
//   {"op": "missionState"}           one line of JSON: the autopilot (online, mode, armed, MISSION_CURRENT seq and
//                                    total), the planner (items, rally, fence), the run tracker (known, items, nav,
//                                    matchesPlan), the route MavVehicle's ghost follows (boatRoute)
//   {"op": "missionEditor", "on": true | false}   the editor opened or closed (core.setMissionEditorActive)
//   {"op": "autopilotMode", "mode": n} / {"op": "autopilotArm", "on": bool}   the autopilot card's buttons
//   {"op": "quit"}
// An epoch can be given as a share of the recording instead: "fromFrac", "toFrac", "epochFrac".
//
// With "live": true at the top of the script the steps do not wait for a recording: a device on a link
// (KOGGER_AUTOTEST=UDP:<port>, a stand) is enough. For the sonar presets (ID_PRESET):
//   {"op": "waitPresets", "timeoutMs": 20000}   until a device has answered about its presets
//   {"op": "presets"}                the presets each device reported, one line a device
//   {"op": "presetRecall", "slot": n}   {"op": "presetSave", "slot": n, "name": s}
//                                    on the first device that keeps presets, as its frequency buttons do
//   {"op": "presetDelete", "slot": n}   empties the slot (no button does: the live check undoes its save)
// A recorder's servo (ID_SERVO_CONTROL; docs/SERVO_MANUAL.md), on the first device with servo control:
//   {"op": "waitServo", "timeoutMs": 20000}   until a device has servo control
//   {"op": "servo"}                  each servo device's state, one line each
//   {"op": "servoManual", "on": bool}   the panel's Manual switch
//   {"op": "servoSet", "deg": x}     the angle to hold, as the knob's release or the angle field
// A GODIO ring board's sync counters (ID_RING_STAT, firmware 1.16+), on the device at bus address "addr":
//   {"op": "devicePage", "addr": n}  its device page opened, as a tap on it in the device list does
//   {"op": "ringStat", "addr": n, "do": "read" | "reset" | "show"}   asked, zeroed, or what the page shows logged
//   {"op": "servoDrag", "from": a, "to": b, "ms": 1000, "everyMs": 5}   the knob dragged: servoManualMove every
//                                    everyMs from a to b over ms, then servoSet b; logs how many moves it made
// A sonar's firmware, as its device page's UPGRADE (behind a recorder: the update through it):
//   {"op": "upgrade", "path": file, "board": n, "addr": n}   the first sonar, or the one of that ID_VERSION board and
//                                    route; logs the sonar it started on; not while a recorder records
//   {"op": "recorderState"}          every recorder's status, one line each
//   {"op": "clickItem", "name": objectName, "index": n}   a tap on that item (its clicked()): "devBadgeButton" opens a
//                                    device's pill
//   {"op": "waitUpgrade", "timeoutMs": 180000}   until that update ends (flashed / failed), the progress every 10 %
//   {"op": "devState", "board": n, "addr": n}   the sonar's settings as the app has them, one log line
// For the Wi-Fi module's radio switched off (ID_WIFI_RF, firmware 0.20), on the first Wi-Fi module, or with
// "addr": n on the one at that bus address:
//   {"op": "wifiPage"}               its device page opened
//   {"op": "wifiRadio", "off": s} / {"op": "wifiRadio", "on": true}
//                                    switched off for s seconds (0 = until on) or on, as the page does
//   {"op": "wifiRadio"}              its radio state as the page reads it, logged

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QElapsedTimer>
#include <QPointer>
#include <QPointF>
#include <QTimer>

#include <memory>
#include <vector>

class Core;
class MavVehicle;
class UiProbe;
class QQuickItem;
class QQuickWindow;

class EchoEditScript : public QObject
{
    Q_OBJECT
public:
    explicit EchoEditScript(Core* core, QObject* parent = nullptr);
    void setUiProbe(UiProbe* probe) { uiProbe_ = probe; }   // for "grab"
    void setBoat(MavVehicle* boat) { boat_ = boat; }        // for "linkTrack"
    void armFromEnvironment();

private:
    void step();
    void runStep(const QJsonObject& s);
    void stroke(const QString& op, int from, int to, float depth);
    void probe(const QString& name, int from, int to, float offset, float radius);
    void dump(const QString& path);
    bool dockStep(const QString& op, const QJsonObject& s);   // the docks' steps; false: not one of them
    QObject* firstServoDevice() const;   // the first device with servo control, or null
    bool missionStep(const QString& op, const QJsonObject& s);   // the missions' steps; false: not one of them
    bool startGesture(const QJsonObject& s);   // "drag": the press
    int gestureStep(const QJsonObject& s);     // the next move or the release: ms to the next call, -1 when done
    bool startDrag3d(const QJsonObject& s);
    void dragMove();
    void finishDrag3d();
    bool startRelayout(const QJsonObject& s);
    bool relayoutStep();             // false when done
    void finishRelayout();

    struct DragFrames;               // counted on the render thread (frameSwapped)
    struct RelayoutFrames;           // the frame that takes a change, timed on the render thread

    Core* core_ = nullptr;
    UiProbe* uiProbe_ = nullptr;
    MavVehicle* boat_ = nullptr;
    QJsonArray steps_;
    int next_ = 0;
    QTimer timer_;
    QElapsedTimer quiet_;            // since the bottom track last changed
    QElapsedTimer settleClock_;
    int servoDragMoves_ = 0;           // the moves the running "servoDrag" step has made
    QPointer<QObject> upgradeDev_;     // the sonar the "upgrade" step started on
    int upgradeLogged_ = -10;          // the progress "waitUpgrade" last logged
    QJsonObject probe_;              // the last magnetProbe's numbers, for dump
    QJsonObject export_;             // the last exportCsv's
    QJsonObject results_;            // the last despike's and turns' counts of pings
    bool live_ = false;              // "live": steps run without a recording open
    QStringList missionResults_;     // the mission transfers that ended since the script began, as logged
    int missionResultsBefore_ = 0;   // their count when the last transfer was started by a step

    // "drag3d"
    QPointer<QQuickItem> dragView_;
    QTimer dragTimer_;
    QElapsedTimer dragClock_;
    QMetaObject::Connection dragFrameConn_;
    std::shared_ptr<DragFrames> dragFrames_;
    std::vector<qint64> dragMoveNs_;  // what each move took on the GUI thread
    qint64 dragLastTickNs_ = 0;
    qint64 dragLateMaxNs_ = 0;
    int dragIntervalMs_ = 10;
    double dragCx_ = 0.0, dragCy_ = 0.0, dragRadius_ = 0.0, dragTurnS_ = 2.0, dragX_ = 0.0, dragY_ = 0.0;

    // "drag"
    QPointer<QQuickWindow> gestureWin_;
    QPointF gestureFrom_, gestureTo_;
    int gestureSteps_ = 20, gestureStep_ = 0, gestureHoldMs_ = 0, gestureMs_ = 400;
    QElapsedTimer gestureClock_;

    // "relayout"
    QPointer<QQuickItem> relayoutItem_;
    QString relayoutProperty_;       // "anchors.rightMargin", ...
    double relayoutOrig_ = 0.0, relayoutMargin_ = 0.0;
    int relayoutLeft_ = 0;
    bool relayoutPending_ = false;
    qint64 relayoutT0_ = 0;
    QElapsedTimer relayoutClock_;
    std::shared_ptr<RelayoutFrames> relayoutFrames_;
    QList<QMetaObject::Connection> relayoutConns_;
    std::vector<qint64> relayoutChangeNs_, relayoutSyncNs_, relayoutShownNs_;
    std::vector<qint64> relayoutFrameCount_, relayoutSyncSumNs_, relayoutLastNs_;   // every frame until the next change
    QPointer<QObject> relayoutStore_;  // "dock": the changes are a widget docked and undocked
    QString relayoutDockId_, relayoutDockSide_;
    bool relayoutCollapse_ = false;
    int relayoutIntervalMs_ = 400;
};
