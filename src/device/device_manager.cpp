#include <QFile>
#include <algorithm>
#include "device_manager.h"
#include "device_defs.h"
#include <QDateTime>
#include "location_reader.h"
#include "core.h"
extern Core core;


DeviceManager::DeviceManager()
    : lastDevs_(nullptr),
      lastDevice_(nullptr),
      mavlinkLink_(nullptr),
      streamList_(this),
      lastAddress_(-1),
      progress_(0),
      isConsoled_(false),
      nmeaConsoled_(true),
      break_(false),
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
    qRegisterMetaType<IDBinUsblSolution::AcousticNavSolution>("IDBinUsblSolution::AcousticNavSolution");
    qRegisterMetaType<IDBinUsblSolution::BaseToBeacon>("IDBinUsblSolution::BaseToBeacon");
    qRegisterMetaType<IDBinModemSolution::ModemSolutionHeader>("IDBinModemSolution::ModemSolutionHeader");
    qRegisterMetaType<IDBinDVL::BeamSolution>("IDBinDVL::BeamSolution");
    qRegisterMetaType<uint16_t>("uint16_t");
    qRegisterMetaType<IDBinDVL::DVLSolution>("IDBinDVL::DVLSolution");
    qRegisterMetaType<uint32_t>("uint32_t");
    qRegisterMetaType<FrameParser>("FrameParser");
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

