#include "dev_driver.h"

#include <QDateTime>
#include <time.h>
#include <core.h>
#include <QDateTime>
#include <QDebug>
#include <limits>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <type_traits>
#include <cmath>
#include <QVariant>
#include <QXmlStreamWriter>
#include <QUrl>
#if defined(Q_OS_ANDROID)
#include "qserialportinfo.h"   // vendored, platform/android/src/qtandroidserialport
#else
#include <QSerialPortInfo>
#endif
#include "notifications.h"
#include "attitude_arbiter.h"

extern Core core;
extern Notifications notifications;

namespace {
// Notifications live on the GUI thread; the driver may not.
void notifyInfo(const QString& text)
{
    QMetaObject::invokeMethod(&notifications, [text]() { notifications.info(text); }, Qt::QueuedConnection);
}
void notifyWarning(const QString& text)
{
    QMetaObject::invokeMethod(&notifications, [text]() { notifications.warning(text); }, Qt::QueuedConnection);
}
} // namespace

DevDriver::DevDriver(QObject *parent)
    : QObject(parent),
      datasetState_(false),
      distSetupState_(false),
      chartSetupState_(false),
      dspSetupState_(false),
      transcState_(false),
      soundSpeedState_(false),
      uartState_(false),
      errorFreezeCnt_(0),
      averageChartLosses_(0),
      linkUuid_(QUuid())
{
    qRegisterMetaType<uint8_t>("uint8_t");
    qRegisterMetaType<uint16_t>("uint16_t");
    qRegisterMetaType<uint32_t>("uint32_t");
    qRegisterMetaType<ProtoBinOut>("ProtoBinOut");
    qRegisterMetaType<Parsers::ProtoBinOut>("Parsers::ProtoBinOut");
    qRegisterMetaType<QVector<QVector<uint8_t>>>("QVector<QVector<uint8_t>>");
    qRegisterMetaType<ChannelId>("ChannelId");
    qRegisterMetaType<ChartParameters>("ChartParameters");

    regID(idTimestamp = new IDBinTimestamp(), &DevDriver::receivedTimestamp);

    regID(idDist = new IDBinDist(), &DevDriver::receivedDist);
    regID(idChart = new IDBinChart(), &DevDriver::receivedChart);
    connect(idChart, &IDBinChart::rawDataRecieved, this, &DevDriver::receivedRaw);
    // Same callback on purpose: IDBinChartZ decodes into idChart, and
    // receivedChart reads from idChart, so it cannot tell the two apart.
    regID(idChartZ = new IDBinChartZ(idChart), &DevDriver::receivedChart);
    regID(idAtt = new IDBinAttitude(), &DevDriver::receivedAtt);
    regID(idTemp = new IDBinTemp(), &DevDriver::receivedTemp);
    regID(idEncoder = new IDBinEncoder(), &DevDriver::receivedEncoder);
    regID(idVoltage = new IDBinVoltage(), &DevDriver::receivedVoltage);
    regID(idPreset = new IDBinPreset(), &DevDriver::receivedPreset);
    regID(idRingStat = new IDBinRingStat(), &DevDriver::receivedRingStat);
    regID(idBus = new IDBinBus(), &DevDriver::receivedBus);
    regID(idPin = new IDBinPin(), &DevDriver::receivedPin);

    regID(idDataset = new IDBinDataset(), &DevDriver::receivedDataset, true);
    regID(idDistSetup = new IDBinDistSetup(), &DevDriver::receivedDistSetup, true);
    regID(idChartSetup = new IDBinChartSetup(), &DevDriver::receivedChartSetup, true);
    regID(idDSPSetup = new IDBinDSPSetup(), &DevDriver::receivedDSPSetup, true);
    regID(idTransc = new IDBinTransc(), &DevDriver::receivedTransc, true);
    regID(idSoundSpeed = new IDBinSoundSpeed(), &DevDriver::receivedSoundSpeed, true);
    regID(idNet = new IDBinNet(), &DevDriver::receivedNet, true);
    regID(idUART = new IDBinUART(), &DevDriver::receivedUART, true);
    regID(idWifi = new IDBinWifi(), &DevDriver::receivedWifi);
    regID(idWifiNet = new IDBinWifiNet(), &DevDriver::receivedWifiNet);
    regID(idWifiSurvey = new IDBinWifiSurvey(), &DevDriver::receivedWifiSurvey);
    regID(idWifiRf = new IDBinWifiRf(), &DevDriver::receivedWifiRf);
    regID(idWifiProfile = new IDBinWifiProfile(), &DevDriver::receivedWifiProfile);
    regID(idVersion = new IDBinVersion(), &DevDriver::receivedVersion, true);

    regID(idMark = new IDBinMark(), &DevDriver::receivedMark);
    regID(idFlash = new IDBinFlash(), &DevDriver::receivedFlash);
    regID(idBoot = new IDBinBoot(), &DevDriver::receivedBoot);
    regID(idUpdate = new IDBinUpdate(), &DevDriver::receivedUpdate);

    regID(idNav = new IDBinNav(), &DevDriver::receivedNav);
    regID(idBoatStatus = new IDBinBoatStatus(), &DevDriver::receivedBoatStatus);
    regID(idRecorderStatus = new IDBinRecorderStatus(), &DevDriver::receivedRecorderStatus);
    regID(idNavSensorStatus = new IDBinNavSensorStatus(), &DevDriver::receivedNavSensorStatus);
    regID(idDVL = new IDBinDVL(), &DevDriver::receivedDVL);
    regID(idDVLMode = new IDBinDVLMode(), &DevDriver::receivedDVLMode);

    regID(idUSBL = new IDBinUsblSolution(), &DevDriver::receivedUSBL);
    regID(idUsblDiag = new IDBinUsblDiag(), &DevDriver::receivedUsblDiag);

    regID(idUSBLControl = new IDBinUsblControl(), &DevDriver::receivedUSBLControl);

    regID(idModemSolution = new IDBinModemSolution(), &DevDriver::receivedModemSolution);

    regID(idServoControl = new IDBinServoControl(), &DevDriver::receivedServoControl, true);
    // Not a setup command: it is control-only, so it has nothing to answer a cold-start
    // request with and must not take part in the sweep.
    regID(idStandScan = new IDBinStandScan(), &DevDriver::receivedStandScan);
    regID(idPwmRoute = new IDBinPwmRoute(), &DevDriver::receivedPwmRoute, true);
    regID(idDevSync = new IDBinDevSync(), &DevDriver::receivedDevSync, true);

    connect(&m_processTimer, &QTimer::timeout, this, &DevDriver::process);

    QObject::connect(idDataset, &IDBin::notifyDevDriver, this, &DevDriver::setDatasetState);
    QObject::connect(idDistSetup, &IDBin::notifyDevDriver, this, &DevDriver::setDistSetupState);
    QObject::connect(idChartSetup, &IDBin::notifyDevDriver, this, &DevDriver::setChartSetupState);
    QObject::connect(idDSPSetup, &IDBin::notifyDevDriver, this, &DevDriver::setDspSetupState);
    QObject::connect(idTransc, &IDBin::notifyDevDriver, this, &DevDriver::setTranscState);
    QObject::connect(idSoundSpeed, &IDBin::notifyDevDriver, this, &DevDriver::setSoundSpeedState);
    QObject::connect(idUART, &IDBin::notifyDevDriver, this, &DevDriver::setUartState);
    QObject::connect(idServoControl, &IDBin::notifyDevDriver, this, &DevDriver::setServoControlState);
    QObject::connect(idPwmRoute, &IDBin::notifyDevDriver, this, &DevDriver::setPwmRouteState);
    QObject::connect(idDevSync, &IDBin::notifyDevDriver, this, &DevDriver::setDevSyncState);

    wifiClock_.start();
    // Wi-Fi module sequences that must finish whether or not a settings page is open.
    wifiPort_ = new WifiPortChange({
        [this](int baud) { if (m_state.connect) { idUART->setBaudrate(U4(baud)); emit UARTChanged(); } },
        [this](int baud) {
            LinkManagerWrapper* links = core.getLinkManagerWrapperPtr();
            const QUuid link = linkUuid_;
            if (links)
                QMetaObject::invokeMethod(links, [links, link, baud]() { links->updateBaudrate(link, baud); }, Qt::QueuedConnection);
        },
        [this]() { if (m_state.connect) idWifi->simpleRequest(v0); },
        [this]() { if (m_state.connect) idFlash->flashing(); }
    }, WifiPortChange::Timing(), this);
    connect(wifiPort_, &WifiPortChange::activeChanged, this, &DevDriver::wifiPortChanged);
    connect(wifiPort_, &WifiPortChange::finished, this, [this](bool ok, int newBaud, int oldBaud) {
        if (ok) {
            qInfo().noquote() << wifiTag() << QStringLiteral("wifi-port: the module answered at %1, saved in the module").arg(newBaud);
            notifyInfo(tr("Port speed %1 set, the module saved it").arg(newBaud));
        } else if (wifiPort_->refused()) {
            qInfo().noquote() << wifiTag() << QStringLiteral("wifi-port: the module refused %1, the port stays at %2").arg(newBaud).arg(oldBaud);
            notifyWarning(tr("The module refused %1 bit/s: nothing changed, the port stays at %2").arg(newBaud).arg(oldBaud));
        } else if (wifiPort_->moduleKeepsIt()) {
            qInfo().noquote() << wifiTag() << QStringLiteral("wifi-port: no answer at %1; the module keeps it (firmware 0.15+), the link stays at %1").arg(newBaud);
            notifyWarning(tr("No answer from the module at %1. It keeps that rate (firmware 0.15 saves it at once), so the link stays at %1. "
                             "If this device cannot do %1, hold the module's BOOT button 5 s: both ports go back to 921600, then set this link to 921600.")
                              .arg(newBaud));
        } else {
            qInfo().noquote() << wifiTag() << QStringLiteral("wifi-port: no answer at %1, link back at %2").arg(newBaud).arg(oldBaud);
            notifyWarning(tr("No answer from the module at %1: the link is back at %2. An unsaved speed is gone after the module's next reboot.")
                              .arg(newBaud).arg(oldBaud));
        }
    });
    // A line's record (ID_WIFI_NET v3): read, sent back with only the edited fields changed, read back. Used for
    // the network side of either line and for the rate of the port this app is not connected through.
    wifiLine_ = new WifiLineChange({
        [this](int) { if (m_state.connect) idWifiNet->simpleRequest(v3); },
        [this](int line, const WifiLineEdit& edit) {
            const IDBinWifiNet::Line& l = idWifiNet->line(line);
            if (!m_state.connect || !l.valid)
                return false;
            const WifiLineRecord r = edit.appliedTo({ l.mode, l.dest, l.ip, l.rport, l.lport, l.baud, l.txPin, l.rxPin });
            return idWifiNet->setLine(uint8_t(line), r.mode, r.dest, IDBinWifiNet::addrText(r.ip), r.rport, r.lport,
                                      r.baud, r.txPin, r.rxPin);
        },
        [this](int) {
            if (!m_state.connect)
                return;
            idWifiNet->simpleRequest(v3);
            if (idWifiNet->portSupport() != 0)
                idWifiNet->requestPorts();
        }
    }, WifiLineChange::Timing(), this);
    connect(wifiLine_, &WifiLineChange::activeChanged, this, &DevDriver::wifiPortChanged);
    connect(wifiLine_, &WifiLineChange::finished, this, [this](int outcome, int line, int resp) {
        const QString port = line == 0 ? QStringLiteral("X1") : QStringLiteral("X2");
        const bool rate = wifiLine_->edit().rateOnly();
        const QString what = rate ? QStringLiteral("rate %1").arg(wifiLine_->target()) : QStringLiteral("line settings");
        switch (outcome) {
        case WifiLineChange::Done:
            qInfo().noquote() << wifiTag() << QStringLiteral("wifi-port: %1 %2 applied through its line record, saved in the module").arg(port, what);
            notifyInfo(rate ? tr("Port %1: %2 bit/s set, the module saved it").arg(port).arg(wifiLine_->target())
                            : tr("Port %1: line settings applied").arg(port));
            break;
        case WifiLineChange::NoRecord:
            qInfo().noquote() << wifiTag() << QStringLiteral("wifi-port: %1 %2: no line record from the module, nothing sent").arg(port, what);
            notifyWarning(tr("Port %1: the module did not send the line's settings, nothing changed").arg(port));
            break;
        case WifiLineChange::NotSent:
            qInfo().noquote() << wifiTag() << QStringLiteral("wifi-port: %1 %2: the line record is not usable, nothing sent").arg(port, what);
            notifyWarning(tr("Port %1: the line's settings are not known, nothing changed").arg(port));
            break;
        case WifiLineChange::Refused:
            qInfo().noquote() << wifiTag() << QStringLiteral("wifi-port: %1 %2: refused by the module (code %3)").arg(port, what).arg(resp);
            notifyWarning(rate ? tr("Port %1: the module refused %2 bit/s (code %3)").arg(port).arg(wifiLine_->target()).arg(resp)
                               : tr("Port %1: the module refused the line settings (code %2)").arg(port).arg(resp));
            break;
        default:
            qInfo().noquote() << wifiTag() << QStringLiteral("wifi-port: %1 %2: no answer, read back").arg(port, what);
            notifyWarning(tr("Port %1: no answer from the module; what is shown is what it reports").arg(port));
            break;
        }
        emit wifiLineChangeFinished(line, outcome, resp, rate);
    });
    wifiRadioCheck_ = new WifiRadioCheck({
        [this]() { if (m_state.connect) idWifi->simpleRequest(v7); },
        [this](double powerDbm, int protocols, int widthMhz, int powerSave) {
            if (m_state.connect)
                idWifi->setRadio(uint8_t(qBound(8, int(std::lround(powerDbm * 4.0)), 80)), uint8_t(protocols),
                                 WifiRadioRules::widthFor(protocols, widthMhz) == 40 ? 2 : 1, uint8_t(qBound(0, powerSave, 2)));
        }
    }, WifiRadioCheck::Timing(), this);
    connect(wifiRadioCheck_, &WifiRadioCheck::activeChanged, this, &DevDriver::wifiRadioCheckChanged);
    // A whole profile (firmware 0.22): the pieces, the apply, then the module's reboot.
    wifiProfileUpload_ = new WifiProfileUpload({
        [this](int ver, const QByteArray& payload) { if (m_state.connect) idWifiProfile->sendSetting(ver, payload); },
        [this](int ver) { if (m_state.connect) idWifiProfile->ask(ver); }
    }, WifiProfileUpload::Timing(), this);
    connect(wifiProfileUpload_, &WifiProfileUpload::progressChanged, this, [this]() {
        const int phase = wifiProfileUpload_->phase();
        const int step = phase == WifiProfileUpload::Begin || phase == WifiProfileUpload::Pieces ? 1
                         : phase == WifiProfileUpload::Idle ? wifiProfileStep_ : 2;
        wifiProfileStep_ = step;
        emit wifiProfileChanged();
    });
    connect(wifiProfileUpload_, &WifiProfileUpload::finished, this, &DevDriver::wifiProfileFinished);
    wifiProfileAskTimer_.setSingleShot(true);
    wifiProfileAskTimer_.setInterval(300);       // a few settings in a row: one question
    connect(&wifiProfileAskTimer_, &QTimer::timeout, this, [this]() {
        if (m_state.connect && wifiProfileSupport() == 1)
            idWifiProfile->requestAll();
    });
    wifiProfileBackTimer_.setInterval(1000);
    connect(&wifiProfileBackTimer_, &QTimer::timeout, this, [this]() {
        if (wifiProfileStep_ != 3) {
            wifiProfileBackTimer_.stop();
            return;
        }
        if (wifiClock_.elapsed() - wifiProfileSavedAtMs_ > 30000) {
            wifiProfileBackTimer_.stop();
            wifiProfileStep_ = 0;
            wifiProfileResult_ = tr("The module took \"%1\" but has not answered here for 30 s since. It may run at another "
                                    "rate or address now (X%2 %3 bit/s, address %4 by the profile): look for it in the device list.")
                                     .arg(wifiProfileLoading_.name).arg(wifiProfileOwnPort_ + 1)
                                     .arg(WifiProfileDiff::baudText(uint32_t(wifiProfileNewBaud_))).arg(wifiProfileLoading_.address);
            wifiProfileResultBad_ = true;
            qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: \"%1\" taken, no answer here within 30 s since").arg(wifiProfileLoading_.name);
            notifyWarning(wifiProfileResult_);
            emit wifiProfileChanged();
            return;
        }
        idWifiProfile->ask(0);                    // the same address: the module answers once it is up
    });
    connect(wifiRadioCheck_, &WifiRadioCheck::finished, this, [this](int outcome, const QString& text) {
        qInfo().noquote() << wifiTag() << "wifi-radio:" << text;
        if (outcome == WifiRadioCheck::Corrected)
            notifyInfo(tr("Radio: %1").arg(text));
        else if (outcome == WifiRadioCheck::NoLink)
            notifyWarning(tr("Radio: %1").arg(text));
    });

    m_devSyncDebounceTimer.setSingleShot(true);
    m_devSyncDebounceTimer.setInterval(300);
    connect(&m_devSyncDebounceTimer, &QTimer::timeout, this, &DevDriver::onDevSyncDebounceFired);

    upgradeStartTimer_.setInterval(int(kUpgradeStartAskMs));
    connect(&upgradeStartTimer_, &QTimer::timeout, this, [this]() {
        if (upgradeStartAt_ == 0) {
            upgradeStartTimer_.stop();
            return;
        }
        if (QDateTime::currentMSecsSinceEpoch() - upgradeStartAt_ >= kUpgradeStartTimeoutMs) {
            finishUpgradeStart(noAnswerUpgrade, QStringLiteral("failed: the board has not answered for %1 s since it was "
                                                               "told to start; whether its firmware runs is not known")
                                                    .arg(kUpgradeStartTimeoutMs / 1000));
            return;
        }
        idVersion->simpleRequest(v2);   // its boot mode
    });

    portsAnswerTimer_.setSingleShot(true);
    portsAnswerTimer_.setInterval(2000);
    connect(&portsAnswerTimer_, &QTimer::timeout, this, [this]() {
        if (portsSupport_ == 0 && idBus->buses().isEmpty()) {
            portsSupport_ = -1;   // a firmware without the port table, asked without the response flag, is silent
            qInfo().noquote() << wifiTag() << "ports: no answer to ID_BUS 0, no port table";
        } else {
            qInfo().noquote() << wifiTag() << "ports: the table's read stopped, no answer";
        }
        emit portsChanged();
    });
    portOpTimer_.setSingleShot(true);
    portOpTimer_.setInterval(2000);
    connect(&portOpTimer_, &QTimer::timeout, this, [this]() {
        qInfo().noquote() << wifiTag() << QStringLiteral("ports: no answer (step %1, function %2)").arg(portOp_).arg(portOpFunction_);
        if (portOp_ == PortOpReadFunctions) {
            portSupport_[portOpFunction_] = -1;
            readNextFunction();
        } else if (portOp_ != PortOpNone) {
            portDone(portOp_ == PortOpSave ? PortNotSaved : PortNoAnswer);
        }
    });
    portsLiveTimer_.setInterval(200);
    connect(&portsLiveTimer_, &QTimer::timeout, this, [this]() {
        if (portsSupport_ == 1 && !portsAnswerTimer_.isActive()) {
            idPin->requestValues();
            // a rate on auto moves while its port searches: the connectors' v0 (the rate in use), one a second
            const QVector<int> uarts = uartBuses();
            if (portSettable() == 1 && portOp_ == PortOpNone && !uarts.isEmpty() && ++portLiveTick_ % 5 == 0) {
                idBus->request(U1(uarts.at((portLiveTick_ / 5) % uarts.size())));
            }
        }
    });

    ringStatTimer_.setSingleShot(true);
    ringStatTimer_.setInterval(2000);
    connect(&ringStatTimer_, &QTimer::timeout, this, [this]() {
        ringStatError_ = tr("No answer from the board");
        qInfo().noquote() << wifiTag() << "ring-stat: no answer";
        emit ringStatChanged();
    });

    servoManualTimer_.setSingleShot(true);
    servoManualTimer_.setInterval(kServoManualMs);
    connect(&servoManualTimer_, &QTimer::timeout, this, &DevDriver::flushServoManual);
    servoReadbackTimer_.setSingleShot(true);
    servoReadbackTimer_.setInterval(250);
    connect(&servoReadbackTimer_, &QTimer::timeout, this, [this]() {
        if (idServoControl) {
            idServoControl->requestAll();
        }
    });
}

DevDriver::~DevDriver()
{
    m_processTimer.stop();
}

ChannelId DevDriver::getChannelId() const
{
    return ChannelId(linkUuid_, lastAddress_);
}

void DevDriver::regID(IDBin* id_bin, ParseCallback method, bool is_setup) {
//    hashIDParsing[id_bin->id()] = id_bin;
//    hashIDCallback[id_bin->id()] = method;

    _hashID[id_bin->id()] = ID_Instance(id_bin, method, is_setup);


//    if(is_setup) {
//        hashIDSetup[id_bin->id()] = id_bin;
//    }

    connect(id_bin, &IDBin::binFrameOut, this, &DevDriver::onIdFrameOut);
}

// Field switches for a sonar behind a recorder (docs/RING_UPDATE.md), off unless set.
// KOGGER_NODE_WRAP=0 / =1: its commands go plain / wrapped in ID_NODE_CMD, and NodeProbe does not run.
// KOGGER_NODE_ADDR=<0..15>: the wrapper names this address instead of the route.
namespace {
int nodeWrapOverride()
{
    static const int mode = [] {
        const QString v = qEnvironmentVariable("KOGGER_NODE_WRAP");
        if (v == QLatin1String("0") || v == QLatin1String("1")) {
            qInfo("node-cmd: KOGGER_NODE_WRAP=%s, commands to a sonar behind a recorder go %s, no probe",
                  qPrintable(v), v == QLatin1String("0") ? "plain" : "wrapped");
            return v.toInt();
        }
        return -1;
    }();
    return mode;
}

int nodeAddressOverride()
{
    static const int addr = [] {
        bool ok = false;
        const int v = qEnvironmentVariable("KOGGER_NODE_ADDR").toInt(&ok);
        if (ok && v >= 0 && v <= 15) {
            qInfo("node-cmd: KOGGER_NODE_ADDR=%d, the wrapper names this address", v);
            return v;
        }
        return -1;
    }();
    return addr;
}
} // namespace

uint8_t DevDriver::nodeAddress() const
{
    return nodeAddressOverride() >= 0 ? uint8_t(nodeAddressOverride()) : lastAddress_;
}

bool DevDriver::commandsGoWrapped() const
{
    if (recorderRoute0_)
        return false;   // a bootloader on route 0 knows no ID_NODE_CMD
    if (nodeWrapOverride() >= 0)
        return nodeWrapOverride() == 1;
    return nodeMode_ != NodeProbe::Mode::Plain;   // wrapped until a probe finds otherwise, as before it
}

void DevDriver::onIdFrameOut(Parsers::ProtoBinOut out) {
    if (!isRingBehindRecorder() || !commandsGoWrapped() || !canWrapNodeCmd(out)) {
        emit binFrameOut(out);
        return;
    }
    emit binFrameOut(wrapNodeCmd(out, nodeAddress()));
}

// Runs NodeProbe once for a ring sonar behind a recorder. While it runs the setup is held back (process());
// its end asks for the setup again, in the mode it found.
void DevDriver::startNodeProbeIfDue()
{
    if (nodeProbeDone_ || nodeWrapOverride() >= 0 || !isRingBehindRecorder()
        || m_state.in_boot || m_state.in_update)
        return;
    if (!nodeProbe_) {
        NodeProbe::Io io;
        io.send = [this](Parsers::ProtoBinOut f) { emit binFrameOut(f); };
        io.nowMs = [] { return QDateTime::currentMSecsSinceEpoch(); };
        nodeProbe_ = new NodeProbe(io, this);
        connect(nodeProbe_, &NodeProbe::logLine, this, [this](const QString& line) {
            qInfo().noquote() << QStringLiteral("[%1] %2").arg(m_busAddress).arg(line);
        });
        connect(nodeProbe_, &NodeProbe::finished, this, [this](NodeProbe::Mode mode, int, const QString&) {
            nodeProbeDone_ = true;
            nodeMode_ = mode;
            emit deviceVersionChanged();   // isRecorderUpgradeSupported follows the mode
            requestSetup();
        });
    }
    if (!nodeProbe_->active())
        nodeProbe_->start(lastAddress_, nodeAddress());
}

bool DevDriver::nodeProbeRunning() const
{
    return nodeProbe_ && nodeProbe_->active();
}


void DevDriver::requestSetup() {
    QHashIterator<ID, ID_Instance> i(_hashID);
    while (i.hasNext()) {
        i.next();
        if(i.value().isSetup) {
            i.value().instance->startColdStartTimer();
            i.value().instance->requestAll();
        }
    }

    m_state.conf = ConfRequest;
}



void DevDriver::setConsoleOut(bool is_console) {
    QHashIterator<ID, ID_Instance> i(_hashID);
    while (i.hasNext()) {
        i.next();
        i.value().instance->setConsoleOut(is_console);
    }
    m_isConsole = is_console;
}

void DevDriver::setBusAddress(int addr) {
    m_busAddress = addr;
    QHashIterator<ID, ID_Instance> i(_hashID);
    while (i.hasNext()) {
        i.next();
        i.value().instance->setAddress(m_busAddress);
    }
}

int DevDriver::getBusAddress() {
    return m_busAddress;
}

void DevDriver::setDevAddress(int addr) {
    idUART->setDevAddress(addr);
}

int DevDriver::getDevAddress() {
    return idUART->devAddress();
}

void DevDriver::setBaudrate(int baudrate) {
    if(!m_state.connect) return;
    bool is_changed = getBaudrate() != baudrate;
    idUART->setBaudrate(baudrate);
    if(is_changed) { emit UARTChanged(); }
}

int DevDriver::getBaudrate() {
    return idUART->getBaudrate();
}

void DevDriver::setDevDefAddress(int addr) {
     idUART->setDevDefAddress(addr);
}

int DevDriver::getDevDefAddress() {
    return idUART->devDefAddress();
}

void DevDriver::dvlChangeMode(bool ismode1, bool ismode2, bool ismode3, bool ismode4, float range_mode4) {
    idDVLMode->setModes(ismode1, ismode2, ismode3, ismode4, range_mode4);
}

void DevDriver::importSettingsFromXML(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    QXmlStreamReader xmlReader(&file);
    while (!xmlReader.atEnd() && !xmlReader.hasError()) {
        const QXmlStreamReader::TokenType token = xmlReader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString elementName = xmlReader.name().toString();
            if (elementName == "Settings")
                continue;

            while (xmlReader.tokenType() != QXmlStreamReader::EndElement || xmlReader.name() != elementName) {
                xmlReader.readNext();
                if (xmlReader.isStartElement()) {
                    if (elementName == "Echogram") {
                        if (xmlReader.name().toString() == "resolution_mm")
                            setChartResolution(xmlReader.readElementText().toInt());
                        else if (xmlReader.name().toString() == "number_of_samples")
                            setChartSamples(xmlReader.readElementText().toInt());
                        else if (xmlReader.name().toString() == "offset_of_samples")
                            setChartOffset(xmlReader.readElementText().toInt());
                    }
                    else if (elementName == "Rangefinder") {
                        if (xmlReader.name().toString() == "max_distance_mm")
                            setDistMax(xmlReader.readElementText().toInt());
                        else if (xmlReader.name().toString() == "dead_zone_mm")
                            setDistDeadZone(xmlReader.readElementText().toInt());
                        else if (xmlReader.name().toString() == "confidence_threshold_perc")
                            setConfidence(xmlReader.readElementText().toInt());
                    }
                    else if (elementName == "Transducer") {
                        if (xmlReader.name().toString() == "pulse_count")
                            setTransPulse(xmlReader.readElementText().toInt());
                        else if (xmlReader.name().toString() == "frequency_khz")
                            setTransFreq(xmlReader.readElementText().toInt());
                        else if (xmlReader.name().toString() == "booster") {
                            const QString state = xmlReader.readElementText().trimmed();
                            setTransBoost(state.toUpper() == "TRUE" ? 1 : 0);
                        }
                    }
                    else if (elementName == "DSP") {
                        if (xmlReader.name().toString() == "horizontal_smoothing_factor")
                            setDspSmoothFactor(xmlReader.readElementText().toInt());
                        else if (xmlReader.name().toString() == "speed_of_sound_m_s")
                            setSoundSpeed(xmlReader.readElementText().toInt());
                    }
                    else if (elementName == "Dataset") {
                        if (xmlReader.name().toString() == "period_ms")
                            setCh1Period(xmlReader.readElementText().toInt());
                        else if (xmlReader.name().toString() == "echogram")
                            xmlReader.readElementText().trimmed().toUpper() == "TRUE" ? setDatasetChart(1) : setDatasetChart(0);
                        else if (xmlReader.name().toString() == "rangefinder") {
                            int state = xmlReader.readElementText().toInt();
                            if (state == 0) { setDatasetDist(0); setDatasetSDDBT(0); }
                            if (state == 1) { setDatasetDist(1); }
                            if (state == 2) { setDatasetSDDBT(1); }
                        }
                        else if (xmlReader.name().toString() == "AHRS")
                            xmlReader.readElementText().trimmed().toUpper() == "TRUE" ? setDatasetEuler(1) : setDatasetEuler(0);
                        else if (xmlReader.name().toString() == "temperature")
                            xmlReader.readElementText().trimmed().toUpper() == "TRUE" ? setDatasetTemp(1) : setDatasetTemp(0);
                        else if (xmlReader.name().toString() == "timestamp")
                            xmlReader.readElementText().trimmed().toUpper() == "TRUE" ? setDatasetTimestamp(1) : setDatasetTimestamp(0);
                        else if (xmlReader.name().toString() == "supply_voltage")
                            xmlReader.readElementText().trimmed().toUpper() == "TRUE" ? setDatasetVoltage(1) : setDatasetVoltage(0);
                    }
                    else if (elementName == "UART") {
                        if (xmlReader.name().toString() == "dev_def_address")
                            setDevDefAddress(xmlReader.readElementText().toInt());
                        else if (xmlReader.name().toString() == "baudrate")
                            setBaudrate(xmlReader.readElementText().toInt());
                        else if (xmlReader.name().toString() == "dev_address")
                            setDevAddress(xmlReader.readElementText().toInt());
                    }
                }
            }
        }
    }

    if (xmlReader.hasError())
        qDebug() << "XML error:" << xmlReader.errorString().toUtf8();

    file.close();
}

