#include "echogram_editor.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>

#include "data_processor.h"
#include "dataset.h"

namespace {

// After the last change: strokes one after another are one write.
constexpr int kSaveDelayMs = 1500;

// The filters' numbers (docs/echogram-editing.md, "Filters"). The smoothing brush: the median of 4 pings
// each side. Gaps up to 1000 pings (about a minute at 15 pings a second) are closed. A spike: farther
// from the median of the 10 pings each side than 4 robust deviations and than 0.2 m + 5 % of the depth.
// A turn: over 10 m of track the heading turns faster than the swath's far edge can follow the boat at
// 1.5 times its range, widened by 5 m of track.
constexpr int kSmoothHalf = 4;
constexpr int kMaxFillGap = 1000;
constexpr int kSpikeHalf = 10;
constexpr float kSpikeK = 4.0f;
constexpr float kSpikeMinAbsM = 0.2f;
constexpr float kSpikeMinRel = 0.05f;
constexpr float kTurnBaseM = 20.0f;        // 10 m took the yaw's wobble along the lines of 00014 (2026-10-18)
constexpr float kTurnRadiusFactor = 1.5f;
constexpr float kTurnMarginM = 5.0f;
constexpr float kTurnMinRunM = 2.0f;       // a shorter run of "turning" pings that turns less is a wobble
constexpr float kTurnMinDeg = 30.0f;

} // namespace

EchogramEditor::EchogramEditor(QObject* parent) :
    QObject(parent)
{
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(kSaveDelayMs);
    connect(&saveTimer_, &QTimer::timeout, this, &EchogramEditor::save);
}

EchogramEditor::~EchogramEditor() = default;   // saved at aboutToQuit (main.cpp): the dataset may be gone here

void EchogramEditor::setDataset(Dataset* dataset)
{
    if (dataset_) {
        disconnect(dataset_, nullptr, this, nullptr);
    }
    dataset_ = dataset;
    if (dataset_) {
        connect(dataset_, &Dataset::poolCleared, this, &EchogramEditor::onPoolCleared);
    }
}

void EchogramEditor::setDataProcessor(DataProcessor* dataProcessor)
{
    dataProcessor_ = dataProcessor;
}

void EchogramEditor::beginStroke(echoedit::Command::Kind kind)
{
    if (!dataset_) {
        return;
    }
    if (strokeDepth_++ > 0) {
        return;
    }
    strokeKind_ = kind;
    strokeBefore_.clear();
    strokeTrimBefore_ = dataset_->editTrim();
}

void EchogramEditor::touch(int epoch)
{
    if (strokeDepth_ <= 0 || !dataset_ || epoch < 0 || epoch >= dataset_->size()) {
        return;
    }
    if (!strokeBefore_.contains(epoch)) {
        strokeBefore_.insert(epoch, dataset_->pingEditState(epoch));
    }
}

void EchogramEditor::endStroke()
{
    if (strokeDepth_ <= 0) {
        return;
    }
    if (--strokeDepth_ > 0) {
        return;
    }
    if (!dataset_) {
        clearStroke();
        return;
    }

    echoedit::Command c;
    c.kind = strokeKind_;
    c.trimBefore = strokeTrimBefore_;
    c.trimAfter = dataset_->editTrim();
    QList<int> epochs = strokeBefore_.keys();
    std::sort(epochs.begin(), epochs.end());
    c.pings.reserve(epochs.size());
    for (int e : std::as_const(epochs)) {
        echoedit::PingChange p;
        p.epoch = e;
        p.before = strokeBefore_.value(e);
        p.after = dataset_->pingEditState(e);
        if (!echoedit::samePing(p.before, p.after)) {
            c.pings.append(p);
        }
    }
    clearStroke();
    if (c.isEmpty()) {
        return;
    }

    history_.push(c);
    afterChange(c, false, /*toolRanTrack*/ true);
    emit historyChanged();
}

void EchogramEditor::clearStroke()
{
    strokeDepth_ = 0;
    strokeBefore_.clear();
}

