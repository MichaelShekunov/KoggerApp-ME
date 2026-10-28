#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <cstdint>

/* Sonar acceptance on a water stand (docs/SONAR_QA.md): a sweep of the transmit frequency, each step measured with
   the transmitter off (noise) and on (ringing and reflections), compared with the reference sonar measured on the
   same stand. Pure computation, no device and no Qt GUI: tools/sonarqa_check runs it offline.

   An echogram byte of the nanoSSS firmware (l412_adc, dsp_kernels.h dk_mag2byte) is 15*log2(amplitude) minus an
   offset: 15 codes per amplitude octave, 2.49 codes per dB, so a difference in codes is a ratio in dB. 0 is below
   the floor, 255 is saturation. */
namespace sonarqa {

/// Codes of the echogram byte per dB of amplitude (15 codes per octave).
constexpr double kCodesPerDb = 2.4914542;

/// The sweep and the measurement, as the owner set them for nanoSSS (2026-10-28), and the detection margins.
struct Params {
    int freqFromKhz = 150;
    int freqToKhz = 900;
    int freqStepKhz = 5;
    int pingsPerStep = 10;      ///< measured pings per frequency and per transmitter state
    int discardPings = 2;       ///< pings dropped after each change, so none is from the previous setting
    int periodMs = 50;
    int samples = 2000;         ///< ID_CHART_SETUP count: both sides of a nanoSSS, so half per side
    int txPulses = 25;          ///< periods of the burst (above 30 the firmware switches to chirp)
    int ringMarginCodes = 15;   ///< ringing ends below the floor plus this (6 dB)
    int ringHoldSamples = 5;    ///< ... for this many samples in a row
    int echoMarginCodes = 25;   ///< a reflection is a peak at least this over the floor (10 dB)
    int echoMinGap = 10;        ///< samples between two reflections
    int maxEchoes = 4;          ///< reflection and its repeats, nearest first

    int stepCount() const;
    int freqAt(int step) const { return freqFromKhz + step * freqStepKhz; }
    /// Pings a whole run takes, dropped ones included.
    int totalPings() const { return stepCount() * 2 * (pingsPerStep + discardPings); }
    QJsonObject toJson() const;
    static Params fromJson(const QJsonObject& o);
};

/// One reflection: its sample index and its byte.
struct Echo {
    int pos = 0;
    int amp = 0;
};

/// One ping of one side.
struct PingMetrics {
    float floor = 0;        ///< 25th percentile: the background under the echoes
    float mean = 0;
    float p50 = 0;
    float p95 = 0;
    int ringdown = 0;       ///< samples from the start until the ringing has died out (transmitter on)
    int ringPeak = 0;       ///< the largest byte of the ringing
    int saturated = 0;      ///< bytes at 255 after the ringing
    QVector<Echo> echoes;   ///< nearest first (transmitter on)
};

/// Measures one ping. With the transmitter off only floor, mean, p50 and p95 matter.
PingMetrics analysePing(const uint8_t* data, int n, bool tx, const Params& p);

/// One side at one frequency: medians over the step's pings.
struct SideStep {
    float noiseMean = 0, noiseP50 = 0, noiseP95 = 0;   ///< transmitter off
    float floor = 0;                                   ///< transmitter on, under the echoes
    float ringdown = 0, ringPeak = 0;
    float saturated = 0;
    QVector<float> echoPos, echoAmp, echoPresence;     ///< per reflection index; presence = share of pings with it
    float echo1Spread = 0;                             ///< max - min of the first reflection over the pings
    int pingsOff = 0, pingsOn = 0;

    /// The first reflection's byte, or the floor when it was not seen.
    float echo1Amp() const { return echoAmp.isEmpty() || echoPresence.value(0) < 0.5f ? floor : echoAmp[0]; }
    float echo1Pos() const { return echoPos.isEmpty() || echoPresence.value(0) < 0.5f ? -1.0f : echoPos[0]; }
};

struct Step {
    int freqKhz = 0;
    QVector<SideStep> sides;
};

/// Medians over the pings of one frequency, per side.
SideStep aggregate(const QVector<PingMetrics>& off, const QVector<PingMetrics>& on, int maxEchoes);

/// A reading series (supply, temperature): what the run saw.
struct Series {
    int n = 0;
    float min = 0, median = 0, max = 0;
    bool valid() const { return n > 0; }
    static Series of(QVector<float> values);
    QJsonObject toJson() const;
    static Series fromJson(const QJsonObject& o);
};

/// Limits of the comparison. Starting values: they are to be set from the spread of the reference and of
/// known-good sonars on the stand (docs/SONAR_QA.md).
struct Tolerances {
    double echoBandMeanDb = 3.0;     ///< mean |difference| of the first reflection over the reference's band
    double echoBandMaxDb = 6.0;      ///< the largest one there
    double bandDb = 6.0;             ///< the band: within this of the reference's strongest first reflection
    double resonanceKhz = 20.0;      ///< shift of the strongest first reflection's frequency
    double bandwidthRatioMin = 0.7, bandwidthRatioMax = 1.4;
    double noiseDb = 3.0;            ///< mean noise above the reference (below is fine)
    double ringdownRatio = 1.3;      ///< ringing longer than the reference's times this ...
    double ringdownSlack = 5.0;      ///< ... plus this many samples fails
    double balanceDb = 3.0;          ///< change of the difference between the sides
    double positionSamples = 10.0;   ///< first reflection elsewhere than the reference's: placement warning
    double supplyV = 0.5;
    double tempDeltaC = 10.0;        ///< a warning
    double tempMinC = -5.0, tempMaxC = 70.0;
    double saturationShare = 0.1;    ///< a warning
    QJsonObject toJson() const;
    static Tolerances fromJson(const QJsonObject& o);
};

/// A whole run: identity, settings, the sweep's results, supply and temperature.
struct Run {
    QString kind;                    ///< "baseline", "check" or "unit"
    QString time;                    ///< ISO 8601, local
    quint32 serial = 0;
    QString deviceName, firmware;
    int board = 0;
    Params params;
    QJsonObject settings;            ///< resolution, offset, boost... the run was made with
    float resolutionM = 0;           ///< metres per sample, as the frames carried it
    QVector<Step> steps;
    Series supply, temperature;
    QJsonObject toJson() const;
    static Run fromJson(const QJsonObject& o);
};

/// One check of a comparison.
struct Check {
    QString id;          ///< machine name: echo_band_mean, resonance, ...
    int side = -1;       ///< 0, 1, or -1 for the device as a whole
    double value = 0;    ///< the unit's figure (difference or ratio)
    double limit = 0;
    QString unit;        ///< dB, kHz, x, samples, V, C
    QString level;       ///< "ok", "warn", "fail"
    QString detail;      ///< the figures behind it, for the report
};

struct Comparison {
    QVector<Check> checks;
    bool pass = true;
    int warnings = 0;
    QJsonObject toJson() const;
};

/// Compares a run with the reference run (same sweep). Fails when the sweeps differ.
Comparison compare(const Run& unit, const Run& ref, const Tolerances& t);

/// Strongest first reflection: its frequency and the -bandDb bandwidth, both in kHz; -1 when nothing was seen.
struct Resonance {
    double freqKhz = -1, bandwidthKhz = -1, peakCodes = 0;
};
Resonance resonance(const Run& run, int side, double bandDb);

} // namespace sonarqa