void DevDriver::exportSettingsToXML(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;

    QXmlStreamWriter xmlWriter(&file);
    xmlWriter.setAutoFormatting(true);
    xmlWriter.writeStartDocument();
    xmlWriter.writeStartElement("Settings");

    xmlWriter.writeStartElement("Echogram");
    xmlWriter.writeTextElement("resolution_mm", QString::number(chartResolution()));
    xmlWriter.writeTextElement("number_of_samples", QString::number(chartSamples()));
    xmlWriter.writeTextElement("offset_of_samples", QString::number(chartOffset()));
    xmlWriter.writeEndElement();
    xmlWriter.writeStartElement("Rangefinder");
    xmlWriter.writeTextElement("max_distance_mm", QString::number(distMax()));
    xmlWriter.writeTextElement("dead_zone_mm", QString::number(distDeadZone()));
    xmlWriter.writeTextElement("confidence_threshold_perc", QString::number(distConfidence()));
    xmlWriter.writeEndElement();
    xmlWriter.writeStartElement("Transducer");
    xmlWriter.writeTextElement("pulse_count", QString::number(transPulse()));
    xmlWriter.writeTextElement("frequency_khz", QString::number(transFreq()));
    xmlWriter.writeTextElement("booster", QVariant(static_cast<bool>(transBoost())).toString());
    xmlWriter.writeEndElement();
    xmlWriter.writeStartElement("DSP");
    xmlWriter.writeTextElement("horizontal_smoothing_factor", QString::number(dspSmoothFactor()));
    xmlWriter.writeTextElement("speed_of_sound_m_s", QString::number(soundSpeed()));
    xmlWriter.writeEndElement();
    xmlWriter.writeStartElement("Dataset");
    const U1 channelId = 1;
    const auto channel = idDataset->getChannel(channelId);
    xmlWriter.writeTextElement("period_ms", QString::number(channel.period));
    xmlWriter.writeTextElement("echogram", QVariant(idDataset->getChart_v0(channelId)).toString());
    {
        int state = 0;
        if (idDataset->getDist_v0(channelId) == 1) state = 1;
        if (idDataset->getSDDBT(channelId) == 1) state = 2;
        xmlWriter.writeTextElement("rangefinder", QString::number(state));
    }
    xmlWriter.writeTextElement("AHRS", QVariant(static_cast<bool>(idDataset->getEuler(channelId))).toString());
    xmlWriter.writeTextElement("temperature", QVariant(idDataset->getTemp_v0(channelId)).toString());
    xmlWriter.writeTextElement("timestamp", QVariant(idDataset->getTimestamp_v0(channelId)).toString());
    xmlWriter.writeTextElement("supply_voltage", QVariant(idDataset->getVoltage_v0(channelId)).toString());
    xmlWriter.writeEndElement();
    xmlWriter.writeStartElement("UART");
    xmlWriter.writeTextElement("dev_def_address", QString::number(getDevDefAddress()));
    xmlWriter.writeTextElement("baudrate", QString::number(getBaudrate()));
    xmlWriter.writeTextElement("dev_address", QString::number(getDevAddress()));
    xmlWriter.writeEndElement();

    xmlWriter.writeEndElement();
    xmlWriter.writeEndDocument();
    file.close();

}

void DevDriver::setDatasetState(bool state) {
    if (state != datasetState_) {
        datasetState_ = state;
        emit datasetChanged();
    }
}

void DevDriver::setDistSetupState(bool state) {
    if (state != distSetupState_) {
        distSetupState_ = state;
        emit distSetupChanged();
    }
}

void DevDriver::setChartSetupState(bool state) {
    if (state != chartSetupState_) {
        chartSetupState_ = state;
        emit sendChartSetup(getChannelId(), idChartSetup->resolution(), idChartSetup->count(), idChartSetup->offset());
        emit chartSetupChanged();
    }
}

void DevDriver::setDspSetupState(bool state) {
    if (state != dspSetupState_) {
        dspSetupState_ = state;
        emit dspSetupChanged();
    }
}

void DevDriver::setTranscState(bool state) {
    if (state != transcState_) {
        transcState_ = state;
        emit sendTranscSetup(getChannelId(), idTransc->freq(), idTransc->pulse(), idTransc->boost());
        emit transChanged();
    }
}

void DevDriver::setSoundSpeedState(bool state) {
    if (state != soundSpeedState_) {
        soundSpeedState_ = state;
        emit sendSoundSpeed(getChannelId(), idSoundSpeed->getSoundSpeed());
        emit soundChanged();
    }
}

void DevDriver::setUartState(bool state) {
    if (state != uartState_) {
        uartState_ = state;
        emit UARTChanged();
    }
}

void DevDriver::standStart(const QVariantMap& config) {
    if (!idStandScan) return;

    IDBinStandScan::Scan scan;
    scan.order = (config.value("order").toString() == QStringLiteral("elAz"))
                 ? IDBinStandScan::ElevationToAzimuth
                 : IDBinStandScan::AzimuthToElevation;

    U1 options = 0;
    if (config.value("reverse").toBool())    options |= IDBinStandScan::OptReverseInner;
    if (config.value("continuous").toBool()) options |= IDBinStandScan::OptContinuousInner;
    scan.options = options;

    scan.fires      = static_cast<U2>(config.value("fires", 1).toUInt());
    scan.cycles     = static_cast<U2>(qMax(1u, config.value("cycles", 1).toUInt()));
    scan.settleMs   = static_cast<U2>(config.value("settleMs", 0).toUInt());
    scan.postFireMs = static_cast<U2>(config.value("postFireMs", 0).toUInt());

    scan.innerStart = config.value("innerStart", 0).toInt();
    scan.innerEnd   = config.value("innerEnd", 0).toInt();
    scan.innerStep  = config.value("innerStep", 0).toInt();
    scan.outerStart = config.value("outerStart", 0).toInt();
    scan.outerEnd   = config.value("outerEnd", 0).toInt();
    scan.outerStep  = config.value("outerStep", 0).toInt();

    // The device rejects these, and a rejection reaches the operator as nothing at all — the
    // stand reports no state. Refusing to send is the honest failure.
    if (scan.innerStep == 0 || scan.outerStep == 0) return;
    if ((options & IDBinStandScan::OptContinuousInner) && scan.fires > 1) return;

    idStandScan->start(scan);
}

void DevDriver::standStop()   { if (idStandScan) idStandScan->stop(); }
void DevDriver::standPause()  { if (idStandScan) idStandScan->pause(); }
void DevDriver::standResume() { if (idStandScan) idStandScan->resume(); }
void DevDriver::standHome()   { if (idStandScan) idStandScan->home(); }

void DevDriver::setServoControlState(bool state) {
    if (state != servoControlState_) {
        servoControlState_ = state;
        emit servoControlChanged();
    }
}

void DevDriver::setPwmRouteState(bool state) {
    if (state != pwmRouteState_) {
        pwmRouteState_ = state;
        emit pwmRouteChanged();
    }
}

void DevDriver::setDevSyncState(bool state) {
    if (state != devSyncState_) {
        devSyncState_ = state;
        emit devSyncChanged();
    }
}

void DevDriver::setDevSyncPeriodMs(int ms) {
    if (!idDevSync) return;
    idDevSync->setPeriodMs(static_cast<U2>(ms));
    emit devSyncChanged();
    m_devSyncDebounceTimer.start();
}

void DevDriver::setDevSyncPortSource(int idx, int src) {
    if (!idDevSync) return;
    idDevSync->setPortSource(idx, static_cast<U1>(src));
    emit devSyncChanged();
    m_devSyncDebounceTimer.start();
}

void DevDriver::onDevSyncDebounceFired() {
    if (idDevSync) idDevSync->flushPending();
}

void DevDriver::setLinkUuid(QUuid linkUuid)
{
    linkUuid_ = linkUuid;
}

QUuid DevDriver::getLinkUuid() const
{
    return linkUuid_;
}

void DevDriver::setLinkStatus(bool connected, bool receivesData, bool notAvailable)
{
    if (linkConnected_    == connected    &&
        linkReceivesData_ == receivesData &&
        linkNotAvailable_ == notAvailable)
        return;

    linkConnected_    = connected;
    linkReceivesData_ = receivesData;
    linkNotAvailable_ = notAvailable;
    emit linkStatusChanged();
}

void DevDriver::askBeaconPosition(IDBinUsblSolution::USBLRequestBeacon ask)
{
    if (!m_state.connect) {
        return;
    }

    idUSBL->askBeacon(ask);
}

void DevDriver::enableBeaconOnce(float timeout)
{
    if (!m_state.connect) {
        return;
    }

    idUSBL->enableBeaconOnce(timeout);
}

void DevDriver::acousticPingRequest(uint8_t address, uint32_t timeout_us) {
    if(!m_state.connect) return;
    trackUsblFrame(v1, idUSBLControl->pingRequest(timeout_us, address));
}

void DevDriver::acousticPingRequestEx(uint8_t address, uint32_t timeout_us, uint8_t cmdId, uint32_t replyDistanceMm, const QByteArray& payload) {
    if(!m_state.connect) return;
    trackUsblFrame(v1, idUSBLControl->pingRequest(timeout_us, address, cmdId, replyDistanceMm, payload));
}

void DevDriver::acousticPingRequestHex(int address, double timeoutUs, int cmdId, double replyDistanceMm,
                                       const QString& hexPayload, int payloadBits) {
    if(!m_state.connect) return;
    const uint32_t timeout = (uint32_t)qBound(0.0, timeoutUs, 4294967295.0);
    const uint32_t reply = (uint32_t)qBound(0.0, replyDistanceMm, 4294967295.0);
    trackUsblFrame(v1, idUSBLControl->pingRequest(timeout, (uint8_t)qBound(0, address, 255),
                                                  (uint8_t)qBound(0, cmdId, 255), reply,
                                                  parseHexPayload(hexPayload), payloadBits));
}

void DevDriver::trackUsblFrame(int ver, uint16_t checksum) {
    UsblPendingFrame f;
    f.ver = ver;
    f.sentMs = QDateTime::currentMSecsSinceEpoch();
    f.relayed = (int)idUSBLControl->lastRelayedChecksum();
    usblPending_[(int)checksum] = f;
    logUsblControl(ver, QStringLiteral("sent ck %1").arg(checksum, 4, 16, QLatin1Char('0')));
    emit usblControlFrameSent(ver, (int)checksum);
}

void DevDriver::logUsblControl(int ver, const QString& what) {
    // Pings go out every step; only configuration frames are rare enough to log unconditionally.
    if(ver == (int)v1 && !usblConsoled_) return;
    qInfo().noquote() << QStringLiteral("USBL ctl v%1 %2 [%3 SN %4 route %5]")
                             .arg(ver).arg(what, m_devName)
                             .arg(devSerialNumber())
                             .arg(getBusAddress());
}

void DevDriver::acousticResponceFilter(uint8_t address) {
    std::array<uint8_t, 8> filter;
    filter.fill(0xFF);
    filter[0] = address;
    usblMem_.hasFilter = true;
    usblMem_.filter = filter;
    sendUsblFilter();
}

void DevDriver::acousticResponceFilterSlots(const QVector<int>& addresses) {
    std::array<uint8_t, 8> filter;
    filter.fill(0xFF);
    const int count = qMin((int)filter.size(), (int)addresses.size());
    for(int i = 0; i < count; ++i) {
        filter[i] = (uint8_t)addresses[i];
    }
    usblMem_.hasFilter = true;
    usblMem_.filter = filter;
    sendUsblFilter();
}

void DevDriver::acousticResponceTimeout(uint32_t timeout_us) {
    usblMem_.hasTransponder = true;
    usblMem_.transponderTimeoutUs = timeout_us;
    sendUsblTransponder();
}

void DevDriver::setUsblTransponderEnable(bool enabled) {
    acousticResponceTimeout(enabled ? 0xFFFFFFFF : 0);
}

void DevDriver::setUsblMonitorConfig(uint32_t suppressSelfResponseUs, uint32_t suppressSelfRequestUs, bool receiveResponseInIdle) {
    usblMem_.hasMonitor = true;
    usblMem_.suppressSelfResponseUs = suppressSelfResponseUs;
    usblMem_.suppressSelfRequestUs = suppressSelfRequestUs;
    usblMem_.receiveResponseInIdle = receiveResponseInIdle;
    sendUsblMonitor();
}

QVariantList DevDriver::usblRespAcceptedAddresses() const {
    QVariantList out;
    if(!usblMem_.hasFilter) return out;
    for(uint8_t a : usblMem_.filter) {
        if(a != 0xFF) out.append((int)a);
    }
    return out;
}

int DevDriver::usblRespApplyState() const {
    bool any = false, pending = false, queued = false, noAnswer = false, confirmed = false;
    for(int ver : { (int)v3, (int)v4, (int)v7 }) {
        switch(usblAck_[ver]) {
        case UsblApplyRejected:  return UsblApplyRejected;
        case UsblApplyPending:   pending = true; any = true; break;
        case UsblApplyQueued:    queued = true; any = true; break;
        case UsblApplyNoAnswer:  noAnswer = true; any = true; break;
        case UsblApplyConfirmed: confirmed = true; any = true; break;
        default: break;
        }
    }
    if(!any) return UsblApplyNone;
    if(pending) return UsblApplyPending;
    if(queued) return UsblApplyQueued;
    if(noAnswer) return UsblApplyNoAnswer;
    return confirmed ? UsblApplyConfirmed : UsblApplyNone;
}

void DevDriver::markUsblSent(int ver) {
    if(ver < 0 || ver >= (int)usblAck_.size()) return;
    usblAck_[ver] = UsblApplyPending;
    usblAckSentMs_[ver] = QDateTime::currentMSecsSinceEpoch();
}

void DevDriver::sendUsblTransponder() {
    if(m_state.connect) {
        trackUsblFrame(v3, idUSBLControl->setTransponderEnable(usblMem_.transponderTimeoutUs));
        markUsblSent(v3);
    } else {
        usblAck_[v3] = UsblApplyQueued;
    }
    emit usblResponseConfigChanged();
}

void DevDriver::sendUsblFilter() {
    if(m_state.connect) {
        trackUsblFrame(v4, idUSBLControl->setResponseAddressFilter(usblMem_.filter));
        markUsblSent(v4);
    } else {
        usblAck_[v4] = UsblApplyQueued;
    }
    emit usblResponseConfigChanged();
}

void DevDriver::sendUsblMonitor() {
    if(m_state.connect) {
        IDBinUsblControl::USBLMonitorConfig cfg;
        cfg.suppressSelfResponse_us = usblMem_.suppressSelfResponseUs;
        cfg.suppressSelfRequest_us = usblMem_.suppressSelfRequestUs;
        cfg.receiveResponseInIdle = usblMem_.receiveResponseInIdle;
        trackUsblFrame(v7, idUSBLControl->setMonitorConfig(cfg));
        markUsblSent(v7);
    } else {
        usblAck_[v7] = UsblApplyQueued;
    }
    emit usblResponseConfigChanged();
}

void DevDriver::resendUsblControl() {
    const bool any = usblMem_.hasTransponder || usblMem_.hasFilter || usblMem_.hasMonitor
                     || !usblMem_.cmdSlots.isEmpty();
    if(!any) return;

    for(auto it = usblMem_.cmdSlots.cbegin(); it != usblMem_.cmdSlots.cend(); ++it) {
        sendUsblSlot(it.value());
    }
    if(usblMem_.hasMonitor) sendUsblMonitor();
    if(usblMem_.hasFilter) sendUsblFilter();
    if(usblMem_.hasTransponder) sendUsblTransponder();

    core.consoleInfo(QString("USBL: configuration re-sent to %1 after (re)connect").arg(m_devName));
}

QByteArray DevDriver::parseHexPayload(const QString& text) {
    QString digits;
    for(const QChar& c : text) {
        if(c.isDigit() || (c.toLower() >= 'a' && c.toLower() <= 'f'))
            digits.append(c);
    }
    // fromHex() silently drops a trailing lone nibble; pad so a half-typed byte still lands.
    if(digits.size() % 2)
        digits.prepend('0');
    return QByteArray::fromHex(digits.toLatin1());
}

void DevDriver::setUsblCmdConfig(int cmdId, int event,
                                 int receiverFunction, int receiveBitLength,
                                 int senderFunction, const QString& sendHexPayload,
                                 int eventAction,
                                 int cmdIdAction, int cmdIdReplacement,
                                 int addressAction, int addressReplacement,
                                 int sendBitLength) {
    UsblSlotArgs a;
    a.cmdId = cmdId;
    a.event = event;
    a.receiverFunction = receiverFunction;
    a.receiveBitLength = receiveBitLength;
    a.senderFunction = senderFunction;
    a.sendHexPayload = sendHexPayload;
    a.eventAction = eventAction;
    a.cmdIdAction = cmdIdAction;
    a.cmdIdReplacement = cmdIdReplacement;
    a.addressAction = addressAction;
    a.addressReplacement = addressReplacement;
    a.sendBitLength = sendBitLength;

    typedef IDBinUsblControl::USBLCmdConfig Cfg;
    const int eventKey = event == Cfg::EventOnReceiveResponse ? 1 : 0;
    usblMem_.cmdSlots[eventKey * 256 + qBound(0, cmdId, 255)] = a;

    if(m_state.connect) sendUsblSlot(a);
}

void DevDriver::sendUsblSlot(const UsblSlotArgs& a) {
    typedef IDBinUsblControl::USBLCmdConfig Cfg;

    // Every enum here is narrow, and the values arrive from QML as plain ints — clamp
    // rather than cast, so a bad value cannot put an undefined byte on the wire.
    auto fn = [](int v) {
        switch(v) {
        case Cfg::FunctionBitArray:     return Cfg::FunctionBitArray;
        case Cfg::FunctionLLGeoAzimuth: return Cfg::FunctionLLGeoAzimuth;
        default:                        return Cfg::FunctionDefault;
        }
    };

    Cfg cfg;
    cfg.cmd_id = (uint8_t)qBound(0, a.cmdId, 255);
    cfg.eventFilter = a.event == Cfg::EventOnReceiveResponse ? Cfg::EventOnReceiveResponse
                                                             : Cfg::EventOnReceiveRequest;
    cfg.cmdIdAction = a.cmdIdAction == Cfg::SendBackCmdIdReplacement ? Cfg::SendBackCmdIdReplacement
                                                                    : Cfg::SendBackCmdIdIncoming;
    cfg.cmd_id_replacement = (uint8_t)qBound(0, a.cmdIdReplacement, 255);
    cfg.addressAction = a.addressAction == Cfg::SendBackAddressReplacement ? Cfg::SendBackAddressReplacement
                                                                          : Cfg::SendBackAddressIncoming;
    cfg.address_replacement = (uint8_t)qBound(0, a.addressReplacement, 255);
    cfg.eventAction = a.eventAction == Cfg::SendBackEventSame ? Cfg::SendBackEventSame
                                                             : Cfg::SendBackEventSwaping;
    cfg.receiver_function = fn(a.receiverFunction);
    cfg.receive_bit_length = (uint16_t)qBound(0, a.receiveBitLength, 65535);
    cfg.sender_function = fn(a.senderFunction);
    // sending_bit_length is derived from the payload actually written, inside setCmdConfig.

    const QByteArray payload = parseHexPayload(a.sendHexPayload);
    trackUsblFrame(v6, idUSBLControl->setCmdConfig(cfg, payload, a.sendBitLength));
}

QString DevDriver::modemLastPayload() const {
    return QString::fromLatin1(idModemSolution->payload().toHex(' '));
}

void DevDriver::doRequestAll()
{
    if (idVersion) {
        //qDebug() << "  dev_driver: requesting all for" << m_devName << m_devAddress;
        idVersion->requestAll();
    }
}

uint32_t DevDriver::devSerialNumber() {
    return idVersion->serialNumber();
}

QString DevDriver::devPN() {
    return QString();
}

void DevDriver::setFirmware(const QByteArray &data)
{
    sendUpdateFW(data);
}

void DevDriver::protoComplete(Parsers::FrameParser& proto)
{
    if (!proto.isComplete()) {
        return;
    }

    int64_t curr_time = QDateTime::currentMSecsSinceEpoch();

    if (proto.completeAsKBP2() && !viaRecorder_) {
        viaRecorder_ = true;   // a recorder re-wraps its port devices' frames (their route: docs/RING_UPDATE.md)
        emit deviceVersionChanged();
    }
    m_state.mark = proto.mark();
    unanswered_.heard(curr_time);

    if(m_state.mark) {
        m_state.lastConnectTime = curr_time;
        if (dataStale_)
            updateDataStale(curr_time);
    }

    // if (rebootFlag_ && !proto.mark()) {
    //     rebootFlag_ = false;
    // }

    // if (rebootFlag_) {
    //      m_state.mark = false;
    // }

    // if(proto.id() == Parsers::ID_BOOT && proto.ver() == v0 && proto.resp() == true) {
    //     qDebug() << "   !!! and seting mark FALSE";
    //     m_state.mark = false;
    // }



    if(_hashID.contains(proto.id())) {
        if(_hashID[proto.id()].instance != nullptr) {
            IDBin* parse_instance = _hashID[proto.id()].instance;
            parse_instance->parse(proto);
            lastAddress_ = proto.route();
            if(parse_instance == idUSBLControl) {
                usblAckEcho_ = (proto.resp() && proto.readAvailable() >= 2) ? (int)proto.read<uint16_t>() : -1;
            }

            if(_hashID[proto.id()].callback != nullptr) {
                ParseCallback& callback = _hashID[proto.id()].callback;
                (this->*callback)(parse_instance->lastType(), parse_instance->lastVersion(), parse_instance->lastResp());
            }
        }
    }

    if (nodeProbeRunning()) {
        const bool uartAddress = proto.id() == ID_UART && proto.ver() == v1 && proto.type() == CONTENT;
        nodeProbe_->onFrame(uint8_t(proto.id()), uint8_t(proto.ver()), uint8_t(proto.type()),
                            uartAddress ? int(idUART->devAddress()) : -1);
    }
    // Any frame of this sonar once the settle time is over: the recorder hears it at the new rate.
    onRecorderUpgradeHeard();
}

void DevDriver::startConnection(bool duplex) {
    m_state.duplex = duplex;
    restartState();
    m_devName = "...";
    m_bootloaderLagacyMode = true;
    m_upgrade_status = 0;

    emit deviceVersionChanged();
}

void DevDriver::stopConnection() {
    m_state.connect = false;
    updateDataStale(QDateTime::currentMSecsSinceEpoch());
    m_processTimer.stop();
    m_devName = "...";
    standProbeSent_ = false;
    portsProbeSent_ = false;
    portsLiveTimer_.stop();
    wifiProbeSent_ = false;
    wifiNetAskedAt_ = 0;
    wifiPortsAskedAt_ = 0;
    wifiPortsAsks_ = 0;
    wifiFastReport_ = false;
    wifiLastState_ = -1;
    resetPresets();
    if (standSupported_) {
        standSupported_ = false;
        emit standChanged();
    }
    if (idDevSync) {
        idDevSync->reset();
        m_devSyncDebounceTimer.stop();
    }
    setDevSyncState(false);
    emit devSyncChanged();
    emit deviceVersionChanged();
}

void DevDriver::restartState() {
    m_processTimer.stop();
    nodeProbeDone_ = false;
    nodeMode_ = NodeProbe::Mode::Unknown;
    if(idNavSensorStatus && idNavSensorStatus->isValid()) {
        idNavSensorStatus->invalidate();
        navSensorStatusStale_ = false;
        emit navSensorStatusChanged();
    }
    //qDebug() << "restart";
    m_state.resetState();
    updateDataStale(QDateTime::currentMSecsSinceEpoch());
    idVersion->reset();
    m_processTimer.start(200);
}

void DevDriver::updateDataStale(int64_t now) {
    // Not during an update (the bootloader answers only the update) and not before the device answered at all.
    const bool stale = DeviceLiveness::stale(m_state.connect, m_state.mark, m_state.in_boot || m_state.in_update,
                                             m_state.lastConnectTime, now);
    if (stale == dataStale_)
        return;
    dataStale_ = stale;
    qInfo().noquote() << QStringLiteral("[%1] device %2: %3").arg(m_busAddress).arg(m_devName,
        stale ? QStringLiteral("nothing for %1 s, shown as without data").arg(double(now - m_state.lastConnectTime) / 1000.0, 0, 'f', 1)
              : QStringLiteral("frames again"));
    emit dataStaleChanged();
    if (isWifi())
        emit wifiReportChanged();   // its signal icon: no link without its reports
}

void DevDriver::requestDist() {
    if(!m_state.connect) return;
    idDist->simpleRequest(v0);
}

void DevDriver::requestChart() {
    if(!m_state.connect) return;
    idChart->simpleRequest(v0);
}

void DevDriver::requestStreamList() {
    ProtoBinOut id_out;
    id_out.create(GETTING, v0, ID_STREAM, getDevAddress());
    id_out.write<U2>(0);
    id_out.write<U2>(0xFFFF);
    id_out.write<U4>(0x0);
    id_out.end();
    emit binFrameOut(id_out);
}

void DevDriver::requestStream(int stream_id) {
    ProtoBinOut id_out;
    id_out.create(SETTING, v1, ID_STREAM, getDevAddress());
    id_out.write<U2>(stream_id);
    id_out.write<U2>(0); // FLAGS
    id_out.write<U4>(0x0);
    id_out.write<U4>(0xFFFFFFF);
    id_out.end();
    emit binFrameOut(id_out);
}

void DevDriver::requestStreamRange(int stream_id, quint32 start, quint32 end) {
    ProtoBinOut id_out;
    id_out.create(SETTING, v1, ID_STREAM, getDevAddress());
    id_out.write<U2>(stream_id);
    id_out.write<U2>(0); // FLAGS
    id_out.write<U4>(start);
    id_out.write<U4>(end);
    id_out.end();
    emit binFrameOut(id_out);
}

void DevDriver::requestStreamRanges(int stream_id, QVector<quint32> ranges) {
    if(ranges.isEmpty() || (ranges.size() % 2) != 0) { return; }
    ProtoBinOut id_out;
    id_out.create(SETTING, v1, ID_STREAM, getDevAddress());
    id_out.write<U2>(stream_id);
    id_out.write<U2>(0); // FLAGS
    for(int i = 0; i + 1 < ranges.size(); i += 2) {
        id_out.write<U4>(ranges[i]);
        id_out.write<U4>(ranges[i + 1]);
    }
    id_out.end();
    emit binFrameOut(id_out);
}

void DevDriver::requestRecorderStatus() {
    ProtoBinOut id_out;
    id_out.create(GETTING, v0, ID_RECORDER_STATUS, getDevAddress());
    id_out.end();
    emit binFrameOut(id_out);
}

void DevDriver::receivedRecorderStatus(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    if (resp == respNone && ver == v0 && idRecorderStatus->isValid()) {
//         const QString s = QString("Recorder status: cond=%1 mode=%2 state=%3 log=%4 rec64k=%5 free1m=%6 dur=%7 sinceWrite=%8 stat=0x%9 warn=0x%10 degr=0x%11 crit=0x%12")
//                           .arg(idRecorderStatus->deviceCondition())
//                           .arg(idRecorderStatus->recordingMode())
//                           .arg(idRecorderStatus->recordingState())
//                           .arg(idRecorderStatus->currentLogId())
//                           .arg(idRecorderStatus->recordedSize64k())
//                           .arg(idRecorderStatus->freeSpace1m())
//                           .arg(idRecorderStatus->recordingDurationSeconds())
//                           .arg(idRecorderStatus->secondsSinceLastWrite())
//                           .arg(idRecorderStatus->statusFlags(), 0, 16)
//                           .arg(idRecorderStatus->warningFlags(), 0, 16)
//                           .arg(idRecorderStatus->degradedFlags(), 0, 16)
//                           .arg(idRecorderStatus->criticalFlags(), 0, 16);
//         qInfo("%s", qUtf8Printable(s));
//         core.consoleInfo(s);
        emit recorderStatusChanged();
    }
}