void EchogramEditor::markRange(int from, int to, bool bad)
{
    if (!dataset_ || strokeDepth_ <= 0) {
        return;
    }
    from = std::max(0, from);
    to = std::min(to, dataset_->size());
    if (from >= to) {
        return;
    }
    for (int e = from; e < to; ++e) {
        touch(e);
    }
    dataset_->setEpochsMarkedBad(from, to, bad);
    if (!bad) {
        for (int e = from; e < to; ++e) {   // unmarking is the way back into the mosaic as well
            echoedit::PingState s = dataset_->pingEditState(e);
            bool changed = false;
            for (echoedit::ChannelBottom& b : s.channels) {
                changed = changed || b.mosaicOut != 0;
                b.mosaicOut = 0;
            }
            if (changed) {
                dataset_->setPingEditState(e, s);
            }
        }
    }
    emit dataset_->dataUpdate();   // the shading on every echogram
}

void EchogramEditor::setMosaicOut(int from, int to, uint8_t address, uint8_t sub, bool out)
{
    if (!dataset_ || strokeDepth_ <= 0 || sub >= 8) {
        return;
    }
    const uint8_t bit = uint8_t(1u << sub);
    from = std::max(0, from);
    to = std::min(to, dataset_->size());
    for (int e = from; e < to; ++e) {
        echoedit::PingState s = dataset_->pingEditState(e);
        bool changed = false;
        for (echoedit::ChannelBottom& b : s.channels) {
            const uint8_t m = out ? uint8_t(b.mosaicOut | bit) : uint8_t(b.mosaicOut & ~bit);
            if (b.address == address && sub < b.subs && m != b.mosaicOut) {
                b.mosaicOut = m;
                changed = true;
            }
        }
        if (changed) {
            touch(e);
            dataset_->setPingEditState(e, s);
        }
    }
    emit dataset_->dataUpdate();
}

QVector<float> EchogramEditor::bottomsOf(int from, int to) const
{
    QVector<float> d;
    const QVector<DatasetChannel> channels = dataset_->channelsList();
    from = std::max(0, from);
    to = std::min(to, dataset_->size());
    d.reserve(std::max(0, to - from));
    for (int e = from; e < to; ++e) {
        const Epoch* ep = dataset_->fromIndex(e);
        const Epoch::DistProcessing* bp = (ep && !channels.isEmpty())
            ? ep->bottomProcessingOf(channels.first().channelId_, channels.first().subChannelId_) : nullptr;
        d.append(bp ? bp->distance : NAN);
    }
    return d;
}

void EchogramEditor::setHandBottom(int epoch, float distance)
{
    touch(epoch);
    echoedit::PingState s = dataset_->pingEditState(epoch);
    for (echoedit::ChannelBottom& b : s.channels) {
        b.distance = distance;
        b.source = echoedit::kSourceHand;
    }
    dataset_->setPingEditState(epoch, s);
}

void EchogramEditor::smoothRange(int from, int to)
{
    if (!dataset_ || strokeDepth_ <= 0) {
        return;
    }
    const int n = dataset_->size();
    from = std::max(0, from);
    to = std::min(to, n);
    const int a = std::max(0, from - kSmoothHalf);
    const QVector<float> d = bottomsOf(a, std::min(n, to + kSmoothHalf));
    QVector<float> smoothed;   // all from the depths before this pass, not from its own results
    for (int e = from; e < to; ++e) {
        smoothed.append(echoedit::smoothedAt(d, e - a, kSmoothHalf));
    }
    for (int e = from; e < to; ++e) {
        const float v = smoothed[e - from];
        if (std::isfinite(v) && v != d[e - a]) {
            setHandBottom(e, v);
        }
    }
    emit dataset_->dataUpdate();
}