int DeviceManager::calcAverageChartLosses()
{
    int retVal = 0;
    int averageChartLosses = 0;
    int numOfDevices = 0;

    for (auto i = devTree_.cbegin(), end = devTree_.cend(); i != end; ++i) {
        const auto& devs = i.value();

        for (auto k = devs.cbegin(), end = devs.cend(); k != end; ++k) {
            ++numOfDevices;
            averageChartLosses += k.value()->getAverageChartLosses();
        }
    }

    if (numOfDevices != 0) {
        retVal = averageChartLosses / numOfDevices;
    }

    return retVal;
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
#ifndef SEPARATE_READING
                core.consoleProto(frame);
#endif
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

            if (frame.id() == ID_VOLTAGE) {
                int v_id = frame.read<U1>();
                int32_t v_uv = frame.read<S4>();
                Q_UNUSED(v_uv);
                if (v_id == 1) {
                    // core.dataset()->addEncoder(float(v_uv));
                    // qInfo("Voltage %f", float(v_uv));
                }
            }
#endif
            dev->protoComplete(frame);
            autoRingUpgrade(uuid);
            autoBusAddress(uuid);
        }

        if (frame.isCompleteAsNMEA()) {
            ProtoNMEA& prot_nmea = (ProtoNMEA&)frame;
            QString str_data = QByteArray((char*)prot_nmea.frame(), prot_nmea.frameLen() - 2);
#ifndef SEPARATE_READING
            if (nmeaConsoled_) {
                core.consoleProtoText(QString(">> NMEA: %5").arg(str_data));
            }
#endif
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
                Q_UNUSED(isCorrect);

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
                    }
                }
            }

            if (prot_nmea.isEqualId("HDT")) {
                const double headingDeg = prot_nmea.readDouble();
                //const char reference = prot_nmea.readChar();
                const bool headingValid = isfinite(headingDeg);

                if (/*reference == 'T' && TODO: check this*/ headingValid) {
                    //qDebug().noquote() << QString("NMEA HDT parsed: heading=%1 deg true").arg(heading_deg, 0, 'f', 3);
                    emit attitudeComplete(static_cast<float>(headingDeg), 0.0f, 0.0f);
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
                ubx_frame.read<U1>(); // Validity flags
                ubx_frame.readSkip(4); // Time accuracy estimate (UTC)
                nanosec = ubx_frame.read<S4>();

                uint8_t fix_type = ubx_frame.read<U1>();
                uint8_t fix_flags = ubx_frame.read<U1>();
                Q_UNUSED(fix_flags);

                ubx_frame.read<U1>();
                uint8_t satellites_in_used = ubx_frame.read<U1>();
                Q_UNUSED(satellites_in_used)

                int32_t lon_int = ubx_frame.read<S4>();
                int32_t lat_int = ubx_frame.read<S4>();

                QDate date(year, month, day);
                QTime time(h, m, s);

                QDateTime dt(date, time, QTimeZone::utc());
                uint32_t unix_time = static_cast<uint32_t>(dt.toSecsSinceEpoch());

                if (fix_type > 1 && fix_type < 5) {
                    emit positionComplete(double(lat_int)*0.0000001, double(lon_int)*0.0000001, unix_time, nanosec);
                }

                // if (isConsoled_) {
#ifndef SEPARATE_READING
                    core.consoleStreamInfo(QString(">> UBX: NAV_PVT, fix %1, sats %2, lat %3, lon %4, time %5:%6:%7.%8")
                                         .arg(fix_type).arg(satellites_in_used).arg(double(lat_int)*0.0000001).arg(double(lon_int)*0.0000001).arg(h).arg(m).arg(s).arg(nanosec/1000));
#endif
                    // }
            }
            else {
                // if (isConsoled_)
#ifndef SEPARATE_READING
                    core.consoleStreamInfo(QString(">> UBX: class/id 0x%1 0x%2, len %3").arg(ubx_frame.msgClass(), 2, 16, QLatin1Char('0')).arg(ubx_frame.msgId(), 2, 16, QLatin1Char('0')).arg(ubx_frame.frameLen()));
#endif
            }
        }

        if (frame.isCompleteAsMAVLink()) {
            if (link == nullptr || proxyLinkUuid_ != uuid) {
                emit writeProxyFrame(frame);

                if (link != nullptr && mavlinUuid_ != uuid) {
                    mavlinUuid_ = uuid;
                    if(mavlinkLink_ != nullptr) {
                        disconnect(this, &DeviceManager::writeMavlinkFrame,  mavlinkLink_, &Link::writeFrame);
                        disconnect(this, &DeviceManager::writeMavlinkBytes,  mavlinkLink_, &Link::write);
                    }
                    mavlinkLink_ = link;
                    connect(this, &DeviceManager::writeMavlinkFrame, mavlinkLink_, &Link::writeFrame, Qt::UniqueConnection);
                    connect(this, &DeviceManager::writeMavlinkBytes, mavlinkLink_, &Link::write, Qt::UniqueConnection);
                }

                if (link != nullptr) {   // live links only: a replayed log must not look like a boat
                    emit mavlinkFrameReceived(QByteArray(reinterpret_cast<const char*>(frame.frame()), frame.frameLen()));
                }

                ProtoMAVLink& mavlink_frame = (ProtoMAVLink&)frame;

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
                        emit positionComplete(pos.latitude(), pos.longitude(), pos.time_boot_msec()/1000, (pos.time_boot_msec()%1000)*1e6);
                        emit gnssVelocityComplete(pos.velocityH(), 0);
                        vru_.velocityH = pos.velocityH();
                        emit vruChanged();
                    }
                }

                if (mavlink_frame.msgId() == 0) { // SYS_STATUS
                    MAVLink_MSG_HEARTBEAT heartbeat = mavlink_frame.read<MAVLink_MSG_HEARTBEAT>();
                    vru_.armState = (int)heartbeat.isArmed();
                    int flight_mode = (int)heartbeat.customMode();
                    if (flight_mode != vru_.flightMode) {
#ifndef SEPARATE_READING
                        core.consoleStreamInfo(QString(">> FC: Flight mode %1").arg(flight_mode));
#endif
                    }
                    vru_.flightMode = flight_mode;
                    emit vruChanged();
                }

                if (mavlink_frame.msgId() == 147) { // BATTERY_STATUS
                    MAVLink_MSG_BATTERY_STATUS battery_status = mavlink_frame.read<MAVLink_MSG_BATTERY_STATUS>();
                    vru_.voltage = battery_status.voltage();
                    vru_.current = battery_status.current();
                    emit vruChanged();
                }

                if (mavlink_frame.msgId() == 30) {
                    MAVLink_MSG_ATTITUDE attitude = mavlink_frame.read<MAVLink_MSG_ATTITUDE>();

                    const float yaw = attitude.yawDeg();
                    const float pitch = attitude.pitchDeg();
                    const float roll = attitude.rollDeg();

                    if (!qFuzzyIsNull(yaw) || !qFuzzyIsNull(pitch) || !qFuzzyIsNull(roll)) {
                        emit attitudeComplete(yaw, attitude.pitchDeg(), attitude.rollDeg());
                    }
                }