namespace {

// Per-packet trace of a firmware update, off unless KOGGER_FW_TRACE is set.
// A firmware runs to thousands of packets, so this is not something to leave on
// in ordinary use - but it is the only way to describe the exchange from what
// actually happened rather than from what the code appears to do.
bool fwTraceEnabled()
{
    static const bool on = qEnvironmentVariableIsSet("KOGGER_FW_TRACE");
    return on;
}

qint64 fwTraceNow()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

void DevDriver::sendUpdateFWAll(QByteArray update_data) {
    if (isRingBoard())
        emit ringUpgradeRequested(linkUuid_, lastAddress_, update_data, upgradeExpect_, true);
}

void DevDriver::setRingUpgradeStatus(int status, bool done) {
    m_upgrade_status = status;
    emit upgradeProgressChanged(m_upgrade_status);
    emit upgradeChanged();
    if (done)
        emit upgradingFirmwareDone();
}

void DevDriver::assignBusAddress(int address) {
    if (busAddressBusy_ || !isBusAddressSettable())
        return;
    busAddressBusy_ = true;
    emit busAddressBusyChanged();
    emit busAddressRequested(linkUuid_, m_busAddress, address);
}

void DevDriver::setBusAddressResult(bool ok, int address, const QString& detail) {
    busAddressBusy_ = false;
    emit busAddressBusyChanged();
    if (ok)
        idUART->simpleRequest(v1);   // the info row shows what the board says, not what was sent
    emit busAddressDone(ok, address, detail);
}

void DevDriver::setRingLineRate(int rate) {
    if (ringRateBusy_ || !isRingBoard())
        return;
    ringRateBusy_ = true;
    emit ringRateBusyChanged();
    emit ringRateRequested(linkUuid_, m_busAddress, rate);
}

void DevDriver::setRingRateResult(bool ok, int rate, const QString& detail) {
    ringRateBusy_ = false;
    emit ringRateBusyChanged();
    if (ok)
        idUART->simpleRequest(v0);   // the device's own view of its rate follows
    emit ringRateDone(ok, rate, detail);
}

void DevDriver::receivedNavSensorStatus(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    if (resp == respNone && ver == v0 && idNavSensorStatus->isValid()) {
        lastNavSensorStatusMs_ = QDateTime::currentMSecsSinceEpoch();
        navSensorStatusStale_ = false;
        emit navSensorStatusChanged();
    }
}

void DevDriver::sendUpdateFW(QByteArray update_data) {
    if (isBehindRecorder()) {
        if (!isRecorderUpgradeSupported()) {
            recorderLog(QStringLiteral("refused: a ring sonar with a bus address behind a recorder, "
                                       "connect it to a computer directly (docs/RING_UPDATE.md)"));
            return;
        }
        beginRecorderUpgrade(update_data);
        return;
    }
    if (isRingBoard()) {
        emit ringUpgradeRequested(linkUuid_, lastAddress_, update_data, upgradeExpect_, false);
        return;
    }
    startStockUpgrade(update_data);
}

void DevDriver::startStockUpgrade(const QByteArray& update_data) {
    if (fwTraceEnabled()) {
        qInfo("FWTRACE %lld start size=%lld bytes packet=%d",
              fwTraceNow(), (long long)update_data.size(), 96);
    }
    upgradeStartTimer_.stop();   // a new update ends the wait for the last one's board
    upgradeStartAt_ = 0;
    reboot();
    if (recorderUpgrade_ != RecorderUpgrade::Idle) {
        // The bootloader takes route 0, not the port's (docs/RING_UPDATE.md). The recorder hands route 0 to its
        // sonar ports and stamps the bootloader's answers with the port, so they come to this device as they are.
        recorderUpgrade_ = RecorderUpgrade::Idle;
        recorderRoute0_ = true;
        setIdRoute(0);
    }
    restartState();

    _timeoutUpgradeAnswerTime = 5000;
    upgradeStartedTime_ = QDateTime::currentMSecsSinceEpoch();
    _lastUpgradeAnswerTime = upgradeStartedTime_;
    upgradeResendCount_ = 0;
    idUpdate->setUpdate(update_data);
    m_bootloaderLagacyMode = true;
    m_state.in_boot = true;

    emit startUpgradingFirmware();
    emit startUpgradingFirmwareDM(linkUuid_, lastAddress_, update_data);

    // QTimer::singleShot(500, idUpdate, SLOT(putUpdate()));
}

// Firmware update of a sonar behind a recorder (docs/RING_UPDATE.md). The bootloader starts at 115200 and the
// recorder follows the rate of its sonar line, so the sonar is moved to 115200 first, and the stock update starts
// once the sonar is heard through the recorder again; a silent sonar is not rebooted. The stock update itself sends
// no chunk before the bootloader answers, so a sonar that never moved fails the handshake, safely.
void DevDriver::recorderLog(const QString& line)
{
    const QString s = QStringLiteral("recorder-upgrade: ") + line;
    qInfo().noquote() << QStringLiteral("[%1] %2").arg(m_busAddress).arg(s);
    core.consoleInfo(QStringLiteral("Upgrade: ") + s);
}

void DevDriver::beginRecorderUpgrade(const QByteArray& image)
{
    if (recorderUpgrade_ != RecorderUpgrade::Idle || recorderRoute0_ || m_state.in_boot || m_state.in_update)
        return;
    const int64_t now = QDateTime::currentMSecsSinceEpoch();
    recorderUpgradeImage_ = image;
    recorderUpgrade_ = RecorderUpgrade::SlowLine;
    recorderUpgradeAsked_ = false;
    recorderUpgradeAskAt_ = now + kRecorderSettleMs;
    recorderUpgradeUntil_ = now + kRecorderFollowMs;
    emit linkRatePinRequested(true);
    recorderLog(QStringLiteral("the sonar is moved to %1, the recorder has to follow it").arg(kBootloaderRate));
    idUART->setBaudrate(U4(kBootloaderRate));
    m_upgrade_status = 0;
    emit upgradeProgressChanged(m_upgrade_status);
}

void DevDriver::stepRecorderUpgrade(int64_t now)
{
    if (recorderUpgrade_ != RecorderUpgrade::SlowLine)
        return;
    if (now >= recorderUpgradeUntil_) {
        failRecorderUpgrade(QStringLiteral("the sonar went silent after the move to %1: the recorder did not follow it. "
                                           "Nothing was rebooted; a power cycle brings back its saved rate").arg(kBootloaderRate));
        return;
    }
    if (now >= recorderUpgradeAskAt_) {
        // The version is asked because the sonar answers it at once; this firmware hardly answers GET ID_UART v0.
        recorderUpgradeAsked_ = true;
        recorderUpgradeAskAt_ = now + kRecorderAskMs;
        idVersion->simpleRequest(v0);
    }
}

void DevDriver::onRecorderUpgradeHeard()
{
    if (recorderUpgrade_ != RecorderUpgrade::SlowLine || !recorderUpgradeAsked_)
        return;
    recorderLog(QStringLiteral("the sonar is heard through the recorder after the move, the update starts; "
                               "nothing is written before its bootloader answers"));
    const QByteArray image = recorderUpgradeImage_;
    recorderUpgradeImage_.clear();
    startStockUpgrade(image);
}

void DevDriver::failRecorderUpgrade(const QString& reason)
{
    recorderUpgrade_ = RecorderUpgrade::Idle;
    recorderUpgradeImage_.clear();
    recorderLog(QStringLiteral("failed: ") + reason);
    emit linkRatePinRequested(false);
    m_upgrade_status = failUpgrade;
    emit upgradeProgressChanged(m_upgrade_status);
    emit upgradeChanged();
}

void DevDriver::setIdRoute(uint8_t route)
{
    QHashIterator<ID, ID_Instance> i(_hashID);
    while (i.hasNext()) {
        i.next();
        i.value().instance->setAddress(route);
    }
}

void DevDriver::endRecorderRoute()
{
    if (!recorderRoute0_)
        return;
    recorderRoute0_ = false;
    setIdRoute(uint8_t(m_busAddress));
    // The board starts its firmware at its saved rate and the recorder follows it; the host port stays put meanwhile.
    QTimer::singleShot(kRecorderUnpinMs, this, [this] { if (!recorderRoute0_) emit linkRatePinRequested(false); });
}

void DevDriver::sendFactoryFW(QByteArray update_data) {
    Q_UNUSED(update_data);
}

int DevDriver::transFreq() {
    return idTransc->freq();
}

void DevDriver::setTransFreq(int freq) {
    if(!m_state.connect) return;
    bool is_changed = transFreq() != freq;
    idTransc->setFreq((U2)freq);

    if (is_changed) {
        emit sendTranscSetup(getChannelId(), static_cast<uint16_t>(freq), idTransc->pulse(), idTransc->boost());
        emit transChanged();
    }
}

int DevDriver::transPulse() {
    return idTransc->pulse();
}

void DevDriver::setTransPulse(int pulse) {
    if(!m_state.connect) return;
    bool is_changed = transPulse() != pulse;
    idTransc->setPulse((U1)pulse);

    if (is_changed) {
        emit sendTranscSetup(getChannelId(), idTransc->freq(), static_cast<uint8_t>(pulse), idTransc->boost());
        emit transChanged();
    }
}

int DevDriver::transBoost() {
    return idTransc->boost();
}

void DevDriver::setTransBoost(int boost) {
    if(!m_state.connect) return;
    bool is_changed = transBoost() != boost;
    idTransc->setBoost((U1)boost);

    if (is_changed) {
        emit sendTranscSetup(getChannelId(), idTransc->freq(), idTransc->pulse(), static_cast<uint8_t>(boost));
        emit transChanged();
    }
}

void DevDriver::receivedNet(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);
    Q_UNUSED(resp);
    emit netChanged();
}

void DevDriver::receivedWifi(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);

    if (resp != respNone) {
        // Join and save (v3), saved networks (v4), the relay (v5), the radio (v7): settings a profile holds.
        if (resp == respOk && (ver == v3 || ver == v4 || ver == v5 || ver == v7))
            wifiProfileAfterSetting();
        // Confirmation of a SETTING, or the module refusing a request. A refused scan (the module
        // is busy joining) must not leave the island in its "scanning" state.
        if (ver == v2 && resp != respOk) {
            idWifi->cancelScan();
            emit wifiNetworksChanged();
        }
        // The saved list is never pushed, so read it back after an action on it.
        if (ver == v4 && resp == respOk) {
            idWifi->requestAll();
        }
        // Firmware before 0.6 knows no relay: remember that instead of nagging on every probe.
        if ((ver == v5 || ver == v6) && resp == respErrorVersion) {
            idWifi->markRelayUnsupported();
            emit wifiRelayChanged();
            return;
        }
        if (ver == v7 && resp == respErrorVersion) {    // radio control came with 0.8
            idWifi->markRadioUnsupported();
            emit wifiRadioChanged();
            return;
        }
        // A refused radio setting leaves nothing to check.
        if (ver == v7 && resp != respOk) {
            wifiRadioCheck_->cancel();
        }
        emit wifiCommandResult(static_cast<int>(ver), static_cast<int>(resp));
        return;
    }

    switch (ver) {
    case v0: {
        const auto& st = idWifi->status();
        const int state = st.state;
        if (wifiLinkLog_.onStatus(state, st.reason, st.disconnectCode, st.clients, st.ssid, st.channel, st.rssi,
                                  wifiClock_.elapsed())) {
            const WifiLinkLog::Event& ev = wifiLinkLog_.events().last();
            if (ev.kind == WifiLinkLog::KindClients)
                qInfo().noquote() << wifiTag() << QStringLiteral("wifi: access point clients %1 (was %2), weakest %3 dBm")
                                         .arg(ev.clients).arg(ev.prevClients).arg(ev.rssi);
            emit wifiLinkEventsChanged();
        }
        if (state != wifiLastState_) {
            // Every change of the station state goes to the log: link drops to the boat are read from it.
            static const char* const names[] = { "idle", "searching", "connecting", "connected", "access point" };
            static const char* const reasons[] = { "none", "not found", "wrong password", "association", "link lost", "other" };
            const auto name = [](int s) { return (s >= 0 && s <= 4) ? QString::fromLatin1(names[s]) : QString::number(s); };
            qInfo().noquote() << wifiTag() << QStringLiteral("wifi: %1 (was %2), ssid '%3' ch %4 rssi %5 dBm; last disconnect: %6, code %7")
                                     .arg(name(state), wifiLastState_ < 0 ? QStringLiteral("-") : name(wifiLastState_), st.ssid)
                                     .arg(st.channel).arg(st.rssi)
                                     .arg(st.reason <= 5 ? QString::fromLatin1(reasons[st.reason]) : QString::number(st.reason))
                                     .arg(st.disconnectCode);
            wifiLastState_ = state;
            // The module does not push its radio state after it reconnects: ask, so what the app shows
            // (the negotiated PHY, the width in use) is what the link runs now.
            if (idWifi->radioSupport() != 0) {
                idWifi->simpleRequest(v7);
            }
            // A network joined with "save" is stored only once an address arrived: now.
            if (state == IDBinWifi::StateConnected) {
                idWifi->simpleRequest(v4);
            }
        }
        wifiPort_->moduleAnswered();
        wifiRfOnStatus((st.flags & IDBinWifi::FlagRadioOff) != 0);
        emit wifiStatusChanged();
        emit wifiReportChanged();   // RSSI and rates live in both frames
        break;
    }
    case v1: {
        const auto& r = idWifi->report();
        if (r.valid) {
            const qint64 now = wifiClock_.elapsed();
            const bool first = !wifiRestart_.seen();
            const uint32_t was = wifiRestart_.lastUptimeS();
            const qint64 expected = wifiRestart_.expectedS(now);
            // 0.11 says why the module last reset: a task watchdog, a brownout and an update look alike otherwise
            const QString why = r.resetReason >= 0
                ? QStringLiteral(", last reset: %1").arg(WifiRestartWatch::reasonText(r.resetReason)) : QString();
            const bool restarted = wifiRestart_.onUptime(r.uptimeS, now);
            // 0.14 counts the requests it could not queue: 0 is normal, anything else means lost answers
            const QString lost = r.lostValid
                ? QStringLiteral(", requests it could not queue: %1").arg(r.lostRequests) : QString();
            if (restarted) {
                qInfo().noquote() << wifiTag() << QStringLiteral("wifi: the module restarted (uptime %1 s, was %2 s, %3 s expected%4)")
                                         .arg(r.uptimeS).arg(was).arg(expected).arg(why);
                wifiLinkLog_.onRestart(r.resetReason >= 0 ? WifiRestartWatch::reasonText(r.resetReason) : QString(), now);
                emit wifiLinkEventsChanged();
                wifiFastReport_ = false;    // back at its saved period
                wifiKeepFastReports();
            }
            else if (first)
                qInfo().noquote() << wifiTag() << QStringLiteral("wifi: module up %1 s%2%3").arg(r.uptimeS).arg(why).arg(lost);
            if (r.lostValid) {
                const uint32_t n = wifiLoss_.onReport(r.lostRequests, now, restarted);
                if (n > 0)
                    qInfo().noquote() << wifiTag() << QStringLiteral("wifi: the module could not queue %1 requests (%2 since its start; 0 is normal)")
                                             .arg(n).arg(r.lostRequests);
            }
        }
        emit wifiReportChanged();
        break;
    }
    case v2:
        emit wifiNetworksChanged();
        break;
    case v4:
        emit wifiSavedChanged();
        break;
    case v5:
        emit wifiRelayChanged();
        break;
    case v6:
        emit wifiRelayStatsChanged();
        break;
    case v7: {
        const auto& r = idWifi->radio();
        const QString line = QStringLiteral("power %1 dBm (now %2) protocols %3 bw %4 ps %5 phy %6").arg(r.powerQ / 4.0).arg(r.powerNowQ / 4.0)
                                 .arg(r.protocols).arg(r.bandwidth).arg(r.powerSave).arg(r.phyNow);
        if (line != lastWifiNetLog_[7]) {
            lastWifiNetLog_[7] = line;
            qInfo().noquote() << wifiTag() << "wifi-radio:" << line;
        }
        wifiRadioCheck_->onRadio(r.powerNowQ / 4.0, r.phyNow, r.bandwidthNow == 2 ? 40 : 20);
        emit wifiRadioChanged();
        break;
    }
    default:
        break;
    }
}

void DevDriver::receivedWifiNet(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);

    if (resp != respNone) {
        if (resp == respOk && ver <= v6)          // role, access point, address and DHCP, lines, own address
            wifiProfileAfterSetting();
        if (resp == respErrorID) {                // the module predates 0.10
            idWifiNet->markUnsupported();
            emit wifiNetChanged();
            emit wifiNetPortsChanged();
            return;
        }
        // v7 is only ever asked for. 0.11 and older refuse it with ERR_VERSION: that means "no port information",
        // not a failed command, so it goes nowhere near the page's notices; other refusals of a page are dropped too.
        if (ver == v7) {
            if (resp == respErrorVersion && idWifiNet->portSupport() != 1) {
                if (idWifiNet->portSupport() != 0)
                    qInfo().noquote() << wifiTag() << "wifi-net: no port information (ID_WIFI_NET v7) in this firmware: before 0.12";
                idWifiNet->markPortsUnsupported();
                emit wifiNetPortsChanged();
            }
            return;
        }
        // The answer to a rate change of a port this app is not on belongs to that sequence.
        if (ver == v3 && wifiLine_->awaitingAnswer()) {
            wifiLine_->onAnswer(static_cast<int>(resp));
            return;
        }
        emit wifiNetCommandResult(static_cast<int>(ver), static_cast<int>(resp));
        return;
    }
    switch (ver) {
    case v4:
        emit wifiNetStatsChanged();
        break;
    case v5:
        emit wifiNetClientsChanged();
        break;
    case v7: {
        const WifiPorts& p = idWifiNet->ports();
        logWifiPort(p.lastPort(), p.lastPage());
        wifiKeepFastReports();
        if (p.lastPage() == 1)
            emit wifiNetPortDevicesChanged();
        else if (p.lastPage() == 2)
            emit wifiNetPortPeersChanged();
        else
            emit wifiNetPortsChanged();
        break;
    }
    default:
        logWifiNet(ver);
        emit wifiNetChanged();
        if (ver == v3)
            wifiLine_->onRecord(idWifiNet->lastLine());
        break;
    }
}

void DevDriver::logWifiPort(int port, int page) {
    // One line per changed port state (not its counters): which port the app is on, rates, what is connected.
    if (port < 0 || port >= WifiPorts::kPorts || page < 0 || page > 2)
        return;
    const WifiPorts& p = idWifiNet->ports();
    const QString name = port == 0 ? QStringLiteral("X1") : QStringLiteral("X2");
    QString line;
    if (page == 0) {
        const WifiPorts::Summary& s = p.summary(port);
        line = QStringLiteral("port %1: flags 0x%2 rate %3 saved %4 connected %5").arg(name).arg(uint(s.flags), 2, 16, QLatin1Char('0'))
                   .arg(s.baud).arg(s.baudSaved).arg(int(s.connected));
    } else if (page == 1) {
        QStringList devs;
        for (const WifiPorts::Device& d : p.devices(port).list)
            devs << (d.kind == WifiPorts::KindMavlink
                         ? QStringLiteral("MAVLink %1/%2 type %3").arg(int(d.addr)).arg(int(d.board)).arg(int(d.major))
                         : QStringLiteral("SBP %1 board %2").arg(int(d.addr)).arg(int(d.board)));
        line = QStringLiteral("port %1 devices: %2").arg(name, devs.isEmpty() ? QStringLiteral("none") : devs.join(QStringLiteral(", ")));
    } else {
        const WifiPorts::Network& n = p.network(port);
        QStringList peers;
        for (const WifiPorts::Peer& peer : n.peers)
            peers << IDBinWifiNet::addrText(peer.ip) + QStringLiteral(":") + QString::number(peer.port);
        line = QStringLiteral("port %1 line: mode %2 state %3 peers %4").arg(name).arg(int(n.mode)).arg(int(n.state))
                   .arg(peers.isEmpty() ? QStringLiteral("none") : peers.join(QStringLiteral(", ")));
    }
    if (line != lastWifiPortLog_[port][page]) {
        lastWifiPortLog_[port][page] = line;
        qInfo().noquote() << wifiTag() << "wifi-net:" << line;
    }
}

void DevDriver::logWifiNet(int ver) {
    // One line per changed setting in the app log: the field record of what the module reported.
    QString line;
    if (ver == v0) {
        const auto& r = idWifiNet->role();
        line = QStringLiteral("role saved %1 running %2").arg(r.saved).arg(r.running);
    } else if (ver == v1) {
        const auto& a = idWifiNet->ap();
        line = QStringLiteral("ap '%1' ch %2 auth %3 max %4 hidden %5").arg(a.ssid).arg(a.channel).arg(a.auth).arg(a.maxClients).arg(a.hidden);
    } else if (ver == v2) {
        const auto& c = idWifiNet->ipCfg();
        line = QStringLiteral("ip %1/%2 dhcp %3 pool %4-%5 lease %6").arg(IDBinWifiNet::addrText(c.ip), IDBinWifiNet::addrText(c.mask))
                   .arg(c.dhcp).arg(IDBinWifiNet::addrText(c.poolStart), IDBinWifiNet::addrText(c.poolEnd)).arg(c.leaseMin);
    } else if (ver == v3) {
        for (int i = 0; i < IDBinWifiNet::kLines; ++i) {
            const auto& l = idWifiNet->line(i);
            if (!l.valid) continue;
            line += QStringLiteral("line%1 mode %2 dest %3 %4:%5 lport %6 baud %7 pins %8/%9 uart %10; ").arg(i).arg(l.mode).arg(l.dest)
                        .arg(IDBinWifiNet::addrText(l.ip)).arg(l.rport).arg(l.lport).arg(l.baud).arg(l.txPin).arg(l.rxPin).arg(l.uart);
        }
    } else if (ver == v6) {
        line = QStringLiteral("own address %1").arg(idWifiNet->ownAddress());
    }
    if (!line.isEmpty() && line != lastWifiNetLog_[ver & 7]) {
        lastWifiNetLog_[ver & 7] = line;
        qInfo().noquote() << wifiTag() << "wifi-net:" << line;
    }
}

bool DevDriver::wifiStatusValid() { return idWifi->status().valid; }
int DevDriver::wifiState() { return idWifi->status().state; }
bool DevDriver::wifiAutoConnect() { return (idWifi->status().flags & IDBinWifi::FlagAutoConnect) != 0; }
bool DevDriver::wifiCurrentSaved() { return (idWifi->status().flags & IDBinWifi::FlagCurrentSaved) != 0; }
QString DevDriver::wifiSsid() { return idWifi->status().ssid; }
int DevDriver::wifiRssi() { return idWifi->report().valid ? idWifi->report().rssi : idWifi->status().rssi; }
int DevDriver::wifiChannel() { return idWifi->status().channel; }
int DevDriver::wifiSecurity() { return idWifi->status().security; }
int DevDriver::wifiReason() { return idWifi->status().reason; }
int DevDriver::wifiDisconnectCode() { return idWifi->status().disconnectCode; }
QString DevDriver::wifiBssid() { return idWifi->status().bssid; }
QString DevDriver::wifiIp() { return idWifi->status().ip; }
QString DevDriver::wifiMask() { return idWifi->status().mask; }
QString DevDriver::wifiGateway() { return idWifi->status().gateway; }
QString DevDriver::wifiDns() { return idWifi->status().dns; }
int DevDriver::wifiRxBps() { return static_cast<int>(idWifi->report().valid ? idWifi->report().rxBps : idWifi->status().rxBps); }
int DevDriver::wifiTxBps() { return static_cast<int>(idWifi->report().valid ? idWifi->report().txBps : idWifi->status().txBps); }
bool DevDriver::wifiReportValid() { return idWifi->report().valid; }
double DevDriver::wifiRxTotal() { return static_cast<double>(idWifi->report().rxTotal); }
double DevDriver::wifiTxTotal() { return static_cast<double>(idWifi->report().txTotal); }
int DevDriver::wifiUptime() { return static_cast<int>(idWifi->report().uptimeS); }
QString DevDriver::wifiTag() const {
    // The route the device answers from (its key in DeviceManager); m_devAddress is never set and was 0 for both
    // modules on the head unit's log (2026-09-29).
    return QStringLiteral("[%1]").arg(m_busAddress);
}

double DevDriver::wifiLostRequests() {
    const auto& r = idWifi->report();
    return r.valid && r.lostValid ? double(r.lostRequests) : -1.0;
}
QString DevDriver::wifiResetText() {
    const auto& r = idWifi->report();
    return r.valid && r.resetReason >= 0 ? WifiRestartWatch::reasonText(r.resetReason) : QString();
}
bool DevDriver::wifiScanning() { return idWifi->scanning(); }

QVariantList DevDriver::wifiNetworks() {
    QVariantList list;
    for (const IDBinWifi::Network& n : idWifi->networks()) {
        if (!n.valid) {
            continue;
        }
        QVariantMap m;
        m["ssid"] = n.ssid;
        m["rssi"] = static_cast<int>(n.rssi);
        m["channel"] = static_cast<int>(n.channel);
        m["security"] = static_cast<int>(n.security);
        m["saved"] = (n.flags & IDBinWifi::NetSaved) != 0;
        m["current"] = (n.flags & IDBinWifi::NetCurrent) != 0;
        m["bssid"] = n.bssid;
        list.append(m);
    }
    return list;
}

static QVector<WifiChannels::Net> scanNets(const IDBinWifi* wifi) {
    QVector<WifiChannels::Net> nets;
    for (const IDBinWifi::Network& n : wifi->networks())
        if (n.valid)
            nets.append({ int(n.channel), int(n.rssi), (n.flags & IDBinWifi::NetCurrent) != 0 });
    return nets;
}

QVariantList DevDriver::wifiChannels() {
    QVariantList list;
    if (idWifi->scanTotal() < 0)
        return list;
    const QVector<WifiChannels::Channel> rated = WifiChannels::rate(scanNets(idWifi));
    const int best = WifiChannels::best(rated);
    for (const WifiChannels::Channel& ch : rated) {
        QVariantMap m;
        m["channel"] = ch.channel;
        m["networks"] = ch.networks;
        m["strongest"] = ch.strongest;
        m["level"] = ch.levelDbm;
        m["best"] = ch.channel == best;
        m["ap"] = ch.channel <= WifiChannels::kLastAp;
        list.append(m);
    }
    return list;
}

int DevDriver::wifiBestChannel() {
    if (idWifi->scanTotal() < 0)
        return 0;
    return WifiChannels::best(WifiChannels::rate(scanNets(idWifi)));
}

int DevDriver::wifiSurveySupport() {
    return isWifi() ? WifiSurvey::supportFor(idVersion->fwVersion(), idVersion->fwVersionMinor()) : 0;
}

bool DevDriver::wifiSurveying() { return idWifiSurvey->running(); }

static QVector<WifiSurvey::Channel> surveyChannels(const IDBinWifiSurvey* s) {
    QVector<WifiSurvey::Channel> out;
    for (const IDBinWifiSurvey::Channel& c : s->channels()) {
        WifiSurvey::Channel w;
        w.valid = c.valid;
        w.channel = c.channel;
        w.dwellMs = c.dwellMs;
        w.busyPermille = c.busyPermille;
        w.frames = c.frames;
        w.transmitters = c.transmitters;
        w.strongest = c.strongest;
        w.noiseDbm = c.noiseDbm;
        w.flags = c.flags;
        w.ownPermille = c.ownPermille;
        w.ownValid = c.ownValid;
        out.append(w);
    }
    return out;
}

QVariantList DevDriver::wifiSurveyChannels() {
    QVariantList list;
    const QVector<WifiSurvey::Channel> chans = surveyChannels(idWifiSurvey);
    const QVector<WifiSurvey::Channel> mean = wifiSurveyWindow_.mean(wifiClock_.elapsed());
    const int best = WifiSurvey::best(mean);
    for (const WifiSurvey::Channel& c : chans) {
        if (!c.valid)
            continue;
        QVariantMap m;
        m["channel"] = c.channel;
        m["busy"] = c.busyPermille;
        // the other networks' airtime (what the pick compares) and ours on top of it
        const int others = WifiSurvey::compared(c);
        m["others"] = others;
        m["ownAir"] = c.busyPermille - others;
        int meanOthers = others;
        for (const WifiSurvey::Channel& a : mean)
            if (a.channel == c.channel)
                meanOthers = WifiSurvey::compared(a);
        m["mean"] = meanOthers;
        m["frames"] = c.frames;
        m["dwellMs"] = c.dwellMs;
        m["transmitters"] = c.transmitters;
        m["strongest"] = c.strongest;
        m["noise"] = c.noiseDbm;
        m["noiseValid"] = c.noiseValid();
        m["own"] = c.own();
        m["ap"] = c.apUsable();
        m["best"] = c.channel == best;
        list.append(m);
    }
    return list;
}

int DevDriver::wifiSurveyBest() { return WifiSurvey::best(wifiSurveyWindow_.mean(wifiClock_.elapsed())); }

int DevDriver::wifiSurveySamples() { return wifiSurveyWindow_.count(wifiClock_.elapsed()); }

QVariantList DevDriver::wifiLinkEvents() {
    QVariantList out;
    for (const WifiLinkLog::Event& e : wifiLinkLog_.events()) {
        QVariantMap m;
        m["kind"] = e.kind;
        m["atMs"] = double(e.atMs);
        m["state"] = e.state;
        m["prevState"] = e.prevState;
        m["reason"] = e.reason;
        m["code"] = e.code;
        m["clients"] = e.clients;
        m["prevClients"] = e.prevClients;
        m["ssid"] = e.ssid;
        m["channel"] = e.channel;
        m["rssi"] = e.rssi;
        m["text"] = e.text;
        out.append(m);
    }
    return out;
}