void EchogramEditor::fillGapsRange(int from, int to)
{
    if (!dataset_ || strokeDepth_ <= 0) {
        return;
    }
    const int n = dataset_->size();
    from = std::max(0, from);
    to = std::min(to, n);
    const int a = std::max(0, from - kMaxFillGap - 1);
    const QVector<float> d = bottomsOf(a, std::min(n, to + kMaxFillGap + 1));
    const QVector<float> filled = echoedit::filledGaps(d, from - a, to - a, kMaxFillGap);
    for (int i = 0; i < filled.size(); ++i) {
        if (std::isfinite(filled[i])) {
            setHandBottom(a + i, filled[i]);
        }
    }
    emit dataset_->dataUpdate();
}

int EchogramEditor::removeSpikes()
{
    if (!dataset_ || inStroke()) {
        return 0;
    }
    const int n = dataset_->size();
    const QVector<float> d = bottomsOf(0, n);
    const QVector<DatasetChannel> channels = dataset_->channelsList();
    QVector<bool> skip(n, false);   // points set by hand, pings left out: neither judged nor neighbours
    for (int e = 0; e < n; ++e) {
        const Epoch* ep = dataset_->fromIndex(e);
        const Epoch::DistProcessing* bp = (ep && !channels.isEmpty())
            ? ep->bottomProcessingOf(channels.first().channelId_, channels.first().subChannelId_) : nullptr;
        skip[e] = dataset_->isEpochExcluded(e)
               || (bp && bp->source == Epoch::DistProcessing::DistanceSource::DistanceSourceDirectHand);
    }
    const QVector<echoedit::Spike> spikes = echoedit::findSpikes(d, skip, kSpikeHalf, kSpikeK, kSpikeMinAbsM, kSpikeMinRel);
    if (spikes.isEmpty()) {
        emit message(tr("No spikes of the bottom found"), false);
        return 0;
    }
    beginStroke(echoedit::Command::Despike);
    for (const echoedit::Spike& s : spikes) {
        setHandBottom(s.epoch, s.median);
    }
    endStroke();
    emit message(tr("Spikes of the bottom replaced with the bottom around them: %1 pings").arg(spikes.size()), false);
    return spikes.size();
}

int EchogramEditor::turnsOutOfMosaic()
{
    if (!dataset_ || inStroke()) {
        return 0;
    }
    const int n = dataset_->size();
    QVector<double> north(n, NAN);
    QVector<double> east(n, NAN);
    QVector<float> heading(n, NAN);
    QVector<float> range(n, NAN);
    for (int e = 0; e < n; ++e) {
        Epoch* ep = dataset_->fromIndex(e);
        if (!ep) {
            continue;
        }
        const NED ned = ep->getSonarPosition().ned;
        if (!ned.isCoordinatesValid()) {
            continue;
        }
        north[e] = ned.n;
        east[e] = ned.e;
        heading[e] = ep->tryRetValidYaw();   // the heading the mosaic lays the swath across
        range[e] = ep->getMaxRange();
    }
    const auto turns = echoedit::findTurns(north, east, heading, range, kTurnBaseM, kTurnRadiusFactor, kTurnMarginM,
                                           kTurnMinRunM, kTurnMinDeg);
    int pings = 0;
    int taken = 0;   // turns with a ping in use that went out
    beginStroke(echoedit::Command::Turns);
    for (const auto& r : turns) {
        const int pingsBefore = pings;
        for (int e = r.first; e < r.second; ++e) {
            if (dataset_->isEpochExcluded(e)) {
                continue;   // out of the mosaic already; its position still shaped the turn
            }
            echoedit::PingState s = dataset_->pingEditState(e);
            bool changed = false;
            for (echoedit::ChannelBottom& b : s.channels) {
                const uint8_t all = echoedit::allSides(b.subs);
                changed = changed || b.mosaicOut != all;
                b.mosaicOut = all;
            }
            if (changed) {
                touch(e);
                dataset_->setPingEditState(e, s);
                ++pings;
            }
        }
        taken += pings > pingsBefore;
    }
    endStroke();
    emit message(turns.isEmpty() ? tr("No turns found")
                                 : tr("Turns left out of the mosaic: %1, %2 pings").arg(taken).arg(pings), false);
    return pings;
}

