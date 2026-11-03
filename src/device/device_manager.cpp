#include <QFile>
#include <algorithm>
#include "device_manager.h"
#include "device_defs.h"
#include <algorithm>
#include <iterator>
#include <QDateTime>
#include "location_reader.h"
#include "core.h"
#include "frame_codec.h"
#include "autopilot_messages.h"
extern Core core;

namespace {

constexpr qint64 kAutopilotHeartbeatTimeoutMs = 3000;
constexpr int kLinkWindowSeconds = 5;
constexpr int kLinkMaxGap = 128;
constexpr qint64 kRadioStatusTimeoutMs = 5000;
constexpr qint64 kProxyTrafficTimeoutMs = 3000;
constexpr int kRadioRssiUnknown = 255;
constexpr qint64 kNavOutputTimeoutMs = 3000;
constexpr float kCrossTrackStepM = 0.1f;
constexpr int kMissionSeqMax = 0xFFFF;
constexpr float kSpeedTypeGround = 1.0f;
constexpr float kThrottleUnchanged = -1.0f;
constexpr qint64 kStreamWatchdogMs = 10000;
constexpr int kStreamRequestAttempts = 3;
constexpr float kMissionCurrentIntervalUs = 1000000.0f;
constexpr float kNavOutputIntervalUs = 500000.0f;
constexpr uint16_t kExtendedStatusRateHz = 2;
constexpr float kGpsRawIntervalUs = 1000000.0f;
constexpr qint64 kGpsRawTimeoutMs = 5000;
constexpr int kGpsRawFixTypeOffset = 28;
constexpr int kGpsRawSatellitesOffset = 29;
constexpr uint8_t kGpsSatellitesUnknown = 0xFF;
constexpr uint16_t kGpsHdopUnknown = 0xFFFF;
constexpr double kGpsHdopScale = 100.0;
constexpr int kSikSystemId = '3';
constexpr int kSikComponentId = 'D';

int radioDbm(int raw, bool sik)
{
    if (sik) {
        return std::clamp(int(std::lround(double(raw) / 1.9 - 127.0)), -120, 0);
    }
    return int(int8_t(uint8_t(raw)));
}

} // namespace


DeviceManager::DeviceManager()
    : lastDevs_(nullptr),
      lastDevice_(nullptr),
      mavlinkLink_(nullptr),
      streamList_(this),
      lastAddress_(-1),
      progress_(0),
      isConsoled_(false),
      nmeaConsoled_(true),
      mavlinkConsoled_(false),
      upgradeUuid_(QUuid()),
      upgradeAddr_(0)
{
    qRegisterMetaType<ProtoBinOut>("ProtoBinOut");
    qRegisterMetaType<Parsers::ProtoBinOut>("Parsers::ProtoBinOut");
    qRegisterMetaType<uint8_t>("uint8_t");
    qRegisterMetaType<int16_t>("int16_t");
    qRegisterMetaType<QVector<uint8_t>>("QVector<uint8_t>");
    qRegisterMetaType<QByteArray>("QByteArray");
    qRegisterMetaType<IDBinUsblSolution::UsblSolution>("IDBinUsblSolution::UsblSolution");
    qRegisterMetaType<IDBinUsblDiag::UsblSignalDiag>("IDBinUsblDiag::UsblSignalDiag");
    qRegisterMetaType<IDBinUsblSolution::AcousticNavSolution>("IDBinUsblSolution::AcousticNavSolution");
    qRegisterMetaType<IDBinUsblSolution::BaseToBeacon>("IDBinUsblSolution::BaseToBeacon");
    qRegisterMetaType<IDBinModemSolution::ModemSolutionHeader>("IDBinModemSolution::ModemSolutionHeader");
    qRegisterMetaType<IDBinDVL::BeamSolution>("IDBinDVL::BeamSolution");
    qRegisterMetaType<uint16_t>("uint16_t");
    qRegisterMetaType<IDBinDVL::DVLSolution>("IDBinDVL::DVLSolution");
    qRegisterMetaType<uint32_t>("uint32_t");
    qRegisterMetaType<FrameParser>("FrameParser");
    qRegisterMetaType<Position>("Position");

    heartbeatClock_.start();
    autopilotTimer_.setInterval(1000);
    QObject::connect(&autopilotTimer_, &QTimer::timeout, this, &DeviceManager::checkAutopilotOnline);
}

DeviceManager::~DeviceManager()
{

}

float DeviceManager::vruVoltage()
{
    return vru_.voltage;
}

float DeviceManager::vruCurrent()
{
    return vru_.current;
}

float DeviceManager::vruVelocityH()
{
    return vru_.velocityH;
}

int DeviceManager::pilotArmState()
{
    return vru_.armState;
}

int DeviceManager::pilotModeState()
{
    return vru_.flightMode;
}

int DeviceManager::vruBatteryPercent()
{
    return vru_.batteryPercent;
}

bool DeviceManager::autopilotOnline()
{
    return vru_.online;
}

int DeviceManager::autopilotSystemId()
{
    return vru_.systemId;
}

void DeviceManager::autopilotArm(bool arm, bool force)
{
    sendCommandLong(MavCmdComponentArmDisarm, arm ? 1.0f : 0.0f, force ? kMavArmForceMagic : 0.0f);
}

void DeviceManager::autopilotSetMode(int customMode)
{
    if (customMode < 0) {
        return;
    }
    sendCommandLong(MavCmdDoSetMode, float(kMavModeFlagCustomModeEnabled), float(customMode));
}

void DeviceManager::autopilotStartMission()
{
    sendCommandLong(MavCmdMissionStart);
}

void DeviceManager::autopilotSetSpeed(float speed)
{
    if (!(speed > 0.0f)) {
        return;
    }
    sendCommandLong(MavCmdDoChangeSpeed, kSpeedTypeGround, speed, kThrottleUnchanged);
}

void DeviceManager::autopilotSetMissionCurrent(int seq)
{
    if (seq < 0 || seq > kMissionSeqMax) {
        return;
    }
    if (missionSetCurrentUnsupported_) {
        sendMissionSetCurrent(seq);
        return;
    }
    pendingMissionCurrentSeq_ = seq;
    sendCommandLong(MavCmdDoSetMissionCurrent, float(seq));
}

void DeviceManager::sendMissionSetCurrent(int seq)
{
    if (!vru_.online || vru_.systemId < 0 || autopilotLink_ == nullptr) {
        emit autopilotCommandAcked(MavCmdDoSetMissionCurrent, MavResultNotSent);
        return;
    }
    MAVLink_MSG_MISSION_SET_CURRENT message;
    message.seq = uint16_t(seq);
    message.target_system = uint8_t(vru_.systemId);
    message.target_component = uint8_t(vru_.componentId);
    sendAutopilotMessage(MAVLink_MSG_MISSION_SET_CURRENT::getID(), autopilot::payloadOf(message), MAVLink_MSG_MISSION_SET_CURRENT::v1Length());
    core.consoleInfo(QString("<< MAVLink: MISSION_SET_CURRENT %1 to sys %2").arg(seq).arg(vru_.systemId));
}

void DeviceManager::requestAutopilotStreams()
{
    if (!vru_.online || vru_.systemId < 0 || autopilotLink_ == nullptr) {
        return;
    }
    ++streamRequests_;
    streamRequestMs_ = heartbeatClock_.elapsed();
    if (streamIntervalUnsupported_) {
        sendDataStreamRequest();
        return;
    }
    sendCommandLong(MavCmdSetMessageInterval, float(MAVLink_MSG_MISSION_CURRENT::getID()), kMissionCurrentIntervalUs);
    sendCommandLong(MavCmdSetMessageInterval, float(MAVLink_MSG_NAV_CONTROLLER_OUTPUT::getID()), kNavOutputIntervalUs);
    sendCommandLong(MavCmdSetMessageInterval, float(MAVLink_MSG_GPS_RAW_INT::getID()), kGpsRawIntervalUs);
}

void DeviceManager::sendDataStreamRequest()
{
    MAVLink_MSG_REQUEST_DATA_STREAM request;
    request.req_message_rate = kExtendedStatusRateHz;
    request.target_system = uint8_t(vru_.systemId);
    request.target_component = uint8_t(vru_.componentId);
    request.req_stream_id = kMavDataStreamExtendedStatus;
    request.start_stop = 1;
    sendAutopilotMessage(MAVLink_MSG_REQUEST_DATA_STREAM::getID(), autopilot::payloadOf(request), MAVLink_MSG_REQUEST_DATA_STREAM::v1Length());
    core.consoleInfo(QString("<< MAVLink: REQUEST_DATA_STREAM extended status %1 Hz to sys %2").arg(kExtendedStatusRateHz).arg(vru_.systemId));
}

void DeviceManager::resetMissionTelemetry()
{
    missionTelemetry_ = MissionTelemetry();
    navOutputMs_ = -1;
    pendingMissionCurrentSeq_ = -1;
    lastMissionCurrentMs_ = -1;
    streamRequestMs_ = -1;
    streamRequests_ = 0;
    statusText_.clear();
    statusTextId_ = 0;
    vru_.latitude = NAN;
    vru_.longitude = NAN;
    gpsFixType_ = -1;
    gpsSatellites_ = -1;
    gpsHdop_ = NAN;
    gpsMs_ = -1;
}

void DeviceManager::handleStatusText(const MAVLink_MSG_STATUSTEXT& message)
{
    const char* begin = std::begin(message.text);
    const char* end = std::find(begin, std::end(message.text), '\0');
    const QString part = QString::fromUtf8(begin, int(end - begin));
    const bool lastChunk = end != std::end(message.text);
    if (message.id == 0) {
        emitStatusText(message.severity, part);
        return;
    }
    if (int(message.id) != statusTextId_ || message.chunk_seq == 0) {
        flushStatusText();
        statusTextId_ = message.id;
        statusTextSeverity_ = message.severity;
    }
    statusText_ += part;
    if (lastChunk) {
        flushStatusText();
    }
}

void DeviceManager::flushStatusText()
{
    if (!statusText_.isEmpty()) {
        emitStatusText(statusTextSeverity_, statusText_);
    }
    statusText_.clear();
    statusTextId_ = 0;
}

void DeviceManager::emitStatusText(int severity, const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    core.consoleInfo(QString(">> FC [%1]: %2").arg(severity).arg(trimmed));
    emit autopilotStatusText(severity, trimmed);
}

void DeviceManager::sendCommandLong(uint16_t command, float p1, float p2, float p3, float p4, float p5, float p6, float p7)
{
    if (!vru_.online || vru_.systemId < 0 || autopilotLink_ == nullptr) {
        emit autopilotCommandAcked(command, MavResultNotSent);
        return;
    }

    MAVLink_MSG_COMMAND_LONG cmd;
    cmd.param1 = p1;
    cmd.param2 = p2;
    cmd.param3 = p3;
    cmd.param4 = p4;
    cmd.param5 = p5;
    cmd.param6 = p6;
    cmd.param7 = p7;
    cmd.command = command;
    cmd.target_system = uint8_t(vru_.systemId);
    cmd.target_component = uint8_t(vru_.componentId);
    cmd.confirmation = 0;

    emit writeMavlinkBytes(autopilot::encodeFrame(vru_.mavlinkVersion, mavlinkSeq_++, MAVLink_MSG_COMMAND_LONG::getID(),
                                                autopilot::payloadOf(cmd), MAVLink_MSG_COMMAND_LONG::v1Length()));
    core.consoleInfo(QString("<< MAVLink: COMMAND_LONG %1 (%2, %3) to sys %4").arg(command).arg(p1).arg(p2).arg(vru_.systemId));
}