int DevDriver::wifiLinkRateSupport() {
    return isWifi() ? WifiLinkRate::supportFor(idVersion->fwVersion(), idVersion->fwVersionMinor()) : 0;
}

bool DevDriver::wifiLinkRateRunning() { return idWifiSurvey->rateRunning(); }

QVariantList DevDriver::wifiLinkRates() {
    QVariantList out;
    for (const QByteArray& page : idWifiSurvey->ratePages()) {
        WifiLinkRate::Page p;
        if (!WifiLinkRate::parse(page, p) || p.total == 0)
            continue;
        QVariantMap m;
        m["mac"] = p.mac;
        m["windowMs"] = p.windowMs;
        m["frames"] = p.frames;
        m["rssi"] = p.rssi;
        m["mode"] = p.mode;
        m["text"] = (p.frames == 0 || p.rates.isEmpty()) ? QString() : WifiLinkRate::text(p.rates.first());
        m["retries"] = p.retries;
        const WifiRetryWindow::Sum r = wifiRetries_.sum(p.mac, wifiClock_.elapsed());
        m["retrySum"] = r.retries;
        m["retryFrames"] = r.frames;
        m["retryReports"] = r.reports;
        QVariantList rates;
        for (const WifiLinkRate::Rate& r : p.rates) {
            QVariantMap x;
            x["kind"] = r.kind;
            x["code"] = r.code;
            x["sgi"] = (r.flags & 0x01) != 0;
            x["bw40"] = (r.flags & 0x02) != 0;
            x["kbps"] = double(r.kbps);
            x["frames"] = r.frames;
            x["text"] = WifiLinkRate::text(r);
            rates.append(x);
        }
        m["rates"] = rates;
        out.append(m);
    }
    return out;
}

void DevDriver::wifiLinkRate() {
    if (!m_state.connect || wifiLinkRateSupport() != 1 || idWifiSurvey->rateRunning() || idWifiSurvey->running()
        || wifiRadioOff())
        return;
    wifiLinkRateError_.clear();
    idWifiSurvey->requestLinkRate(uint16_t(WifiLinkRate::kWindowMs));
    emit wifiLinkRateChanged();
}

void DevDriver::wifiSurvey() {
    if (!m_state.connect || wifiSurveySupport() != 1 || idWifiSurvey->running())
        return;
    if (wifiRadioOff()) {                         // the module refuses it (ERR_RUNTIME) until the radio is back
        wifiSurveyError_ = tr("the radio is switched off");
        emit wifiSurveyChanged();
        return;
    }
    wifiSurveyError_.clear();
    wifiSurveyAskedAgain_ = false;
    idWifiSurvey->start(uint16_t(WifiSurvey::kDwellMs), 0);   // every channel it may visit
    emit wifiSurveyChanged();
}

void DevDriver::receivedWifiSurvey(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    if (ver == v1) {                              // the link rate report
        if (resp != respNone) {
            if (resp == respOk)
                return;
            idWifiSurvey->cancelLinkRate();
            wifiLinkRateError_ = resp == respErrorRuntime
                ? tr("the module is busy (a window, a survey, a scan or joining runs, or the last window was under 1 s ago)")
                : tr("the module refused the report (code %1)").arg(int(resp));
            emit wifiLinkRateChanged();
            return;
        }
        if (idWifiSurvey->rateCompletedCount() != wifiLinkRateTaken_) {
            wifiLinkRateTaken_ = idWifiSurvey->rateCompletedCount();
            QStringList parts, mains;
            const qint64 now = wifiClock_.elapsed();
            for (const QByteArray& page : idWifiSurvey->ratePages()) {
                WifiLinkRate::Page p;
                if (!WifiLinkRate::parse(page, p) || p.total == 0)
                    continue;
                wifiRetries_.add(p.mac, p.retries, p.frames, now);
                const QString main = p.rates.isEmpty() ? QStringLiteral("no data frames") : WifiLinkRate::text(p.rates.first());
                mains << p.mac + QStringLiteral(": ") + main;
                const WifiRetryWindow::Sum r = wifiRetries_.sum(p.mac, now);
                parts << QStringLiteral("%1: %2 (%3 of %4 frames, %5 dBm%6)").arg(p.mac, main)
                             .arg(p.rates.isEmpty() ? 0 : p.rates.first().frames).arg(p.frames).arg(p.rssi)
                             .arg(p.retries < 0 ? QString()
                                  : QStringLiteral("; came again %1, over the last %2 reports %3 of %4")
                                        .arg(p.retries).arg(r.reports).arg(r.retries).arg(r.frames));
            }
            // the log keeps a changed main rate, not every report
            const QString key = mains.join(QStringLiteral("; "));
            if (key != wifiLinkRateLogged_) {
                wifiLinkRateLogged_ = key;
                qInfo().noquote() << wifiTag() << QStringLiteral("wifi-rate: %1")
                                         .arg(parts.isEmpty() ? QStringLiteral("no other end") : parts.join(QStringLiteral("; ")));
            }
        }
        emit wifiLinkRateChanged();
        return;
    }
    if (resp != respNone) {
        if (resp == respOk)
            return;                               // the survey runs; its frames follow
        idWifiSurvey->cancel();
        wifiSurveyError_ = resp == respErrorRuntime ? tr("the module is busy (a survey, a scan or joining a network runs)")
                         : resp == respErrorKey ? tr("the module refused the key")
                         : tr("the module refused the survey (code %1)").arg(int(resp));
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-survey: not started: %1").arg(wifiSurveyError_);
        emit wifiSurveyChanged();
        return;
    }
    if (idWifiSurvey->completedCount() != wifiSurveyTaken_) {   // a survey is complete: into the mean, once
        wifiSurveyTaken_ = idWifiSurvey->completedCount();
        wifiSurveyWindow_.add(surveyChannels(idWifiSurvey), wifiClock_.elapsed());
        logWifiSurvey();
    }
    emit wifiSurveyChanged();
}

void DevDriver::logWifiSurvey() {
    QStringList parts;
    for (const IDBinWifiSurvey::Channel& c : idWifiSurvey->channels())
        if (c.valid)
            parts << QStringLiteral("%1%2:%3.%4%%5").arg(c.channel).arg((c.flags & 0x01) ? QStringLiteral("*") : QString())
                         .arg(c.busyPermille / 10).arg(c.busyPermille % 10)
                         .arg(c.ownValid && c.ownPermille > 0 ? QStringLiteral("(ours %1.%2)").arg(c.ownPermille / 10).arg(c.ownPermille % 10)
                                                              : QString());
    qInfo().noquote() << wifiTag() << QStringLiteral("wifi-survey: Wi-Fi airtime %1; best %2 by the mean of %3 (* our channel)")
                             .arg(parts.join(QLatin1Char(' '))).arg(wifiSurveyBest()).arg(wifiSurveySamples());
}

int DevDriver::wifiRfSupport() {
    return isWifi() ? WifiRf::supportFor(idVersion->fwVersion(), idVersion->fwVersionMinor()) : 0;
}

bool DevDriver::wifiRadioOff() {
    const IDBinWifiRf::State& s = idWifiRf->state();
    if (s.valid)
        return !s.on;
    const auto& st = idWifi->status();
    return st.valid && (st.flags & IDBinWifi::FlagRadioOff) != 0;
}

bool DevDriver::wifiRadioOffTimed() {
    const IDBinWifiRf::State& s = idWifiRf->state();
    return s.valid && !s.on && s.secondsLeft > 0;
}

bool DevDriver::wifiRadioOffUntilOn() {
    const IDBinWifiRf::State& s = idWifiRf->state();
    return s.valid && !s.on && s.secondsLeft == 0;
}

int DevDriver::wifiRadioOffLeft() {
    if (!wifiRadioOffTimed())
        return 0;
    return WifiRf::leftS(idWifiRf->state().secondsLeft, wifiRfAtMs_, wifiClock_.elapsed());
}

bool DevDriver::wifiRadioWired() { return wifiNetOwnPort() >= 0; }

bool DevDriver::wifiRadioSwitchOff(int seconds) {
    const bool wired = wifiRadioWired();
    if (!m_state.connect || wifiRfSupport() != 1 || !WifiRf::offAllowed(seconds, wired))
        return false;
    wifiRfError_.clear();
    idWifiRf->switchOff(uint16_t(seconds));
    qInfo().noquote() << wifiTag() << QStringLiteral("wifi-rf: switching the radio off %1, over %2")
                             .arg(seconds > 0 ? QStringLiteral("for %1 s").arg(seconds) : QStringLiteral("until on"),
                                  wired ? QStringLiteral("a wire") : QStringLiteral("the network"));
    emit wifiRfChanged();
    return true;
}

bool DevDriver::wifiRadioSwitchOn() {
    if (!m_state.connect || wifiRfSupport() != 1)
        return false;
    wifiRfError_.clear();
    idWifiRf->switchOn();
    qInfo().noquote() << wifiTag() << "wifi-rf: switching the radio on";
    emit wifiRfChanged();
    return true;
}

void DevDriver::wifiRadioRefresh() {
    if (m_state.connect && wifiRfSupport() == 1)
        idWifiRf->requestAll();
}

void DevDriver::receivedWifiRf(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);
    if (resp != respNone) {
        if (resp == respOk)
            return;                               // the state follows (CONTENT v0)
        wifiRfError_ = resp == respErrorRuntime
            ? tr("the radio is busy: a channel survey or a link speed window, a scan, or a firmware update (or the first minute after one) runs; try again in a moment")
            : resp == respErrorPayload
            ? (wifiRadioWired() ? tr("the module refused the request (ERR_PAYLOAD)")
                                : tr("over the network the module switches its radio off only for 1 s to 10 min"))
            : resp == respErrorKey ? tr("the module refused the key")
            : (resp == respErrorID || resp == respErrorVersion || resp == respErrorType)
            ? tr("this firmware cannot switch the radio off (that came with 0.20)")
            : tr("the module refused the request (code %1)").arg(int(resp));
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-rf: refused: %1").arg(wifiRfError_);
        emit wifiRfChanged();
        return;
    }
    wifiRfAtMs_ = wifiClock_.elapsed();
    const IDBinWifiRf::State& s = idWifiRf->state();
    qInfo().noquote() << wifiTag() << (s.on ? QStringLiteral("wifi-rf: radio on")
                                       : s.secondsLeft > 0 ? QStringLiteral("wifi-rf: radio off, on again in %1 s").arg(s.secondsLeft)
                                       : QStringLiteral("wifi-rf: radio off until switched on or a reboot"));
    emit wifiRfChanged();
}

void DevDriver::wifiRfOnStatus(bool flagOff) {
    const IDBinWifiRf::State& s = idWifiRf->state();
    const WifiRf::StatusStep step = WifiRf::onStatus(s.valid, s.on, flagOff, wifiClock_.elapsed() - wifiRfAtMs_);
    if (step == WifiRf::StatusBackOn) {
        qInfo().noquote() << wifiTag() << "wifi-rf: the status says the radio is back on";
        idWifiRf->markOn();
    } else if (step == WifiRf::StatusOffAsk) {
        qInfo().noquote() << wifiTag() << "wifi-rf: the status says the radio is off: asking for how long";
        idWifiRf->reset();                        // off by the status until the module says for how long
    }
    if ((step == WifiRf::StatusOffAsk || step == WifiRf::StatusAsk) && m_state.connect && wifiRfSupport() == 1)
        idWifiRf->requestAll();
    emit wifiRfChanged();
}

int DevDriver::wifiProfileSupport() {
    return isWifi() ? WifiProfileRules::supportFor(idVersion->fwVersion(), idVersion->fwVersionMinor()) : 0;
}

QString DevDriver::wifiProfileName() const {
    const IDBinWifiProfile::Status& s = idWifiProfile->status();
    return s.valid && (s.flags & 1) ? s.name : QString();
}

bool DevDriver::wifiProfileLoaded() const {
    const IDBinWifiProfile::Status& s = idWifiProfile->status();
    return s.valid && (s.flags & 1);
}

bool DevDriver::wifiProfileModified() const {
    const IDBinWifiProfile::Status& s = idWifiProfile->status();
    return s.valid && (s.flags & 1) && (s.flags & 2);
}

int DevDriver::wifiProfileSent() const { return wifiProfileUpload_ ? wifiProfileUpload_->sent() : 0; }

int DevDriver::wifiProfileTotal() const { return wifiProfileUpload_ ? wifiProfileUpload_->total() : 0; }

QString DevDriver::wifiProfileFolder() const { return WifiProfileStore::folder(); }

namespace {

// The passwords typed on the page over the profile's own: "ap" and "net<index>".
void applyProfilePasswords(WifiProfile* p, const QVariantMap& passwords)
{
    const auto ap = passwords.constFind(QStringLiteral("ap"));
    if (ap != passwords.constEnd())
        p->apPassword = ap.value().toString();
    for (int i = 0; i < p->networks.size(); ++i) {
        const auto n = passwords.constFind(QStringLiteral("net%1").arg(i));
        if (n != passwords.constEnd())
            p->networks[i].password = n.value().toString();
    }
}

QVariantList profileSlots(const WifiProfile& p)
{
    QVariantList out;
    if (p.apAuth != 0)
        out << QVariantMap{ { QStringLiteral("key"), QStringLiteral("ap") }, { QStringLiteral("ssid"), p.apSsid },
                            { QStringLiteral("kind"), QStringLiteral("ap") }, { QStringLiteral("value"), p.apPassword },
                            { QStringLiteral("required"), true } };
    for (int i = 0; i < p.networks.size(); ++i)
        out << QVariantMap{ { QStringLiteral("key"), QStringLiteral("net%1").arg(i) },
                            { QStringLiteral("ssid"), p.networks[i].ssid }, { QStringLiteral("kind"), QStringLiteral("net") },
                            { QStringLiteral("value"), p.networks[i].password },
                            { QStringLiteral("required"), p.networks[i].secure } };
    return out;
}

} // namespace

WifiProfile DevDriver::wifiProfileNow(uint32_t* known) {
    WifiProfile p;
    uint32_t k = 0;
    const IDBinWifiNet::RoleInfo& role = idWifiNet->role();
    if (role.valid) {
        p.role = role.saved;                      // what the module boots into: the profile sets that
        k |= WifiProfileDiff::KnownRole;
    }
    if (idWifiNet->ownAddress() > 0) {
        p.address = uint8_t(idWifiNet->ownAddress());
        k |= WifiProfileDiff::KnownAddress;
    }
    const WifiPorts& ports = idWifiNet->ports();
    if (idWifiNet->portSupport() == 1 && ports.summary(0).valid) {
        p.x1Baud = ports.summary(0).baudSaved;
        k |= WifiProfileDiff::KnownX1;
    }
    if (idWifiNet->portSupport() == 1 && ports.summary(1).valid) {
        p.x2Baud = ports.summary(1).baudSaved;
        k |= WifiProfileDiff::KnownX2;
    }
    const IDBinWifi::Radio& r = idWifi->radio();
    if (r.valid) {
        p.powerQ = r.powerQ;
        p.protocols = r.protocols;
        p.bandwidth = r.bandwidth;
        p.powerSave = r.powerSave;
        k |= WifiProfileDiff::KnownRadio;
    }
    const IDBinWifiNet::Ap& ap = idWifiNet->ap();
    if (ap.valid) {
        p.apChannel = ap.channel;
        p.apHidden = ap.hidden;
        p.apMaxClients = ap.maxClients;
        p.apAuth = ap.auth;
        p.apSsid = ap.ssid;
        k |= WifiProfileDiff::KnownAp;
    }
    const IDBinWifiNet::IpCfg& ip = idWifiNet->ipCfg();
    if (ip.valid) {
        p.ip = ip.ip;
        p.mask = ip.mask;
        p.dhcp = ip.dhcp ? 1 : 0;
        p.poolStart = ip.poolStart;
        p.poolEnd = ip.poolEnd;
        p.leaseMin = ip.leaseMin;
        p.offer = ip.offer;
        k |= WifiProfileDiff::KnownIp;
    }
    for (int i = 0; i < IDBinWifiNet::kLines; ++i) {
        const IDBinWifiNet::Line& l = idWifiNet->line(i);
        if (!l.valid)
            continue;
        WifiProfile::Line& t = i == 0 ? p.line0 : p.line1;
        t.mode = l.mode;
        t.dest = l.dest;
        t.ip = l.ip;
        t.rport = l.rport;
        t.lport = l.lport;
        k |= i == 0 ? WifiProfileDiff::KnownLine0 : WifiProfileDiff::KnownLine1;
        if (i == 1) {
            p.txPin = l.txPin;
            p.rxPin = l.rxPin;
            if (!(k & WifiProfileDiff::KnownX2))
                p.x2Baud = l.baud;                // the rate in effect; the port summary's saved one is preferred
            if (l.xlatSaved >= 0) {
                p.xlat = uint8_t(l.xlatSaved);
                k |= WifiProfileDiff::KnownXlat;
            }
        }
    }
    const QStringList& saved = idWifi->savedNetworks();
    if (idWifi->savedTotal() >= 0 && saved.size() == idWifi->savedTotal() && !saved.contains(QString())) {
        for (const QString& s : saved)
            p.networks.append({ s, QString(), true });
        k |= WifiProfileDiff::KnownNets;
    }
    if (known)
        *known = k;
    return p;
}

int DevDriver::wifiProfileAdapterMax() const {
    LinkManagerWrapper* links = core.getLinkManagerWrapperPtr();
    if (!links || linkUuid_.isNull())
        return 0;
    const QUuid link = linkUuid_;
    QString port;
    const auto read = [links, link, &port]() {
        LinkListModel* m = links->getModelPtr();
        if (m && m->containsUuid(link)
            && m->valueForUuid(link, LinkListModel::Roles::LinkType).toInt() == static_cast<int>(LinkType::kLinkSerial))
            port = m->valueForUuid(link, LinkListModel::Roles::PortName).toString();
    };
    if (links->thread() == QThread::currentThread())
        read();
    else
        QMetaObject::invokeMethod(links, read, Qt::BlockingQueuedConnection);
    if (port.isEmpty())
        return 0;
    for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts())
        if ((info.portName() == port || info.systemLocation() == port) && info.hasVendorIdentifier() && info.hasProductIdentifier())
            return WifiProfileRules::adapterMaxBaud(info.vendorIdentifier(), info.productIdentifier());
    return 0;
}

QVariantList DevDriver::wifiProfileList() {
    QVariantList out;
    for (const WifiProfileStore::Entry& e : WifiProfileStore::list()) {
        QStringList nets;
        for (const WifiProfile::Net& n : e.profile.networks)
            nets << n.ssid;
        out << QVariantMap{ { QStringLiteral("id"), e.id }, { QStringLiteral("builtin"), e.builtin },
                            { QStringLiteral("error"), e.error }, { QStringLiteral("file"), e.file },
                            { QStringLiteral("name"), e.error.isEmpty() ? e.profile.name : QFileInfo(e.file).fileName() },
                            { QStringLiteral("role"), int(e.profile.role) }, { QStringLiteral("apSsid"), e.profile.apSsid },
                            { QStringLiteral("networks"), nets }, { QStringLiteral("address"), int(e.profile.address) },
                            { QStringLiteral("x1Baud"), double(e.profile.x1Baud) },
                            { QStringLiteral("x2Baud"), double(e.profile.x2Baud) } };
    }
    return out;
}

QVariantMap DevDriver::wifiProfilePreview(const QString& id, const QVariantMap& passwords) {
    QVariantMap out;
    out.insert(QStringLiteral("id"), id);
    out.insert(QStringLiteral("ready"), false);
    WifiProfileStore::Entry e;
    if (!WifiProfileStore::find(id, &e)) {
        out.insert(QStringLiteral("error"), tr("This profile is not there any more"));
        return out;
    }
    if (!e.error.isEmpty()) {
        out.insert(QStringLiteral("error"), tr("The file cannot be used: %1").arg(e.error));
        return out;
    }
    WifiProfile p = e.profile;
    applyProfilePasswords(&p, passwords);
    out.insert(QStringLiteral("name"), p.name);
    out.insert(QStringLiteral("builtin"), e.builtin);
    out.insert(QStringLiteral("slots"), profileSlots(p));

    uint32_t known = 0;
    const WifiProfile now = wifiProfileNow(&known);
    QVariantList changes;
    for (const WifiProfileChange& c : WifiProfileDiff::changes(now, known, p))
        changes << QVariantMap{ { QStringLiteral("key"), c.key }, { QStringLiteral("what"), c.what },
                                { QStringLiteral("from"), c.from }, { QStringLiteral("to"), c.to },
                                { QStringLiteral("main"), c.primary } };
    out.insert(QStringLiteral("changes"), changes);
    QStringList unread;
    const auto miss = [&](uint32_t flag, const QString& what) { if (!(known & flag)) unread << what; };
    miss(WifiProfileDiff::KnownRole, tr("role"));
    miss(WifiProfileDiff::KnownAp, tr("access point"));
    miss(WifiProfileDiff::KnownNets, tr("station networks"));
    miss(WifiProfileDiff::KnownAddress, tr("address"));
    miss(WifiProfileDiff::KnownX1 | WifiProfileDiff::KnownX2, tr("port rates"));
    miss(WifiProfileDiff::KnownRadio, tr("radio"));
    miss(WifiProfileDiff::KnownLine0 | WifiProfileDiff::KnownLine1, tr("lines"));
    miss(WifiProfileDiff::KnownIp, tr("address and DHCP"));
    out.insert(QStringLiteral("unread"), unread);

    QString encodeError;
    const QByteArray blob = p.encode(&encodeError);
    QStringList problems = p.problems();
    if (!encodeError.isEmpty())
        problems << encodeError;
    out.insert(QStringLiteral("problems"), problems);
    out.insert(QStringLiteral("bytes"), int(blob.size()));

    // Which port of the module this app is on decides what the reboot does to this app's link.
    const WifiPorts& ports = idWifiNet->ports();
    const bool portsKnown = idWifiNet->portSupport() == 1 && ports.summary(0).valid && ports.summary(1).valid;
    const int own = portsKnown ? ports.askedPort() : -1;
    const int newBaud = WifiProfileRules::portBaudAfter(p, own);
    const int linkBaud = wifiLinkBaud();
    out.insert(QStringLiteral("ownPort"), own);
    out.insert(QStringLiteral("newBaud"), newBaud);
    out.insert(QStringLiteral("linkBaud"), linkBaud);

    QStringList warnings;
    QString blocked;
    warnings << tr("The module reboots into the profile: its link drops for a few seconds.");
    if (!m_state.connect)
        blocked = tr("No connection to the module.");
    else if (wifiProfileSupport() != 1)
        blocked = tr("The module's firmware cannot take profiles (that came with 0.22).");
    else if (wifiProfileStep_ != 0)
        blocked = tr("A profile is being loaded.");
    else if (!portsKnown)
        blocked = tr("The module's ports are not read yet (which one this app is on): wait a moment.");
    else if (own >= 0 && linkBaud > 0 && wifiProfileAdapterMax() > 0 && newBaud > wifiProfileAdapterMax())
        blocked = tr("This app is on the module's X%1 through an adapter that follows at most %2 bit/s (CP2105), and the "
                     "profile sets X%1 to %3 bit/s: the module would be lost to this wire until its BOOT button resets "
                     "the rates. Load this profile over the network or through another adapter.")
                      .arg(own + 1).arg(WifiProfileDiff::baudText(uint32_t(wifiProfileAdapterMax())))
                      .arg(WifiProfileDiff::baudText(uint32_t(newBaud)));
    else if (own == 1 && p.xlat != 0)
        blocked = tr("This app is on X2, and the profile makes X2 the input of the GODIO telemetry translator, which only "
                     "listens: the module would be lost to this wire. Load it through X1 or over the network.");
    if (own >= 0 && linkBaud > 0 && newBaud > 0 && newBaud != linkBaud)
        warnings << tr("The profile sets this app's port X%1 to %2 bit/s: the app follows the module there after the reboot.")
                        .arg(own + 1).arg(WifiProfileDiff::baudText(uint32_t(newBaud)));
    if ((known & WifiProfileDiff::KnownAddress) && now.address != p.address)
        warnings << tr("The module comes back at address %1: it shows in the device list as another device.").arg(p.address);
    const IDBinWifiNet::Line& l1 = idWifiNet->line(1);
    if (p.xlat != 0 && l1.valid && l1.xlatBuild >= 0 && !(l1.xlatBuild & 1))
        warnings << tr("The module's firmware has no GODIO telemetry translator: it will refuse this profile.");
    bool netChange = false;
    for (const WifiProfileChange& c : WifiProfileDiff::changes(now, known, p))
        netChange = netChange || c.key == QLatin1String("role") || c.key == QLatin1String("ap")
                    || c.key == QLatin1String("networks") || c.key == QLatin1String("radio");
    if (netChange)
        warnings << (own < 0 && portsKnown
                     ? tr("This app reaches the module over the network. A boat and head unit pair is changed in this "
                          "order: first the boat's module over the air, then the head unit's by wire to the boat's new "
                          "network; the other way round they lose each other.")
                     : tr("If this module is one of a boat and head unit pair: change the boat's module first (over the "
                          "air), then the head unit's (by wire), or they lose each other."));
    for (const WifiProfile::Net& n : p.networks)
        if (n.password.isEmpty() && !n.secure)
            warnings << tr("The network \"%1\" has no password: the module joins it as an open network.").arg(n.ssid);
    out.insert(QStringLiteral("warnings"), warnings);
    out.insert(QStringLiteral("blocked"), blocked);
    out.insert(QStringLiteral("ready"), blocked.isEmpty() && problems.isEmpty() && !blob.isEmpty());
    return out;
}

bool DevDriver::wifiProfileLoad(const QString& id, const QVariantMap& passwords) {
    const QVariantMap pv = wifiProfilePreview(id, passwords);
    if (!pv.value(QStringLiteral("ready")).toBool()) {
        QString why = pv.value(QStringLiteral("error")).toString();
        if (why.isEmpty())
            why = pv.value(QStringLiteral("blocked")).toString();
        if (why.isEmpty())
            why = pv.value(QStringLiteral("problems")).toStringList().join(QStringLiteral("; "));
        wifiProfileResult_ = tr("Not loaded: %1").arg(why);
        wifiProfileResultBad_ = true;
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: \"%1\" not sent: %2").arg(id, why);
        emit wifiProfileChanged();
        return false;
    }
    WifiProfileStore::Entry e;
    WifiProfileStore::find(id, &e);
    WifiProfile p = e.profile;
    applyProfilePasswords(&p, passwords);
    const QByteArray blob = p.encode();
    wifiProfileLoading_ = p;
    wifiProfileOwnPort_ = pv.value(QStringLiteral("ownPort")).toInt();
    wifiProfileNewBaud_ = pv.value(QStringLiteral("newBaud")).toInt();
    wifiProfileOldAddress_ = idWifiNet->ownAddress();
    if (!wifiProfileUpload_->start(blob))
        return false;
    wifiProfileResult_.clear();
    wifiProfileResultBad_ = false;
    qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: loading \"%1\": %2 bytes, crc %3, %4")
                             .arg(p.name).arg(blob.size()).arg(WifiProfile::crc32(blob), 8, 16, QLatin1Char('0'))
                             .arg(wifiProfileOwnPort_ >= 0 ? QStringLiteral("this app on X%1, %2 bit/s after it")
                                                                 .arg(wifiProfileOwnPort_ + 1).arg(wifiProfileNewBaud_)
                                                           : QStringLiteral("this app over the network"));
    emit wifiProfileChanged();
    return true;
}