void EchogramEditor::moveTrim(int epoch)
{
    if (!dataset_ || strokeDepth_ <= 0) {
        return;
    }
    const int n = dataset_->size();
    echoedit::Trim t = strokeTrimBefore_;
    if (strokeKind_ == echoedit::Command::TrimStart) {
        t.start = std::clamp(epoch, 0, std::max(0, n));
        if (t.end <= t.start) {
            t.end = echoedit::kNoTrimEnd;
        }
    }
    else if (strokeKind_ == echoedit::Command::TrimEnd) {
        t.end = epoch + 1;   // the ping under the cursor is kept
        if (t.end >= n) {
            t.end = echoedit::kNoTrimEnd;
        }
        if (t.end <= t.start) {
            t.start = 0;
        }
    }
    else {
        return;
    }
    if (t != dataset_->editTrim()) {
        dataset_->setEditTrim(t);
        emit dataset_->dataUpdate();
    }
}

int EchogramEditor::undoKind() const
{
    const echoedit::Command* c = history_.nextUndo();
    return c ? int(c->kind) : -1;
}

int EchogramEditor::redoKind() const
{
    const echoedit::Command* c = history_.nextRedo();
    return c ? int(c->kind) : -1;
}

void EchogramEditor::setMagnet(bool on)
{
    if (magnet_ == on) {
        return;
    }
    magnet_ = on;
    emit magnetChanged();
}

bool EchogramEditor::trimmed() const
{
    return dataset_ && dataset_->editTrim().active();
}

bool EchogramEditor::saving() const
{
    return !recordPath_.isEmpty() && files_ == 1 && !saveBlocked_;
}

QString EchogramEditor::editsFile() const
{
    return recordPath_.isEmpty() ? QString() : editsPathFor(recordPath_);
}

void EchogramEditor::undo()
{
    if (!dataset_ || inStroke() || !history_.canUndo()) {
        return;
    }
    const echoedit::Command c = history_.takeUndo();
    apply(c, true);
    emit historyChanged();
}

void EchogramEditor::redo()
{
    if (!dataset_ || inStroke() || !history_.canRedo()) {
        return;
    }
    const echoedit::Command c = history_.takeRedo();
    apply(c, false);
    emit historyChanged();
}

void EchogramEditor::clearTrim()
{
    if (!dataset_ || inStroke() || !dataset_->editTrim().active()) {
        return;
    }
    echoedit::Command c;
    c.kind = echoedit::Command::TrimClear;
    c.trimBefore = dataset_->editTrim();
    c.trimAfter = echoedit::Trim();
    history_.push(c);
    apply(c, false);
    emit historyChanged();
}

void EchogramEditor::resetAll()
{
    if (!dataset_ || inStroke()) {
        return;
    }
    echoedit::Command c;
    c.kind = echoedit::Command::Reset;
    c.trimBefore = dataset_->editTrim();
    c.trimAfter = echoedit::Trim();
    const int n = dataset_->size();
    for (int e = 0; e < n; ++e) {
        if (!dataset_->epochHasEdits(e)) {
            continue;
        }
        echoedit::PingChange p;
        p.epoch = e;
        p.before = dataset_->pingEditState(e);
        p.after = p.before;
        p.after.bad = false;
        for (echoedit::ChannelBottom& b : p.after.channels) {
            b.min = NAN;
            b.max = NAN;
            b.mosaicOut = 0;
            if (b.source == echoedit::kSourceHand) {
                b.source = echoedit::kSourceNone;   // the bottom track finds it again (afterChange)
                b.distance = NAN;
            }
        }
        c.pings.append(p);
    }

    if (saveBlocked_) {
        // The file that did not fit goes aside: what is written now is this recording's.
        const QString path = editsFile();
        QFile::remove(path + QStringLiteral(".bak"));
        if (!path.isEmpty() && QFile::exists(path) && !QFile::rename(path, path + QStringLiteral(".bak"))) {
            emit message(tr("Could not move the file of edits aside: %1").arg(path), true);
            return;
        }
        saveBlocked_ = false;
        dirty_ = true;
    }

    if (c.isEmpty()) {
        scheduleSave();
        return;
    }
    history_.push(c);
    apply(c, false);
    emit historyChanged();
}

