#include "dev_driver.h"

#include <QDateTime>
#include <time.h>
#include <core.h>
#include <QDateTime>
#include <QFile>
#include <QThread>
#include <type_traits>
#include <QVariant>
#include <QXmlStreamWriter>
#include "notifications.h"

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
    regID(idVersion = new IDBinVersion(), &DevDriver::receivedVersion, true);

    regID(idMark = new IDBinMark(), &DevDriver::receivedMark);
    regID(idFlash = new IDBinFlash(), &DevDriver::receivedFlash);
    regID(idBoot = new IDBinBoot(), &DevDriver::receivedBoot);
    regID(idUpdate = new IDBinUpdate(), &DevDriver::receivedUpdate);

    regID(idNav = new IDBinNav(), &DevDriver::receivedNav);
    regID(idBoatStatus = new IDBinBoatStatus(), &DevDriver::receivedBoatStatus);
    regID(idRecorderStatus = new IDBinRecorderStatus(), &DevDriver::receivedRecorderStatus);
    regID(idDVL = new IDBinDVL(), &DevDriver::receivedDVL);
    regID(idDVLMode = new IDBinDVLMode(), &DevDriver::receivedDVLMode);

    regID(idUSBL = new IDBinUsblSolution(), &DevDriver::receivedUSBL);

    regID(idUSBLControl = new IDBinUsblControl(), &DevDriver::receivedUSBLControl);

    regID(idModemSolution = new IDBinModemSolution(), &DevDriver::receivedModemSolution);

    regID(idServoControl = new IDBinServoControl(), &DevDriver::receivedServoControl, true);
    // Not a setup command: it is control-only, so it has nothing to answer a cold-start
    // request with and must not take part in the sweep.
    regID(idStandScan = new IDBinStandScan(), &DevDriver::receivedStandScan);
    regID(idPwmRoute = new IDBinPwmRoute(), &DevDriver::receivedPwmRoute, true);
    regID(idDevSync = new IDBinDevSync(), &DevDriver::receivedDevSync, true);

#ifndef SEPARATE_READING
    connect(&m_processTimer, &QTimer::timeout, this, &DevDriver::process);
#endif

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
}

DevDriver::~DevDriver()
{
#ifdef SEPARATE_READING
    QList<QTimer*> timers = getChildTimers();
    foreach (QTimer* timer, timers) {
        if (timer->isActive()) {
            timer->stop();
        }
    }

    if (m_processTimer.isActive()) {
        m_processTimer.stop();
    }
#endif
    m_processTimer.stop();
}

ChannelId DevDriver::getChannelId() const
{
    return ChannelId(linkUuid_, lastAddress_);
}

#ifdef SEPARATE_READING
QTimer *DevDriver::getProcessTimer()
{
    return &m_processTimer;
}

QList<QTimer *> DevDriver::getChildTimers()
{
    QList<QTimer*> timers;

    if (idTimestamp) {
        timers.append(idTimestamp->getSetTimer());
        timers.append(idTimestamp->getColdStartTimer());
    }
    if (idDist) {
        timers.append(idDist->getSetTimer());
        timers.append(idDist->getColdStartTimer());
    }
    if (idChart) {
        timers.append(idChart->getSetTimer());
        timers.append(idChart->getColdStartTimer());
    }
    if (idAtt) {
        timers.append(idAtt->getSetTimer());
        timers.append(idAtt->getColdStartTimer());
    }
    if (idTemp) {
        timers.append(idTemp->getSetTimer());
        timers.append(idTemp->getColdStartTimer());
    }
    if (idDataset) {
        timers.append(idDataset->getSetTimer());
        timers.append(idDataset->getColdStartTimer());
    }
    if (idDistSetup) {
        timers.append(idDistSetup->getSetTimer());
        timers.append(idDistSetup->getColdStartTimer());
    }
    if (idChartSetup) {
        timers.append(idChartSetup->getSetTimer());
        timers.append(idChartSetup->getColdStartTimer());
    }
    if (idDSPSetup) {
        timers.append(idDSPSetup->getSetTimer());
        timers.append(idDSPSetup->getColdStartTimer());
    }
    if (idTransc) {
        timers.append(idTransc->getSetTimer());
        timers.append(idTransc->getColdStartTimer());
    }
    if (idSoundSpeed) {
        timers.append(idSoundSpeed->getSetTimer());
        timers.append(idSoundSpeed->getColdStartTimer());
    }
    if (idUART) {
        timers.append(idUART->getSetTimer());
        timers.append(idUART->getColdStartTimer());
    }
    if (idWifi) {
        timers.append(idWifi->getSetTimer());
        timers.append(idWifi->getColdStartTimer());
    }
    if (idWifiNet) {
        timers.append(idWifiNet->getSetTimer());
        timers.append(idWifiNet->getColdStartTimer());
    }
    if (idVersion) {
        timers.append(idVersion->getSetTimer());
        timers.append(idVersion->getColdStartTimer());
    }
    if (idMark) {
        timers.append(idMark->getSetTimer());
        timers.append(idMark->getColdStartTimer());
    }
    if (idFlash) {
        timers.append(idFlash->getSetTimer());
        timers.append(idFlash->getColdStartTimer());
    }
    if (idBoot) {
        timers.append(idBoot->getSetTimer());
        timers.append(idBoot->getColdStartTimer());
    }
    if (idUpdate) {
        timers.append(idUpdate->getSetTimer());
        timers.append(idUpdate->getColdStartTimer());
    }
    if (idNav) {
        timers.append(idNav->getSetTimer());
        timers.append(idNav->getColdStartTimer());
    }
    if (idBoatStatus) {
        timers.append(idBoatStatus->getSetTimer());
        timers.append(idBoatStatus->getColdStartTimer());
    }
    if (idDVL) {
        timers.append(idDVL->getSetTimer());
        timers.append(idDVL->getColdStartTimer());
    }
    if (idDVLMode) {
        timers.append(idDVLMode->getSetTimer());
        timers.append(idDVLMode->getColdStartTimer());
    }
    if (idUSBL) {
        timers.append(idUSBL->getSetTimer());
        timers.append(idUSBL->getColdStartTimer());
    }

    return timers;
}
#endif

