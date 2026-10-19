// Offline check of the echogram edits' model (src/echogram_edit/echo_edit_model.*): the steps kept for
// undo and redo, what a step changes downstream, the file of edits next to a recording, the pencil's
// magnet. Without the application: the dataset is a vector of ping states here, changed the way
// EchogramEditor changes the epochs (a stroke: the states before, the edit, the states after).
// docs/echogram-editing.md.
//
//     echoedit_check          all checks, exit code 0 when they pass

#include "echo_edit_model.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace echoedit;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* name, const QString& detail = QString())
{
    if (ok) {
        ++g_pass;
        std::printf("PASS %s\n", name);
    }
    else {
        ++g_fail;
        std::printf("FAIL %s%s%s\n", name, detail.isEmpty() ? "" : ": ", detail.toLocal8Bit().constData());
    }
}

// A fixed sequence: the same run every time.
struct Lcg {
    uint64_t s;
    explicit Lcg(uint64_t seed) : s(seed) {}
    uint32_t next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return uint32_t(s >> 33); }
    int below(int n) { return n > 0 ? int(next() % uint32_t(n)) : 0; }
    float unit() { return float(next() & 0xFFFFFF) / float(0x1000000); }
};

constexpr uint8_t kProcessing = 1;   // DistanceSourceProcessing
constexpr uint8_t kConstrain = 3;    // DistanceSourceConstrainHand

// A ping of two channels, addresses 1 and 2, as the bottom track leaves them.
PingState autoPing(float d)
{
    PingState p;
    for (uint8_t a : { uint8_t(1), uint8_t(2) }) {
        ChannelBottom b;
        b.address = a;
        b.distance = d;
        b.source = kProcessing;
        p.channels.append(b);
    }
    return p;
}

// The dataset: what EchogramEditor reads and writes through Dataset::pingEditState / setPingEditState
// and the trim.
struct Sim {
    QVector<PingState> pings;
    Trim trim;

    bool operator==(const Sim& o) const
    {
        if (trim != o.trim || pings.size() != o.pings.size()) {
            return false;
        }
        for (int i = 0; i < pings.size(); ++i) {
            if (!samePing(pings[i], o.pings[i])) {
                return false;
            }
        }
        return true;
    }

    // EchogramEditor::apply
    void apply(const Command& c, bool undo)
    {
        for (const PingChange& p : c.pings) {
            pings[p.epoch] = undo ? p.before : p.after;
        }
        if (c.trimBefore != c.trimAfter) {
            trim = undo ? c.trimBefore : c.trimAfter;
        }
    }

    bool hasEdits(int e) const
    {
        const PingState& p = pings[e];
        if (p.bad) {
            return true;
        }
        for (const ChannelBottom& b : p.channels) {
            if (b.source == kSourceHand || std::isfinite(b.min) || std::isfinite(b.max)) {
                return true;
            }
        }
        return false;
    }
};

// A stroke as EchogramEditor records it: the states before, the edit, the states after; pings the
// edit left as they were are not in the step.
Command stroke(Sim& sim, Command::Kind kind, int from, int to, float depth, int trimAt)
{
    Command c;
    c.kind = kind;
    c.trimBefore = sim.trim;
    QVector<PingState> before = sim.pings;
    const int n = sim.pings.size();
    switch (kind) {
    case Command::Draw:
    case Command::Erase:
    case Command::Raise:
    case Command::Lower:
    case Command::Delete3d:
        for (int e = from; e < to; ++e) {
            for (ChannelBottom& b : sim.pings[e].channels) {
                if (kind == Command::Draw) {
                    b.distance = depth + 0.001f * float(e - from);
                    b.source = kSourceHand;
                }
                else if (kind == Command::Erase || kind == Command::Delete3d) {
                    b.distance = NAN;
                    b.source = kSourceHand;
                }
                else if (kind == Command::Raise) {
                    b.max = depth;
                    b.source = kConstrain;
                }
                else {
                    b.min = depth;
                    b.source = kConstrain;
                }
            }
        }
        break;
    case Command::Mark:
    case Command::Unmark:
        for (int e = from; e < to; ++e) {
            sim.pings[e].bad = (kind == Command::Mark);
        }
        break;
    case Command::TrimStart:
        sim.trim.start = std::clamp(trimAt, 0, n);
        if (sim.trim.end <= sim.trim.start) {
            sim.trim.end = kNoTrimEnd;
        }
        break;
    case Command::TrimEnd:
        sim.trim.end = trimAt + 1 >= n ? kNoTrimEnd : trimAt + 1;
        if (sim.trim.end <= sim.trim.start) {
            sim.trim.start = 0;
        }
        break;
    case Command::TrimClear:
        sim.trim = Trim();
        break;
    case Command::Reset:
        for (int e = 0; e < n; ++e) {
            if (!sim.hasEdits(e)) {
                continue;
            }
            PingState& p = sim.pings[e];
            p.bad = false;
            for (ChannelBottom& b : p.channels) {
                b.min = NAN;
                b.max = NAN;
                if (b.source == kSourceHand) {
                    b.source = kSourceNone;
                    b.distance = NAN;
                }
            }
        }
        sim.trim = Trim();
        break;
    }
    c.trimAfter = sim.trim;
    for (int e = 0; e < n; ++e) {
        if (!samePing(before[e], sim.pings[e])) {
            PingChange p;
            p.epoch = e;
            p.before = before[e];
            p.after = sim.pings[e];
            c.pings.append(p);
        }
    }
    return c;
}

