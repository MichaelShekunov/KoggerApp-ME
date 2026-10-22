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
//   {"op": "drag3d", "seconds": s, "hz": n, "radius": px, "turnSeconds": s}
//                                    a left-button drag of the 3D view through its triggers, as the QML pointer
//                                    handlers (a finger on the head unit) call them: the pointer circles about the
//                                    view's middle. Logged: the moves, the frames and the longest gap between them,
//                                    the time a move took on the GUI thread, how late the moves' timer fired
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
#include <QTimer>

#include <memory>
#include <vector>

class Core;
class UiProbe;
class QQuickItem;

class EchoEditScript : public QObject
{
    Q_OBJECT
public:
    explicit EchoEditScript(Core* core, QObject* parent = nullptr);
    void setUiProbe(UiProbe* probe) { uiProbe_ = probe; }   // for "grab"
    void armFromEnvironment();

private:
    void step();
    void runStep(const QJsonObject& s);
    void stroke(const QString& op, int from, int to, float depth);
    void probe(const QString& name, int from, int to, float offset, float radius);
    void dump(const QString& path);
    bool startDrag3d(const QJsonObject& s);
    void dragMove();
    void finishDrag3d();

    struct DragFrames;               // counted on the render thread (frameSwapped)

    Core* core_ = nullptr;
    UiProbe* uiProbe_ = nullptr;
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
};
