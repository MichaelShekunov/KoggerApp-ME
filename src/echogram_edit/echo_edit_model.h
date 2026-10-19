#pragma once

// Echogram edits: what EchogramEditor keeps, undoes and saves, without the dataset, so that
// tools/echoedit_check tests it alone (Qt Core only). docs/echogram-editing.md.

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

#include <cmath>
#include <cstdint>
#include <limits>

namespace echoedit {

constexpr int kNoTrimEnd = std::numeric_limits<int>::max();

// The values of Epoch::DistProcessing::DistanceSource the model needs (checked in dataset.cpp).
constexpr uint8_t kSourceNone = 0;
constexpr uint8_t kSourceHand = 4;   // DistanceSourceDirectHand: drawn or erased by hand

// One channel's bottom in one ping: the fields of Epoch::DistProcessing a hand changes (its sub-channels
// share them), and which of its sub-channels' sides are left out of the mosaic
// (Epoch::Echogram::mosaicOut). The two sides of a DSSS pair are sub-channels 0 and 1 of one channel.
struct ChannelBottom {
    uint8_t address = 0;   // ChannelId::address; the channels of a file share its uuid
    float distance = NAN;
    float min = NAN;       // the search limits "raise" and "lower" set; NaN: none
    float max = NAN;
    uint8_t source = 0;    // Epoch::DistProcessing::DistanceSource
    uint8_t mosaicOut = 0; // bit s: sub-channel s's side not painted into the mosaic; its depth stays
    uint8_t subs = 1;      // the channel's sub-channels in this ping; the data's, not an edit (not compared)
};

// Every side of a channel with that many sub-channels, as mosaicOut bits.
constexpr uint8_t allSides(uint8_t subs) { return subs >= 8 ? uint8_t(0xFF) : uint8_t((1u << subs) - 1u); }

// NaN equals NaN here: a ping whose bottom stays erased is unchanged.
bool sameBottom(const ChannelBottom& a, const ChannelBottom& b);

// What an edit can change in one ping: the "bad" mark and the bottom of each of its channels.
struct PingState {
    bool bad = false;
    QVector<ChannelBottom> channels;
};

bool samePing(const PingState& a, const PingState& b);

struct PingChange {
    int epoch = -1;
    PingState before;
    PingState after;
};

// The pings kept: [start, end). end == kNoTrimEnd: up to the last, the ones still to come included.
struct Trim {
    int start = 0;
    int end = kNoTrimEnd;

    bool active() const { return start > 0 || end != kNoTrimEnd; }
    bool trims(int epoch) const { return epoch < start || epoch >= end; }
    bool operator==(const Trim& o) const { return start == o.start && end == o.end; }
    bool operator!=(const Trim& o) const { return !(*this == o); }
};

// One undoable step: a stroke of a tool, a trim, a reset.
struct Command {
    enum Kind : uint8_t {
        Draw = 0,     // the pencil
        Erase,        // the eraser
        Raise,        // "raise": the bottom looked for no farther than the stroke (DistProcessing::max)
        Lower,        // "lower": no nearer than the stroke (DistProcessing::min)
        Mark,         // pings marked bad
        Unmark,       // the mark taken off
        Delete3d,     // bottom-track points deleted in the 3D view
        TrimStart,
        TrimEnd,
        TrimClear,
        Reset,        // every edit of the recording taken back
        Smooth,       // the bottom brushed with the median of its neighbours
        FillGaps,     // pings with no bottom given one between the bottoms around them
        MosaicOut,    // a side of pings left out of the mosaic, their depths kept
        Despike,      // spikes of the bottom replaced with the median around them, the whole recording
        Turns         // the turns left out of the mosaic, the whole recording
    };

    Kind kind = Draw;
    QVector<PingChange> pings;   // by epoch, ascending, one entry an epoch
    Trim trimBefore;
    Trim trimAfter;

