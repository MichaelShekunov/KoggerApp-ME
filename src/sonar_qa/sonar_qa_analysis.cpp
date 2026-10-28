#include "sonar_qa_analysis.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace sonarqa {

namespace {

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

QVector<float> echo1Curve(const Run& r, int side)
{
    QVector<float> v;
    v.reserve(r.steps.size());
    for (const Step& s : r.steps) {
        v.append(side < s.sides.size() ? s.sides[side].echo1Amp() : 0.0f);
    }
    return v;
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
             { "echoMinGap", echoMinGap }, { "maxEchoes", maxEchoes } };
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
    const int ringLevel = int(m.floor) + p.ringMarginCodes;
    const int hold = qMax(1, p.ringHoldSamples);
    m.ringdown = n;
    for (int i = 0, quiet = 0; i < n; ++i) {
        quiet = a[i] < ringLevel ? quiet + 1 : 0;
        if (quiet >= hold) {
            m.ringdown = i - hold + 1;
            break;
        }
    }
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
             { "tempMaxC", tempMaxC }, { "saturationShare", saturationShare } };
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
                { "pingsOff", s.pingsOff }, { "pingsOn", s.pingsOn } });
        }
        stepsJ.append(QJsonObject{ { "freqKhz", st.freqKhz }, { "sides", sidesJ } });
    }
    return { { "format", "kogger-sonar-qa/1" }, { "kind", kind }, { "time", time },
             { "device", QJsonObject{ { "serial", double(serial) }, { "name", deviceName },
                                      { "firmware", firmware }, { "board", board } } },
             { "params", params.toJson() }, { "settings", settings }, { "resolutionM", resolutionM },
             { "supply", supply.toJson() }, { "temperature", temperature.toJson() }, { "steps", stepsJ } };
}

Run Run::fromJson(const QJsonObject& o)
{
    Run r;
    r.kind = o.value("kind").toString();
    r.time = o.value("time").toString();
    const QJsonObject d = o.value("device").toObject();
    r.serial = quint32(d.value("serial").toDouble());
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
            st.sides.append(s);
        }
        r.steps.append(st);
    }
    return r;
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

Resonance resonance(const Run& run, int side, double bandDb)
{
    Resonance r;
    const QVector<float> c = smooth3(echo1Curve(run, side));
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
    r.freqKhz = 0.5 * (run.params.freqAt(topLo) + run.params.freqAt(topHi));
    r.bandwidthKhz = double(hi - lo) * run.params.freqStepKhz;
    r.peakCodes = c[peak];
    return r;
}