void DevDriver::regID(IDBin* id_bin, ParseCallback method, bool is_setup) {
//    hashIDParsing[id_bin->id()] = id_bin;
//    hashIDCallback[id_bin->id()] = method;

    _hashID[id_bin->id()] = ID_Instance(id_bin, method, is_setup);


//    if(is_setup) {
//        hashIDSetup[id_bin->id()] = id_bin;
//    }

    connect(id_bin, &IDBin::binFrameOut, this, &DevDriver::onIdFrameOut);
}

void DevDriver::onIdFrameOut(Parsers::ProtoBinOut out) {
    // The ring firmware runs a wrapped command of at most 61 bytes (the wrapper stays within its 64) and only
    // a command (SETTING or GETTING); anything else goes as it is.
    const uint8_t* f = out.frame();
    if (!isRingBehindRecorder() || out.frameLen() < 8 || (f[3] & 0x3) < 2 || f[5] > 61) {
        emit binFrameOut(out);
        return;
    }
    ProtoBinOut node;
    node.create(Type(f[3] & 0x3), v0, ID_NODE_CMD, 0);
    node.write<U1>(lastAddress_);
    node.write<U1>(f[3]);
    node.write<U1>(f[4]);
    for (int i = 0; i < f[5]; ++i)
        node.write<U1>(f[6 + i]);
    node.end();
    emit binFrameOut(node);
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
    idUSBLControl->pingRequest(timeout_us, address);
}

void DevDriver::acousticPingRequestEx(uint8_t address, uint32_t timeout_us, uint8_t cmdId, uint32_t replyDistanceMm, const QByteArray& payload) {
    if(!m_state.connect) return;
    idUSBLControl->pingRequest(timeout_us, address, cmdId, replyDistanceMm, payload);
}

void DevDriver::acousticResponceFilter(uint8_t address) {
    if(!m_state.connect) return;
    idUSBLControl->setResponseAddressFilter(address);
}

void DevDriver::acousticResponceFilterSlots(const QVector<int>& addresses) {
    if(!m_state.connect) return;

    std::array<uint8_t, 8> filter;
    filter.fill(0xFF);
    const int count = qMin((int)filter.size(), (int)addresses.size());
    for(int i = 0; i < count; ++i) {
        filter[i] = (uint8_t)addresses[i];
    }
    idUSBLControl->setResponseAddressFilter(filter);
}

void DevDriver::acousticResponceTimeout(uint32_t timeout_us) {
    if(!m_state.connect) return;
    idUSBLControl->setResponseTimeout(timeout_us);
}

void DevDriver::setUsblTransponderEnable(bool enabled) {
    if(!m_state.connect) return;
    idUSBLControl->setTransponderEnable(enabled ? 0xFFFFFFFF : 0);
}