void EchogramEditor::apply(const echoedit::Command& c, bool undo)
{
    for (const echoedit::PingChange& p : c.pings) {
        dataset_->setPingEditState(p.epoch, undo ? p.before : p.after);
    }
    if (c.trimBefore != c.trimAfter) {
        dataset_->setEditTrim(undo ? c.trimBefore : c.trimAfter);
    }
    afterChange(c, undo, false);
}

void EchogramEditor::afterChange(const echoedit::Command& c, bool undo, bool toolRanTrack)
{
    const int n = dataset_->size();

    // What changed: the pings of the command, and between the old and the new trim boundaries.
    QVector<QPair<int, int>> ranges;
    if (!c.pings.isEmpty()) {
        QVector<int> epochs;
        epochs.reserve(c.pings.size());
        for (const echoedit::PingChange& p : c.pings) {
            epochs.append(p.epoch);
        }
        ranges += echoedit::rangesOf(epochs);
    }
    if (c.trimBefore != c.trimAfter) {
        const int s0 = std::min(c.trimBefore.start, c.trimAfter.start);
        const int s1 = std::max(c.trimBefore.start, c.trimAfter.start);
        const int e0 = std::min(std::min(c.trimBefore.end, c.trimAfter.end), n);
        const int e1 = std::min(std::max(c.trimBefore.end, c.trimAfter.end), n);
        if (s0 < s1) {
            ranges.append(qMakePair(s0, std::min(s1, n)));
        }
        if (e0 < e1) {
            ranges.append(qMakePair(e0, e1));
        }
    }

    emit dataset_->dataUpdate();   // every echogram
    for (const auto& r : std::as_const(ranges)) {
        notifyRange(r.first, r.second);
    }

    if (!toolRanTrack) {   // a "raise" or "lower" stroke has started its run as it went
        const QVector<int> again = echoedit::epochsToTrackAgain(c, undo);
        for (const auto& r : echoedit::rangesOf(again)) {
            rerunBottomTrack(r.first, r.second);
        }
    }

    requestRebuild(echoedit::removesPoints(c, undo) || echoedit::removesFromMosaic(c, undo));
    scheduleSave();
    emit stateChanged();
}

void EchogramEditor::notifyRange(int from, int to)
{
    // The 3D bottom track (and through it the surface) takes the range again; it treats 0..size as
    // "everything anew", with other vertex numbers, so a whole range goes in two parts.
    const QVector<DatasetChannel> channels = dataset_->channelsList();
    const int n = dataset_->size();
    from = std::max(0, from);
    to = std::min(to, n);
    if (channels.isEmpty() || from >= to) {
        return;
    }
    const ChannelId first = channels.first().channelId_;
    if (from == 0 && to == n && n > 1) {
        emit dataset_->bottomTrackUpdated(first, 0, n - 1, true, false);
        emit dataset_->bottomTrackUpdated(first, n - 1, n, true, false);
        return;
    }
    emit dataset_->bottomTrackUpdated(first, from, to, true, false);
}

void EchogramEditor::rerunBottomTrack(int from, int to)
{
    if (!dataProcessor_) {
        return;
    }
    const QVector<DatasetChannel> channels = dataset_->channelsList();
    if (channels.isEmpty()) {
        return;
    }
    BottomTrackParam p = dataset_->getBottomTrackParam();
    p.indexFrom = from;
    p.indexTo = to;
    QMetaObject::invokeMethod(dataProcessor_, "bottomTrackProcessing", Qt::QueuedConnection,
                              Q_ARG(DatasetChannel, channels[0]),
                              Q_ARG(DatasetChannel, channels.size() >= 2 ? channels[1] : DatasetChannel()),
                              Q_ARG(BottomTrackParam, p),
                              Q_ARG(bool, true),
                              Q_ARG(bool, false));
}

void EchogramEditor::requestRebuild(bool withSurface)
{
    if (dataProcessor_) {
        QMetaObject::invokeMethod(dataProcessor_, "rebuildAfterEdit", Qt::QueuedConnection, Q_ARG(bool, withSurface));
    }
}

