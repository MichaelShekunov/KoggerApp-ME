#include "echogram_sonifier.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

namespace {

constexpr int   kLutSize    = 4096;
constexpr float kAmpEpsilon = 1.0e-4f;
// Amplitude glide. Now that the cursor path interpolates between neighbouring
// epochs, consecutive targets differ by little and this only has to take the
// edge off; 8 ms rounds the corner audibly while costing ~3 ms of settling
// against a 22 ms sink buffer. Raise it if columns still arrive as steps, lower
// it if a fast sweep smears.
constexpr float kSmoothMs   = 8.0f;
constexpr float kMixNorm    = 1.0f / 16.0f; // 1/sqrt(kBinCount)
static_assert(EchogramSonifier::kBinCount == 256,
              "kMixNorm is 1/sqrt(kBinCount) written out by hand - change both together");

// Qt hands QAudioSink a 500 ms buffer by default, which is 500 ms between moving
// the pointer and hearing the column under it. Ask for far less; every backend
// clamps to its own floor, so this is a request for "your minimum", not a number
// that has to be right. Measured on Windows/FFmpeg: 22 ms granted, 35 ms end-to-end.
constexpr int kTargetLatencyMs = 20;

// readData is handed up to ~85 ms of frames at a time. Re-reading the targets
// once per call would put that whole span between a new column and the sound, so
// refresh them every block instead - 64 frames is 1.3 ms at 48 kHz, and the extra
// mutex traffic is nothing next to the oscillator loop it guards.
constexpr int kControlBlockFrames = 64;

int bytesPerFrameOf(const QAudioFormat& f)
{
    return f.channelCount() * (f.sampleFormat() == QAudioFormat::Int16 ? 2 : 4);
}

const std::array<float, kLutSize + 1>& sineLut()
{
    static const std::array<float, kLutSize + 1> lut = [] {
        std::array<float, kLutSize + 1> table{};
        for (int i = 0; i <= kLutSize; ++i) {
            table[i] = float(std::sin(2.0 * std::numbers::pi_v<double> * double(i) / double(kLutSize)));
        }
        return table;
    }();
    return lut;
}

inline float sineAt(float phase)
{
    const auto& lut = sineLut();
    const float x = phase * float(kLutSize);
    const int   i = int(x);
    const float f = x - float(i);
    return lut[i] + (lut[i + 1] - lut[i]) * f;
}

} // namespace


// Oscillator state (amp_, phase_) belongs to the audio thread and is touched
// only inside readData; everything a caller can change goes through mutex_.
class SonifierDevice : public QIODevice {
public:
    explicit SonifierDevice(QObject* parent = nullptr) : QIODevice(parent) {}

    void configure(const QAudioFormat& format)
    {
        QMutexLocker lock(&mutex_);
        format_ = format;
        smooth_ = 1.0f - std::exp(-1000.0f / (kSmoothMs * float(format.sampleRate())));
        chunkBytes_ = qint64(format.sampleRate()) * bytesPerFrameOf(format) * kTargetLatencyMs / 1000;
    }

    int sampleRate() const
    {
        QMutexLocker lock(&mutex_);
        return format_.sampleRate();
    }

    void setIncrements(const float* increments)
    {
        QMutexLocker lock(&mutex_);
        std::copy(increments, increments + EchogramSonifier::kBinCount, inc_);
    }

    void setTargets(const float* targets)
    {
        QMutexLocker lock(&mutex_);
        std::copy(targets, targets + EchogramSonifier::kBinCount, target_);
    }

    void clearTargets()
    {
        QMutexLocker lock(&mutex_);
        std::fill(std::begin(target_), std::end(target_), 0.0f);
    }

    void setGain(float gain)
    {
        QMutexLocker lock(&mutex_);
        gain_ = gain;
    }

    void resetVoices()
    {
        std::fill(std::begin(amp_), std::end(amp_), 0.0f);
        for (int k = 0; k < EchogramSonifier::kBinCount; ++k) {
            phase_[k] = float((static_cast<uint32_t>(k) * 2654435761u) % 1000u) / 1000.0f;
        }
    }

    bool isSequential() const override { return true; }