QString trimText(const Trim& t)
{
    return QStringLiteral("%1..%2").arg(t.start).arg(t.end == kNoTrimEnd ? QStringLiteral("end") : QString::number(t.end));
}

// ---------------------------------------------------------------------------------------------------

void checkBasics()
{
    ChannelBottom a;
    ChannelBottom b;
    check(sameBottom(a, b), "an erased bottom equals an erased bottom (NaN == NaN)");
    b.min = 1.0f;
    check(!sameBottom(a, b), "a search limit set differs from none");
    b = a;
    b.source = kSourceHand;
    check(!sameBottom(a, b), "the source counts: a point erased by hand differs from one never found");

    PingState p = autoPing(5.0f);
    PingState q = p;
    q.bad = true;
    check(!samePing(p, q), "the bad mark counts");

    Trim t;
    check(!t.active() && !t.trims(0) && !t.trims(1000000), "no trim: nothing trimmed, the pings to come neither");
    t.start = 10;
    t.end = 20;
    check(t.active() && t.trims(9) && !t.trims(10) && !t.trims(19) && t.trims(20), "a trim keeps [start, end)");

    const auto r = rangesOf({ 1, 2, 3, 7, 8, 10 });
    check(r.size() == 3 && r[0] == qMakePair(1, 4) && r[1] == qMakePair(7, 9) && r[2] == qMakePair(10, 11),
          "consecutive epochs make one range");
    check(rangesOf({}).isEmpty() && rangesOf({ 5, 5, 6 }).size() == 1, "no epochs, no range; a repeat does not split one");

    QVector<Run> runs;
    for (int e : { 5, 6, 7 }) {
        appendToRuns(runs, 1, e, float(e));
        if (e < 7) {
            appendToRuns(runs, 2, e, float(-e));   // the epoch-major order Dataset::collectEditState scans in
        }
    }
    appendToRuns(runs, 1, 10, 10.0f);
    check(runs.size() == 3 && runs[0].address == 1 && runs[0].from == 5 && runs[0].values.size() == 3
              && runs[1].address == 2 && runs[1].from == 5 && runs[1].values.size() == 2
              && runs[2].address == 1 && runs[2].from == 10,
          "runs: per channel, broken by a gap", QStringLiteral("%1 runs").arg(runs.size()));
}