void EchogramEditor::scheduleSave()
{
    dirty_ = true;
    saveTimer_.start();   // also when nothing is saved (live data): hasEdits is counted then
}

void EchogramEditor::flush()
{
    saveTimer_.stop();
    save();
}

void EchogramEditor::save()
{
    if (!dataset_ || !dirty_) {
        return;
    }
    const echoedit::EditFile f = collect();
    if (hasEdits_ != !f.isEmpty()) {
        hasEdits_ = !f.isEmpty();
        emit stateChanged();
    }
    if (!saving()) {
        return;   // dirty stays: a recording opened later does not take these
    }
    const QString path = editsPathFor(recordPath_);
    if (f.isEmpty()) {
        if (QFile::exists(path) && !QFile::remove(path)) {
            emit message(tr("Could not remove the file of edits: %1").arg(path), true);
            return;
        }
        dirty_ = false;
        lastSaveError_.clear();
        return;
    }

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(echoedit::toJson(f)) < 0 || !out.commit()) {
        const QString why = out.errorString();
        qWarning().noquote() << QStringLiteral("echogram edits: not saved to %1: %2").arg(path, why);
        if (why != lastSaveError_) {   // once, not at every stroke
            emit message(tr("Echogram edits not saved: %1").arg(why), true);
        }
        lastSaveError_ = why;
        return;
    }
    dirty_ = false;
    lastSaveError_.clear();
    qInfo().noquote() << QStringLiteral("echogram edits: saved to %1 (%2 bad ranges, %3 runs drawn, trim %4..%5)")
                         .arg(path).arg(f.bad.size()).arg(f.bottom.size()).arg(f.trim.start)
                         .arg(f.trim.end == echoedit::kNoTrimEnd ? QStringLiteral("end") : QString::number(f.trim.end));
}

echoedit::EditFile EchogramEditor::collect() const
{
    echoedit::EditFile f;
    if (!dataset_) {
        return f;
    }
    const QFileInfo fi(recordPath_);
    f.record = fi.fileName();
    f.recordBytes = recordPath_.isEmpty() ? -1 : fi.size();
    f.epochs = dataset_->size();
    f.trim = dataset_->editTrim();
    dataset_->collectEditState(f);
    return f;
}

QString EchogramEditor::editsPathFor(const QString& recordPath)
{
    return recordPath + QStringLiteral(".kedit");
}

float EchogramEditor::magnetDistance(Dataset* dataset, int epoch, const ChannelId& ch1, uint8_t sub1,
                                     const ChannelId& ch2, uint8_t sub2, float dist, float radiusM)
{
    if (!dataset || !std::isfinite(dist) || epoch < 0 || epoch >= dataset->size()) {
        return dist;
    }
    const BottomTrackParam p = dataset->getBottomTrackParam();
    echoedit::TrackFilter f;
    f.preset = int(p.preset);
    f.gainSlope = p.gainSlope;
    f.threshold = p.threshold;

    // The pings of the bottom track's window around this one, each as the bottom track takes it: two
    // channels of the same length merged as (a + b) / 4, else the one there is.
    const int half = std::max(0, p.windowSize / 2);
    const int n = dataset->size();
    QVector<qint64> sum;
    QVector<int32_t> one;
    QVector<uint8_t> merged;
    float resolution = 0.0f;
    int pings = 0;
    for (int e = std::max(0, epoch - half); e <= std::min(n - 1, epoch + half); ++e) {
        Epoch* ep = dataset->fromIndex(e);
        if (!ep) {
            continue;
        }
        Epoch::Echogram* g1 = ep->chart(ch1, sub1);
        Epoch::Echogram* g2 = ch2.isValid() ? ep->chart(ch2, sub2) : nullptr;
        if (!g1 && g2) {
            std::swap(g1, g2);
        }
        if (!g1 || g1->amplitude.isEmpty() || !(g1->resolution > 0.0f)) {
            continue;
        }
        const uint8_t* data = g1->amplitude.constData();
        const int size = g1->amplitude.size();
        if (g2 && g2->resolution == g1->resolution && g2->amplitude.size() == size) {
            merged.resize(size);
            const uint8_t* d2 = g2->amplitude.constData();
            for (int i = 0; i < size; ++i) {
                merged[i] = uint8_t((uint16_t(data[i]) + uint16_t(d2[i])) >> 2);
            }
            data = merged.constData();
        }
        if (resolution == 0.0f) {
            resolution = g1->resolution;
        }
        else if (g1->resolution != resolution) {
            continue;   // another range setting: its samples are other distances
        }
        int first = 0;
        int end = 0;
        if (!echoedit::trackResponse(data, size, resolution, f, one, &first, &end)) {
            continue;
        }
        if (sum.size() < one.size()) {
            sum.resize(one.size());
        }
        for (int i = first; i < end; ++i) {
            sum[i] += one[i];
        }
        ++pings;
    }
    if (pings == 0) {
        return dist;
    }
    const float from = std::max(dist - std::fabs(radiusM), p.minDistance);
    const float to = std::min(dist + std::fabs(radiusM), p.maxDistance);
    const float got = echoedit::trackPeakWithin(sum, pings, resolution, f, from, to);
    return std::isfinite(got) ? got : dist;
}

