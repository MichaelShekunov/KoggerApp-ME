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
//   {"op": "grab", "path": file.png}                         the window, through the UI probe
//   {"op": "quit"}
// An epoch can be given as a share of the recording instead: "fromFrac", "toFrac", "epochFrac".

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QElapsedTimer>
#include <QTimer>

class Core;
class UiProbe;

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
};
