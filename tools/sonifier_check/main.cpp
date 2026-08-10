#include "echogram_sonifier.h"

#include <QAudio>
#include <QCoreApplication>
#include <QDebug>
#include <QTimer>

#include <array>
#include <cstdlib>

namespace {

constexpr int kSteps    = 60;
constexpr int kStepMs   = 50;
constexpr int kBandHalf = 4;

// The sink's queue is the floor on how long after a column the sound changes.
// Qt's default is 500 ms, which makes pointer-follow useless; EchogramSonifier
// asks for ~20 ms and backends clamp upward. Fail well below the default so a
// dropped setBufferSize call cannot come back unnoticed.
constexpr float kMaxLatencyMs = 100.0f;

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    EchogramSonifier sonifier;
    sonifier.setLevels(10.0f, 100.0f);
    sonifier.setGain(1.0f);
    if (argc > 1 && QString::fromLatin1(argv[1]) == QLatin1String("--linear")) {
        sonifier.setFrequencyScale(EchogramSonifier::LinearScale);
    }

    // Direction: near edge high, far edge low. Checked before any audio, because
    // an upstream merge can silently flip the ramp and nothing else would notice.
    {
        const int   last  = EchogramSonifier::kBinCount - 1;
        const float fNear = sonifier.binFrequency(0);
        const float fFar  = sonifier.binFrequency(last);
        qInfo("bin 0 (near) = %.0f Hz, bin %d (far) = %.0f Hz", double(fNear), last, double(fFar));

        bool monotonic = true;
        for (int k = 1; k <= last; ++k) {
            if (sonifier.binFrequency(k) > sonifier.binFrequency(k - 1)) {
                monotonic = false;
                break;
            }
        }
        if (fNear < fFar || !monotonic) {
            qCritical("FAIL: frequency ramp is not descending from near to far");
            return 3;
        }
        if (qAbs(fNear - EchogramSonifier::kFreqMax) > 1.0f
            || qAbs(fFar - EchogramSonifier::kFreqMin) > 1.0f) {
            qCritical("FAIL: ramp endpoints are not kFreqMax..kFreqMin");
            return 3;
        }
    }

    if (!sonifier.setActive(true)) {
        qCritical("FAIL: could not start audio output");
        return 2;
    }
    qInfo("sample rate: %d Hz", sonifier.sampleRate());
    qInfo("sink buffer: %d bytes = %.1f ms", sonifier.sinkBufferBytes(), double(sonifier.sinkLatencyMs()));

    // One bright band walking from the near edge of the range to the far edge.
    // Audibly the pitch must sweep 7 kHz -> 400 Hz over three seconds: near is
    // high, far is low.
    auto* timer = new QTimer(&app);
    int step = 0;
    QObject::connect(timer, &QTimer::timeout, [&sonifier, &step, timer] {
        std::array<int16_t, EchogramSonifier::kBinCount> column{};
        const int centre = step * (EchogramSonifier::kBinCount - 1) / (kSteps - 1);
        for (int k = 0; k < int(column.size()); ++k) {
            column[k] = std::abs(k - centre) < kBandHalf ? int16_t(255) : int16_t(0);
        }
        sonifier.pushColumn(column.data(), int(column.size()));

        if (++step < kSteps) {
            return;
        }

        timer->stop();
        const int   state   = sonifier.sinkState();
        const int   error   = sonifier.sinkError();
        const float latency = sonifier.sinkLatencyMs();
        qInfo("sink state=%d error=%d", state, error);

        const bool pulling = state == int(QAudio::ActiveState) && error == int(QAudio::NoError);
        const bool prompt  = latency > 0.0f && latency <= kMaxLatencyMs;
        if (!pulling) {
            qInfo("FAIL: audio sink is not actively pulling samples");
        }
        else if (!prompt) {
            qInfo("FAIL: sink latency %.1f ms exceeds %.1f ms - pointer-follow will lag",
                  double(latency), double(kMaxLatencyMs));
        }
        else {
            qInfo("PASS");
        }
        QCoreApplication::exit(pulling && prompt ? 0 : 1);
    });
    timer->start(kStepMs);

    return app.exec();
}
