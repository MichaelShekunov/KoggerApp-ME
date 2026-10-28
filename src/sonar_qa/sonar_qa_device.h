#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <cstdint>

/* The sonar under test as the acceptance run sees it (docs/SONAR_QA.md): who it is, its settings, the transmitter,
   and its pings, supply and temperature. SonarQaDevDriver adapts the application's DevDriver; tools/sonarqa_check
   has a simulated sonar. */
class SonarQaDevice : public QObject
{
    Q_OBJECT
public:
    struct Identity {
        QString name;
        QString firmware;
        quint32 serial = 0;
        int board = 0;
    };

    explicit SonarQaDevice(QObject* parent = nullptr) : QObject(parent) {}

    virtual Identity identity() const = 0;
    /// The settings a run changes (transmitter, echogram, period, dataset), to be put back after it.
    virtual QVariantMap snapshot() const = 0;
    virtual void restore(const QVariantMap& settings) = 0;
    /// The settings of the run: samples (both sides), period, resolution and offset in mm, booster; the echogram
    /// uncompressed, the supply voltage and the temperature on.
    virtual void applyRun(int samples, int periodMs, int resolutionMm, int offset, int boost) = 0;
    virtual void setTransmitter(int freqKhz, int pulses) = 0;

signals:
    /// A ping: one array per side, metres per sample.
    void ping(const QVector<QVector<uint8_t>>& sides, float resolutionM);
    void supply(float volts);
    void temperature(float celsius);
    /// The device went away.
    void lost();
};

/// The adapter for a device from the application's device list (a DevDriver; sonar_qa_device.cpp); nullptr for
/// anything else.
SonarQaDevice* makeSonarQaDevice(QObject* device, QObject* parent);
