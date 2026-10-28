#pragma once

#include "sonar_qa_device.h"

#include <QTimer>

/* A simulated nanoSSS in a tank for the acceptance module (docs/SONAR_QA.md): a target and its repeats, a resonance,
   ringing after the burst, noise, supply and temperature. tools/sonarqa_check runs every scenario on it; in the
   application it is offered only when KOGGER_SONARQA_DEMO is set (checks of the window without a stand). */
class SonarQaDemoDevice : public SonarQaDevice
{
public:
    struct Model {
        quint32 serial = 1001;
        QString firmware = QStringLiteral("11.21");
        int board = 17;
        double f0 = 600, bw = 220;          ///< kHz: resonance and the response's half-power width
        double gainDb = 0;
        double floorOn = 30, floorOff = 36; ///< codes; with the transmitter off the firmware's filter is short
        double ringTop = 230, ringLen = 40, ringScale = 1;
        int targetPos = 180;
        double echoTop = 205, multiLoss = 25;
        double supply = 12.0, temp = 22.0;
        bool tagPings = false;              ///< the last two bytes carry frequency / 5 and pulses (the raw-file check)
    };

    /// periodMs 0 = as fast as the event loop turns; otherwise the period the run asks for.
    explicit SonarQaDemoDevice(bool realTime, QObject* parent = nullptr);

    Model m;
    QVariantMap settings;
    QVariantMap restored;     ///< what restore() was last given
    bool silent = false;

    Identity identity() const override;
    QVariantMap snapshot() const override { return settings; }
    void restore(const QVariantMap& s) override;
    void applyRun(int samples, int periodMs, int resolutionMm, int offset, int boost) override;
    /// Like the firmware, a change takes effect between pings: the ping after the command still has the old one.
    void setTransmitter(int freqKhz, int pulses) override;

private:
    void emitPing();
    double noise();

    QTimer timer_;
    bool realTime_;
    int freq_ = 700, pulses_ = 10, pendingFreq_ = -1, pendingPulses_ = -1, count_ = 0;
    quint32 rng_ = 12345;
};