double DeviceManager::vehicleHomeLat()
{
    return vru_.homeLat;
}

double DeviceManager::vehicleHomeLon()
{
    return vru_.homeLon;
}

void DeviceManager::requestVehicleHome()
{
    sendCommandLong(MavCmdRequestMessage, float(MAVLink_MSG_HOME_POSITION::getID()));
}

void DeviceManager::sendAutopilotMessage(quint32 msgId, const QByteArray& payload, int v1Length)
{
    if (!vru_.online || autopilotLink_ == nullptr) {
        return;
    }
    emit writeMavlinkBytes(autopilot::encodeFrame(vru_.mavlinkVersion, mavlinkSeq_++, msgId, payload, v1Length));
}

void DeviceManager::startMissionUpload(const autopilot::MissionBatches& batches)
{
    if (!vru_.online || vru_.systemId < 0 || autopilotLink_ == nullptr) {
        emit missionUploadRefused(autopilot::MissionTransfer::ResultNoLink, MavMissionTypeMission);
        return;
    }
    QStringList sizes;
    for (const auto& b : batches) {
        sizes.append(QString::number(b.items.size()));
    }
    core.consoleInfo(QString("<< MAVLink: mission upload (route/fence/rally items %1) to sys %2").arg(sizes.join(QLatin1Char('/'))).arg(vru_.systemId));
    if (vru_.mavlinkVersion == 1) {
        autopilot::MissionBatches routeOnly;
        for (const auto& b : batches) {
            if (b.missionType == MavMissionTypeMission) {
                routeOnly.append(b);
            } else if (!b.items.isEmpty()) {
                emit missionUploadRefused(autopilot::MissionTransfer::ResultMavlink1, MavMissionTypeMission);
                return;
            }
        }
        emit missionUploadStart(routeOnly, vru_.systemId, vru_.componentId);
        return;
    }
    emit missionUploadStart(batches, vru_.systemId, vru_.componentId);
}

void DeviceManager::startMissionDownload()
{
    if (!vru_.online || vru_.systemId < 0 || autopilotLink_ == nullptr) {
        emit missionDownloadRefused(autopilot::MissionTransfer::ResultNoLink);
        return;
    }
    core.consoleInfo(QString("<< MAVLink: mission download (route/fence/rally) from sys %1").arg(vru_.systemId));
    if (vru_.mavlinkVersion == 1) {
        emit missionDownloadStart({ MavMissionTypeMission }, vru_.systemId, vru_.componentId);
        return;
    }
    emit missionDownloadStart({ MavMissionTypeMission, MavMissionTypeFence, MavMissionTypeRally }, vru_.systemId, vru_.componentId);
}

void DeviceManager::publishVru()
{
    AutopilotState state;
    state.voltage = vru_.voltage;
    state.current = vru_.current;
    state.velocityH = vru_.velocityH;
    state.batteryPercent = vru_.batteryPercent;
    state.armState = vru_.armState;
    state.flightMode = vru_.flightMode;
    state.online = vru_.online;
    state.proxyTraffic = proxyTraffic_;
    state.homeLat = vru_.homeLat;
    state.homeLon = vru_.homeLon;
    state.linkQuality = vru_.online ? linkQuality_ : -1;
    state.radioRssiValid = vru_.online && radioRssiMs_ >= 0;
    state.radioRssi = radioRssi_;
    state.latitude = vru_.latitude;
    state.longitude = vru_.longitude;
    state.gpsFixType = gpsFixType_;
    state.gpsSatellites = gpsSatellites_;
    state.gpsHdop = gpsHdop_;
    state.mission = missionTelemetry_;
    emit vruChanged(state);
}

bool DeviceManager::proxyLinkActive()
{
    return proxyTraffic_;
}

void DeviceManager::checkAutopilotOnline()
{
    if (!vru_.online) {
        autopilotTimer_.stop();
        return;
    }
    if (heartbeatClock_.elapsed() - vru_.lastHeartbeatMs > kAutopilotHeartbeatTimeoutMs) {
        vru_.online = false;
        vru_.homeLat = NAN;
        vru_.homeLon = NAN;
        autopilotTimer_.stop();
        unbindAutopilotLink();
        resetLinkQuality();
        resetMissionTelemetry();
        publishVru();
        return;
    }
    const qint64 now = heartbeatClock_.elapsed();
    const bool missionCurrentMissing = lastMissionCurrentMs_ < 0 || now - lastMissionCurrentMs_ > kStreamWatchdogMs;
    if (missionCurrentMissing && streamRequests_ < kStreamRequestAttempts && now - streamRequestMs_ > kStreamWatchdogMs) {
        requestAutopilotStreams();
    }
    if (gpsMs_ >= 0 && now - gpsMs_ > kGpsRawTimeoutMs) {
        gpsFixType_ = -1;
        gpsSatellites_ = -1;
        gpsHdop_ = NAN;
        gpsMs_ = -1;
        publishVru();
    }
    if (missionTelemetry_.navValid && heartbeatClock_.elapsed() - navOutputMs_ > kNavOutputTimeoutMs) {
        missionTelemetry_.navValid = false;
        missionTelemetry_.waypointDistance = NAN;
        missionTelemetry_.crossTrackError = NAN;
        publishVru();
    }
    if (proxyTraffic_ && heartbeatClock_.elapsed() - proxyTrafficMs_ > kProxyTrafficTimeoutMs) {
        proxyTraffic_ = false;
        publishVru();
    }
    updateLinkQuality();
}

void DeviceManager::resetLinkQuality()
{
    linkLastSeq_ = -1;
    linkReceived_ = 0;
    linkLost_ = 0;
    linkWindow_.clear();
    linkQuality_ = -1;
    radioRssiMs_ = -1;
}

void DeviceManager::countAutopilotFrame(int seq)
{
    if (linkLastSeq_ >= 0) {
        const int gap = (seq - linkLastSeq_ - 1) & 0xFF;
        if (gap < kLinkMaxGap) {
            linkLost_ += gap;
        }
    }
    linkLastSeq_ = seq;
    ++linkReceived_;
}

void DeviceManager::updateLinkQuality()
{
    linkWindow_.append(qMakePair(linkReceived_, linkLost_));
    while (linkWindow_.size() > kLinkWindowSeconds) {
        linkWindow_.removeFirst();
    }
    linkReceived_ = 0;
    linkLost_ = 0;
    int received = 0;
    int lost = 0;
    for (const auto& bucket : std::as_const(linkWindow_)) {
        received += bucket.first;
        lost += bucket.second;
    }
    const int quality = received + lost > 0 ? int(std::lround(100.0 * received / (received + lost))) : linkQuality_;
    const bool radioStale = radioRssiMs_ >= 0 && heartbeatClock_.elapsed() - radioRssiMs_ > kRadioStatusTimeoutMs;
    if (radioStale) {
        radioRssiMs_ = -1;
    }
    if (quality != linkQuality_ || radioStale) {
        linkQuality_ = quality;
        publishVru();
    }
}

void DeviceManager::bindAutopilotLink(QUuid uuid, Link* link)
{
    if (autopilotLink_ == link && autopilotLinkUuid_ == uuid) {
        return;
    }
    unbindAutopilotLink();
    autopilotLink_ = link;
    autopilotLinkUuid_ = uuid;
    missionSetCurrentUnsupported_ = false;
    streamIntervalUnsupported_ = false;
    resetMissionTelemetry();
    connect(this, &DeviceManager::writeMavlinkBytes, autopilotLink_, &Link::write, Qt::UniqueConnection);
}

void DeviceManager::resetAutopilot()
{
    autopilotTimer_.stop();
    unbindAutopilotLink();
    resetLinkQuality();
    resetMissionTelemetry();
    missionSetCurrentUnsupported_ = false;
    vru_.cleanVru();
}

void DeviceManager::unbindAutopilotLink()
{
    if (autopilotLink_ != nullptr) {
        disconnect(this, &DeviceManager::writeMavlinkBytes, autopilotLink_, &Link::write);
        emit missionTransferAbort(autopilot::MissionTransfer::ResultLinkLost);
    }
    autopilotLink_ = nullptr;
    autopilotLinkUuid_ = QUuid();
}

int DeviceManager::calcAverageChartLosses()
{
    int averageChartLosses = 0;
    int numOfDevices = 0;

    for (auto i = devTree_.cbegin(), end = devTree_.cend(); i != end; ++i) {
        const auto& devs = i.value();

        for (auto k = devs.cbegin(), end = devs.cend(); k != end; ++k) {
            if (!k.value()->isBoardInited()) {
                continue;
            }
            ++numOfDevices;
            averageChartLosses += k.value()->getAverageChartLosses();
        }
    }

    return numOfDevices != 0 ? averageChartLosses / numOfDevices : -1;
}

void DeviceManager::initStreamList()
{
    streamList_.initTimer();
}

QList<DevQProperty *> DeviceManager::getDevList()
{
    devList_.clear();

    for (auto i = devTree_.cbegin(), end = devTree_.cend(); i != end; ++i) {
        const auto& devs = i.value();

        for (auto k = devs.cbegin(), end = devs.cend(); k != end; ++k) {
            devList_.append(k.value());
        }
    }

    return devList_;
}

QList<DevQProperty *> DeviceManager::getDevList(BoardVersion ver) {
    QList<DevQProperty *> list;

    for (auto i = devTree_.cbegin(), end = devTree_.cend(); i != end; ++i) {
        const auto& devs = i.value();

        for (auto k = devs.cbegin(), end = devs.cend(); k != end; ++k) {
            if(k.value()->boardVersion() == ver) {
                list.append(k.value());
            }
        }
    }

    return list;
}

// What a live link carries: every complete frame under its kind, "traffic [link]: ..." in the log once a minute.
// A KP2 frame without the address option keeps the parser's last address (an approximation for the log).
void DeviceManager::countTraffic(const QUuid& uuid, Parsers::FrameParser& frame)
{
    using P = Parsers::FrameParser;
    TrafficTally::Key k;
    const uint8_t* f = frame.frame();
    switch (frame.proto()) {
    case P::ProtoKP1:
        k.proto = TrafficTally::ProtoKp1;
        k.addr = frame.route();
        k.id = frame.id();
        k.ver = frame.ver();
        k.unmarked = !frame.mark();
        break;
    case P::ProtoKP2:
        k.proto = TrafficTally::ProtoKp2;
        k.addr = frame.route();
        k.id = frame.id();
        k.ver = frame.ver();
        k.proxy = frame.isProxy();
        k.stream = frame.isStream();
        k.unmarked = !k.proxy && !frame.mark();   // a proxy frame's flags come from its content
        break;
    case P::ProtoMAVLink1:
        k.proto = TrafficTally::ProtoMavlink1;
        k.id = f[5];
        break;
    case P::ProtoMAVLink2:
        k.proto = TrafficTally::ProtoMavlink2;
        k.id = uint32_t(f[7]) | (uint32_t(f[8]) << 8) | (uint32_t(f[9]) << 16);
        break;
    case P::ProtoNMEA:
        k.proto = TrafficTally::ProtoNmea;
        break;
    case P::ProtoUBX:
        k.proto = TrafficTally::ProtoUbx;
        k.id = (uint32_t(f[2]) << 8) | f[3];
        break;
    default:
        k.proto = TrafficTally::ProtoOther;
        break;
    }
    TrafficTally& t = traffic_[uuid];
    t.add(k, frame.frameLen(), frame.isNested());
    if (!frame.isNested()) {   // a proxy frame's content is in its bytes already
        if (!airClock_.isValid())
            airClock_.start();
        air_.add(AirMeter::kindOf(k), frame.frameLen(), airClock_.elapsed());
        if (!airTimer_) {
            airTimer_ = new QTimer(this);
            connect(airTimer_, &QTimer::timeout, this, &DeviceManager::emitAir);
            airTimer_->start(1000);
        }
    }
    QElapsedTimer& clock = trafficClock_[uuid];
    if (!clock.isValid()) {
        clock.start();
    } else if (clock.elapsed() >= 60000) {
        const QString link8 = uuid.toString(QUuid::WithoutBraces).left(8);
        const double seconds = clock.elapsed() / 1000.0;
        qInfo().noquote() << QStringLiteral("traffic [%1]: %2").arg(link8, t.summary(seconds));
        qInfo().noquote() << QStringLiteral("traffic [%1] sent: %2").arg(link8, sent_[uuid].summary(seconds));
        clock.restart();
    }
}

