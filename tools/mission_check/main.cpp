// mission_check - drives MavVehicle (the app's MAVLink vehicle class) through a whole boat mission against
// ArduPilot SITL over TCP and reports PASS/FAIL per step. Exit code 0 = every check passed.
//
//   mission_check [--host 127.0.0.1] [--port 5760] [--wall-timeout-scale 1.0]
//
// SITL (ArduRover, model motorboat) must already listen on the port; see docs/BOAT_MISSION.md.
// Before SITL it checks the navigation-readiness rules on made-up EKF reports (no SITL needed).

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <functional>

#include "mav_vehicle.h"

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

int g_failed = 0;
int g_checks = 0;
double g_scale = 1.0;

bool check(const char* name, bool ok, const QString& detail = QString())
{
    ++g_checks;
    if (!ok)
        ++g_failed;
    std::printf("%s  %s%s\n", ok ? "PASS" : "FAIL", name,
                detail.isEmpty() ? "" : qPrintable(QStringLiteral("  -- ") + detail));
    std::fflush(stdout);
    return ok;
}

bool waitFor(const std::function<bool()>& pred, int ms)
{
    QElapsedTimer t;
    t.start();
    const qint64 limit = qint64(ms * g_scale);
    while (t.elapsed() < limit) {
        if (pred())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return pred();
}

// Frames as if the autopilot (system 1, component 1, as SITL and the boat) had sent them.
constexpr uint8_t kFakeChan = MAVLINK_COMM_2;

QByteArray frame(const mavlink_message_t& m)
{
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    const uint16_t n = mavlink_msg_to_send_buffer(buf, &m);
    return QByteArray(reinterpret_cast<const char*>(buf), int(n));
}

QByteArray fakeHeartbeat(bool armed, int mode)
{
    mavlink_message_t m;
    mavlink_msg_heartbeat_pack_chan(1, 1, kFakeChan, &m, MAV_TYPE_SURFACE_BOAT, MAV_AUTOPILOT_ARDUPILOTMEGA,
                                    MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | (armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0),
                                    uint32_t(mode), MAV_STATE_ACTIVE);
    return frame(m);
}

QByteArray fakeGps(int fix, int sats)
{
    mavlink_message_t m;
    mavlink_msg_gps_raw_int_pack_chan(1, 1, kFakeChan, &m, 0, uint8_t(fix), 402000000, 445360000, 1300000, 90, 120, 0, 0,
                                      uint8_t(sats), 0, 0, 0, 0, 0, 0);
    return frame(m);
}

QByteArray fakeEkf(uint16_t flags, float vel, float pos, float mag)
{
    mavlink_message_t m;
    mavlink_msg_ekf_status_report_pack_chan(1, 1, kFakeChan, &m, flags, vel, pos, 0.1f, mag, 0.0f, 0.0f);
    return frame(m);
}

QByteArray fakeAck(uint16_t command, uint8_t result)
{
    mavlink_message_t m;
    mavlink_msg_command_ack_pack_chan(1, 1, kFakeChan, &m, command, result, 0, 0, MavVehicle::kGcsSystemId,
                                      MavVehicle::kGcsComponentId);
    return frame(m);
}

// One outgoing frame of MavVehicle (it sends exactly one per sendBytes): is it this COMMAND_LONG?
bool isCommand(const QByteArray& bytes, uint16_t command)
{
    mavlink_message_t m;
    mavlink_status_t st;
    for (char c : bytes) {
        if (mavlink_parse_char(MAVLINK_COMM_3, uint8_t(c), &m, &st) && m.msgid == MAVLINK_MSG_ID_COMMAND_LONG)
            return mavlink_msg_command_long_get_command(&m) == command;
    }
    return false;
}

constexpr uint16_t kEkfBase = EKF_ATTITUDE | EKF_VELOCITY_HORIZ | EKF_VELOCITY_VERT | EKF_POS_VERT_ABS;

void navRules()
{
    MavVehicle u(nullptr, false);
    u.feed(fakeHeartbeat(false, MavVehicle::ModeHold));
    u.feed(fakeGps(3, 12));
    check("nav: no EKF report yet -> unknown", u.navState() == MavVehicle::NavUnknown, u.navText());

    struct Case { const char* name; uint16_t flags; float vel, pos, mag; int want; };
    const Case cases[] = {
        { "nav: EKF uninitialised -> not ready", EKF_UNINITIALIZED, 0.1f, 0.1f, 0.1f, MavVehicle::NavNotReady },
        { "nav: constant position mode -> not ready", uint16_t(kEkfBase | EKF_CONST_POS_MODE | EKF_PRED_POS_HORIZ_ABS),
          0.1f, 0.1f, 0.1f, MavVehicle::NavNotReady },
        { "nav: predicted position only (enough disarmed, not armed) -> not ready",
          uint16_t(kEkfBase | EKF_PRED_POS_HORIZ_ABS), 0.1f, 0.1f, 0.1f, MavVehicle::NavNotReady },
        { "nav: absolute position, low variances -> ready", uint16_t(kEkfBase | EKF_POS_HORIZ_ABS | EKF_POS_HORIZ_REL),
          0.1f, 0.1f, 0.1f, MavVehicle::NavReady },
        { "nav: relative position only -> ready (Rover accepts it)", uint16_t(kEkfBase | EKF_POS_HORIZ_REL),
          0.1f, 0.1f, 0.1f, MavVehicle::NavReady },
        { "nav: velocity variance at FS_EKF_THRESH -> not ready", uint16_t(kEkfBase | EKF_POS_HORIZ_ABS),
          0.8f, 0.1f, 0.1f, MavVehicle::NavNotReady },
        { "nav: position and compass variances over -> not ready", uint16_t(kEkfBase | EKF_POS_HORIZ_ABS),
          0.1f, 0.9f, 0.9f, MavVehicle::NavNotReady },
        { "nav: position variance alone over -> ready", uint16_t(kEkfBase | EKF_POS_HORIZ_ABS),
          0.1f, 0.9f, 0.1f, MavVehicle::NavReady },
    };
    for (const Case& c : cases) {
        u.feed(fakeEkf(c.flags, c.vel, c.pos, c.mag));
        check(c.name, u.navState() == c.want, u.navText());
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCommandLineParser cli;
    cli.addHelpOption();
    cli.addOption({ QStringLiteral("host"), QStringLiteral("SITL host"), QStringLiteral("host"), QStringLiteral("127.0.0.1") });
    cli.addOption({ QStringLiteral("port"), QStringLiteral("SITL TCP port"), QStringLiteral("port"), QStringLiteral("5760") });
    cli.addOption({ QStringLiteral("wall-timeout-scale"), QStringLiteral("multiply every wait by this"), QStringLiteral("k"),
                    QStringLiteral("1.0") });
    cli.process(app);
    g_scale = qMax(0.1, cli.value(QStringLiteral("wall-timeout-scale")).toDouble());

    navRules();

    QTcpSocket sock;
    MavVehicle v(nullptr, false);
    // refuseAuto: the transport keeps DO_SET_MODE from SITL and answers it with COMMAND_ACK FAILED, which is
    // what the real boat did when its EKF had no position ("Flight mode change failed").
    bool refuseAuto = false;
    QObject::connect(&sock, &QTcpSocket::readyRead, &v, [&]() { v.feed(sock.readAll()); });
    QObject::connect(&v, &MavVehicle::sendBytes, &sock, [&](const QByteArray& b) {
        if (refuseAuto && isCommand(b, MAV_CMD_DO_SET_MODE)) {
            QTimer::singleShot(50, &app, [&]() { v.feed(fakeAck(MAV_CMD_DO_SET_MODE, MAV_RESULT_FAILED)); });
            return;
        }
        sock.write(b);
    });
    QString lastResult;
    bool lastOk = false;
    int results = 0;
    QObject::connect(&v, &MavVehicle::commandResult, &app, [&](const QString& what, bool ok, const QString& detail) {
        lastResult = what + QStringLiteral(": ") + (ok ? QStringLiteral("ok") : detail);
        lastOk = ok;
        ++results;
        std::printf("      result  %s\n", qPrintable(lastResult));
    });
    QObject::connect(&v, &MavVehicle::messagesChanged, &app, [&]() {
        if (!v.messages().isEmpty())
            std::printf("      vehicle %s\n", qPrintable(v.messages().first()));
    });

    sock.connectToHost(cli.value(QStringLiteral("host")), quint16(cli.value(QStringLiteral("port")).toUInt()));
    if (!check("TCP connect", sock.waitForConnected(10000), sock.errorString()))
        return 2;

    check("autopilot heartbeat", waitFor([&] { return v.connected(); }, 30000), v.autopilotText());
    check("GPS fix, position and home", waitFor([&] { return v.gpsFix() >= 3 && v.positionValid() && v.homeValid(); }, 120000),
          QStringLiteral("fix %1, home %2").arg(v.gpsFix()).arg(v.homeValid()));
    check("navigation ready (EKF status from the autopilot)", waitFor([&] { return v.navState() == MavVehicle::NavReady; }, 60000),
          v.navText());
    check("FS_EKF_THRESH read from the autopilot", waitFor([&] { return v.ekfThresholdKnown(); }, 30000));
    // the connect-time read-back of the vehicle's mission has to finish before we transfer
    waitFor([&] { return v.transferState() != MavVehicle::TransferDownloading; }, 15000);

    const double lat0 = v.latitude(), lon0 = v.longitude();
    v.clearPlan();
    v.addPoint(lat0 + 0.0006, lon0);
    v.addPoint(lat0 + 0.0006, lon0 + 0.0008);
    v.addPoint(lat0, lon0 + 0.0008);
    v.setPlanSpeed(2.0);
    v.setPlanReturnHome(true);
    check("plan of 3 points", v.planCount() == 3, QStringLiteral("%1 m").arg(v.planLengthM(), 0, 'f', 0));
    check("start is blocked before upload", !v.startBlocker().isEmpty(), v.startBlocker());

    v.uploadPlan();
    check("upload + read-back", waitFor([&] { return v.transferState() == MavVehicle::TransferDone
                                                    || v.transferState() == MavVehicle::TransferFailed; }, 30000)
          && v.transferState() == MavVehicle::TransferDone && v.planOnVehicle(), v.transferText());

    v.movePoint(1, lat0 + 0.0007, lon0 + 0.0008);
    check("editing the plan clears planOnVehicle", !v.planOnVehicle());
    v.movePoint(1, lat0 + 0.0006, lon0 + 0.0008);
    check("restoring it sets planOnVehicle again", v.planOnVehicle());

    v.readBack();
    check("separate read-back matches", waitFor([&] { return v.transferState() == MavVehicle::TransferDone
                                                            || v.transferState() == MavVehicle::TransferFailed; }, 30000)
          && v.planOnVehicle(), v.transferText());

    // An EKF without a position (a made-up report between two real ones): no start, and nothing armed.
    v.feed(fakeEkf(kEkfBase | EKF_CONST_POS_MODE, 0.1f, 0.1f, 0.1f));
    check("start blocked while the navigation is not ready", v.startBlocker().contains(QStringLiteral("navigation")),
          v.startBlocker());
    int before = results;
    v.startMission();
    check("start refused, boat not armed", results > before && !lastOk && !v.armed(), lastResult);
    check("navigation ready again from the autopilot's reports",
          waitFor([&] { return v.navState() == MavVehicle::NavReady; }, 15000), v.navText());

    // The boat refuses AUTO after the start armed it: the start has to disarm it again.
    refuseAuto = true;
    before = results;
    v.startMission();
    const bool reported = waitFor([&] { return results > before; }, 20000);
    const QString startResult = lastResult;
    check("start with AUTO refused: reported as failed", reported && !startResult.endsWith(QStringLiteral(": ok")), startResult);
    check("start with AUTO refused: the boat is disarmed again",
          waitFor([&] { return results > before + 1 && !v.busy(); }, 20000) && lastOk && !v.armed()
              && lastResult.startsWith(QStringLiteral("Disarm after the failed start")),
          lastResult);
    refuseAuto = false;

    check("start allowed", v.startBlocker().isEmpty(), v.startBlocker());
    before = results;
    v.startMission();
    check("start mission: armed and AUTO", waitFor([&] { return results > before; }, 20000) && lastOk && v.armed()
          && v.mode() == MavVehicle::ModeAuto, lastResult);
    check("heading for the first point", waitFor([&] { return v.currentPlanIndex() == 0; }, 10000),
          QStringLiteral("mission item %1").arg(v.missionCurrent()));
    check("first point reached, heading for the second", waitFor([&] { return v.currentPlanIndex() == 1; }, 180000),
          QStringLiteral("mission item %1, reached %2").arg(v.missionCurrent()).arg(v.missionReached()));

    before = results;
    v.hold();
    check("HOLD", waitFor([&] { return results > before; }, 10000) && lastOk && v.mode() == MavVehicle::ModeHold, lastResult);

    before = results;
    v.returnHome();
    check("RTL", waitFor([&] { return results > before; }, 10000) && lastOk && v.mode() == MavVehicle::ModeRtl, lastResult);
    check("back home", waitFor([&] {
              return MavVehicle::distanceM(v.latitude(), v.longitude(), v.homeLatitude(), v.homeLongitude()) < 5.0; }, 180000),
          QStringLiteral("%1 m from home").arg(MavVehicle::distanceM(v.latitude(), v.longitude(), v.homeLatitude(),
                                                                      v.homeLongitude()), 0, 'f', 1));

    before = results;
    v.disarm();
    check("disarm", waitFor([&] { return results > before; }, 10000) && lastOk && !v.armed(), lastResult);

    std::printf("\n%d checks, %d failed\nMISSION_CHECK_%s\n", g_checks, g_failed, g_failed ? "FAILED" : "DONE");
    return g_failed ? 1 : 0;
}
