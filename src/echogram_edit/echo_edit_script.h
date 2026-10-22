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
//                                    the workspace's margin on a side set and put back n times, ms apart (400 without);
//                                    with "dock": {"widget": id, "side": s} instead, that widget docked there and
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
//   {"op": "grab", "path": file.png}                         the window, through the UI probe
//   {"op": "quit"}
// An epoch can be given as a share of the recording instead: "fromFrac", "toFrac", "epochFrac".
//
// With "live": true at the top of the script the steps do not wait for a recording: a device on a link
// (KOGGER_AUTOTEST=UDP:<port>, a stand) is enough. For the sonar presets (ID_PRESET):
//   {"op": "waitPresets", "timeoutMs": 20000}   until a device has answered about its presets
//   {"op": "presets"}                the presets each device reported, one line a device
//   {"op": "presetRecall", "slot": n}   {"op": "presetSave", "slot": n, "name": s}
//                                    on the first device that keeps presets, as its frequency buttons do

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
    QJsonObject probe_;              // the last magnetProbe's numbers, for dump
    QJsonObject export_;             // the last exportCsv's
    QJsonObject results_;            // the last despike's and turns' counts of pings
    bool live_ = false;              // "live": steps run without a recording open

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