void DevDriver::wifiProfileFinished(int outcome) {
    const QString name = wifiProfileLoading_.name;
    const int reason = wifiProfileUpload_->reason();
    const int resp = wifiProfileUpload_->resp();
    wifiProfileStep_ = 0;
    wifiProfileResultBad_ = outcome != WifiProfileUpload::Saved;
    switch (outcome) {
    case WifiProfileUpload::Saved: {
        wifiProfileSavedAtMs_ = wifiClock_.elapsed();
        const bool moved = wifiProfileOldAddress_ > 0 && wifiProfileOldAddress_ != wifiProfileLoading_.address;
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: \"%1\" taken, the module reboots into it (address %2%3)")
                                 .arg(name).arg(wifiProfileLoading_.address)
                                 .arg(moved ? QStringLiteral(", was %1").arg(wifiProfileOldAddress_) : QString());
        // This app's port follows the module's new rate once it reboots (it does 200 ms after its answer).
        const int linkBaud = wifiLinkBaud();
        if (wifiProfileOwnPort_ >= 0 && linkBaud > 0 && wifiProfileNewBaud_ > 0 && wifiProfileNewBaud_ != linkBaud) {
            LinkManagerWrapper* links = core.getLinkManagerWrapperPtr();
            const QUuid link = linkUuid_;
            const int baud = wifiProfileNewBaud_;
            QTimer::singleShot(300, this, [this, links, link, baud]() {
                if (links)
                    QMetaObject::invokeMethod(links, [links, link, baud]() { links->updateBaudrate(link, baud); }, Qt::QueuedConnection);
                qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: this app's link to %1 bit/s after the module").arg(baud);
            });
        }
        if (moved) {
            // Another address is another device in this app: this one will not hear it again.
            wifiProfileResult_ = tr("The module took \"%1\" and reboots into it. It comes back at address %2, as another "
                                    "device in the list.").arg(name).arg(wifiProfileLoading_.address);
            wifiProfileResultBad_ = false;
            notifyInfo(wifiProfileResult_);
        } else {
            wifiProfileStep_ = 3;
            wifiProfileResult_ = tr("The module took \"%1\" and reboots into it…").arg(name);
            wifiProfileBackTimer_.start();
        }
        break;
    }
    case WifiProfileUpload::Refused: {
        const QString why = reason == WifiProfile::ReasonIncomplete ? tr("the upload did not arrive whole")
            : reason == WifiProfile::ReasonCrc ? tr("the upload arrived damaged (its CRC)")
            : reason == WifiProfile::ReasonFormat ? tr("the block's format or a length")
            : reason == WifiProfile::ReasonName ? tr("the name (1 to 32 bytes)")
            : reason == WifiProfile::ReasonRole ? tr("the role")
            : reason == WifiProfile::ReasonAddress ? tr("the address (0 and 255 are not allowed)")
            : reason == WifiProfile::ReasonX1Baud ? tr("the X1 rate (9600 to 5 000 000)")
            : reason == WifiProfile::ReasonRadio ? tr("the radio settings")
            : reason == WifiProfile::ReasonAp ? tr("the access point (channel 1 to 11, 1 to 10 clients, a password of 8 to 63 characters)")
            : reason == WifiProfile::ReasonIp ? tr("the address and DHCP")
            : reason == WifiProfile::ReasonLine0 ? tr("the X1 line")
            : reason == WifiProfile::ReasonLine1 ? tr("the X2 line (its rate or pins)")
            : reason == WifiProfile::ReasonLines ? tr("both UDP lines on one port")
            : reason == WifiProfile::ReasonXlat ? tr("the input translator, which this firmware does not have")
            : reason == WifiProfile::ReasonNets ? tr("the networks (more than 8, a length, a name twice or a password)")
            : reason >= 0 ? tr("reason %1").arg(reason) : tr("the reason did not come");
        wifiProfileResult_ = tr("The module refused \"%1\": %2. Nothing changed in it.").arg(name, why);
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: \"%1\" refused (code %2): %3")
                                 .arg(name).arg(resp).arg(WifiProfile::reasonName(reason));
        notifyWarning(wifiProfileResult_);
        break;
    }
    case WifiProfileUpload::Busy:
        wifiProfileResult_ = tr("The module is updating its firmware, or its new firmware has not confirmed itself yet: "
                                "try again in a minute. Nothing changed.");
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: \"%1\" refused: busy (a firmware update)").arg(name);
        notifyWarning(wifiProfileResult_);
        break;
    case WifiProfileUpload::WriteFailed:
        wifiProfileResult_ = tr("The module's flash refused the write halfway: part of \"%1\" is saved and the module did "
                                "not reboot (its profile shows as changed). Load it again.").arg(name);
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: \"%1\": ERR_RUNTIME without a reason, written halfway").arg(name);
        notifyWarning(wifiProfileResult_);
        break;
    case WifiProfileUpload::Rejected:
        wifiProfileResult_ = tr("The module refused the upload of \"%1\" (code %2). Nothing changed.").arg(name).arg(resp);
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: \"%1\": upload refused (code %2)").arg(name).arg(resp);
        notifyWarning(wifiProfileResult_);
        break;
    default: {
        const bool atApply = wifiProfileUpload_->sent() == wifiProfileUpload_->total();
        wifiProfileResult_ = atApply
            ? tr("No answer to \"apply\": the module may have rebooted into \"%1\". If it is gone, look for it in the device "
                 "list (address %2).").arg(name).arg(wifiProfileLoading_.address)
            : tr("No answer from the module while sending \"%1\". Nothing changed.").arg(name);
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: \"%1\": no answer (%2 of %3 bytes confirmed)")
                                 .arg(name).arg(wifiProfileUpload_->sent()).arg(wifiProfileUpload_->total());
        notifyWarning(wifiProfileResult_);
        break;
    }
    }
    emit wifiProfileChanged();
}

void DevDriver::wifiProfileAfterSetting() {
    // The module does not push its profile status when a setting changes (the module session, 2026-11-05: the
    // contract stays); the host asks right after each of its own settings a profile holds, the page every 5 s.
    if (wifiProfileSupport() == 1)
        wifiProfileAskTimer_.start();
}

void DevDriver::wifiProfileRefresh() {
    if (m_state.connect && wifiProfileSupport() != 0)
        idWifiProfile->requestAll();
}

void DevDriver::receivedWifiProfile(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    if (resp != respNone) {
        if (wifiProfileUpload_->active())
            wifiProfileUpload_->onAnswer(int(ver), int(resp));
        return;
    }
    if (ver == v3) {
        if (wifiProfileUpload_->active())
            wifiProfileUpload_->onReason(idWifiProfile->lastReason());
        return;
    }
    if (ver != v0)
        return;
    const IDBinWifiProfile::Status& s = idWifiProfile->status();
    WifiProfileStatus st;
    st.valid = s.valid;
    st.loaded = (s.flags & 1) != 0;
    st.modified = (s.flags & 2) != 0;
    st.fingerprint = s.fingerprint;
    st.received = s.received;
    st.announced = s.announced;
    st.name = s.name;
    if (wifiProfileUpload_->active())
        wifiProfileUpload_->onStatus(st);
    if (wifiProfileStep_ == 3 && wifiClock_.elapsed() - wifiProfileSavedAtMs_ > 500 && st.loaded
        && st.name == wifiProfileLoading_.name && st.announced == 0) {
        wifiProfileBackTimer_.stop();
        wifiProfileStep_ = 0;
        wifiProfileResultBad_ = st.modified;
        wifiProfileResult_ = st.modified
            ? tr("The module is back with \"%1\", but says a setting changed since.").arg(st.name)
            : tr("Profile \"%1\" loaded: the module is back in %2 s.").arg(st.name)
                  .arg(QString::number((wifiClock_.elapsed() - wifiProfileSavedAtMs_) / 1000.0, 'f', 1));
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: back with \"%1\" after %2 ms%3").arg(st.name)
                                 .arg(wifiClock_.elapsed() - wifiProfileSavedAtMs_)
                                 .arg(st.modified ? QStringLiteral(", changed since") : QString());
        if (st.modified)
            notifyWarning(wifiProfileResult_);
        else
            notifyInfo(wifiProfileResult_);
    }
    const QString logged = !st.loaded ? QStringLiteral("no profile") : QStringLiteral("\"%1\"%2").arg(st.name)
                               .arg(st.modified ? QStringLiteral(", changed since") : QString());
    if (logged != wifiProfileLogged_) {
        wifiProfileLogged_ = logged;
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: the module carries %1").arg(logged);
    }
    emit wifiProfileChanged();
}

void DevDriver::wifiProfileCapture() {
    if (!m_state.connect || wifiProfileSupport() == 0) {
        emit wifiProfileCaptured({ { QStringLiteral("error"), tr("No connection to the module.") } });
        return;
    }
    if (wifiProfileCapturing_)
        return;
    wifiProfileCapturing_ = true;
    idWifi->requestAll();
    idWifiNet->requestAll();
    idWifiNet->requestPorts();
    idWifiProfile->requestAll();
    QTimer::singleShot(2500, this, &DevDriver::wifiProfileCaptureDone);
}

void DevDriver::wifiProfileCaptureDone() {
    wifiProfileCapturing_ = false;
    uint32_t known = 0;
    WifiProfile p = wifiProfileNow(&known);
    QStringList unread;
    const auto miss = [&](uint32_t flag, const QString& what) { if ((known & flag) != flag) unread << what; };
    miss(WifiProfileDiff::KnownRole, tr("role"));
    miss(WifiProfileDiff::KnownAp, tr("access point"));
    miss(WifiProfileDiff::KnownNets, tr("station networks"));
    miss(WifiProfileDiff::KnownAddress, tr("address"));
    miss(WifiProfileDiff::KnownX1 | WifiProfileDiff::KnownX2, tr("port rates"));
    miss(WifiProfileDiff::KnownRadio, tr("radio"));
    miss(WifiProfileDiff::KnownLine0 | WifiProfileDiff::KnownLine1, tr("lines"));
    miss(WifiProfileDiff::KnownIp, tr("address and DHCP"));
    QVariantMap out;
    out.insert(QStringLiteral("unread"), unread);
    if (!unread.isEmpty()) {
        out.insert(QStringLiteral("error"), tr("Not every setting was read from the module: %1. Try again in a moment.")
                                                .arg(unread.join(QStringLiteral(", "))));
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: capture: not read: %1").arg(unread.join(QStringLiteral(", ")));
        emit wifiProfileCaptured(out);
        return;
    }
    p.name = wifiProfileLoaded() ? wifiProfileName() : tr("Module %1").arg(p.address);
    wifiProfileCaptured_ = p;
    wifiProfileCapturedKnown_ = known;
    out.insert(QStringLiteral("name"), p.name);
    out.insert(QStringLiteral("slots"), profileSlots(p));
    qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: capture: the module's settings read (%1 networks)")
                             .arg(p.networks.size());
    emit wifiProfileCaptured(out);
}

QString DevDriver::wifiProfileSaveCaptured(const QString& name, const QVariantMap& passwords) {
    if (wifiProfileCapturedKnown_ == 0)
        return tr("Read the module's settings first.");
    WifiProfile p = wifiProfileCaptured_;
    p.name = name.trimmed();
    const int bytes = int(p.name.toUtf8().size());
    if (bytes < 1 || bytes > 32)
        return tr("A name of 1 to 32 bytes is needed (a Cyrillic letter takes 2).");
    applyProfilePasswords(&p, passwords);
    // A password left empty is asked when the profile is loaded: every saved network stays "secure" (the module does
    // not say which are open), the access point's is asked by its security.
    QStringList bad;
    const int apLen = int(p.apPassword.toUtf8().size());
    if (p.apAuth != 0 && apLen > 0 && (apLen < 8 || apLen > 63))
        bad << tr("the access point's password: 8 to 63 characters");
    WifiProfile check = p;
    for (WifiProfile::Net& n : check.networks)
        if (n.password.isEmpty())
            n.secure = false;                     // empty is allowed in the file
    check.apAuth = 0;                             // checked above
    bad << check.problems();
    if (!bad.isEmpty())
        return bad.join(QStringLiteral("; "));
    QString id;
    const QString err = WifiProfileStore::save(p, &id);
    if (!err.isEmpty())
        return err;
    qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: the module's settings saved as \"%1\" (%2)").arg(p.name, id);
    emit wifiProfileListChanged();
    return QString();
}

QString DevDriver::wifiProfileImport(const QString& path) {
    const QUrl url(path);
    const QString file = url.isLocalFile() ? url.toLocalFile() : path;
    QString id;
    const QString err = WifiProfileStore::import(file, &id);
    if (err.isEmpty()) {
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: imported %1 as %2").arg(QFileInfo(file).fileName(), id);
        emit wifiProfileListChanged();
    }
    return err;
}

bool DevDriver::wifiProfileDelete(const QString& id) {
    const bool ok = WifiProfileStore::remove(id);
    if (ok) {
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-profile: %1 removed").arg(id);
        emit wifiProfileListChanged();
    }
    return ok;
}

QStringList DevDriver::wifiSavedNetworks() {
    QStringList list;
    for (const QString& s : idWifi->savedNetworks()) {
        if (!s.isEmpty()) {
            list.append(s);
        }
    }
    return list;
}

int DevDriver::wifiRelaySupport() { return idWifi->relaySupport(); }
bool DevDriver::wifiRelayValid() { return idWifi->relay().valid; }
int DevDriver::wifiRelayMode() { return idWifi->relay().mode; }
int DevDriver::wifiRelayAddr() { return idWifi->relay().addr; }
QString DevDriver::wifiRelayIp() { return idWifi->relay().ip; }
int DevDriver::wifiRelayPort() { return idWifi->relay().rport; }
int DevDriver::wifiRelayLocalPort() { return idWifi->relay().lport; }
bool DevDriver::wifiRelayStatsValid() { return idWifi->relayStats().valid; }
int DevDriver::wifiRelayState() { return idWifi->relayStats().state; }
QString DevDriver::wifiRelayPeerIp() { return idWifi->relayStats().peerIp; }
int DevDriver::wifiRelayPeerPort() { return idWifi->relayStats().peerPort; }
double DevDriver::wifiRelayUpFrames() { return static_cast<double>(idWifi->relayStats().upFrames); }
double DevDriver::wifiRelayUpPackets() { return static_cast<double>(idWifi->relayStats().upPackets); }
double DevDriver::wifiRelayUpDrops() { return static_cast<double>(idWifi->relayStats().upDrops); }
double DevDriver::wifiRelayDownPackets() { return static_cast<double>(idWifi->relayStats().downPackets); }
double DevDriver::wifiRelayDownFrames() { return static_cast<double>(idWifi->relayStats().downFrames); }
double DevDriver::wifiRelayDownDrops() { return static_cast<double>(idWifi->relayStats().downDrops); }
double DevDriver::wifiRelayLocalFrames() { return static_cast<double>(idWifi->relayStats().localFrames); }

int DevDriver::wifiClients() { return idWifi->status().clients; }
int DevDriver::wifiRadioSupport() { return idWifi->radioSupport(); }

QVariantMap DevDriver::wifiRadio() {
    const IDBinWifi::Radio& r = idWifi->radio();
    QVariantMap m;
    m["valid"] = r.valid;
    m["powerDbm"] = r.powerQ / 4.0;
    m["protocols"] = int(r.protocols);
    m["bandwidthMhz"] = r.bandwidth == 2 ? 40 : 20;
    m["powerSave"] = int(r.powerSave);
    m["powerNowDbm"] = r.powerNowQ / 4.0;
    m["phyNow"] = int(r.phyNow);
    m["protocolsNow"] = int(r.protocolsNow);
    m["bandwidthNowMhz"] = r.bandwidthNow == 2 ? 40 : 20;
    return m;
}

int DevDriver::wifiNetSupport() { return idWifiNet->support(); }

QVariantMap DevDriver::wifiNetRole() {
    const IDBinWifiNet::RoleInfo& r = idWifiNet->role();
    QVariantMap m;
    m["valid"] = r.valid;
    m["saved"] = int(r.saved);
    m["running"] = int(r.running);
    return m;
}

QVariantMap DevDriver::wifiNetAp() {
    const IDBinWifiNet::Ap& a = idWifiNet->ap();
    QVariantMap m;
    m["valid"] = a.valid;
    m["channel"] = int(a.channel);
    m["hidden"] = a.hidden != 0;
    m["maxClients"] = int(a.maxClients);
    m["auth"] = int(a.auth);
    m["ssid"] = a.ssid;
    return m;
}

QVariantMap DevDriver::wifiNetIp() {
    const IDBinWifiNet::IpCfg& c = idWifiNet->ipCfg();
    QVariantMap m;
    m["valid"] = c.valid;
    m["ip"] = IDBinWifiNet::addrText(c.ip);
    m["mask"] = IDBinWifiNet::addrText(c.mask);
    m["dhcp"] = c.dhcp;
    m["poolStart"] = IDBinWifiNet::addrText(c.poolStart);
    m["poolEnd"] = IDBinWifiNet::addrText(c.poolEnd);
    m["leaseMin"] = int(c.leaseMin);
    m["offerGateway"] = (c.offer & 1) != 0;
    m["offerDns"] = (c.offer & 2) != 0;
    return m;
}

QVariantList DevDriver::wifiNetLines() {
    QVariantList list;
    for (int i = 0; i < IDBinWifiNet::kLines; ++i) {
        const IDBinWifiNet::Line& l = idWifiNet->line(i);
        QVariantMap m;
        m["line"] = i;
        m["valid"] = l.valid;
        m["mode"] = int(l.mode);
        m["dest"] = int(l.dest);
        m["ip"] = IDBinWifiNet::addrText(l.ip);
        m["rport"] = int(l.rport);
        m["lport"] = int(l.lport);
        m["baud"] = int(l.baud);
        m["txPin"] = int(l.txPin);
        m["rxPin"] = int(l.rxPin);
        m["uart"] = int(l.uart);
        list.append(m);
    }
    return list;
}

QVariantList DevDriver::wifiNetLineStats() {
    static const char* names[12] = { "upFrames", "upBytes", "upPackets", "upDrops", "downPackets", "downBytes",
                                     "downFrames", "downDrops", "localFrames", "tcpConnects", "uartOverflows",
                                     "uartDrops" };
    QVariantList list;
    for (int i = 0; i < IDBinWifiNet::kLines; ++i) {
        const IDBinWifiNet::LineStats& s = idWifiNet->lineStats(i);
        QVariantMap m;
        m["line"] = i;
        m["valid"] = s.valid;
        m["state"] = int(s.state);
        m["peer"] = IDBinWifiNet::addrText(s.peerIp) + QStringLiteral(":") + QString::number(s.peerPort);
        m["senders"] = int(s.senders);
        for (int k = 0; k < 12; ++k)
            m[names[k]] = static_cast<double>(s.counters[k]);
        list.append(m);
    }
    return list;
}

QVariantList DevDriver::wifiNetClients() {
    QVariantList list;
    for (const IDBinWifiNet::Client& c : idWifiNet->clients()) {
        QVariantMap m;
        m["mac"] = c.mac;
        m["ip"] = IDBinWifiNet::addrText(c.ip);
        m["rssi"] = int(c.rssi);
        list.append(m);
    }
    return list;
}

int DevDriver::wifiNetOwnAddr() { return idWifiNet->ownAddress(); }

int DevDriver::wifiNetPortSupport() { return idWifiNet->portSupport(); }

namespace {
// Ages of the port pages are in 0.1 s, 0xFFFF = never: seconds for QML, -1 for never.
double wifiAgeS(uint16_t age) { return age == WifiPorts::kAgeNever ? -1.0 : age / 10.0; }
} // namespace

QVariantList DevDriver::wifiNetPorts() {
    static const char* const protocolNames[WifiPorts::kProtocols] = { "KP1", "KP2", "UBX", "MAVLink 1", "MAVLink 2", "raw" };
    const WifiPorts& p = idWifiNet->ports();
    QVariantList list;
    for (int i = 0; i < WifiPorts::kPorts; ++i) {
        const WifiPorts::Summary& s = p.summary(i);
        QVariantMap m;
        m["port"] = i;
        m["valid"] = s.valid;
        m["flags"] = int(s.flags);
        m["uartRunning"] = (s.flags & WifiPorts::FlagUartRunning) != 0;
        m["bridging"] = (s.flags & WifiPorts::FlagBridging) != 0;
        m["here"] = (s.flags & WifiPorts::FlagAskedHere) != 0;
        m["reports"] = (s.flags & WifiPorts::FlagGetsReports) != 0;
        m["slip"] = (s.flags & WifiPorts::FlagSlip) != 0;
        m["provisional"] = (s.flags & WifiPorts::FlagProvisional) != 0;
        m["usb"] = (s.flags & WifiPorts::FlagUsb) != 0;
        m["baud"] = int(s.baud);
        m["baudSaved"] = int(s.baudSaved);
        m["connected"] = int(s.connected);
        m["rxBytes"] = double(s.rxBytes);
        m["txBytes"] = double(s.txBytes);
        m["rxAge"] = wifiAgeS(s.rxAge);
        m["txAge"] = wifiAgeS(s.txAge);
        m["rxBps"] = s.rxBps;
        m["txBps"] = s.txBps;
        QVariantList protocols;
        for (int k = 0; k < WifiPorts::kProtocols; ++k) {
            QVariantMap pm;
            pm["name"] = QString::fromLatin1(protocolNames[k]);
            pm["fromPort"] = double(s.protocols[k].fromPort);
            pm["toPort"] = double(s.protocols[k].toPort);
            pm["age"] = wifiAgeS(s.protocols[k].ageFrom);
            protocols.append(QVariant(pm));
        }
        m["protocols"] = protocols;
        m["sbpRequestsFrom"] = double(s.sbpRequestsFrom);
        m["sbpContentFrom"] = double(s.sbpContentFrom);
        m["sbpRequestsTo"] = double(s.sbpRequestsTo);
        m["sbpContentTo"] = double(s.sbpContentTo);
        m["moduleRequests"] = double(s.moduleRequests);
        m["moduleFrames"] = double(s.moduleFrames);
        m["rxOverflows"] = double(s.rxOverflows);
        m["txDrops"] = double(s.txDrops);
        list.append(QVariant(m));
    }
    return list;
}

QVariantList DevDriver::wifiNetPortDevices() {
    const WifiPorts& p = idWifiNet->ports();
    QVariantList out;
    for (int i = 0; i < WifiPorts::kPorts; ++i) {
        QVariantList list;
        for (const WifiPorts::Device& d : p.devices(i).list) {
            QVariantMap m;
            m["kind"] = int(d.kind);
            m["age"] = wifiAgeS(d.age);
            if (d.kind == WifiPorts::KindMavlink) {
                m["sysid"] = int(d.addr);
                m["compid"] = int(d.board);
                m["heartbeat"] = (d.flags & WifiPorts::DevHeartbeat) != 0;
                m["mavType"] = int(d.major);
                m["autopilot"] = int(d.minor);
                m["frames"] = double(d.serial);
            } else {
                const bool known = (d.flags & WifiPorts::DevVersionKnown) != 0;
                m["addr"] = int(d.addr);
                m["board"] = int(d.board);
                m["name"] = d.board ? knownBoardName(d.board) : QString();
                m["fw"] = known ? QStringLiteral("%1.%2").arg(int(d.major)).arg(int(d.minor)) : QString();
                m["serial"] = double(d.serial);
            }
            list.append(QVariant(m));
        }
        out.append(QVariant(list));   // one element per port, not the entries themselves
    }
    return out;
}

QVariantList DevDriver::wifiNetPortPeers() {
    const WifiPorts& p = idWifiNet->ports();
    QVariantList out;
    for (int i = 0; i < WifiPorts::kPorts; ++i) {
        const WifiPorts::Network& n = p.network(i);
        QVariantMap m;
        m["valid"] = n.valid;
        m["mode"] = int(n.mode);
        m["state"] = int(n.state);
        QVariantList peers;
        for (const WifiPorts::Peer& peer : n.peers) {
            QVariantMap pm;
            pm["ip"] = IDBinWifiNet::addrText(peer.ip);
            pm["port"] = int(peer.port);
            pm["age"] = wifiAgeS(peer.age);
            peers.append(QVariant(pm));
        }
        m["peers"] = peers;
        out.append(QVariant(m));
    }
    return out;
}

int DevDriver::wifiNetOwnPort() {
    return idWifiNet->portSupport() == 1 ? idWifiNet->ports().askedPort() : -1;
}

QString DevDriver::knownBoardName(int board) {
    switch (board) {
    case BoardEnhanced:
    case BoardNEnhanced:
        return QStringLiteral("2D-Enhanced");
    case BoardChirp:
        return QStringLiteral("2D-Chirp");
    case BoardBase:
    case BoardNBase:
        return QStringLiteral("2D-Base");
    case BoardAssist:
    case BoardRecorderMini:
        return QStringLiteral("Recorder");
    case BoardSideEnhanced:
        return QStringLiteral("Side-Enhanced");
    case BoardDVL:
        return QStringLiteral("DVL");
    case BoardBasic2D:
        return QStringLiteral("Basic2D");
    case BoardNanoSSS:
        return QStringLiteral("NanoSSS");
    case BoardBaikalSonar:
        return QStringLiteral("Baikal-sonar");
    case BoardWiFi:
        return QStringLiteral("Wi-Fi");
    case BoardUSBL:
        return QStringLiteral("USBL");
    case BoardUSBLBeacon:
        return QStringLiteral("Beacon");
    case BoardPULSEred_2D:
        return QStringLiteral("PULSEred 2D DSP");
    case BoardPULSEblue_DSS:
        return QStringLiteral("PULSEblue DSS");
    default:
        return QString();
    }
}

bool DevDriver::wifiSetRadio(double powerDbm, int protocols, int bandwidthMhz, int powerSave) {
    // LR only on an access point: 0.12 takes it (module-to-module links; no phone or laptop can join), 0.11 answers
    // ERR_PAYLOAD, which the page explains. 20 dBm is the driver's top.
    const bool ap = idWifiNet->role().valid && idWifiNet->role().running == 1;
    if (!m_state.connect) return false;
    const int q = qBound(8, int(std::lround(powerDbm * 4.0)), 80);
    const int ps = qBound(0, powerSave, 2);
    // 40 MHz only with 802.11n: the module refuses b, b/g and LR only at 40 MHz (ERR_PAYLOAD), so they go at 20
    const int width = WifiRadioRules::widthFor(protocols, bandwidthMhz);
    idWifi->setRadio(uint8_t(q), uint8_t(protocols), width == 40 ? 2 : 1, uint8_t(ps));
    wifiRadioCheck_->start(q / 4.0, protocols, width, ps, ap);
    return true;
}

bool DevDriver::wifiRadio40Allowed(int protocols) const { return WifiRadioRules::allows40(protocols); }

int DevDriver::wifiSignalLevel() {
    // A silent module's last report is no link (dataStale): the boat's module after the link went.
    if (dataStale_)
        return -1;
    // The report (once a second) is newer than the status (on a change); both carry the state and the RSSI.
    const IDBinWifi::Report& r = idWifi->report();
    const IDBinWifi::Status& st = idWifi->status();
    const int state = r.valid ? r.state : (st.valid ? st.state : -1);
    const int rssi = r.valid ? r.rssi : st.rssi;
    return WifiSignal::level(state == IDBinWifi::StateConnected || state == IDBinWifi::StateAp, rssi);
}

bool DevDriver::wifiRateSavedAtOnce() {
    return WifiNetRules::rateSavedAtOnce(idVersion->fwVersion(), idVersion->fwVersionMinor());
}

int DevDriver::wifiMaxBaud() {
    return WifiRates::maxFor(idVersion->fwVersion(), idVersion->fwVersionMinor());
}

int DevDriver::wifiLrApSupport() {
    return WifiRadioRules::lrOnlyApSupport(idVersion->fwVersion(), idVersion->fwVersionMinor());
}

int DevDriver::wifiLinkBaud() const {
    LinkManagerWrapper* links = core.getLinkManagerWrapperPtr();
    if (!links || linkUuid_.isNull()) return 0;
    const QUuid link = linkUuid_;
    int baud = 0;
    const auto read = [links, link, &baud]() {
        LinkListModel* m = links->getModelPtr();
        if (m && m->containsUuid(link)
            && m->valueForUuid(link, LinkListModel::Roles::LinkType).toInt() == static_cast<int>(LinkType::kLinkSerial))
            baud = m->valueForUuid(link, LinkListModel::Roles::Baudrate).toInt();
    };
    // The link list lives on the GUI thread; the driver does too unless reading runs in its own thread.
    if (links->thread() == QThread::currentThread())
        read();
    else
        QMetaObject::invokeMethod(links, read, Qt::BlockingQueuedConnection);
    return baud;
}

int DevDriver::wifiOwnLine() const {
    if (idWifiNet->portSupport() == 1)
        return idWifiNet->ports().askedPort();
    return wifiLinkBaud() > 0 ? 0 : -1;
}

void DevDriver::wifiKeepFastReports() {
    // Only the module this app's requests enter through a port (v7 says so): one reached through the relay would
    // send its reports over the radio, it keeps its second. Before 0.12 the module cannot be told apart. The module
    // keeps the period until it restarts; ID_FLASH v0 (a port rate change on 0.12-0.14) saves it with the rates.
    const int port = idWifiNet->portSupport() == 1 ? idWifiNet->ports().askedPort() : -1;
    if (wifiFastReport_ || !m_state.connect || port < 0)
        return;
    wifiFastReport_ = true;
    idWifi->setReportPeriod(uint16_t(WifiSignal::kFastReportMs));
    qInfo().noquote() << wifiTag() << QStringLiteral("wifi: link report every %1 ms asked (the module on this app's port X%2)")
                             .arg(WifiSignal::kFastReportMs).arg(port + 1);
}

bool DevDriver::wifiSetLineBaud(int line, int baud) {
    if (!m_state.connect || line < 0 || line > 1 || baud < WifiRates::kMin || baud > WifiRates::kMax || wifiPortBusy()) return false;
    if (line == wifiOwnLine()) {
        // The old rate is the one this app's link runs at now: the ID_UART mirror starts at 115200 and would
        // send the link back somewhere it never was after a failed change.
        const int now = wifiLinkBaud();
        if (linkUuid_.isNull() || now <= 0) return false;
        wifiPortLine_ = line;
        if (!wifiPort_->start(baud, now, wifiRateSavedAtOnce())) return false;
        qInfo().noquote() << wifiTag() << QStringLiteral("wifi-port: set X%1 (this app's port) to %2 (was %3)").arg(line + 1).arg(baud).arg(now);
        return true;
    }
    // Another port: through its line record. Before 0.12 the module ignores line 0's rate there.
    if (idWifiNet->support() != 1 || (line == 0 && idWifiNet->portSupport() != 1)) return false;
    WifiLineEdit edit;
    edit.baud = uint32_t(baud);
    if (!wifiLine_->start(line, edit)) return false;
    qInfo().noquote() << wifiTag() << QStringLiteral("wifi-port: set X%1 to %2 through its line record (not this app's port)").arg(line + 1).arg(baud);
    return true;
}

