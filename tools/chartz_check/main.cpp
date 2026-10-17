// Standalone check of the compressed-echogram path, IDBinChartZ -> IDBinChart, on frames a real
// two-channel sonar sent. Every ping in the capture carries BOTH formats: the plain ID_CHART v1
// (the sides interleaved) and ID_CHART_Z v1 (the sides end to end, compressed). Decoded by the
// application's own classes, the compressed ping has to match its plain twin side for side:
// port against port, starboard against starboard.
//
//   chartz_check <capture.bin>          (set CHARTZ_VERBOSE=1 for a line per ping)
//
// The capture is the raw byte stream off the serial port (nanosss_v119_both.bin: firmware 11.9,
// 4000 cells, both DATASET bits). Exit 0 = pass, 1 = fail, 2 = could not run.
//
// Built against 6c38f881's id_binnary.* (-DCHARTZ_SRC=<dir>) this check must FAIL: that version
// refused every v1 compressed frame, which is the bug it is here to keep fixed.
#include <QCoreApplication>
#include <QFile>
#include <QMap>
#include <QVector>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "id_binnary.h"

namespace {

struct Ping {
    QVector<int> port, stbd;
};

Ping take(IDBinChart& c, bool twoSides)
{
    Ping p;
    const int n = c.chartSize();
    for (int i = 0; i < n; ++i) {
        p.port.append(c.logData8()[i]);
        if (twoSides) {
            p.stbd.append(c.logData28()[i]);
        }
    }
    return p;
}

double rms(const QVector<int>& a, const QVector<int>& b)
{
    if (a.size() != b.size() || a.isEmpty()) {
        return 1e9;
    }
    double s = 0.0;
    for (int i = 0; i < a.size(); ++i) {
        const double d = a[i] - b[i];
        s += d * d;
    }
    return std::sqrt(s / a.size());
}

double both(const Ping& z, const Ping& p, bool swap)
{
    const double a = rms(z.port, swap ? p.stbd : p.port);
    const double b = rms(z.stbd, swap ? p.port : p.stbd);
    return std::sqrt((a * a + b * b) / 2.0);
}

int fails = 0;
void ok(const char* name, bool bad, const QString& detail)
{
    std::printf("  %-54s %s %s\n", name, bad ? "FAIL" : "ok", detail.toUtf8().constData());
    if (bad) {
        ++fails;
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::printf("usage: chartz_check <capture.bin>\n");
        return 2;
    }
    const bool verbose = std::getenv("CHARTZ_VERBOSE") != nullptr;
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("cannot open %s\n", argv[1]);
        return 2;
    }
    QByteArray raw = f.readAll();

    IDBinChart plain;           // the plain frames go here
    IDBinChart zsink;           // and the compressed ones are decoded into this one
    IDBinChartZ chartz(&zsink); // two sinks, so the two formats of one ping cannot overwrite each other

    QVector<Ping> P, Z;
    int zframes = 0, zrefused = 0, pframes = 0;
    Parsers::FrameParser proto;
    proto.setContext(reinterpret_cast<uint8_t*>(raw.data()), static_cast<uint32_t>(raw.size()));
    while (proto.availContext() > 0) {
        proto.process();
        if (!proto.isComplete()) {
            continue;
        }
        if (proto.id() == Parsers::ID_CHART) {
            ++pframes;
            plain.parse(proto);
            if (plain.readAndClearIsCompleteChart()) {
                P.append(take(plain, plain.lastVersion() == Parsers::v1));
            }
        } else if (proto.id() == Parsers::ID_CHART_Z) {
            ++zframes;
            if (chartz.parse(proto) == Parsers::respErrorVersion) {
                ++zrefused;
            }
            if (zsink.readAndClearIsCompleteChart()) {
                Z.append(take(zsink, true));
            }
        }
    }

    std::printf("capture: %lld bytes, %d plain frames, %d compressed frames\n\n",
                static_cast<long long>(raw.size()), pframes, zframes);

    ok("compressed frames are not refused", zrefused != 0,
       QString("%1 of %2 refused").arg(zrefused).arg(zframes));
    ok("compressed pings decode", Z.size() < 5, QString("%1 pings").arg(Z.size()));
    ok("plain pings assemble", P.size() < 5, QString("%1 pings").arg(P.size()));
    if (Z.size() < 5 || P.size() < 5) {
        std::printf("\nFAILED\n");
        return 1;
    }