#ifndef SEPARATE_READING
                core.consoleProtoText(QString(">> MAVLink v%1: ID %2, comp. id %3, seq numb %4, len %5").arg(mavlink_frame.MAVLinkVersion()).arg(mavlink_frame.msgId()).arg(mavlink_frame.componentID()).arg(mavlink_frame.sequenceNumber()).arg(mavlink_frame.frameLen()));
#endif
            }
            else {
                if (link != nullptr) {
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
#ifdef SEPARATE_READING
    break_ = false;
#endif

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

#ifdef SEPARATE_READING
    emit fileStartOpening();
    bool fileReadEnough{false};
#endif

    while (true) {

#ifdef SEPARATE_READING
        QCoreApplication::processEvents();
        if (break_) {
            emit fileBreaked(onOpen_);
            onOpen_ = false;
            file.close();
            emit fileStopsOpening();
            return;
        }
#else
        if (break_) {
            file.close();
            return;
        }
#endif

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

#ifdef SEPARATE_READING
        int sleepCnt = 0;
#endif

        while (frameParser.availContext() > 0) {

#ifdef SEPARATE_READING
            QCoreApplication::processEvents();
            if (break_) {
                emit fileBreaked(onOpen_);
                onOpen_ = false;
                file.close();
                return;
            }
            if (sleepCnt > 10) {
                QThread::msleep(1);
                sleepCnt = 0;
            }
            ++sleepCnt;
#endif
            frameParser.process();
            if (frameParser.isComplete()) {
                frameInput(someUuid, nullptr, frameParser);
#ifdef SEPARATE_READING
                if (!fileReadEnough) { // TODO: check this
                    emit onFileReadEnough();
                    fileReadEnough = true;
                }
#endif
            }
        }

        chunk.clear();
    }
    file.close();

    vru_.cleanVru();
    delAllDev();
    emit vruChanged();

    emit fileOpened();
    emit fileStopsOpening();
}

#ifdef SEPARATE_READING
void DeviceManager::closeFile(bool onOpen)
{
    onOpen_ = onOpen;
    break_ = true;

    vru_.cleanVru();
    delAllDev();
    emit vruChanged();
}
#else
void DeviceManager::closeFile()
{
    delAllDev();
    vru_.cleanVru();
    emit vruChanged();
}
#endif

void DeviceManager::onLinkOpened(QUuid uuid, Link *link)
{
    if (link) {
        if (link->getIsProxy()) {
            proxyLinkUuid_ = uuid;
            connect(this, &DeviceManager::writeProxyFrame, link, &Link::writeFrame);
        } else if(link->attribute() == LinkAttribute::kLinkAttributeBoot) {
#ifndef SEPARATE_READING
            core.consoleInfo("Device: Boot opened");
#endif
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
#ifndef SEPARATE_READING
        core.consoleProto(protoOut, false);
#endif
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

void DeviceManager::onRingUpgradeRequested(QUuid linkUuid, int address, QByteArray firmware, QString expect, bool everyBoard)
{
    DevQProperty* requester = devTree_.value(linkUuid).value(uint8_t(address), nullptr);
    Link* link = linkOf_.value(linkUuid).data();
    auto refuse = [&](const QString& why) {
        qInfo().noquote() << "ring-upgrade: refused:" << why;
        if (requester)
            requester->setRingUpgradeStatus(RingUpgrade::kStatusFail, true);
    };
    if (ringUp_ && ringUp_->active()) {
        refuse(QStringLiteral("a ring update is already running"));
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
    if ((busAddr_ && busAddr_->active()) || (ringUp_ && ringUp_->active()))
        return refuse(QStringLiteral("an address change or a ring update is running"));
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
    if (!busAddr_->start(toAddress))     // finished(false) has run onBusAddressFinished() already
        return;
    busTimer_->start();
}

void DeviceManager::onBusAddressFinished(bool ok, int address, const QString& detail)
{
    if (busTimer_)
        busTimer_->stop();
    const QUuid uuid = busLink_;
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
#ifdef SEPARATE_READING
        QMetaObject::invokeMethod(other, "deleteLater", Qt::QueuedConnection);
#else
        other->deleteLater();
#endif
    }
    devs.remove(from);
    devs[to] = dev;
    dev->setBusAddress(to);
    lastDevice_ = nullptr;             // getDevice() caches the last (link, route) pair
    qInfo().noquote() << QStringLiteral("bus-address: the device of route %1 answers on %2 now").arg(from).arg(to);
    emit devChanged();
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
    emit attitudeComplete(smplNav.yaw, 0.0, 0.0);

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
    QList<QUuid> keysToDelete;
    for (auto i = devTree_.cbegin(), end = devTree_.cend(); i != end; ++i) {
        keysToDelete.append(i.key());
    }

    for (const auto& key : keysToDelete) {
        deleteDevicesByLink(key);
    }
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

#ifdef SEPARATE_READING
            QMetaObject::invokeMethod(i.value(), "deleteLater", Qt::QueuedConnection);
#else
            i.value()->deleteLater();
#endif
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

#ifdef SEPARATE_READING
    auto connType = Qt::AutoConnection;

    if (link != nullptr) {
        connect(dev, &DevQProperty::binFrameOut, this, &DeviceManager::binFrameOut, connType);
        connect(dev, &DevQProperty::binFrameOut, link, &Link::writeFrame, connType);
        connect(dev, &DevQProperty::startUpgradingFirmware, link, &Link::onStartUpgradingFirmware, connType);
        connect(dev, &DevQProperty::upgradingFirmwareDone, link, &Link::onUpgradingFirmwareDone, connType);
    }

    connect(dev, &DevQProperty::startUpgradingFirmwareDM, this, &DeviceManager::onStartUpgradingFirmware, connType);
    connect(dev, &DevQProperty::upgradingFirmwareDoneDM, this, &DeviceManager::onUpgradingFirmwareDone, connType);
    connect(dev, &DevQProperty::ringUpgradeRequested, this, &DeviceManager::onRingUpgradeRequested, connType);
    connect(dev, &DevQProperty::busAddressRequested, this, &DeviceManager::onBusAddressRequested, connType);

    //
    connect(dev, &DevQProperty::sendChartSetup, this, &DeviceManager::sendChartSetup, connType);
    connect(dev, &DevQProperty::sendTranscSetup, this, &DeviceManager::sendTranscSetup, connType);
    connect(dev, &DevQProperty::sendSoundSpeed, this, &DeviceManager::sendSoundSpeeed, connType);
    connect(dev, &DevQProperty::averageChartLossesChanged, this, &DeviceManager::chartLossesChanged, connType);
    connect(dev, &DevQProperty::standChanged, this, &DeviceManager::standAvailableChanged, connType);

    connect(dev, &DevQProperty::chartComplete, this, &DeviceManager::chartComplete, connType);
    connect(dev, &DevQProperty::rawDataRecieved, this, &DeviceManager::rawDataRecieved, connType);
    connect(dev, &DevQProperty::attitudeComplete, this, &DeviceManager::attitudeComplete, connType);
    connect(dev, &DevQProperty::tempComplete, this, &DeviceManager::tempComplete, connType);
    connect(dev, &DevQProperty::distComplete, this, &DeviceManager::distComplete, connType);
    connect(dev, &DevQProperty::usblSolutionComplete, this, &DeviceManager::usblSolutionComplete, connType);
    connect(dev, &DevQProperty::dopplerBeamComplete, this, &DeviceManager::dopplerBeamComlete, connType);
    connect(dev, &DevQProperty::dvlSolutionComplete, this, &DeviceManager::dvlSolutionComplete, connType);
    connect(dev, &DevQProperty::upgradeProgressChanged, this, &DeviceManager::upgradeProgressChanged, connType);

    connect(dev, &DevQProperty::positionComplete, this, &DeviceManager::positionComplete, connType);
    connect(dev, &DevQProperty::gnssVelocityComplete, this, &DeviceManager::gnssVelocityComplete, connType);
    connect(dev, &DevQProperty::simpleNavV2Complete, this, &DeviceManager::simpleNavV2Complete, connType);
    connect(dev, &DevQProperty::boatStatusComplete, this, &DeviceManager::boatStatusComplete, connType);
    connect(dev, &DevQProperty::depthComplete, this, &DeviceManager::depthComplete, connType);

    dev->moveToThread(qApp->thread());
    dev->getProcessTimer()->moveToThread(qApp->thread());
    QList<QTimer*> timers = dev->getChildTimers();
    foreach (QTimer* timer, timers) {
        timer->moveToThread(qApp->thread());
    }

    QMetaObject::invokeMethod(dev, "initProcessTimerConnects", Qt::QueuedConnection);
    QMetaObject::invokeMethod(dev, "initChildsTimersConnects", Qt::QueuedConnection);
    QMetaObject::invokeMethod(dev, "startConnection", Qt::QueuedConnection, Q_ARG(bool, link != nullptr));
#else
    if (link != nullptr) {
        connect(dev, &DevQProperty::binFrameOut, this, &DeviceManager::binFrameOut);
        connect(dev, &DevQProperty::binFrameOut, link, &Link::writeFrame);
        connect(dev, &DevQProperty::startUpgradingFirmware, link, &Link::onStartUpgradingFirmware);
        connect(dev, &DevQProperty::upgradingFirmwareDone, link, &Link::onUpgradingFirmwareDone);
    }

    connect(dev, &DevQProperty::startUpgradingFirmwareDM, this, &DeviceManager::onStartUpgradingFirmware);
    connect(dev, &DevQProperty::upgradingFirmwareDoneDM, this, &DeviceManager::onUpgradingFirmwareDone);
    connect(dev, &DevQProperty::ringUpgradeRequested, this, &DeviceManager::onRingUpgradeRequested);
    connect(dev, &DevQProperty::busAddressRequested, this, &DeviceManager::onBusAddressRequested);

    //
    connect(dev, &DevQProperty::sendChartSetup,  this, &DeviceManager::sendChartSetup);
    connect(dev, &DevQProperty::sendTranscSetup, this, &DeviceManager::sendTranscSetup);
    connect(dev, &DevQProperty::sendSoundSpeed, this, &DeviceManager::sendSoundSpeeed);
    connect(dev, &DevQProperty::averageChartLossesChanged, this, &DeviceManager::chartLossesChanged);

    connect(dev, &DevQProperty::chartComplete, this, &DeviceManager::chartComplete);
    connect(dev, &DevQProperty::rawDataRecieved, this, &DeviceManager::rawDataRecieved);
    connect(dev, &DevQProperty::attitudeComplete, this, &DeviceManager::attitudeComplete);
    connect(dev, &DevQProperty::tempComplete, this, &DeviceManager::tempComplete);
    connect(dev, &DevQProperty::distComplete, this, &DeviceManager::distComplete);
    connect(dev, &DevQProperty::encoderComplete, this, &DeviceManager::encoderComplete);
    connect(dev, &DevQProperty::usblSolutionComplete, this, &DeviceManager::usblSolutionComplete);
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
#endif

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

    emit devChanged();

    return dev;
}