void checkWhatAStepChanges()
{
    Sim sim;
    for (int i = 0; i < 50; ++i) {
        sim.pings.append(autoPing(4.0f + 0.01f * float(i)));
    }

    Sim s = sim;
    const Command mark = stroke(s, Command::Mark, 10, 20, 0, 0);
    check(mark.changesMarks() && removesPoints(mark, false) && !removesPoints(mark, true),
          "marking takes points out; its undo puts them back");

    const Command unmark = stroke(s, Command::Unmark, 10, 15, 0, 0);
    check(!removesPoints(unmark, false) && removesPoints(unmark, true), "unmarking puts points back; its undo takes them out");

    s = sim;
    const Command erase = stroke(s, Command::Erase, 5, 8, 0, 0);
    check(removesPoints(erase, false) && !removesPoints(erase, true), "the eraser takes points out");
    const Command drawOnErased = stroke(s, Command::Draw, 5, 8, 3.0f, 0);
    check(!removesPoints(drawOnErased, false) && removesPoints(drawOnErased, true),
          "the pencil over an erased stretch puts points back");

    s = sim;
    const Command trim = stroke(s, Command::TrimStart, 0, 0, 0, 12);
    check(trim.pings.isEmpty() && removesPoints(trim, false) && !removesPoints(trim, true),
          "trimming the start takes points out (a step with no pings)");
    const Command trimEnd = stroke(s, Command::TrimEnd, 0, 0, 0, 40);
    check(removesPoints(trimEnd, false) && s.trim.start == 12 && s.trim.end == 41, "trimming the end keeps the ping under the cursor",
          trimText(s.trim));
    const Command trimEndLast = stroke(s, Command::TrimEnd, 0, 0, 0, 49);
    check(s.trim.end == kNoTrimEnd && !removesPoints(trimEndLast, false), "the end moved to the last ping is no end trim", trimText(s.trim));
    stroke(s, Command::TrimEnd, 0, 0, 0, 5);
    check(s.trim.start == 0 && s.trim.end == 6, "an end before the start drops the start trim", trimText(s.trim));

    s = sim;
    const Command raise = stroke(s, Command::Raise, 20, 25, 3.0f, 0);
    check(raise.changesLimits() && epochsToTrackAgain(raise, false).size() == 5,
          "a search limit (its redo): those pings are tracked again");
    check(epochsToTrackAgain(raise, true).isEmpty(),
          "its undo: the depths kept from before come back as they were, no run");
    const Command raiseAgain = stroke(s, Command::Raise, 22, 24, 2.5f, 0);
    check(epochsToTrackAgain(raiseAgain, true).size() == 2,
          "undoing a second limit over the first: the first limit is back, those pings tracked again");

    s = sim;
    const Command draw = stroke(s, Command::Draw, 30, 33, 2.0f, 0);
    check(!draw.changesLimits() && epochsToTrackAgain(draw, false).isEmpty(),
          "the pencil: nothing to track again (the hand's points stay)");
    check(epochsToTrackAgain(draw, true).isEmpty(), "its undo: the depths the bottom track had found come back, no run");

    // One side of a DSSS pair (sub-channels 0 and 1 of one channel) out of the mosaic.
    PingState pair;
    {
        ChannelBottom b;
        b.address = 1;
        b.distance = 5.0f;
        b.source = kProcessing;
        b.subs = 2;
        pair.channels.append(b);
    }
    PingChange sideOut;
    sideOut.epoch = 3;
    sideOut.before = pair;
    sideOut.after = pair;
    sideOut.after.channels[0].mosaicOut = 0x02;
    Command out;
    out.kind = Command::MosaicOut;
    out.pings = { sideOut };
    check(!samePing(sideOut.before, sideOut.after) && allSides(2) == 0x03 && allSides(8) == 0xFF,
          "a side out of the mosaic counts in a ping; all the sides of a pair are two bits");
    check(removesFromMosaic(out, false) && !removesFromMosaic(out, true) && !removesPoints(out, false) && !removesPoints(out, true),
          "a side out of the mosaic: taken out (built again), its undo puts it back; no point goes");
    PingChange whole = sideOut;
    whole.after.bad = true;
    Command outWhole;
    outWhole.kind = Command::Mark;
    outWhole.pings = { whole };
    check(!removesFromMosaic(outWhole, false) && removesPoints(outWhole, false),
          "a ping out whole is the points' case, not the side's");
    PingState sameSubs = pair;
    sameSubs.channels[0].subs = 1;
    check(samePing(pair, sameSubs), "the count of sub-channels is the data's, not an edit");

    const Command reset = stroke(s, Command::Reset, 0, 0, 0, 0);
    check(reset.kind == Command::Reset && reset.pings.size() == 3 && epochsToTrackAgain(reset, false).size() == 3
              && removesPoints(reset, false),
          "reset: the drawn pings left without a bottom until it is tracked again");
}

void checkHistory()
{
    auto step = [](Command::Kind k, int pings) {
        Command c;
        c.kind = k;
        for (int i = 0; i < pings; ++i) {
            PingChange p;
            p.epoch = i;
            p.after.bad = true;
            c.pings.append(p);
        }
        return c;
    };

    History h(3, 1000);
    check(!h.canUndo() && !h.canRedo() && !h.nextUndo(), "a new history has nothing to undo or redo");
    h.push(Command());
    check(!h.canUndo(), "an empty step is not kept");
    for (int k = 0; k < 5; ++k) {
        h.push(step(Command::Kind(k), 1));
    }
    check(h.undoCount() == 3 && h.nextUndo()->kind == Command::Kind(4), "at the step limit the oldest go");
    const Command u = h.takeUndo();
    check(u.kind == Command::Kind(4) && h.canRedo() && h.nextRedo()->kind == Command::Kind(4), "undo moves the step to the redo side");
    const Command r = h.takeRedo();
    check(r.kind == Command::Kind(4) && !h.canRedo() && h.undoCount() == 3, "redo moves it back");
    h.takeUndo();
    h.push(step(Command::Mark, 1));
    check(!h.canRedo() && h.nextUndo()->kind == Command::Mark, "a new step drops what could be redone");

    History big(100, 10);
    big.push(step(Command::Draw, 4));
    big.push(step(Command::Erase, 4));
    big.push(step(Command::Mark, 4));
    check(big.undoCount() == 2 && big.nextUndo()->kind == Command::Mark, "past the limit of ping changes the oldest go",
          QString::number(big.undoCount()));
    big.push(step(Command::Reset, 50));
    check(big.undoCount() == 1 && big.nextUndo()->kind == Command::Reset, "a step alone over the limit stays: what was just done can be undone");
    h.clear();
    check(!h.canUndo() && !h.canRedo(), "clear: nothing left");
}