    // The capture starts wherever the port happened to be, so its first plain ping can be a
    // fragment. Take the length most plain pings have, not the first one's.
    QMap<int, int> sizes;
    for (const Ping& p : P) {
        ++sizes[p.port.size()];
    }
    int np = 0, npCount = 0;
    for (auto it = sizes.cbegin(); it != sizes.cend(); ++it) {
        if (it.value() > npCount) {
            np = it.key();
            npCount = it.value();
        }
    }
    if (verbose) {
        for (int i = 0; i < P.size(); ++i) {
            std::printf("    P[%d] %d / %d cells\n", i, int(P[i].port.size()), int(P[i].stbd.size()));
        }
        for (int i = 0; i < Z.size(); ++i) {
            std::printf("    Z[%d] %d / %d cells\n", i, int(Z[i].port.size()), int(Z[i].stbd.size()));
        }
    }
    int wrongSize = 0;
    for (const Ping& z : Z) {
        if (z.port.size() != np || z.stbd.size() != np) {
            ++wrongSize;
        }
    }
    ok("each side of a decoded ping has the plain ping's length", wrongSize != 0,
       QString("%1 cells a side, %2 wrong").arg(np).arg(wrongSize));

    // Pair each compressed ping with its plain twin. The indices do not line up by themselves: a
    // plain ping only completes when the next one starts, and at the start of a capture IDBinChart
    // reports fragments (0 and 120 cells here) as complete pings. So the fragments are dropped,
    // and the twin is found by content -- but only accepted when it is CLEARLY closer than the
    // other pings around it. Neighbouring pings of the same noise come out rms ~4-5 apart, and
    // without that margin a neighbour passes for the twin (it did, in the first version of this).
    QVector<int> full;                      // indices into P of whole pings
    for (int j = 0; j < P.size(); ++j) {
        if (P[j].port.size() == np && P[j].stbd.size() == np) {
            full.append(j);
        }
    }
    double worst = 0.0;
    int swapped = 0, conclusive = 0, paired = 0;
    QMap<int, int> offsets;                 // twin index - compressed index, should be one value
    for (int i = 0; i < Z.size(); ++i) {
        double best = 1e9, second = 1e9;
        int bk = -1;
        for (int k = 0; k < full.size(); ++k) {
            const double e = both(Z[i], P[full[k]], false);
            if (e < best) {
                second = best;
                best = e;
                bk = k;
            } else if (e < second) {
                second = e;
            }
        }
        const bool clear = bk >= 0 && best <= 4.0 && second >= 2.0 * best;
        if (verbose) {
            std::printf("    Z[%d] -> P[%d] rms %.2f, next best %.2f%s\n", i, bk >= 0 ? full[bk] : -1,
                        best, second, clear ? "" : "  (no clear twin)");
        }
        if (!clear) {
            continue;
        }
        ++paired;
        ++offsets[bk - i];
        worst = qMax(worst, best);
        const int bj = full[bk];
        // The same comparison with the sides exchanged must come out worse, or the halves are
        // mirrored. A ping whose two sides are identical cannot tell, and counts for neither.
        const double sw = both(Z[i], P[bj], true);
        if (sw != best) {
            ++conclusive;
            if (sw < best) {
                ++swapped;
            }
        }
    }
    // The first and last compressed pings can lack a whole twin in the capture (it starts and ends
    // wherever the port did); every other one must have one.
    ok("every compressed ping inside the capture has a clear twin", paired < Z.size() - 2,
       QString("%1 of %2").arg(paired).arg(Z.size()));
    ok("the twins line up one to one (a single index offset)", offsets.size() != 1,
       QString("%1 distinct offset(s)").arg(offsets.size()));
    ok("decoded ping matches its twin (rms <= 4)", worst > 4.0,
       QString("worst rms %1").arg(worst, 0, 'f', 2));
    ok("port and starboard are not exchanged", swapped != 0 || conclusive == 0,
       QString("%1 swapped of %2 that can tell").arg(swapped).arg(conclusive));

    std::printf("\n%s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
