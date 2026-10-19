#include "echo_edit_model.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cstdlib>

namespace echoedit {

namespace {

const char kKind[] = "KoggerApp echogram edits";

bool sameFloat(float a, float b)
{
    return (std::isnan(a) && std::isnan(b)) || a == b;
}

bool firstHasBottom(const PingState& p)
{
    return !p.channels.isEmpty() && std::isfinite(p.channels.first().distance);
}

// The float as it is: read back, it is the same float, and a recording opened again is built from the
// same depths (rounded to 0.1 mm, a drawn stretch came back 36 um off, 2026-10-18).
QJsonValue toValue(float v)
{
    if (!std::isfinite(v)) {
        return QJsonValue(QJsonValue::Null);
    }
    return QJsonValue(double(v));
}

QJsonArray runsToJson(const QVector<Run>& runs)
{
    QJsonArray out;
    for (const Run& r : runs) {
        QJsonArray values;
        for (float v : r.values) {
            values.append(toValue(v));
        }
        QJsonObject o;
        o.insert(QStringLiteral("address"), int(r.address));
        o.insert(QStringLiteral("from"), r.from);
        o.insert(QStringLiteral("values"), values);
        out.append(o);
    }
    return out;
}

bool runsFromJson(const QJsonValue& v, const char* what, QVector<Run>* out, QString* error)
{
    out->clear();
    if (v.isUndefined() || v.isNull()) {
        return true;
    }
    if (!v.isArray()) {
        *error = QStringLiteral("\"%1\" is not a list").arg(QLatin1String(what));
        return false;
    }
    for (const QJsonValue& item : v.toArray()) {
        const QJsonObject o = item.toObject();
        const int address = o.value(QStringLiteral("address")).toInt(-1);
        const int from = o.value(QStringLiteral("from")).toInt(-1);
        const QJsonValue values = o.value(QStringLiteral("values"));
        if (address < 0 || address > 255 || from < 0 || !values.isArray()) {
            *error = QStringLiteral("a run of \"%1\" is malformed").arg(QLatin1String(what));
            return false;
        }
        Run r;
        r.address = uint8_t(address);
        r.from = from;
        for (const QJsonValue& x : values.toArray()) {
            if (x.isNull()) {
                r.values.append(NAN);
            }
            else if (x.isDouble()) {
                r.values.append(float(x.toDouble()));
            }
            else {
                *error = QStringLiteral("a value of \"%1\" is neither a number nor null").arg(QLatin1String(what));
                return false;
            }
        }
        out->append(r);
    }
    return true;
}

qint64 pingCount(const Command& c)
{
    return c.pings.size();
}

} // namespace

bool sameBottom(const ChannelBottom& a, const ChannelBottom& b)
{
    return a.address == b.address && a.source == b.source && a.mosaicOut == b.mosaicOut
        && sameFloat(a.distance, b.distance) && sameFloat(a.min, b.min) && sameFloat(a.max, b.max);
}

bool samePing(const PingState& a, const PingState& b)
{
    if (a.bad != b.bad || a.channels.size() != b.channels.size()) {
        return false;
    }
    for (int i = 0; i < a.channels.size(); ++i) {
        if (!sameBottom(a.channels[i], b.channels[i])) {
            return false;
        }
    }
    return true;
}

int Command::firstEpoch() const
{
    return pings.isEmpty() ? -1 : pings.first().epoch;
}

int Command::lastEpoch() const
{
    return pings.isEmpty() ? -1 : pings.last().epoch;
}

bool Command::changesLimits() const
{
    for (const PingChange& p : pings) {
        const int n = std::min(p.before.channels.size(), p.after.channels.size());
        for (int i = 0; i < n; ++i) {
            if (!sameFloat(p.before.channels[i].min, p.after.channels[i].min)
                || !sameFloat(p.before.channels[i].max, p.after.channels[i].max)) {
                return true;
            }
        }
    }
    return false;
}

bool Command::changesMarks() const
{
    for (const PingChange& p : pings) {
        if (p.before.bad != p.after.bad) {
            return true;
        }
    }
    return false;
}

bool takesPart(const PingState& p, int epoch, const Trim& trim)
{
    return !p.bad && !trim.trims(epoch) && firstHasBottom(p);
}

bool removesPoints(const Command& c, bool undo)
{
    const Trim& from = undo ? c.trimAfter : c.trimBefore;
    const Trim& to = undo ? c.trimBefore : c.trimAfter;
    if (to.start > from.start || to.end < from.end) {
        return true;
    }
    for (const PingChange& p : c.pings) {
        const PingState& was = undo ? p.after : p.before;
        const PingState& now = undo ? p.before : p.after;
        if (takesPart(was, p.epoch, from) && !takesPart(now, p.epoch, to)) {
            return true;
        }
    }
    return false;
}

bool removesFromMosaic(const Command& c, bool undo)
{
    const Trim& from = undo ? c.trimAfter : c.trimBefore;
    const Trim& to = undo ? c.trimBefore : c.trimAfter;
    for (const PingChange& p : c.pings) {
        const PingState& was = undo ? p.after : p.before;
        const PingState& now = undo ? p.before : p.after;
        if (!takesPart(was, p.epoch, from) || !takesPart(now, p.epoch, to)) {
            continue;   // not in the mosaic before, or out of it whole now: removesPoints' case
        }
        for (const ChannelBottom& b : now.channels) {
            uint8_t before = 0;
            for (const ChannelBottom& w : was.channels) {
                if (w.address == b.address) {
                    before = w.mosaicOut;
                }
            }
            if ((b.mosaicOut & ~before) != 0) {
                return true;
            }
        }
    }
    return false;
}

QVector<int> epochsToTrackAgain(const Command& c, bool undo)
{
    QVector<int> out;
    for (const PingChange& p : c.pings) {
        const PingState& from = undo ? p.after : p.before;
        const PingState& to = undo ? p.before : p.after;
        bool again = false;
        for (const ChannelBottom& t : to.channels) {
            if (t.source == kSourceHand) {
                continue;   // the hand's: no run changes it
            }
            const bool limited = std::isfinite(t.min) || std::isfinite(t.max);
            for (const ChannelBottom& f : from.channels) {
                if (f.address != t.address) {
                    continue;
                }
                const bool limitsChanged = !sameFloat(f.min, t.min) || !sameFloat(f.max, t.max);
                if (undo ? (limited && limitsChanged) : (limitsChanged || f.source == kSourceHand)) {
                    again = true;
                }
            }
        }
        if (again) {
            out.append(p.epoch);
        }
    }
    return out;
}

History::History(int maxSteps, int maxPingChanges) :
    maxSteps_(std::max(1, maxSteps)),
    maxPingChanges_(std::max(1, maxPingChanges))
{
}

void History::push(Command c)
{
    if (c.isEmpty()) {
        return;
    }
    redo_.clear();
    undo_.append(std::move(c));
    trim();
}

Command History::takeUndo()
{
    if (undo_.isEmpty()) {
        return Command();
    }
    Command c = undo_.takeLast();
    redo_.append(c);
    return c;
}

Command History::takeRedo()
{
    if (redo_.isEmpty()) {
        return Command();
    }
    Command c = redo_.takeLast();
    undo_.append(c);
    return c;
}

void History::clear()
{
    undo_.clear();
    redo_.clear();
}

void History::trim()
{
    qint64 total = 0;
    for (const Command& c : undo_) {
        total += pingCount(c);
    }
    // The newest step stays even when it alone is over the limit: what was just done can be undone.
    while (undo_.size() > 1 && (undo_.size() > maxSteps_ || total > maxPingChanges_)) {
        total -= pingCount(undo_.first());
        undo_.removeFirst();
    }
}

QVector<QPair<int, int>> rangesOf(const QVector<int>& sortedEpochs)
{
    QVector<QPair<int, int>> out;
    for (int e : sortedEpochs) {
        if (!out.isEmpty() && out.last().second == e) {
            out.last().second = e + 1;
        }
        else if (out.isEmpty() || out.last().second < e) {
            out.append(qMakePair(e, e + 1));
        }
    }
    return out;
}

void appendToRuns(QVector<Run>& runs, uint8_t address, int epoch, float value)
{
    for (int i = runs.size() - 1; i >= 0; --i) {
        Run& r = runs[i];
        if (r.address != address) {
            continue;
        }
        if (r.from + r.values.size() == epoch) {
            r.values.append(value);
            return;
        }
        break;   // the channel's last run ended before: a new one
    }
    Run r;
    r.address = address;
    r.from = epoch;
    r.values.append(value);
    runs.append(r);
}

bool EditFile::isEmpty() const
{
    return !trim.active() && bad.isEmpty() && bottom.isEmpty() && limitMin.isEmpty() && limitMax.isEmpty()
        && mosaicOut.isEmpty();
}

void appendToSides(QVector<EditFile::Side>& sides, uint8_t address, uint8_t sub, int epoch)
{
    EditFile::Side* side = nullptr;
    for (EditFile::Side& s : sides) {
        if (s.address == address && s.sub == sub) {
            side = &s;
            break;
        }
    }
    if (!side) {
        sides.append(EditFile::Side());
        side = &sides.last();
        side->address = address;
        side->sub = sub;
    }
    if (!side->ranges.isEmpty() && side->ranges.last().second == epoch) {
        side->ranges.last().second = epoch + 1;
    }
    else if (side->ranges.isEmpty() || side->ranges.last().second < epoch) {
        side->ranges.append(qMakePair(epoch, epoch + 1));
    }
}

QByteArray toJson(const EditFile& f)
{
    QJsonObject o;
    o.insert(QStringLiteral("kind"), QLatin1String(kKind));
    o.insert(QStringLiteral("version"), EditFile::kVersion);
    o.insert(QStringLiteral("record"), f.record);
    o.insert(QStringLiteral("recordBytes"), double(f.recordBytes));
    o.insert(QStringLiteral("epochs"), f.epochs);

    QJsonObject trim;
    trim.insert(QStringLiteral("start"), f.trim.start);
    trim.insert(QStringLiteral("end"), f.trim.end == kNoTrimEnd ? QJsonValue(QJsonValue::Null) : QJsonValue(f.trim.end));
    o.insert(QStringLiteral("trim"), trim);

    QJsonArray bad;
    for (const auto& r : f.bad) {
        bad.append(QJsonArray{ r.first, r.second });
    }
    o.insert(QStringLiteral("bad"), bad);
    o.insert(QStringLiteral("bottom"), runsToJson(f.bottom));
    o.insert(QStringLiteral("limitMin"), runsToJson(f.limitMin));
    o.insert(QStringLiteral("limitMax"), runsToJson(f.limitMax));

    QJsonArray sides;
    for (const EditFile::Side& s : f.mosaicOut) {
        QJsonArray ranges;
        for (const auto& r : s.ranges) {
            ranges.append(QJsonArray{ r.first, r.second });
        }
        sides.append(QJsonObject{ { QStringLiteral("address"), int(s.address) }, { QStringLiteral("sub"), int(s.sub) },
                                  { QStringLiteral("ranges"), ranges } });
    }
    o.insert(QStringLiteral("mosaicOut"), sides);
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

bool fromJson(const QByteArray& json, EditFile* out, QString* error)
{
    QString dummy;
    if (!error) {
        error = &dummy;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = QStringLiteral("not JSON: %1").arg(pe.errorString());
        return false;
    }
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("kind")).toString() != QLatin1String(kKind)) {
        *error = QStringLiteral("not a file of echogram edits");
        return false;
    }
    const int version = o.value(QStringLiteral("version")).toInt(0);
    if (version < 1) {
        *error = QStringLiteral("no version");
        return false;
    }
    if (version > EditFile::kVersion) {
        *error = QStringLiteral("version %1, made by a newer KoggerApp").arg(version);
        return false;
    }