void DeviceManager::emitAir()
{
    const auto r = air_.rates(airClock_.elapsed());
    emit airChanged(r[AirMeter::Echogram], r[AirMeter::Telemetry], r[AirMeter::Service], wifiLinkRssi());
}

// The head unit's side of the radio link: a Wi-Fi module that is a station (wifiState 3, connected) reports the
// signal of the access point it is on; failing that, an access point (4) reports its weakest client. 0 when none.
int DeviceManager::wifiLinkRssi()
{
    int station = 0, ap = 0;
    for (auto l = devTree_.cbegin(); l != devTree_.cend(); ++l) {
        for (auto d = l.value().cbegin(); d != l.value().cend(); ++d) {
            DevQProperty* dev = d.value();
            if (!dev || !dev->isWifi())
                continue;
            const int state = dev->wifiState();
            const int rssi = dev->wifiRssi();
            if (rssi >= 0 || rssi < -127)
                continue;
            if (state == 3)
                station = station ? std::min(station, rssi) : rssi;
            else if (state == 4)
                ap = ap ? std::min(ap, rssi) : rssi;
        }
    }
    return station ? station : ap;
}

void DeviceManager::frameInput(QUuid uuid, Link* link, Parsers::FrameParser frame)
{
    if (loggingStarted_) {
        emit sendFrameInputToLogger(uuid, link, frame);
    }

    if (frame.isComplete()) {
        if (link != nullptr)
            countTraffic(uuid, frame);

        if (frame.isStream())
            streamList_.append(&frame);
        if (frame.id() == ID_STREAM)
            streamList_.parse(&frame);
        if (streamList_.isListChenged()) {
            emit streamChanged();
            // qInfo("stream-list: %d logs", streamList_.streamsList()->size());
            static const QString kAutoDl = qEnvironmentVariable("KOGGER_AUTODOWNLOAD");
            if (!kAutoDl.isEmpty() && !autoDownloadStarted_) {
                autoDownloadStarted_ = true;
                startStreamDownload(kAutoDl.toInt());
            }
        }

        if (link != nullptr) {
            if (frame.isProxy() || frame.completeAsKBP()) {
                otherProtocolStat_.remove(uuid);
            }
        }

        if (frame.isProxy()) {
            return; //continue;
        }

        if (link != nullptr)
            linkOf_[uuid] = link;
        // A ring firmware update sees every frame of its line. The bootloader answers on route 0: those are
        // not a device of their own, so they stop here while the update runs.
        if (ringUp_ && ringUp_->active() && uuid == ringLink_ && frame.completeAsKBP()) {
            ringUp_->onFrame(QByteArray(reinterpret_cast<const char*>(frame.frame()), frame.frameLen()));
            if (frame.route() == 0)
                return;
        }

        // Changing the line rate: every frame of the line goes to the procedure, which owns the line meanwhile.
        if (rate_ && rate_->active() && uuid == rateLink_) {
            if (frame.completeAsKBP())
                rate_->onFrame(QByteArray(reinterpret_cast<const char*>(frame.frame()), frame.frameLen()));
            if (frame.completeAsKBP() || frame.completeAsKBP2())
                return;
        }

        // Setting a bus address: the board answers on its old route, the new one and a temporary one in
        // turn. Every frame of its line goes to the procedure only, or each route would become a device.
        if (busAddr_ && busAddr_->active() && uuid == busLink_) {
            if (frame.completeAsKBP())
                busAddr_->onFrame(QByteArray(reinterpret_cast<const char*>(frame.frame()), frame.frameLen()));
            if (frame.completeAsKBP() || frame.completeAsKBP2())
                return;
        }

        if (frame.completeAsKBP() || frame.completeAsKBP2()) {
            DevQProperty* dev = getDevice(uuid, link, frame.route());

            if (isConsoled_ && link && frame.id() != 32 && frame.id() != 33) { // link ptr check added
                core.consoleProto(frame);
            }

#if !defined(Q_OS_ANDROID)
            if (frame.id() == ID_TIMESTAMP && frame.ver() == v1) {
                int t = static_cast<int>(frame.read<U4>());
                int u = static_cast<int>(frame.read<U4>());
                emit eventComplete(t, 0, u);
            }

            if (frame.id() == ID_EVENT) {
                int timestamp = frame.read<U4>();
                int id = frame.read<U4>();
                if (id < 100) {
                    emit eventComplete(timestamp, id, 0);
                }

            }

            // No ID_VOLTAGE here: DevDriver parses it (IDBinVoltage), and a read here moved the frame past
            // its payload first -- the parser then read channel 0, value 0, every frame (2026-10-19).
#endif
            dev->protoComplete(frame);
            if (frame.route() != 0) {   // the line's search waits while its devices answer (DeviceLiveness::searchHeld)
                const auto line = devTree_.constFind(uuid);
                if (line != devTree_.cend()) {
                    if (DevQProperty* search = line->value(0, nullptr)) {
                        search->holdSearch();
                    }
                }
            }
            autoRingUpgrade(uuid);
            autoBusAddress(uuid);
            autoRingRate(uuid);
        }

        if (frame.isCompleteAsNMEA()) {
            ProtoNMEA& prot_nmea = (ProtoNMEA&)frame;
            QString str_data = QByteArray((char*)prot_nmea.frame(), prot_nmea.frameLen() - 2);
            if (nmeaConsoled_) {
                core.consoleProtoText(QString(">> NMEA: %5").arg(str_data));
            }
            if (prot_nmea.isEqualId("DBT")) {
                prot_nmea.skip();
                prot_nmea.skip();
                double depth_m = prot_nmea.readDouble();
                if (isfinite(depth_m)) {
                    if (auto* dev = getDevice(uuid, link, frame.route()); dev) { // work?
                        emit rangefinderComplete(dev->getChannelId(), depth_m);
                    }
                }
            }

            if (prot_nmea.isEqualId("RMC")) {
                uint8_t h = 0, m = 0, s = 0;
                uint16_t ms = 0;

                bool isCorrect =  prot_nmea.readTime(&h, &m, &s, &ms);

                char c = prot_nmea.readChar();
                if (c == 'A' || c == 'D') {
                    double lat = prot_nmea.readLatitude();
                    double lon = prot_nmea.readLongitude();

                    prot_nmea.skip();
                    prot_nmea.skip();

                    uint16_t year = 0;
                    uint8_t month = 0, day = 0;
                    prot_nmea.readDate(&year, &month, & day);

                    QDate date(year, month, day);
                    QTime time(h, m, s);

                    QDateTime dt(date, time, QTimeZone::utc());
                    uint32_t unix_time = static_cast<uint32_t>(dt.toSecsSinceEpoch());

                    emit positionComplete(lat, lon, unix_time, (uint32_t)ms*1000*1000);
                    if (isCorrect && date.isValid()) {
                        emit gnssUtcComplete(qint64(unix_time) * 1000000 + qint64(ms) * 1000);
                    }
                }
            }

            if (prot_nmea.isEqualId("GGA")) {
                uint8_t h = 0, m = 0, s = 0;
                uint16_t ms = 0;

                bool isCorrect =  prot_nmea.readTime(&h, &m, &s, &ms);
                Q_UNUSED(isCorrect);

                double lat = prot_nmea.readLatitude();
                double lon = prot_nmea.readLongitude();

                char q = prot_nmea.readChar();

                prot_nmea.skip(); // sv
                prot_nmea.skip(); // HDOP

                float height_msl = prot_nmea.readDouble(); // Orthometric height (MSL reference)
                prot_nmea.skip();
                const double geoid_separation = prot_nmea.readDouble();

                if (q == '1' || q == '2' || q == '4' || q == '5') {
                    uint16_t year = 1971;
                    uint8_t month = 1, day = 1;

                    Position pos;
                    pos.lla.latitude = lat;
                    pos.lla.longitude = lon;
                    pos.lla.altitude = height_msl;
                    pos.lla.source = PositionSourceRTK;
                    pos.lla.altSource = AltitudeSourceRTK;
                    pos.time = DateTime(year, month, day, h, m, s, int64_t(ms)*1000*1000);

                    if(q == '4') {
                        emit positionCompleteRTK(pos);
                        if (isfinite(height_msl)) {
                            emit gnssAltitudeMslComplete(height_msl);
                            if (isfinite(geoid_separation)) {
                                emit gnssAltitudeEllipsoidComplete(double(height_msl) + geoid_separation);
                            }
                        }
                    }
                }
            }

            if (prot_nmea.isEqualId("HDT")) {
                const double headingDeg = prot_nmea.readDouble();
                //const char reference = prot_nmea.readChar();
                const bool headingValid = isfinite(headingDeg);

                if (/*reference == 'T' && TODO: check this*/ headingValid) {
                    //qDebug().noquote() << QString("NMEA HDT parsed: heading=%1 deg true").arg(heading_deg, 0, 'f', 3);
                    passAttitude(AttitudeArbiter::KindGnssHeading, 0, static_cast<float>(headingDeg), 0.0f, 0.0f);
                }
                else {
                    //qDebug().noquote() << QString("NMEA HDT rejected: heading=%1 ref=%2")
                    //                      .arg(heading_valid ? QString::number(heading_deg, 'f', 3) : QStringLiteral("nan"))
                    //                      .arg(reference);
                }
            }
        }

        if (frame.isCompleteAsUBX()) {
            ProtoUBX& ubx_frame = (ProtoUBX&)frame;

            if (ubx_frame.msgClass() == 1 && ubx_frame.msgId() == 7) {

                uint8_t h = 0, m = 0, s = 0;
                uint16_t year = 0;
                uint8_t month = 0, day = 0;
                int32_t nanosec = 0;

                ubx_frame.readSkip(4);
                year = ubx_frame.read<U2>();
                month = ubx_frame.read<U1>();
                day = ubx_frame.read<U1>();
                h = ubx_frame.read<U1>();
                m = ubx_frame.read<U1>();
                s = ubx_frame.read<U1>();
                uint8_t time_valid = ubx_frame.read<U1>();
                ubx_frame.readSkip(4); // Time accuracy estimate (UTC)
                nanosec = ubx_frame.read<S4>();

                uint8_t fix_type = ubx_frame.read<U1>();
                uint8_t fix_flags = ubx_frame.read<U1>();

                ubx_frame.read<U1>();
                uint8_t satellites_in_used = ubx_frame.read<U1>();
                Q_UNUSED(satellites_in_used)

                int32_t lon_int = ubx_frame.read<S4>();
                int32_t lat_int = ubx_frame.read<S4>();

                const int32_t height_mm = ubx_frame.read<S4>();
                const bool is_rtk_fixed = (fix_flags & 0x01) && (fix_flags >> 6) == 2;
                if (is_rtk_fixed && ubx_frame.readAvailable() >= 4) {
                    const int32_t hmsl_mm = ubx_frame.read<S4>();
                    emit gnssAltitudeMslComplete(double(hmsl_mm) * 0.001);
                    emit gnssAltitudeEllipsoidComplete(double(height_mm) * 0.001);
                }

                QDate date(year, month, day);
                QTime time(h, m, s);

                QDateTime dt(date, time, QTimeZone::utc());
                uint32_t unix_time = static_cast<uint32_t>(dt.toSecsSinceEpoch());

                if (fix_type > 1 && fix_type < 5) {
                    emit positionComplete(double(lat_int)*0.0000001, double(lon_int)*0.0000001, unix_time, nanosec);
                    if ((time_valid & 0x03) == 0x03) {
                        emit gnssUtcComplete(qint64(unix_time) * 1000000 + nanosec / 1000);
                    }
                }

                // if (isConsoled_) {
                    core.consoleStreamInfo(QString(">> UBX: NAV_PVT, fix %1, sats %2, lat %3, lon %4, time %5:%6:%7.%8")
                                         .arg(fix_type).arg(satellites_in_used).arg(double(lat_int)*0.0000001).arg(double(lon_int)*0.0000001).arg(h).arg(m).arg(s).arg(nanosec/1000));
                    // }
            }
            else {
                // if (isConsoled_)
                    core.consoleStreamInfo(QString(">> UBX: class/id 0x%1 0x%2, len %3").arg(ubx_frame.msgClass(), 2, 16, QLatin1Char('0')).arg(ubx_frame.msgId(), 2, 16, QLatin1Char('0')).arg(ubx_frame.frameLen()));
            }
        }

        if (frame.isCompleteAsMAVLink()) {
            if (link == nullptr || proxyLinkUuid_ != uuid) {
                emit writeProxyFrame(frame);

                // The external GCS's frames go to the last link MAVLink came on; the app's own bytes
                // (writeMavlinkBytes) to the autopilot's link only, once it is bound (bindAutopilotLink).
                if (link != nullptr && mavlinUuid_ != uuid) {
                    mavlinUuid_ = uuid;
                    if(mavlinkLink_ != nullptr) {
                        disconnect(this, &DeviceManager::writeMavlinkFrame,  mavlinkLink_, &Link::writeFrame);
                    }
                    mavlinkLink_ = link;
                    connect(this, &DeviceManager::writeMavlinkFrame, mavlinkLink_, &Link::writeFrame, Qt::UniqueConnection);
                }

                if (link != nullptr) {   // live links only: a replayed log must not look like a boat
                    emit mavlinkFrameReceived(QByteArray(reinterpret_cast<const char*>(frame.frame()), frame.frameLen()));
                }

                ProtoMAVLink& mavlink_frame = (ProtoMAVLink&)frame;
                const bool fromAutopilot = !vru_.online
                                           || (uuid == autopilotLinkUuid_ && int(mavlink_frame.systemID()) == vru_.systemId);
                if (vru_.online && uuid == autopilotLinkUuid_) {
                    if (int(mavlink_frame.systemID()) == vru_.systemId && int(mavlink_frame.componentID()) == vru_.componentId) {
                        countAutopilotFrame(mavlink_frame.sequenceNumber());
                    }
                    const bool radioStatus = mavlink_frame.msgId() == MAVLink_MSG_RADIO_STATUS::getID();
                    const auto radio = radioStatus ? mavlink_frame.read<MAVLink_MSG_RADIO_STATUS>() : MAVLink_MSG_RADIO_STATUS{};
                    if (radioStatus && radio.rssi != kRadioRssiUnknown) {
                        const bool sik = mavlink_frame.systemID() == kSikSystemId && mavlink_frame.componentID() == kSikComponentId;
                        const int dbm = radioDbm(radio.rssi, sik);
                        const bool changed = radioRssiMs_ < 0 || dbm != radioRssi_;
                        radioRssi_ = dbm;
                        radioRssiMs_ = heartbeatClock_.elapsed();
                        if (changed) {
                            publishVru();
                        }
                    }
                }

                // if (mavlink_frame.msgId() == 24) { // GLOBAL_POSITION_INT
                //     MAVLink_MSG_GPS_RAW_INT pos = mavlink_frame.read<MAVLink_MSG_GPS_RAW_INT>();
                //     if (pos.isValid()) {
                //         emit positionComplete(pos.latitude(), pos.longitude(), pos.time_boot_msec()/1000, (pos.time_boot_msec()%1000)*1e6);
                //         emit gnssVelocityComplete(pos.velocityH(), 0);
                //         vru_.velocityH = pos.velocityH();
                //         emit vruChanged();
                //     }
                // }

                if(mavlink_frame.msgId() == MAVLink_MSG_GLOBAL_POSITION_INT::getID()) {
                    MAVLink_MSG_GLOBAL_POSITION_INT pos = mavlink_frame.read<MAVLink_MSG_GLOBAL_POSITION_INT>();
                    if (pos.isValid()) {
                        if (fromAutopilot && vru_.online) {
                            vru_.latitude = pos.latitude();
                            vru_.longitude = pos.longitude();
                        }
                        emit positionComplete(pos.latitude(), pos.longitude(), pos.time_boot_msec()/1000, (pos.time_boot_msec()%1000)*1e6);
                        if (mavlinkUnixOffsetValid_ && mavlink_frame.systemID() == mavlinkUnixOffsetSysId_) {
                            if (pos.time_boot_msec() < mavlinkLastPositionBootMs_) {
                                mavlinkUnixOffsetValid_ = false;
                            }
                            mavlinkLastPositionBootMs_ = pos.time_boot_msec();
                            if (mavlinkUnixOffsetValid_) {
                                emit gnssUtcComplete(qint64(pos.time_boot_msec()) * 1000 + mavlinkUnixOffsetUs_);
                            }
                        }
                        emit gnssVelocityComplete(pos.velocityH(), 0);
                        vru_.velocityH = pos.velocityH();
                        publishVru();
                    }
                }

                if (mavlink_frame.msgId() == MAVLink_MSG_SYSTEM_TIME::getID()) {
                    MAVLink_MSG_SYSTEM_TIME system_time = mavlink_frame.read<MAVLink_MSG_SYSTEM_TIME>();
                    if (system_time.hasUnixTime()) {
                        mavlinkUnixOffsetUs_ = system_time.unixOffsetUs();
                        mavlinkUnixOffsetSysId_ = mavlink_frame.systemID();
                        mavlinkUnixOffsetValid_ = true;
                        mavlinkLastPositionBootMs_ = 0;
                    }
                }

                const bool isGpsRaw = mavlink_frame.msgId() == MAVLink_MSG_GPS_RAW_INT::getID();
                MAVLink_MSG_GPS_RAW_INT gpsRaw{};
                if (isGpsRaw) {
                    gpsRaw = mavlink_frame.read<MAVLink_MSG_GPS_RAW_INT>();
                    if (gpsRaw.isValid() && gpsRaw.isRtkFixed()) {
                        emit gnssAltitudeMslComplete(gpsRaw.altitudeMsl());
                        if (gpsRaw.hasAltitudeEllipsoid()) {
                            emit gnssAltitudeEllipsoidComplete(gpsRaw.altitudeEllipsoid());
                        }
                    }
                }

                MAVLink_MSG_HEARTBEAT heartbeat;
                bool isAutopilotHeartbeat = false;
                if (fromAutopilot && mavlink_frame.msgId() == MAVLink_MSG_HEARTBEAT::getID()
                    && mavlink_frame.componentID() == kMavCompIdAutopilot1) {
                    heartbeat = mavlink_frame.read<MAVLink_MSG_HEARTBEAT>();
                    isAutopilotHeartbeat = heartbeat.isVehicle();
                }
                if (isAutopilotHeartbeat) {
                    if (link != nullptr && !vru_.online) {
                        bindAutopilotLink(uuid, link);
                    }
                    const bool wasOnline = vru_.online;
                    const int previousArm = vru_.armState;
                    vru_.armState = (int)heartbeat.isArmed();
                    int flight_mode = (int)heartbeat.customMode();
                    if (flight_mode != vru_.flightMode) {
                        core.consoleStreamInfo(QString(">> FC: Flight mode %1").arg(flight_mode));
                    }
                    vru_.flightMode = flight_mode;
                    vru_.systemId = mavlink_frame.systemID();
                    vru_.componentId = mavlink_frame.componentID();
                    vru_.mavlinkVersion = mavlink_frame.MAVLinkVersion();
                    vru_.lastHeartbeatMs = heartbeatClock_.elapsed();
                    vru_.online = link != nullptr;
                    if (vru_.online && !autopilotTimer_.isActive()) {
                        autopilotTimer_.start();
                    }
                    if (vru_.online && !wasOnline) {
                        resetLinkQuality();
                    }
                    if (vru_.online && (!wasOnline || (previousArm == 0 && vru_.armState == 1))) {
                        requestVehicleHome();
                    }
                    if (vru_.online && !wasOnline) {
                        requestAutopilotStreams();
                    }
                    publishVru();
                }

                if (fromAutopilot && vru_.online && mavlink_frame.msgId() == MAVLink_MSG_COMMAND_ACK::getID()
                    && int(mavlink_frame.componentID()) == vru_.componentId) {
                    MAVLink_MSG_COMMAND_ACK ack = mavlink_frame.read<MAVLink_MSG_COMMAND_ACK>();
                    const bool addressedToUs = (ack.target_system == 0 || ack.target_system == kMavGcsSystemId)
                                               && (ack.target_component == 0 || ack.target_component == kMavGcsComponentId);
                    if (addressedToUs) {
                        core.consoleInfo(QString(">> MAVLink: COMMAND_ACK %1 result %2").arg(ack.command).arg(ack.result));
                        bool forwardAck = true;
                        if (ack.command == MavCmdSetMessageInterval && ack.result == MavResultUnsupported && !streamIntervalUnsupported_) {
                            streamIntervalUnsupported_ = true;
                            sendDataStreamRequest();
                        }
                        if (ack.command == MavCmdDoSetMissionCurrent && pendingMissionCurrentSeq_ >= 0 && ack.result != MavResultInProgress) {
                            const int seq = pendingMissionCurrentSeq_;
                            pendingMissionCurrentSeq_ = -1;
                            if (ack.result == MavResultUnsupported) {
                                missionSetCurrentUnsupported_ = true;
                                forwardAck = false;
                                sendMissionSetCurrent(seq);
                            }
                        }
                        if (forwardAck) {
                            emit autopilotCommandAcked(ack.command, ack.result);
                        }
                    }
                }

                const quint32 msgId = mavlink_frame.msgId();
                if (fromAutopilot && vru_.online
                    && (msgId == MAVLink_MSG_MISSION_REQUEST::getID() || msgId == MAVLink_MSG_MISSION_REQUEST_INT::getID()
                        || msgId == MAVLink_MSG_MISSION_ACK::getID() || msgId == MAVLink_MSG_MISSION_COUNT::getID()
                        || msgId == MAVLink_MSG_MISSION_ITEM_INT::getID() || msgId == MAVLink_MSG_PARAM_VALUE::getID())) {
                    const uint16_t length = mavlink_frame.payloadLen();
                    emit missionFrameReceived(msgId, QByteArray(reinterpret_cast<const char*>(mavlink_frame.read(length)), length));
                }

                const bool fromAutopilotComponent = fromAutopilot && vru_.online && int(mavlink_frame.componentID()) == vru_.componentId;
                if (fromAutopilotComponent && msgId == MAVLink_MSG_MISSION_CURRENT::getID()) {
                    const auto current = mavlink_frame.read<MAVLink_MSG_MISSION_CURRENT>();
                    MissionTelemetry next = missionTelemetry_;
                    next.seq = current.seq;
                    next.total = current.total != 0 ? int(current.total) : -1;
                    next.state = current.mission_state;
                    next.missionId = current.mission_id;
                    next.reports = missionTelemetry_.reports + 1;
                    lastMissionCurrentMs_ = heartbeatClock_.elapsed();
                    if (next.seq != missionTelemetry_.seq) {
                        core.consoleInfo(QString(">> MAVLink: MISSION_CURRENT %1 (total %2, state %3)").arg(next.seq).arg(next.total).arg(next.state));
                    }
                    missionTelemetry_ = next;
                    publishVru();
                }

                if (fromAutopilotComponent && msgId == MAVLink_MSG_MISSION_ITEM_REACHED::getID()) {
                    const auto reached = mavlink_frame.read<MAVLink_MSG_MISSION_ITEM_REACHED>();
                    missionTelemetry_.reachedSeq = reached.seq;
                    core.consoleInfo(QString(">> MAVLink: MISSION_ITEM_REACHED %1").arg(reached.seq));
                    emit autopilotMissionItemReached(reached.seq);
                    publishVru();
                }

                if (fromAutopilotComponent && msgId == MAVLink_MSG_NAV_CONTROLLER_OUTPUT::getID()) {
                    const auto nav = mavlink_frame.read<MAVLink_MSG_NAV_CONTROLLER_OUTPUT>();
                    const float distance = float(nav.wp_dist);
                    const bool changed = !missionTelemetry_.navValid || distance != missionTelemetry_.waypointDistance
                                         || std::abs(nav.xtrack_error - missionTelemetry_.crossTrackError) >= kCrossTrackStepM;
                    missionTelemetry_.navValid = true;
                    missionTelemetry_.waypointDistance = distance;
                    missionTelemetry_.crossTrackError = nav.xtrack_error;
                    navOutputMs_ = heartbeatClock_.elapsed();
                    if (changed) {
                        publishVru();
                    }
                }

                if (fromAutopilotComponent && isGpsRaw) {
                    const int length = mavlink_frame.payloadLen();
                    const MAVLink_MSG_GPS_RAW_INT& gps = gpsRaw;
                    const int fixType = length > kGpsRawFixTypeOffset ? int(gps.fix_type) : 0;
                    const int satellites = length > kGpsRawSatellitesOffset
                                               ? (gps.satellites_visible == kGpsSatellitesUnknown ? -1 : int(gps.satellites_visible))
                                               : 0;
                    const double hdop = gps.eph == kGpsHdopUnknown ? NAN : double(gps.eph) / kGpsHdopScale;
                    const bool changed = gpsMs_ < 0 || fixType != gpsFixType_ || satellites != gpsSatellites_
                                         || (std::isfinite(hdop) != std::isfinite(gpsHdop_))
                                         || (std::isfinite(hdop) && std::abs(hdop - gpsHdop_) >= 0.05);
                    gpsFixType_ = fixType;
                    gpsSatellites_ = satellites;
                    gpsHdop_ = hdop;
                    gpsMs_ = heartbeatClock_.elapsed();
                    if (changed) {
                        publishVru();
                    }
                }

                if (fromAutopilot && vru_.online && msgId == MAVLink_MSG_STATUSTEXT::getID()) {
                    handleStatusText(mavlink_frame.read<MAVLink_MSG_STATUSTEXT>());
                }

                if (fromAutopilot && vru_.online && msgId == MAVLink_MSG_HOME_POSITION::getID()) {
                    const MAVLink_MSG_HOME_POSITION home = mavlink_frame.read<MAVLink_MSG_HOME_POSITION>();
                    vru_.homeLat = double(home.latitude) / 1.0e7;
                    vru_.homeLon = double(home.longitude) / 1.0e7;
                    publishVru();
                }

                if (fromAutopilot && mavlink_frame.msgId() == 147) { // BATTERY_STATUS
                    MAVLink_MSG_BATTERY_STATUS battery_status = mavlink_frame.read<MAVLink_MSG_BATTERY_STATUS>();
                    vru_.voltage = battery_status.voltage();
                    vru_.current = battery_status.current();
                    vru_.batteryPercent = battery_status.battery_remaining >= 0 ? int(battery_status.battery_remaining) : -1;
                    publishVru();
                }

                if (mavlink_frame.msgId() == 30) {
                    MAVLink_MSG_ATTITUDE attitude = mavlink_frame.read<MAVLink_MSG_ATTITUDE>();

                    const float yaw = attitude.yawDeg();
                    const float pitch = attitude.pitchDeg();
                    const float roll = attitude.rollDeg();

                    if (!qFuzzyIsNull(yaw) || !qFuzzyIsNull(pitch) || !qFuzzyIsNull(roll)) {
                        passAttitude(AttitudeArbiter::KindAutopilot,
                                     (quint64(mavlink_frame.systemID()) << 8) | mavlink_frame.componentID(),
                                     yaw, attitude.pitchDeg(), attitude.rollDeg());
                    }
                }
                if (mavlinkConsoled_) {
                    core.consoleProtoText(QString(">> MAVLink v%1: ID %2, comp. id %3, seq numb %4, len %5").arg(mavlink_frame.MAVLinkVersion()).arg(mavlink_frame.msgId()).arg(mavlink_frame.componentID()).arg(mavlink_frame.sequenceNumber()).arg(mavlink_frame.frameLen()));
                }
            }
            else {
                if (link != nullptr) {
                    proxyTrafficMs_ = heartbeatClock_.elapsed();
                    if (!proxyTraffic_) {
                        proxyTraffic_ = true;
                        publishVru();
                    }
                    emit writeMavlinkFrame(frame);
                }
            }
        }

        if (link != nullptr) {
            if ((frame.isCompleteAsNMEA() && !((ProtoNMEA*)&frame)->isEqualId("DBT")) ||
                frame.isCompleteAsUBX() ||
                frame.isCompleteAsMAVLink()) {
                if (!frame.isNested()) {
                    otherProtocolStat_[uuid]++;
                    if (otherProtocolStat_[uuid] > 30) {
                        deleteDevicesByLink(uuid);
                    }
                }
            }
        }
    }
}

