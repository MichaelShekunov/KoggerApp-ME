#include "sonar_qa_analysis.h"

#include <QCoreApplication>
#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace sonarqa {

namespace {

// 2: the reflection read at the baseline's place, serial text; 3: compared only from reflectionMinDb over the floor
const char* const kRunFormat = "kogger-sonar-qa/3";

// The checks' words (lupdate's context SonarQaCheck). The figures of a check are this run's against the one it is
// compared with: a sonar against the reference, or the reference now against its baseline.
struct SonarQaCheck {
    Q_DECLARE_TR_FUNCTIONS(SonarQaCheck)
};
QString num(double v, int decimals = 1)
{
    return QString::number(v, 'f', decimals);
}

float medianOf(QVector<float> v)
{
    if (v.isEmpty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    const int n = v.size();
    return (n % 2) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

QJsonArray toArray(const QVector<float>& v)
{
    QJsonArray a;
    for (float x : v) {
        a.append(double(x));
    }
    return a;
}

QVector<float> fromArray(const QJsonArray& a)
{
    QVector<float> v;
    v.reserve(a.size());
    for (const auto& x : a) {
        v.append(float(x.toDouble()));
    }
    return v;
}

// A median of three: one odd step (a ping lost to a bubble) does not move the peak.
QVector<float> smooth3(const QVector<float>& v)
{
    QVector<float> out(v.size());
    for (int i = 0; i < v.size(); ++i) {
        float a = v[qMax(0, i - 1)], b = v[i], c = v[qMin(v.size() - 1, i + 1)];
        out[i] = std::max(std::min(a, b), std::min(std::max(a, b), c));
    }
    return out;
}

QVector<float> echoCurve(const Run& r, int side)
{
    QVector<float> v;
    v.reserve(r.steps.size());
    for (const Step& s : r.steps) {
        v.append(side < s.sides.size() ? s.sides[side].echoLevel : 0.0f);
    }
    return v;
}

// The curve over the given steps only, in their order.
QVector<float> pick(const QVector<float>& v, const QVector<int>& steps)
{
    QVector<float> out;
    out.reserve(steps.size());
    for (int i : steps) {
        out.append(v.value(i));
    }
    return out;
}

// Where the ringing ends: the first sample after which the bytes stay under level for hold samples.
int ringEnd(const uint8_t* a, int n, int level, int hold)
{
    for (int i = 0, quiet = 0; i < n; ++i) {
        quiet = a[i] < level ? quiet + 1 : 0;
        if (quiet >= hold) {
            return i - hold + 1;
        }
    }
    return n;
}

float percentile(QVector<uint8_t> s, double q)
{
    if (s.isEmpty()) {
        return 0;
    }
    std::sort(s.begin(), s.end());
    return float(s[qMin(s.size() - 1, int(q * (s.size() - 1) + 0.5))]);
}

Check make(const QString& id, int side, double value, double limit, const QString& unit, bool ok, bool warnOnly,
           const QString& detail)
{
    Check c;
    c.id = id;
    c.side = side;
    c.value = value;
    c.limit = limit;
    c.unit = unit;
    c.level = ok ? QStringLiteral("ok") : warnOnly ? QStringLiteral("warn") : QStringLiteral("fail");
    c.detail = detail;
    return c;
}

} // namespace

int Params::stepCount() const
{
    if (freqStepKhz <= 0 || freqToKhz < freqFromKhz) {
        return 0;
    }
    return (freqToKhz - freqFromKhz) / freqStepKhz + 1;
}

QJsonObject Params::toJson() const
{
    return { { "freqFromKhz", freqFromKhz }, { "freqToKhz", freqToKhz }, { "freqStepKhz", freqStepKhz },
             { "pingsPerStep", pingsPerStep }, { "discardPings", discardPings }, { "periodMs", periodMs },
             { "samples", samples }, { "txPulses", txPulses }, { "ringMarginCodes", ringMarginCodes },
             { "ringHoldSamples", ringHoldSamples }, { "echoMarginCodes", echoMarginCodes },
             { "echoMinGap", echoMinGap }, { "maxEchoes", maxEchoes }, { "echoWindow", echoWindow },
             { "reflectionMinDb", reflectionMinDb } };
}

Params Params::fromJson(const QJsonObject& o)
{
    Params p;
    p.freqFromKhz = o.value("freqFromKhz").toInt(p.freqFromKhz);
    p.freqToKhz = o.value("freqToKhz").toInt(p.freqToKhz);
    p.freqStepKhz = o.value("freqStepKhz").toInt(p.freqStepKhz);
    p.pingsPerStep = o.value("pingsPerStep").toInt(p.pingsPerStep);
    p.discardPings = o.value("discardPings").toInt(p.discardPings);
    p.periodMs = o.value("periodMs").toInt(p.periodMs);
    p.samples = o.value("samples").toInt(p.samples);
    p.txPulses = o.value("txPulses").toInt(p.txPulses);
    p.ringMarginCodes = o.value("ringMarginCodes").toInt(p.ringMarginCodes);
    p.ringHoldSamples = o.value("ringHoldSamples").toInt(p.ringHoldSamples);
    p.echoMarginCodes = o.value("echoMarginCodes").toInt(p.echoMarginCodes);
    p.echoMinGap = o.value("echoMinGap").toInt(p.echoMinGap);
    p.maxEchoes = o.value("maxEchoes").toInt(p.maxEchoes);
    p.echoWindow = o.value("echoWindow").toInt(p.echoWindow);
    p.reflectionMinDb = o.value("reflectionMinDb").toInt(p.reflectionMinDb);
    return p;
}

PingMetrics analysePing(const uint8_t* a, int n, bool tx, const Params& p)
{
    PingMetrics m;
    if (!a || n <= 0) {
        return m;
    }
    QVector<uint8_t> s(a, a + n);
    std::sort(s.begin(), s.end());
    auto pct = [&s, n](double q) { return float(s[qMin(n - 1, int(q * (n - 1) + 0.5))]); };
    m.floor = pct(0.25);
    m.p50 = pct(0.5);
    m.p95 = pct(0.95);
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        sum += a[i];
    }
    m.mean = float(sum / n);
    if (!tx) {
        return m;
    }

    // Ringing: from the first sample until the bytes stay under the floor plus the margin for ringHoldSamples.
    m.ringdown = ringEnd(a, n, int(m.floor) + p.ringMarginCodes, qMax(1, p.ringHoldSamples));
    for (int i = 0; i < m.ringdown; ++i) {
        m.ringPeak = qMax(m.ringPeak, int(a[i]));
    }

    // Reflections: peaks over the floor plus the margin, apart by echoMinGap, the first of a plateau.
    const int echoLevel = int(m.floor) + p.echoMarginCodes;
    const int gap = qMax(1, p.echoMinGap);
    for (int i = m.ringdown; i < n && m.echoes.size() < p.maxEchoes; ++i) {
        if (a[i] < echoLevel) {
            continue;
        }
        bool peak = true;
        for (int j = qMax(m.ringdown, i - gap); j <= qMin(n - 1, i + gap) && peak; ++j) {
            peak = !(a[j] > a[i] || (a[j] == a[i] && j < i));
        }
        if (peak) {
            m.echoes.append({ i, int(a[i]) });
            i += gap;
        }
    }
    for (int i = m.ringdown; i < n; ++i) {
        m.saturated += a[i] == 255 ? 1 : 0;
    }
    return m;
}

SideStep aggregate(const QVector<PingMetrics>& off, const QVector<PingMetrics>& on, int maxEchoes)
{
    SideStep s;
    s.pingsOff = off.size();
    s.pingsOn = on.size();
    QVector<float> mean, p50, p95;
    for (const PingMetrics& m : off) {
        mean.append(m.mean);
        p50.append(m.p50);
        p95.append(m.p95);
    }
    s.noiseMean = medianOf(mean);
    s.noiseP50 = medianOf(p50);
    s.noiseP95 = medianOf(p95);

    QVector<float> floor, ring, ringPeak, sat;
    for (const PingMetrics& m : on) {
        floor.append(m.floor);
        ring.append(m.ringdown);
        ringPeak.append(m.ringPeak);
        sat.append(m.saturated);
    }
    s.floor = medianOf(floor);
    s.ringdown = medianOf(ring);
    s.ringPeak = medianOf(ringPeak);
    s.saturated = medianOf(sat);

    for (int k = 0; k < maxEchoes; ++k) {
        QVector<float> pos, amp;
        for (const PingMetrics& m : on) {
            if (m.echoes.size() > k) {
                pos.append(m.echoes[k].pos);
                amp.append(m.echoes[k].amp);
            }
        }
        s.echoPos.append(medianOf(pos));
        s.echoAmp.append(medianOf(amp));
        s.echoPresence.append(on.isEmpty() ? 0.0f : float(pos.size()) / float(on.size()));
        if (k == 0 && !amp.isEmpty()) {
            const auto mm = std::minmax_element(amp.begin(), amp.end());
            s.echo1Spread = *mm.second - *mm.first;
        }
    }
    return s;
}

QVector<uint8_t> medianProfile(const QVector<QVector<uint8_t>>& pings)
{
    int n = 0;
    for (const auto& p : pings) {
        n = qMax(n, p.size());
    }
    QVector<uint8_t> out(n);
    QVector<uint8_t> col;
    for (int i = 0; i < n; ++i) {
        col.clear();
        for (const auto& p : pings) {
            if (i < p.size()) {
                col.append(p[i]);
            }
        }
        std::sort(col.begin(), col.end());
        out[i] = col.isEmpty() ? 0 : col[(col.size() - 1) / 2];   // the lower middle of an even count: a byte
    }
    return out;
}

QVector<uint8_t> measureProfiles(SideStep& s, const QVector<QVector<uint8_t>>& off, const QVector<QVector<uint8_t>>& on,
                                 const Params& p)
{
    const QVector<uint8_t> pOn = medianProfile(on), pOff = medianProfile(off);
    const int n = qMin(pOn.size(), pOff.size());
    const int ring = n > 0 ? ringEnd(pOn.constData(), n, int(percentile(pOn, 0.25)) + p.ringMarginCodes,
                                     qMax(1, p.ringHoldSamples))
                           : 0;
    s.txOffKnown = ring >= qMax(1, p.ringHoldSamples);
    s.txOffDb = 0;
    if (s.txOffKnown) {
        double sum = 0;
        for (int i = 0; i < ring; ++i) {
            sum += double(pOn[i]) - double(pOff[i]);
        }
        s.txOffDb = float(sum / ring / kCodesPerDb);
    }
    return pOn;
}

int reflectionAt(const QVector<uint8_t>& profile, const Params& p)
{
    const int n = profile.size();
    if (n == 0) {
        return -1;
    }
    const float floor = percentile(profile, 0.25);
    const int start = ringEnd(profile.constData(), n, int(floor) + p.ringMarginCodes, qMax(1, p.ringHoldSamples));
    int best = -1;
    for (int i = start; i < n; ++i) {
        if (best < 0 || profile[i] > profile[best]) {
            best = i;
        }
    }
    const double margin = qMax(double(p.echoMarginCodes), p.reflectionMinDb * kCodesPerDb);
    return best >= 0 && profile[best] >= floor + margin ? best : -1;
}

void measureReflections(Run& run, const QVector<QVector<QVector<uint8_t>>>& profiles, const Run* ref)
{
    const int w = qMax(0, run.params.echoWindow);
    const int search = 4 * qMax(1, w);   // how far the placement looks for this run's own peak
    for (int i = 0; i < run.steps.size(); ++i) {
        for (int s = 0; s < run.steps[i].sides.size(); ++s) {
            SideStep& x = run.steps[i].sides[s];
            if (ref) {
                const bool has = i < ref->steps.size() && s < ref->steps[i].sides.size();
                x.echoRefPos = has ? ref->steps[i].sides[s].echoRefPos : -1;
                x.echoFound = has && ref->steps[i].sides[s].echoFound;
            } else {
                x.echoRefPos = i < profiles.size() && s < profiles[i].size() ? reflectionAt(profiles[i][s], run.params)
                                                                            : -1;
                x.echoFound = x.echoRefPos >= 0;
            }
        }
    }
    // A step without the baseline's reflection is not read at all: read where the others are, at a low frequency
    // it lies inside the longer ringing, which then passes for the strongest reflection of the sweep.
    for (int i = 0; i < run.steps.size(); ++i) {
        for (int s = 0; s < run.steps[i].sides.size(); ++s) {
            SideStep& x = run.steps[i].sides[s];
            x.echoLevel = 0;
            x.echoShift = 0;
            const QVector<uint8_t>* prof = i < profiles.size() && s < profiles[i].size() ? &profiles[i][s] : nullptr;
            if (!prof || prof->isEmpty() || x.echoRefPos < 0) {
                continue;
            }
            const int n = prof->size(), c = qMin(x.echoRefPos, n - 1);
            for (int k = qMax(0, c - w); k <= qMin(n - 1, c + w); ++k) {
                x.echoLevel = qMax(x.echoLevel, float((*prof)[k]));
            }
            int best = c;
            for (int k = qMax(0, c - search); k <= qMin(n - 1, c + search); ++k) {
                if ((*prof)[k] > (*prof)[best]) {
                    best = k;
                }
            }
            x.echoShift = float(best - c);
        }
    }
}

double txOffDb(const Run& run, int side)
{
    QVector<float> d;
    for (const Step& st : run.steps) {
        if (side < st.sides.size() && st.sides[side].txOffKnown) {
            d.append(st.sides[side].txOffDb);
        }
    }
    return d.isEmpty() ? std::nan("") : double(medianOf(d));
}

QVector<Check> selfChecks(const Run& run, const Tolerances& t)
{
    QVector<Check> out;
    const int sides = run.steps.isEmpty() ? 0 : run.steps.first().sides.size();
    for (int s = 0; s < sides; ++s) {
        const double off = txOffDb(run, s);
        if (!std::isnan(off)) {
            const bool quiet = off >= t.txOffDb;
            out.append(make("tx_off", s, off, t.txOffDb, "dB", quiet, true,
                            SonarQaCheck::tr("with 0 pulses the ringing drops by %1 dB (it must drop by %2 dB or more)")
                                    .arg(num(off), num(t.txOffDb, 0))
                                + (quiet ? QString() : SonarQaCheck::tr(": the transmitter keeps sending, the noise is not measured"))));
        }
        int found = 0;
        for (const Step& st : run.steps) {
            found += st.sides[s].echoFound ? 1 : 0;
        }
        out.append(make("reflection", s, found, run.steps.size(), "", found > 0, true,
                        found > 0 ? SonarQaCheck::tr("a clear reflection (%1 dB over the floor) at %2 of %3 frequencies: only these "
                                        "are compared")
                                        .arg(run.params.reflectionMinDb).arg(found).arg(run.steps.size())
                                  : SonarQaCheck::tr("no reflection %1 dB over the floor at any frequency: the target is not seen, "
                                        "reflections will not be compared")
                                        .arg(run.params.reflectionMinDb)));
    }
    out.append(make("supply", -1, run.supply.median, t.supplyV, "V", run.supply.valid(), true,
                    run.supply.valid() ? SonarQaCheck::tr("%1 V (from %2 to %3 V during the run)").arg(num(run.supply.median, 2),
                                             num(run.supply.min, 2), num(run.supply.max, 2))
                                       : SonarQaCheck::tr("the sonar sent no supply readings")));
    const bool tempOk = run.temperature.valid() && run.temperature.median >= t.tempMinC
                        && run.temperature.median <= t.tempMaxC;
    out.append(make("temperature", -1, run.temperature.median, t.tempMaxC, "C", tempOk, true,
                    run.temperature.valid() ? SonarQaCheck::tr("%1 °C (from %2 to %3 °C during the run)")
                                                  .arg(num(run.temperature.median), num(run.temperature.min),
                                                       num(run.temperature.max))
                                            : SonarQaCheck::tr("the sonar sent no temperature readings")));
    return out;
}

Series Series::of(QVector<float> values)
{
    Series s;
    s.n = values.size();
    if (s.n > 0) {
        std::sort(values.begin(), values.end());
        s.min = values.first();
        s.max = values.last();
        s.median = medianOf(values);
    }
    return s;
}

QJsonObject Series::toJson() const
{
    return { { "n", n }, { "min", min }, { "median", median }, { "max", max } };
}

Series Series::fromJson(const QJsonObject& o)
{
    Series s;
    s.n = o.value("n").toInt();
    s.min = float(o.value("min").toDouble());
    s.median = float(o.value("median").toDouble());
    s.max = float(o.value("max").toDouble());
    return s;
}

QJsonObject Tolerances::toJson() const
{
    return { { "echoBandMeanDb", echoBandMeanDb }, { "echoBandMaxDb", echoBandMaxDb }, { "bandDb", bandDb },
             { "resonanceKhz", resonanceKhz }, { "bandwidthRatioMin", bandwidthRatioMin },
             { "bandwidthRatioMax", bandwidthRatioMax }, { "noiseDb", noiseDb }, { "ringdownRatio", ringdownRatio },
             { "ringdownSlack", ringdownSlack }, { "balanceDb", balanceDb }, { "positionSamples", positionSamples },
             { "supplyV", supplyV }, { "tempDeltaC", tempDeltaC }, { "tempMinC", tempMinC },
             { "tempMaxC", tempMaxC }, { "saturationShare", saturationShare }, { "txOffDb", txOffDb } };
}

Tolerances Tolerances::fromJson(const QJsonObject& o)
{
    Tolerances t;
    auto get = [&o](const char* k, double& v) { v = o.value(k).toDouble(v); };
    get("echoBandMeanDb", t.echoBandMeanDb);
    get("echoBandMaxDb", t.echoBandMaxDb);
    get("bandDb", t.bandDb);
    get("resonanceKhz", t.resonanceKhz);
    get("bandwidthRatioMin", t.bandwidthRatioMin);
    get("bandwidthRatioMax", t.bandwidthRatioMax);
    get("noiseDb", t.noiseDb);
    get("ringdownRatio", t.ringdownRatio);
    get("ringdownSlack", t.ringdownSlack);
    get("balanceDb", t.balanceDb);
    get("positionSamples", t.positionSamples);
    get("supplyV", t.supplyV);
    get("tempDeltaC", t.tempDeltaC);
    get("tempMinC", t.tempMinC);
    get("tempMaxC", t.tempMaxC);
    get("saturationShare", t.saturationShare);
    get("txOffDb", t.txOffDb);
    return t;
}

QJsonObject Run::toJson() const
{
    QJsonArray stepsJ;
    for (const Step& st : steps) {
        QJsonArray sidesJ;
        for (const SideStep& s : st.sides) {
            sidesJ.append(QJsonObject{
                { "noiseMean", s.noiseMean }, { "noiseP50", s.noiseP50 }, { "noiseP95", s.noiseP95 },
                { "floor", s.floor }, { "ringdown", s.ringdown }, { "ringPeak", s.ringPeak },
                { "saturated", s.saturated }, { "echoPos", toArray(s.echoPos) }, { "echoAmp", toArray(s.echoAmp) },
                { "echoPresence", toArray(s.echoPresence) }, { "echo1Spread", s.echo1Spread },
                { "pingsOff", s.pingsOff }, { "pingsOn", s.pingsOn }, { "txOffDb", s.txOffDb },
                { "txOffKnown", s.txOffKnown },
                { "echoRefPos", s.echoRefPos }, { "echoFound", s.echoFound }, { "echoLevel", s.echoLevel },
                { "echoShift", s.echoShift } });
        }
        stepsJ.append(QJsonObject{ { "freqKhz", st.freqKhz }, { "sides", sidesJ } });
    }
    return { { "format", kRunFormat }, { "kind", kind }, { "time", time },
             { "device", QJsonObject{ { "serial", serial }, { "reportedSerial", double(reportedSerial) },
                                      { "name", deviceName }, { "firmware", firmware }, { "board", board } } },
             { "params", params.toJson() }, { "settings", settings }, { "resolutionM", resolutionM },
             { "supply", supply.toJson() }, { "temperature", temperature.toJson() }, { "steps", stepsJ } };
}

Run Run::fromJson(const QJsonObject& o)
{
    Run r;
    r.format = o.value("format").toString();
    r.kind = o.value("kind").toString();
    r.time = o.value("time").toString();
    const QJsonObject d = o.value("device").toObject();
    const QJsonValue sv = d.value("serial");   // format 1 kept the reported number, as a number
    r.serial = sv.isDouble() ? QString::number(qint64(sv.toDouble())) : sv.toString();
    r.reportedSerial = quint32(d.value(d.contains("reportedSerial") ? "reportedSerial" : "serial").toVariant()
                                   .toLongLong());
    r.deviceName = d.value("name").toString();
    r.firmware = d.value("firmware").toString();
    r.board = d.value("board").toInt();
    r.params = Params::fromJson(o.value("params").toObject());
    r.settings = o.value("settings").toObject();
    r.resolutionM = float(o.value("resolutionM").toDouble());
    r.supply = Series::fromJson(o.value("supply").toObject());
    r.temperature = Series::fromJson(o.value("temperature").toObject());
    for (const auto& sv : o.value("steps").toArray()) {
        const QJsonObject so = sv.toObject();
        Step st;
        st.freqKhz = so.value("freqKhz").toInt();
        for (const auto& dv : so.value("sides").toArray()) {
            const QJsonObject x = dv.toObject();
            SideStep s;
            s.noiseMean = float(x.value("noiseMean").toDouble());
            s.noiseP50 = float(x.value("noiseP50").toDouble());
            s.noiseP95 = float(x.value("noiseP95").toDouble());
            s.floor = float(x.value("floor").toDouble());
            s.ringdown = float(x.value("ringdown").toDouble());
            s.ringPeak = float(x.value("ringPeak").toDouble());
            s.saturated = float(x.value("saturated").toDouble());
            s.echoPos = fromArray(x.value("echoPos").toArray());
            s.echoAmp = fromArray(x.value("echoAmp").toArray());
            s.echoPresence = fromArray(x.value("echoPresence").toArray());
            s.echo1Spread = float(x.value("echo1Spread").toDouble());
            s.pingsOff = x.value("pingsOff").toInt();
            s.pingsOn = x.value("pingsOn").toInt();
            s.txOffDb = float(x.value("txOffDb").toDouble());
            s.txOffKnown = x.value("txOffKnown").toBool();
            s.echoRefPos = x.value("echoRefPos").toInt(-1);
            s.echoFound = x.value("echoFound").toBool();
            s.echoLevel = float(x.value("echoLevel").toDouble());
            s.echoShift = float(x.value("echoShift").toDouble());
            st.sides.append(s);
        }
        r.steps.append(st);
    }
    return r;
}

bool Run::needsRebuild() const
{
    return format != QLatin1String(kRunFormat);
}

QJsonObject Comparison::toJson() const
{
    QJsonArray a;
    for (const Check& c : checks) {
        a.append(QJsonObject{ { "id", c.id }, { "side", c.side }, { "value", c.value }, { "limit", c.limit },
                              { "unit", c.unit }, { "level", c.level }, { "detail", c.detail } });
    }
    return { { "pass", pass }, { "warnings", warnings }, { "checks", a } };
}

QVector<int> foundSteps(const Run& run, int side)
{
    QVector<int> out;
    for (int i = 0; i < run.steps.size(); ++i) {
        if (side < run.steps[i].sides.size() && run.steps[i].sides[side].echoFound) {
            out.append(i);
        }
    }
    return out;
}

Resonance resonance(const Run& run, int side, double bandDb, const QVector<int>& stepsIn)
{
    Resonance r;
    const QVector<int> steps = stepsIn.isEmpty() ? foundSteps(run, side) : stepsIn;
    const QVector<float> c = smooth3(pick(echoCurve(run, side), steps));
    if (c.isEmpty()) {
        return r;
    }
    const int peak = int(std::max_element(c.begin(), c.end()) - c.begin());
    // A byte steps by 0.4 dB, so the top of a response is flat over several steps: its middle is the resonance
    // (the strongest step alone is the plateau's first).
    auto around = [&c, peak](float level, int* lo, int* hi) {
        *lo = *hi = peak;
        while (*lo > 0 && c[*lo - 1] >= level) {
            --*lo;
        }
        while (*hi < c.size() - 1 && c[*hi + 1] >= level) {
            ++*hi;
        }
    };
    int topLo, topHi, lo, hi;
    around(c[peak] - float(kCodesPerDb), &topLo, &topHi);
    around(c[peak] - float(bandDb * kCodesPerDb), &lo, &hi);
    r.freqKhz = 0.5 * (run.params.freqAt(steps[topLo]) + run.params.freqAt(steps[topHi]));
    r.bandwidthKhz = double(run.params.freqAt(steps[hi]) - run.params.freqAt(steps[lo]));
    r.peakCodes = c[peak];
    return r;
}

double responseShiftKhz(const Run& unit, const Run& ref, int side, const QVector<int>& steps)
{
    const int stepKhz = qMax(1, ref.params.freqStepKhz);
    const int maxShift = qMax(4, 100 / stepKhz);
    const QVector<float> u = echoCurve(unit, side), r = echoCurve(ref, side);
    const int need = qMax(3, int(std::ceil(0.6 * steps.size())));
    double best = std::nan(""), bestRms = 0;
    for (int k = 0; k <= 2 * maxShift; ++k) {
        const int d = (k % 2 ? 1 : -1) * ((k + 1) / 2);   // 0, 1, -1, 2, -2 ...: a tie keeps the smaller shift
        QVector<double> diff;
        for (int i : steps) {
            if (steps.contains(i - d)) {
                diff.append(double(u.value(i)) - double(r.value(i - d)));
            }
        }
        if (diff.size() < need) {
            continue;
        }
        double mean = 0, sq = 0;
        for (double x : diff) {
            mean += x;
        }
        mean /= diff.size();
        for (double x : diff) {
            sq += (x - mean) * (x - mean);
        }
        const double rms = std::sqrt(sq / diff.size());
        if (std::isnan(best) || rms < bestRms - 1e-9) {
            best = double(d) * stepKhz;
            bestRms = rms;
        }
    }
    return best;
}

double bandWidthKhz(const Run& run, int side, const QVector<int>& steps, double bandDb)
{
    const QVector<float> v = echoCurve(run, side);
    float top = 0;
    for (int i : steps) {
        top = qMax(top, v.value(i));
    }
    const double edge = top - bandDb * kCodesPerDb;
    double n = 0;
    for (int i : steps) {
        n += qBound(0.0, (v.value(i) - edge) / kCodesPerDb + 0.5, 1.0);   // 1 dB soft edge
    }
    return n * run.params.freqStepKhz;
}

Comparison compare(const Run& unit, const Run& ref, const Tolerances& t)
{
    Comparison cmp;
    auto add = [&cmp](const Check& c) {
        cmp.checks.append(c);
        cmp.pass = cmp.pass && c.level != QLatin1String("fail");
        cmp.warnings += c.level == QLatin1String("warn") ? 1 : 0;
    };
    // whose each figure is: "this sonar ... reference ..." or, for the reference's check, "now ... baseline ..."
    const bool check = unit.kind == QLatin1String("check");
    const QString me = check ? SonarQaCheck::tr("now") : SonarQaCheck::tr("this sonar");
    const QString them = check ? SonarQaCheck::tr("baseline") : SonarQaCheck::tr("reference");
    auto pair = [&me, &them](const QString& mine, const QString& theirs) {
        return SonarQaCheck::tr("%1 %2, %3 %4").arg(me, mine, them, theirs);
    };

    const bool sameSweep = unit.params.freqFromKhz == ref.params.freqFromKhz
                           && unit.params.freqToKhz == ref.params.freqToKhz
                           && unit.params.freqStepKhz == ref.params.freqStepKhz && unit.steps.size() == ref.steps.size()
                           && !unit.steps.isEmpty();
    int sides = 0;
    if (sameSweep) {
        sides = qMin(unit.steps.first().sides.size(), ref.steps.first().sides.size());
    }
    add(make("sweep", -1, sides, 0, "", sameSweep && sides > 0, false,
             SonarQaCheck::tr("frequencies: %1; sides: %2").arg(pair(QString::number(unit.steps.size()),
                                                         QString::number(ref.steps.size())))
                 .arg(sides)));
    if (!sameSweep || sides == 0) {
        return cmp;
    }

    const float bandCodes = float(t.bandDb * kCodesPerDb);
    QVector<QVector<int>> bands(sides);
    QVector<bool> seen(sides, false);
    for (int s = 0; s < sides; ++s) {
        // 0 pulses must silence the transmitter: a sonar that keeps sending where the reference went quiet is
        // faulty; when neither goes quiet the firmware does not mute it and the noise cannot be measured.
        // NaN (no ringing to compare over) counts as quiet: a dead transmitter fails on its reflections instead
        const double offU = txOffDb(unit, s), offR = txOffDb(ref, s);
        const bool quietU = std::isnan(offU) || offU >= t.txOffDb, quietR = std::isnan(offR) || offR >= t.txOffDb;
        if (!std::isnan(offU)) {
            add(make("tx_off", s, offU, t.txOffDb, "dB", quietU, !quietR,
                     SonarQaCheck::tr("with 0 pulses the ringing drops by: %1 (it must drop by %2 dB or more)")
                             .arg(pair(SonarQaCheck::tr("%1 dB").arg(num(offU)), SonarQaCheck::tr("%1 dB").arg(num(offR))), num(t.txOffDb, 0))
                         + (quietU   ? QString()
                            : quietR ? SonarQaCheck::tr(": this sonar keeps transmitting at 0 pulses")
                                     : SonarQaCheck::tr(": the transmitter keeps sending, the noise is not measured"))));
        }

        const QVector<int> found = foundSteps(ref, s);
        seen[s] = !found.isEmpty();
        if (seen[s]) {
            // over the steps where the baseline had a reflection, in their order
            const QVector<float> rc = smooth3(pick(echoCurve(ref, s), found));
            const QVector<float> uc = echoCurve(unit, s), rraw = echoCurve(ref, s);
            const float top = *std::max_element(rc.begin(), rc.end());
            QVector<int>& band = bands[s];
            for (int k = 0; k < rc.size(); ++k) {
                if (rc[k] >= top - bandCodes) {
                    band.append(found[k]);
                }
            }
            double sumAbs = 0, maxAbs = 0;
            int maxAt = band.value(0);
            for (int i : band) {
                const double d = (uc[i] - rraw[i]) / kCodesPerDb;
                sumAbs += std::fabs(d);
                if (std::fabs(d) > maxAbs) {
                    maxAbs = std::fabs(d);
                    maxAt = i;
                }
            }
            const double meanAbs = band.isEmpty() ? 0 : sumAbs / band.size();
            add(make("echo_band_mean", s, meanAbs, t.echoBandMeanDb, "dB", meanAbs <= t.echoBandMeanDb, false,
                     SonarQaCheck::tr("how much the reflection differs on average over %1 frequencies, %2 to %3 kHz (where the "
                         "reflection is strongest); allowed %4 dB")
                         .arg(band.size()).arg(ref.params.freqAt(band.value(0)))
                         .arg(ref.params.freqAt(band.isEmpty() ? 0 : band.last())).arg(num(t.echoBandMeanDb))));
            add(make("echo_band_max", s, maxAbs, t.echoBandMaxDb, "dB", maxAbs <= t.echoBandMaxDb, false,
                     SonarQaCheck::tr("the largest difference, at %1 kHz: %2 (2.5 codes = 1 dB); allowed %3 dB")
                         .arg(ref.params.freqAt(maxAt))
                         .arg(pair(SonarQaCheck::tr("%1 codes").arg(num(uc.value(maxAt), 0)),
                                   SonarQaCheck::tr("%1 codes").arg(num(rraw.value(maxAt), 0))),
                              num(t.echoBandMaxDb))));

            // the whole curve against the reference's, not its strongest step: two near-equal peaks swap places
            // from run to run, and the step decides
            const Resonance ru = resonance(unit, s, t.bandDb, found), rr = resonance(ref, s, t.bandDb, found);
            const double shift = responseShiftKhz(unit, ref, s, found);
            if (!std::isnan(shift)) {
                add(make("resonance", s, shift, t.resonanceKhz, "kHz", std::fabs(shift) <= t.resonanceKhz, false,
                         SonarQaCheck::tr("the response (reflection against frequency) matches the %1's best when shifted by %2 kHz; "
                             "allowed ±%3 kHz. Strongest reflection: %4")
                             .arg(them, num(shift, 0), num(t.resonanceKhz, 0),
                                  pair(SonarQaCheck::tr("%1 kHz").arg(num(ru.freqKhz, 0)), SonarQaCheck::tr("%1 kHz").arg(num(rr.freqKhz, 0))))));
            }
            const double wu = bandWidthKhz(unit, s, found, t.bandDb), wr = bandWidthKhz(ref, s, found, t.bandDb);
            const double ratio = wr > 0 ? wu / wr : (wu > 0 ? 99 : 1);
            add(make("bandwidth", s, ratio, t.bandwidthRatioMax, "x",
                     ratio >= t.bandwidthRatioMin && ratio <= t.bandwidthRatioMax, false,
                     SonarQaCheck::tr("frequencies within %1 dB of the strongest reflection: %2; allowed %3 to %4 times the %5's")
                         .arg(num(t.bandDb, 0),
                              pair(SonarQaCheck::tr("%1 kHz").arg(num(wu, 0)), SonarQaCheck::tr("%1 kHz").arg(num(wr, 0))),
                              num(t.bandwidthRatioMin, 2), num(t.bandwidthRatioMax, 2), them)));
        } else {
            add(make("reflection", s, 0, 0, "", false, true,
                     SonarQaCheck::tr("the %1 shows no reflection %2 dB over the floor: reflections are not compared (the target "
                         "is not seen: too close, too small or inside the ringing)")
                         .arg(them).arg(ref.params.reflectionMinDb)));
        }

        if (quietU && quietR) {
            double noiseSum = 0;
            for (int i = 0; i < ref.steps.size(); ++i) {
                noiseSum += (unit.steps[i].sides[s].noiseP50 - ref.steps[i].sides[s].noiseP50) / kCodesPerDb;
            }
            const double noise = noiseSum / ref.steps.size();
            add(make("noise", s, noise, t.noiseDb, "dB", noise <= t.noiseDb, false,
                     SonarQaCheck::tr("noise with the transmitter off, mean over %1 frequencies: %2 louder than the %3 by %4 dB "
                         "(minus: quieter); allowed up to %5 dB louder")
                         .arg(ref.steps.size()).arg(me, them, num(noise), num(t.noiseDb))));
        } else {
            add(make("noise", s, 0, t.noiseDb, "dB", false, true,
                     SonarQaCheck::tr("not measured: with 0 pulses the transmitter keeps sending")));
        }

        // the ringing over the reflection's band, or over the whole sweep when there is no reflection
        QVector<int> span = bands[s];
        if (span.isEmpty()) {
            for (int i = 0; i < ref.steps.size(); ++i) {
                span.append(i);
            }
        }
        QVector<float> ru_ring, rr_ring, shifts;
        int saturatedSteps = 0;
        for (int i : span) {
            const SideStep& us = unit.steps[i].sides[s];
            const SideStep& rs = ref.steps[i].sides[s];
            ru_ring.append(us.ringdown);
            rr_ring.append(rs.ringdown);
            if (rs.echoFound) {
                shifts.append(std::fabs(us.echoShift - rs.echoShift));
            }
            saturatedSteps += seen[s] && us.echoLevel >= 255 ? 1 : 0;
        }
        const double ringU = medianOf(ru_ring), ringR = medianOf(rr_ring);
        const double ringLimit = ringR * t.ringdownRatio + t.ringdownSlack;
        add(make("ringdown", s, ringU, ringLimit, "samples", ringU <= ringLimit, false,
                 SonarQaCheck::tr("length of the ringing after the burst: %1; allowed up to %2 samples")
                     .arg(pair(SonarQaCheck::tr("%1 samples").arg(num(ringU, 0)), SonarQaCheck::tr("%1 samples").arg(num(ringR, 0))),
                          num(ringLimit, 0))));
        if (seen[s]) {
            const double pos = shifts.isEmpty() ? 0 : medianOf(shifts);
            const bool placed = pos <= t.positionSamples;
            add(make("position", s, pos, t.positionSamples, "samples", placed, true,
                     placed ? SonarQaCheck::tr("the reflection is where the %1's is (off by %2 samples, allowed %3)")
                                  .arg(them, num(pos, 0), num(t.positionSamples, 0))
                            : SonarQaCheck::tr("the reflection is %1 samples off the %2's: the sonar sits otherwise on the stand")
                                  .arg(num(pos, 0), them)));
            const double satShare = span.isEmpty() ? 0 : double(saturatedSteps) / span.size();
            add(make("saturation", s, satShare, t.saturationShare, "", satShare <= t.saturationShare, true,
                     SonarQaCheck::tr("the reflection at full scale (255) at %1 of %2 frequencies").arg(saturatedSteps)
                         .arg(span.size())));
        }
    }

    if (sides >= 2 && seen[0] && seen[1]) {
        // the steps of either side's band where both sides had a reflection
        QVector<int> band;
        for (int i = 0; i < ref.steps.size(); ++i) {
            const bool inBand = bands[0].contains(i) || bands[1].contains(i);
            if (inBand && ref.steps[i].sides[0].echoFound && ref.steps[i].sides[1].echoFound) {
                band.append(i);
            }
        }
        const QVector<float> u0 = echoCurve(unit, 0), u1 = echoCurve(unit, 1);
        const QVector<float> r0 = echoCurve(ref, 0), r1 = echoCurve(ref, 1);
        double sum = 0;
        for (int i : band) {
            sum += std::fabs(((u0[i] - u1[i]) - (r0[i] - r1[i])) / kCodesPerDb);
        }
        const double bal = band.isEmpty() ? 0 : sum / band.size();
        add(make("balance", -1, bal, t.balanceDb, "dB", bal <= t.balanceDb, false,
                 SonarQaCheck::tr("side 1 against side 2: the difference between them changed by %1 dB against the %2's, over %3 "
                     "frequencies; allowed %4 dB")
                     .arg(num(bal), them).arg(band.size()).arg(num(t.balanceDb))));
    }

    if (unit.supply.valid() && ref.supply.valid()) {
        const double d = unit.supply.median - ref.supply.median;
        add(make("supply", -1, d, t.supplyV, "V", std::fabs(d) <= t.supplyV, false,
                 SonarQaCheck::tr("supply: %1 (from %2 to %3 V during the run); allowed ±%4 V")
                     .arg(pair(SonarQaCheck::tr("%1 V").arg(num(unit.supply.median, 2)), SonarQaCheck::tr("%1 V").arg(num(ref.supply.median, 2))),
                          num(unit.supply.min, 2), num(unit.supply.max, 2), num(t.supplyV, 2))));
    } else {
        add(make("supply", -1, 0, t.supplyV, "V", false, true,
                 unit.supply.valid() ? SonarQaCheck::tr("%1 V; the %2 has no supply readings to compare with")
                                           .arg(num(unit.supply.median, 2), them)
                                     : SonarQaCheck::tr("the sonar sent no supply readings")));
    }
    if (unit.temperature.valid()) {
        const bool inRange = unit.temperature.median >= t.tempMinC && unit.temperature.median <= t.tempMaxC;
        add(make("temperature", -1, unit.temperature.median, t.tempMaxC, "C", inRange, false,
                 SonarQaCheck::tr("%1 °C (from %2 to %3 °C during the run); allowed %4 to %5 °C")
                     .arg(num(unit.temperature.median), num(unit.temperature.min), num(unit.temperature.max),
                          num(t.tempMinC, 0), num(t.tempMaxC, 0))));
        if (ref.temperature.valid()) {
            const double d = unit.temperature.median - ref.temperature.median;
            add(make("temperature_delta", -1, d, t.tempDeltaC, "C", std::fabs(d) <= t.tempDeltaC, true,
                     SonarQaCheck::tr("%1; more than %2 °C apart is a warning")
                         .arg(pair(SonarQaCheck::tr("%1 °C").arg(num(unit.temperature.median)),
                                   SonarQaCheck::tr("%1 °C").arg(num(ref.temperature.median))),
                              num(t.tempDeltaC, 0))));
        }
    } else {
        add(make("temperature", -1, 0, t.tempMaxC, "C", false, true, SonarQaCheck::tr("the sonar sent no temperature readings")));
    }
    return cmp;
}

} // namespace sonarqa