void DevDriver::setUsblMonitorConfig(uint32_t suppressSelfResponseUs, uint32_t suppressSelfRequestUs, bool receiveResponseInIdle) {
    if(!m_state.connect) return;

    IDBinUsblControl::USBLMonitorConfig cfg;
    cfg.suppressSelfResponse_us = suppressSelfResponseUs;
    cfg.suppressSelfRequest_us = suppressSelfRequestUs;
    cfg.receiveResponseInIdle = receiveResponseInIdle;
    idUSBLControl->setMonitorConfig(cfg);
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
                                 int addressAction, int addressReplacement) {
    if(!m_state.connect) return;

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
    cfg.cmd_id = (uint8_t)qBound(0, cmdId, 255);
    cfg.eventFilter = event == Cfg::EventOnReceiveResponse ? Cfg::EventOnReceiveResponse
                                                           : Cfg::EventOnReceiveRequest;
    cfg.cmdIdAction = cmdIdAction == Cfg::SendBackCmdIdReplacement ? Cfg::SendBackCmdIdReplacement
                                                                  : Cfg::SendBackCmdIdIncoming;
    cfg.cmd_id_replacement = (uint8_t)qBound(0, cmdIdReplacement, 255);
    cfg.addressAction = addressAction == Cfg::SendBackAddressReplacement ? Cfg::SendBackAddressReplacement
                                                                        : Cfg::SendBackAddressIncoming;
    cfg.address_replacement = (uint8_t)qBound(0, addressReplacement, 255);
    cfg.eventAction = eventAction == Cfg::SendBackEventSame ? Cfg::SendBackEventSame
                                                           : Cfg::SendBackEventSwaping;
    cfg.receiver_function = fn(receiverFunction);
    cfg.receive_bit_length = (uint16_t)qBound(0, receiveBitLength, 65535);
    cfg.sender_function = fn(senderFunction);
    // sending_bit_length is derived from the payload actually written, inside setCmdConfig.

    const QByteArray payload = parseHexPayload(sendHexPayload);
    idUSBLControl->setCmdConfig(cfg, payload);
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

#ifdef SEPARATE_READING
void DevDriver::initProcessTimerConnects()
{
    connect(&m_processTimer, &QTimer::timeout, this, &DevDriver::process, Qt::QueuedConnection);
}

void DevDriver::initChildsTimersConnects()
{
    if (idTimestamp) {
        idTimestamp->initTimersConnects();
    }
    if (idDist) {
        idDist->initTimersConnects();
    }
    if (idChart) {
        idChart->initTimersConnects();
    }
    if (idAtt) {
        idAtt->initTimersConnects();
    }
    if (idTemp) {
        idTemp->initTimersConnects();
    }
    if (idDataset) {
        idDataset->initTimersConnects();
    }
    if (idDistSetup) {
        idDistSetup->initTimersConnects();
    }
    if (idChartSetup) {
        idChartSetup->initTimersConnects();
    }
    if (idDSPSetup) {
        idDSPSetup->initTimersConnects();
    }
    if (idTransc) {
        idTransc->initTimersConnects();
    }
    if (idSoundSpeed) {
        idSoundSpeed->initTimersConnects();
    }
    if (idUART) {
        idUART->initTimersConnects();
    }
    if (idWifi) {
        idWifi->initTimersConnects();
    }
    if (idWifiNet) {
        idWifiNet->initTimersConnects();
    }
    if (idVersion) {
        idVersion->initTimersConnects();
    }
    if (idMark) {
        idMark->initTimersConnects();
    }
    if (idFlash) {
        idFlash->initTimersConnects();
    }
    if (idBoot) {
        idBoot->initTimersConnects();
    }
    if (idUpdate) {
        idUpdate->initTimersConnects();
    }
    if (idNav) {
        idNav->initTimersConnects();
    }
    if (idBoatStatus) {
        idBoatStatus->initTimersConnects();
    }
    if (idDVL) {
        idDVL->initTimersConnects();
    }
    if (idDVLMode) {
        idDVLMode->initTimersConnects();
    }
    if (idUSBL) {
        idUSBL->initTimersConnects();
    }
}
#endif

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

    if (proto.completeAsKBP2())
        viaRecorder_ = true;   // a recorder re-wraps its port devices' frames and stamps its port as their route
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

            if(_hashID[proto.id()].callback != nullptr) {
                ParseCallback& callback = _hashID[proto.id()].callback;
                (this->*callback)(parse_instance->lastType(), parse_instance->lastVersion(), parse_instance->lastResp());
            }
        }
    }
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
    wifiProbeSent_ = false;
    wifiNetAskedAt_ = 0;
    wifiPortsAskedAt_ = 0;
    wifiPortsAsks_ = 0;
    wifiFastReport_ = false;
    wifiLastState_ = -1;
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
// #ifndef SEPARATE_READING
//         core.consoleInfo(s);
// #endif
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