bool DevDriver::wifiSetLineConfig(int line, const QVariantMap& edits) {
    // Firmware 0.12: the cards of the Ports island. The rate is not taken here (wifiSetLineBaud): for the line of
    // this app's port 0.12 ignores it in v3, and the fresh record carries the rate in effect for the other line.
    if (!m_state.connect || line < 0 || line > 1 || idWifiNet->support() != 1 || wifiPortBusy() || edits.contains(QStringLiteral("baud")))
        return false;
    WifiLineEdit edit;
    bool ok = true;
    const auto number = [&](const char* key, int lo, int hi, auto& field) {
        const QVariant v = edits.value(QString::fromLatin1(key));
        if (!v.isValid())
            return;
        bool okNum = false;
        const int n = v.toInt(&okNum);
        if (!okNum || n < lo || n > hi)
            ok = false;
        else
            field = static_cast<typename std::decay_t<decltype(field)>::value_type>(n);
    };
    number("mode", 0, 2, edit.mode);
    number("dest", 0, 2, edit.dest);
    number("rport", 0, 65535, edit.rport);
    number("lport", 0, 65535, edit.lport);
    if (edits.contains(QStringLiteral("txPin")) || edits.contains(QStringLiteral("rxPin"))) {
        if (line != 1)            // line 0's pins are fixed by the module's build
            return false;
        number("txPin", -1, 127, edit.txPin);
        number("rxPin", -1, 127, edit.rxPin);
    }
    if (edits.contains(QStringLiteral("ip"))) {
        quint32 a = 0;
        if (!IDBinWifiNet::addrValue(edits.value(QStringLiteral("ip")).toString(), &a))
            return false;
        edit.ip = a;
    }
    if (!ok || !wifiLine_->start(line, edit))
        return false;
    qInfo().noquote() << wifiTag() << QStringLiteral("wifi-port: X%1 line settings: %2").arg(line + 1)
                             .arg(QStringList(edits.keys()).join(QStringLiteral(", ")));
    return true;
}

void DevDriver::wifiNetRefresh() {
    if (m_state.connect) idWifiNet->requestAll();
}

void DevDriver::wifiNetRefreshPorts() {
    if (!m_state.connect || idWifiNet->support() != 1 || idWifiNet->portSupport() == 0) return;
    idWifiNet->requestPorts();
    if (idWifiNet->portSupport() != 1) return;
    for (int p = 0; p < WifiPorts::kPorts; ++p) {
        idWifiNet->requestPortPage(uint8_t(p), 1);
        idWifiNet->requestPortPage(uint8_t(p), 2);
    }
    // The cards also carry each line's record and counters (the Lines island is hidden with 0.12).
    idWifiNet->simpleRequest(v3);
    idWifiNet->requestStats();
}

void DevDriver::wifiNetRefreshStats() {
    if (m_state.connect && idWifiNet->support() == 1) idWifiNet->requestStats();
}

void DevDriver::wifiNetRefreshClients() {
    if (m_state.connect && idWifiNet->support() == 1) idWifiNet->requestClients();
}

void DevDriver::wifiNetSetRole(int role, bool reboot, bool forgetLines) {
    if (m_state.connect && (role == 0 || role == 1)) idWifiNet->setRole(uint8_t(role), reboot, forgetLines);
}

bool DevDriver::wifiNetSetAp(int channel, bool hidden, int maxClients, int auth, const QString& ssid, const QString& password) {
    if (!m_state.connect || channel < 1 || channel > 11 || maxClients < 1 || maxClients > 10 || auth < 0 || auth > 2) return false;
    return idWifiNet->setAp(uint8_t(channel), hidden, uint8_t(maxClients), uint8_t(auth), ssid, password);
}

bool DevDriver::wifiNetSetIp(const QString& ip, const QString& mask, bool dhcp, const QString& poolStart,
                             const QString& poolEnd, int leaseMin, bool offerGateway, bool offerDns) {
    if (!m_state.connect || leaseMin < 1 || leaseMin > 2880) return false;
    return idWifiNet->setIpCfg(ip, mask, dhcp, poolStart, poolEnd, uint16_t(leaseMin), offerGateway, offerDns);
}

bool DevDriver::wifiNetSetLine(int line, int mode, int dest, const QString& ip, int rport, int lport, int baud,
                               int txPin, int rxPin) {
    // Line 0's pins are fixed by the module's build; before 0.12 its speed is ignored here too (ID_UART sets it).
    // A line whose speed counts needs a real one, or the module refuses the whole line with ERR_PAYLOAD.
    const bool rateCounts = line == 1 || idWifiNet->portSupport() == 1;
    if (!m_state.connect || line < 0 || line > 1 || rport < 0 || rport > 65535 || lport < 0 || lport > 65535
        || baud < 0 || (rateCounts && (baud < WifiRates::kMin || baud > WifiRates::kMax))
        || txPin < -1 || txPin > 127 || rxPin < -1 || rxPin > 127) return false;
    return idWifiNet->setLine(uint8_t(line), uint8_t(mode), uint8_t(dest), ip, uint16_t(rport), uint16_t(lport),
                              uint32_t(baud), int8_t(txPin), int8_t(rxPin));
}

bool DevDriver::wifiNetSetOwnAddr(int addr) {
    // 0 is where devices sit by default: the module would take their frames (0.11 refuses it); 0.12 refuses 255 too
    const int top = idWifiNet->portSupport() == 1 ? 254 : 255;
    if (!m_state.connect || addr < 1 || addr > top) return false;
    idWifiNet->setOwnAddress(uint8_t(addr));
    return true;
}

void DevDriver::wifiRelayRefresh() {
    if (!m_state.connect || idWifi->relaySupport() == 0) return;
    idWifi->requestRelay();
    idWifi->requestRelayStats();
}

void DevDriver::wifiRelayStatsRefresh() {
    if (m_state.connect && idWifi->relaySupport() == 1) idWifi->requestRelayStats();
}

bool DevDriver::wifiSetRelay(int mode, int addr, const QString& ip, int rport, int lport) {
    if (!m_state.connect) return false;
    if (mode < 0 || mode > 2 || addr < 0 || addr > 255 || rport < 0 || rport > 65535 || lport < 0 || lport > 65535) return false;
    return idWifi->setRelay(static_cast<uint8_t>(mode), static_cast<uint8_t>(addr), ip,
                            static_cast<uint16_t>(rport), static_cast<uint16_t>(lport));
}

void DevDriver::wifiRefresh() {
    if (!m_state.connect) return;
    idWifi->requestAll();
    if (wifiRfSupport() == 1) idWifiRf->requestAll();
}

void DevDriver::wifiScan() {
    if (!m_state.connect || wifiRadioOff()) return;   // refused (ERR_RUNTIME) while the radio is off
    idWifi->scan();
    emit wifiNetworksChanged();
}

bool DevDriver::wifiConnect(const QString& ssid, const QString& password, bool save) {
    if (!m_state.connect) return false;
    return idWifi->connectTo(ssid, password, save);
}

void DevDriver::wifiDisconnect() {
    if (m_state.connect) idWifi->action(IDBinWifi::ActDisconnect);
}

void DevDriver::wifiSetAutoConnect(bool on) {
    if (m_state.connect) idWifi->action(on ? IDBinWifi::ActAutoConnect : IDBinWifi::ActDisconnect);
}

void DevDriver::wifiForget(const QString& ssid) {
    if (m_state.connect) idWifi->action(IDBinWifi::ActForget, ssid);
}

void DevDriver::wifiForgetAll() {
    if (m_state.connect) idWifi->action(IDBinWifi::ActForgetAll);
}

QString DevDriver::netIp() { return idNet->ip(); }
QString DevDriver::netMask() { return idNet->mask(); }
QString DevDriver::netHost() { return idNet->host(); }
QString DevDriver::netMac() { return idNet->mac(); }
bool DevDriver::netDhcp() { return idNet->dhcp(); }
bool DevDriver::netLive() { return idNet->live(); }
bool DevDriver::netValid() { return idNet->valid(); }

bool DevDriver::applyNet(const QString& ip, const QString& mask, const QString& host, bool dhcp) {
    if (!m_state.connect) return false;
    if (!idNet->setNet(ip, mask, host, dhcp)) return false;
    // Read back rather than believe the write: the device validates the three
    // addresses itself and keeps the old ones if they cannot work together.
    idNet->simpleRequest(v0);
    return true;
}

void DevDriver::requestNet() {
    if (m_state.connect) idNet->simpleRequest(v0);
}

int DevDriver::soundSpeed() {
    return idSoundSpeed->getSoundSpeed();
}

void DevDriver::setSoundSpeed(int speed) {
    if(!m_state.connect) return;
    bool is_changed = transBoost() != speed;
    idSoundSpeed->setSoundSpeed(speed);

    if (is_changed) {
        emit sendSoundSpeed(getChannelId(), static_cast<uint32_t>(speed));
        emit soundChanged();
    }
}

float DevDriver::yaw() {
    return idAtt->yaw();
}

float DevDriver::pitch() {
    return idAtt->pitch();
}

float DevDriver::roll() {
    return idAtt->roll();
}

void DevDriver::flashSettings() {
    if(!m_state.connect) return;
    idFlash->flashing();
}

void DevDriver::resetSettings() {
    if(!m_state.connect) return;
    idFlash->erase();
}

void DevDriver::reboot() {
    if(!m_state.connect) return;
    idVersion->reset();
    rebootAtTime_ = QDateTime::currentMSecsSinceEpoch();
    idBoot->reboot();
    m_state.reboot = true;
    emit onReboot();
}

bool DevDriver::isStaleVersionAfterReboot() const {
    return rebootAtTime_ != 0 &&
           QDateTime::currentMSecsSinceEpoch() - rebootAtTime_ < staleVersionGuardMsec;
}

int DevDriver::distMax() {
    return idDistSetup->max();
}
void DevDriver::setDistMax(int dist) {
    if(!m_state.connect) return;
    bool is_changed = dist != distMax();
    idDistSetup->setMax(dist);
    if(is_changed) { emit distSetupChanged(); }
}

int DevDriver::distDeadZone() {
    return idDistSetup->deadZone();
}

void DevDriver::setDistDeadZone(int dead_zone) {
    if(!m_state.connect) return;
    bool is_changed = dead_zone != distDeadZone();
    idDistSetup->setDeadZone(dead_zone);
    if(is_changed) { emit distSetupChanged(); }
}

int DevDriver::distConfidence() {
    return idDistSetup->confidence();
}

void DevDriver::setConfidence(int confidence) {
    if(!m_state.connect) return;
    bool is_changed = confidence != distConfidence();
    idDistSetup->setConfidence(confidence);
    if(is_changed) { emit distSetupChanged(); }
}

int DevDriver::chartSamples() {
    return idChartSetup->count();
}

void DevDriver::setChartSamples(int smpls) {
    if(!m_state.connect) return;
    bool is_changed = smpls != chartSamples();
    idChartSetup->setCount((U2)smpls);
    logChartSetupSent();
    if (is_changed) {
        emit sendChartSetup(getChannelId(), idChartSetup->resolution(), static_cast<uint16_t>(smpls), idChartSetup->offset());
        emit chartSetupChanged();
    }
}

int DevDriver::chartResolution() {
    return idChartSetup->resolution();
}

void DevDriver::setChartResolution(int resol) {
    if(!m_state.connect) return;
    bool is_changed = resol != chartResolution();
    idChartSetup->setResolution((U2)resol);
    logChartSetupSent();
    if (is_changed) {
        emit sendChartSetup(getChannelId(), static_cast<uint16_t>(resol), idChartSetup->count(), idChartSetup->offset());
        emit chartSetupChanged();
    }
}

int DevDriver::chartOffset() {
    return idChartSetup->offset();
}

void DevDriver::setChartOffset(int offset) {
    if(!m_state.connect) return;
    bool is_changed = offset != chartOffset();
    idChartSetup->setOffset((U2)offset);
    logChartSetupSent();
    if (is_changed) {
        emit sendChartSetup(getChannelId(), idChartSetup->resolution(), idChartSetup->count(), static_cast<uint16_t>(offset));
        emit chartSetupChanged();
    }
}

// What went out in SETTING ID_CHART_SETUP, from the values setV0 actually wrote (it may lower the
// resolution). The owner set 20 mm and saw 10 m a side while the board held count 998
// (2026-10-17): without this line nothing showed which side of the link the number came from.
void DevDriver::logChartSetupSent()
{
    const int count = idChartSetup->count();
    const int resol = idChartSetup->resolution();
    const int sides = qMax(1, chartSides_);
    qInfo().noquote() << QStringLiteral("chart setup sent: count %1, resolution %2 mm, offset %3; %4 a side over %5 side(s) = %6 m")
                             .arg(count).arg(resol).arg(idChartSetup->offset())
                             .arg(count / sides).arg(sides)
                             .arg(0.001 * resol * (count / sides), 0, 'f', 2);
}

int DevDriver::dspSmoothFactor() {
    return idDSPSetup->horSmoothFactor();
}

void DevDriver::setDspSmoothFactor(int dsp_smooth) {
    if(!m_state.connect) return;
    bool is_changed = dsp_smooth != dspSmoothFactor();
    idDSPSetup->setHorSmoothFactor((U1)dsp_smooth);
    if(is_changed) { emit dspSetupChanged(); }
}

int DevDriver::datasetTimestamp() {
    int ch_param = 0;
    if(idDataset->getTimestamp_v0(1)) {
        ch_param |= 1;
    }
    if(idDataset->getTimestamp_v0(2)) {
        ch_param |= 2;
    }
    return ch_param;
}

void DevDriver::setDatasetTimestamp(int ch_param) {
    if(!m_state.connect) return;
    bool is_changed = (ch_param != datasetTimestamp());
    idDataset->setTimestamp_v0(ch_param);
    idDataset->commit();
    if(is_changed) { emit datasetChanged(); }
}

int DevDriver::datasetDist() {
    int ch_param = 0;
    if(idDataset->getDist_v0(1)) {
        ch_param |= 1;
    }
    if(idDataset->getDist_v0(2)) {
        ch_param |= 2;
    }
    return ch_param;
}

void DevDriver::setDatasetDist(int ch_param) {
    if(!m_state.connect) return;
    bool is_changed = (ch_param != datasetDist());
    idDataset->setDist_v0(ch_param);
    idDataset->commit();
    if(is_changed) { emit datasetChanged(); }
}

int DevDriver::datasetChart() {
    int ch_param = 0;
    if(idDataset->getChart_v0(1)) {
        ch_param |= 1;
    }
    if(idDataset->getChart_v0(2)) {
        ch_param |= 2;
    }
    return ch_param;
}

void DevDriver::setDatasetChart(int ch_param) {
    if(!m_state.connect) return;
    bool is_changed = (ch_param != datasetChart());
    idDataset->setChart_v0(ch_param);
    idDataset->commit();
    if(is_changed) { emit datasetChanged(); }
}

int DevDriver::datasetChartZ() {
    int ch_param = 0;
    if(idDataset->getChartZ_v0(1)) {
        ch_param |= 1;
    }
    if(idDataset->getChartZ_v0(2)) {
        ch_param |= 2;
    }
    return ch_param;
}

void DevDriver::setDatasetChartZ(int ch_param) {
    if(!m_state.connect) return;
    bool is_changed = (ch_param != datasetChartZ());
    idDataset->setChartZ_v0(ch_param);
    idDataset->commit();
    if(is_changed) { emit datasetChanged(); }
}

int DevDriver::datasetTapIq() {
    int ch_param = 0;
    if(idDataset->getTapIq_v0(1)) {
        ch_param |= 1;
    }
    if(idDataset->getTapIq_v0(2)) {
        ch_param |= 2;
    }
    return ch_param;
}

void DevDriver::setDatasetTapIq(int ch_param) {
    if(!m_state.connect) return;
    bool is_changed = (ch_param != datasetTapIq());
    idDataset->setTapIq_v0(ch_param);
    idDataset->commit();
    if(is_changed) { emit datasetChanged(); }
}

int DevDriver::datasetTapRaw() {
    int ch_param = 0;
    if(idDataset->getTapRaw_v0(1)) {
        ch_param |= 1;
    }
    if(idDataset->getTapRaw_v0(2)) {
        ch_param |= 2;
    }
    return ch_param;
}

void DevDriver::setDatasetTapRaw(int ch_param) {
    if(!m_state.connect) return;
    bool is_changed = (ch_param != datasetTapRaw());
    idDataset->setTapRaw_v0(ch_param);
    idDataset->commit();
    if(is_changed) { emit datasetChanged(); }
}

int DevDriver::datasetTemp() {
    int ch_param = 0;
    if(idDataset->getTemp_v0(1)) {
        ch_param |= 1;
    }
    if(idDataset->getTemp_v0(2)) {
        ch_param |= 2;
    }
    return ch_param;
}

void DevDriver::setDatasetTemp(int ch_param) {
    if(!m_state.connect) return;
    bool is_changed = (ch_param != datasetTemp());
    idDataset->setTemp_v0(ch_param);
    idDataset->commit();
    if(is_changed) { emit datasetChanged();  }
}

int DevDriver::datasetVoltage() {
    int ch_param = 0;
    if(idDataset->getVoltage_v0(1)) {
        ch_param |= 1;
    }
    if(idDataset->getVoltage_v0(2)) {
        ch_param |= 2;
    }
    return ch_param;
}

void DevDriver::setDatasetVoltage(int ch_param) {
    if(!m_state.connect) return;
    bool is_changed = (ch_param != datasetVoltage());
    idDataset->setVoltage_v0(ch_param);
    idDataset->commit();
    if(is_changed) { emit datasetChanged(); }
}

int DevDriver::datasetEuler() {
    int ch_param = 0;
    if(idDataset->getEuler(1)) {
        ch_param |= 1;
    }
    if(idDataset->getEuler(2)) {
        ch_param |= 2;
    }
    return ch_param;
}

void DevDriver::setDatasetEuler(int ch_param) {
    if(!m_state.connect) return;
    bool is_changed = (ch_param != datasetEuler());
    idDataset->setEuler(ch_param);
    idDataset->commit();
    if(is_changed) { emit datasetChanged();  }
}

int DevDriver::datasetSDDBT() {
    int ch_param = 0;
    if(idDataset->getSDDBT(1)) {
        ch_param |= 1;
    }
    if(idDataset->getSDDBT(2)) {
        ch_param |= 2;
    }
    return ch_param;
}

void DevDriver::setDatasetSDDBT(int ch_param) {
    if(!m_state.connect) return;
    bool is_changed = (ch_param != datasetSDDBT());
    idDataset->setSDDBT(ch_param);
    idDataset->commit();
    if(is_changed) {  emit datasetChanged(); }
}

int DevDriver::datasetSDDBT_P2() {
    int ch_param = 0;
    if(idDataset->getSDDBT_P2(1)) {
        ch_param |= 1;
    }
    if(idDataset->getSDDBT_P2(2)) {
        ch_param |= 2;
    }
    return ch_param;
}

void DevDriver::setDatasetSDDBT_P2(int ch_param) {
    if(!m_state.connect) return;
    bool is_changed = (ch_param != datasetSDDBT_P2());
    idDataset->setSDDBT_P2(ch_param);
    idDataset->commit();
    if(is_changed) {  emit datasetChanged(); }
}


int DevDriver::ch1Period() {
    return (int)idDataset->period(1);
}

void DevDriver::setCh1Period(int period) {
    if(!m_state.connect) return;
    bool is_changed = (period != ch1Period());
    idDataset->setPeriod(1, (uint)period);
    if(is_changed) {  emit datasetChanged();  }
}

int DevDriver::ch2Period() {
    return (int)idDataset->period(2);
}

void DevDriver::setCh2Period(int period) {
    if(!m_state.connect) return;
    bool is_changed = (period != ch2Period());
    idDataset->setPeriod(2, (uint)period);
    if(is_changed) {  emit datasetChanged();  }
}

void DevDriver::receivedTimestamp(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type);
    Q_UNUSED(ver);
    Q_UNUSED(resp);
}

void DevDriver::receivedDist(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);
    Q_UNUSED(resp);

    emit distComplete(getChannelId(), idDist->dist_mm());
}

// Once a minute while echogram frames come: how many pings came out whole, and what the compressed paths lost --
// a KEC stream with a slice missing, a whole KEC stream the decoder refused (a format it does not know: GODIO's
// feature-block KEC before src/kec was updated), echocodec blocks taken from the previous ping. Called on every
// chart frame, whole or not, so a stream that never decodes is still reported.
void DevDriver::logChartHealth()
{
    if (!chartLogClock_.isValid()) {
        chartLogClock_.start();
        return;
    }
    if (chartLogClock_.elapsed() < 60000) {
        return;
    }
    const double s = chartLogClock_.restart() / 1000.0;
    const uint32_t holed = idChartZ->takeDropped(), undecoded = idChartZ->takeUndecoded();
    const uint32_t v3 = idChart->takeV3Pings(), filled = idChart->takeV3Filled();
    qInfo().noquote() << QStringLiteral("[%1] echogram: %2 pings in %3 s; ID_CHART_Z %4 dropped for a missing slice, "
                                        "%5 the decoder refused; ID_CHART V3 %6 pings, %7 blocks from the previous ping")
                             .arg(m_busAddress).arg(chartPings_).arg(s, 0, 'f', 1).arg(holed).arg(undecoded).arg(v3).arg(filled);
    chartPings_ = 0;
}

void DevDriver::receivedChart(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type);
    Q_UNUSED(resp);

    bool isCompleteChart = idChart->readAndClearIsCompleteChart();
    if (isCompleteChart) {
        ++chartPings_;
    }
    logChartHealth();

    if (!isCompleteChart || (ver != v0 && ver != v1 && ver != v3)) {   // v3: echocodec, one side
        return;
    }

    const int sides = (ver == v1) ? 2 : 1;
    if (sides != chartSides_) {
        chartSides_ = sides;
        emit chartSidesChanged();
    }

    // connection quality
    if (errorFreezeCnt_ > 100) {
        averageChartLosses_ = idChart->getAverageLosses();
        emit averageChartLossesChanged();
        errorFreezeCnt_ = 0;
    }
    ++errorFreezeCnt_;

    const int size = idChart->chartSize();
    // An empty chart carries nothing and would only open an epoch of its own, putting every later
    // chart one epoch off its heading (IDBinChart::parsePayload once produced one on connecting).
    if (size <= 0) {
        return;
    }
    QVector<QVector<uint8_t>> data(1); // always size == 1 or 2 for chart logic *1

    if (idChart->logData8()) {
        data[0].resize(size);
        memcpy(data[0].data(), idChart->logData8(), size);
    }

    if (ver == v1) {
        data.resize(2); // *2

        if (idChart->logData28()) {
            data[1].resize(size);
            memcpy(data[1].data(), idChart->logData28(), idChart->chartSize());
        }
    }

    ChannelId channelId(linkUuid_, lastAddress_);
    ChartParameters chartParams(idVersion->boardVersion(), ver, idChart->getErrList());
    float resolution = 0.001f * idChart->resolution();
    float offsetRange = 0.001f * idChart->offsetRange();

    emit chartComplete(channelId, chartParams, data, resolution, offsetRange);
}

void DevDriver::receivedRaw(RawData raw_data) {
    emit rawDataRecieved(ChannelId(linkUuid_, lastAddress_), raw_data);
}

void DevDriver::resetPresets() {
    presetsAnswered_ = false;
    presetsUnsupported_ = false;
    presetAsks_ = 0;
    presetAskedAt_ = 0;
}

void DevDriver::receivedPreset(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    if (resp == respNone) {
        // A slot's frame: the eight after a request, or the one a save sends back.
        const int slot = idPreset->lastSlot();
        if (slot < 0) {
            return;
        }
        if (!presetsAnswered_) {
            presetsAnswered_ = true;
            qInfo().noquote() << QStringLiteral("preset: the sonar keeps presets (ID_PRESET)");
        }
        const IDBinPreset::Slot& s = idPreset->slot(slot);
        // The name last: it comes from the sonar, and an .arg() after it would fill a "%1" in it.
        const QString what = s.used
            ? QStringLiteral("%1 kHz, %2 pulses, DSP %3, %4 cells of %5 mm, offset %6, period %7 ms%8, \"")
                  .arg(s.freqKhz).arg(s.pulses).arg(s.dsp).arg(s.cells).arg(s.cellMm).arg(s.offset).arg(s.periodMs)
                  .arg(s.factory ? QStringLiteral(", factory") : QString()) + s.name + QStringLiteral("\"")
            : QStringLiteral("empty");
        qInfo().noquote() << QStringLiteral("preset: slot %1: ").arg(slot) + what;
        emit presetsChanged();
        return;
    }
    if (resp == respErrorID) {
        presetsUnsupported_ = true;   // a sonar before presets: not asked again on this connection
        return;
    }
    if (resp != respOk) {
        qInfo().noquote() << QStringLiteral("preset: SETTING v%1 refused, code %2").arg(int(ver)).arg(int(resp));
        // The message from here, once an answer: QML has a frequency pill per device view.
        const QString what = ver == v1 ? tr("The preset was not saved")
                           : ver == v2 ? tr("The preset was not deleted")
                           : tr("The preset was not applied");
        const QString why = resp == respErrorCheck ? tr("the sonar's flash did not take it")
                          : resp == respErrorPayload ? tr("there is no such preset in the sonar")
                          : resp == respErrorKey ? tr("the sonar asked for the key")
                          : tr("code %1").arg(int(resp));
        notifyWarning(what + QStringLiteral(": ") + why);
        emit presetResult(int(ver), int(resp));
    }
}

QVariantList DevDriver::presetSlots() {
    QVariantList list;
    if (!presetsAnswered_) {
        return list;
    }
    for (int i = 0; i < IDBinPreset::kSlots; ++i) {
        const IDBinPreset::Slot& s = idPreset->slot(i);
        list.append(QVariantMap{
            { QStringLiteral("slot"), i }, { QStringLiteral("known"), s.known }, { QStringLiteral("used"), s.used },
            { QStringLiteral("factory"), s.factory }, { QStringLiteral("name"), s.name },
            { QStringLiteral("freqKhz"), int(s.freqKhz) }, { QStringLiteral("pulses"), int(s.pulses) },
            { QStringLiteral("boost"), int(s.boost) }, { QStringLiteral("dsp"), int(s.dsp) },
            { QStringLiteral("cells"), int(s.cells) }, { QStringLiteral("cellMm"), int(s.cellMm) },
            { QStringLiteral("offset"), int(s.offset) }, { QStringLiteral("periodMs"), qint64(s.periodMs) } });
    }
    return list;
}

void DevDriver::presetRecall(int slot) {
    if (!m_state.connect || !presetsAnswered_ || slot < 0 || slot >= IDBinPreset::kSlots) {
        return;
    }
    qInfo().noquote() << QStringLiteral("preset: recall slot %1 \"%2\"").arg(slot).arg(idPreset->slot(slot).name);
    idPreset->recall(slot);
}

void DevDriver::presetSave(int slot, const QString& name) {
    if (!m_state.connect || !presetsAnswered_ || slot < 0 || slot >= IDBinPreset::kSlots) {
        return;
    }
    qInfo().noquote() << QStringLiteral("preset: save the sonar's settings into slot %1 as \"%2\"")
                         .arg(slot).arg(QString::fromUtf8(IDBinPreset::nameBytes(name)));
    idPreset->save(slot, name);
}

void DevDriver::presetDelete(int slot) {
    if (!m_state.connect || !presetsAnswered_ || slot < 0 || slot >= IDBinPreset::kSlots) {
        return;
    }
    qInfo().noquote() << QStringLiteral("preset: delete slot %1 \"%2\"").arg(slot).arg(idPreset->slot(slot).name);
    idPreset->remove(slot);
}

void DevDriver::receivedAtt(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);
    Q_UNUSED(resp);

    const float yaw = idAtt->yaw();
    const float pitch = idAtt->pitch();
    const float roll = idAtt->roll();

    if (!qFuzzyIsNull(yaw) || !qFuzzyIsNull(pitch) || !qFuzzyIsNull(roll)) {
        emit attitudeComplete(yaw, idAtt->pitch(), idAtt->roll(), AttitudeArbiter::KindDeviceImu, attitudeSource());
    }

    emit servoCurrentAngleChanged();
}

void DevDriver::receivedTemp(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);
    Q_UNUSED(resp);

    emit tempComplete(idTemp->temp());
    //core.getDatasetPtr()->addTemp(idTemp->temp());
}

/* ID_VOLTAGE 0x31. The class has existed for a long time but was never registered, so every
 * one of these frames was silently dropped. Channel 0 is the supply rail in millivolts. The
 * nanoSSS v4 firmware also uses the spare channels as the only diagnostic path that reaches this
 * application at all -- it has no parser for ID_IMU or ID_MAG -- so they are named here instead
 * of being shown as nonsensical voltages. */
void DevDriver::receivedVoltage(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);
    Q_UNUSED(resp);

    const quint8 ch = idVoltage->lastId();
    const qint32 raw = idVoltage->lastRaw();
    emit voltageComplete(ch, raw);
    if (ch <= 1) {
        (ch == 0 ? voltage0Mv_ : voltage1Mv_) = raw;
        emit voltagesChanged();
    }
    if (ch == 0) {
        emit supplyVoltageComplete(raw / 1000.0f);
    }

    QString line;
    switch (ch) {
    case 0: line = QString("Supply: %1 V").arg(raw / 1000.0, 0, 'f', 3); break;
    case 1: line = QString("IMU id: 0x%1%2").arg(raw, 2, 16, QChar('0'))
                       .arg(raw == 0x47 ? " (ICM-42688-P)" : " (not the expected 0x47)"); break;
    case 2: line = QString("Magnetometer id: 0x%1%2").arg(raw, 2, 16, QChar('0'))
                       .arg(raw == 0x30 ? " (MMC5983MA)" : " (not the expected 0x30)"); break;
    case 3: line = QString("Raw ADC, temperature node: %1").arg(raw); break;
    case 4: line = QString("Raw ADC, supply divider: %1").arg(raw); break;
    default: line = QString("Voltage channel %1: %2").arg(ch).arg(raw); break;
    }
    /* These arrive every ping. Report each channel the first time it is seen, and the supply
       again only when it has actually moved, so the console stays readable. */
    const bool first = !loggedVoltageChannels_.contains(ch);
    const bool supplyMoved = (ch == 0) && (qAbs(raw - lastSupplyLogged_) > 100);
    if (first || supplyMoved) {
        loggedVoltageChannels_.insert(ch);
        if (ch == 0) lastSupplyLogged_ = raw;
        core.consoleInfo(line);
    }
}