// Many steps at random, undone and redone at random: every undo gives back the state before the step,
// every redo the one after -- a timeline of states kept beside the history says which.
void checkRandomSessions()
{
    Lcg rnd(20261018);
    int sessions = 0;
    int mismatches = 0;
    int steps = 0;
    int undos = 0;
    int redos = 0;
    QString first;
    for (int session = 0; session < 40; ++session) {
        Sim sim;
        const int n = 60 + rnd.below(300);
        for (int i = 0; i < n; ++i) {
            sim.pings.append(autoPing(2.0f + 10.0f * rnd.unit()));
        }
        History h(1000, 10000000);
        QVector<Sim> timeline{ sim };
        int at = 0;   // timeline[at] is the state now
        for (int op = 0; op < 150; ++op) {
            const int what = rnd.below(10);
            if (what < 2 && h.canUndo()) {
                const Command c = h.takeUndo();
                sim.apply(c, true);
                --at;
                ++undos;
            }
            else if (what < 3 && h.canRedo()) {
                const Command c = h.takeRedo();
                sim.apply(c, false);
                ++at;
                ++redos;
            }
            else {
                const Command::Kind k = Command::Kind(rnd.below(int(Command::Reset) + 1));
                const int a = rnd.below(n);
                const int b = std::min(n, a + 1 + rnd.below(40));
                Command c = stroke(sim, k, a, b, 1.0f + 5.0f * rnd.unit(), rnd.below(n));
                if (c.isEmpty()) {
                    continue;
                }
                h.push(c);
                timeline.resize(at + 1);
                timeline.append(sim);
                ++at;
                ++steps;
            }
            if (!(sim == timeline[at])) {
                ++mismatches;
                if (first.isEmpty()) {
                    first = QStringLiteral("session %1, operation %2").arg(session).arg(op);
                }
            }
        }
        // All the way back, then forward again.
        while (h.canUndo()) {
            sim.apply(h.takeUndo(), true);
            --at;
        }
        mismatches += !(sim == timeline[0]);
        while (h.canRedo()) {
            sim.apply(h.takeRedo(), false);
            ++at;
        }
        mismatches += !(sim == timeline.last());
        ++sessions;
    }
    check(mismatches == 0, "random sessions: every undo and redo lands on the state it should",
          QStringLiteral("%1 sessions, %2 steps, %3 undos, %4 redos, %5 mismatches%6")
              .arg(sessions).arg(steps).arg(undos).arg(redos).arg(mismatches)
              .arg(first.isEmpty() ? QString() : QStringLiteral(", first at ") + first));
    std::printf("     %d sessions, %d steps, %d undos, %d redos\n", sessions, steps, undos, redos);
}

EditFile sampleFile()
{
    EditFile f;
    f.record = QStringLiteral("00014_03.05.2025.KLF");
    f.recordBytes = 123456789012LL;
    f.epochs = 186000;
    f.trim.start = 2924;
    f.bad = { qMakePair(5000, 5200), qMakePair(9000, 9001) };
    appendToRuns(f.bottom, 1, 6000, 3.14159f);
    appendToRuns(f.bottom, 1, 6001, NAN);   // erased by hand
    appendToRuns(f.bottom, 2, 6000, 3.14159f);
    appendToRuns(f.limitMin, 1, 7000, 1.5f);
    appendToRuns(f.limitMax, 2, 7100, 12.25f);
    for (int e : { 8000, 8001, 8002, 8010 }) {
        appendToSides(f.mosaicOut, 1, 1, e);   // a DSSS pair's second side
    }
    appendToSides(f.mosaicOut, 1, 0, 8001);
    return f;
}

