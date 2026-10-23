#include "mav_vehicle.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QTime>
#include <QtMath>
#include <cmath>
#include <cstring>

// The official generated MAVLink C library (MIT, third_party/mavlink), ArduPilot's dialect (it includes
// common): EKF_STATUS_REPORT is an ArduPilot message.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "mavlink/ardupilotmega/mavlink.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

constexpr uint8_t kRxChan = MAVLINK_COMM_0;
constexpr uint8_t kTxChan = MAVLINK_COMM_1;
constexpr int kConnectedMs = 3000;
constexpr int kHeartbeatKeepMs = 10000;   // stop our heartbeat when the boat has been silent this long
constexpr int kTransferTimeoutMs = 1500;
constexpr int kTransferRetries = 6;
constexpr int kMaxMessages = 40;
constexpr int kEkfStaleMs = 5000;     // an EKF report older than this says nothing
constexpr int kEkfPollMs = 2500;      // ask for one when none came for this long
constexpr int kNavRelogMs = 60000;    // a new reason within the same state is logged at most this often
constexpr int kEkfPollsMax = 10;      // unanswered polls before the app stops asking (re-armed on reconnect)
constexpr int kSettleLogMs = 3000;    // a health or GPS-heading change must hold this long to be logged

// Read once and logged one line per group: what decides whether the autopilot's EKF navigates (and whether
// it would say why: ARMING_CHECK 0 silences its pre-arm reports), and whether it gets a GPS heading from two
// antennas (moving baseline: receiver types, which one is used, antenna offsets; AP_GPS of Rover 4.4). The
// GPS_MBn_OFS_* only exist while GPS_MBn_TYPE is 1, so "no answer" for them is normal otherwise.
struct Setting { const char* name; int group; };
const Setting kSettingsToRead[] = {
    { "AHRS_EKF_TYPE", 0 }, { "EK3_ENABLE", 0 }, { "EK3_SRC1_POSXY", 0 }, { "EK3_SRC1_VELXY", 0 }, { "EK3_SRC1_YAW", 0 },
    { "EK3_GPS_CHECK", 0 }, { "COMPASS_ENABLE", 0 }, { "COMPASS_USE", 0 }, { "ARMING_CHECK", 0 }, { "FS_EKF_ACTION", 0 },
    { "FS_EKF_THRESH", 0 },
    { "GPS_TYPE", 1 }, { "GPS_TYPE2", 1 }, { "GPS_AUTO_SWITCH", 1 }, { "GPS_PRIMARY", 1 }, { "GPS_AUTO_CONFIG", 1 },
    { "GPS_DRV_OPTIONS", 1 }, { "GPS_POS1_X", 1 }, { "GPS_POS1_Y", 1 }, { "GPS_POS1_Z", 1 }, { "GPS_POS2_X", 1 },
    { "GPS_POS2_Y", 1 }, { "GPS_POS2_Z", 1 }, { "GPS_MB1_TYPE", 1 }, { "GPS_MB1_OFS_X", 1 }, { "GPS_MB1_OFS_Y", 1 },
    { "GPS_MB1_OFS_Z", 1 }, { "GPS_MB2_TYPE", 1 }, { "GPS_MB2_OFS_X", 1 }, { "GPS_MB2_OFS_Y", 1 }, { "GPS_MB2_OFS_Z", 1 },
};
constexpr int kSettingsCount = int(sizeof(kSettingsToRead) / sizeof(kSettingsToRead[0]));
constexpr int kSettingsPerTick = 6;   // the autopilot queues parameter requests; a burst of 30 would overflow it

QString gpsFixText(int fix)
{
    switch (fix) {
    case 0: case 1: return QObject::tr("no fix");
    case 2: return QStringLiteral("2D");
    case 3: return QStringLiteral("3D");
    case 4: return QStringLiteral("DGPS");
    case 5: return QStringLiteral("RTK float");
    case 6: return QStringLiteral("RTK fixed");
    }
    return QString::number(fix);
}
const char kSettingsGroup[] = "boatMission";

QString commandResultText(int r)
{
    switch (r) {
    case MAV_RESULT_ACCEPTED: return QObject::tr("accepted");
    case MAV_RESULT_TEMPORARILY_REJECTED: return QObject::tr("temporarily rejected");
    case MAV_RESULT_DENIED: return QObject::tr("denied");
    case MAV_RESULT_UNSUPPORTED: return QObject::tr("not supported");
    case MAV_RESULT_FAILED: return QObject::tr("failed");
    case MAV_RESULT_IN_PROGRESS: return QObject::tr("in progress");
    case MAV_RESULT_CANCELLED: return QObject::tr("cancelled");
    }
    return QObject::tr("result %1").arg(r);
}

QString missionResultText(int r)
{
    switch (r) {
    case MAV_MISSION_ACCEPTED: return QObject::tr("accepted");
    case MAV_MISSION_ERROR: return QObject::tr("error");
    case MAV_MISSION_UNSUPPORTED_FRAME: return QObject::tr("unsupported frame");
    case MAV_MISSION_UNSUPPORTED: return QObject::tr("unsupported command");
    case MAV_MISSION_NO_SPACE: return QObject::tr("no space (the autopilot may still be starting)");
    case MAV_MISSION_INVALID: return QObject::tr("invalid");
    case MAV_MISSION_INVALID_SEQUENCE: return QObject::tr("items out of sequence");
    case MAV_MISSION_DENIED: return QObject::tr("denied");
    case MAV_MISSION_OPERATION_CANCELLED: return QObject::tr("cancelled");
    }
    return QObject::tr("code %1").arg(r);
}

} // namespace

MavVehicle::MavVehicle(QObject* parent, bool persist)
    : QObject(parent),
      persist_(persist)
{
    clock_.start();
    secondTimer_.setInterval(1000);
    connect(&secondTimer_, &QTimer::timeout, this, &MavVehicle::onSecondTick);
    secondTimer_.start();

    transferTimer_.setSingleShot(true);
    transferTimer_.setInterval(kTransferTimeoutMs);
    connect(&transferTimer_, &QTimer::timeout, this, &MavVehicle::onTransferTimeout);

    stepTimer_.setSingleShot(true);
    connect(&stepTimer_, &QTimer::timeout, this, &MavVehicle::onStepTimeout);

    healthLogTimer_.setSingleShot(true);
    healthLogTimer_.setInterval(kSettleLogMs);
    connect(&healthLogTimer_, &QTimer::timeout, this, &MavVehicle::logHealth);
    gpsHeadingLogTimer_.setSingleShot(true);
    gpsHeadingLogTimer_.setInterval(kSettleLogMs);
    connect(&gpsHeadingLogTimer_, &QTimer::timeout, this, &MavVehicle::logGpsHeading);

    if (persist_)
        loadSettings();
    updateNav();
}

MavVehicle::~MavVehicle() = default;

// ---- link ------------------------------------------------------------------------------------------

bool MavVehicle::connected() const
{
    return detected() && lastHeartbeat_.isValid() && lastHeartbeat_.elapsed() < kConnectedMs;
}

int MavVehicle::heartbeatAgeS() const
{
    return lastHeartbeat_.isValid() ? int(lastHeartbeat_.elapsed() / 1000) : -1;
}

void MavVehicle::setControlEnabled(bool on)
{
    if (on == controlEnabled_)
        return;
    controlEnabled_ = on;
    if (persist_) {
        QSettings s;
        s.beginGroup(kSettingsGroup);
        s.setValue("controlEnabled", on);
    }
    emit controlEnabledChanged();
}

void MavVehicle::setMissionsByPlanner(bool on)
{
    if (on == missionsByPlanner_)
        return;
    missionsByPlanner_ = on;
    if (on)
        plan_.clear();   // not saved: boatMission/planJson stays for a way back
    emit planChanged();
}

void MavVehicle::setVehicleRoute(const QVector<int>& seqs, const QVector<double>& lats, const QVector<double>& lons)
{
    QVector<mission_progress::Geo> route;
    const int n = int(std::min({ seqs.size(), lats.size(), lons.size() }));
    route.reserve(n);
    for (int i = 0; i < n; ++i)
        route.append({ lats.at(i), lons.at(i) });
    vehicleRouteSeqs_ = seqs.mid(0, n);
    vehicleRoute_ = route;
}

bool MavVehicle::refusedForPlanner(const char* what)
{
    if (!missionsByPlanner_)
        return false;
    qInfo().noquote() << QStringLiteral("boat: %1 ignored: the missions and the commands go through the mission planner")
                         .arg(QLatin1String(what));
    return true;
}

QString MavVehicle::modeNameOf(int mode)
{
    switch (mode) {
    case ModeManual: return QStringLiteral("MANUAL");
    case ModeAcro: return QStringLiteral("ACRO");
    case ModeSteering: return QStringLiteral("STEERING");
    case ModeHold: return QStringLiteral("HOLD");
    case ModeLoiter: return QStringLiteral("LOITER");
    case ModeFollow: return QStringLiteral("FOLLOW");
    case ModeSimple: return QStringLiteral("SIMPLE");
    case ModeAuto: return QStringLiteral("AUTO");
    case ModeRtl: return QStringLiteral("RTL");
    case ModeSmartRtl: return QStringLiteral("SMART_RTL");
    case ModeGuided: return QStringLiteral("GUIDED");
    case ModeInitialising: return QStringLiteral("INITIALISING");
    }
    return mode < 0 ? QString() : QStringLiteral("MODE %1").arg(mode);
}