void DeviceManager::openFile(QString filePath)
{
    QFile file;
    const QUrl url(filePath);
    url.isLocalFile() ? file.setFileName(url.toLocalFile()) : file.setFileName(url.toString());

    if (!file.open(QIODevice::ReadOnly)) {
        emit fileStopsOpening();
        return;
    }

    const qint64 totalSize = file.size();
    qint64 bytesRead = 0;
    Parsers::FrameParser frameParser;
    const QUuid someUuid(kFileUuidStr);

    delAllDev();

    epochFileOffsets_.clear();

    qint64 pendingFrameStart = -1;
    qint64 lastFrameStart = -1;
    bool justWrapped = false;

    while (true) {
        QByteArray chunk = file.read(static_cast<qint64>(1024) * 1024);
        const qint64 chunkSize = chunk.size();

        if (chunkSize == 0)
            break;

        bytesRead += chunkSize;

        auto currProgress = static_cast<int>((static_cast<float>(bytesRead) / static_cast<float>(totalSize)) * 100.0f);
        currProgress = std::max(0, currProgress);
        currProgress = std::min(100, currProgress);

        if (progress_ != currProgress) {
            progress_ = currProgress;
        }

        frameParser.setContext((uint8_t*)chunk.data(), chunk.size());

        while (frameParser.availContext() > 0) {

            if (!justWrapped && !frameParser.isNested() && pendingFrameStart < 0) {
                pendingFrameStart = bytesRead - frameParser.availContext();
            }

            frameParser.process();
            if (frameParser.isComplete()) {
                if (!frameParser.isNested()) {
                    lastFrameStart = pendingFrameStart;
                    pendingFrameStart = -1;
                    justWrapped = frameParser.isProxy();
                }
                else {
                    justWrapped = false;
                }

                frameInput(someUuid, nullptr, frameParser);

                if (Dataset* datasetPtr = core.getDatasetPtr(); datasetPtr && lastFrameStart >= 0) {
                    const int epochs = datasetPtr->size();
                    while (epochFileOffsets_.size() < epochs) {
                        epochFileOffsets_.append(lastFrameStart);
                    }
                }
            }
        }

        chunk.clear();
    }
    file.close();

    resetAutopilot();
    delAllDev();
    publishVru();

    emit fileOpened();
    emit fileStopsOpening();
}