    EditFile f;
    f.record = o.value(QStringLiteral("record")).toString();
    f.recordBytes = qint64(o.value(QStringLiteral("recordBytes")).toDouble(-1));
    f.epochs = o.value(QStringLiteral("epochs")).toInt(-1);
    if (f.epochs < 0) {
        *error = QStringLiteral("no epoch count");
        return false;
    }

    const QJsonObject trim = o.value(QStringLiteral("trim")).toObject();
    f.trim.start = trim.value(QStringLiteral("start")).toInt(0);
    const QJsonValue end = trim.value(QStringLiteral("end"));
    f.trim.end = (end.isNull() || end.isUndefined()) ? kNoTrimEnd : end.toInt(-1);
    if (f.trim.start < 0 || f.trim.end <= f.trim.start) {
        *error = QStringLiteral("the trim is malformed");
        return false;
    }

    for (const QJsonValue& v : o.value(QStringLiteral("bad")).toArray()) {
        const QJsonArray r = v.toArray();
        const int from = r.size() == 2 ? r.at(0).toInt(-1) : -1;
        const int to = r.size() == 2 ? r.at(1).toInt(-1) : -1;
        if (from < 0 || to <= from) {
            *error = QStringLiteral("a range of \"bad\" is malformed");
            return false;
        }
        f.bad.append(qMakePair(from, to));
    }