    // Advisory: it caps how much Qt asks for in one readData, nothing more. A
    // fixed 65536 here invited 341 ms chunks.
    qint64 bytesAvailable() const override
    {
        qint64 chunk = 0;
        {
            QMutexLocker lock(&mutex_);
            chunk = chunkBytes_;
        }
        return (chunk > 0 ? chunk : 65536) + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 maxlen) override;
    qint64 writeData(const char*, qint64) override { return 0; }

private:
    mutable QMutex mutex_;
    QAudioFormat   format_;
    qint64         chunkBytes_ = 0;
    float          smooth_ = 0.001f;
    float          gain_   = 1.0f;
    float          inc_[EchogramSonifier::kBinCount]    = {};
    float          target_[EchogramSonifier::kBinCount] = {};

    float amp_[EchogramSonifier::kBinCount]   = {};
    float phase_[EchogramSonifier::kBinCount] = {};
};


qint64 SonifierDevice::readData(char* data, qint64 maxlen)
{
    int channels = 1;
    QAudioFormat::SampleFormat sampleFormat = QAudioFormat::Float;
    {
        QMutexLocker lock(&mutex_);
        channels = format_.channelCount();
        sampleFormat = format_.sampleFormat();
    }

    const int bytesPerSample = sampleFormat == QAudioFormat::Int16 ? 2 : 4;
    const int bytesPerFrame  = channels * bytesPerSample;
    if (bytesPerFrame <= 0 || maxlen < bytesPerFrame) {
        return 0;
    }

    const qint64 frames = maxlen / bytesPerFrame;
    char* out = data;

    float inc[EchogramSonifier::kBinCount];
    float target[EchogramSonifier::kBinCount];
    float gain = 0.0f;
    float smooth = 0.001f;

    for (qint64 done = 0; done < frames; done += kControlBlockFrames) {
        const qint64 block = std::min<qint64>(kControlBlockFrames, frames - done);

        // Re-read per block, not per call: a column pushed while this buffer is
        // being filled is heard within the block, not at the end of the buffer.
        {
            QMutexLocker lock(&mutex_);
            std::copy(std::begin(inc_), std::end(inc_), inc);
            std::copy(std::begin(target_), std::end(target_), target);
            gain = gain_;
            smooth = smooth_;
        }

        for (qint64 n = 0; n < block; ++n) {
            float acc = 0.0f;
            for (int k = 0; k < EchogramSonifier::kBinCount; ++k) {
                const float t = target[k];
                float a = amp_[k];
                if (a < kAmpEpsilon && t < kAmpEpsilon) {
                    amp_[k] = 0.0f;
                    continue;
                }
                a += (t - a) * smooth;
                amp_[k] = a;

                float p = phase_[k] + inc[k];
                if (p >= 1.0f) {
                    p -= 1.0f;
                }
                phase_[k] = p;

                acc += a * sineAt(p);
            }

            const float value = std::tanh(acc * kMixNorm * gain);

            if (sampleFormat == QAudioFormat::Int16) {
                const auto sample = qint16(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
                for (int c = 0; c < channels; ++c) {
                    std::memcpy(out, &sample, sizeof(sample));
                    out += sizeof(sample);
                }
            }
            else {
                for (int c = 0; c < channels; ++c) {
                    std::memcpy(out, &value, sizeof(value));
                    out += sizeof(value);
                }
            }
        }
    }

    return frames * bytesPerFrame;
}


EchogramSonifier::EchogramSonifier(QObject* parent) : QObject(parent)
{
}

EchogramSonifier::~EchogramSonifier()
{
    setActive(false);
}

bool EchogramSonifier::setActive(bool state)
{
    if (state == active_) {
        return active_;
    }

    if (!state) {
        if (sink_) {
            sink_->stop();
            sink_->deleteLater();
            sink_ = nullptr;
        }
        if (device_) {
            device_->close();
            device_->deleteLater();
            device_ = nullptr;
        }
        active_ = false;
        return false;
    }

    const QAudioDevice output = QMediaDevices::defaultAudioOutput();
    if (output.isNull()) {
        return false;
    }

    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Float);
    if (!output.isFormatSupported(format)) {
        format = output.preferredFormat();
    }
    if (format.sampleRate() <= 0 || format.channelCount() <= 0) {
        return false;
    }
    if (format.sampleFormat() != QAudioFormat::Float && format.sampleFormat() != QAudioFormat::Int16) {
        format.setSampleFormat(QAudioFormat::Int16);
        if (!output.isFormatSupported(format)) {
            return false;
        }
    }

    device_ = new SonifierDevice(this);
    device_->configure(format);
    device_->resetVoices();
    device_->setGain(gain_);
    if (!device_->open(QIODevice::ReadOnly)) {
        device_->deleteLater();
        device_ = nullptr;
        return false;
    }

    active_ = true;
    rebuildFrequencies();

    sink_ = new QAudioSink(output, format, this);
    // Must precede start(): afterwards Qt ignores it, and the default is 500 ms.
    sink_->setBufferSize(int(qint64(format.sampleRate()) * bytesPerFrameOf(format) * kTargetLatencyMs / 1000));
    sink_->start(device_);
    if (sink_->error() != QAudio::NoError) {
        setActive(false);
        return false;
    }

    return true;
}