void DeviceManager::closeFile()
{
    delAllDev();
    resetAutopilot();
    epochFileOffsets_.clear();
    publishVru();
}

void DeviceManager::onLinkOpened(QUuid uuid, Link *link)
{
    if (link) {
        if (link->getIsProxy()) {
            proxyLinkUuid_ = uuid;
            connect(this, &DeviceManager::writeProxyFrame, link, &Link::writeFrame);
            publishVru();
        } else if(link->attribute() == LinkAttribute::kLinkAttributeBoot) {
            core.consoleInfo("Device: Boot opened");
        } else {
            getDevice(uuid, link, 0);
        }
    }
}

void DeviceManager::onLinkClosed(QUuid uuid, Link *link)
{
    Q_UNUSED(uuid);

    if (link) {
        deleteDevicesByLink(uuid);
        this->disconnect(link);
        otherProtocolStat_.remove(uuid);
        if(uuid == mavlinUuid_) {
            mavlinUuid_ = QUuid();
            mavlinkLink_ = nullptr;   // disconnect(link) above already cut both MAVLink write paths
        }
        if(uuid == proxyLinkUuid_) {
            proxyLinkUuid_ = QUuid();
            proxyTraffic_ = false;
            publishVru();
        }
        if(uuid == autopilotLinkUuid_) {
            resetAutopilot();
            publishVru();
        }
    }
}

void DeviceManager::onLinkDeleted(QUuid uuid, Link *link)
{
    Q_UNUSED(uuid);

    if (link) {
        deleteDevicesByLink(uuid);
        this->disconnect(link);
        otherProtocolStat_.remove(uuid);
        if(uuid == mavlinUuid_) {
            mavlinUuid_ = QUuid();
            mavlinkLink_ = nullptr;   // the Link is going away; never touch the old pointer again
        }
        if(uuid == proxyLinkUuid_) {
            proxyLinkUuid_ = QUuid();
            proxyTraffic_ = false;
            publishVru();
        }
        if(uuid == autopilotLinkUuid_) {
            resetAutopilot();
            publishVru();
        }
    }
}