    if (!runsFromJson(o.value(QStringLiteral("bottom")), "bottom", &f.bottom, error)
        || !runsFromJson(o.value(QStringLiteral("limitMin")), "limitMin", &f.limitMin, error)
        || !runsFromJson(o.value(QStringLiteral("limitMax")), "limitMax", &f.limitMax, error)) {
        return false;
    }

    for (const QJsonValue& v : o.value(QStringLiteral("mosaicOut")).toArray()) {   // absent in a file before it
        const QJsonObject so = v.toObject();
        EditFile::Side s;
        const int address = so.value(QStringLiteral("address")).toInt(-1);
        const int sub = so.value(QStringLiteral("sub")).toInt(-1);
        if (address < 0 || address > 255 || sub < 0 || sub > 7 || !so.value(QStringLiteral("ranges")).isArray()) {
            *error = QStringLiteral("a side of \"mosaicOut\" is malformed");
            return false;
        }
        s.address = uint8_t(address);
        s.sub = uint8_t(sub);
        for (const QJsonValue& rv : so.value(QStringLiteral("ranges")).toArray()) {
            const QJsonArray r = rv.toArray();
            const int from = r.size() == 2 ? r.at(0).toInt(-1) : -1;
            const int to = r.size() == 2 ? r.at(1).toInt(-1) : -1;
            if (from < 0 || to <= from) {
                *error = QStringLiteral("a range of \"mosaicOut\" is malformed");
                return false;
            }
            s.ranges.append(qMakePair(from, to));
        }
        f.mosaicOut.append(s);
    }