void EchogramSonifier::setFrequencyScale(FrequencyScale scale)
{
    if (scale_ == scale) {
        return;
    }
    scale_ = scale;
    rebuildFrequencies();
}

void EchogramSonifier::setLevels(float low, float high)
{
    low_  = low;
    high_ = high;
}

void EchogramSonifier::setGain(float gain)
{
    gain_ = std::clamp(gain, 0.0f, 4.0f);
    if (device_) {
        device_->setGain(gain_);
    }
}

void EchogramSonifier::pushColumn(const int16_t* samples, int count)
{
    if (!device_ || !samples || count <= 0) {
        return;
    }

    // Mirrors Plot2DEchogram::updateColors(): the window that maps raw amplitude
    // onto displayed brightness maps it onto loudness here, so the level slider
    // moves both together.
    const float span   = (high_ - low_) * 2.55f;
    const float scale  = span > 0.0f ? 255.0f / span : 1.0e4f;
    const float offset = low_ * 2.5f;

    float target[kBinCount];
    for (int k = 0; k < kBinCount; ++k) {
        const int src = count == kBinCount
                          ? k
                          : int(std::min<int64_t>(int64_t(k) * count / kBinCount, count - 1));
        const float brightness = std::clamp((float(samples[src]) - offset) * scale, 0.0f, 255.0f);
        target[k] = brightness / 255.0f;
    }

    device_->setTargets(target);
}

void EchogramSonifier::fadeOut()
{
    if (device_) {
        device_->clearTargets();
    }
}

int EchogramSonifier::sinkState() const
{
    return sink_ ? int(sink_->state()) : -1;
}

int EchogramSonifier::sinkError() const
{
    return sink_ ? int(sink_->error()) : -1;
}

int EchogramSonifier::sampleRate() const
{
    return device_ ? device_->sampleRate() : -1;
}

int EchogramSonifier::sinkBufferBytes() const
{
    return sink_ ? int(sink_->bufferSize()) : -1;
}

float EchogramSonifier::sinkLatencyMs() const
{
    if (!sink_ || !device_) {
        return -1.0f;
    }
    const int rate = device_->sampleRate();
    const int bpf  = bytesPerFrameOf(sink_->format());
    if (rate <= 0 || bpf <= 0) {
        return -1.0f;
    }
    return 1000.0f * float(sink_->bufferSize()) / (float(rate) * float(bpf));
}

float EchogramSonifier::binFrequency(int k) const
{
    // t is 1 at the near edge and 0 at the far edge: closer echoes sound higher.
    const float t = 1.0f - float(std::clamp(k, 0, kBinCount - 1)) / float(kBinCount - 1);
    return scale_ == LogarithmicScale
             ? kFreqMin * std::pow(kFreqMax / kFreqMin, t)
             : kFreqMin + (kFreqMax - kFreqMin) * t;
}

void EchogramSonifier::rebuildFrequencies()
{
    if (!device_) {
        return;
    }

    const float rate = float(device_->sampleRate());
    if (rate <= 0.0f) {
        return;
    }
    const float ceiling = 0.45f * rate;

    float inc[kBinCount];
    for (int k = 0; k < kBinCount; ++k) {
        inc[k] = std::min(binFrequency(k), ceiling) / rate;
    }

    device_->setIncrements(inc);
}
