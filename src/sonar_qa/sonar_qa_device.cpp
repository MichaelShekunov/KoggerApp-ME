#include "sonar_qa_device.h"

#include "dev_driver.h"
#include "sonar_qa_runner.h"

#include <QCoreApplication>
#include <QThread>
#include <QQmlEngine>

namespace {

// One runner for the application (import KoggerSonarQa 1.0, SonarQa): a run goes on when its window or the settings
// page is closed, and a second window cannot start a second run on the same sonar. Nothing in main.cpp.
void registerSonarQaTypes()
{
    qmlRegisterSingletonType<SonarQaRunner>("KoggerSonarQa", 1, 0, "SonarQa", [](QQmlEngine*, QJSEngine*) -> QObject* {
        static SonarQaRunner* runner = new SonarQaRunner(QCoreApplication::instance());
        QQmlEngine::setObjectOwnership(runner, QQmlEngine::CppOwnership);
        return runner;
    });
}
Q_COREAPP_STARTUP_FUNCTION(registerSonarQaTypes)

// Runs f in the driver's thread and waits for it: the getters read its state.
template <typename F>
void inDriverThread(DevDriver* dev, F f, bool wait)
{
    if (!dev) {
        return;
    }
    if (dev->thread() == QThread::currentThread()) {
        f();
    } else {
        QMetaObject::invokeMethod(dev, f, wait ? Qt::BlockingQueuedConnection : Qt::QueuedConnection);
    }
}

// SonarQaDevice over a DevDriver. The driver lives in the device manager's thread: every call is queued there and
// the readings come back through queued signals, so this object can live in the GUI thread.
class SonarQaDevDriver : public SonarQaDevice
{
public:
    explicit SonarQaDevDriver(DevDriver* dev, QObject* parent = nullptr);

    Identity identity() const override;
    QVariantMap snapshot() const override;
    void restore(const QVariantMap& settings) override;
    void applyRun(int samples, int periodMs, int resolutionMm, int offset, int boost) override;
    void setTransmitter(int freqKhz, int pulses) override;

private:
    QPointer<DevDriver> dev_;
};

} // namespace

SonarQaDevice* makeSonarQaDevice(QObject* device, QObject* parent)
{
    DevDriver* dev = qobject_cast<DevDriver*>(device);
    return dev ? new SonarQaDevDriver(dev, parent) : nullptr;
}

SonarQaDevDriver::SonarQaDevDriver(DevDriver* dev, QObject* parent)
    : SonarQaDevice(parent), dev_(dev)
{
    if (!dev) {
        return;
    }
    connect(dev, &DevDriver::chartComplete, this,
            [this](const ChannelId&, const ChartParameters&, const QVector<QVector<uint8_t>>& data, float resolution,
                   float) { emit ping(data, resolution); });
    connect(dev, &DevDriver::supplyVoltageComplete, this, [this](float v) { emit supply(v); });
    connect(dev, &DevDriver::tempComplete, this, [this](float c) { emit temperature(c); });
    connect(dev, &QObject::destroyed, this, [this]() { emit lost(); });
}

SonarQaDevice::Identity SonarQaDevDriver::identity() const
{
    Identity id;
    DevDriver* dev = dev_.data();
    inDriverThread(dev, [dev, &id]() {
        id.name = dev->devName();
        id.firmware = dev->fwVersion();
        id.serial = dev->devSerialNumber();
        id.board = int(dev->boardVersion());
    }, true);
    return id;
}

QVariantMap SonarQaDevDriver::snapshot() const
{
    QVariantMap m;
    DevDriver* dev = dev_.data();
    inDriverThread(dev, [dev, &m]() {
        m[QStringLiteral("freq")] = dev->transFreq();
        m[QStringLiteral("pulse")] = dev->transPulse();
        m[QStringLiteral("boost")] = dev->transBoost();
        m[QStringLiteral("samples")] = dev->chartSamples();
        m[QStringLiteral("resolution")] = dev->chartResolution();
        m[QStringLiteral("offset")] = dev->chartOffset();
        m[QStringLiteral("period")] = dev->ch1Period();
        m[QStringLiteral("chart")] = dev->datasetChart();
        m[QStringLiteral("chartZ")] = dev->datasetChartZ();
        m[QStringLiteral("voltage")] = dev->datasetVoltage();
        m[QStringLiteral("temp")] = dev->datasetTemp();
    }, true);
    return m;
}

void SonarQaDevDriver::restore(const QVariantMap& s)
{
    DevDriver* dev = dev_.data();
    inDriverThread(dev, [dev, s]() {
        dev->setTransFreq(s.value(QStringLiteral("freq")).toInt());
        dev->setTransPulse(s.value(QStringLiteral("pulse")).toInt());
        dev->setTransBoost(s.value(QStringLiteral("boost")).toInt());
        dev->setChartResolution(s.value(QStringLiteral("resolution")).toInt());
        dev->setChartOffset(s.value(QStringLiteral("offset")).toInt());
        dev->setChartSamples(s.value(QStringLiteral("samples")).toInt());
        dev->setDatasetVoltage(s.value(QStringLiteral("voltage")).toInt());
        dev->setDatasetTemp(s.value(QStringLiteral("temp")).toInt());
        dev->setDatasetChartZ(s.value(QStringLiteral("chartZ")).toInt());
        dev->setDatasetChart(s.value(QStringLiteral("chart")).toInt());
        dev->setCh1Period(s.value(QStringLiteral("period")).toInt());
    }, false);
}

void SonarQaDevDriver::applyRun(int samples, int periodMs, int resolutionMm, int offset, int boost)
{
    DevDriver* dev = dev_.data();
    inDriverThread(dev, [=]() {
        dev->setTransBoost(boost);
        dev->setChartResolution(resolutionMm);
        dev->setChartOffset(offset);
        dev->setChartSamples(samples);
        // the plain 8-bit echogram: the compressed one loses detail the comparison measures
        dev->setDatasetChartZ(0);
        dev->setDatasetChart(1);
        dev->setDatasetVoltage(1);
        dev->setDatasetTemp(1);
        dev->setCh1Period(periodMs);
    }, false);
}

void SonarQaDevDriver::setTransmitter(int freqKhz, int pulses)
{
    DevDriver* dev = dev_.data();
    inDriverThread(dev, [=]() {
        dev->setTransFreq(freqKhz);
        dev->setTransPulse(pulses);
    }, false);
}