    *out = f;
    return true;
}

namespace {

// BottomTrackProcessor's constants of a preset, and those it scales to a resolution.
struct TrackCoeffs {
    int istartRef = 4, initWinRef = 6, scaleWinRef = 20;
    int16_t c1 = -6, c2 = 6, c3 = 4, c4 = 2, c5 = 0;
    float s2 = 1.04f, s3 = 1.06f, s4 = 1.10f, s5 = 1.15f;
    float t1 = 1.07f;
};

TrackCoeffs coeffsFor(int preset)
{
    TrackCoeffs c;
    if (preset == 1) {   // BottomTrackOneBeamNarrow
        c.scaleWinRef = 35;
        c.c1 = -3; c.c2 = 8; c.c3 = 5; c.c4 = -1; c.c5 = -1;
        c.s2 = 1.015f; c.s3 = 1.035f; c.s4 = 1.08f; c.s5 = 1.12f;
        c.t1 = 1.04f;
    }
    if (preset == 2) {   // BottomTrackSideScan
        c.scaleWinRef = 12;
        c.c1 = -3; c.c2 = -3; c.c3 = 4; c.c4 = 2; c.c5 = 1;
        c.s2 = 1.1f; c.s3 = 1.2f; c.s4 = 1.3f; c.s5 = 1.4f;
        c.t1 = 1.2f;
    }
    return c;
}

struct TrackScaled {
    int istart = 0, initWin = 1;
    float initWinScaled = 0.0f;
    int indexGainScaleQ12 = 1 << 12, initScaleTermQ12 = 0, scaleWinQ12 = 0;
};

TrackScaled scaledFor(const TrackCoeffs& c, float resolution)
{
    constexpr float kRefResolution = 0.01f;   // kBottomTrackRefResolution
    const float samplesScale = kRefResolution / resolution;
    TrackScaled s;
    s.istart = std::max(0, static_cast<int>(std::lround(static_cast<float>(c.istartRef) * samplesScale)));
    s.initWinScaled = static_cast<float>(c.initWinRef) * samplesScale;
    s.initWin = std::max(1, static_cast<int>(std::lround(s.initWinScaled)));
    const float indexGainScale = resolution / kRefResolution;
    s.indexGainScaleQ12 = std::max(1, static_cast<int>(std::lround(indexGainScale * static_cast<float>(1 << 12))));
    s.scaleWinQ12 = c.scaleWinRef << 12;
    s.initScaleTermQ12 = std::max(1, static_cast<int>(std::lround(s.initWinScaled * static_cast<float>(s.scaleWinQ12))));
    return s;
}

} // namespace