void DeviceManager::binFrameOut(Parsers::ProtoBinOut protoOut)
{
    // What the drivers send, for the minute's traffic line (countTraffic).
    if (const auto* dev = qobject_cast<DevDriver*>(sender())) {
        TrafficTally::Key k;
        k.proto = TrafficTally::ProtoKp1;
        k.addr = protoOut.route();
        k.id = protoOut.id();
        k.ver = protoOut.ver();
        sent_[dev->getLinkUuid()].add(k, protoOut.frameLen(), false);
    }
    if (isConsoled_ && protoOut.id() != 33) {
        core.consoleProto(protoOut, false);
    }
    emit sendProtoFrame(protoOut);
}

bool DeviceManager::isCreatedId(int id)
{
    return getDevList().size() > id;
}

void DeviceManager::setProtoBinConsoled(bool isConsoled)
{
    isConsoled_ = isConsoled;
}

void DeviceManager::setNmeaConsoled(bool isConsoled)
{
    nmeaConsoled_ = isConsoled;
}

void DeviceManager::setMavlinkConsoled(bool isConsoled)
{
    mavlinkConsoled_ = isConsoled;
}

void DeviceManager::setUsblConsoled(bool isConsoled)
{
    DevDriver::usblConsoled_ = isConsoled;
}

void DeviceManager::upgradeLastDev(QByteArray data)
{
    if (lastDevs_ != nullptr) {
        lastDevs_->sendUpdateFW(data);
    }
}

void DeviceManager::beaconActivationReceive(uint8_t id) {
    Q_UNUSED(id)

    QList<DevQProperty *> usbl_devs = getDevList(BoardUSBL);
    if (!usbl_devs.isEmpty()) {
        IDBinUsblSolution::USBLRequestBeacon ask = {};
        usbl_devs[0]->askBeaconPosition(ask);
    }
}

void DeviceManager::beaconDirectQueueAsk() {
    QList<DevQProperty *> usbl_devs = getDevList(BoardUSBLBeacon);
    qDebug("Sent request to the Beacon # %d", -1);
    if (!usbl_devs.isEmpty()) {
        usbl_devs[0]->enableBeaconOnce(3);
        qDebug("Sent request to the Beacon # %d", 0);
    }
}

void DeviceManager::setUSBLBeaconDirectAsk(bool is_ask) {
    isUSBLBeaconDirectAsk = is_ask;
    qDebug("Beacon auto scan is: %d", is_ask);
    if (is_ask) {
        QObject::connect(&beacon_timer, &QTimer::timeout, this, &DeviceManager::beaconDirectQueueAsk);
        beacon_timer.setInterval(3000);
        beacon_timer.start();
    } else {
        beacon_timer.stop();
    }
}

void DeviceManager::onLoggingKlfStarted(bool started)
{
    loggingStarted_ = started;

    if (loggingStarted_) {
        for (auto i = devTree_.cbegin(), end = devTree_.cend(); i != end; ++i) {
            const auto& devs = i.value();
            for (auto k = devs.cbegin(), end = devs.cend(); k != end; ++k) {
                k.value()->requestSetup();
            }
        }
    }
}

void DeviceManager::onSendRequestAll(QUuid uuid)
{
    if (devTree_.contains(uuid)) {
        const auto& devs = devTree_[uuid];
        for (auto i = devs.cbegin(), end = devs.cend(); i != end; ++i) {
            if (auto* dev = i.value(); dev) {
                dev->doRequestAll();
            }
        }
    }
}

StreamListModel* DeviceManager::streamsList()
{
    return streamList_.streamsList();
}

void DeviceManager::startStreamDownload(int id)
{
    QList<DevQProperty*> recs = getDevList(BoardRecorderMini);
    if (recs.isEmpty()) {
        qInfo("startStreamDownload: no recorder device connected");
        return;
    }
    DevQProperty* rec = recs.first();
    qRegisterMetaType<QVector<quint32>>("QVector<quint32>");
    connect(&streamList_, &StreamList::requestRanges, rec, &DevDriver::requestStreamRanges, Qt::UniqueConnection);
    streamList_.startDownload(id);
}

void DeviceManager::cancelStreamDownload(int id)
{
    streamList_.cancelDownload(id);
}

void DeviceManager::refreshStreamList()
{
    QList<DevQProperty*> recs = getDevList(BoardRecorderMini);
    if (!recs.isEmpty()) {
        if (!streamList_.hasActiveDownload()) {
            streamList_.reset();
            emit streamChanged();
        }
        recs.first()->requestStreamList();
    }
}

void DeviceManager::readyReadProxy(Link* link)
{
    while (link->parse()) {
        FrameParser* frame = link->frameParser();

        if (frame->isComplete()) {
            QByteArray data((char*)frame->frame(), frame->frameLen());
            emit dataSend(data);
        }
    }
}

void DeviceManager::readyReadProxyNav(Link* link)
{
    while (link->parse()) {
        FrameParser* frame = link->frameParser();

        if (frame->isComplete()) {
            QByteArray data((char*)frame->frame(), frame->frameLen());
            emit dataSend(data);
        }
    }
}

void DeviceManager::onStartUpgradingFirmware(QUuid linkUuid, uint8_t address, const QByteArray& firmware)
{
    qDebug() << "DeviceManager::onStartUpgradingFirmware";

    upgradeUuid_ = linkUuid;
    upgradeAddr_ = address;
    upgradeData_ = firmware;
}

void DeviceManager::onUpgradingFirmwareDone()
{
    qDebug() << "DeviceManager::onUpgradingFirmwareDone";

    upgradeUuid_ = QUuid();
    upgradeAddr_ = 0;
    upgradeData_.clear();
}

void DeviceManager::onUpgradeAwaitingStart(QUuid linkUuid, uint8_t address, qint64 toldAtMs)
{
    startWaitUuid_ = linkUuid;
    startWaitAddr_ = address;
    startWaitAt_ = toldAtMs;
    // The board's device waits kUpgradeStartTimeoutMs; one gone with its link and never back cannot say so: this
    // does, a moment later, unless the wait was settled meanwhile.
    const qint64 left = DevDriver::kUpgradeStartTimeoutMs - (QDateTime::currentMSecsSinceEpoch() - toldAtMs);
    QTimer::singleShot(int(std::max<qint64>(0, left) + 2000), this, [this, toldAtMs]() {
        if (startWaitAt_ != toldAtMs) {
            return;
        }
        startWaitAt_ = 0;
        qInfo().noquote() << QStringLiteral("upgrade: no device of the board came back within %1 s after it was told to start")
                                 .arg(DevDriver::kUpgradeStartTimeoutMs / 1000);
        emit upgradeProgressChanged(DevDriver::noAnswerUpgrade);
    });
}

void DeviceManager::onUpgradeStartSettled(QUuid linkUuid, uint8_t address)
{
    if (startWaitUuid_ == linkUuid && startWaitAddr_ == address) {
        startWaitAt_ = 0;
    }
}

void DeviceManager::onRingUpgradeRequested(QUuid linkUuid, int address, QByteArray firmware, QString expect, bool everyBoard)
{
    DevQProperty* requester = devTree_.value(linkUuid).value(uint8_t(address), nullptr);
    Link* link = linkOf_.value(linkUuid).data();
    auto refuse = [&](const QString& why) {
        qInfo().noquote() << "ring-upgrade: refused:" << why;
        if (requester)
            requester->setRingUpgradeStatus(RingUpgrade::kStatusFail, true);
    };
    if ((ringUp_ && ringUp_->active()) || (rate_ && rate_->active()) || (busAddr_ && busAddr_->active())) {
        refuse(QStringLiteral("a ring update, a line rate change or an address change is running"));
        return;
    }
    if (!link) {
        refuse(QStringLiteral("the line of the device is not known"));
        return;
    }

    // The ring boards of that line: Basic2D sonars answering on a bus address 1..15.
    QList<int> boards;
    const auto devs = devTree_.value(linkUuid);
    for (auto it = devs.cbegin(); it != devs.cend(); ++it)
        if (it.value() && it.value()->isRingBoard())
            boards.append(int(it.key()));
    std::sort(boards.begin(), boards.end());

    if (!ringUp_) {
        RingUpgrade::Io io;
        io.write = [this](const QByteArray& bytes) {
            if (QPointer<Link> l = ringLinkPtr_)
                QMetaObject::invokeMethod(l, [l, bytes] { if (l) l->write(bytes); }, Qt::QueuedConnection);
        };
        io.setRate = [this](int rate) {
            if (QPointer<Link> l = ringLinkPtr_)
                QMetaObject::invokeMethod(l, [l, rate] { if (l) l->setBaudrateAfterWrite(rate); }, Qt::QueuedConnection);
        };
        io.nowMs = [this] { return ringClock_.elapsed(); };
        ringUp_ = new RingUpgrade(io, this);
        ringClock_.start();
        ringTimer_ = new QTimer(this);
        ringTimer_->setInterval(5);
        connect(ringTimer_, &QTimer::timeout, ringUp_, &RingUpgrade::tick);
        connect(ringUp_, &RingUpgrade::logLine, this, [](const QString& line) { qInfo().noquote() << line; });
        connect(ringUp_, &RingUpgrade::targetProgress, this, [this](int addr, int pct) {
            if (DevQProperty* dev = devTree_.value(ringLink_).value(uint8_t(addr), nullptr))
                dev->setRingUpgradeStatus(pct, false);
        });
        connect(ringUp_, &RingUpgrade::targetFinished, this, [this](int addr, bool ok, const QString&) {
            if (DevQProperty* dev = devTree_.value(ringLink_).value(uint8_t(addr), nullptr))
                dev->setRingUpgradeStatus(ok ? RingUpgrade::kStatusOk : RingUpgrade::kStatusFail, true);
        });
        connect(ringUp_, &RingUpgrade::finished, this, [this](bool, const QString&) { endRingUpgrade(); });
    }

    RingUpgrade::Plan plan;
    plan.boards = boards;
    plan.targets = everyBoard ? RingUpgrade::allTargets(boards) : QList<int>{ address };
    plan.image = firmware;
    plan.expect = expect;
    plan.lineRate = link->getBaudrate();

    ringLink_ = linkUuid;
    ringLinkPtr_ = link;
    QMetaObject::invokeMethod(link, [l = QPointer<Link>(link)] { if (l) l->setRatePinned(true); }, Qt::QueuedConnection);
    if (!ringUp_->start(plan)) {        // finished(false) has run endRingUpgrade() already
        if (requester)
            requester->setRingUpgradeStatus(RingUpgrade::kStatusFail, true);
        return;
    }
    ringTimer_->start();
}