    bool isEmpty() const { return pings.isEmpty() && trimBefore == trimAfter; }
    int firstEpoch() const;      // over pings; -1 when there are none
    int lastEpoch() const;
    bool changesLimits() const;  // a min or a max changed: the bottom track is to be found again there
    bool changesMarks() const;
};

// A ping takes part in the mosaic, the bottom surface and the exports when it is not marked bad, not
// trimmed, and its first channel has a bottom.
bool takesPart(const PingState& p, int epoch, const Trim& trim);

// Applying the command (undo: its reverse) takes a ping out that took part. The bottom surface
// cannot drop a point, so it is then built again (DataProcessor::rebuildAfterEdit).
bool removesPoints(const Command& c, bool undo);

// Applying the command (undo: its reverse) takes a side of a ping out of the mosaic, the ping staying in
// the rest. The mosaic's own restart keeps the heights it wrote where it paints no more
// (ComputeWorker::clearMosaic), and the surface reads them, so the products are then built again from
// nothing, as when points go.
bool removesFromMosaic(const Command& c, bool undo);

// Applying the command (undo: its reverse) leaves pings whose bottom is the bottom track's to find
// again. The epochs, ascending. Forward (a redo, a reset): their search limits changed, or a point set
// by hand gave way -- the depths kept with the step may predate the run its tool started. Undone: only
// where a search limit comes back; elsewhere the depths kept from before the step are exact, and a run
// over a stretch would not give them back (its first pings are found with a shorter window).
QVector<int> epochsToTrackAgain(const Command& c, bool undo);

// The steps to undo and to redo. A new step drops the redo side; past the limits the oldest go.
class History {
public:
    explicit History(int maxSteps = 100, int maxPingChanges = 2000000);

    void push(Command c);
    bool canUndo() const { return !undo_.isEmpty(); }
    bool canRedo() const { return !redo_.isEmpty(); }
    const Command* nextUndo() const { return undo_.isEmpty() ? nullptr : &undo_.last(); }
    const Command* nextRedo() const { return redo_.isEmpty() ? nullptr : &redo_.last(); }
    Command takeUndo();          // to the redo side; the caller applies its "before"
    Command takeRedo();          // to the undo side; the caller applies its "after"
    void clear();
    int undoCount() const { return undo_.size(); }
    int redoCount() const { return redo_.size(); }

private:
    void trim();

    QVector<Command> undo_;
    QVector<Command> redo_;
    int maxSteps_;
    int maxPingChanges_;
};

// Epochs, ascending, as [from, to) ranges of consecutive ones.
QVector<QPair<int, int>> rangesOf(const QVector<int>& sortedEpochs);

// Values of consecutive epochs of one channel.
struct Run {
    uint8_t address = 0;
    int from = 0;
    QVector<float> values;       // NaN: erased by hand (bottom), none (limits)
};

// Appends the value of an epoch to the channel's last run when the epoch follows it, else opens a run.
// Epochs are to come ascending per channel.
void appendToRuns(QVector<Run>& runs, uint8_t address, int epoch, float value);

// The file of edits next to a recording (<recording>.kedit). Indices are the recording's epochs, as
// KoggerApp reads it; the size and the epoch count say whether it still is that recording.
struct EditFile {
    static constexpr int kVersion = 1;

    QString record;              // the recording's file name, for a person reading the file
    qint64 recordBytes = -1;
    int epochs = -1;
    Trim trim;
    QVector<QPair<int, int>> bad;   // [from, to)
    QVector<Run> bottom;         // drawn by hand; NaN: erased by hand
    QVector<Run> limitMin;       // "lower": the bottom is looked for no nearer than this
    QVector<Run> limitMax;       // "raise": no farther than this
    struct Side {
        uint8_t address = 0;
        uint8_t sub = 0;                   // the sub-channel: a side of a DSSS pair
        QVector<QPair<int, int>> ranges;   // [from, to): this side out of the mosaic
    };
    QVector<Side> mosaicOut;