bool trackResponse(const uint8_t* data, int data_size, float resolution, const TrackFilter& f,
                   QVector<int32_t>& out, int* first, int* end)
{
    if (!data || !(resolution > 0.0f) || !(f.gainSlope > 0.0f)) {
        return false;
    }
    const TrackCoeffs c = coeffsFor(f.preset);
    const TrackScaled s = scaledFor(c, resolution);
    if (data_size <= (s.istart + s.initWin)) {
        return false;
    }
    out.fill(0, data_size);
    int32_t* cash_data = out.data();
    const int gain_slope_inv = 1000/(f.gainSlope);

    // The moving average, wider with range.
    const uint8_t* data_from = data + s.istart;
    const uint8_t* data_to = data + (s.istart + s.initWin);
    const uint8_t* data_end = data + data_size;
    int data_acc = 0;
    for (int idata = s.istart; idata < (s.istart + s.initWin); idata++) {
        data_acc += data[idata];
    }
    int avrg_range = s.initWin;
    for (int idata = s.istart; (idata + avrg_range) < data_size; idata++) {
        if (data_from >= data_end || data_to >= data_end) {
            break;
        }
        data_acc -= *data_from; data_from++;
        data_acc += *data_to; data_to++;
        while ((data_to < data_end) &&
               (((idata << 12) + s.initScaleTermQ12) >= ((avrg_range = int(data_to - data_from)) * s.scaleWinQ12))) {
            data_acc += *data_to; data_to++;
        }
        cash_data[idata] = 10*data_acc / (avrg_range);
    }

    // The five taps and the gain with range.
    const int data_conv_size = data_size - avrg_range;
    int idata = s.istart;
    for (; ; idata++) {
        const float fidata = idata + s.initWinScaled + 1.0f;
        const int di1 = idata;
        const int di2 = fidata*c.s2;
        const int di3 = fidata*c.s3;
        const int di4 = fidata*c.s4;
        const int di5 = fidata*c.s5;
        if (di5 >= data_conv_size) {
            break;
        }
        int calc = (c.c1*cash_data[di1] + c.c2*cash_data[di2] + c.c3*cash_data[di3] + c.c4*cash_data[di4] + c.c5*cash_data[di5]);
        const int idata_ref = std::max(0, (idata * s.indexGainScaleQ12 + (1 << 11)) >> 12);
        calc = calc*gain_slope_inv + calc*idata_ref;
        cash_data[idata] = calc;
    }
    *first = s.istart;
    *end = idata;
    return true;
}