void EchogramEditor::fileOpening(const QString& path, bool append)
{
    flush();   // the edits made so far go to their file before the epochs go
    if (!append) {
        recordPath_ = QFileInfo(path).absoluteFilePath();
        files_ = 1;
        saveBlocked_ = false;
        dirty_ = false;
        lastSaveError_.clear();
    }
    else {
        ++files_;
        if (files_ == 2) {
            emit message(tr("Echogram edits are not saved while several files are open together"), true);
        }
    }
    emit stateChanged();
}

void EchogramEditor::fileRead(bool processingStarted)
{
    if (!dataset_ || files_ != 1 || recordPath_.isEmpty() || dataset_->size() <= 0) {
        return;
    }
    const QString path = editsPathFor(recordPath_);
    QFile in(path);
    if (!in.exists()) {
        return;
    }
    if (!in.open(QIODevice::ReadOnly)) {
        saveBlocked_ = true;
        emit message(tr("The file of edits could not be read, edits are not saved: %1").arg(in.errorString()), true);
        emit stateChanged();
        return;
    }
    echoedit::EditFile f;
    QString why;
    if (!echoedit::fromJson(in.readAll(), &f, &why)) {
        saveBlocked_ = true;
        qWarning().noquote() << QStringLiteral("echogram edits: %1 not read: %2").arg(path, why);
        emit message(tr("The file of edits is not understood (%1); it is left as it is, edits are not saved").arg(why), true);
        emit stateChanged();
        return;
    }
    const qint64 bytes = QFileInfo(recordPath_).size();
    if (f.epochs != dataset_->size() || (f.recordBytes >= 0 && f.recordBytes != bytes)) {
        saveBlocked_ = true;
        qWarning().noquote() << QStringLiteral("echogram edits: %1 is for %2 pings and %3 bytes, the recording has %4 and %5")
                                .arg(path).arg(f.epochs).arg(f.recordBytes).arg(dataset_->size()).arg(bytes);
        emit message(tr("The file of edits does not fit this recording (%1 pings, the recording has %2); not applied, left as it is")
                     .arg(f.epochs).arg(dataset_->size()), true);
        emit stateChanged();
        return;
    }

    int skipped = 0;
    const int pings = applyFile(f, &skipped);
    qInfo().noquote() << QStringLiteral("echogram edits: read %1 (%2 pings, %3 bad ranges, trim %4..%5, %6 values for channels not found)")
                         .arg(path).arg(pings).arg(f.bad.size()).arg(f.trim.start)
                         .arg(f.trim.end == echoedit::kNoTrimEnd ? QStringLiteral("end") : QString::number(f.trim.end))
                         .arg(skipped);
    if (skipped > 0) {
        emit message(tr("Some echogram edits are for channels this recording lacks; %1 left out").arg(skipped), true);
    }

    if (processingStarted) {
        // The bottom track and the products may have run before the edits were there: once more.
        QVector<int> limited;
        for (const auto* runs : { &f.limitMin, &f.limitMax }) {
            for (const echoedit::Run& r : *runs) {
                for (int i = 0; i < r.values.size(); ++i) {
                    limited.append(r.from + i);
                }
            }
        }
        std::sort(limited.begin(), limited.end());
        limited.erase(std::unique(limited.begin(), limited.end()), limited.end());
        for (const auto& r : echoedit::rangesOf(limited)) {
            rerunBottomTrack(r.first, r.second);
        }
        notifyRange(0, dataset_->size());
        requestRebuild(true);
    }

    hasEdits_ = !f.isEmpty();
    emit stateChanged();
    emit editsLoaded(pings);
    if (pings > 0 || f.trim.active()) {
        emit message(tr("Echogram edits restored: %1").arg(QFileInfo(path).fileName()), false);
    }
}