Comparison compare(const Run& unit, const Run& ref, const Tolerances& t)
{
    Comparison cmp;
    auto add = [&cmp](const Check& c) {
        cmp.checks.append(c);
        cmp.pass = cmp.pass && c.level != QLatin1String("fail");
        cmp.warnings += c.level == QLatin1String("warn") ? 1 : 0;
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
             QStringLiteral("steps %1 / %2, sides %3").arg(unit.steps.size()).arg(ref.steps.size()).arg(sides)));
    if (!sameSweep || sides == 0) {
        return cmp;
    }

    const float bandCodes = float(t.bandDb * kCodesPerDb);
    QVector<QVector<int>> bands(sides);
    for (int s = 0; s < sides; ++s) {
        const QVector<float> rc = smooth3(echo1Curve(ref, s)), uc = echo1Curve(unit, s);
        const float top = rc.isEmpty() ? 0 : *std::max_element(rc.begin(), rc.end());
        QVector<int>& band = bands[s];
        for (int i = 0; i < rc.size(); ++i) {
            if (rc[i] >= top - bandCodes) {
                band.append(i);
            }
        }
        const QVector<float> rraw = echo1Curve(ref, s);
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
                 QStringLiteral("band %1 steps, %2..%3 kHz").arg(band.size())
                     .arg(ref.params.freqAt(band.value(0))).arg(ref.params.freqAt(band.isEmpty() ? 0 : band.last()))));
        add(make("echo_band_max", s, maxAbs, t.echoBandMaxDb, "dB", maxAbs <= t.echoBandMaxDb, false,
                 QStringLiteral("at %1 kHz: %2 / %3 codes").arg(ref.params.freqAt(maxAt))
                     .arg(uc.value(maxAt)).arg(rraw.value(maxAt))));

        const Resonance ru = resonance(unit, s, t.bandDb), rr = resonance(ref, s, t.bandDb);
        const double shift = ru.freqKhz - rr.freqKhz;
        add(make("resonance", s, shift, t.resonanceKhz, "kHz", std::fabs(shift) <= t.resonanceKhz, false,
                 QStringLiteral("%1 / %2 kHz").arg(ru.freqKhz).arg(rr.freqKhz)));
        const double ratio = rr.bandwidthKhz > 0 ? ru.bandwidthKhz / rr.bandwidthKhz : (ru.bandwidthKhz > 0 ? 99 : 1);
        add(make("bandwidth", s, ratio, t.bandwidthRatioMax, "x",
                 ratio >= t.bandwidthRatioMin && ratio <= t.bandwidthRatioMax, false,
                 QStringLiteral("%1 / %2 kHz").arg(ru.bandwidthKhz).arg(rr.bandwidthKhz)));

        double noiseSum = 0;
        for (int i = 0; i < ref.steps.size(); ++i) {
            noiseSum += (unit.steps[i].sides[s].noiseP50 - ref.steps[i].sides[s].noiseP50) / kCodesPerDb;
        }
        const double noise = noiseSum / ref.steps.size();
        add(make("noise", s, noise, t.noiseDb, "dB", noise <= t.noiseDb, false,
                 QStringLiteral("mean over %1 steps, transmitter off").arg(ref.steps.size())));

        QVector<float> ru_ring, rr_ring, posDiff;
        int saturatedSteps = 0;
        for (int i : band) {
            const SideStep& us = unit.steps[i].sides[s];
            const SideStep& rs = ref.steps[i].sides[s];
            ru_ring.append(us.ringdown);
            rr_ring.append(rs.ringdown);
            if (us.echo1Pos() >= 0 && rs.echo1Pos() >= 0) {
                posDiff.append(std::fabs(us.echo1Pos() - rs.echo1Pos()));
            }
            saturatedSteps += us.echo1Amp() >= 255 ? 1 : 0;
        }
        const double ringU = medianOf(ru_ring), ringR = medianOf(rr_ring);
        const double ringLimit = ringR * t.ringdownRatio + t.ringdownSlack;
        add(make("ringdown", s, ringU, ringLimit, "samples", ringU <= ringLimit, false,
                 QStringLiteral("%1 / %2 samples over the band").arg(ringU).arg(ringR)));
        const double pos = posDiff.isEmpty() ? 0 : medianOf(posDiff);
        add(make("position", s, pos, t.positionSamples, "samples", !posDiff.isEmpty() && pos <= t.positionSamples,
                 true, posDiff.isEmpty() ? QStringLiteral("no first reflection on both")
                                         : QStringLiteral("first reflection moved: placement on the stand")));
        const double satShare = band.isEmpty() ? 0 : double(saturatedSteps) / band.size();
        add(make("saturation", s, satShare, t.saturationShare, "", satShare <= t.saturationShare, true,
                 QStringLiteral("%1 of %2 band steps at 255").arg(saturatedSteps).arg(band.size())));
    }

    if (sides >= 2) {
        QVector<int> band = bands[0];
        for (int i : bands[1]) {
            if (!band.contains(i)) {
                band.append(i);
            }
        }
        const QVector<float> u0 = echo1Curve(unit, 0), u1 = echo1Curve(unit, 1);
        const QVector<float> r0 = echo1Curve(ref, 0), r1 = echo1Curve(ref, 1);
        double sum = 0;
        for (int i : band) {
            sum += std::fabs(((u0[i] - u1[i]) - (r0[i] - r1[i])) / kCodesPerDb);
        }
        const double bal = band.isEmpty() ? 0 : sum / band.size();
        add(make("balance", -1, bal, t.balanceDb, "dB", bal <= t.balanceDb, false,
                 QStringLiteral("difference between the sides against the reference's")));
    }

    if (unit.supply.valid() && ref.supply.valid()) {
        const double d = unit.supply.median - ref.supply.median;
        add(make("supply", -1, d, t.supplyV, "V", std::fabs(d) <= t.supplyV, false,
                 QStringLiteral("%1 / %2 V (min %3, max %4)").arg(unit.supply.median, 0, 'f', 2)
                     .arg(ref.supply.median, 0, 'f', 2).arg(unit.supply.min, 0, 'f', 2).arg(unit.supply.max, 0, 'f', 2)));
    } else {
        add(make("supply", -1, 0, t.supplyV, "V", false, true, QStringLiteral("no supply readings")));
    }
    if (unit.temperature.valid()) {
        const bool inRange = unit.temperature.median >= t.tempMinC && unit.temperature.median <= t.tempMaxC;
        add(make("temperature", -1, unit.temperature.median, t.tempMaxC, "C", inRange, false,
                 QStringLiteral("%1..%2 C").arg(unit.temperature.min, 0, 'f', 1).arg(unit.temperature.max, 0, 'f', 1)));
        if (ref.temperature.valid()) {
            const double d = unit.temperature.median - ref.temperature.median;
            add(make("temperature_delta", -1, d, t.tempDeltaC, "C", std::fabs(d) <= t.tempDeltaC, true,
                     QStringLiteral("%1 / %2 C").arg(unit.temperature.median, 0, 'f', 1)
                         .arg(ref.temperature.median, 0, 'f', 1)));
        }
    } else {
        add(make("temperature", -1, 0, t.tempMaxC, "C", false, true, QStringLiteral("no temperature readings")));
    }
    return cmp;
}

} // namespace sonarqa