float trackPeakWithin(const QVector<qint64>& sum, int pings, float resolution, const TrackFilter& f,
                      float fromM, float toM)
{
    if (sum.isEmpty() || pings <= 0 || !(resolution > 0.0f) || !(f.gainSlope > 0.0f)) {
        return NAN;
    }
    const TrackCoeffs c = coeffsFor(f.preset);
    const TrackScaled s = scaledFor(c, resolution);
    const int gain_slope_inv = 1000/(f.gainSlope);
    const int threshold_int = 10*gain_slope_inv*1000*f.threshold;
    int start = fromM/c.t1/resolution;
    int stop = toM/c.t1/resolution;
    start = std::clamp(start, 0, int(sum.size()));
    stop = std::clamp(stop, 0, int(sum.size()));

    qint64 best = qint64(threshold_int) * pings;
    int bestI = -1;
    for (int i = start; i < stop; i++) {
        if (best < sum[i]) {
            best = sum[i];
            bestI = i;
        }
    }
    if (bestI <= 0) {
        return NAN;
    }
    return ((bestI + s.initWinScaled + 1.0f) * c.t1) * resolution;
}

namespace {

float medianOf(QVector<float>& v)   // reorders v
{
    if (v.isEmpty()) {
        return NAN;
    }
    const int m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + m, v.end());
    return v[m];
}

constexpr double kPi = 3.14159265358979323846;

double wrapDeg(double a)
{
    a = std::fmod(a + 180.0, 360.0);
    if (a < 0.0) {
        a += 360.0;
    }
    return a - 180.0;
}

} // namespace

float medianAround(const QVector<float>& d, int epoch, int half)
{
    QVector<float> w;
    w.reserve(2 * half + 1);
    const int from = std::max(0, epoch - half);
    const int to = std::min(int(d.size()) - 1, epoch + half);
    for (int j = from; j <= to; ++j) {
        if (std::isfinite(d[j])) {
            w.append(d[j]);
        }
    }
    return medianOf(w);
}

float smoothedAt(const QVector<float>& d, int epoch, int half)
{
    if (epoch < 0 || epoch >= d.size() || !std::isfinite(d[epoch])) {
        return NAN;
    }
    return medianAround(d, epoch, half);
}

QVector<float> filledGaps(const QVector<float>& d, int from, int to, int maxGap)
{
    const int n = d.size();
    QVector<float> out(n, NAN);
    from = std::max(0, from);
    to = std::min(to, n);
    int i = from;
    while (i < to) {
        if (std::isfinite(d[i])) {
            ++i;
            continue;
        }
        int left = i - 1;   // the gap's bounds, looked for beyond the brushed stretch too
        while (left >= 0 && !std::isfinite(d[left]) && i - left <= maxGap + 1) {
            --left;
        }
        int right = i;
        while (right < n && !std::isfinite(d[right]) && right - left <= maxGap + 1) {
            ++right;
        }
        const bool closed = left >= 0 && right < n && std::isfinite(d[left]) && std::isfinite(d[right])
                         && right - left - 1 <= maxGap;
        const int gapEnd = std::min(right, to);
        if (closed) {
            for (int e = i; e < gapEnd; ++e) {
                out[e] = d[left] + (d[right] - d[left]) * float(e - left) / float(right - left);
            }
        }
        i = std::max(gapEnd, i + 1);
    }
    return out;
}

QVector<Spike> findSpikes(const QVector<float>& d, const QVector<bool>& skip, int half, float k,
                          float minAbs, float minRel)
{
    QVector<Spike> out;
    const int n = d.size();
    QVector<float> w;
    QVector<float> dev;
    w.reserve(2 * half + 1);
    dev.reserve(2 * half + 1);
    auto skipped = [&](int j) { return j < skip.size() && skip[j]; };
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(d[i]) || skipped(i)) {
            continue;
        }
        w.clear();
        for (int j = std::max(0, i - half); j <= std::min(n - 1, i + half); ++j) {
            if (j != i && std::isfinite(d[j]) && !skipped(j)) {
                w.append(d[j]);
            }
        }
        if (w.size() < 4) {
            continue;   // too few neighbours to judge it by
        }
        const float med = medianOf(w);
        dev.clear();
        for (float x : std::as_const(w)) {
            dev.append(std::fabs(x - med));
        }
        const float mad = medianOf(dev);
        const float limit = std::max(k * 1.4826f * mad, minAbs + minRel * std::fabs(med));
        if (std::fabs(d[i] - med) > limit) {
            out.append(Spike{ i, med });
        }
    }
    return out;
}

