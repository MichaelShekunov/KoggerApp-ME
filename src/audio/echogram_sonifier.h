#pragma once

#include <QObject>
#include <QMutex>
#include <QPointer>
#include <cstdint>

class QAudioSink;
class SonifierDevice;

class EchogramSonifier : public QObject {
    Q_OBJECT
public:
    enum FrequencyScale : uint8_t {
        LogarithmicScale = 0,
        LinearScale      = 1
    };

    static constexpr int   kBinCount = 256;
    static constexpr float kFreqMin  = 400.0f;
    static constexpr float kFreqMax  = 7000.0f;

    explicit EchogramSonifier(QObject* parent = nullptr);
    ~EchogramSonifier() override;

    bool active() const { return active_; }
    bool setActive(bool state);

    void setFrequencyScale(FrequencyScale scale);
    FrequencyScale frequencyScale() const { return scale_; }

    // Frequency a bin maps to, before the anti-alias clamp. Bin 0 is the near
    // edge and the highest pitch; kBinCount-1 is the far edge and the lowest.
    float binFrequency(int k) const;

    void setLevels(float low, float high);
    void setGain(float gain);

    // Column runs from the near edge of the visible distance window to the far
    // edge, in raw echogram amplitude (0..255) - the same values the echogram
    // image is built from.
    void pushColumn(const int16_t* samples, int count);
    void fadeOut();

    // QAudio::State / QAudio::Error as int, -1 while inactive.
    int sinkState() const;
    int sinkError() const;
    int sampleRate() const;

    // Depth of the sink's own queue - the floor on how long after a pushColumn
    // the change becomes audible. -1 while inactive.
    int   sinkBufferBytes() const;
    float sinkLatencyMs() const;

private:
    void rebuildFrequencies();

    QAudioSink*     sink_   = nullptr;
    SonifierDevice* device_ = nullptr;

    bool           active_ = false;
    FrequencyScale scale_  = LogarithmicScale;
    float          low_    = 10.0f;
    float          high_   = 100.0f;
    float          gain_   = 1.0f;
};