    bool isEmpty() const;
};

// Adds an epoch to the side's ranges of mosaicOut (epochs ascending per side).
void appendToSides(QVector<EditFile::Side>& sides, uint8_t address, uint8_t sub, int epoch);

// ---- Filters of the bottom line (the palette's brushes and its whole-recording buttons). Each works
// on the depths of one channel, one value an epoch, NaN where a ping has none, and leaves to the
// editor what to do with the result (a step, hand points). ----

// The median of the finite values over [epoch - half, epoch + half]; NaN when there are none.
float medianAround(const QVector<float>& d, int epoch, int half);

// The smoothing brush (Hydromagic's median filter, applied where the brush goes and as often as it
// passes): the median of the neighbours of a ping that has a bottom. NaN for a ping without one.
float smoothedAt(const QVector<float>& d, int epoch, int half);

// Pings with no bottom between two that have one get the straight line between them (HYPACK's
// "interpolate" instead of "delete"). Only gaps of [from, to) closed on both sides within maxGap pings
// are filled; the result has the new values of those pings and NaN elsewhere.
QVector<float> filledGaps(const QVector<float>& d, int from, int to, int maxGap);

// Spikes of the bottom: a ping whose depth is farther from the median of the 2*half + 1 around it than
// k robust deviations (1.4826 x the median absolute deviation there) and than minAbs + minRel x the
// median. Pings in `skip` (set by hand, left out) neither count as neighbours nor are judged. The
// spikes ascending, each with the median it is replaced with (MAD and median after the FIG 2025
// single-beam sea-spike paper; the numbers are the editor's).
struct Spike {
    int epoch = -1;
    float median = NAN;
};
QVector<Spike> findSpikes(const QVector<float>& d, const QVector<bool>& skip, int half, float k,
                          float minAbs, float minRel);

// Turns of the survey, where a ping's side-scan picture lands on its neighbours'. The mosaic lays a
// ping's swath across its heading, so where the heading turns by dpsi over a distance ds the far edge of
// the swath moves range x dpsi while the boat moves ds: with range x |dpsi/ds| > 1 the inner side folds
// back over itself. Per ping (positions north and east in m, heading in degrees, range in m; NaN where
// a ping has none): the heading change between the pings baseM/2 behind and ahead of it (straight-line
// distance; a boat turning on the spot is a turn) over the distance between them, judged against
// 1 / (radiusFactor x range). For a steady turn that is its radius against radiusFactor x range,
// whatever baseM; a longer base averages the heading's wobble out. A run of pings judged turning that
// is shorter than minRunM of track and turns less than minTurnDeg is the heading's wobble, not a turn.
// The rest, widened by marginM of track on each side, as [from, to) ranges.
QVector<QPair<int, int>> findTurns(const QVector<double>& north, const QVector<double>& east,
                                   const QVector<float>& headingDeg, const QVector<float>& rangeM,
                                   float baseM, float radiusFactor, float marginM, float minRunM, float minTurnDeg);

QByteArray toJson(const EditFile& f);
bool fromJson(const QByteArray& json, EditFile* out, QString* error);

// The pencil's magnet looks for the bottom as the bottom track does, only near the pencil: this is the
// bottom track's per-ping filter (BottomTrackProcessor::bottomTrackProcessing) word for word -- the
// moving average that widens with range, the five taps of the preset, the gain with range -- and its
// peak search. A rule of its own ("the strongest rise near the cursor") landed within 10 cm of the
// bottom track's bottom on 27 % of the pings of 00014 from 0.3 m below and 7 % from above (2026-10-18).
struct TrackFilter {
    int preset = 0;            // BottomTrackPreset: 0 one beam, 1 one beam narrow, 2 side-scan
    float gainSlope = 1.0f;
    float threshold = 1.0f;
};

// One ping's response: out[i] for i in [*first, *end), 0 elsewhere (out is resized to size). False
// when the column is too short to have one.
bool trackResponse(const uint8_t* data, int size, float resolution, const TrackFilter& f,
                   QVector<int32_t>& out, int* first, int* end);

// The responses of `pings` pings summed (the bottom track's window): the distance of their peak within
// [fromM, toM], as the bottom track reports a peak; NaN when none passes its threshold.
float trackPeakWithin(const QVector<qint64>& sum, int pings, float resolution, const TrackFilter& f,
                      float fromM, float toM);

} // namespace echoedit