QVector<QPair<int, int>> findTurns(const QVector<double>& north, const QVector<double>& east,
                                   const QVector<float>& headingDeg, const QVector<float>& rangeM,
                                   float baseM, float radiusFactor, float marginM, float minRunM, float minTurnDeg)
{
    constexpr int kMaxScan = 5000;   // pings looked through each way for the stretch's ends
    const int n = std::min({ int(north.size()), int(east.size()), int(headingDeg.size()), int(rangeM.size()) });
    auto valid = [&](int i) {
        return std::isfinite(north[i]) && std::isfinite(east[i]) && std::isfinite(headingDeg[i]) && rangeM[i] > 0.0f;
    };
    auto dist = [&](int a, int b) { return std::hypot(north[a] - north[b], east[a] - east[b]); };

    // The heading unwrapped along the valid pings: a turn of more than half a circle stays one.
    QVector<double> unwrapped(n, NAN);
    int prev = -1;
    for (int i = 0; i < n; ++i) {
        if (!valid(i)) {
            continue;
        }
        unwrapped[i] = prev < 0 ? double(headingDeg[i]) : unwrapped[prev] + wrapDeg(double(headingDeg[i]) - double(headingDeg[prev]));
        prev = i;
    }

    const double halfBase = std::max(0.1, double(baseM) / 2.0);
    QVector<char> turning(n, 0);
    for (int i = 0; i < n; ++i) {
        if (!valid(i)) {
            continue;
        }
        int j = i;
        for (int s = 0, c = i - 1; c >= 0 && s < kMaxScan; --c, ++s) {
            if (!valid(c)) {
                continue;
            }
            j = c;
            if (dist(i, c) >= halfBase) {
                break;
            }
        }
        int k = i;
        for (int s = 0, c = i + 1; c < n && s < kMaxScan; ++c, ++s) {
            if (!valid(c)) {
                continue;
            }
            k = c;
            if (dist(i, c) >= halfBase) {
                break;
            }
        }
        if (j == i || k == i) {
            continue;
        }
        const double dpsi = std::fabs(unwrapped[k] - unwrapped[j]) * kPi / 180.0;
        const double ds = dist(j, k);
        turning[i] = ds < double(radiusFactor) * double(rangeM[i]) * dpsi;
    }

    // Each run: a wobble of the heading dropped, the rest widened by marginM of track both ways, then
    // joined.
    QVector<QPair<int, int>> out;
    for (int i = 0; i < n;) {
        if (!turning[i]) {
            ++i;
            continue;
        }
        int a = i;
        int b = i;
        while (b + 1 < n && (turning[b + 1] || !valid(b + 1))) {
            ++b;
        }
        while (b > a && !turning[b]) {
            --b;   // not past the last ping judged turning
        }
        i = b + 1;
        double length = 0.0;
        for (int c = a, p = -1; c <= b; ++c) {
            if (!valid(c)) {
                continue;
            }
            if (p >= 0) {
                length += dist(p, c);
            }
            p = c;
        }
        if (length < double(minRunM) && std::fabs(unwrapped[b] - unwrapped[a]) < double(minTurnDeg)) {
            continue;
        }
        int lo = a;
        for (int c = a - 1; c >= 0 && a - c <= kMaxScan; --c) {
            if (valid(c) && dist(a, c) > marginM) {
                break;
            }
            lo = c;
        }
        int hi = b;
        for (int c = b + 1; c < n && c - b <= kMaxScan; ++c) {
            if (valid(c) && dist(b, c) > marginM) {
                break;
            }
            hi = c;
        }
        if (!out.isEmpty() && lo <= out.last().second) {
            out.last().second = std::max(out.last().second, hi + 1);
        }
        else {
            out.append(qMakePair(lo, hi + 1));
        }
    }
    return out;
}

} // namespace echoedit