void DevDriver::sendUpdateFW(QByteArray update_data) {
    if (isRingBoard()) {
        emit ringUpgradeRequested(linkUuid_, lastAddress_, update_data, upgradeExpect_, false);
        return;
    }
    if (fwTraceEnabled()) {
        qInfo("FWTRACE %lld start size=%lld bytes packet=%d",
              fwTraceNow(), (long long)update_data.size(), 96);
    }
    reboot();
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
    if (!m_state.connect || wifiLinkRateSupport() != 1 || idWifiSurvey->rateRunning() || idWifiSurvey->running())
        return;
    wifiLinkRateError_.clear();
    idWifiSurvey->requestLinkRate(uint16_t(WifiLinkRate::kWindowMs));
    emit wifiLinkRateChanged();
}

void DevDriver::wifiSurvey() {
    if (!m_state.connect || wifiSurveySupport() != 1 || idWifiSurvey->running())
        return;
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
            for (const QByteArray& page : idWifiSurvey->ratePages()) {
                WifiLinkRate::Page p;
                if (!WifiLinkRate::parse(page, p) || p.total == 0)
                    continue;
                const QString main = p.rates.isEmpty() ? QStringLiteral("no data frames") : WifiLinkRate::text(p.rates.first());
                mains << p.mac + QStringLiteral(": ") + main;
                parts << QStringLiteral("%1: %2 (%3 of %4 frames, %5 dBm)").arg(p.mac, main)
                             .arg(p.rates.isEmpty() ? 0 : p.rates.first().frames).arg(p.frames).arg(p.rssi);
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
    if (m_state.connect) idWifi->requestAll();
}

void DevDriver::wifiScan() {
    if (!m_state.connect) return;
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
    if (is_changed) {
        emit sendChartSetup(getChannelId(), idChartSetup->resolution(), idChartSetup->count(), static_cast<uint16_t>(offset));
        emit chartSetupChanged();
    }
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

void DevDriver::receivedChart(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type);
    Q_UNUSED(resp);

    bool isCompleteChart = idChart->readAndClearIsCompleteChart();

    if (!isCompleteChart || (ver != v0 && ver != v1)) {
        return;
    }

    // connection quality
    if (errorFreezeCnt_ > 100) {
        averageChartLosses_ = idChart->getAverageLosses();
        emit averageChartLossesChanged();
        errorFreezeCnt_ = 0;
    }
    ++errorFreezeCnt_;

    const int size = idChart->chartSize();
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

void DevDriver::receivedAtt(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);
    Q_UNUSED(ver);
    Q_UNUSED(resp);

    const float yaw = idAtt->yaw();
    const float pitch = idAtt->pitch();
    const float roll = idAtt->roll();

    if (!qFuzzyIsNull(yaw) || !qFuzzyIsNull(pitch) || !qFuzzyIsNull(roll)) {
        emit attitudeComplete(yaw, idAtt->pitch(), idAtt->roll());
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
    Q_UNUSED(ver);

    if (resp == respNone) { emit servoControlChanged(); }
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
    if(resp == respNone) { emit UARTChanged(); }
}

void DevDriver::receivedVersion(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp) {
    Q_UNUSED(type);

    if(resp == respNone && idVersion->bootMode() != IDBinVersion::BootModeBootloader &&
       isStaleVersionAfterReboot()) {
        idVersion->reset();
        return;
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
    Q_UNUSED(resp);
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
        m_upgrade_status = successUpgrade;
        upgradeStartedTime_ = 0;
        upgradeResendCount_ = 0;

        emit upgradingFirmwareDone();
        emit upgradingFirmwareDoneDM();

#ifndef SEPARATE_READING
        core.consoleInfo("Upgrade: done");
#endif
        restartState();
    }
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

#ifndef SEPARATE_READING
    core.consoleInfo(QString("Upgrade: timeout, resend %1/%2").arg(upgradeResendCount_).arg(upgradeResendLimit));
#endif

    if (fwTraceEnabled()) {
        qInfo("FWTRACE %lld retransmit packet=%u offset=%d resend %d/%d",
              fwTraceNow(), idUpdate->currentNumPacket(), idUpdate->currentFwOffset(),
              (int)upgradeResendCount_, (int)upgradeResendLimit);
    }

    idUpdate->putUpdate(false);
    return false;
}

void DevDriver::abortUpgrade(const QString& reason) {
#ifndef SEPARATE_READING
    core.consoleInfo(QString("Upgrade: aborted -- %1").arg(reason));
#else
    Q_UNUSED(reason);
#endif

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
#ifndef SEPARATE_READING
                    core.consoleInfo(QString("Upgrade: back offset condition error with device msg/offset %1 %2, host msg/offset %3 %4").arg(prog.lastNumMsg).arg(prog.lastOffset).arg(idUpdate->currentNumPacket()).arg(idUpdate->currentFwOffset()));
#endif
                }
                else if(prog.type == 2) {
#ifndef SEPARATE_READING
                    core.consoleInfo(QString("Upgrade: forward offset condition error with device msg/offset %1 %2, host msg/offset %3 %4").arg(prog.lastNumMsg).arg(prog.lastOffset).arg(idUpdate->currentNumPacket()).arg(idUpdate->currentFwOffset()));
#endif
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

#ifndef SEPARATE_READING
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
#endif

        emit simpleNavV2Complete(gnssFixType, numSats, unixTime, unixOffsetMs,
                                 latitude, longitude, courseDeg, velocityMps,
                                 yawDeg, pitchDeg, rollDeg);
        emit positionComplete(latitude, longitude, unixTime, static_cast<uint32_t>(unixOffsetNs));
        emit gnssVelocityComplete(velocityMps, courseDeg);
        emit attitudeComplete(yawDeg, pitchDeg, rollDeg);

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

#ifndef SEPARATE_READING
        core.consoleInfo(QString("ROV: yaw: %1, pitch: %2, roll: %3, lat: %4, lon: %5, depth: %6")
                             .arg(yawDeg)
                             .arg(pitchDeg)
                             .arg(rollDeg)
                             .arg(latitude)
                             .arg(longitude)
                             .arg(depthM));
#endif

        emit positionComplete(latitude, longitude, 0, 0);
        emit attitudeComplete(yawDeg, pitchDeg, rollDeg);
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
        emit usblSolutionComplete(idUSBL->usblSolution());
        break;
    case IDBinUsblSolution::PayloadKind::AcousticNav:
        emit acousticNavSolutionComplete(idUSBL->acousticNavSolution());
        emit usblSolutionComplete(idUSBL->usblSolution());
        break;
    case IDBinUsblSolution::PayloadKind::BaseToBeacon:
        emit baseToBeaconComplete(idUSBL->baseToBeacon());
        emit usblSolutionComplete(idUSBL->usblSolution());
        break;
    case IDBinUsblSolution::PayloadKind::BeaconActivation:
        emit beaconActivationComplete(0);
        break;
    case IDBinUsblSolution::PayloadKind::None:
        break;
    }
}

void DevDriver::receivedUSBLControl(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type)
    Q_UNUSED(ver)
    Q_UNUSED(resp)
}