int EchogramEditor::applyFile(const echoedit::EditFile& f, int* skipped)
{
    const int n = dataset_->size();
    dataset_->setEditTrim(f.trim);
    QSet<int> touched;
    for (const auto& r : f.bad) {
        dataset_->setEpochsMarkedBad(r.first, r.second, true);
        for (int e = std::max(0, r.first); e < std::min(r.second, n); ++e) {
            touched.insert(e);
        }
    }

    QHash<int, echoedit::PingState> states;
    auto stateOf = [&](int e) -> echoedit::PingState& {
        auto it = states.find(e);
        if (it == states.end()) {
            it = states.insert(e, dataset_->pingEditState(e));
        }
        return it.value();
    };
    enum class Field { Bottom, Min, Max };
    auto put = [&](const QVector<echoedit::Run>& runs, Field field) {
        for (const echoedit::Run& r : runs) {
            for (int i = 0; i < r.values.size(); ++i) {
                const int e = r.from + i;
                if (e < 0 || e >= n) {
                    ++*skipped;
                    continue;
                }
                bool found = false;
                for (echoedit::ChannelBottom& b : stateOf(e).channels) {
                    if (b.address != r.address) {
                        continue;
                    }
                    found = true;
                    if (field == Field::Bottom) {
                        b.distance = r.values[i];
                        b.source = echoedit::kSourceHand;
                    }
                    else if (field == Field::Min) {
                        b.min = r.values[i];
                    }
                    else {
                        b.max = r.values[i];
                    }
                }
                if (!found) {
                    ++*skipped;
                }
            }
        }
    };
    put(f.bottom, Field::Bottom);
    put(f.limitMin, Field::Min);
    put(f.limitMax, Field::Max);
    for (const echoedit::EditFile::Side& side : f.mosaicOut) {
        for (const auto& r : side.ranges) {
            for (int e = r.first; e < r.second; ++e) {
                if (e < 0 || e >= n) {
                    ++*skipped;
                    continue;
                }
                bool found = false;
                for (echoedit::ChannelBottom& b : stateOf(e).channels) {
                    if (b.address == side.address && side.sub < b.subs) {
                        b.mosaicOut |= uint8_t(1u << side.sub);
                        found = true;
                    }
                }
                if (!found) {
                    ++*skipped;
                }
            }
        }
    }
    for (auto it = states.cbegin(); it != states.cend(); ++it) {
        dataset_->setPingEditState(it.key(), it.value());
        touched.insert(it.key());
    }
    return touched.size();
}

void EchogramEditor::fileClosed()
{
    flush();
    recordPath_.clear();
    files_ = 0;
    saveBlocked_ = false;
    dirty_ = false;
    emit stateChanged();
}

void EchogramEditor::onPoolCleared()
{
    // The epochs went (a file closed or opened, a reset): nothing kept refers to them now. What was
    // unsaved went to its file before (Core calls flush ahead of the reset).
    clearStroke();
    history_.clear();
    saveTimer_.stop();
    dirty_ = false;
    hasEdits_ = false;
    emit historyChanged();
    emit stateChanged();
}