void DevDriver::receivedEncoder(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);
    Q_UNUSED(resp);
    IDBinEncoder::Encoder data = idEncoder->get();
    qDebug() << "encoder: " << data.e1 << data.e2 << data.e3;
    emit encoderComplete(data.e1, data.e2, data.e3);
}

void DevDriver::receivedDataset(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);

    if(resp == respNone) { emit datasetChanged(); } else {  }
}

void DevDriver::receivedDistSetup(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);

    if(resp == respNone) {   emit distSetupChanged();  }
}

void DevDriver::receivedServoControl(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);

    if (ver == v1 || ver == v2) {   // the PWM frequency, the step source
        if (resp == respNone) {
            if (ver == v1) {
                qInfo().noquote() << wifiTag() << QStringLiteral("servo: PWM %1 Hz").arg(idServoControl->pwmHz());
            } else {
                qInfo().noquote() << wifiTag() << QStringLiteral("servo: steps on kind %1, function %2, address %3")
                                                      .arg(idServoControl->trigKind()).arg(idServoControl->trigFunction())
                                                      .arg(idServoControl->trigAddress());
            }
            emit servoControlChanged();
        } else if (resp != respOk) {
            qInfo().noquote() << wifiTag() << QStringLiteral("servo: ID_SERVO_CONTROL v%1 (%2) answered %3")
                                                  .arg(int(ver)).arg(ver == v1 ? QStringLiteral("PWM frequency")
                                                                               : QStringLiteral("step source"))
                                                  .arg(int(resp));
        }
        return;
    }

    if (resp == respNone) {
        if (idServoControl->manual() && servoManualSupport_ != 1) {
            servoManualSupport_ = 1;
        }
        if (servoManualSent_ > 0) {   // a read-back after manual mode was used on this connection
            qInfo().noquote() << wifiTag() << QStringLiteral("servo: the device reports manual %1 at %2, scan center %3; "
                                                             "manual frames sent %4, settings answered %5")
                                                  .arg(idServoControl->manual() ? 1 : 0)
                                                  .arg(idServoControl->manualDeg() / double(IDBinServoControl::AngleScale), 0, 'f', 2)
                                                  .arg(idServoControl->scanCenterDeg() / double(IDBinServoControl::AngleScale), 0, 'f', 2)
                                                  .arg(servoManualSent_).arg(servoAnswered_);
        }
        emit servoControlChanged();
        return;
    }
    if (resp == respOk) {
        ++servoAnswered_;
    }
    // A refused setting while manual mode is asked for and the device has never shown it: a firmware without
    // manual mode, which refuses bit 1 of general this way. It never left the scan, so nothing is sent back.
    if (resp == respErrorPayload && idServoControl->manual() && servoManualSupport_ != 1) {
        servoManualSupport_ = -1;
        stopServoManualDrag();
        idServoControl->dropManual();
        qInfo().noquote() << wifiTag() << "servo: manual mode refused, the firmware has none";
        emit servoControlChanged();
    }
}

void DevDriver::resetPorts() {
    idBus->reset();
    idPin->reset();
    portsSupport_ = 0;
    portsAnswerTimer_.stop();
    portOpTimer_.stop();
    portOp_ = PortOpNone;
    portQueue_.clear();
    portSupport_ = {};
    portState_ = PortIdle;
    portOpBus_ = -1;
    emit portSettingsChanged();
}

// The next frame of the table: the buses 1..count-1, then each listed pin; one at a time.
void DevDriver::askNextPort() {
    const auto& buses = idBus->buses();
    for (int b = 0; b < idBus->count(); ++b) {
        if (!buses.contains(b)) {
            idBus->request(U1(b));
            portsAnswerTimer_.start();
            return;
        }
    }
    for (const IDBinBus::Bus& bus : buses) {
        for (U1 pin : bus.pins) {
            if (!idPin->pins().contains(pin)) {
                idPin->request(pin);
                portsAnswerTimer_.start();
                return;
            }
        }
    }
    portsAnswerTimer_.stop();
    if (portsSupport_ != 1) {
        portsSupport_ = 1;
        QStringList parts;
        for (const IDBinBus::Bus& bus : buses) {
            parts.append(QStringLiteral("%1 f%2 u%3 s%4").arg(bus.label).arg(bus.function).arg(bus.uart).arg(bus.supply));
        }
        qInfo().noquote() << wifiTag() << QStringLiteral("ports: %1 buses, %2 pins: %3")
                                              .arg(buses.size()).arg(idPin->pins().size()).arg(parts.join(QStringLiteral(", ")));
        if (portsLiveWanted_) {
            portsLiveTimer_.start();
        }
        emit portsChanged();

        portQueue_.clear();
        int caps = 0;
        for (const IDBinBus::Bus& bus : buses) {
            caps |= bus.functionCaps;
        }
        for (int f = 1; f <= 4; ++f) {
            if (caps & (1 << f)) {
                portQueue_.append(f);
            }
        }
        if (portOp_ == PortOpNone && !portQueue_.isEmpty()) {
            portOp_ = PortOpReadFunctions;
            readNextFunction();
        }
    }
    emit portValuesChanged();
}

QVector<int> DevDriver::uartBuses() const {
    QVector<int> out;
    for (const IDBinBus::Bus& b : idBus->buses()) {
        if (b.functionCaps & 0x1E) {   // the UART functions 1..4
            out.append(b.id);
        }
    }
    return out;
}

int DevDriver::busOfFunction(int function) const {
    for (const IDBinBus::Bus& b : idBus->buses()) {
        if ((b.functionCaps & 0x1E) && b.function == function) {
            return b.id;
        }
    }
    return -1;
}

int DevDriver::portSettable() const {
    if (portOp_ == PortOpReadFunctions) {   // known once every function has answered or not
        return 0;
    }
    bool asked = false;
    for (int f = 1; f <= 4; ++f) {
        if (portSupport_[f] == 1) {
            return 1;
        }
        asked = asked || portSupport_[f] != 0;
    }
    return asked ? -1 : 0;
}

QVariantList DevDriver::portFunctions() const {
    QVariantList list;
    for (int f = 0; f <= 4; ++f) {
        const IDBinBus::FunctionSettings& s = idBus->functionSettings(f);
        QVariantMap m;
        m[QStringLiteral("support")] = f ? portSupport_[f] : 0;
        m[QStringLiteral("valid")] = f && s.valid;
        m[QStringLiteral("baud")] = qint64(s.baud);
        list.append(m);
    }
    return list;
}

QVariantList DevDriver::portBauds() const {
    QVariantList list;
    for (U4 b : IDBinBus::functionBauds()) {
        list.append(int(b));
    }
    return list;
}

void DevDriver::setLinkSerial(bool on) {
    if (linkSerial_ != on) {
        linkSerial_ = on;
        emit portSettingsChanged();
    }
}

void DevDriver::readNextFunction() {
    if (portQueue_.isEmpty()) {
        portOpTimer_.stop();
        portOp_ = PortOpNone;
        QStringList parts;
        for (int f = 1; f <= 4; ++f) {
            parts.append(QStringLiteral("f%1 %2 baud %3").arg(f).arg(portSupport_[f]).arg(idBus->functionSettings(f).baud));
        }
        qInfo().noquote() << wifiTag() << QStringLiteral("ports: settings %1").arg(parts.join(QStringLiteral(", ")));
        emit portSettingsChanged();
        emit portValuesChanged();
        return;
    }
    portOpFunction_ = portQueue_.takeFirst();
    idBus->requestFunction(U1(portOpFunction_));
    portOpTimer_.start();
}

void DevDriver::readNextBus() {
    if (portQueue_.isEmpty()) {
        if (portOpMoves_) {
            const IDBinBus::Bus b = idBus->buses().value(portOpBus_);
            portResult_ = b.function == portOpFunction_ ? PortSaved : PortRefusedValue;
        }
        qInfo().noquote() << wifiTag() << QStringLiteral("ports: read back, result %1").arg(portResult_);
        emit portValuesChanged();
        portDone(portResult_);
        return;
    }
    idBus->request(U1(portQueue_.takeFirst()));
    portOpTimer_.start();
}

void DevDriver::portDone(int state) {
    portOpTimer_.stop();
    portOp_ = PortOpNone;
    portQueue_.clear();
    portState_ = state;
    emit portSettingsChanged();
}

void DevDriver::setPortFunction(int bus, int function) {
    const IDBinBus::Bus b = idBus->buses().value(bus);
    if (portSettable() != 1 || portOp_ != PortOpNone || !idBus->buses().contains(bus) || function < 1 || function > 4
        || !(b.functionCaps & (1 << function)) || b.function == function) {
        return;
    }
    U1 uart = b.uart;
    for (int u = 0; !uart && u < 8; ++u) {   // the bus's own UART: its one UART caps bit when it reports none
        if (b.uartCaps & (1 << u)) {
            uart = U1(u + 1);
        }
    }
    portOp_ = PortOpSetFunction;
    portOpMoves_ = true;
    portOpBus_ = bus;
    portOpFunction_ = function;
    portState_ = PortSending;
    idBus->setBusFunction(U1(bus), U1(function), uart);
    portOpTimer_.start();
    qInfo().noquote() << wifiTag() << QStringLiteral("ports: function %1 onto %2 (was %3; %4 link)")
                                          .arg(function).arg(b.label).arg(b.function).arg(linkSerial_ ? "serial" : "network");
    emit portSettingsChanged();
}

void DevDriver::setPortBaud(int function, int baud) {
    if (portSettable() != 1 || portOp_ != PortOpNone || function < 1 || function > 4 || portSupport_[function] != 1
        || baud < 0 || !IDBinBus::functionBauds().contains(U4(baud)) || idBus->functionSettings(function).baud == U4(baud)) {
        return;
    }
    portOp_ = PortOpSetBaud;
    portOpMoves_ = false;
    portOpFunction_ = function;
    portOpBus_ = busOfFunction(function);
    portWanted_ = U4(baud);
    portState_ = PortSending;
    idBus->setFunctionBaud(U1(function), portWanted_);
    portOpTimer_.start();
    qInfo().noquote() << wifiTag() << QStringLiteral("ports: function %1 rate %2 (was %3; %4 link)")
                                          .arg(function).arg(baud).arg(idBus->functionSettings(function).baud)
                                          .arg(linkSerial_ ? "serial" : "network");
    emit portSettingsChanged();
}

// A SETTING's answer: 1 RespOk, 3 a bad value, 8 the change would touch the UART it came through; any other code (a
// firmware without the setting) is taken as a refusal of the value.
void DevDriver::portSetAnswered(Parsers::Resp resp) {
    qInfo().noquote() << wifiTag() << QStringLiteral("ports: the setting answered %1").arg(int(resp));
    if (resp == respOk) {
        portOp_ = PortOpSave;
        idFlash->flashing();
        portOpTimer_.start();
    } else {
        portDone(resp == respErrorRuntime ? PortRefusedLink : PortRefusedValue);
    }
}

// An ID_BUS frame while an exchange is out; false: not the exchange's, the table's (receivedBus goes on with it).
// The firmware acks a GET after its CONTENT: that RespOk never ends a read, the CONTENT does.
bool DevDriver::portAnswer(Parsers::Version ver, Parsers::Resp resp) {
    const Version fver = IDBinBus::functionVersion(portOpFunction_);
    switch (portOp_) {
    case PortOpReadFunctions:
    case PortOpReadBack:
        if (ver != fver) {
            return false;
        }
        if (resp == respOk) {
            return true;
        }
        if (portOp_ == PortOpReadFunctions) {
            portSupport_[portOpFunction_] = resp == respNone && idBus->functionSettings(portOpFunction_).valid ? 1 : -1;
            readNextFunction();
            return true;
        }
        if (resp != respNone) {
            portDone(PortNoAnswer);
            return true;
        }
        portResult_ = idBus->functionSettings(portOpFunction_).baud == portWanted_ ? PortSaved : PortRefusedValue;
        portOp_ = PortOpReadBuses;
        portQueue_.clear();
        if (busOfFunction(portOpFunction_) >= 0) {
            portQueue_.append(busOfFunction(portOpFunction_));   // its connector: the rate in use
        }
        emit portSettingsChanged();
        readNextBus();
        return true;
    case PortOpSetFunction:
        if (ver != v0 || resp == respNone) {
            return false;
        }
        portSetAnswered(resp);
        return true;
    case PortOpSetBaud:
        if (ver != fver || resp == respNone) {
            return false;
        }
        portSetAnswered(resp);
        return true;
    case PortOpReadBuses:
        if (ver != v0) {
            return false;
        }
        if (resp == respOk) {
            return true;
        }
        if (resp != respNone) {
            portDone(PortNoAnswer);
            return true;
        }
        readNextBus();
        return true;
    default:
        return false;
    }
}

void DevDriver::receivedBus(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);

    if (portOp_ != PortOpNone && portOp_ != PortOpSave && portAnswer(ver, resp)) {
        return;
    }
    if (resp == respNone) {
        askNextPort();
        return;
    }
    if (resp == respErrorID && idBus->buses().isEmpty()) {
        portsAnswerTimer_.stop();
        portsSupport_ = -1;
        qInfo().noquote() << wifiTag() << "ports: ID_BUS unknown to this firmware, no port table";
        emit portsChanged();
    }
}

void DevDriver::receivedPin(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);

    if (resp != respNone) {
        return;
    }
    if (ver == v0) {
        askNextPort();
    } else {
        emit portValuesChanged();
    }
}

QVariantList DevDriver::portBuses() {
    QVariantList list;
    if (portsSupport_ != 1) {
        return list;
    }
    for (const IDBinBus::Bus& b : idBus->buses()) {
        QVariantMap m;
        m[QStringLiteral("id")] = int(b.id);
        m[QStringLiteral("label")] = b.label;
        m[QStringLiteral("supply")] = int(b.supply);
        m[QStringLiteral("function")] = int(b.function);
        m[QStringLiteral("functionSaved")] = int(b.functionSaved);
        m[QStringLiteral("functionCaps")] = int(b.functionCaps);
        m[QStringLiteral("uart")] = int(b.uart);
        m[QStringLiteral("uartSaved")] = int(b.uartSaved);
        m[QStringLiteral("uartCaps")] = int(b.uartCaps);
        m[QStringLiteral("txPin")] = int(b.txPin);
        m[QStringLiteral("rxPin")] = int(b.rxPin);
        m[QStringLiteral("baud")] = double(b.baud);
        QVariantList pins;
        for (U1 id : b.pins) {
            const auto it = idPin->pins().constFind(id);
            if (it == idPin->pins().cend()) {
                continue;
            }
            QVariantMap p;
            p[QStringLiteral("id")] = int(it->id);
            p[QStringLiteral("label")] = it->label;
            p[QStringLiteral("mcuPin")] = it->mcuPin;
            p[QStringLiteral("roleCaps")] = double(it->roleCaps);
            p[QStringLiteral("role")] = int(it->role);
            p[QStringLiteral("roleSaved")] = int(it->roleSaved);
            p[QStringLiteral("timer")] = int(it->timer);
            p[QStringLiteral("param")] = int(it->param);
            p[QStringLiteral("value")] = int(it->value);
            pins.append(p);
        }
        m[QStringLiteral("pins")] = pins;
        list.append(m);
    }
    return list;
}

QStringList DevDriver::portPwmOutPins() {
    QStringList outs{ QString(), QString(), QString() };
    if (portsSupport_ != 1) {
        return outs;
    }
    for (const IDBinPin::Pin& p : idPin->pins()) {
        if (p.role == IDBinPin::RolePwm && p.param < 3) {
            outs[p.param] += (outs[p.param].isEmpty() ? QString() : QStringLiteral(", ")) + p.label;
        }
    }
    return outs;
}

void DevDriver::setPortsLive(bool on) {
    portsLiveWanted_ = on;
    if (on && portsSupport_ == 1) {
        if (!portsLiveTimer_.isActive()) {
            portsLiveTimer_.start();
        }
    } else {
        portsLiveTimer_.stop();
    }
}

QVariantMap DevDriver::ringStat() {
    const IDBinRingStat::State& s = idRingStat->state();
    QVariantMap m;
    m[QStringLiteral("known")] = s.known;
    m[QStringLiteral("address")] = int(s.address);
    m[QStringLiteral("role")] = int(s.role);
    m[QStringLiteral("upstream")] = s.upstream != 0;
    m[QStringLiteral("guardMs")] = s.guardUs / 1000.0;
    m[QStringLiteral("breakIntervalMs")] = s.breakIntervalUs / 1000.0;
    m[QStringLiteral("relayed")] = double(s.relayed);
    m[QStringLiteral("breaks")] = double(s.breaks);
    m[QStringLiteral("torn")] = double(s.torn);
    m[QStringLiteral("timeouts")] = double(s.timeouts);
    m[QStringLiteral("skipped")] = double(s.skipped);
    m[QStringLiteral("error")] = ringStatError_;
    return m;
}

void DevDriver::ringStatRead() {
    ringStatError_.clear();
    idRingStat->read();
    ringStatTimer_.start();
    emit ringStatChanged();
}

void DevDriver::ringStatReset() {
    ringStatError_.clear();
    idRingStat->reset();
    ringStatTimer_.start();
    emit ringStatChanged();
}

void DevDriver::receivedRingStat(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);

    if (resp == respOk) {
        return;   // the reset taken: its state frame follows
    }
    ringStatTimer_.stop();
    if (resp == respNone) {
        const IDBinRingStat::State& s = idRingStat->state();
        ringStatError_.clear();
        // a leader sends the BREAKs and does not time them: it reports an interval of 0. "torn" is a relayed frame
        // lost: cut by a BREAK up to 1.16, dropped for want of room in the relay buffer from 1.17
        const QString interval = s.breakIntervalUs ? QStringLiteral("%1 ms").arg(s.breakIntervalUs / 1000.0, 0, 'f', 2)
                                                   : QStringLiteral("not measured");
        qInfo().noquote() << wifiTag() << QStringLiteral("ring-stat: address %1, %2%3, guard %4 ms, BREAK interval %5; "
                                                         "relayed %6, BREAKs %7, lost passing on %8, leads on a timeout %9, skipped %10")
                                              .arg(s.address).arg(s.role ? QStringLiteral("follower") : QStringLiteral("leader"))
                                              .arg(s.upstream ? QStringLiteral(" (a board before it)") : QString())
                                              .arg(s.guardUs / 1000.0, 0, 'f', 2).arg(interval)
                                              .arg(s.relayed).arg(s.breaks).arg(s.torn).arg(s.timeouts).arg(s.skipped);
    } else {
        // RespErrorID: a firmware that answers an unknown id with an error. GODIO before 1.16 stays silent instead
        // (only a SETTING gets a RESP there), so for it ringStatSupported's version gate decides, and silence from a
        // 1.16+ board is the timeout's "no answer".
        ringStatError_ = resp == respErrorID ? tr("This firmware has no ring counters")
                                             : tr("Refused by the board (code %1)").arg(int(resp));
        qInfo().noquote() << wifiTag() << QStringLiteral("ring-stat: refused, code %1").arg(int(resp));
    }
    emit ringStatChanged();
}

namespace {
S2 servoDegToS2(double deg) {
    return static_cast<S2>(qBound(-32768.0, std::round(deg * IDBinServoControl::AngleScale), 32767.0));
}
}

bool DevDriver::servoManual() const {
    return idServoControl && idServoControl->manual();
}

void DevDriver::setServoManual(bool on) {
    if (!idServoControl || on == idServoControl->manual()) {
        return;
    }
    stopServoManualDrag();
    idServoControl->setManual(on);
    ++servoManualSent_;
    qInfo().noquote() << wifiTag() << QStringLiteral("servo: manual %1, center_deg %2")
                                          .arg(on ? "on" : "off").arg(idServoControl->centerDeg() / double(IDBinServoControl::AngleScale), 0, 'f', 2);
    emit servoControlChanged();
    servoReadbackTimer_.start();
}

double DevDriver::servoManualDeg() const {
    return idServoControl ? idServoControl->manualDeg() / double(IDBinServoControl::AngleScale) : 0.0;
}

void DevDriver::servoManualMove(double deg) {
    if (!idServoControl || !idServoControl->manual()) {
        return;
    }
    servoReadbackTimer_.stop();   // an answer from before the drag would pull the dial back
    servoManualPending_ = deg;
    servoManualDirty_ = true;
    if (!servoManualTimer_.isActive()) {
        flushServoManual();
    }
}

void DevDriver::servoManualSet(double deg) {
    if (!idServoControl || !idServoControl->manual()) {
        return;
    }
    stopServoManualDrag();
    idServoControl->setManualDeg(servoDegToS2(deg));
    ++servoManualSent_;
    emit servoControlChanged();
    servoReadbackTimer_.start();
}

void DevDriver::flushServoManual() {
    if (!servoManualDirty_ || !idServoControl) {
        return;
    }
    servoManualDirty_ = false;
    idServoControl->setManualDeg(servoDegToS2(servoManualPending_));
    ++servoManualSent_;
    servoManualTimer_.start();
    emit servoControlChanged();
}

void DevDriver::stopServoManualDrag() {
    servoManualTimer_.stop();
    servoManualDirty_ = false;
}

// The probe's answer, and the only thing the app ever learns about a stand. An unknown-id
// response is a build without one; anything else means the command reached a parser that owns
// it — including a rejected payload, which is a stand refusing this particular scan.
void DevDriver::receivedStandScan(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);

    if (resp == respNone) return;

    const bool supported = (resp != respErrorID);
    if (supported != standSupported_) {
        standSupported_ = supported;
        emit standChanged();
    }
}

void DevDriver::receivedPwmRoute(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);

    if (resp == respNone) { emit pwmRouteChanged(); }
}

void DevDriver::receivedDevSync(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);
    if (!idDevSync) return;

    if (resp == respNone || resp == respOk) {
        idDevSync->commitFromDisplayed();
        emit devSyncChanged();
    } else if (resp == respErrorPayload) {
        idDevSync->revertToCommitted();
        emit devSyncErrorOccurred(tr("Sync config rejected by device"));
        idDevSync->simpleRequest(v0);
        emit devSyncChanged();
    }
}

void DevDriver::receivedChartSetup(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);

    if(resp == respNone) {
        const QString held = QStringLiteral("count %1, resolution %2 mm, offset %3")
                                 .arg(idChartSetup->count()).arg(idChartSetup->resolution()).arg(idChartSetup->offset());
        if (held != lastChartSetupLog_) {
            lastChartSetupLog_ = held;
            qInfo().noquote() << QStringLiteral("chart setup on the device: %1").arg(held);
        }
        emit sendChartSetup(getChannelId(), idChartSetup->resolution(), idChartSetup->count(), idChartSetup->offset());
        emit chartSetupChanged();
    }
}

void DevDriver::receivedDSPSetup(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);

    if(resp == respNone) {  emit dspSetupChanged();  }
}

void DevDriver::receivedTransc(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);

    if (resp == respNone) {
        emit sendTranscSetup(getChannelId(), idTransc->freq(), idTransc->pulse(), idTransc->boost());
        emit transChanged();
    }
}

void DevDriver::receivedSoundSpeed(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);

    if (resp == respNone) {
        emit sendSoundSpeed(getChannelId(), idSoundSpeed->getSoundSpeed());
        emit soundChanged();
    }
}

void DevDriver::receivedUART(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);

    // The Wi-Fi module refusing a port speed (above its firmware's limit): the change stops before the link moves.
    if (resp != respNone && resp != respOk && wifiPort_ && wifiPort_->active()) {
        wifiPort_->moduleRefused();
    }
    if (resp == respOk && isWifi())                // a port rate: a setting a profile holds
        wifiProfileAfterSetting();
    if(resp == respNone) { emit UARTChanged(); }
}

void DevDriver::receivedVersion(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);

    if(resp == respNone && idVersion->bootMode() != IDBinVersion::BootModeBootloader &&
       isStaleVersionAfterReboot()) {
        idVersion->reset();
        return;
    }

    // An update waiting for the board to start its firmware (fwUpgradeProcess): the boot mode it answers with.
    if(resp == respNone && ver == v2 && upgradeStartAt_ != 0) {
        const int64_t since = QDateTime::currentMSecsSinceEpoch() - upgradeStartAt_;
        if(idVersion->bootMode() == IDBinVersion::BootModeFirmware) {
            finishUpgradeStart(successUpgrade, QStringLiteral("done: the board answers from its firmware %1.%2, %3 s after "
                                                              "it was told to start")
                                                   .arg(idVersion->fwVersion()).arg(idVersion->fwVersionMinor())
                                                   .arg(double(since) / 1000.0, 0, 'f', 1));
        } else if(idVersion->bootMode() == IDBinVersion::BootModeBootloader && since >= kUpgradeStartGraceMs) {
            finishUpgradeStart(bootloaderUpgrade, QStringLiteral("failed: the board is still in its bootloader %1 s after "
                                                                 "it was told to start; its firmware did not start")
                                                      .arg(double(since) / 1000.0, 0, 'f', 1));
        }
    }

    if(resp == respNone) {
        if(ver == v0) {
            // The board names live in knownBoardName(), which the Wi-Fi module's port list uses as well.
            const int board = idVersion->boardVersion();
            const QString known = knownBoardName(board);
            if(board == BoardNone && idVersion->boardVersionMinor() == BoardAssist) {
                m_devName = "Assist";
            } else if(board != BoardNone && !known.isEmpty()) {
                m_devName = known;
            } else {
                m_devName = QString("Device ID: %1.%2").arg(idVersion->boardVersion()).arg(idVersion->boardVersionMinor());
            }

            emit deviceVersionChanged();
        } else if(ver == v1) {
            emit deviceIDChanged(idVersion->uid());
            emit deviceVersionChanged();
        } else if(ver == v2) {
            // 0.x is a version too (the Wi-Fi module's firmware); only 0.0 means none was reported.
            if(idVersion->fwVersion() != 0 || idVersion->fwVersionMinor() != 0) {
                m_fwVer = QString("%1.%2").arg(idVersion->fwVersion()).arg(idVersion->fwVersionMinor());
                if(isWifi() && m_fwVer != wifiFwLogged_) {
                    wifiFwLogged_ = m_fwVer;
                    qInfo().noquote() << wifiTag() << QStringLiteral("wifi: module firmware %1").arg(m_fwVer);
                }
            }

            if(idVersion->bootVersion() != 0) {
                m_bootVer = QString("%1.%2").arg(idVersion->bootVersion()).arg(idVersion->bootVersionMinor());
            }

            emit deviceVersionChanged();
        }
    }
}

void DevDriver::receivedMark(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type);
    Q_UNUSED(ver);
    Q_UNUSED(resp);
}

void DevDriver::receivedFlash(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type);
    Q_UNUSED(ver);

    if (portOp_ == PortOpSave && resp != respNone) {   // the save after a connector's change
        qInfo().noquote() << wifiTag() << QStringLiteral("ports: ID_FLASH answered %1").arg(int(resp));
        if (resp != respOk) {
            portDone(PortNotSaved);
        } else if (portOpMoves_) {   // a swap moves two functions: all four connectors read again
            portOp_ = PortOpReadBuses;
            portQueue_ = uartBuses();
            readNextBus();
        } else {
            portOp_ = PortOpReadBack;
            idBus->requestFunction(U1(portOpFunction_));
            portOpTimer_.start();
        }
    }
}

void DevDriver::receivedBoot(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type);
    Q_UNUSED(ver);
    Q_UNUSED(resp);
}

void DevDriver::fwUpgradeProcess() {
    if(!m_state.in_update) {
        return;
    }

    if (fwTraceEnabled()) {
        qInfo("FWTRACE %lld out  packet=%u offset=%d remaining=%d",
              fwTraceNow(), idUpdate->currentNumPacket(),
              idUpdate->currentFwOffset(), idUpdate->availSend());
    }
    bool is_avail_data = idUpdate->putUpdate();
    if(is_avail_data && idUpdate->currentNumPacket() == 3) {
        is_avail_data = idUpdate->putUpdate();
    }
    m_upgrade_status = idUpdate->progress();
    if(!is_avail_data) {
        if (fwTraceEnabled()) {
            qInfo("FWTRACE %lld runFW (ID_BOOT v1) - body complete", fwTraceNow());
        }
        idBoot->runFW();
        m_state.in_boot = false;
        m_state.in_update = false;
        upgradeStartedTime_ = 0;
        upgradeResendCount_ = 0;
        // Not flashed yet: that is when the board answers from its firmware (finishUpgradeStart). Behind a recorder
        // the route stays the bootloader's (0) till then, so a board that stayed in its bootloader answers too.
        m_upgrade_status = 100;
        upgradeStartAt_ = QDateTime::currentMSecsSinceEpoch();
        upgradeStartTimer_.start();

        emit upgradingFirmwareDone();
        emit upgradingFirmwareDoneDM();
        emit upgradeAwaitingStart(linkUuid_, lastAddress_, upgradeStartAt_);

        core.consoleInfo("Upgrade: the image is written and the board is told to start it; waiting for it to answer "
                         "from its firmware");
        restartState();
    }
}