void checkFile()
{
    const EditFile f = sampleFile();
    EditFile g;
    QString why;
    const bool ok = fromJson(toJson(f), &g, &why);
    bool same = ok && g.record == f.record && g.recordBytes == f.recordBytes && g.epochs == f.epochs && g.trim == f.trim
             && g.bad == f.bad && g.bottom.size() == 2 && g.limitMin.size() == 1 && g.limitMax.size() == 1;
    if (same) {
        same = g.bottom[0].address == 1 && g.bottom[0].from == 6000 && g.bottom[0].values.size() == 2
            && g.bottom[0].values[0] == 3.14159f && std::isnan(g.bottom[0].values[1])
            && g.limitMax[0].address == 2 && g.limitMax[0].from == 7100 && g.limitMax[0].values[0] == 12.25f;
    }
    check(same, "the file of edits: written and read back, an erased point as null, every float as it was", why);
    const bool sides = ok && g.mosaicOut.size() == 2
        && g.mosaicOut[0].address == 1 && g.mosaicOut[0].sub == 1 && g.mosaicOut[0].ranges.size() == 2
        && g.mosaicOut[0].ranges[0] == qMakePair(8000, 8003) && g.mosaicOut[0].ranges[1] == qMakePair(8010, 8011)
        && g.mosaicOut[1].address == 1 && g.mosaicOut[1].sub == 0 && g.mosaicOut[1].ranges.size() == 1;
    check(sides, "the sides out of the mosaic: per sub-channel, as ranges, written and read back");

    const QJsonObject o = QJsonDocument::fromJson(toJson(f)).object();
    check(o.value(QStringLiteral("trim")).toObject().value(QStringLiteral("end")).isNull()
              && o.value(QStringLiteral("version")).toInt() == EditFile::kVersion,
          "no end trim is null in the file; the version is written");

    EditFile none;
    none.epochs = 10;
    check(none.isEmpty() && f.isEmpty() == false, "a file with no edits is empty (the editor removes it)");

    auto rejects = [&](const char* name, QByteArray text, const QString& expect) {
        EditFile x;
        QString err;
        const bool read = fromJson(text, &x, &err);
        check(!read && err.contains(expect), name, err);
    };
    QByteArray good = toJson(f);
    rejects("not JSON: refused", QByteArray("{ nope"), QStringLiteral("not JSON"));
    rejects("another kind of file: refused", QByteArray(R"({"kind":"something else","version":1,"epochs":3})"), QStringLiteral("not a file"));
    rejects("a newer version: refused, not misread", QByteArray(good).replace("\"version\": 1", "\"version\": 2"), QStringLiteral("newer"));
    rejects("no epoch count: refused", QByteArray(R"({"kind":"KoggerApp echogram edits","version":1})"), QStringLiteral("epoch count"));
    rejects("a trim ending before it starts: refused",
            QByteArray(R"({"kind":"KoggerApp echogram edits","version":1,"epochs":9,"trim":{"start":5,"end":5}})"), QStringLiteral("trim"));
    rejects("a backwards bad range: refused",
            QByteArray(R"({"kind":"KoggerApp echogram edits","version":1,"epochs":9,"bad":[[4,2]]})"), QStringLiteral("bad"));
    rejects("a value that is no number: refused",
            QByteArray(R"({"kind":"KoggerApp echogram edits","version":1,"epochs":9,"bottom":[{"address":1,"from":0,"values":["x"]}]})"),
            QStringLiteral("neither"));
    rejects("a side with no sub-channel, or past the 8th: refused",
            QByteArray(R"({"kind":"KoggerApp echogram edits","version":1,"epochs":9,"mosaicOut":[{"address":1,"sub":8,"ranges":[[1,2]]}]})"),
            QStringLiteral("malformed"));
    rejects("a run with no start: refused",
            QByteArray(R"({"kind":"KoggerApp echogram edits","version":1,"epochs":9,"limitMin":[{"address":1,"values":[1]}]})"),
            QStringLiteral("malformed"));
}

// A column of returns at 1 cm a sample: water noise, a return from `edge` on, fading; and a weaker
// one (a fish) over [fish, fish + 20) when fish >= 0.
QVector<uint8_t> column(int size, int edge, int noise, int level, int fish, Lcg& rnd)
{
    QVector<uint8_t> d(size);
    for (int i = 0; i < size; ++i) {
        float v = 8.0f + float(rnd.below(noise + 1));
        if (i >= edge) {
            v += float(level) * std::exp(-float(i - edge) / 300.0f);
        }
        if (fish >= 0 && i >= fish && i < fish + 20) {
            v += 60.0f;
        }
        d[i] = uint8_t(std::min(255.0f, v));
    }
    return d;
}