// Headless test hook (docs/RING_UPDATE.md): KOGGER_AUTOUPGRADE=<file.ufw>|<all|address>|<ring boards>
// starts a ring update the way the device page does, once that many ring boards answer on a line.
// Off unless set; with KOGGER_AUTOTEST=<port> the whole run needs no click.
void DeviceManager::autoRingUpgrade(QUuid uuid)
{
    static const QStringList kAuto = qEnvironmentVariable("KOGGER_AUTOUPGRADE").split(QLatin1Char('|'));
    if (kAuto.size() != 3 || autoUpgradeStarted_)
        return;
    QList<DevQProperty*> ring;
    const auto devs = devTree_.value(uuid);
    for (auto it = devs.cbegin(); it != devs.cend(); ++it)
        if (it.value() && it.value()->isRingBoard())
            ring.append(it.value());
    if (ring.isEmpty() || ring.size() < kAuto[2].toInt())
        return;
    autoUpgradeStarted_ = true;
    QFile file(kAuto[0]);
    if (!file.open(QIODevice::ReadOnly)) {
        qInfo().noquote() << "ring-upgrade: KOGGER_AUTOUPGRADE cannot open" << kAuto[0];
        return;
    }
    DevQProperty* dev = ring.first();
    for (DevQProperty* d : ring)
        if (kAuto[1] != QLatin1String("all") && d->getBusAddress() == kAuto[1].toInt())
            dev = d;
    qInfo().noquote() << "ring-upgrade: KOGGER_AUTOUPGRADE" << kAuto[1] << "with" << kAuto[0];
    dev->setUpgradeExpect(RingUpgrade::versionFromFileName(kAuto[0]));
    if (kAuto[1] == QLatin1String("all"))
        dev->sendUpdateFWAll(file.readAll());
    else
        dev->sendUpdateFW(file.readAll());
}

void DeviceManager::endRingUpgrade()
{
    if (ringTimer_)
        ringTimer_->stop();
    if (QPointer<Link> l = ringLinkPtr_)
        QMetaObject::invokeMethod(l, [l] { if (l) l->setRatePinned(false); }, Qt::QueuedConnection);
    ringLinkPtr_ = nullptr;
    ringLink_ = QUuid();
}

void DeviceManager::onBusAddressRequested(QUuid linkUuid, int fromAddress, int toAddress)
{
    DevQProperty* dev = devTree_.value(linkUuid).value(fromAddress, nullptr);
    Link* link = linkOf_.value(linkUuid).data();
    auto refuse = [&](const QString& why) {
        qInfo().noquote() << "bus-address: refused:" << why;
        if (dev)
            dev->setBusAddressResult(false, fromAddress, why);
    };
    if ((busAddr_ && busAddr_->active()) || (ringUp_ && ringUp_->active()) || (rate_ && rate_->active()))
        return refuse(QStringLiteral("an address change, a ring update or a line rate change is running"));
    if (!link)
        return refuse(QStringLiteral("the line of the device is not known"));

    if (!busAddr_) {
        BusAddress::Io io;
        io.write = [this](const QByteArray& bytes) {
            if (QPointer<Link> l = busLinkPtr_)
                QMetaObject::invokeMethod(l, [l, bytes] { if (l) l->write(bytes); }, Qt::QueuedConnection);
        };
        io.nowMs = [this] { return busClock_.elapsed(); };
        busAddr_ = new BusAddress(io, this);
        busClock_.start();
        busTimer_ = new QTimer(this);
        busTimer_->setInterval(5);
        connect(busTimer_, &QTimer::timeout, busAddr_, &BusAddress::tick);
        connect(busAddr_, &BusAddress::logLine, this, [](const QString& line) { qInfo().noquote() << line; });
        connect(busAddr_, &BusAddress::finished, this, &DeviceManager::onBusAddressFinished);
    }
    busLink_ = linkUuid;
    busLinkPtr_ = link;
    busFrom_ = fromAddress;
    // The line is the procedure's for the run: no baud search or "ask everybody", no device frames.
    QMetaObject::invokeMethod(link, [l = QPointer<Link>(link)] {
        if (l) {
            l->setRatePinned(true);
            l->setDevicesMuted(true);
        }
    }, Qt::QueuedConnection);
    if (!busAddr_->start(toAddress))     // finished(false) has run onBusAddressFinished() already
        return;
    busTimer_->start();
}

void DeviceManager::onBusAddressFinished(bool ok, int address, const QString& detail)
{
    if (busTimer_)
        busTimer_->stop();
    const QUuid uuid = busLink_;
    if (QPointer<Link> l = busLinkPtr_)
        QMetaObject::invokeMethod(l, [l] {
            if (l) {
                l->setDevicesMuted(false);
                l->setRatePinned(false);
            }
        }, Qt::QueuedConnection);
    const int from = busFrom_;
    busLink_ = QUuid();
    busLinkPtr_ = nullptr;
    busFrom_ = -1;
    DevQProperty* dev = devTree_.value(uuid).value(from, nullptr);
    if (ok && dev && from != address)
        rekeyDevice(uuid, from, address);
    if (dev)
        dev->setBusAddressResult(ok, address, detail);
}

// The device keeps its object -- its page, settings and plots stay -- and is keyed by its new route.
void DeviceManager::rekeyDevice(QUuid uuid, int from, int to)
{
    auto& devs = devTree_[uuid];
    DevQProperty* dev = devs.value(from, nullptr);
    if (!dev || from == to)
        return;
    // A device already keyed by the new route -- the empty one a link's opening makes at 0, or one left
    // from an earlier address -- goes: that route is this board's now.
    if (DevQProperty* other = devs.value(to, nullptr); other && other != dev) {
        if (lastDevice_ == other)
            lastDevice_ = nullptr;
        disconnect(other);
        other->deleteLater();
    }
    devs.remove(from);
    devs[to] = dev;
    dev->setBusAddress(to);
    lastDevice_ = nullptr;             // getDevice() caches the last (link, route) pair
    qInfo().noquote() << QStringLiteral("bus-address: the device of route %1 answers on %2 now").arg(from).arg(to);
    emit devChanged();
}

void DeviceManager::onRingRateRequested(QUuid linkUuid, int fromAddress, int rate)
{
    DevQProperty* dev = devTree_.value(linkUuid).value(fromAddress, nullptr);
    Link* link = linkOf_.value(linkUuid).data();
    auto refuse = [&](const QString& why) {
        qInfo().noquote() << "ring-rate: refused:" << why;
        if (dev)
            dev->setRingRateResult(false, link ? link->getBaudrate() : 0, why);
    };
    if ((rate_ && rate_->active()) || (ringUp_ && ringUp_->active()) || (busAddr_ && busAddr_->active()))
        return refuse(QStringLiteral("a line rate change, a ring update or an address change is running"));
    if (!link)
        return refuse(QStringLiteral("the line of the device is not known"));

    // The ring boards of that line: Basic2D sonars answering on a bus address 1..15, the smaller one first.
    QList<int> boards;
    const auto devs = devTree_.value(linkUuid);
    for (auto it = devs.cbegin(); it != devs.cend(); ++it)
        if (it.value() && it.value()->isRingBoard())
            boards.append(int(it.key()));
    std::sort(boards.begin(), boards.end());

    if (!rate_) {
        RingRate::Io io;
        io.write = [this](const QByteArray& bytes) {
            if (QPointer<Link> l = rateLinkPtr_)
                QMetaObject::invokeMethod(l, [l, bytes] { if (l) l->write(bytes); }, Qt::QueuedConnection);
        };
        io.setRate = [this](int r) {
            if (QPointer<Link> l = rateLinkPtr_)
                QMetaObject::invokeMethod(l, [l, r] { if (l) l->setBaudrateAfterWrite(r); }, Qt::QueuedConnection);
        };
        io.nowMs = [this] { return rateClock_.elapsed(); };
        rate_ = new RingRate(io, this);
        rateClock_.start();
        rateTimer_ = new QTimer(this);
        rateTimer_->setInterval(5);
        connect(rateTimer_, &QTimer::timeout, rate_, &RingRate::tick);
        connect(rate_, &RingRate::logLine, this, [](const QString& line) { qInfo().noquote() << line; });
        connect(rate_, &RingRate::finished, this, &DeviceManager::onRingRateFinished);
    }
    rateLink_ = linkUuid;
    rateLinkPtr_ = link;
    rateFrom_ = fromAddress;
    // The line is the procedure's for the run: no baud search or "ask everybody", no device frames.
    QMetaObject::invokeMethod(link, [l = QPointer<Link>(link)] {
        if (l) {
            l->setRatePinned(true);
            l->setDevicesMuted(true);
        }
    }, Qt::QueuedConnection);
    RingRate::Plan plan;
    plan.boards = boards;
    plan.from = link->getBaudrate();
    plan.to = rate;
    if (!rate_->start(plan))            // finished(false) has run onRingRateFinished() already
        return;
    rateTimer_->start();
}

void DeviceManager::onRingRateFinished(bool ok, int rate, const QString& detail)
{
    if (rateTimer_)
        rateTimer_->stop();
    if (QPointer<Link> l = rateLinkPtr_)
        QMetaObject::invokeMethod(l, [l] {
            if (l) {
                l->setDevicesMuted(false);
                l->setRatePinned(false);
            }
        }, Qt::QueuedConnection);
    const QUuid uuid = rateLink_;
    const int from = rateFrom_;
    rateLink_ = QUuid();
    rateLinkPtr_ = nullptr;
    rateFrom_ = -1;
    if (DevQProperty* dev = devTree_.value(uuid).value(from, nullptr))
        dev->setRingRateResult(ok, rate, detail);
}

// Headless test hook (docs/RING_UPDATE.md): KOGGER_AUTORATE=<rate>|<ring boards> moves the line to that rate
// the way a ring sonar's page does, once that many ring boards answer on it. Off unless set.
void DeviceManager::autoRingRate(QUuid uuid)
{
    static const QStringList kAuto = qEnvironmentVariable("KOGGER_AUTORATE").split(QLatin1Char('|'));
    if (kAuto.size() != 2 || autoRateStarted_)
        return;
    QList<DevQProperty*> ring;
    const auto devs = devTree_.value(uuid);
    for (auto it = devs.cbegin(); it != devs.cend(); ++it)
        if (it.value() && it.value()->isRingBoard())
            ring.append(it.value());
    if (ring.isEmpty() || ring.size() < kAuto[1].toInt())
        return;
    autoRateStarted_ = true;
    qInfo().noquote() << "ring-rate: KOGGER_AUTORATE" << kAuto[0] << "with" << ring.size() << "ring boards";
    ring.first()->setRingLineRate(kAuto[0].toInt());
}

// Headless test hook (docs/RING_UPDATE.md): KOGGER_AUTOADDRESS=<address> sets the bus address of the
// first sonar on a line that can take one, the way its device page does. Off unless set.
void DeviceManager::autoBusAddress(QUuid uuid)
{
    static const QString kAuto = qEnvironmentVariable("KOGGER_AUTOADDRESS");
    if (kAuto.isEmpty() || autoAddressStarted_)
        return;
    const auto devs = devTree_.value(uuid);
    for (auto it = devs.cbegin(); it != devs.cend(); ++it)
        if (it.value() && it.value()->isBusAddressSettable()) {
            autoAddressStarted_ = true;
            qInfo().noquote() << "bus-address: KOGGER_AUTOADDRESS" << kAuto << "for the device of route" << it.key();
            it.value()->assignBusAddress(kAuto.toInt());
            return;
        }
}

void DeviceManager::createLocationReader()
{
    //qDebug() << "DeviceManager::createLocationReader";

    if (locReader_) {
        return;
    }

    locReader_ = new LocationReader(this);
    connect(locReader_, &LocationReader::positionUpdated, this, &DeviceManager::onPositionUpdated, Qt::QueuedConnection);
    connect(locReader_, &LocationReader::gpsAlive, &core, &Core::setIsGPSAlive, Qt::QueuedConnection);
}