void DevDriver::finishUpgradeStart(int status, const QString& line) {
    upgradeStartTimer_.stop();
    upgradeStartAt_ = 0;
    endRecorderRoute();
    m_upgrade_status = status;
    core.consoleInfo(QStringLiteral("Upgrade: ") + line);
    emit upgradeStartSettled(linkUuid_, lastAddress_);
    emit upgradeProgressChanged(m_upgrade_status);
    emit upgradeChanged();
}

void DevDriver::continueUpgradeStart(qint64 toldAtMs) {
    // A board on USB (the logger, 2026-11-03) leaves the link when it starts anew and comes back as a new device:
    // the wait goes on here, its time counted from when the board was told to start.
    upgradeStartAt_ = toldAtMs;
    m_upgrade_status = 100;
    upgradeStartTimer_.start();
    core.consoleInfo(QStringLiteral("Upgrade: the board is back on the link %1 s after it was told to start; waiting for "
                                    "it to answer from its firmware")
                         .arg(double(QDateTime::currentMSecsSinceEpoch() - toldAtMs) / 1000.0, 0, 'f', 1));
    emit upgradeAwaitingStart(linkUuid_, lastAddress_, upgradeStartAt_);
}

bool DevDriver::checkUpgradeTimeouts(int64_t curr_time) {
    if(!(m_state.in_boot || m_state.in_update)) {
        return false;
    }

    if(m_state.in_boot) {
        if(upgradeStartedTime_ != 0 && curr_time - upgradeStartedTime_ > bootHandshakeTimeoutMsec) {
            abortUpgrade("bootloader did not answer in time");
            return true;
        }
        return false;
    }

    if(_timeoutUpgradeAnswerTime <= 0 ||
       curr_time - _lastUpgradeAnswerTime <= _timeoutUpgradeAnswerTime) {
        return false;
    }

    if(upgradeResendCount_ >= upgradeResendLimit) {
        abortUpgrade(QString("no answer after %1 resends").arg(upgradeResendCount_));
        return true;
    }

    ++upgradeResendCount_;
    _lastUpgradeAnswerTime = curr_time;

    core.consoleInfo(QString("Upgrade: timeout, resend %1/%2").arg(upgradeResendCount_).arg(upgradeResendLimit));

    if (fwTraceEnabled()) {
        qInfo("FWTRACE %lld retransmit packet=%u offset=%d resend %d/%d",
              fwTraceNow(), idUpdate->currentNumPacket(), idUpdate->currentFwOffset(),
              (int)upgradeResendCount_, (int)upgradeResendLimit);
    }

    idUpdate->putUpdate(false);
    return false;
}

void DevDriver::abortUpgrade(const QString& reason) {
    endRecorderRoute();
    core.consoleInfo(QString("Upgrade: aborted -- %1").arg(reason));

    m_upgrade_status = failUpgrade;
    m_state.in_boot = false;
    m_state.in_update = false;
    m_bootloaderLagacyMode = true;
    _timeoutUpgradeAnswerTime = 0;
    upgradeStartedTime_ = 0;
    upgradeResendCount_ = 0;

    emit upgradingFirmwareDone();
    emit upgradingFirmwareDoneDM();
    emit upgradeProgressChanged(m_upgrade_status);
    emit upgradeChanged();

    restartState();
}

void DevDriver::receivedUpdate(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type);

    // Another device's update, answered here too: the logger takes the bootloader's route 0 as its own and refuses
    // each packet (2026-10-29), and the progress shown, one for all devices, fell back to 0 % on each refusal.
    if (!m_state.in_boot && !m_state.in_update) {
        return;
    }

    if(resp == respNone) {
        if(ver == v0) {
            const bool wasLegacy = m_bootloaderLagacyMode;
            m_bootloaderLagacyMode = false;
            _timeoutUpgradeAnswerTime = packetAnswerTimeoutMsec;
            IDBinUpdate::ID_UPGRADE_V0 prog = idUpdate->getDeviceProgress();
            if (fwTraceEnabled()) {
                qInfo("FWTRACE %lld in   type=%u devMsg=%u devOffset=%u rcv=%u"
                      " hostMsg=%u hostOffset=%d%s",
                      fwTraceNow(), prog.type, prog.lastNumMsg, prog.lastOffset,
                      prog.rcvNumMsg, idUpdate->currentNumPacket(),
                      idUpdate->currentFwOffset(),
                      wasLegacy ? "  [leaving legacy mode]" : "");
            }

            if(prog.type <= 2) {
                _lastUpgradeAnswerTime = QDateTime::currentMSecsSinceEpoch();
                upgradeResendCount_ = 0;

                if(prog.type == 1) {
                    core.consoleInfo(QString("Upgrade: back offset condition error with device msg/offset %1 %2, host msg/offset %3 %4").arg(prog.lastNumMsg).arg(prog.lastOffset).arg(idUpdate->currentNumPacket()).arg(idUpdate->currentFwOffset()));
                }
                else if(prog.type == 2) {
                    core.consoleInfo(QString("Upgrade: forward offset condition error with device msg/offset %1 %2, host msg/offset %3 %4").arg(prog.lastNumMsg).arg(prog.lastOffset).arg(idUpdate->currentNumPacket()).arg(idUpdate->currentFwOffset()));
                    idUpdate->setUpgradeNewPoint(prog.lastNumMsg, prog.lastOffset);
                }

                fwUpgradeProcess();
            } else {
                abortUpgrade(QString("device reported a fatal condition (type %1)").arg(prog.type));
            }
        }
    } else {
        if(resp == respOk) {
            if( m_state.in_update && m_bootloaderLagacyMode) {
                if (fwTraceEnabled()) {
                    qInfo("FWTRACE %lld in   respOk (legacy acknowledgement)", fwTraceNow());
                }
                _lastUpgradeAnswerTime = QDateTime::currentMSecsSinceEpoch();
                upgradeResendCount_ = 0;
                fwUpgradeProcess();
            }
        } else {
            if( m_state.in_update && m_bootloaderLagacyMode) {
                abortUpgrade("lagacy mode error");
            }
        }
    }


    emit upgradeProgressChanged(m_upgrade_status);
    emit upgradeChanged();
}

void DevDriver::receivedNav(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type);

    if (resp == respNone && ver == v2) {
        const uint8_t gnssFixType = idNav->gnssFixTypeV2();
        const uint8_t numSats = idNav->numSatsV2();
        const uint32_t unixTime = idNav->unixTimeV2();
        const int16_t unixOffsetMs = idNav->unixOffsetMsV2();

        int32_t unixOffsetNs = static_cast<int32_t>(idNav->unixOffsetMsV2()) * 1000000;
        if (unixOffsetNs < 0) {
            unixOffsetNs = 0;
        }

        const double latitude = idNav->latitudeV2();
        const double longitude = idNav->longitudeV2();
        const double courseDeg = idNav->groundCourseDegV2();
        const double velocityMps = idNav->groundVelocityMpsV2();
        const float yawDeg = idNav->yawV2();
        const float pitchDeg = idNav->pitchV2();
        const float rollDeg = idNav->rollV2();

        core.consoleInfo(QString("SimpleNavV2: fix %1 sats %2 lat %3 lon %4 course %5 speed %6 yaw %7 pitch %8 roll %9")
                             .arg(gnssFixType)
                             .arg(numSats)
                             .arg(latitude, 0, 'f', 7)
                             .arg(longitude, 0, 'f', 7)
                             .arg(courseDeg, 0, 'f', 2)
                             .arg(velocityMps, 0, 'f', 3)
                             .arg(yawDeg, 0, 'f', 2)
                             .arg(pitchDeg, 0, 'f', 2)
                             .arg(rollDeg, 0, 'f', 2));

        emit simpleNavV2Complete(gnssFixType, numSats, unixTime, unixOffsetMs,
                                 latitude, longitude, courseDeg, velocityMps,
                                 yawDeg, pitchDeg, rollDeg);
        emit positionComplete(latitude, longitude, unixTime, static_cast<uint32_t>(unixOffsetNs));
        emit gnssVelocityComplete(velocityMps, courseDeg);
        emit attitudeComplete(yawDeg, pitchDeg, rollDeg, AttitudeArbiter::KindNav, attitudeSource());

        //qDebug() << "SimpleNavV2 parsed (driver):"
        //         << "fix=" << idNav->gnssFixTypeV2()
        //         << "sats=" << idNav->numSatsV2()
        //         << "unixTime=" << idNav->unixTimeV2()
        //         << "unixOffsetMs=" << idNav->unixOffsetMsV2()
        //         << "lat=" << latitude
        //         << "lon=" << longitude
        //         << "courseDeg=" << courseDeg
        //         << "velocityMps=" << velocityMps
        //         << "yaw=" << yawDeg
        //         << "pitch=" << pitchDeg
        //         << "roll=" << rollDeg;
    }

    if (resp == respNone && ver == v1) {
        const double latitude = idNav->latitude();
        const double longitude = idNav->longitude();
        const float yawDeg = idNav->yaw();
        const float pitchDeg = idNav->pitch();
        const float rollDeg = idNav->roll();
        const float depthM = idNav->depth();

        core.consoleInfo(QString("ROV: yaw: %1, pitch: %2, roll: %3, lat: %4, lon: %5, depth: %6")
                             .arg(yawDeg)
                             .arg(pitchDeg)
                             .arg(rollDeg)
                             .arg(latitude)
                             .arg(longitude)
                             .arg(depthM));

        emit positionComplete(latitude, longitude, 0, 0);
        emit attitudeComplete(yawDeg, pitchDeg, rollDeg, AttitudeArbiter::KindNav, attitudeSource());
        emit depthComplete(depthM);
    }
}

void DevDriver::receivedBoatStatus(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type);

    if (resp == respNone && ver == v0) {
        const uint8_t batteryBoat = idBoatStatus->batteryBoatPercent();
        const uint8_t batteryBridge = idBoatStatus->batteryBridgePercent();
        const uint8_t signalBoat = idBoatStatus->signalQualityBoatPercent();
        const uint8_t signalBridge = idBoatStatus->signalQualityBridgePercent();

        emit boatStatusComplete(batteryBoat, batteryBridge, signalBoat, signalBridge);

        //qDebug() << "BoatStatusV0 parsed (driver):"
        //         << "batteryBoat=" << batteryBoat
        //         << "batteryBridge=" << batteryBridge
        //         << "signalBoat=" << signalBoat
        //         << "signalBridge=" << signalBridge;
    }
}

void DevDriver::receivedDVL(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type);

    if(resp == respNone) {
        if(ver == v0) {
            emit dopplerVeloComplete();
        } else  if(ver == v1) {
             emit dopplerBeamComplete(idDVL->beams(), idDVL->beamsCount());
        } else  if(ver == v2) {
            emit dvlSolutionComplete(idDVL->dvlSolution());
       }
    }
}

void DevDriver::receivedDVLMode(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type);
    Q_UNUSED(ver);
    Q_UNUSED(resp);
}

namespace {

// NAN is ordinary in a USBL solution rather than a fault: v1/v2 payloads carry
// no SNR at all, and a beacon that has not been located yet has no position.
// Printing "nan" for those makes a healthy line read as a broken one.
QString usblNum(double v, int prec)
{
    return qIsFinite(v) ? QString::number(v, 'f', prec) : QStringLiteral("--");
}

// One decoded line per acoustic fix, at the rate the fixes arrive.
//
// The hex frame dump lives behind the console's "Binary" toggle. With that
// toggle off -- which is the default and the readable way to watch a run --
// nothing from the device reached the console at all, so a working link was
// indistinguishable from a dead one. This is the text form of the same data.
QString usblConsoleLine(const char* kind, const IDBinUsblSolution::UsblSolution& s)
{
    return QString("USBL %1: addr %2, dist %3 m, az %4, el %5, snr %6"
                   ", head %7 %8, yaw %9, beacon %10 %11")
        .arg(QString::fromLatin1(kind),
             QString::number(static_cast<int>(s.id)),
             usblNum(s.distance_m, 2),
             usblNum(s.azimuth_deg, 1),
             usblNum(s.elevation_deg, 1),
             usblNum(s.snr, 1),
             usblNum(s.usbl_latitude, 7),
             usblNum(s.usbl_longitude, 7),
             usblNum(s.usbl_yaw, 1),
             usblNum(s.beacon_latitude, 7),
             usblNum(s.beacon_longitude, 7));
}

} // namespace

void DevDriver::receivedUSBL(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type);
    Q_UNUSED(ver);

    if(resp != respNone) {
        return;
    }

    // Dispatch on what was actually parsed: v1 carries either a nav solution or
    // a beacon activation response, so the version is not enough.
    switch(idUSBL->lastPayloadKind()) {
    case IDBinUsblSolution::PayloadKind::Solution:
        if (usblConsoled_) core.consoleInfo(usblConsoleLine("solution", idUSBL->usblSolution()));
        emit usblSolutionComplete(idUSBL->usblSolution());
        break;
    case IDBinUsblSolution::PayloadKind::AcousticNav:
        if (usblConsoled_) core.consoleInfo(usblConsoleLine("acoustic-nav", idUSBL->usblSolution()));
        emit acousticNavSolutionComplete(idUSBL->acousticNavSolution());
        emit usblSolutionComplete(idUSBL->usblSolution());
        break;
    case IDBinUsblSolution::PayloadKind::BaseToBeacon:
        if (usblConsoled_) core.consoleInfo(usblConsoleLine("base-to-beacon", idUSBL->usblSolution()));
        emit baseToBeaconComplete(idUSBL->baseToBeacon());
        emit usblSolutionComplete(idUSBL->usblSolution());
        break;
    case IDBinUsblSolution::PayloadKind::BeaconActivation:
        if (usblConsoled_) core.consoleInfo(QStringLiteral("USBL beacon-activation"));
        emit beaconActivationComplete(0);
        break;
    case IDBinUsblSolution::PayloadKind::None:
        break;
    }
}

void DevDriver::receivedUsblDiag(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type);

    if (resp != respNone || ver != v0 || !idUsblDiag->isValid()) {
        return;
    }

    emit usblDiagComplete(idUsblDiag->diag());
}

void DevDriver::receivedUSBLControl(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type)
    if(resp != respNone) {
        const int echo = usblAckEcho_;
        int matched = -1;
        QString how;
        const int64_t now = QDateTime::currentMSecsSinceEpoch();
        for(auto it = usblAnswered_.begin(); it != usblAnswered_.end();)
            it = (now - it.value()) > 10000 ? usblAnswered_.erase(it) : std::next(it);
        if(echo >= 0) {
            for(auto it = usblPending_.cbegin(); it != usblPending_.cend() && matched < 0; ++it) {
                if(it.key() == echo || it.value().relayed == echo) matched = it.key();
            }
        }
        if(matched >= 0) {
            how = matched == echo ? QStringLiteral("echo matched") : QStringLiteral("relayed echo matched");
        } else if(echo >= 0 && usblAnswered_.contains(echo)) {
            // A second RESP to a frame that was sent twice: answered already, and matching it by
            // version would confirm the next frame before it was answered.
            logUsblControl((int)ver, QStringLiteral("duplicate ack code %1 ck %2 ignored")
                                         .arg((int)resp).arg(echo, 4, 16, QLatin1Char('0')));
            return;
        } else {
            // The echo names neither a frame we sent nor its relayed form. The oldest frame still
            // waiting on this version is the one a device answering in order answers.
            int64_t oldest = std::numeric_limits<int64_t>::max();
            for(auto it = usblPending_.cbegin(); it != usblPending_.cend(); ++it) {
                if(it.value().ver == (int)ver && it.value().sentMs < oldest) {
                    oldest = it.value().sentMs;
                    matched = it.key();
                }
            }
            how = matched >= 0 ? QStringLiteral("matched by version, echo %1").arg(echo, 4, 16, QLatin1Char('0'))
                               : QStringLiteral("unmatched, echo %1").arg(echo, 4, 16, QLatin1Char('0'));
        }
        int ackVer = (int)ver;
        if(matched >= 0) {
            const UsblPendingFrame answered = usblPending_.take(matched);
            ackVer = answered.ver;
            usblAnswered_[matched] = now;
            if(answered.relayed >= 0) usblAnswered_[answered.relayed] = now;
        }
        logUsblControl(ackVer, QStringLiteral("ack code %1 ck %2 %3")
                                   .arg((int)resp).arg(matched >= 0 ? matched : echo, 4, 16, QLatin1Char('0')).arg(how));
        emit usblControlAck(ackVer, matched >= 0 ? matched : echo, (int)resp);
    }
    if(resp == respNone || (int)ver >= (int)usblAck_.size()) return;
    if(usblAck_[ver] != UsblApplyPending && usblAck_[ver] != UsblApplyNoAnswer) return;
    usblAck_[ver] = resp == respOk ? UsblApplyConfirmed : UsblApplyRejected;
    emit usblResponseConfigChanged();
}

void DevDriver::receivedModemSolution(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type)
    Q_UNUSED(ver)

    if(resp != respNone) {
        return;
    }

    const auto header = idModemSolution->header();
    QVariantMap entry;
    entry["payload"]   = modemLastPayload();
    entry["bits"]      = static_cast<int>(header.bit_length);
    entry["cmdId"]     = static_cast<int>(header.cmd_id_from);
    entry["addressTo"] = static_cast<int>(header.address_to);
    entry["event"]     = static_cast<int>(header.event);
    entry["atMs"]      = static_cast<double>(QDateTime::currentMSecsSinceEpoch());
    modemByAddr_[QString::number(static_cast<int>(header.address_from))] = entry;

    emit modemSolutionComplete(header, idModemSolution->payload());
    emit modemPayloadChanged();
}

void DevDriver::process() {
    int64_t curr_time = QDateTime::currentMSecsSinceEpoch();

    const int64_t processGap = lastProcessAtMs_ != 0 ? curr_time - lastProcessAtMs_ : 0;
    lastProcessAtMs_ = curr_time;
    if((m_state.in_boot || m_state.in_update) && processGap > processStallLogMsec) {
        core.consoleInfo(QString("Upgrade: event loop blocked for %1 ms").arg(processGap));
    }
    if (nodeProbeRunning())
        nodeProbe_->tick();
    stepRecorderUpgrade(curr_time);

    updateDataStale(curr_time);

    if(checkUpgradeTimeouts(curr_time)) {
        return;
    }

    bool usblAckExpired = false;
    for(int ver : { (int)v3, (int)v4, (int)v7 }) {
        if(usblAck_[ver] == UsblApplyPending && (curr_time - usblAckSentMs_[ver]) > usblAckTimeoutMsec) {
            usblAck_[ver] = UsblApplyNoAnswer;
            usblAckExpired = true;
        }
    }
    if(usblAckExpired) {
        emit usblResponseConfigChanged();
    }

    for(auto it = usblPending_.begin(); it != usblPending_.end();) {
        if((curr_time - it.value().sentMs) > usblAckTimeoutMsec) {
            const int ver = it.value().ver;
            const int checksum = it.key();
            it = usblPending_.erase(it);
            logUsblControl(ver, QStringLiteral("no answer ck %1").arg(checksum, 4, 16, QLatin1Char('0')));
            emit usblControlAck(ver, checksum, 0);
        } else {
            ++it;
        }
    }

    if(idNavSensorStatus && idNavSensorStatus->isValid() && !navSensorStatusStale_
       && (curr_time - lastNavSensorStatusMs_) > navSensorStatusStaleMsec) {
        navSensorStatusStale_ = true;
        emit navSensorStatusChanged();
    }

    if(m_state.duplex) {
        if(m_state.mark) {
            unanswered_.marked(curr_time);
            if(idVersion->boardVersion() != BoardNone) {
                if(m_state.uptime != UptimeFix) {
                    //qDebug() << "UptimeFix";
                    m_state.uptime = UptimeFix;
                }

                if(!m_state.connect) {
                    //qDebug() << "connect = true";
                    m_state.connect = true;
                    resendUsblControl();
                }

                // A quiet device (a stopped sonar) is asked for its mark, so only a silent one goes stale.
                if(DeviceLiveness::probeDue(m_state.lastConnectTime, lastQuietProbe_, curr_time)) {
                    lastQuietProbe_ = curr_time;
                    idMark->simpleRequest(v0);
                }

                if(isRecorder() && (curr_time - lastRecorderStatusReq_) >= 3000) {
                    lastRecorderStatusReq_ = curr_time;
                    requestRecorderStatus();
                }

                if(isRecorder() && !streamListRequested_) {
                    streamListRequested_ = true;
                    requestStreamList();
                }

                // Recorders only, once per connection. Every other device would be asked about
                // a command it has never heard of, and the answer would not change.
                if(isRecorder() && !standProbeSent_) {
                    standProbeSent_ = true;
                    if(idStandScan) idStandScan->probe();
                }

                // Recorders only, once per connection: the port table (ID_BUS, then ID_PIN), one frame at a time.
                // Never broadcast: on a GODIO ring a broadcast GET costs the following board a ping.
                if(isRecorder() && !portsProbeSent_) {
                    portsProbeSent_ = true;
                    resetPorts();
                    idBus->request(0);
                    portsAnswerTimer_.start();
                }

                // Wi-Fi module only: nothing else knows ID_WIFI. Asked once per connection; from
                // then on the module pushes its status on every change and a report each second.
                if(isWifi() && !wifiProbeSent_) {
                    wifiProbeSent_ = true;
                    idWifi->requestAll();
                    idWifiNet->requestAll();
                    idWifiNet->requestPorts();        // 0.12 port information; 0.11 answers ERR_VERSION
                    if (wifiProfileSupport() != 0)
                        idWifiProfile->requestAll();  // 0.22: the profile it carries; older firmware stays silent
                    wifiNetAskedAt_ = curr_time;
                }

                // The 2-channel L412 sonar only (firmware 11.21+ keeps presets; an older one answers ERR_ID or nothing).
                // Its firmware calls itself PULSEblue DSS (129) from 11.4 on, nanoSSS (17) only when built with
                // BOARD_ID=17 (an image's "_ID17" name says nothing of it): the board on this PC answered as 129 with
                // 11.21 (2026-10-23).
                if((boardVersion() == BoardNanoSSS || boardVersion() == BoardPULSEblue_DSS) && !presetsAnswered_
                   && !presetsUnsupported_ && presetAsks_ < 5
                   && curr_time - presetAskedAt_ >= 3000) {
                    ++presetAsks_;
                    presetAskedAt_ = curr_time;
                    idPreset->requestAll();
                }
                // Firmware before 0.10 does not know ID_WIFI_NET and may not answer at all; before 0.12 the
                // module may say nothing to v7 either. Both fall back to what the page did before.
                if(isWifi() && wifiNetAskedAt_ > 0 && curr_time - wifiNetAskedAt_ > 4000) {
                    if(idWifiNet->support() < 0) {
                        idWifiNet->markUnsupported();
                        emit wifiNetChanged();
                        emit wifiNetPortsChanged();
                    } else if(idWifiNet->portSupport() < 0) {
                        // The firmware decides (WifiNetRules): a module that has just started can leave the first v7
                        // unanswered, and taking that for "before 0.12" hid the X1/X2 cards for the whole session.
                        const int byFw = WifiNetRules::portsFor(idVersion->fwVersion(), idVersion->fwVersionMinor());
                        const QString fw = QStringLiteral("%1.%2").arg(idVersion->fwVersion()).arg(idVersion->fwVersionMinor());
                        if(byFw == 0 || (byFw < 0 && wifiPortsAsks_ >= 3)) {
                            qInfo().noquote() << wifiTag() << QStringLiteral("wifi-net: no port information (ID_WIFI_NET v7), firmware %1: the page before 0.12").arg(byFw == 0 ? fw : QStringLiteral("not known"));
                            idWifiNet->markPortsUnsupported();
                            emit wifiNetPortsChanged();
                        } else if(curr_time - wifiPortsAskedAt_ > 2000) {
                            idWifiNet->requestPorts();
                            wifiPortsAskedAt_ = curr_time;
                            ++wifiPortsAsks_;
                            if(wifiPortsAsks_ == 1 || wifiPortsAsks_ % 30 == 0)
                                qInfo().noquote() << wifiTag() << QStringLiteral("wifi-net: no answer to ID_WIFI_NET v7 yet (firmware %1, ask %2): asking again every 2 s").arg(fw).arg(wifiPortsAsks_);
                        }
                    }
                }
                if(isWifi() && idWifi->expireScan(curr_time)) {
                    emit wifiNetworksChanged();
                }
                // A link rate report is due right after its window: without it by then plus 3 s, given up.
                if(isWifi() && idWifiSurvey->rateRunning()
                   && curr_time - idWifiSurvey->rateStartedMs() > idWifiSurvey->rateWindowMs() + 3000) {
                    idWifiSurvey->cancelLinkRate();
                    wifiLinkRateError_ = tr("no answer from the module");
                    emit wifiLinkRateChanged();
                }
                // A survey whose frames are late is asked for once more (GETTING v0), then given up.
                const int surveyDwell = idWifiSurvey->dwellMs() > 0 ? idWifiSurvey->dwellMs() : WifiSurvey::kDefaultDwellMs;
                if(isWifi() && idWifiSurvey->running()
                   && curr_time - idWifiSurvey->startedMs() > WifiSurvey::timeoutMs(11, surveyDwell)) {
                    if(!wifiSurveyAskedAgain_) {
                        wifiSurveyAskedAgain_ = true;
                        qInfo().noquote() << wifiTag() << "wifi-survey: late, asking for it once more";
                        idWifiSurvey->requestLast();
                    } else if(curr_time - idWifiSurvey->startedMs() > 2 * WifiSurvey::timeoutMs(11, surveyDwell)) {
                        idWifiSurvey->cancel();
                        wifiSurveyError_ = tr("no answer from the module");
                        qInfo().noquote() << wifiTag() << "wifi-survey: no answer, given up";
                        emit wifiSurveyChanged();
                    }
                }

                if(m_state.in_boot && idVersion->bootMode() == IDBinVersion::BootModeFirmware) {
                    idVersion->requestAll();
                }
                else if(m_state.in_boot) {
                    m_state.in_boot = false;
                    m_state.in_update = true;
                    // idUpdate->putUpdate();

                    _timeoutUpgradeAnswerTime = packetAnswerTimeoutMsec;
                    _lastUpgradeAnswerTime = curr_time;
                    upgradeResendCount_ = 0;

                    QTimer::singleShot(100, idUpdate, [update = idUpdate]() {
                        update->putUpdate();
                    });
                    //qDebug() << "To upgrading";
                }

                startNodeProbeIfDue();
                if(!(m_state.in_boot || m_state.in_update) && m_state.conf < ConfRequest && !nodeProbeRunning()) {
                    requestSetup();
                    //qDebug() << "Request setup";
                }

            } else {
                idVersion->requestAll();
                //qDebug() << "Request version again";
            }
        } else {
            const bool held = m_busAddress == 0
                              && DeviceLiveness::searchHeld(unanswered_.lastFrameMs, otherDeviceHeardMs_, searchAsks_,
                                                            curr_time);
            if(held != searchHeldLogged_) {
                searchHeldLogged_ = held;
                qInfo().noquote() << wifiTag() << (held ? "search: another device of the line answers, the search waits"
                                                        : "search: the line is quiet, the search asks again");
            }
            if(held)
                return;
            // Without the mark: asked each tick at first, then every DeviceLiveness::kUnansweredSlowMs.
            if(!unanswered_.askDue(curr_time))
                return;
            if(searchAsks_ < DeviceLiveness::kSearchFirstAsks)
                ++searchAsks_;
            restartState();
            streamListRequested_ = false;
            standProbeSent_ = false;
            portsProbeSent_ = false;   // a device back from a restart may have new roles: the table is read again
            portsLiveTimer_.stop();
            wifiProbeSent_ = false;
            wifiLastState_ = -1;
            wifiNetAskedAt_ = 0;
            wifiPortsAskedAt_ = 0;
            wifiPortsAsks_ = 0;
            wifiFastReport_ = false;
            resetPresets();
            if(idWifiNet->support() >= 0) {
                idWifiNet->reset();
                emit wifiNetChanged();
                emit wifiNetStatsChanged();
                emit wifiNetClientsChanged();
                emit wifiNetPortsChanged();
                emit wifiNetPortDevicesChanged();
                emit wifiNetPortPeersChanged();
            }
            if(idWifiSurvey->total() >= 0 || idWifiSurvey->running()) {
                idWifiSurvey->reset();
                wifiSurveyWindow_.clear();
                emit wifiSurveyChanged();
            }
            wifiRetries_.clear();
            if(idWifiProfile->status().valid) {
                idWifiProfile->reset();
                emit wifiProfileChanged();
            }
            // A restart switches the radio on (nothing of ID_WIFI_RF is saved)
            if(idWifiRf->state().valid || !wifiRfError_.isEmpty()) {
                idWifiRf->reset();
                wifiRfError_.clear();
                emit wifiRfChanged();
            }
            if(idWifi->hasData()) {
                idWifi->reset();
                emit wifiStatusChanged();
                emit wifiReportChanged();
                emit wifiNetworksChanged();
                emit wifiSavedChanged();
            }
            if(standSupported_) {
                standSupported_ = false;
                emit standChanged();
            }
            idMark->setMark();
            idVersion->requestAll();
            //qDebug() << "Reset state";
        }
    }
}