// The bottom the bottom track's filter finds in pings summed, looked for within [fromM, toM].
float peak(const QVector<QVector<uint8_t>>& pings, const TrackFilter& f, float fromM, float toM)
{
    QVector<qint64> sum;
    QVector<int32_t> one;
    int used = 0;
    for (const QVector<uint8_t>& p : pings) {
        int first = 0;
        int end = 0;
        if (!trackResponse(p.constData(), p.size(), 0.01f, f, one, &first, &end)) {
            continue;
        }
        sum.resize(std::max(sum.size(), one.size()));
        for (int i = first; i < end; ++i) {
            sum[i] += one[i];
        }
        ++used;
    }
    return trackPeakWithin(sum, used, 0.01f, f, fromM, toM);
}

void checkMagnet()
{
    TrackFilter oneBeam;
    oneBeam.threshold = 0.0f;
    TrackFilter sideScan = oneBeam;
    sideScan.preset = 2;

    Lcg rnd(7);
    QVector<QVector<uint8_t>> clean{ column(5000, 1000, 0, 150, -1, rnd) };
    const float d1 = peak(clean, oneBeam, 0.0f, 50.0f);
    const float d2 = peak(clean, sideScan, 0.0f, 50.0f);
    check(std::fabs(d1 - 10.0f) < 0.3f && std::fabs(d2 - 10.0f) < 0.6f,
          "a return from 10 m: the filter's peak there, one beam and side-scan",
          QStringLiteral("%1 m, %2 m").arg(d1).arg(d2));

    QVector<QVector<uint8_t>> withFish{ column(5000, 1200, 4, 150, 600, rnd) };
    const float nearFish = peak(withFish, oneBeam, 5.5f, 6.5f);
    const float nearBottom = peak(withFish, oneBeam, 11.5f, 12.5f);
    const float whole = peak(withFish, oneBeam, 0.0f, 50.0f);
    check(std::fabs(nearFish - 6.0f) < 0.3f && std::fabs(nearBottom - 12.0f) < 0.3f && std::fabs(whole - 12.0f) < 0.3f,
          "a fish above the bottom: found where it is looked for, the bottom when the window holds both",
          QStringLiteral("fish %1 m, bottom %2 m, both %3 m").arg(nearFish).arg(nearBottom).arg(whole));

    check(std::isnan(peak(withFish, oneBeam, 8.0f, 10.0f)) || peak(withFish, oneBeam, 8.0f, 10.0f) > 0.0f,
          "a window with no return: no answer, or the filter's own best there");
    // The bottom track's threshold is 1e7 x the setting in an int (as there): 100 stays inside it and is
    // over the ~4e7 the column above gives.
    TrackFilter strict = oneBeam;
    strict.threshold = 100.0f;
    check(std::isnan(peak(withFish, strict, 0.0f, 50.0f)), "nothing over the threshold: no answer, the pencil stays");

    QVector<int32_t> out;
    int first = 0;
    int end = 0;
    const QVector<uint8_t> tiny(8, 100);
    check(!trackResponse(tiny.constData(), tiny.size(), 0.01f, oneBeam, out, &first, &end)
              && !trackResponse(nullptr, 0, 0.01f, oneBeam, out, &first, &end)
              && std::isnan(trackPeakWithin(QVector<qint64>(), 1, 0.01f, oneBeam, 0.0f, 10.0f)),
          "a column too short, none at all: no response, no answer");

    // Noisy pings, eleven summed as the bottom track's window does: the bottom found where it is.
    float worst = 0.0f;
    for (int k = 0; k < 60; ++k) {
        const int edge = 400 + rnd.below(2500);
        QVector<QVector<uint8_t>> window;
        for (int p = 0; p < 11; ++p) {
            window.append(column(5000, edge + rnd.below(5), 40, 120, -1, rnd));
        }
        const float truth = float(edge) * 0.01f;
        const float got = peak(window, k % 2 ? sideScan : oneBeam, truth - 0.5f, truth + 0.5f);
        worst = std::max(worst, std::isnan(got) ? 99.0f : std::fabs(got - truth) / truth);
    }
    check(worst < 0.04f, "noisy pings, one beam and side-scan: the bottom within 4 % of its depth, 60 windows",
          QStringLiteral("worst %1 %").arg(double(worst) * 100.0, 0, 'f', 2));
}