void DeviceManager::destroyLocationReader()
{
    if (!locReader_) {
        return;
    }

    locReader_->deleteLater();
    locReader_ = nullptr;
}

void DeviceManager::shutdown()
{
    destroyLocationReader();
}

void DeviceManager::onPositionUpdated(const QGeoPositionInfo &info)
{
    if (!useGPS_) {
        return;
    }

    IDBinNav::SimpleNav smplNav;
    smplNav.latitude = info.coordinate().latitude();
    smplNav.longitude = info.coordinate().longitude();
    smplNav.depth = 0;
    smplNav.yaw = info.attribute(QGeoPositionInfo::Attribute::Direction) ;
    smplNav.pitch = 0;
    smplNav.roll = 0;

    emit positionComplete(smplNav.latitude, smplNav.longitude, info.timestamp().toSecsSinceEpoch(), info.timestamp().toMSecsSinceEpoch());
    if (info.timestamp().isValid()) {
        emit gnssUtcComplete(info.timestamp().toMSecsSinceEpoch() * 1000);
    }
    // The course of this computer's own location: the least kind, like a device's IMU.
    passAttitude(AttitudeArbiter::KindDeviceImu, 0, smplNav.yaw, 0.0f, 0.0f);

    // LOGGING
    if (loggingStarted_) {
        ProtoBinOut req_out;
        req_out.create(Parsers::CONTENT, IDBinNav::SimpleNav::getVer(), IDBinNav::SimpleNav::getId(), 0/*m_address*/);
        req_out.write<IDBinNav::SimpleNav>(smplNav);
        req_out.end();

        //QString str1 = "emit coords 1 " + QString::number(smplNav.latitude, 'f', 4) + " " + QString::number(smplNav.longitude, 'f', 4) + " " + QString::number(smplNav.depth, 'f', 4) + " "
        //+ QString::number(smplNav.yaw, 'f', 4) + " " + QString::number(smplNav.pitch, 'f', 4) + " " + QString::number(smplNav.roll, 'f', 4);
        //core.consoleInfo(str1);
        //QString str2 = "emit coords 2 " + QString::number(req_out.binError()) + " " + QString::number(req_out.isComplete()) + " " + QString::number(req_out.payloadLen()) + " " + QString::number(req_out.frameLen()) + " "
        //               + QString::number(req_out.readAvailable()) + " " + QString::number(req_out.availContext());
        //core.consoleInfo(str2);

        emit sendFrameInputToLogger(QUuid(), nullptr, req_out);
    }
}

void DeviceManager::setUseGPS(bool state)
{
    //qDebug() << "DeviceManager::setUseGPS" << state;
    useGPS_ = state;
}

DevQProperty* DeviceManager::getDevice(QUuid uuid, Link *link, uint8_t addr)
{
    if ((link == nullptr || lastUuid_ == uuid) && lastAddress_ == addr && lastDevice_ != nullptr) {
        return lastDevice_;
    }
    else {
        lastDevice_ = devTree_[uuid][addr];
        if (lastDevice_ == nullptr) {
            lastDevice_ = createDev(uuid, link, addr);
        }
        lastUuid_ = uuid;
        lastAddress_ = addr;
    }

    return lastDevice_;
}

void DeviceManager::delAllDev()
{
    mavlinkUnixOffsetValid_ = false;
    mavlinkLastPositionBootMs_ = 0;

    QList<QUuid> keysToDelete;
    for (auto i = devTree_.cbegin(), end = devTree_.cend(); i != end; ++i) {
        keysToDelete.append(i.key());
    }

    for (const auto& key : keysToDelete) {
        deleteDevicesByLink(key);
    }
    attitudeArb_.reset();   // a new recording or a new set of devices chooses its attitude source again
}

void DeviceManager::passAttitude(int kind, quint64 source, float yaw, float pitch, float roll)
{
    if (!attitudeArb_.accept(AttitudeArbiter::Kind(kind), source)) {
        return;
    }
    if (attitudeArb_.changed()) {
        QString what;
        switch (kind) {
        case AttitudeArbiter::KindAutopilot:
            what = QStringLiteral("the autopilot (MAVLink ATTITUDE, system %1 component %2)").arg(source >> 8).arg(source & 0xFF);
            break;
        case AttitudeArbiter::KindGnssHeading:
            what = QStringLiteral("a GNSS compass (NMEA HDT)");
            break;
        case AttitudeArbiter::KindNav:
            what = QStringLiteral("a navigation board (ID_NAV, address %1)").arg(source & 0xFF);
            break;
        default:
            what = source ? QStringLiteral("a device's own IMU (ID_ATTITUDE, address %1)").arg(source & 0xFF)
                          : QStringLiteral("the course of this computer's location");
            break;
        }
        qInfo().noquote() << QStringLiteral("attitude source: %1").arg(what);
    }
    emit attitudeComplete(yaw, pitch, roll);
}

void DeviceManager::onDevAttitude(float yaw, float pitch, float roll, int kind, quint64 source)
{
    passAttitude(kind, source, yaw, pitch, roll);
}

void DeviceManager::deleteDevicesByLink(QUuid uuid)
{
    if (devTree_.contains(uuid)) {
        const auto& devs = devTree_[uuid];
        bool hadRecorder = false;
        for (auto i = devs.cbegin(), end = devs.cend(); i != end; ++i) {
            if (i.value() && i.value()->isRecorder()) {
                hadRecorder = true;
            }
            if (lastDevice_ == i.value()) {
                lastDevice_ = nullptr;
            }
            disconnect(i.value());

            i.value()->deleteLater();
        }
        devTree_[uuid].clear();
        devTree_.remove(uuid);
        if (hadRecorder) {
            streamList_.reset();
            emit streamChanged();
        }
        emit devChanged();
        emit standAvailableChanged();
    }
}

bool DeviceManager::standAvailable()
{
    const QList<DevQProperty*> devs = getDevList();
    for (DevQProperty* dev : devs) {
        if (dev && dev->getStandState())
            return true;
    }
    return false;
}

DevQProperty* DeviceManager::createDev(QUuid uuid, Link* link, uint8_t addr)
{
    DevQProperty* dev = new DevQProperty();
    devTree_[uuid][addr] = dev;
    dev->setBusAddress(addr);
    dev->setLinkUuid(uuid);
    dev->setLinkSerial(link != nullptr && link->getLinkType() == LinkType::kLinkSerial);

    if (link != nullptr) {
        connect(dev, &DevQProperty::binFrameOut, this, &DeviceManager::binFrameOut);
        connect(dev, &DevQProperty::binFrameOut, link, &Link::writeFrame);
        connect(dev, &DevQProperty::startUpgradingFirmware, link, &Link::onStartUpgradingFirmware);
        connect(dev, &DevQProperty::upgradingFirmwareDone, link, &Link::onUpgradingFirmwareDone);
        connect(dev, &DevQProperty::linkRatePinRequested, link, &Link::setRatePinned);
    }

    connect(dev, &DevQProperty::startUpgradingFirmwareDM, this, &DeviceManager::onStartUpgradingFirmware);
    connect(dev, &DevQProperty::upgradingFirmwareDoneDM, this, &DeviceManager::onUpgradingFirmwareDone);
    connect(dev, &DevQProperty::upgradeAwaitingStart, this, &DeviceManager::onUpgradeAwaitingStart);
    connect(dev, &DevQProperty::upgradeStartSettled, this, &DeviceManager::onUpgradeStartSettled);
    connect(dev, &DevQProperty::ringUpgradeRequested, this, &DeviceManager::onRingUpgradeRequested);
    connect(dev, &DevQProperty::busAddressRequested, this, &DeviceManager::onBusAddressRequested);
    connect(dev, &DevQProperty::ringRateRequested, this, &DeviceManager::onRingRateRequested);

    //
    connect(dev, &DevQProperty::sendChartSetup,  this, &DeviceManager::sendChartSetup);
    connect(dev, &DevQProperty::sendTranscSetup, this, &DeviceManager::sendTranscSetup);
    connect(dev, &DevQProperty::sendSoundSpeed, this, &DeviceManager::sendSoundSpeeed);
    connect(dev, &DevQProperty::averageChartLossesChanged, this, &DeviceManager::chartLossesChanged);
    connect(dev, &DevQProperty::standChanged, this, &DeviceManager::standAvailableChanged);

    connect(dev, &DevQProperty::chartComplete, this, &DeviceManager::chartComplete);
    connect(dev, &DevQProperty::rawDataRecieved, this, &DeviceManager::rawDataRecieved);
    connect(dev, &DevQProperty::attitudeComplete, this, &DeviceManager::onDevAttitude);
    connect(dev, &DevQProperty::tempComplete, this, &DeviceManager::tempComplete);
    connect(dev, &DevQProperty::supplyVoltageComplete, this, &DeviceManager::supplyVoltageComplete);
    connect(dev, &DevQProperty::distComplete, this, &DeviceManager::distComplete);
    connect(dev, &DevQProperty::encoderComplete, this, &DeviceManager::encoderComplete);
    connect(dev, &DevQProperty::usblSolutionComplete, this, &DeviceManager::usblSolutionComplete);
    connect(dev, &DevQProperty::usblDiagComplete, this, &DeviceManager::usblDiagComplete);
    connect(dev, &DevQProperty::beaconActivationComplete, this, &DeviceManager::beaconActivationReceive);
    connect(dev, &DevQProperty::dopplerBeamComplete, this, &DeviceManager::dopplerBeamComlete);
    connect(dev, &DevQProperty::dvlSolutionComplete, this, &DeviceManager::dvlSolutionComplete);
    connect(dev, &DevQProperty::upgradeProgressChanged, this, &DeviceManager::upgradeProgressChanged);

    connect(dev, &DevQProperty::positionComplete, this, &DeviceManager::positionComplete);
    connect(dev, &DevQProperty::gnssVelocityComplete, this, &DeviceManager::gnssVelocityComplete);
    connect(dev, &DevQProperty::simpleNavV2Complete, this, &DeviceManager::simpleNavV2Complete);
    connect(dev, &DevQProperty::boatStatusComplete, this, &DeviceManager::boatStatusComplete);
    connect(dev, &DevQProperty::depthComplete, this, &DeviceManager::depthComplete);

    dev->startConnection(link != nullptr);

    if (link != nullptr) {
        auto syncStatus = [dev, link](QUuid = {}) {
            dev->setLinkStatus(link->getConnectionStatus(),
                               link->getIsRecievesData(),
                               link->getIsNotAvailable());
        };
        connect(link, &Link::connectionStatusChanged, dev, syncStatus);
        connect(link, &Link::isReceivesDataChanged,   dev, syncStatus);
        connect(link, &Link::isNotAvailableChanged,   dev, syncStatus);
        syncStatus();
    }

    if (upgradeUuid_ == uuid && upgradeAddr_ == addr) {
        upgradeUuid_ = QUuid();
        QMetaObject::invokeMethod(dev, [dev, firmware = upgradeData_]() {
            dev->setFirmware(firmware);
        }, Qt::QueuedConnection);
    }
    if (startWaitAt_ != 0 && startWaitUuid_ == uuid && startWaitAddr_ == addr) {
        QMetaObject::invokeMethod(dev, [dev, toldAt = startWaitAt_]() {
            dev->continueUpgradeStart(toldAt);
        }, Qt::QueuedConnection);
    }

    emit devChanged();

    return dev;
}