void MavVehicle::feed(const QByteArray& bytes)
{
    mavlink_message_t msg;
    mavlink_status_t status;
    for (char c : bytes) {
        if (mavlink_parse_char(kRxChan, static_cast<uint8_t>(c), &msg, &status))
            handleMessage(&msg);
    }
}

void MavVehicle::sendMessage(const void* p)
{
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    const uint16_t n = mavlink_msg_to_send_buffer(buf, static_cast<const mavlink_message_t*>(p));
    emit sendBytes(QByteArray(reinterpret_cast<const char*>(buf), n));
}

void MavVehicle::sendHeartbeat()
{
    mavlink_message_t msg;
    mavlink_msg_heartbeat_pack_chan(kGcsSystemId, kGcsComponentId, kTxChan, &msg, MAV_TYPE_GCS,
                                    MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
    sendMessage(&msg);
}

void MavVehicle::sendCommandLong(uint16_t command, float p1, float p2, float p3, float p4, float p5, float p6, float p7)
{
    if (!detected())
        return;
    mavlink_message_t msg;
    mavlink_msg_command_long_pack_chan(kGcsSystemId, kGcsComponentId, kTxChan, &msg, uint8_t(vehicleSysId_),
                                       uint8_t(vehicleCompId_), command, 0, p1, p2, p3, p4, p5, p6, p7);
    sendMessage(&msg);
}

void MavVehicle::requestMessage(uint32_t messageId)
{
    sendCommandLong(MAV_CMD_REQUEST_MESSAGE, float(messageId));
}

void MavVehicle::sendParamSet(const char* id, float value, int type)
{
    if (!detected())
        return;
    char name[16] = {};                 // the packer copies all 16 bytes
    std::strncpy(name, id, sizeof(name));
    mavlink_message_t msg;
    mavlink_msg_param_set_pack_chan(kGcsSystemId, kGcsComponentId, kTxChan, &msg, uint8_t(vehicleSysId_),
                                    uint8_t(vehicleCompId_), name, value, uint8_t(type));
    sendMessage(&msg);
}

void MavVehicle::requestParam(const char* id)
{
    if (!detected())
        return;
    char name[16] = {};                 // the packer copies all 16 bytes
    std::strncpy(name, id, sizeof(name));
    mavlink_message_t msg;
    mavlink_msg_param_request_read_pack_chan(kGcsSystemId, kGcsComponentId, kTxChan, &msg, uint8_t(vehicleSysId_),
                                             uint8_t(vehicleCompId_), name, -1);
    sendMessage(&msg);
}

void MavVehicle::requestStreams()
{
    // Only for a vehicle that streams nothing on its own (SITL on a fresh EEPROM). The boat's autopilot
    // streams at its SRx_ rates, and a request would overwrite those, so it is never sent to it.
    streamsRequested_ = true;
    const uint8_t streams[] = { MAV_DATA_STREAM_EXTENDED_STATUS, MAV_DATA_STREAM_POSITION, MAV_DATA_STREAM_EXTRA2 };
    for (uint8_t id : streams) {
        mavlink_message_t msg;
        mavlink_msg_request_data_stream_pack_chan(kGcsSystemId, kGcsComponentId, kTxChan, &msg, uint8_t(vehicleSysId_),
                                                  uint8_t(vehicleCompId_), id, 2, 1);
        sendMessage(&msg);
    }
}

void MavVehicle::onSecondTick()
{
    if (!detected())
        return;
    ++secondsSinceDetect_;

    const bool now = connected();
    if (now != wasConnected_) {
        wasConnected_ = now;
        if (!now) {
            addMessage(tr("Link to the boat lost"));
            linkLostAt_.start();
            lostSinceFix_ = true;
        }
    }
    emit linkChanged();   // heartbeatAgeS moves every second
    updateProgress();
    updateGhost();
    if (linkTrack_.version() != linkTrackEmitted_) {
        linkTrackEmitted_ = linkTrack_.version();
        emit linkTrackChanged();
    }

    if (controlEnabled_ && lastHeartbeat_.isValid() && lastHeartbeat_.elapsed() < kHeartbeatKeepMs)
        sendHeartbeat();

    if (!positionSeen_ && !streamsRequested_ && secondsSinceDetect_ >= 5)
        requestStreams();
    if (!homeValid_ && secondsSinceDetect_ % 10 == 0)
        requestMessage(MAVLINK_MSG_ID_HOME_POSITION);
    if (now) {
        // The boat may stream EKF_STATUS_REPORT (SRx_EXTRA3); if not, it is asked for, without touching its rates.
        // An autopilot that never answers (UNSUPPORTED, not ArduPilot) is asked kEkfPollsMax times, not forever.
        if ((!ekfSeen_.isValid() || ekfSeen_.elapsed() >= kEkfPollMs) && secondsSinceDetect_ % 2 == 0
            && ekfPollsUnanswered_ < kEkfPollsMax) {
            requestMessage(MAVLINK_MSG_ID_EKF_STATUS_REPORT);
            ++ekfPollsUnanswered_;
        }
        const int s = secondsSinceDetect_ - settingsEpoch_;
        const bool window = (s >= 3 && s < 9) || (s >= 13 && s < 19) || (s >= 23 && s < 29);
        if (!settingsLogged_ && window) {
            int sent = 0;
            for (int k = 0; k < kSettingsCount && sent < kSettingsPerTick; ++k) {
                const int i = (settingsCursor_ + k) % kSettingsCount;
                if (!settings_.contains(QLatin1String(kSettingsToRead[i].name))) {
                    requestParam(kSettingsToRead[i].name);
                    ++sent;
                    settingsCursor_ = (i + 1) % kSettingsCount;
                }
            }
        }
    }
    if (!settingsLogged_ && secondsSinceDetect_ - settingsEpoch_ == 33 && !settings_.isEmpty())
        logSettings();   // with whatever came; names this firmware lacks never answer
    updateNav();   // a report that stops coming turns the state to unknown
}

// ---- incoming --------------------------------------------------------------------------------------

void MavVehicle::handleMessage(const void* p)
{
    const mavlink_message_t& msg = *static_cast<const mavlink_message_t*>(p);

    if (msg.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
        mavlink_heartbeat_t hb;
        mavlink_msg_heartbeat_decode(&msg, &hb);
        const bool isVehicle = hb.autopilot != MAV_AUTOPILOT_INVALID && hb.type != MAV_TYPE_GCS;
        if (!detected() && isVehicle) {
            vehicleSysId_ = msg.sysid;
            vehicleCompId_ = msg.compid;
            secondsSinceDetect_ = 0;
            settingsEpoch_ = 0;
            const QString ap = hb.autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA ? QStringLiteral("ArduPilot")
                             : hb.autopilot == MAV_AUTOPILOT_PX4 ? QStringLiteral("PX4")
                                                                 : tr("autopilot %1").arg(hb.autopilot);
            const QString kind = hb.type == MAV_TYPE_SURFACE_BOAT ? tr("boat")
                               : hb.type == MAV_TYPE_GROUND_ROVER ? tr("rover")
                                                                  : tr("type %1").arg(hb.type);
            autopilotText_ = ap + QStringLiteral(" · ") + kind;
            emit identityChanged();
            addMessage(tr("Autopilot found: %1, system %2").arg(autopilotText_).arg(vehicleSysId_));
            requestMessage(MAVLINK_MSG_ID_AUTOPILOT_VERSION);
            requestMessage(MAVLINK_MSG_ID_HOME_POSITION);
            if (controlEnabled_)
                sendHeartbeat();
            // What mission does the boat hold? Compared with the plan as soon as it is read.
            QTimer::singleShot(1500, this, [this]() {
                if (transferState_ == TransferIdle && connected() && !missionsByPlanner_)
                    startDownload(false);
            });
        }
        if (msg.sysid != vehicleSysId_ || msg.compid != vehicleCompId_)
            return;
        const bool wasConn = connected();
        lastHeartbeat_.start();
        const bool nowArmed = (hb.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;
        const int nowMode = (hb.base_mode & MAV_MODE_FLAG_CUSTOM_MODE_ENABLED) ? int(hb.custom_mode) : -1;
        if (nowArmed != armed_ || nowMode != mode_) {
            // The first state seen goes to the log too, so the log alone tells whether the boat is armed.
            if (mode_ < 0 && nowMode >= 0)
                addMessage(tr("Mode %1, %2").arg(modeNameOf(nowMode), nowArmed ? tr("armed") : tr("disarmed")));
            if (mode_ >= 0 && nowMode != mode_)
                addMessage(tr("Mode %1").arg(modeNameOf(nowMode)));
            if (mode_ >= 0 && nowArmed != armed_)
                addMessage(nowArmed ? tr("Armed") : tr("Disarmed"));
            // ArduPilot moves home to where the boat is armed (the launch point): asked for, not only left to its
            // own broadcast over a lossy link (the "From launch point" widget and the return home read it).
            if (nowArmed && !armed_)
                requestMessage(MAVLINK_MSG_ID_HOME_POSITION);
            armed_ = nowArmed;
            mode_ = nowMode;
            emit stateChanged();
        }
        if (!wasConn) {
            wasConnected_ = true;
            if (linkLostAt_.isValid()) {
                // the loss was logged kConnectedMs after the last heartbeat before it
                addMessage(tr("Link to the boat restored after %1 s").arg((linkLostAt_.elapsed() + kConnectedMs) / 1000.0, 0, 'f', 1));
                linkLostAt_.invalidate();
                // The read windows may have passed while the link was down: the heading source and the EKF
                // threshold must be known, so read again (the missing names only).
                if (!settings_.contains(QStringLiteral("EK3_SRC1_YAW")) || !settings_.contains(QStringLiteral("FS_EKF_THRESH"))) {
                    settingsEpoch_ = secondsSinceDetect_;
                    settingsLogged_ = false;
                }
                ekfPollsUnanswered_ = 0;
            }
            emit linkChanged();
        }
    } else {
        if (!detected() || msg.sysid != vehicleSysId_)
            return;
        switch (msg.msgid) {
        case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
            mavlink_global_position_int_t g;
            mavlink_msg_global_position_int_decode(&msg, &g);
            positionSeen_ = true;
            const bool valid = g.lat != 0 || g.lon != 0;
            positionValid_ = valid;
            lat_ = g.lat * 1e-7;
            lon_ = g.lon * 1e-7;
            if (g.hdg != UINT16_MAX)
                headingDeg_ = g.hdg / 100.0;
            groundSpeed_ = std::hypot(double(g.vx), double(g.vy)) / 100.0;
            if (valid) {
                lastPositionAt_.start();
                speed_.add(groundSpeed_, clock_.elapsed());
                linkTrack_.add(lat_, lon_, linkRssi_, lostSinceFix_);
                lostSinceFix_ = false;
            }
            emit positionChanged();
            break;
        }
        case MAVLINK_MSG_ID_GPS_RAW_INT: {
            mavlink_gps_raw_int_t g;
            mavlink_msg_gps_raw_int_decode(&msg, &g);
            const int sats = g.satellites_visible == 255 ? -1 : int(g.satellites_visible);
            const double hdop = g.eph == UINT16_MAX ? -1.0 : g.eph / 100.0;
            if (int(g.fix_type) != gpsFix_ || sats != gpsSats_ || !qFuzzyCompare(hdop + 2.0, gpsHdop_ + 2.0)) {
                gpsFix_ = g.fix_type;
                gpsSats_ = sats;
                gpsHdop_ = hdop;
                emit gpsChanged();
            }
            gpsYaw_[0] = g.yaw;        // a MAVLink 1 frame or an old firmware leaves the extension at 0
            updateGpsHeading();
            break;
        }
        case MAVLINK_MSG_ID_GPS2_RAW: {
            mavlink_gps2_raw_t g;
            mavlink_msg_gps2_raw_decode(&msg, &g);
            gps2Fix_ = g.fix_type;
            gps2Sats_ = g.satellites_visible == 255 ? -1 : int(g.satellites_visible);
            gpsYaw_[1] = g.yaw;
            updateGpsHeading();
            break;
        }
        case MAVLINK_MSG_ID_EKF_STATUS_REPORT: {
            mavlink_ekf_status_report_t e;
            mavlink_msg_ekf_status_report_decode(&msg, &e);
            ekfFlags_ = e.flags;
            ekfVelVar_ = e.velocity_variance;
            ekfPosVar_ = e.pos_horiz_variance;
            ekfMagVar_ = e.compass_variance;
            ekfSeen_.start();
            ekfPollsUnanswered_ = 0;
            updateNav();
            break;
        }
        case MAVLINK_MSG_ID_PARAM_VALUE: {
            mavlink_param_value_t pv;
            mavlink_msg_param_value_decode(&msg, &pv);
            char name[17] = {};
            std::memcpy(name, pv.param_id, 16);
            const QString key = QString::fromLatin1(name);
            bool wanted = false;
            for (const Setting& x : kSettingsToRead)
                wanted = wanted || key == QLatin1String(x.name);
            if (!wanted)
                break;
            const bool hadIt = settings_.contains(key);
            const double before = settings_.value(key);
            settings_[key] = pv.param_value;
            settingTypes_[key] = pv.param_type;
            emit settingsChanged();
            if (key == QLatin1String("EK3_SRC1_YAW") && hadIt && before != pv.param_value) {
                addMessage(tr("Heading source on the boat: %1 (EK3_SRC1_YAW %2)")
                               .arg(headingSourceName(int(pv.param_value))).arg(pv.param_value));
                yawFallbackActive_ = false;
            }
            if (key == QLatin1String("EK3_SRC1_YAW"))
                emit headingInUseChanged();
            if (key == QLatin1String("FS_EKF_THRESH")) {
                ekfThresh_ = pv.param_value;
                ekfThreshKnown_ = true;
                updateNav();
            }
            if (!settingsLogged_ && settings_.size() == kSettingsCount)
                logSettings();
            break;
        }
        case MAVLINK_MSG_ID_SYS_STATUS: {
            mavlink_sys_status_t s;
            mavlink_msg_sys_status_decode(&msg, &s);
            batteryVoltage_ = s.voltage_battery == UINT16_MAX ? -1.0 : s.voltage_battery / 1000.0;
            batteryRemaining_ = s.battery_remaining;
            if (progressValid_)
                battery_.add(clock_.elapsed(), batteryRemaining_);
            emit batteryChanged();
            // The autopilot's own verdict on what it has and uses; it holds whether pre-arm checks run or not.
            struct Sensor { uint32_t bit; const char* name; };
            static const Sensor sensors[] = {
                { MAV_SYS_STATUS_SENSOR_3D_GYRO, QT_TR_NOOP("gyro") },
                { MAV_SYS_STATUS_SENSOR_3D_ACCEL, QT_TR_NOOP("accelerometer") },
                { MAV_SYS_STATUS_SENSOR_3D_MAG, QT_TR_NOOP("compass") },
                { MAV_SYS_STATUS_SENSOR_ABSOLUTE_PRESSURE, QT_TR_NOOP("barometer") },
                { MAV_SYS_STATUS_SENSOR_GPS, QT_TR_NOOP("GPS") },
                { MAV_SYS_STATUS_AHRS, QT_TR_NOOP("AHRS/EKF") },
                { MAV_SYS_STATUS_SENSOR_RC_RECEIVER, QT_TR_NOOP("RC receiver") },
                { MAV_SYS_STATUS_SENSOR_BATTERY, QT_TR_NOOP("battery") },
                { MAV_SYS_STATUS_PREARM_CHECK, QT_TR_NOOP("pre-arm checks") },
            };
            const uint32_t used = s.onboard_control_sensors_present & s.onboard_control_sensors_enabled;
            QStringList bad;
            for (const Sensor& x : sensors) {
                if ((used & x.bit) && !(s.onboard_control_sensors_health & x.bit))
                    bad << tr(x.name);
            }
            const QString health = bad.isEmpty() ? QString() : tr("not healthy: %1").arg(bad.join(QStringLiteral(", ")));
            if (!healthSeen_ || health != healthText_) {
                healthSeen_ = true;
                healthText_ = health;
                emit healthChanged();
                ++healthChanges_;
                healthLogTimer_.start();   // logged once it has held for kSettleLogMs
            }
            break;
        }
        case MAVLINK_MSG_ID_HOME_POSITION: {
            mavlink_home_position_t h;
            mavlink_msg_home_position_decode(&msg, &h);
            const bool changed = !homeValid_ || h.latitude != qRound(homeLat_ * 1e7) || h.longitude != qRound(homeLon_ * 1e7);
            homeValid_ = h.latitude != 0 || h.longitude != 0;
            homeLat_ = h.latitude * 1e-7;
            homeLon_ = h.longitude * 1e-7;
            if (changed)
                emit homeChanged();
            break;
        }
        case MAVLINK_MSG_ID_MISSION_CURRENT: {
            mavlink_mission_current_t c;
            mavlink_msg_mission_current_decode(&msg, &c);
            missionCurrentFresh_ = true;
            if (int(c.seq) != missionCurrent_) {
                missionCurrent_ = c.seq;
                emit missionStateChanged();
            }
            break;
        }
        case MAVLINK_MSG_ID_MISSION_ITEM_REACHED: {
            mavlink_mission_item_reached_t r;
            mavlink_msg_mission_item_reached_decode(&msg, &r);
            missionReached_ = r.seq;
            addMessage(tr("Reached mission item %1").arg(r.seq));
            emit missionStateChanged();
            break;
        }
        case MAVLINK_MSG_ID_NAV_CONTROLLER_OUTPUT: {
            mavlink_nav_controller_output_t n;
            mavlink_msg_nav_controller_output_decode(&msg, &n);
            wpDistance_ = n.wp_dist;
            emit missionStateChanged();
            break;
        }
        case MAVLINK_MSG_ID_STATUSTEXT: {
            mavlink_statustext_t t;
            mavlink_msg_statustext_decode(&msg, &t);
            const QString text = QString::fromUtf8(t.text, int(strnlen(t.text, sizeof(t.text)))).trimmed();
            // ArduPilot 4.4 AP_NavEKF3_MagFusion.cpp: "EKF3 IMU%u yaw fallback active" - the compass took over
            if (text.startsWith(QLatin1String("EKF3 IMU")) && text.endsWith(QLatin1String("yaw fallback active"))
                && !yawFallbackActive_) {
                yawFallbackActive_ = true;
                emit headingInUseChanged();
            }
            if (!text.isEmpty()) {
                addMessage(text);
                if (t.severity <= MAV_SEVERITY_WARNING)
                    emit vehicleWarning(text);
            }
            break;
        }
        case MAVLINK_MSG_ID_AUTOPILOT_VERSION: {
            mavlink_autopilot_version_t v;
            mavlink_msg_autopilot_version_decode(&msg, &v);
            const QString fw = QStringLiteral("%1.%2.%3").arg((v.flight_sw_version >> 24) & 0xFF)
                                                          .arg((v.flight_sw_version >> 16) & 0xFF)
                                                          .arg((v.flight_sw_version >> 8) & 0xFF);
            if (fw != firmwareText_) {
                firmwareText_ = fw;
                emit identityChanged();
            }
            break;
        }
        case MAVLINK_MSG_ID_COMMAND_ACK: {
            mavlink_command_ack_t a;
            mavlink_msg_command_ack_decode(&msg, &a);
            if (!steps_.isEmpty() && steps_.first().ackCommand == a.command
                && a.result != MAV_RESULT_ACCEPTED && a.result != MAV_RESULT_IN_PROGRESS) {
                finishSteps(false, tr("%1: %2").arg(steps_.first().what, commandResultText(a.result)));
                return;
            }
            break;
        }
        case MAVLINK_MSG_ID_MISSION_REQUEST_INT:
        case MAVLINK_MSG_ID_MISSION_REQUEST: {
            // ArduPilot 4.4 asks with MISSION_REQUEST even when it is given MISSION_ITEM_INT (SITL, 2026-09-26).
            uint8_t ts, tc, type;
            uint16_t seq;
            if (msg.msgid == MAVLINK_MSG_ID_MISSION_REQUEST_INT) {
                mavlink_mission_request_int_t r;
                mavlink_msg_mission_request_int_decode(&msg, &r);
                ts = r.target_system; tc = r.target_component; seq = r.seq; type = r.mission_type;
            } else {
                mavlink_mission_request_t r;
                mavlink_msg_mission_request_decode(&msg, &r);
                ts = r.target_system; tc = r.target_component; seq = r.seq; type = r.mission_type;
            }
            if (transferState_ != TransferUploading || ts != kGcsSystemId || tc != kGcsComponentId
                || type != MAV_MISSION_TYPE_MISSION)
                break;
            uploadRequestSeen_ = true;
            transferRetries_ = 0;
            lastRequestedSeq_ = seq;
            if (seq < upItems_.size()) {
                sendMissionItem(seq);
                setTransfer(TransferUploading, (seq + 1) * 90 / upItems_.size(),
                            tr("Uploading item %1 of %2").arg(seq + 1).arg(upItems_.size()));
                transferTimer_.start();
            }
            break;
        }
        case MAVLINK_MSG_ID_MISSION_ACK: {
            mavlink_mission_ack_t a;
            mavlink_msg_mission_ack_decode(&msg, &a);
            if (a.target_system != kGcsSystemId || a.target_component != kGcsComponentId
                || a.mission_type != MAV_MISSION_TYPE_MISSION)
                break;
            if (transferState_ == TransferUploading) {
                if (a.type == MAV_MISSION_ACCEPTED)
                    startDownload(true);
                else
                    finishTransfer(false, tr("The boat refused the mission: %1").arg(missionResultText(a.type)));
            } else if (transferState_ == TransferDownloading && a.type != MAV_MISSION_ACCEPTED) {
                finishTransfer(false, tr("Reading the mission failed: %1").arg(missionResultText(a.type)));
            }
            break;
        }
        case MAVLINK_MSG_ID_MISSION_COUNT: {
            mavlink_mission_count_t c;
            mavlink_msg_mission_count_decode(&msg, &c);
            if (transferState_ != TransferDownloading || c.target_system != kGcsSystemId
                || c.target_component != kGcsComponentId || c.mission_type != MAV_MISSION_TYPE_MISSION || downCount_ >= 0)
                break;
            downCount_ = c.count;
            missionCount_ = c.count;
            emit missionStateChanged();
            downItems_ = QVector<Item>(downCount_);
            downNext_ = 0;
            transferRetries_ = 0;
            if (downCount_ == 0) {
                vehicleItems_.clear();
                vehicleItemsValid_ = true;
                updatePlanOnVehicle();
                if (importAfterDownload_) {
                    importAfterDownload_ = false;
                    finishTransfer(false, tr("No mission on the boat: nothing to load, the plan is unchanged"));
                    break;
                }
                finishTransfer(!verifyAfterUpload_, verifyAfterUpload_ ? tr("Read-back is empty") : tr("No mission on the boat"));
            } else {
                requestDownloadItem(0);
            }
            break;
        }
        case MAVLINK_MSG_ID_MISSION_ITEM_INT:
        case MAVLINK_MSG_ID_MISSION_ITEM: {
            if (transferState_ != TransferDownloading || downCount_ <= 0)
                break;
            Item it;
            uint16_t seq;
            uint8_t ts, tc, type;
            if (msg.msgid == MAVLINK_MSG_ID_MISSION_ITEM_INT) {
                mavlink_mission_item_int_t m;
                mavlink_msg_mission_item_int_decode(&msg, &m);
                seq = m.seq; ts = m.target_system; tc = m.target_component; type = m.mission_type;
                it.command = m.command; it.frame = m.frame; it.p1 = m.param1; it.p2 = m.param2; it.p3 = m.param3;
                it.p4 = m.param4; it.x = m.x; it.y = m.y; it.z = m.z;
            } else {
                mavlink_mission_item_t m;
                mavlink_msg_mission_item_decode(&msg, &m);
                seq = m.seq; ts = m.target_system; tc = m.target_component; type = m.mission_type;
                it.command = m.command; it.frame = m.frame; it.p1 = m.param1; it.p2 = m.param2; it.p3 = m.param3;
                it.p4 = m.param4; it.x = qRound(m.x * 1e7); it.y = qRound(m.y * 1e7); it.z = m.z;
            }
            if (ts != kGcsSystemId || tc != kGcsComponentId || type != MAV_MISSION_TYPE_MISSION || seq != downNext_)
                break;
            downItems_[seq] = it;
            ++downNext_;
            transferRetries_ = 0;
            if (downNext_ < downCount_) {
                setTransfer(TransferDownloading, verifyAfterUpload_ ? 90 + downNext_ * 10 / downCount_ : downNext_ * 100 / downCount_,
                            tr("Reading item %1 of %2").arg(downNext_ + 1).arg(downCount_));
                requestDownloadItem(downNext_);
            } else {
                mavlink_message_t ack;
                mavlink_msg_mission_ack_pack_chan(kGcsSystemId, kGcsComponentId, kTxChan, &ack, uint8_t(vehicleSysId_),
                                                  uint8_t(vehicleCompId_), MAV_MISSION_ACCEPTED, MAV_MISSION_TYPE_MISSION, 0);
                sendMessage(&ack);
                vehicleItems_ = downItems_;
                vehicleItemsValid_ = true;
                if (importAfterDownload_) {
                    importAfterDownload_ = false;
                    importVehicleItems();          // sets the plan, reports, and finishes the transfer
                    break;
                }
                updatePlanOnVehicle();
                if (verifyAfterUpload_) {
                    finishTransfer(planOnVehicle_, planOnVehicle_
                                   ? tr("Mission on the boat: %1 points, checked").arg(plan_.size())
                                   : tr("The mission read back from the boat differs from the plan"));
                } else {
                    finishTransfer(true, planOnVehicle_ ? tr("The boat holds this plan (%1 points)").arg(plan_.size())
                                                        : tr("The boat holds another mission (%1 items)").arg(downCount_));
                }
            }
            break;
        }
        default:
            break;
        }
    }

    // A running command step may be satisfied by what just arrived (mode, armed, current item).
    if (!steps_.isEmpty() && steps_.first().done && steps_.first().done()) {
        stepTimer_.stop();
        steps_.removeFirst();
        if (steps_.isEmpty())
            finishSteps(true, QString());
        else
            startStep();
    }
}

// ---- plan ------------------------------------------------------------------------------------------

QVariantList MavVehicle::plan() const
{
    QVariantList out;
    out.reserve(plan_.size());
    for (const Point& p : plan_) {
        QVariantMap m;
        m.insert(QStringLiteral("lat"), p.lat);
        m.insert(QStringLiteral("lon"), p.lon);
        out.append(m);
    }
    return out;
}

void MavVehicle::addPoint(double lat, double lon)
{
    insertPoint(plan_.size(), lat, lon);
}

void MavVehicle::insertPoint(int index, double lat, double lon)
{
    if (refusedForPlanner("insertPoint"))
        return;
    if (!std::isfinite(lat) || !std::isfinite(lon) || qAbs(lat) > 90.0 || qAbs(lon) > 180.0)
        return;
    index = qBound(0, index, int(plan_.size()));
    plan_.insert(index, Point{lat, lon});
    qInfo().noquote() << QStringLiteral("boat: point %1 of %2 at %3, %4").arg(index + 1).arg(plan_.size())
                             .arg(lat, 0, 'f', 7).arg(lon, 0, 'f', 7);
    savePlan();
}

void MavVehicle::movePoint(int index, double lat, double lon)
{
    if (refusedForPlanner("movePoint"))
        return;
    if (index < 0 || index >= plan_.size() || !std::isfinite(lat) || !std::isfinite(lon))
        return;
    plan_[index] = Point{lat, lon};
    savePlan();
}

void MavVehicle::removePoint(int index)
{
    if (refusedForPlanner("removePoint"))
        return;
    if (index < 0 || index >= plan_.size())
        return;
    plan_.removeAt(index);
    savePlan();
}

void MavVehicle::clearPlan()
{
    if (refusedForPlanner("clearPlan"))
        return;
    if (plan_.isEmpty())
        return;
    plan_.clear();
    savePlan();
}

void MavVehicle::setPlanSpeed(double metresPerSecond)
{
    if (refusedForPlanner("setPlanSpeed"))
        return;
    const double v = std::isfinite(metresPerSecond) ? qBound(0.0, metresPerSecond, 20.0) : 0.0;
    if (qFuzzyCompare(v + 1.0, planSpeed_ + 1.0))
        return;
    planSpeed_ = v;
    savePlan();
}

void MavVehicle::setPlanReturnHome(bool on)
{
    if (refusedForPlanner("setPlanReturnHome"))
        return;
    if (on == planReturnHome_)
        return;
    planReturnHome_ = on;
    savePlan();
}

double MavVehicle::planLengthM() const
{
    double sum = 0.0;
    for (int i = 1; i < plan_.size(); ++i)
        sum += distanceM(plan_[i - 1].lat, plan_[i - 1].lon, plan_[i].lat, plan_[i].lon);
    return sum;
}

QVector<mission_progress::Geo> MavVehicle::planGeo() const
{
    QVector<mission_progress::Geo> out;
    out.reserve(plan_.size());
    for (const Point& p : plan_)
        out.append({ p.lat, p.lon });
    return out;
}

QVector<mission_progress::Geo> MavVehicle::autoRouteAhead(const mission_progress::Geo& boat) const
{
    namespace mp = mission_progress;
    if (!missionsByPlanner_) {
        const int target = currentPlanIndex();
        return target >= 0 ? mp::routeAhead(boat, planGeo(), target, planReturnHome_, homeValid_, mp::Geo{ homeLat_, homeLon_ })
                           : QVector<mp::Geo>{};
    }
    if (missionCurrent_ < 0)
        return {};
    int i = 0;
    while (i < vehicleRouteSeqs_.size() && vehicleRouteSeqs_.at(i) < missionCurrent_)
        ++i;
    if (i >= vehicleRoute_.size())
        return {};
    QVector<mp::Geo> route{ boat };
    route.append(vehicleRoute_.mid(i));
    return route;
}

// Each second: what the running mission has left (armed, AUTO on our plan, or RTL home). The speed is the boat's,
// smoothed, or the plan's while it is too slow to tell; the battery trend runs from the start of the mode.
void MavVehicle::updateProgress()
{
    namespace mp = mission_progress;
    const mp::Geo boat{ lat_, lon_ }, home{ homeLat_, homeLon_ };
    const QVector<mp::Geo> ahead = armed_ && mode_ == ModeAuto ? autoRouteAhead(boat) : QVector<mp::Geo>{};
    const bool autoRun = ahead.size() > 1;
    const bool rtlRun = armed_ && mode_ == ModeRtl && homeValid_;
    if (!positionValid_ || !(autoRun || rtlRun)) {
        if (progressValid_) {
            progressValid_ = false;
            remainingM_ = etaS_ = mp::kNaN;
            batteryAtEnd_ = -1;
            battery_.reset();
            progressMode_ = -1;
            emit missionProgressChanged();
        }
        return;
    }
    if (progressMode_ != mode_) {
        battery_.reset();
        progressMode_ = mode_;
        battery_.add(clock_.elapsed(), batteryRemaining_);
    }
    const QVector<mp::Geo> route = autoRun ? ahead : QVector<mp::Geo>{ boat, home };
    remainingM_ = mp::lengthM(route);
    const double v = speed_.value();
    etaS_ = mp::etaS(remainingM_, (std::isfinite(v) && v >= mp::kMinSpeedMs) ? v : planSpeed_);
    batteryAtEnd_ = battery_.atEnd(etaS_);
    progressValid_ = true;
    emit missionProgressChanged();
}

// Each second while the link is lost: the boat from its last position, along what it was doing then - the
// mission (AUTO keeps it when the ground station is lost, FS_GCS_ENABLE 2) or the way home (RTL).
void MavVehicle::updateGhost()
{
    namespace mp = mission_progress;
    const bool lost = detected() && !connected() && positionValid_ && lastPositionAt_.isValid();
    if (!lost) {
        if (ghostValid_) {
            ghostValid_ = false;
            emit ghostChanged();
        }
        return;
    }
    const mp::Geo boat{ lat_, lon_ }, home{ homeLat_, homeLon_ };
    QVector<mp::Geo> route;
    if (armed_ && mode_ == ModeAuto)
        route = autoRouteAhead(boat);
    else if (armed_ && mode_ == ModeRtl && homeValid_)
        route = { boat, home };
    const double v = speed_.value();
    const double speed = (std::isfinite(v) && v >= mp::kMinSpeedMs) ? v : planSpeed_;
    ghostMoving_ = route.size() > 1 && speed >= mp::kMinSpeedMs;
    const mp::Geo g = ghostMoving_ ? mp::alongRoute(route, speed, lastPositionAt_.elapsed() / 1000.0) : boat;
    ghostLat_ = g.lat;
    ghostLon_ = g.lon;
    ghostValid_ = true;
    emit ghostChanged();
}

QVariantList MavVehicle::linkTrack() const
{
    QVariantList out;
    const auto& samples = linkTrack_.samples();
    out.reserve(samples.size());
    for (const LinkTrack::Sample& s : samples)
        out.append(QVariantMap{ { QStringLiteral("lat"), s.lat }, { QStringLiteral("lon"), s.lon }, { QStringLiteral("q"), s.quality } });
    return out;
}

QList<double> MavVehicle::linkTrackLats() const
{
    QList<double> out;
    out.reserve(linkTrack_.samples().size());
    for (const LinkTrack::Sample& s : linkTrack_.samples())
        out.append(s.lat);
    return out;
}

QList<double> MavVehicle::linkTrackLons() const
{
    QList<double> out;
    out.reserve(linkTrack_.samples().size());
    for (const LinkTrack::Sample& s : linkTrack_.samples())
        out.append(s.lon);
    return out;
}

QList<int> MavVehicle::linkTrackQualities() const
{
    QList<int> out;
    out.reserve(linkTrack_.samples().size());
    for (const LinkTrack::Sample& s : linkTrack_.samples())
        out.append(s.quality);
    return out;
}

void MavVehicle::setLinkTrackShown(bool on)
{
    if (on == linkTrackShown_)
        return;
    linkTrackShown_ = on;
    if (persist_) {
        QSettings s;
        s.beginGroup(kSettingsGroup);
        s.setValue(QStringLiteral("linkTrackShown"), on);
    }
    emit linkTrackShownChanged();
}

void MavVehicle::clearLinkTrack()
{
    linkTrack_.clear();
    linkTrackEmitted_ = linkTrack_.version();
    emit linkTrackChanged();
}

bool MavVehicle::fillLinkTrackForTest(int n, int runLen, double lat, double lon)
{
    if (!std::isfinite(lat) || !std::isfinite(lon))
        return false;
    linkTrack_.clear();
    constexpr double kStepM = 2.5;
    constexpr int kRow = 40;   // 100 m a row, rows 10 m apart
    const double mLat = 1.0 / 111320.0;
    const double mLon = 1.0 / (111320.0 * std::cos(lat * 3.14159265358979323846 / 180.0));
    static const int kRssi[4] = { -55, -65, -75, -85 };   // good, fair, weak, bad
    runLen = std::max(1, runLen);
    for (int i = 0; i < std::min(n, LinkTrack::kMaxSamples); ++i) {
        const int row = i / kRow, col = i % kRow;
        const double eastM = (row % 2 == 0 ? col : kRow - 1 - col) * kStepM;
        const double northM = row * 10.0;
        const int run = i / runLen;
        const bool loss = i % runLen == 0 && run % 10 == 9;
        linkTrack_.add(lat + northM * mLat, lon + eastM * mLon, kRssi[run % 4], loss);
    }
    linkTrackEmitted_ = linkTrack_.version();
    emit linkTrackChanged();
    return true;
}

bool MavVehicle::firstPlanPoint(double* lat, double* lon) const
{
    if (plan_.isEmpty())
        return false;
    *lat = plan_.first().lat;
    *lon = plan_.first().lon;
    return true;
}

void MavVehicle::setLinkRssi(int dBm)
{
    if (dBm == linkRssi_)
        return;
    linkRssi_ = dBm;
    emit linkRssiChanged();
}

double MavVehicle::distanceM(double lat1, double lon1, double lat2, double lon2)
{
    const double r = 6371008.8;
    const double p1 = qDegreesToRadians(lat1), p2 = qDegreesToRadians(lat2);
    const double dp = p2 - p1, dl = qDegreesToRadians(lon2 - lon1);
    const double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2.0 * r * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

int MavVehicle::currentPlanIndex() const
{
    if (!planOnVehicle_ || missionCurrent_ < 0)
        return -1;
    const int firstNav = planSpeed_ > 0.0 ? 2 : 1;
    const int idx = missionCurrent_ - firstNav;
    return (idx >= 0 && idx < plan_.size()) ? idx : -1;
}

QVector<MavVehicle::Item> MavVehicle::buildItems() const
{
    QVector<Item> v;
    if (plan_.isEmpty())
        return v;
    // Item 0 is the home slot: ArduPilot keeps its own home there and ignores what is sent.
    Item home;
    home.command = MAV_CMD_NAV_WAYPOINT;
    home.frame = MAV_FRAME_GLOBAL;
    home.x = qRound((homeValid_ ? homeLat_ : plan_.first().lat) * 1e7);
    home.y = qRound((homeValid_ ? homeLon_ : plan_.first().lon) * 1e7);
    v.append(home);
    if (planSpeed_ > 0.0) {
        Item speed;
        speed.command = MAV_CMD_DO_CHANGE_SPEED;
        speed.frame = MAV_FRAME_MISSION;
        speed.p1 = 1.0f;                     // ground speed
        speed.p2 = float(planSpeed_);
        speed.p3 = -1.0f;                    // throttle unchanged
        v.append(speed);
    }
    for (const Point& p : plan_) {
        Item wp;
        wp.command = MAV_CMD_NAV_WAYPOINT;
        wp.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
        wp.x = qRound(p.lat * 1e7);
        wp.y = qRound(p.lon * 1e7);
        v.append(wp);
    }
    if (planReturnHome_) {
        Item rtl;
        rtl.command = MAV_CMD_NAV_RETURN_TO_LAUNCH;
        rtl.frame = MAV_FRAME_MISSION;
        v.append(rtl);
    }
    return v;
}

bool MavVehicle::itemsMatch(const QVector<Item>& planned, const QVector<Item>& onVehicle)
{
    if (planned.size() != onVehicle.size() || planned.isEmpty())
        return false;
    for (int i = 1; i < planned.size(); ++i) {          // item 0 is the vehicle's own home
        const Item& a = planned[i];
        const Item& b = onVehicle[i];
        if (a.command != b.command)
            return false;
        if (a.command == MAV_CMD_NAV_WAYPOINT && (a.x != b.x || a.y != b.y || qAbs(a.p1 - b.p1) > 0.01f))
            return false;
        if (a.command == MAV_CMD_DO_CHANGE_SPEED && qAbs(a.p2 - b.p2) > 0.01f)
            return false;
    }
    return true;
}

void MavVehicle::updatePlanOnVehicle()
{
    const bool now = vehicleItemsValid_ && itemsMatch(buildItems(), vehicleItems_);
    if (now != planOnVehicle_) {
        planOnVehicle_ = now;
        emit transferChanged();
        emit missionStateChanged();   // currentPlanIndex depends on it
    }
}

// ---- mission transfer ------------------------------------------------------------------------------

void MavVehicle::setTransfer(int state, int progress, const QString& text)
{
    transferState_ = state;
    transferProgress_ = qBound(0, progress, 100);
    transferText_ = text;
    emit transferChanged();
}

void MavVehicle::finishTransfer(bool ok, const QString& text)
{
    transferTimer_.stop();
    if (!ok)
        importAfterDownload_ = false;
    setTransfer(ok ? TransferDone : TransferFailed, ok ? 100 : transferProgress_, text);
    addMessage(text);
}

void MavVehicle::uploadPlan()
{
    if (refusedForPlanner("uploadPlan"))
        return;
    if (transferState_ == TransferUploading || transferState_ == TransferDownloading)
        return;
    if (!connected()) {
        finishTransfer(false, tr("No link to the boat"));
        return;
    }
    if (plan_.isEmpty()) {
        finishTransfer(false, tr("The plan has no points"));
        return;
    }
    upItems_ = buildItems();
    uploadRequestSeen_ = false;
    lastRequestedSeq_ = -1;
    transferRetries_ = 0;
    setTransfer(TransferUploading, 0, tr("Uploading %1 items").arg(upItems_.size()));
    mavlink_message_t msg;
    mavlink_msg_mission_count_pack_chan(kGcsSystemId, kGcsComponentId, kTxChan, &msg, uint8_t(vehicleSysId_),
                                        uint8_t(vehicleCompId_), uint16_t(upItems_.size()), MAV_MISSION_TYPE_MISSION, 0);
    sendMessage(&msg);
    transferTimer_.start();
}

void MavVehicle::readBack()
{
    if (refusedForPlanner("readBack"))
        return;
    if (transferState_ == TransferUploading || transferState_ == TransferDownloading)
        return;
    if (!connected()) {
        finishTransfer(false, tr("No link to the boat"));
        return;
    }
    startDownload(false);
}

void MavVehicle::loadPlanFromVehicle()
{
    if (refusedForPlanner("loadPlanFromVehicle"))
        return;
    if (transferState_ == TransferUploading || transferState_ == TransferDownloading)
        return;
    if (!connected()) {
        finishTransfer(false, tr("No link to the boat"));
        return;
    }
    importAfterDownload_ = true;
    startDownload(false);
}

bool MavVehicle::importVehicleItems()
{
    // Item 0 is the vehicle's home. What the plan can hold: waypoints, one speed, a return home at the end.
    QVector<Point> pts;
    double speed = 0.0;
    bool rtl = false;
    bool holds = false;
    QStringList skipped;
    for (int i = 1; i < vehicleItems_.size(); ++i) {
        const Item& it = vehicleItems_[i];
        if (it.command == MAV_CMD_NAV_WAYPOINT && (it.x != 0 || it.y != 0)) {
            pts.append(Point{ it.x * 1e-7, it.y * 1e-7 });
            holds = holds || it.p1 > 0.0f;
        } else if (it.command == MAV_CMD_DO_CHANGE_SPEED && it.p2 > 0.0f && speed <= 0.0) {
            speed = std::round(double(it.p2) * 100.0) / 100.0;   // 0.8f is 0.800000011... as a double
        } else if (it.command == MAV_CMD_NAV_RETURN_TO_LAUNCH && i == vehicleItems_.size() - 1) {
            rtl = true;
        } else {
            skipped << tr("item %1 (command %2)").arg(i).arg(it.command);
        }
    }
    if (pts.isEmpty()) {
        finishTransfer(false, tr("The boat's mission has no waypoints to load; the plan is unchanged"));
        return false;
    }
    plan_ = pts;
    planSpeed_ = speed;
    planReturnHome_ = rtl;
    savePlan();                        // emits planChanged, compares with the items just read
    QString text = tr("Loaded from the boat: %1 points").arg(pts.size());
    text += speed > 0.0 ? tr(", speed %1 m/s").arg(speed, 0, 'f', 1) : tr(", the boat's own speed");
    text += rtl ? tr(", return home at the end") : QString();
    if (holds)
        text += tr("; waypoint hold times are not kept");
    if (!skipped.isEmpty())
        text += tr("; left out: %1").arg(skipped.join(QStringLiteral(", ")));
    finishTransfer(true, text);
    return true;
}

void MavVehicle::sendMissionItem(int seq)
{
    const Item& it = upItems_[seq];
    mavlink_message_t msg;
    mavlink_msg_mission_item_int_pack_chan(kGcsSystemId, kGcsComponentId, kTxChan, &msg, uint8_t(vehicleSysId_),
                                           uint8_t(vehicleCompId_), uint16_t(seq), it.frame, it.command, 0, 1,
                                           it.p1, it.p2, it.p3, it.p4, it.x, it.y, it.z, MAV_MISSION_TYPE_MISSION);
    sendMessage(&msg);
}

void MavVehicle::startDownload(bool afterUpload)
{
    verifyAfterUpload_ = afterUpload;
    downItems_.clear();
    downCount_ = -1;
    downNext_ = 0;
    transferRetries_ = 0;
    setTransfer(TransferDownloading, afterUpload ? 90 : 0,
                afterUpload ? tr("Checking the mission on the boat") : tr("Reading the mission from the boat"));
    mavlink_message_t msg;
    mavlink_msg_mission_request_list_pack_chan(kGcsSystemId, kGcsComponentId, kTxChan, &msg, uint8_t(vehicleSysId_),
                                               uint8_t(vehicleCompId_), MAV_MISSION_TYPE_MISSION);
    sendMessage(&msg);
    transferTimer_.start();
}

void MavVehicle::requestDownloadItem(int seq)
{
    mavlink_message_t msg;
    mavlink_msg_mission_request_int_pack_chan(kGcsSystemId, kGcsComponentId, kTxChan, &msg, uint8_t(vehicleSysId_),
                                              uint8_t(vehicleCompId_), uint16_t(seq), MAV_MISSION_TYPE_MISSION);
    sendMessage(&msg);
    transferTimer_.start();
}

void MavVehicle::onTransferTimeout()
{
    if (transferState_ != TransferUploading && transferState_ != TransferDownloading)
        return;
    if (++transferRetries_ > kTransferRetries) {
        finishTransfer(false, transferState_ == TransferUploading ? tr("Upload failed: no answer from the boat")
                                                                  : tr("Reading failed: no answer from the boat"));
        return;
    }
    if (transferState_ == TransferUploading) {
        // The last item went out and no MISSION_ACK came: over a lossy link it may be the ACK that was lost, and
        // the autopilot has already stored the mission and left the upload (a lost item it re-requests itself,
        // and an item re-sent outside the upload it may answer with an error). The read-back says which.
        if (uploadRequestSeen_ && lastRequestedSeq_ == int(upItems_.size()) - 1) {
            addMessage(tr("No answer after the last item: reading the mission back to check"));
            startDownload(true);
            return;
        }
        if (!uploadRequestSeen_) {
            mavlink_message_t msg;
            mavlink_msg_mission_count_pack_chan(kGcsSystemId, kGcsComponentId, kTxChan, &msg, uint8_t(vehicleSysId_),
                                                uint8_t(vehicleCompId_), uint16_t(upItems_.size()), MAV_MISSION_TYPE_MISSION, 0);
            sendMessage(&msg);
        } else if (lastRequestedSeq_ >= 0 && lastRequestedSeq_ < upItems_.size()) {
            sendMissionItem(lastRequestedSeq_);
        }
        transferTimer_.start();
    } else if (downCount_ < 0) {
        mavlink_message_t msg;
        mavlink_msg_mission_request_list_pack_chan(kGcsSystemId, kGcsComponentId, kTxChan, &msg, uint8_t(vehicleSysId_),
                                                   uint8_t(vehicleCompId_), MAV_MISSION_TYPE_MISSION);
        sendMessage(&msg);
        transferTimer_.start();
    } else {
        requestDownloadItem(downNext_);
    }
}

// ---- commands --------------------------------------------------------------------------------------

MavVehicle::Step MavVehicle::stepSetMode(int mode, const QString& what)
{
    Step s;
    s.what = what;
    s.ackCommand = MAV_CMD_DO_SET_MODE;
    s.send = [this, mode]() { sendCommandLong(MAV_CMD_DO_SET_MODE, MAV_MODE_FLAG_CUSTOM_MODE_ENABLED, float(mode)); };
    s.done = [this, mode]() { return mode_ == mode; };
    return s;
}

MavVehicle::Step MavVehicle::stepArm(bool on)
{
    Step s;
    s.what = on ? tr("Arming") : tr("Disarming");
    s.ackCommand = MAV_CMD_COMPONENT_ARM_DISARM;
    s.send = [this, on]() { sendCommandLong(MAV_CMD_COMPONENT_ARM_DISARM, on ? 1.0f : 0.0f); };
    s.done = [this, on]() { return armed_ == on; };
    return s;
}

void MavVehicle::runSteps(const QList<Step>& steps)
{
    if (steps.isEmpty())
        return;
    steps_ = steps;
    emit busyChanged();
    startStep();
}

void MavVehicle::startStep()
{
    while (!steps_.isEmpty() && steps_.first().done && steps_.first().done())
        steps_.removeFirst();                          // already true: nothing to send
    if (steps_.isEmpty()) {
        finishSteps(true, QString());
        return;
    }
    stepTry_ = 1;
    steps_.first().send();
    stepTimer_.start(steps_.first().timeoutMs);
    emit busyChanged();
}

void MavVehicle::onStepTimeout()
{
    if (steps_.isEmpty())
        return;
    Step& s = steps_.first();
    if (s.done && s.done()) {
        steps_.removeFirst();
        if (steps_.isEmpty())
            finishSteps(true, QString());
        else
            startStep();
        return;
    }
    if (stepTry_ >= s.tries) {
        finishSteps(false, tr("%1: no answer from the boat").arg(s.what));
        return;
    }
    ++stepTry_;
    s.send();
    stepTimer_.start(s.timeoutMs);
}

void MavVehicle::finishSteps(bool ok, const QString& detail)
{
    stepTimer_.stop();
    const bool hadSteps = !steps_.isEmpty() || ok;
    steps_.clear();
    std::function<void()> onFail;
    onFail.swap(onSequenceFail_);      // it belongs to this sequence only
    emit busyChanged();
    if (hadSteps) {
        addMessage(ok ? tr("%1: done").arg(sequenceWhat_) : tr("%1: %2").arg(sequenceWhat_, detail));
        emit commandResult(sequenceWhat_, ok, detail);
    }
    if (!ok && onFail)
        onFail();
}

void MavVehicle::interruptSequence()
{
    onSequenceFail_ = nullptr;         // the user's HOLD / RTL / disarm replaces the start: no disarm of our own
    if (busy())
        finishSteps(false, tr("interrupted"));
}

// ---- navigation readiness --------------------------------------------------------------------------

void MavVehicle::logSettings()
{
    settingsLogged_ = true;
    const QString groupName[] = { tr("EKF"), tr("GPS") };
    for (int group = 0; group < 2; ++group) {
        QStringList parts;
        QStringList missing;
        for (const Setting& x : kSettingsToRead) {
            if (x.group != group)
                continue;
            const QString key = QLatin1String(x.name);
            if (settings_.contains(key))
                parts << QStringLiteral("%1 %2").arg(key).arg(settings_.value(key), 0, 'g', 6);
            else
                missing << key;
        }
        addMessage(tr("Autopilot settings (%1): %2%3")
                       .arg(groupName[group], parts.join(QStringLiteral(", ")),
                            missing.isEmpty() ? QString() : tr(" (no answer for %1)").arg(missing.join(QStringLiteral(", ")))));
    }
}

void MavVehicle::updateGpsHeading()
{
    // Best of the two receivers: a heading beats "configured but none now", which beats "not configured".
    int state = GpsHeadingUnknown;
    int which = -1;
    for (int i = 0; i < 2; ++i) {
        const int yaw = gpsYaw_[i];
        const int st = yaw < 0 ? GpsHeadingUnknown
                     : yaw == 0 ? GpsHeadingNotConfigured
                     : yaw == 65535 ? GpsHeadingUnavailable
                                    : GpsHeadingOk;
        if (st > state) {
            state = st;
            which = i;
        }
    }
    QString text;
    switch (state) {
    case GpsHeadingOk:
        text = tr("%1° from GPS %2").arg((gpsYaw_[which] % 36000) / 100.0, 0, 'f', 1).arg(which + 1);
        break;
    case GpsHeadingUnavailable:
        text = tr("configured on GPS %1, none now").arg(which + 1);
        break;
    case GpsHeadingNotConfigured:
        text = tr("no GPS is set up to give a heading");
        break;
    default:
        text = tr("no GPS report yet");
        break;
    }
    if (gps2Fix_ >= 0)
        text += tr(" (GPS 2: %1, %2 sat)").arg(gpsFixText(gps2Fix_)).arg(gps2Sats_);
    const bool stateChanged = state != gpsHeadingState_;
    if (stateChanged || text != gpsHeadingText_) {
        gpsHeadingState_ = state;
        gpsHeadingText_ = text;
        if (state == GpsHeadingOk)
            yawFallbackActive_ = false;       // "reset to GPS yaw if it becomes available" (SelectMagFusion)
        emit gpsChanged();
        if (stateChanged)
            emit headingInUseChanged();
    }
    if (stateChanged && state != GpsHeadingUnknown) {
        ++gpsHeadingChanges_;
        gpsHeadingLogTimer_.start();   // logged once it has held for kSettleLogMs
    }
}

void MavVehicle::logGpsHeading()
{
    const int changes = gpsHeadingChanges_;
    gpsHeadingChanges_ = 0;
    if (gpsHeadingState_ == GpsHeadingUnknown || gpsHeadingState_ == gpsHeadingLogged_)
        return;
    gpsHeadingLogged_ = gpsHeadingState_;
    addMessage(tr("GPS heading: %1 (yaw fields: GPS 1 %2, GPS 2 %3)%4")
                   .arg(gpsHeadingText_).arg(gpsYaw_[0])
                   .arg(gpsYaw_[1] < 0 ? tr("no report") : QString::number(gpsYaw_[1]))
                   .arg(changes > 1 ? tr(" after %1 changes").arg(changes) : QString()));
}

void MavVehicle::logHealth()
{
    const int changes = healthChanges_;
    healthChanges_ = 0;
    if (healthLoggedOnce_ && healthText_ == healthLogged_)
        return;
    healthLoggedOnce_ = true;
    healthLogged_ = healthText_;
    const QString tail = changes > 1 ? tr(" (after %1 changes)").arg(changes) : QString();
    addMessage((healthText_.isEmpty() ? tr("Autopilot sensors: all healthy") : tr("Autopilot sensors %1").arg(healthText_)) + tail);
}

void MavVehicle::updateNav()
{
    // Rover 4.4 once armed: Mode::enter() needs ekf_position_ok() (horizontal position, absolute or relative,
    // not constant-position mode) and no EKF failsafe; ekf_over_threshold() fires that failsafe when the
    // velocity variance reaches FS_EKF_THRESH (it is counted twice) or when two of velocity, position and
    // compass do. The report gives the compass variance as its largest axis where Rover takes the vector
    // length, so the compass part here is a little more lenient than Rover's.
    const bool fresh = ekfFlags_ >= 0 && ekfSeen_.isValid() && ekfSeen_.elapsed() < kEkfStaleMs;
    int reason;
    QString text;
    if (!fresh) {
        reason = NavReasonNoReport;
        text = tr("no EKF status from the boat");
    } else if ((ekfFlags_ & EKF_UNINITIALIZED) || !(ekfFlags_ & (EKF_POS_HORIZ_ABS | EKF_POS_HORIZ_REL))
               || (ekfFlags_ & EKF_CONST_POS_MODE)) {
        // One reason and one text for all of these: with two EKF lanes taking turns as primary the report
        // alternates between them (seen on the boat: 0x0000 / 0x0400 every few seconds).
        reason = NavReasonNoPosition;
        text = (ekfFlags_ & EKF_CONST_POS_MODE) ? tr("no position: the EKF is not using GPS (constant position mode)")
                                                : tr("no position estimate in the EKF");
    } else if (ekfThresh_ > 0 && (ekfVelVar_ >= ekfThresh_ || (ekfPosVar_ >= ekfThresh_ && ekfMagVar_ >= ekfThresh_))) {
        reason = NavReasonVariance;
        text = tr("EKF variances over %1: velocity %2, position %3, compass %4")
                   .arg(ekfThresh_, 0, 'f', 2).arg(ekfVelVar_, 0, 'f', 2).arg(ekfPosVar_, 0, 'f', 2).arg(ekfMagVar_, 0, 'f', 2);
    } else {
        reason = NavReasonReady;
        text = tr("ready");
    }
    const int state = reason == NavReasonReady ? NavReady : reason == NavReasonNoReport ? NavUnknown : NavNotReady;
    if (state != navState_ || text != navText_) {
        navState_ = state;
        navText_ = text;
        emit navChanged();
    }
    if (reason == navLoggedReason_)
        return;
    if (state == navLoggedState_ && navLoggedAt_.isValid() && navLoggedAt_.elapsed() < kNavRelogMs)
        return;      // same state, another reason: not worth a line every few seconds
    const bool first = navLoggedReason_ < 0;
    navLoggedReason_ = reason;
    navLoggedState_ = state;
    navLoggedAt_.start();
    if (reason == NavReasonNoReport) {
        if (!first)
            addMessage(tr("Navigation: %1").arg(text));
        return;
    }
    const QString head = state == NavReady ? tr("Navigation ready") : tr("Navigation not ready: %1").arg(text);
    addMessage(tr("%1 (EKF flags 0x%2, variances: velocity %3, position %4, compass %5; GPS %6, %7 sat, HDOP %8)")
                   .arg(head)
                   .arg(ekfFlags_, 4, 16, QLatin1Char('0'))
                   .arg(ekfVelVar_, 0, 'f', 2).arg(ekfPosVar_, 0, 'f', 2).arg(ekfMagVar_, 0, 'f', 2)
                   .arg(gpsFixText(gpsFix_)).arg(gpsSats_).arg(gpsHdop_, 0, 'f', 1));
}

QString MavVehicle::startBlocker() const
{
    if (!connected())
        return tr("No link to the boat");
    if (plan_.isEmpty())
        return tr("The plan has no points");
    if (transferState_ == TransferUploading || transferState_ == TransferDownloading)
        return tr("The mission is being transferred");
    if (!planOnVehicle_)
        return tr("Upload the mission to the boat first");
    if (gpsFix_ < 3)
        return tr("No GPS fix on the boat");
    if (navState_ == NavNotReady)
        return tr("The boat's navigation is not ready: %1").arg(navText_);
    if (!homeValid_)
        return tr("The boat has no home position yet");
    if (busy())
        return tr("Another command is running");
    return QString();
}

void MavVehicle::startMission()
{
    if (refusedForPlanner("startMission"))
        return;
    // The name is set only once the start really runs: a refusal must not rename a sequence still running.
    const QString why = startBlocker();
    if (!why.isEmpty()) {
        emit commandResult(tr("Start mission"), false, why);
        return;
    }
    sequenceWhat_ = tr("Start mission");
    const int firstNav = planSpeed_ > 0.0 ? 2 : 1;
    QList<Step> s;
    Step current;
    current.what = tr("Selecting the first point");
    current.send = [this]() {
        missionCurrentFresh_ = false;
        mavlink_message_t msg;
        mavlink_msg_mission_set_current_pack_chan(kGcsSystemId, kGcsComponentId, kTxChan, &msg, uint8_t(vehicleSysId_),
                                                  uint8_t(vehicleCompId_), 1);
        sendMessage(&msg);
    };
    current.done = [this, firstNav]() { return missionCurrentFresh_ && (missionCurrent_ == 1 || missionCurrent_ == firstNav); };
    s.append(current);
    // Through HOLD unless the boat is in HOLD already. ArduPilot starts a mission only when AUTO is entered: a boat in
    // AUTO whose mission is complete (a plan without the return home) did not run it again (field test 2026-09-28,
    // reproduced on SITL); and Rover does not arm in RTL, where a plan with the return home ends.
    if (mode_ != ModeHold)
        s.append(stepSetMode(ModeHold, tr("Switching to HOLD")));
    if (!armed_) {
        s.append(stepArm(true));
        // Armed by this start: if it fails from here on (Rover refuses AUTO without a position estimate once
        // armed, or no answer), the boat must not stay armed in HOLD.
        onSequenceFail_ = [this]() {
            if (armed_ && mode_ != ModeAuto) {
                addMessage(tr("The start failed after arming (navigation: %1): disarming").arg(navText_));
                sequenceWhat_ = tr("Disarm after the failed start");
                runSteps({ stepArm(false) });
            }
        };
    }
    s.append(stepSetMode(ModeAuto, tr("Switching to AUTO")));
    runSteps(s);
}

void MavVehicle::hold()
{
    if (refusedForPlanner("hold"))
        return;
    // HOLD, RTL and disarm pre-empt whatever else is running: they are the ways to stop the boat.
    interruptSequence();
    sequenceWhat_ = tr("Hold");
    runSteps({ stepSetMode(ModeHold, tr("Switching to HOLD")) });
}

void MavVehicle::returnHome()
{
    if (refusedForPlanner("returnHome"))
        return;
    interruptSequence();
    sequenceWhat_ = tr("Return home");
    runSteps({ stepSetMode(ModeRtl, tr("Switching to RTL")) });
}

void MavVehicle::arm()
{
    if (refusedForPlanner("arm"))
        return;
    if (busy()) {
        emit commandResult(tr("Arm"), false, tr("Another command is running"));
        return;
    }
    sequenceWhat_ = tr("Arm");
    runSteps({ stepArm(true) });
}

int MavVehicle::headingSource() const
{
    const auto it = settings_.constFind(QStringLiteral("EK3_SRC1_YAW"));
    return it == settings_.constEnd() ? -1 : int(*it);
}

int MavVehicle::headingInUse() const
{
    const int src = headingSource();
    switch (src) {
    case HeadingCompass:
        return InUseCompass;
    case HeadingGps:
        return gpsHeadingState_ == GpsHeadingOk ? InUseGps : InUseNone;
    case HeadingGpsCompassFallback:
        if (gpsHeadingState_ == GpsHeadingOk)
            return InUseGps;
        return yawFallbackActive_ ? InUseCompassFallback : InUseNone;
    }
    return src < 0 ? InUseUnknown : InUseOther;
}

QString MavVehicle::headingInUseText() const
{
    switch (headingInUse()) {
    case InUseGps: return tr("GPS, two antennas");
    case InUseCompass: return tr("compass");
    case InUseCompassFallback: return tr("compass, as the fallback (no GPS heading)");
    case InUseNone:
        return tr("none: no GPS heading (it needs RTK fixed), and the compass takes over only in motion, after the GPS "
                  "heading has worked");
    case InUseOther: return headingSourceName(headingSource());
    }
    return tr("not known yet");
}

QString MavVehicle::headingSourceName(int source)
{
    switch (source) {
    case HeadingNone: return tr("none");
    case HeadingCompass: return tr("compass");
    case HeadingGps: return tr("GPS, two antennas");
    case HeadingGpsCompassFallback: return tr("GPS, two antennas, compass fallback");
    case HeadingExtNav: return tr("external navigation");
    case HeadingGsf: return tr("GPS track (GSF)");
    }
    return source < 0 ? tr("not read yet") : tr("source %1").arg(source);
}

void MavVehicle::setHeadingSource(int source)
{
    const QString what = tr("Heading source");
    QString why;
    if (source != HeadingCompass && source != HeadingGpsCompassFallback)
        why = tr("not offered here: %1").arg(source);
    else if (!connected())
        why = tr("No link to the boat");
    else if (armed_)
        why = tr("Disarm the boat first");         // a yaw reset under way would turn the boat
    else if (busy())
        why = tr("Another command is running");
    else if (headingSource() < 0)
        why = tr("The boat's settings are not read yet");
    if (!why.isEmpty()) {
        emit commandResult(what, false, why);
        return;
    }
    if (headingSource() == source) {
        emit commandResult(what, true, QString());
        return;
    }
    sequenceWhat_ = what;
    const int type = settingTypes_.value(QStringLiteral("EK3_SRC1_YAW"), MAV_PARAM_TYPE_INT8);
    Step s;
    s.what = tr("Setting the heading source: %1").arg(headingSourceName(source));
    s.send = [this, source, type]() { sendParamSet("EK3_SRC1_YAW", float(source), type); };
    s.done = [this, source]() { return headingSource() == source; };
    runSteps({ s });
}

void MavVehicle::disarm()
{
    if (refusedForPlanner("disarm"))
        return;
    interruptSequence();
    sequenceWhat_ = tr("Disarm");
    runSteps({ stepArm(false) });
}

// ---- misc ------------------------------------------------------------------------------------------

void MavVehicle::addMessage(const QString& text)
{
    qInfo().noquote() << "boat:" << text;   // the kiosk's journal keeps the session for field diagnosis
    messages_.prepend(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")) + QStringLiteral("  ") + text);
    while (messages_.size() > kMaxMessages)
        messages_.removeLast();
    emit messagesChanged();
}

void MavVehicle::loadSettings()
{
    QSettings s;
    s.beginGroup(kSettingsGroup);
    controlEnabled_ = s.value(QStringLiteral("controlEnabled"), true).toBool();
    planSpeed_ = s.value(QStringLiteral("speed"), 0.0).toDouble();
    planReturnHome_ = s.value(QStringLiteral("returnHome"), true).toBool();
    linkTrackShown_ = s.value(QStringLiteral("linkTrackShown"), true).toBool();
    const QJsonArray arr = QJsonDocument::fromJson(s.value(QStringLiteral("planJson")).toByteArray()).array();
    plan_.clear();
    for (const QJsonValue& v : arr) {
        const QJsonArray p = v.toArray();
        if (p.size() >= 2)
            plan_.append(Point{p.at(0).toDouble(), p.at(1).toDouble()});
    }
}

void MavVehicle::savePlan()
{
    if (persist_) {
        QJsonArray arr;
        for (const Point& p : plan_)
            arr.append(QJsonArray{p.lat, p.lon});
        QSettings s;
        s.beginGroup(kSettingsGroup);
        s.setValue(QStringLiteral("planJson"), QJsonDocument(arr).toJson(QJsonDocument::Compact));
        s.setValue(QStringLiteral("speed"), planSpeed_);
        s.setValue(QStringLiteral("returnHome"), planReturnHome_);
    }
    emit planChanged();
    updatePlanOnVehicle();
}