void checkFilters()
{
    const float nan = NAN;
    check(medianAround({ 1.0f, 2.0f, nan, 100.0f, 3.0f }, 2, 2) == 3.0f && std::isnan(medianAround({ nan, nan }, 0, 1)),
          "the median of a ping's neighbours skips pings with no bottom");
    const QVector<float> flat{ 5.0f, 5.0f, 5.0f, 9.0f, 5.0f, 5.0f, 5.0f };
    check(smoothedAt(flat, 3, 2) == 5.0f && std::isnan(smoothedAt({ 5.0f, nan, 5.0f }, 1, 1)),
          "the smoothing brush takes a spike down to its neighbours, leaves a ping with no bottom alone");

    const QVector<float> gaps{ 1.0f, nan, nan, 4.0f, nan, 7.0f, nan };
    const QVector<float> f = filledGaps(gaps, 0, 7, 10);
    check(f[1] == 2.0f && f[2] == 3.0f && f[4] == 5.5f && std::isnan(f[0]) && std::isnan(f[3]) && std::isnan(f[6]),
          "gaps filled on the straight line between their bottoms; an open end is not");
    const QVector<float> part = filledGaps(gaps, 2, 3, 10);
    check(std::isnan(part[1]) && part[2] == 3.0f, "a brush over part of a gap fills that part, from the gap's own ends");
    QVector<float> longGap(30, nan);
    longGap[0] = 1.0f;
    longGap[29] = 2.0f;
    check(std::isnan(filledGaps(longGap, 0, 30, 10)[15]) && filledGaps(longGap, 0, 30, 30)[15] > 1.0f,
          "a gap longer than the limit is left open");
    QVector<float> fits(12, nan);    // 10 pings with no bottom
    fits[0] = 1.0f;
    fits[11] = 2.0f;
    QVector<float> over(13, nan);    // 11
    over[0] = 1.0f;
    over[12] = 2.0f;
    check(std::isfinite(filledGaps(fits, 0, 12, 10)[6]) && std::isnan(filledGaps(over, 0, 13, 10)[6]),
          "the limit of a gap: 10 pings filled with a limit of 10, 11 left open");

    // A bottom of 8..12 m over 3000 pings, 2 cm of noise, a cliff of 3 m, and 30 spikes of 1-3 pings.
    Lcg rnd(11);
    QVector<float> truth(3000);
    for (int i = 0; i < truth.size(); ++i) {
        truth[i] = 10.0f + 2.0f * std::sin(float(i) / 300.0f) + (i >= 1500 ? 3.0f : 0.0f);
    }
    QVector<float> d = truth;
    for (int i = 0; i < d.size(); ++i) {
        d[i] += 0.02f * (rnd.unit() - 0.5f);
    }
    QVector<int> spiked;
    for (int s = 0; s < 30; ++s) {
        const int at = 50 + s * 95;
        const int width = 1 + s % 3;
        for (int w = 0; w < width; ++w) {
            d[at + w] = truth[at + w] + (s % 2 ? -5.0f : 3.0f);
            spiked.append(at + w);
        }
    }
    const QVector<Spike> found = findSpikes(d, QVector<bool>(), 10, 4.0f, 0.2f, 0.05f);
    int hits = 0;
    int false_ = 0;
    float worst = 0.0f;
    for (const Spike& s : found) {
        if (spiked.contains(s.epoch)) {
            ++hits;
            worst = std::max(worst, std::fabs(s.median - truth[s.epoch]));
        }
        else {
            ++false_;
        }
    }
    check(hits == spiked.size() && false_ == 0 && worst < 0.05f,
          "spikes: all 60 pings of 30 spikes found, none elsewhere (not the 3 m cliff), each given the bottom around it",
          QStringLiteral("%1 of %2 found, %3 elsewhere, worst %4 m").arg(hits).arg(spiked.size()).arg(false_).arg(worst));
    QVector<bool> skip(d.size(), false);
    skip[spiked.first()] = true;
    const QVector<Spike> withSkip = findSpikes(d, skip, 10, 4.0f, 0.2f, 0.05f);
    check(withSkip.size() == found.size() - 1, "a ping set by hand is not judged a spike");

    // A lawnmower survey: lines 200 m north and south, 30 m apart, joined by half circles of 15 m,
    // all eastwards (a right turn at the north end, a left one at the south end); a ping every 0.1 m,
    // heading along the track with 1 degree of noise, in [0, 360) as a compass gives it (the lines
    // north cross 0), range 25 m.
    QVector<double> north;
    QVector<double> east;
    QVector<float> heading;
    QVector<int> inTurn;   // per ping: 1 on a half circle
    QVector<double> toTurn;   // along the track to the nearest half circle; 0 on one
    double e0 = 0.0;
    for (int line = 0; line < 4; ++line) {
        const bool up = line % 2 == 0;
        for (double s = 0.0; s < 200.0; s += 0.1) {
            north.append(up ? s : 200.0 - s);
            east.append(e0);
            heading.append(float(std::fmod((up ? 0.0 : 180.0) + (rnd.unit() - 0.5) + 360.0, 360.0)));
            inTurn.append(0);
            // The survey starts and ends on a line: those ends are no turn.
            toTurn.append(std::min(line > 0 ? s : 1e9, line < 3 ? 200.0 - s : 1e9));
        }
        if (line == 3) {
            break;
        }
        const double cn = up ? 200.0 : 0.0;   // the half circle's centre
        const double ce = e0 + 15.0;
        for (double a = 0.0; a < M_PI; a += 0.1 / 15.0) {
            // From the line's end (west of the centre) round to the next line (east of it).
            north.append(cn + (up ? 1.0 : -1.0) * 15.0 * std::sin(a));
            east.append(ce - 15.0 * std::cos(a));
            const double deg = a * 180.0 / M_PI;
            heading.append(float(std::fmod((up ? deg : 180.0 - deg) + 360.0, 360.0)));
            inTurn.append(1);
            toTurn.append(0.0);
        }
        e0 += 30.0;
    }
    const QVector<float> range(north.size(), 25.0f);
    // The editor's settings (echogram_editor.cpp): a base of 20 m, turns tighter than 1.5 ranges, a
    // margin of 5 m, a wobble shorter than 2 m of track and 30 degrees dropped.
    auto turnsOf = [](const QVector<double>& tn, const QVector<double>& te, const QVector<float>& th) {
        return findTurns(tn, te, th, QVector<float>(tn.size(), 25.0f), 20.0f, 1.5f, 5.0f, 2.0f, 30.0f);
    };
    const auto turns = turnsOf(north, east, heading);
    auto covered = [&](int i) {
        for (const auto& r : turns) {
            if (i >= r.first && i < r.second) {
                return true;
            }
        }
        return false;
    };
    int missedTurn = 0;
    int lineMarked = 0;
    int marginMissed = 0;
    for (int i = 0; i < north.size(); ++i) {
        if (inTurn[i] && !covered(i)) {
            ++missedTurn;
        }
        if (!inTurn[i] && toTurn[i] > 15.0 && covered(i)) {   // 15 m or more from a turn: in none
            ++lineMarked;
        }
        if (!inTurn[i] && toTurn[i] <= 4.0 && !covered(i)) {  // the margin of 5 m
            ++marginMissed;
        }
    }
    check(turns.size() == 3 && missedTurn == 0 && lineMarked == 0,
          "turns: the three half circles of a lawnmower survey found whole, the lines left alone",
          QStringLiteral("%1 turns, %2 turning pings missed, %3 line pings marked").arg(turns.size()).arg(missedTurn).arg(lineMarked));
    check(marginMissed == 0, "a turn is widened by its margin: the lines' last 4 m before and after it go too",
          QStringLiteral("%1 line pings near a turn not taken").arg(marginMissed));

    QVector<double> gn;
    QVector<double> ge;
    QVector<float> gh;
    for (double a = 0.0; a < 1.0; a += 0.0005) {   // a curve of 200 m radius, 200 m of it
        gn.append(200.0 * std::sin(a));
        ge.append(200.0 * (1.0 - std::cos(a)));
        gh.append(float(a * 180.0 / M_PI));
    }
    check(turnsOf(gn, ge, gh).isEmpty(), "a wide curve (200 m radius, a 25 m range) is no turn");
    QVector<double> sn(400, 0.0);
    QVector<double> se(400, 0.0);
    QVector<float> sh(400);
    for (int i = 0; i < sh.size(); ++i) {
        sn[i] = 0.001 * (i % 7);
        sh[i] = float(i) * 0.5f;   // turning on the spot
    }
    check(!turnsOf(sn, se, sh).isEmpty(), "a boat turning on the spot is a turn");

    // A straight line whose heading wobbles, as a small boat's yaw does along the lines of 00014: 8
    // degrees either way over swings of 6 m, a ping every 6.4 cm.
    QVector<double> wn;
    QVector<double> we;
    QVector<float> wh;
    for (double s = 0.0; s < 300.0; s += 0.064) {
        wn.append(s);
        we.append(0.0);
        wh.append(float(std::fmod(8.0 * std::sin(2.0 * M_PI * s / 6.0) + 360.0, 360.0)));
    }
    check(turnsOf(wn, we, wh).isEmpty(), "a straight line whose heading wobbles 8 degrees either way is no turn");
    QVector<float> glitch = QVector<float>(wn.size(), 0.0f);
    glitch[2000] = 40.0f;   // one ping's heading off
    check(turnsOf(wn, we, glitch).isEmpty(), "a heading off in one ping is no turn");
}

} // namespace

int main()
{
    checkBasics();
    checkWhatAStepChanges();
    checkHistory();
    checkRandomSessions();
    checkFile();
    checkMagnet();
    checkFilters();
    std::printf("echoedit_check: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