void DevDriver::receivedModemSolution(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp)
{
    Q_UNUSED(type)
    Q_UNUSED(ver)

    if(resp != respNone) {
        return;
    }

    emit modemSolutionComplete(idModemSolution->header(), idModemSolution->payload());
    emit modemPayloadChanged();
}

void DevDriver::process() {
    int64_t curr_time = QDateTime::currentMSecsSinceEpoch();

    updateDataStale(curr_time);

    if(checkUpgradeTimeouts(curr_time)) {
        return;
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

                // Wi-Fi module only: nothing else knows ID_WIFI. Asked once per connection; from
                // then on the module pushes its status on every change and a report each second.
                if(isWifi() && !wifiProbeSent_) {
                    wifiProbeSent_ = true;
                    idWifi->requestAll();
                    idWifiNet->requestAll();
                    idWifiNet->requestPorts();        // 0.12 port information; 0.11 answers ERR_VERSION
                    wifiNetAskedAt_ = curr_time;
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

                if(!(m_state.in_boot || m_state.in_update) && m_state.conf < ConfRequest) {
                    requestSetup();
                    //qDebug() << "Request setup";
                }

            } else {
                idVersion->requestAll();
                //qDebug() << "Request version again";
            }
        } else {
            // Without the mark: asked each tick at first, then every DeviceLiveness::kUnansweredSlowMs.
            if(!unanswered_.askDue(curr_time))
                return;
            restartState();
            streamListRequested_ = false;
            standProbeSent_ = false;
            wifiProbeSent_ = false;
            wifiLastState_ = -1;
            wifiNetAskedAt_ = 0;
            wifiPortsAskedAt_ = 0;
            wifiPortsAsks_ = 0;
            wifiFastReport_ = false;
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
