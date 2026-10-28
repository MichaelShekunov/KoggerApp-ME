// mission_check - drives MavVehicle (the app's MAVLink vehicle class) through a whole boat mission against
// ArduPilot SITL over TCP and reports PASS/FAIL per step. Exit code 0 = every check passed.
//
//   mission_check [--host 127.0.0.1] [--port 5760] [--wall-timeout-scale 1.0] [--offline]
//
// SITL (ArduRover, model motorboat) must already listen on the port; see docs/BOAT_MISSION.md.
// Before SITL it checks the navigation-readiness rules on made-up EKF reports, the Wi-Fi module's
// sequences and port-page decoding on made-up frames, and the device-liveness and on-screen-keyboard
// rules on made-up times and touches (no SITL needed); --offline stops after those.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <functional>

#include "device_liveness.h"
#include "mav_vehicle.h"
#include "touch_tap_gate.h"
#include "wifi_ops.h"
#include "traffic_tally.h"
#include "mission_progress.h"
#include "link_track.h"
#include "air_meter.h"
#include "system_stats.h"
#include "system_monitor.h"
#include "link/link_ask_all.h"
#include "link/link_rate_search.h"

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

QByteArray fakeGps(int fix, int sats, uint16_t yaw = 0)
{
    mavlink_message_t m;
    mavlink_msg_gps_raw_int_pack_chan(1, 1, kFakeChan, &m, 0, uint8_t(fix), 402000000, 445360000, 1300000, 90, 120, 0, 0,
                                      uint8_t(sats), 0, 0, 0, 0, 0, yaw);
    return frame(m);
}

QByteArray fakeGps2(int fix, int sats, uint16_t yaw)
{
    mavlink_message_t m;
    mavlink_msg_gps2_raw_pack_chan(1, 1, kFakeChan, &m, 0, uint8_t(fix), 402000000, 445360000, 1300000, 90, 120, 0, 0,
                                   uint8_t(sats), 0, 0, yaw, 0, 0, 0, 0, 0);
    return frame(m);
}

QByteArray fakeEkf(uint16_t flags, float vel, float pos, float mag)
{
    mavlink_message_t m;
    mavlink_msg_ekf_status_report_pack_chan(1, 1, kFakeChan, &m, flags, vel, pos, 0.1f, mag, 0.0f, 0.0f);
    return frame(m);
}

QByteArray fakeSysStatus(uint32_t present, uint32_t health)
{
    mavlink_message_t m;
    mavlink_msg_sys_status_pack_chan(1, 1, kFakeChan, &m, present, present, health, 0, 12000, -1, 80, 0, 0, 0, 0, 0, 0, 0, 0, 0);
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

// DO_SET_MODE to this custom mode (param2).
bool isSetMode(const QByteArray& bytes, int mode)
{
    mavlink_message_t m;
    mavlink_status_t st;
    for (char c : bytes) {
        if (mavlink_parse_char(MAVLINK_COMM_3, uint8_t(c), &m, &st) && m.msgid == MAVLINK_MSG_ID_COMMAND_LONG)
            return mavlink_msg_command_long_get_command(&m) == MAV_CMD_DO_SET_MODE
                   && int(mavlink_msg_command_long_get_param2(&m)) == mode;
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

    // The boat's report alternated between two lanes (0x0000 / 0x0400): one text, no new log line each time.
    u.feed(fakeEkf(0, 0, 0, 0));
    const QString t0 = u.navText();
    const int n0 = u.messages().size();
    for (int i = 0; i < 6; ++i)
        u.feed(fakeEkf(i % 2 ? EKF_UNINITIALIZED : 0, 0, 0.05f * (i % 2), 0));
    check("nav: alternating lanes -> same text, no extra log lines", u.navText() == t0 && u.messages().size() == n0,
          QStringLiteral("%1 new lines").arg(u.messages().size() - n0));

    // GPS heading from the yaw fields (ArduPilot: 0 not configured, 65535 configured but none, else cdeg).
    u.feed(fakeGps(3, 12, 0));
    check("heading: yaw 0 -> not configured", u.gpsHeadingState() == MavVehicle::GpsHeadingNotConfigured, u.gpsHeadingText());
    u.feed(fakeGps2(5, 20, 65535));
    check("heading: GPS 2 yaw 65535 -> configured, none now", u.gpsHeadingState() == MavVehicle::GpsHeadingUnavailable
          && u.gpsHeadingText().contains(QStringLiteral("GPS 2")), u.gpsHeadingText());
    u.feed(fakeGps2(6, 20, 12345));
    check("heading: GPS 2 yaw 12345 -> 123.5 degrees", u.gpsHeadingState() == MavVehicle::GpsHeadingOk
          && u.gpsHeadingText().startsWith(QStringLiteral("123.5")), u.gpsHeadingText());

    const uint32_t present = MAV_SYS_STATUS_SENSOR_3D_GYRO | MAV_SYS_STATUS_SENSOR_3D_ACCEL | MAV_SYS_STATUS_SENSOR_3D_MAG
                           | MAV_SYS_STATUS_SENSOR_GPS | MAV_SYS_STATUS_AHRS;
    u.feed(fakeSysStatus(present, present));
    check("health: all healthy -> empty", u.healthText().isEmpty(), u.healthText());
    u.feed(fakeSysStatus(present, present & ~uint32_t(MAV_SYS_STATUS_SENSOR_3D_MAG | MAV_SYS_STATUS_AHRS)));
    check("health: compass and AHRS unhealthy -> named", u.healthText().contains(QStringLiteral("compass"))
          && u.healthText().contains(QStringLiteral("AHRS")), u.healthText());
}

// A refused start must not rename the command still running; flapping heading states give one log line.
void sequenceAndLogRules()
{
    MavVehicle s(nullptr, false);
    QString lastWhat;
    QObject::connect(&s, &MavVehicle::commandResult, [&](const QString& what, bool, const QString&) { lastWhat = what; });
    s.feed(fakeHeartbeat(false, MavVehicle::ModeHold));
    s.returnHome();                                   // waits for the autopilot to report RTL
    s.startMission();                                 // refused: no points, and a command is running
    check("sequence: start refused while a command runs", lastWhat == QStringLiteral("Start mission") && s.busy(), lastWhat);
    s.feed(fakeHeartbeat(false, MavVehicle::ModeRtl));
    check("sequence: the running command finishes under its own name", lastWhat == QStringLiteral("Return home") && !s.busy(),
          lastWhat);

    // Only the heading lines count: with no more heartbeats this vehicle also logs its link as lost.
    MavVehicle h(nullptr, false);
    h.feed(fakeHeartbeat(false, MavVehicle::ModeHold));
    auto headingLines = [&h]() {
        QStringList lines;
        for (const QString& m : h.messages())
            if (m.contains(QStringLiteral("GPS heading")))
                lines << m;
        return lines;
    };
    for (int i = 0; i < 6; ++i)
        h.feed(fakeGps2(i % 2 ? 6 : 5, 20, uint16_t(i % 2 ? 12345 : 65535)));
    check("heading log: flips are not logged one by one", headingLines().isEmpty(), headingLines().join(QStringLiteral(" | ")));
    waitFor([&] { return !headingLines().isEmpty(); }, 5000);
    check("heading log: one line once it holds, with the count", headingLines().size() == 1
          && headingLines().value(0).contains(QStringLiteral("after")), headingLines().join(QStringLiteral(" | ")));
}

// The Wi-Fi module's port-speed change and radio check, driven by made-up events (the driver wires the same
// classes to ID_UART / ID_WIFI / ID_FLASH and the link manager).
void wifiOpsRules()
{
    QStringList calls;
    bool answer = true;
    WifiPortChange* port = nullptr;
    port = new WifiPortChange({
        [&](int b) { calls << QStringLiteral("module %1").arg(b); },
        [&](int b) { calls << QStringLiteral("link %1").arg(b); },
        [&]() {
            calls << QStringLiteral("ask");
            if (answer)
                QTimer::singleShot(20, [&]() { port->moduleAnswered(); });
        },
        [&]() { calls << QStringLiteral("save"); }
    }, WifiPortChange::Timing{ 50, 100, 5 });
    int done = 0;
    bool ok = false;
    QObject::connect(port, &WifiPortChange::finished, [&](bool r, int, int) { ++done; ok = r; });
    check("port: refuses a speed above 5000000", !port->start(6000000, 2000000));
    check("ports: v7 from firmware 0.12, not before, -1 while the version is not known",
          WifiNetRules::portsFor(0, 12) == 1 && WifiNetRules::portsFor(0, 13) == 1 && WifiNetRules::portsFor(0, 11) == 0
          && WifiNetRules::portsFor(0, 0) == -1 && WifiNetRules::portsFor(1, 0) == 1);
    check("rates: 5 Mbaud from firmware 0.13, 4 Mbaud for 0.12 and while unknown",
          WifiRates::maxFor(0, 13) == 5000000 && WifiRates::maxFor(0, 12) == 4000000 && WifiRates::maxFor(0, 0) == 4000000
          && WifiRates::maxFor(1, 0) == 5000000);
    check("port: 2M -> 3M starts with the module", port->start(3000000, 2000000) && calls.value(0) == QStringLiteral("module 3000000"),
          calls.join(QStringLiteral(", ")));
    check("port: a second change is refused while one runs", !port->start(4000000, 2000000));
    waitFor([&] { return done > 0; }, 3000);
    check("port: link followed, module answered, saved", ok && calls.contains(QStringLiteral("link 3000000"))
          && calls.contains(QStringLiteral("save")) && !calls.contains(QStringLiteral("link 2000000")), calls.join(QStringLiteral(", ")));
    calls.clear();
    done = 0;
    answer = false;
    port->start(4000000, 3000000);
    waitFor([&] { return done > 0; }, 3000);
    check("port: no answer -> link back at the old speed, nothing saved", !ok && calls.contains(QStringLiteral("link 4000000"))
          && calls.value(calls.size() - 1) == QStringLiteral("link 3000000") && !calls.contains(QStringLiteral("save")),
          calls.join(QStringLiteral(", ")));
    // Firmware 0.15 keeps the new speed at once: without an answer the link stays where the module is.
    calls.clear();
    done = 0;
    answer = false;
    check("port (0.15): starts", port->start(3000000, 2000000, true) && port->moduleKeepsIt());
    waitFor([&] { return done > 0; }, 3000);
    check("port (0.15): no answer -> the link stays at the new speed, nothing sent back", !ok
          && calls.contains(QStringLiteral("link 3000000")) && !calls.contains(QStringLiteral("link 2000000"))
          && !calls.contains(QStringLiteral("save")), calls.join(QStringLiteral(", ")));
    check("port rules: rate saved at once from 0.15", WifiNetRules::rateSavedAtOnce(0, 15) && !WifiNetRules::rateSavedAtOnce(0, 14)
          && !WifiNetRules::rateSavedAtOnce(0, 0) && WifiNetRules::rateSavedAtOnce(1, 0));
    // A module whose firmware does not take the speed refuses ID_UART (at the old speed, before switching).
    calls.clear();
    done = 0;
    check("port: 5M starts", port->start(5000000, 3000000) && calls.value(0) == QStringLiteral("module 5000000"));
    port->moduleRefused();
    check("port: refused -> ends at once, the link never moves", done == 1 && !ok && port->refused() && !port->active()
          && calls == QStringList{ QStringLiteral("module 5000000") }, calls.join(QStringLiteral(", ")));
    delete port;

    QStringList rc;
    WifiRadioCheck radio({
        [&]() { rc << QStringLiteral("ask"); },
        [&](double p, int proto, int w, int ps) { rc << QStringLiteral("save %1 %2 %3 %4").arg(p).arg(proto).arg(w).arg(ps); }
    }, WifiRadioCheck::Timing{ 100, 150, 1500 });
    int outcome = -1;
    QString text;
    QObject::connect(&radio, &WifiRadioCheck::finished, [&](int o, const QString& t) { outcome = o; text = t; });
    radio.start(20, 7, 40, 0);
    radio.onRadio(20, 0xFF);                               // reconnecting
    waitFor([&] { return rc.contains(QStringLiteral("ask")); }, 1000);
    check("radio: asks for the state while the link is down", rc.contains(QStringLiteral("ask")) && outcome < 0);
    radio.onRadio(20, 4);                                  // back on HT20
    check("radio: 40 MHz on an HT20 link -> saved as 20", outcome == WifiRadioCheck::Corrected
          && rc.contains(QStringLiteral("save 20 7 20 0")), text);
    outcome = -1;
    rc.clear();
    radio.start(11, 7, 20, 0);
    QThread::msleep(120);                                  // past the settle time
    radio.onRadio(11, 4);
    check("radio: as asked -> nothing saved", outcome == WifiRadioCheck::AsAsked && !rc.join(QStringLiteral(",")).contains(QStringLiteral("save")),
          text);
    outcome = -1;
    radio.start(20, 8, 20, 0);                             // LR only, the access point has no LR
    waitFor([&] { return outcome >= 0; }, 3000);
    check("radio: no link -> gives up, nothing saved", outcome == WifiRadioCheck::NoLink, text);

    WifiRestartWatch w;
    check("restart: the first report is not one", !w.onUptime(100, 0));
    check("restart: 100 s then 102 s two seconds later -> running", !w.onUptime(102, 2000));
    check("restart: 102 s, a minute later 3 s -> restart", w.onUptime(3, 60000));
    check("restart: a 4 s boot loop (3 s, a second later 1 s) -> restart", w.onUptime(1, 61000));
    w.onUptime(50, 100000);
    check("restart: between rare reports (50 s, a minute later 20 s) -> restart", w.onUptime(20, 160000));
    w.onUptime(50, 200000);
    check("restart: a report 2 s late on this side -> running", !w.onUptime(108, 260000));
    // Requests the module could not queue (0.14): the first report is the baseline, one line per interval with the sum.
    WifiLossWatch loss(10000);
    const bool lossBase = loss.onReport(5, 0, false) == 0 && loss.onReport(5, 500, false) == 0;
    const bool lossFirst = loss.onReport(8, 1000, false) == 3;
    const bool lossHeld = loss.onReport(9, 2000, false) == 0 && loss.onReport(9, 12000, false) == 1;
    check("lost requests: baseline, first growth at once, then one line per 10 s with the sum", lossBase && lossFirst && lossHeld);
    WifiLossWatch wrap(0);
    wrap.onReport(0xFFFFFFFEu, 0, false);
    check("lost requests: a wrap at 2^32 still counts", wrap.onReport(1u, 100, false) == 3u);
    WifiLossWatch again(0);
    again.onReport(40, 0, false);
    check("lost requests: after a module restart its new count adds up", again.onReport(2, 100, true) == 2u
          && again.onReport(2, 200, false) == 0u);
    check("restart: reason 6 -> task watchdog", WifiRestartWatch::reasonText(6).contains(QStringLiteral("task watchdog")));
    check("restart: an unlisted reason -> its code", WifiRestartWatch::reasonText(42).contains(QStringLiteral("42")));

    // Access-point role (firmware 0.10+): there is no negotiated PHY, the width in effect is the driver's.
    QStringList apSaves;
    WifiRadioCheck apCheck({
        [&]() {},
        [&](double p, int proto, int wd, int ps) { apSaves << QStringLiteral("%1 %2 %3 %4").arg(p).arg(proto).arg(wd).arg(ps); }
    }, WifiRadioCheck::Timing{ 100, 150, 1500 });
    int apOutcome = -1;
    QObject::connect(&apCheck, &WifiRadioCheck::finished, [&](int o, const QString&) { apOutcome = o; });
    apCheck.start(20, 7, 20, 0, true);
    QThread::msleep(120);
    apCheck.onRadio(20, 0xFF, 20);
    check("radio (access point): decided without a negotiated PHY", apOutcome == WifiRadioCheck::AsAsked);
    apOutcome = -1;
    apCheck.start(18, 7, 20, 0, true);
    QThread::msleep(120);
    apCheck.onRadio(17.5, 0xFF, 20);
    check("radio (access point): the driver's power step is saved", apOutcome == WifiRadioCheck::Corrected
          && apSaves.contains(QStringLiteral("17.5 7 20 0")), apSaves.join(QStringLiteral(", ")));
    apCheck.start(20, 7, 20, 0, true);
    apCheck.cancel();
    check("radio: a refused setting cancels the check", !apCheck.active());
}

bool target0(const WifiLineChange& c) { return c.target() == 0; }

// The module's radio rules (ERR_PAYLOAD otherwise): 40 MHz only with 802.11n; LR only for an access point from 0.12.
void wifiRadioRules()
{
    check("radio rule: 40 MHz with b/g/n and b/g/n+LR only", WifiRadioRules::allows40(0x07) && WifiRadioRules::allows40(0x0F)
          && !WifiRadioRules::allows40(0x01) && !WifiRadioRules::allows40(0x03) && !WifiRadioRules::allows40(0x08));
    check("radio rule: LR, b and b/g asked at 40 MHz go at 20", WifiRadioRules::widthFor(0x08, 40) == 20
          && WifiRadioRules::widthFor(0x01, 40) == 20 && WifiRadioRules::widthFor(0x03, 40) == 20);
    check("radio rule: b/g/n and b/g/n+LR keep the width asked", WifiRadioRules::widthFor(0x07, 40) == 40
          && WifiRadioRules::widthFor(0x0F, 40) == 40 && WifiRadioRules::widthFor(0x07, 20) == 20 && WifiRadioRules::widthFor(0x08, 20) == 20);
    check("signal: not linked, or an access point without clients (0) -> dim", WifiSignal::level(false, -50) == -1
          && WifiSignal::level(true, 0) == -1);
    check("signal: -60 and better 3 arcs, -61..-70 2, -71..-80 1, below -80 the dot", WifiSignal::level(true, -45) == 3
          && WifiSignal::level(true, -60) == 3 && WifiSignal::level(true, -61) == 2 && WifiSignal::level(true, -70) == 2
          && WifiSignal::level(true, -71) == 1 && WifiSignal::level(true, -80) == 1 && WifiSignal::level(true, -81) == 0
          && WifiSignal::level(true, -95) == 0);
    check("signal: the fast report period is one the module takes (100..60000 ms) and faster than its second",
          WifiSignal::kFastReportMs >= 100 && WifiSignal::kFastReportMs < 1000);
    check("radio rule: LR only for an access point: 0.11 no, 0.12 and 1.0 yes, 0.0 unknown",
          WifiRadioRules::lrOnlyApSupport(0, 11) == 0 && WifiRadioRules::lrOnlyApSupport(0, 12) == 1
          && WifiRadioRules::lrOnlyApSupport(0, 13) == 1 && WifiRadioRules::lrOnlyApSupport(1, 0) == 1
          && WifiRadioRules::lrOnlyApSupport(0, 0) == -1);
}

// Firmware 0.12 port information (ID_WIFI_NET v7), decoded from payloads laid out byte by byte as the ports
// contract's tables, and the rate change of the port this host is not on (ID_WIFI_NET v3 record).
void wifiPortsRules()
{
    const auto put16 = [](QByteArray& b, int at, uint16_t v) {
        b[at] = char(v & 0xFF);
        b[at + 1] = char(v >> 8);
    };
    const auto put32 = [](QByteArray& b, int at, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            b[at + i] = char((v >> (8 * i)) & 0xFF);
    };
    // Page 0 with 3 bytes a later firmware might append (ignored by the minimum-length rule).
    const auto summary = [&](int port, uint8_t flags, uint32_t baud, uint32_t saved, uint8_t connected, uint32_t rx, uint32_t tx) {
        QByteArray b(WifiPorts::kSummaryBytes, char(0));
        b[0] = char(port);
        b[1] = 0;
        b[2] = char(flags);
        put32(b, 3, baud);
        put32(b, 7, saved);
        b[11] = char(connected);
        put32(b, 12, rx);
        put32(b, 16, tx);
        put16(b, 20, 5);            // RX 0.5 s ago
        put16(b, 22, 0xFFFF);       // TX never
        for (int k = 0; k < WifiPorts::kProtocols; ++k)
            put16(b, 24 + 10 * k + 8, 0xFFFF);
        put32(b, 24, 40);           // KP1: 40 from the port, 12 to it, last 0.3 s ago
        put32(b, 28, 12);
        put16(b, 32, 3);
        put32(b, 24 + 40, 7);       // MAVLink 2 (the fifth): 7 from the port
        put32(b, 84, 11);
        put32(b, 88, 22);
        put32(b, 92, 33);
        put32(b, 96, 44);
        put32(b, 100, 55);
        put32(b, 104, 66);
        put32(b, 108, 1);
        put32(b, 112, 2);
        b.append(QByteArray(3, char(0xAA)));
        return b;
    };
    const auto apply = [](WifiPorts& p, const QByteArray& b, qint64 t) {
        return p.apply(reinterpret_cast<const uint8_t*>(b.constData()), int(b.size()), t);
    };
    const auto near = [](double a, double b) { return a > b - 0.01 && a < b + 0.01; };

    WifiPorts p;
    const int page0 = apply(p, summary(0, 0x0F, 3000000, 2000000, 2, 1000, 500), 0);
    const WifiPorts::Summary& x1 = p.summary(0);
    check("ports: page 0 of X1, every field at its offset", page0 == 0 && x1.valid && x1.flags == 0x0F && x1.baud == 3000000
          && x1.baudSaved == 2000000 && x1.connected == WifiPorts::ConnSbpHost && x1.rxBytes == 1000 && x1.txBytes == 500
          && x1.rxAge == 5 && x1.txAge == WifiPorts::kAgeNever && x1.protocols[0].fromPort == 40 && x1.protocols[0].toPort == 12
          && x1.protocols[0].ageFrom == 3 && x1.protocols[4].fromPort == 7 && x1.protocols[5].ageFrom == WifiPorts::kAgeNever
          && x1.sbpRequestsFrom == 11 && x1.sbpContentFrom == 22 && x1.sbpRequestsTo == 33 && x1.sbpContentTo == 44
          && x1.moduleRequests == 55 && x1.moduleFrames == 66 && x1.rxOverflows == 1 && x1.txDrops == 2);
    check("ports: flag b2 on X1 -> this host is on X1", p.askedPort() == 0);
    check("ports: the first summary has no byte rate yet", p.summary(0).rxBps < 0 && p.summary(0).txBps < 0);
    apply(p, summary(0, 0x0F, 3000000, 2000000, 2, 21000, 2500), 2000);
    check("ports: 20000 / 2000 bytes more 2 s later -> 10000 / 1000 B/s", near(p.summary(0).rxBps, 10000) && near(p.summary(0).txBps, 1000),
          QStringLiteral("%1 / %2").arg(p.summary(0).rxBps).arg(p.summary(0).txBps));
    apply(p, summary(0, 0x0F, 3000000, 2000000, 2, 21100, 2500), 2300);
    check("ports: a second answer 0.3 s later keeps the rate", near(p.summary(0).rxBps, 10000));
    apply(p, summary(0, 0x0F, 3000000, 2000000, 2, 50, 10), 4300);
    check("ports: a counter that went back (module restart) -> rate unknown again", p.summary(0).rxBps < 0);
    apply(p, summary(0, 0x0F, 3000000, 2000000, 2, 50, 10), 30000);
    apply(p, summary(0, 0x0F, 3000000, 2000000, 2, 5050, 10), 32000);
    check("ports: after a long gap the next pair gives the rate again", near(p.summary(0).rxBps, 2500),
          QString::number(p.summary(0).rxBps));
    apply(p, summary(1, 0x03, 115200, 115200, 5, 0, 0), 32000);
    check("ports: X2 without b2 -> this host stays on X1", p.askedPort() == 0 && p.summary(1).connected == WifiPorts::ConnMavlink);
    apply(p, summary(0, 0x0B, 3000000, 3000000, 3, 0, 0), 34000);
    apply(p, summary(1, 0x27, 115200, 115200, 4, 0, 0), 34000);
    check("ports: b2 on X2 -> this host is on X2 (and b5 = provisional)", p.askedPort() == 1
          && (p.summary(1).flags & WifiPorts::FlagProvisional) != 0);
    apply(p, summary(1, 0x23, 115200, 115200, 4, 0, 0), 36000);
    check("ports: b2 on neither -> the network (-1)", p.askedPort() == -1);
    check("ports: a summary one byte short is refused and changes nothing",
          apply(p, summary(0, 0x0F, 9600, 9600, 0, 0, 0).left(WifiPorts::kSummaryBytes - 1), 38000) < 0 && p.summary(0).baud == 3000000);
    check("ports: port 2 is refused", apply(p, summary(2, 0x0F, 9600, 9600, 0, 0, 0), 38000) < 0);

    QByteArray dev(3 + 2 * 12, char(0));
    dev[0] = 1;
    dev[1] = 1;
    dev[2] = 2;
    dev[3] = char(WifiPorts::KindSbp);                          // SBP device at address 0, Basic2D (12), fw 3.4
    dev[4] = 0;
    dev[5] = 12;
    dev[6] = 3;
    dev[7] = 4;
    dev[8] = char(WifiPorts::DevVersionKnown);
    put16(dev, 9, 7);
    put32(dev, 11, 0x01020304);
    dev[15] = char(WifiPorts::KindMavlink);                     // MAVLink 1/1, boat (11), ArduPilot (3), 250 frames
    dev[16] = 1;
    dev[17] = 1;
    dev[18] = 11;
    dev[19] = 3;
    dev[20] = char(WifiPorts::DevHeartbeat);
    put16(dev, 21, 0xFFFF);
    put32(dev, 23, 250);
    const int page1 = apply(p, dev, 38000);
    const QVector<WifiPorts::Device>& dl = p.devices(1).list;
    check("ports: page 1 of X2, an SBP device and a MAVLink system", page1 == 1 && p.devices(1).valid && dl.size() == 2
          && dl[0].kind == WifiPorts::KindSbp && dl[0].addr == 0 && dl[0].board == 12 && dl[0].major == 3 && dl[0].minor == 4
          && dl[0].flags == WifiPorts::DevVersionKnown && dl[0].age == 7 && dl[0].serial == 0x01020304
          && dl[1].kind == WifiPorts::KindMavlink && dl[1].addr == 1 && dl[1].board == 1 && dl[1].major == 11 && dl[1].minor == 3
          && dl[1].age == WifiPorts::kAgeNever && dl[1].serial == 250 && !p.devices(0).valid);
    check("ports: page 1 shorter than its count is refused", apply(p, dev.left(3 + 12 + 5), 38000) < 0 && p.devices(1).list.size() == 2);
    check("ports: page 1 with no devices", apply(p, QByteArray("\x01\x01\x00", 3), 38000) == 1 && p.devices(1).valid
          && p.devices(1).list.isEmpty());

    QByteArray net(5 + 8, char(0));
    net[0] = 0;
    net[1] = 2;
    net[2] = 1;                                                 // UDP
    net[3] = 3;                                                 // ready
    net[4] = 1;
    net[5] = 10;
    net[6] = 0;
    net[7] = 0;
    net[8] = 10;
    put16(net, 9, 14444);
    put16(net, 11, 4);
    const int page2 = apply(p, net, 38000);
    const WifiPorts::Network& n0 = p.network(0);
    check("ports: page 2 of X1, UDP ready, peer 10.0.0.10:14444 0.4 s ago", page2 == 2 && n0.valid && n0.mode == 1 && n0.state == 3
          && n0.peers.size() == 1 && n0.peers[0].ip == 0x0A00000Au && n0.peers[0].port == 14444 && n0.peers[0].age == 4);
    check("ports: an unknown page is refused", apply(p, QByteArray("\x00\x03\x00\x00\x00", 5), 38000) < 0 && p.lastPage() == 2);
    p.reset();
    check("ports: reset forgets everything", !p.summary(0).valid && !p.devices(1).valid && !p.network(0).valid && p.askedPort() == -1);

    // A line record: only the edited fields change, the rest is what the module sent last.
    const WifiLineRecord fresh{ 1, 1, 0x0A00000Au, 14444, 0, 921600, 4, 5 };
    WifiLineEdit ed;
    ed.lport = 14446;
    ed.dest = 0;
    const WifiLineRecord out = ed.appliedTo(fresh);
    check("edit: only lport and dest change", out.mode == 1 && out.dest == 0 && out.ip == 0x0A00000Au && out.rport == 14444
          && out.lport == 14446 && out.baud == 921600 && out.txPin == 4 && out.rxPin == 5 && !ed.rateOnly() && !ed.empty());
    WifiLineEdit rateEd;
    rateEd.baud = 115200;
    check("edit: a rate alone is the port-rate path", rateEd.rateOnly() && rateEd.appliedTo(fresh).baud == 115200
          && rateEd.appliedTo(fresh).lport == 0 && WifiLineEdit().empty());

    QStringList lc;
    bool sendOk = true;
    WifiLineChange line({
        [&](int l) { lc << QStringLiteral("ask %1").arg(l); },
        [&](int l, const WifiLineEdit& e) {
            const WifiLineRecord r = e.appliedTo(fresh);
            lc << QStringLiteral("send %1 mode %2 lport %3 baud %4").arg(l).arg(r.mode).arg(r.lport).arg(r.baud);
            return sendOk;
        },
        [&](int l) { lc << QStringLiteral("readback %1").arg(l); }
    }, WifiLineChange::Timing{ 150 });
    int lcOutcome = -1;
    int lcResp = -1;
    QObject::connect(&line, &WifiLineChange::finished, [&](int o, int, int r) { lcOutcome = o; lcResp = r; });
    WifiLineEdit tooFast;
    tooFast.baud = 6000000;
    check("line: refuses line 2, 6000000 and an empty edit", !line.start(2, rateEd) && !line.start(1, tooFast)
          && !line.start(1, WifiLineEdit()) && lc.isEmpty());
    check("line: X2 -> 115200 reads the record first", line.start(1, rateEd) && lc == QStringList{ QStringLiteral("ask 1") }
          && !line.awaitingAnswer() && line.target() == 115200, lc.join(QStringLiteral(", ")));
    check("line: a second change is refused while one runs", !line.start(0, ed));
    line.onRecord(0);
    check("line: the other line's record is not used", lc.size() == 1);
    line.onRecord(1);
    check("line: the record came -> sent back with the new rate only, waiting for the answer",
          lc.value(1) == QStringLiteral("send 1 mode 1 lport 0 baud 115200") && line.awaitingAnswer(), lc.join(QStringLiteral(", ")));
    line.onAnswer(1);
    check("line: OK -> read back, done", lcOutcome == WifiLineChange::Done && lc.value(2) == QStringLiteral("readback 1")
          && !line.active(), lc.join(QStringLiteral(", ")));
    lc.clear();
    lcOutcome = -1;
    line.start(0, ed);
    waitFor([&] { return lcOutcome >= 0; }, 2000);
    check("line: no record -> nothing sent", lcOutcome == WifiLineChange::NoRecord
          && !lc.join(QStringLiteral(",")).contains(QStringLiteral("send")), lc.join(QStringLiteral(", ")));
    lc.clear();
    lcOutcome = -1;
    line.start(1, ed);
    line.onRecord(1);
    check("line: settings edit -> the fresh record with lport changed, the rate as the module had it",
          lc.value(1) == QStringLiteral("send 1 mode 1 lport 14446 baud 921600") && target0(line), lc.join(QStringLiteral(", ")));
    line.onAnswer(3);                                          // ERR_PAYLOAD: two UDP lines on one local port
    check("line: refused -> reported with its code, read back", lcOutcome == WifiLineChange::Refused && lcResp == 3
          && lc.contains(QStringLiteral("readback 1")) && !line.edit().rateOnly(), lc.join(QStringLiteral(", ")));
    lc.clear();
    lcOutcome = -1;
    line.start(1, rateEd);
    line.onRecord(1);
    waitFor([&] { return lcOutcome >= 0; }, 2000);
    check("line: no answer -> read back, reported", lcOutcome == WifiLineChange::NoAnswer && lc.contains(QStringLiteral("readback 1")),
          lc.join(QStringLiteral(", ")));
    sendOk = false;
    lc.clear();
    lcOutcome = -1;
    line.start(1, rateEd);
    line.onRecord(1);
    check("line: a record that cannot be sent -> over, nothing waits", lcOutcome == WifiLineChange::NotSent && !line.active());
}

} // namespace

// Field test 2026-09-29: devices that look present without a link, the keyboard that came up while scrolling.
void fieldTestRules()
{
    using L = DeviceLiveness;
    check("liveness: last frame 2.9 s ago -> there; 3.0 s -> without data",
          !L::stale(true, true, false, 1000, 3900) && L::stale(true, true, false, 1000, 4000));
    check("liveness: never before the first frame, during an update, unconnected or unmarked",
          !L::stale(true, true, false, 0, 100000) && !L::stale(true, true, true, 1000, 100000)
          && !L::stale(false, true, false, 1000, 100000) && !L::stale(true, false, false, 1000, 100000));
    check("liveness: a quiet device is asked after 1 s, then once a second, not in between",
          !L::probeDue(1000, 0, 1999) && L::probeDue(1000, 0, 2000) && !L::probeDue(1000, 2000, 2999)
          && L::probeDue(1000, 2000, 3000));
    check("liveness: asked twice before it counts as without data (a lost answer is not enough)",
          L::kStaleMs >= 2 * L::kQuietProbeMs + L::kQuietProbeMs);

    const QRectF field(0, 100, 200, 30);          // a text field, window coordinates
    const qreal drag = 10;
    {
        TouchTapGate g;
        g.touchBegan(QPointF(50, 110), 1);
        g.focusChanged(true, false);              // the press focused the field
        const bool heldWhileDown = g.heldBack();
        const bool tap = g.touchEnded(QPointF(52, 111), false);
        check("keyboard: a tap that focuses a field brings the keyboard up at its end",
              heldWhileDown && tap && g.releaseOnTap(tap, field, drag) && !g.heldBack());
    }
    {
        TouchTapGate g;
        g.touchBegan(QPointF(50, 110), 1);
        g.focusChanged(true, false);
        g.touchMoved(QPointF(50, 150), 1, drag);  // the finger scrolls the page
        const bool tap = g.touchEnded(QPointF(50, 300), false);
        const bool stays = !tap && !g.releaseOnTap(tap, field, drag) && g.heldBack();
        // the page moved: the field is at y 300 now, the operator taps it there
        g.touchBegan(QPointF(60, 310), 1);
        const bool tap2 = g.touchEnded(QPointF(61, 311), false);
        check("keyboard: a scroll that starts on a field leaves it down; a tap on the field then brings it up",
              stays && tap2 && g.releaseOnTap(tap2, QRectF(0, 300, 200, 30), drag) && !g.heldBack());
    }
    {
        TouchTapGate g;
        g.touchBegan(QPointF(50, 110), 1);
        g.focusChanged(true, false);
        g.touchMoved(QPointF(50, 150), 1, drag);
        g.touchEnded(QPointF(50, 150), false);
        g.touchBegan(QPointF(150, 500), 1);       // a tap somewhere else
        const bool tap = g.touchEnded(QPointF(150, 500), false);
        check("keyboard: a tap elsewhere keeps it down", tap && !g.releaseOnTap(tap, field, drag) && g.heldBack());
    }
    {
        TouchTapGate g;
        g.touchBegan(QPointF(50, 110), 1);
        g.focusChanged(true, false);
        g.touchMoved(QPointF(50, 110), 2, drag);  // a second finger
        const bool pinch = g.touchEnded(QPointF(50, 110), false);
        TouchTapGate c;
        c.touchBegan(QPointF(50, 110), 1);
        c.focusChanged(true, false);
        const bool canceled = c.touchEnded(QPointF(50, 110), true);
        check("keyboard: a second finger or a canceled touch is no tap", !pinch && g.heldBack() && !canceled && c.heldBack());
    }
    {
        TouchTapGate g;
        g.focusChanged(true, false);              // Tab key, no touch
        const bool byKey = !g.heldBack();
        g.touchBegan(QPointF(50, 110), 1);
        g.focusChanged(true, true);               // the keyboard is up already for another field
        const bool whileUp = !g.heldBack();
        g.focusChanged(false, false);             // a button took the focus
        check("keyboard: not held for focus without a touch, with the keyboard already up, or on a non-text item",
              byKey && whileUp && !g.heldBack());
    }
    {
        TouchTapGate g;
        g.touchBegan(QPointF(50, 110), 1);
        g.focusChanged(true, false);
        g.touchMoved(QPointF(55, 114), 1, drag);  // a shaky tap: 9 px
        const bool tap = g.touchEnded(QPointF(55, 114), false);
        check("keyboard: a tap that moves less than the drag distance is still a tap", tap && g.releaseOnTap(tap, field, drag));
    }
}

// Owner 2026-09-29: pick the freest channel for the boat's access point from a scan.
void channelRules()
{
    using C = WifiChannels;
    auto near = [](double a, double b) { return a > b - 0.05 && a < b + 0.05; };
    auto at = [](const QVector<C::Channel>& r, int ch) { return r.value(ch - C::kFirst); };

    const QVector<C::Channel> none = C::rate({});
    bool floor = none.size() == C::kLast - C::kFirst + 1;
    for (const C::Channel& ch : none)
        floor = floor && ch.networks == 0 && near(ch.levelDbm, C::kFloorDbm);
    check("channels: nothing heard -> all 13 at the floor, channel 1 best", floor && C::best(none) == 1);

    const QVector<C::Channel> one = C::rate({ { 6, -40, false } });
    check("channels: a network on 6 at -40 counts fully on 6, 14/18 one off, 9/18 two off, 4/18 three off, "
          "-20 dB four off, nothing five off",
          near(at(one, 6).levelDbm, -40.0) && near(at(one, 5).levelDbm, -41.09) && near(at(one, 7).levelDbm, -41.09)
          && near(at(one, 4).levelDbm, -43.01) && near(at(one, 3).levelDbm, -46.53) && near(at(one, 2).levelDbm, -60.0)
          && near(at(one, 1).levelDbm, C::kFloorDbm) && near(at(one, 11).levelDbm, C::kFloorDbm)
          && C::best(one) == 1,
          QStringLiteral("5: %1, 4: %2, 3: %3, 2: %4, best %5").arg(at(one, 5).levelDbm).arg(at(one, 4).levelDbm)
              .arg(at(one, 3).levelDbm).arg(at(one, 2).levelDbm).arg(C::best(one)));

    const QVector<C::Channel> own = C::rate({ { 6, -40, true } });
    check("channels: our own network is left out", at(own, 6).networks == 0 && near(at(own, 6).levelDbm, C::kFloorDbm));

    check("channels: networks on 1 and 11 -> 6 is the least busy", C::best(C::rate({ { 1, -50, false }, { 11, -50, false } })) == 6);

    // a network on 4: channels 9, 10 and 11 hear nothing of it; 11 goes first (1, 6, 11 do not overlap each other)
    check("channels: within 1 dB, 1/6/11 before the others", C::best(C::rate({ { 4, -60, false } })) == 11);

    const QVector<C::Channel> many = C::rate({ { 6, -70, false }, { 6, -50, false }, { 3, -80, false } });
    check("channels: the networks on a channel and the strongest of them",
          at(many, 6).networks == 2 && at(many, 6).strongest == -50 && at(many, 3).networks == 1
          && at(many, 3).strongest == -80 && at(many, 4).networks == 0);

    const QVector<C::Channel> bad = C::rate({ { 0, -40, false }, { 14, -40, false }, { 6, 0, false } });
    bool ignored = true;
    for (const C::Channel& ch : bad)
        ignored = ignored && ch.networks == 0 && near(ch.levelDbm, C::kFloorDbm);
    check("channels: channel 0 or 14 and RSSI 0 are ignored", ignored);
}

// The module's survey (ID_WIFI_SURVEY, firmware 0.16): Wi-Fi airtime per channel.
void surveyRules()
{
    using S = WifiSurvey;
    check("survey: from firmware 0.16, not 0.15, unknown while no version",
          S::supportFor(0, 16) == 1 && S::supportFor(1, 0) == 1 && S::supportFor(0, 15) == 0 && S::supportFor(0, 0) == -1);
    auto ch = [](int channel, int busy, int flags) {
        S::Channel c;
        c.valid = true;
        c.channel = channel;
        c.busyPermille = busy;
        c.flags = flags;
        return c;
    };
    const int ap = 0x02, own = 0x01 | 0x02;
    // our 6 at 4.5 % (2.0 % of it our own link) against 3.0 % and 2.8 %: no move for 0.2 %
    check("survey: our own channel is kept when about as free (our link's share left out)",
          S::best({ ch(1, 30, ap), ch(6, 45, own), ch(11, 28, ap) }) == 6);
    // our 6 at 8 % against 1.0 % on 3 and 1.2 % on 11: 11 goes first within 0.5 %
    check("survey: a clearly freer channel wins, 1/6/11 first within 0.5 %",
          S::best({ ch(3, 10, ap), ch(6, 80, own), ch(11, 12, ap) }) == 11);
    check("survey: a channel the access point may not use is left out",
          S::best({ ch(3, 40, ap), ch(2, 0, 0) }) == 3 && S::best({ ch(1, 40, ap), ch(12, 0, 0) }) == 1 && S::best({}) == 0);
    check("survey: timeout 11 channels x (120 + 30) ms + 6 s", S::timeoutMs(11, 120) == 7650);

    // The three surveys of the kiosk log (2026-09-29, per mille, * = our channel 10): one by one they pick 2, 7 and 5.
    auto survey = [&](std::initializer_list<int> busy) {
        QVector<S::Channel> v;
        int channel = 1;
        for (int b : busy) {
            v.append(ch(channel, b, channel == 10 ? own : ap));
            ++channel;
        }
        return v;
    };
    const QVector<S::Channel> s1 = survey({ 382, 105, 151, 153, 167, 191, 102, 125, 428, 385, 517 });
    const QVector<S::Channel> s2 = survey({ 152, 43, 62, 72, 97, 141, 0, 186, 295, 354, 286 });
    const QVector<S::Channel> s3 = survey({ 338, 68, 203, 289, 0, 367, 74, 382, 377, 454, 489 });
    WifiSurveyWindow w;
    w.add(s1, 0);
    w.add(s2, 10000);
    w.add(s3, 20000);
    const QVector<S::Channel> m = w.mean(20000);
    auto meanOf = [&](int channel) {
        for (const S::Channel& c : m)
            if (c.channel == channel)
                return c.busyPermille;
        return -1;
    };
    check("survey mean: the kiosk's three surveys pick 2, 7, 5 one by one and 7 by their mean",
          S::best(s1) == 2 && S::best(s2) == 7 && S::best(s3) == 5 && w.count(20000) == 3 && S::best(m) == 7
              && meanOf(7) == 59 && meanOf(5) == 88 && meanOf(2) == 72,
          QStringLiteral("mean 2: %1, 5: %2, 7: %3, best %4").arg(meanOf(2)).arg(meanOf(5)).arg(meanOf(7)).arg(S::best(m)));
    WifiSurveyWindow five;
    for (int i = 0; i < 7; ++i)
        five.add(s1, i * 1000);
    check("survey mean: over 5 surveys at most", five.count(6000) == WifiSurveyWindow::kKeep);
    WifiSurveyWindow moved;
    moved.add(s1, 0);
    QVector<S::Channel> after = s1;
    after[9].flags = ap;                          // channel 10 no longer ours
    after[6].flags = own;                         // our link moved to 7
    moved.add(after, 5000);
    check("survey mean: our channel changed -> the mean starts over", moved.count(5000) == 1);
    // Firmware 0.17: our own network's airtime, subtracted on every channel. Our channel 10 at 38.5 % (6.0 % ours); its
    // neighbour 9 at 13.0 % of which 4.0 % ours (it hears our link) against 2 at 10.0 %: 9 has 9.0 % of the others.
    auto withOwn = [&](int channel, int busy, int ownAir, int flags) {
        S::Channel c = ch(channel, busy, flags);
        c.ownPermille = ownAir;
        c.ownValid = true;
        return c;
    };
    check("survey 0.17: our own network's airtime left out on every channel, a neighbour of ours included",
          S::best({ withOwn(2, 100, 0, ap), withOwn(9, 130, 40, ap), withOwn(10, 385, 60, own) }) == 9
              && S::compared(withOwn(10, 385, 60, own)) == 325 && S::compared(withOwn(9, 130, 40, ap)) == 90);
    check("survey 0.16 (no such field): our channel less the 2 % guess, the others as measured",
          S::compared(ch(10, 385, own)) == 365 && S::compared(ch(9, 130, ap)) == 130);
    WifiSurveyWindow ownMean;
    ownMean.add({ withOwn(9, 130, 40, ap), withOwn(10, 385, 60, own) }, 0);
    ownMean.add({ withOwn(9, 110, 20, ap), withOwn(10, 365, 40, own) }, 5000);
    check("survey mean: our own network's airtime averaged too",
          ownMean.mean(5000).value(0).ownValid && ownMean.mean(5000).value(0).ownPermille == 30
              && S::compared(ownMean.mean(5000).value(0)) == 90);
    WifiSurveyWindow old;
    old.add(s1, 0);
    old.add(s2, 50000);
    // 70 s on, with no survey since: the first is over a minute old
    check("survey mean: a survey older than a minute is left out",
          old.count(50000) == 2 && old.count(70000) == 1 && old.mean(70000).value(6).busyPermille == 0);
}

// WifiLinkLog: the link's recent events for the Wi-Fi island.
void linkLogRules()
{
    using L = WifiLinkLog;
    const QString boat = QStringLiteral("KoggerBoat");
    // The head unit's module 2026-09-29 (kogger.log): up, the boat's beacons gone (200), the network not heard (201),
    // back. A repeated status is no event.
    L st;
    const bool added = st.onStatus(3, 0, 0, 0, boat, 10, -50, 1000);
    const bool repeat = st.onStatus(3, 0, 0, 0, boat, 10, -51, 2000);
    st.onStatus(2, 4, 200, 0, boat, 0, 0, 961000);
    st.onStatus(1, 1, 201, 0, QString(), 0, 0, 968000);
    st.onStatus(3, 0, 0, 0, boat, 10, -52, 3000000);
    const auto& e = st.events();
    check("link log: a state change is an event, a repeated status is not", added && !repeat && e.size() == 4);
    check("link log: the first event has no state before it", e.size() == 4 && e[0].prevState == -1 && e[0].state == 3);
    check("link log: the link lost keeps why (the module's class and the ESP-IDF code) and what it left",
          e.size() == 4 && e[1].state == 2 && e[1].prevState == 3 && e[1].reason == 4 && e[1].code == 200
              && e[1].atMs == 961000 && e[2].state == 1 && e[2].code == 201 && e[3].prevState == 1 && e[3].rssi == -52);

    // The boat's module as an access point: its clients come and go; a station's clients are no event.
    L ap;
    ap.onStatus(4, 0, 0, 1, boat, 10, -70, 0);
    ap.onStatus(4, 0, 0, 0, boat, 10, 0, 5000);
    ap.onStatus(4, 0, 0, 1, boat, 10, -68, 9000);
    ap.onStatus(4, 0, 0, 1, boat, 10, -69, 9500);
    const auto& a = ap.events();
    check("link log: an access point's clients coming and going",
          a.size() == 3 && a[0].kind == L::KindState && a[0].clients == 1 && a[1].kind == L::KindClients
              && a[1].clients == 0 && a[1].prevClients == 1 && a[2].clients == 1 && a[2].prevClients == 0);
    L sta;
    sta.onStatus(3, 0, 0, 0, boat, 10, -50, 0);
    check("link log: a station's client field is no event", !sta.onStatus(3, 0, 0, 2, boat, 10, -50, 1000));

    // At most kKeep, the newest kept; a restart is an event of its own.
    L many;
    for (int i = 0; i < L::kKeep + 8; ++i)
        many.onStatus(i % 2 ? 2 : 3, 0, 0, 0, boat, 10, -50, i * 1000);
    many.onRestart(QStringLiteral("task watchdog"), 999000);
    const auto& m = many.events();
    check("link log: at most kKeep kept, the newest last",
          m.size() == L::kKeep && m.last().kind == L::KindRestart && m.last().text == QStringLiteral("task watchdog")
              && m[m.size() - 2].atMs == (L::kKeep + 7) * 1000);
}

// A COMMAND_LONG REQUEST_MESSAGE for this message?
bool isRequest(const QByteArray& bytes, uint32_t messageId)
{
    mavlink_message_t m;
    mavlink_status_t st;
    for (char c : bytes) {
        if (mavlink_parse_char(MAVLINK_COMM_3, uint8_t(c), &m, &st) && m.msgid == MAVLINK_MSG_ID_COMMAND_LONG)
            return mavlink_msg_command_long_get_command(&m) == MAV_CMD_REQUEST_MESSAGE
                   && uint32_t(mavlink_msg_command_long_get_param1(&m)) == messageId;
    }
    return false;
}

// Which message an outgoing frame of MavVehicle is.
uint32_t messageOf(const QByteArray& bytes)
{
    mavlink_message_t m;
    mavlink_status_t st;
    for (char c : bytes) {
        if (mavlink_parse_char(MAVLINK_COMM_3, uint8_t(c), &m, &st))
            return m.msgid;
    }
    return UINT32_MAX;
}

// The app runs MavVehicle beside the mission planner (src/mission, src/autopilot; setMissionsByPlanner): it listens and
// keeps the heartbeat, its own missions and commands are refused - one autopilot, one stack talking missions and
// commands to it (2026-10-23).
void plannerModeRules()
{
    MavVehicle v(nullptr, false);
    v.setMissionsByPlanner(true);
    QList<QByteArray> sent;
    QObject::connect(&v, &MavVehicle::sendBytes, [&](const QByteArray& b) { sent.append(b); });
    v.feed(fakeHeartbeat(false, MavVehicle::ModeHold));
    v.feed(fakeHeartbeat(false, MavVehicle::ModeHold));
    waitFor([] { return false; }, 2500);   // past the 1.5 s after which it would read the mission
    int heartbeats = 0;
    bool missionAsked = false;
    for (const QByteArray& b : sent) {
        heartbeats += messageOf(b) == MAVLINK_MSG_ID_HEARTBEAT ? 1 : 0;
        missionAsked = missionAsked || messageOf(b) == MAVLINK_MSG_ID_MISSION_REQUEST_LIST;
    }
    check("planner mode: the mission is not read when the autopilot appears", !missionAsked,
          QStringLiteral("%1 frames sent").arg(sent.size()));
    check("planner mode: the heartbeat goes on", heartbeats >= 1, QStringLiteral("%1 heartbeats").arg(heartbeats));
    sent.clear();
    v.addPoint(40.35, 45.0);
    v.uploadPlan();
    v.readBack();
    v.loadPlanFromVehicle();
    v.startMission();
    v.hold();
    v.returnHome();
    v.arm();
    v.disarm();
    waitFor([] { return false; }, 300);
    int other = 0;
    for (const QByteArray& b : sent)
        other += messageOf(b) != MAVLINK_MSG_ID_HEARTBEAT ? 1 : 0;
    check("planner mode: points, upload, read-back, start, HOLD, RTL, arm and disarm refused",
          v.planCount() == 0 && other == 0 && v.transferState() == MavVehicle::TransferIdle,
          QStringLiteral("plan %1, %2 frames other than heartbeats, transfer %3").arg(v.planCount()).arg(other).arg(v.transferState()));
}

QByteArray fakeMissionCurrent(int seq);
QByteArray fakePosition(double lat, double lon, int vxCms);
QByteArray fakeHome(double lat, double lon);

// Planner mode: the ghost and what is left in AUTO go along the mission the autopilot holds (setVehicleRoute, from
// the planner's tracker), from MISSION_CURRENT's target on (the owner, 2026-10-23: "I like the ghost").
void plannerGhostRules()
{
    using V = MavVehicle;
    V v(nullptr, false);
    v.setMissionsByPlanner(true);
    const double lat0 = 60.0, lon0 = 30.0;
    const double east100 = 100.0 / (111195.0 * 0.5);   // 100 m of longitude at 60 degrees
    // seq 2 behind the boat, seq 3 (the target) 100 m east of it, seq 5 100 m further
    v.setVehicleRoute({ 2, 3, 5 }, { lat0, lat0, lat0 }, { lon0 - east100, lon0 + east100, lon0 + 2 * east100 });
    v.feed(fakeHeartbeat(true, V::ModeAuto));
    v.feed(fakeHeartbeat(true, V::ModeAuto));
    v.feed(fakeHome(lat0 - 0.001, lon0));
    v.feed(fakeMissionCurrent(3));
    v.feed(fakePosition(lat0, lon0, 100));   // 1 m/s
    const double left = V::distanceM(lat0, lon0, lat0, lon0 + east100) + V::distanceM(lat0, lon0 + east100, lat0, lon0 + 2 * east100);
    const bool valid = waitFor([&] { return v.missionProgressValid(); }, 2500);
    check("planner mode: AUTO - what is left goes along the vehicle's route from its target",
          valid && std::abs(v.missionRemainingM() - left) < 1.0,
          QStringLiteral("%1 m, expected %2 m").arg(v.missionRemainingM(), 0, 'f', 1).arg(left, 0, 'f', 1));
    // the link goes: the ghost walks east, towards seq 3, not to home in the south nor back to seq 2
    const bool ghost = waitFor([&] { return v.ghostValid(); }, 6000);
    const double moved = V::distanceM(lat0, lon0, v.ghostLatitude(), v.ghostLongitude());
    check("planner mode: link lost - the ghost moves along the vehicle's route",
          ghost && v.ghostMoving() && v.ghostLongitude() > lon0 && std::abs(v.ghostLatitude() - lat0) < 1e-6,
          QStringLiteral("age %1 s, moved %2 m east").arg(v.ghostAgeS()).arg(moved, 0, 'f', 1));
}

// The "From launch point" widget: home is asked for again on arming (ArduPilot moves it there); the distance.
void homeRules()
{
    MavVehicle v(nullptr, false);
    QList<QByteArray> sent;
    QObject::connect(&v, &MavVehicle::sendBytes, [&](const QByteArray& b) { sent.append(b); });
    v.feed(fakeHeartbeat(false, MavVehicle::ModeHold));
    v.feed(fakeHeartbeat(false, MavVehicle::ModeHold));
    sent.clear();
    v.feed(fakeHeartbeat(true, MavVehicle::ModeHold));
    bool asked = false;
    for (const QByteArray& b : sent)
        asked = asked || isRequest(b, MAVLINK_MSG_ID_HOME_POSITION);
    check("home: asked for again on arming (the launch point)", asked, QStringLiteral("%1 frames sent").arg(sent.size()));
    sent.clear();
    v.feed(fakeHeartbeat(true, MavVehicle::ModeHold));
    bool again = false;
    for (const QByteArray& b : sent)
        again = again || isRequest(b, MAVLINK_MSG_ID_HOME_POSITION);
    check("home: not asked on every heartbeat while armed", !again);
    // 0.001 degree of latitude on the 6371008.8 m sphere: 111.195 m; of longitude at 60 degrees: half of that
    check("distance: 0.001 degree of latitude = 111.2 m",
          std::fabs(MavVehicle::distanceM(0, 0, 0.001, 0) - 111.195) < 0.01,
          QString::number(MavVehicle::distanceM(0, 0, 0.001, 0), 'f', 3));
    check("distance: 0.001 degree of longitude at 60 degrees = 55.6 m",
          std::fabs(MavVehicle::distanceM(60, 30, 60, 30.001) - 55.597) < 0.01,
          QString::number(MavVehicle::distanceM(60, 30, 60, 30.001), 'f', 3));
}

QByteArray fakeParam(const char* id, float value)
{
    mavlink_message_t m;
    char name[16] = {};
    std::strncpy(name, id, sizeof(name));
    mavlink_msg_param_value_pack_chan(1, 1, kFakeChan, &m, name, value, MAV_PARAM_TYPE_INT8, 1, 0);
    return frame(m);
}

QByteArray fakeStatusText(const char* text)
{
    mavlink_message_t m;
    char t[50] = {};
    std::strncpy(t, text, sizeof(t));
    mavlink_msg_statustext_pack_chan(1, 1, kFakeChan, &m, MAV_SEVERITY_INFO, t, 0, 0);
    return frame(m);
}

// What the EKF's yaw rests on (ArduPilot 4.4 SelectMagFusion), not only the setting.
void headingInUseRules()
{
    using V = MavVehicle;
    V v(nullptr, false);
    v.feed(fakeHeartbeat(false, V::ModeHold));
    check("heading in use: nothing read yet -> unknown", v.headingInUse() == V::InUseUnknown, v.headingInUseText());
    v.feed(fakeParam("EK3_SRC1_YAW", 3));
    v.feed(fakeGps(3, 20, 0));
    v.feed(fakeGps2(5, 20, 65535));                 // the pair is configured, RTK float: no heading
    check("heading in use: GPS selected, no GPS heading -> none (the fallback needs motion)",
          v.headingInUse() == V::InUseNone, v.headingInUseText());
    v.feed(fakeGps2(6, 20, 12345));                 // RTK fixed: a heading
    check("heading in use: GPS selected, GPS heading -> GPS", v.headingInUse() == V::InUseGps, v.headingInUseText());
    v.feed(fakeGps2(5, 20, 65535));
    v.feed(fakeStatusText("EKF3 IMU0 yaw fallback active"));
    check("heading in use: GPS lost and the EKF's fallback message -> compass fallback",
          v.headingInUse() == V::InUseCompassFallback, v.headingInUseText());
    v.feed(fakeGps2(6, 20, 12345));
    check("heading in use: the GPS heading back -> GPS again", v.headingInUse() == V::InUseGps, v.headingInUseText());
    v.feed(fakeGps2(5, 20, 65535));
    check("heading in use: the fallback does not come back by itself", v.headingInUse() == V::InUseNone,
          v.headingInUseText());
    v.feed(fakeStatusText("EKF3 IMU0 yaw fallback active"));
    v.feed(fakeParam("EK3_SRC1_YAW", 1));
    check("heading in use: compass selected -> compass", v.headingInUse() == V::InUseCompass, v.headingInUseText());
    v.feed(fakeParam("EK3_SRC1_YAW", 3));
    check("heading in use: back to GPS without a heading -> none (the old fallback is dropped)",
          v.headingInUse() == V::InUseNone, v.headingInUseText());
}

// The link rate report's page (ID_WIFI_SURVEY v1, firmware 0.18) and its words.
QByteArray ratePage(int index, int total, const uint8_t mac[6], int window, int frames, int rssi, int mode,
                    const QVector<WifiLinkRate::Rate>& rates)
{
    QByteArray b;
    const auto u16 = [&b](int v) { b.append(char(v & 0xFF)); b.append(char((v >> 8) & 0xFF)); };
    b.append(char(index));
    b.append(char(total));
    for (int i = 0; i < 6; ++i)
        b.append(char(mac[i]));
    u16(window);
    u16(frames);
    b.append(char(int8_t(rssi)));
    b.append(char(mode));
    b.append(char(rates.size()));
    b.append(char(0));
    for (const WifiLinkRate::Rate& r : rates) {
        b.append(char(r.kind));
        b.append(char(r.code));
        b.append(char(r.flags));
        for (int k = 0; k < 4; ++k)
            b.append(char((r.kbps >> (8 * k)) & 0xFF));
        u16(r.frames);
    }
    return b;
}

void linkRateRules()
{
    using R = WifiLinkRate;
    const uint8_t boat[6] = { 0xd8, 0x3b, 0xda, 0x29, 0xcf, 0xd1 };
    // the module session's measurement 2026-09-30: boat -> head unit, 17 of 21 frames at MCS7 SGI, 4 at 65.0
    R::Rate sgi; sgi.kind = R::Kind11n; sgi.code = 7; sgi.flags = 0x01; sgi.kbps = 72200; sgi.frames = 17;
    R::Rate lgi; lgi.kind = R::Kind11n; lgi.code = 7; lgi.flags = 0x00; lgi.kbps = 65000; lgi.frames = 4;
    R::Page p;
    const QByteArray page = ratePage(0, 1, boat, 200, 21, -43, 4, { sgi, lgi });
    const bool ok = R::parse(page, p);
    check("link rate: a page of 16 + 9 x 2 bytes", page.size() == 16 + 9 * 2 && ok && p.total == 1 && p.rates.size() == 2);
    check("link rate: the other end, its frames and RSSI", ok && p.mac == QStringLiteral("d8:3b:da:29:cf:d1")
          && p.windowMs == 200 && p.frames == 21 && p.rssi == -43 && p.mode == 4, p.mac);
    check("link rate: 11n MCS7 SGI in words", ok && R::text(p.rates[0]) == QStringLiteral("72.2 Mbit/s · 11n MCS7 SGI")
          && p.rates[0].frames == 17, ok ? R::text(p.rates[0]) : QString());
    check("link rate: the long guard interval", ok && R::text(p.rates[1]) == QStringLiteral("65.0 Mbit/s · 11n MCS7"),
          ok ? R::text(p.rates[1]) : QString());
    // LR (the module session's calibration 2026-10-02): code 0x1A is 500 kbit/s, sent as a number (flag bit2) only
    // where the frames' timing proves it; another code is not known
    R::Rate lr; lr.kind = R::KindLr; lr.code = 0x1A; lr.kbps = 0; lr.frames = 30;
    R::Page q;
    check("link rate: LR 0x1A without a number is 500 kbit/s by the code",
          R::parse(ratePage(0, 1, boat, 200, 30, -60, 0, { lr }), q) && q.rates.size() == 1 && q.rates[0].kbps == 500
              && R::text(q.rates[0]) == QStringLiteral("0.5 Mbit/s · LR"),
          q.rates.isEmpty() ? QString() : R::text(q.rates[0]));
    R::Rate lrProven = lr; lrProven.flags = 0x04; lrProven.kbps = 500;
    R::Page q2;
    check("link rate: LR proven by the frames' timing (0.19, flag bit2)",
          R::parse(ratePage(0, 1, boat, 200, 30, -60, 0, { lrProven }), q2) && q2.rates.size() == 1
              && q2.rates[0].kbps == 500 && R::text(q2.rates[0]) == QStringLiteral("0.5 Mbit/s · LR"),
          q2.rates.isEmpty() ? QString() : R::text(q2.rates[0]));
    R::Rate lrOther = lr; lrOther.code = 0x1B;
    R::Page q3;
    check("link rate: LR with another code is not known",
          R::parse(ratePage(0, 1, boat, 200, 30, -60, 0, { lrOther }), q3) && q3.rates.size() == 1 && q3.rates[0].kbps == 0
              && R::text(q3.rates[0]) == QStringLiteral("LR, rate not known (code 0x1B)"),
          q3.rates.isEmpty() ? QString() : R::text(q3.rates[0]));
    R::Rate gCode = lr; gCode.kind = R::Kind11g;
    R::Page q4;
    check("link rate: code 0x1A of another kind is not taken for LR's 500",
          R::parse(ratePage(0, 1, boat, 200, 30, -60, 2, { gCode }), q4) && q4.rates.size() == 1 && q4.rates[0].kbps == 0,
          q4.rates.isEmpty() ? QString() : R::text(q4.rates[0]));
    R::Page none;
    check("link rate: no other end is {0, 0}", R::parse(QByteArray("\x00\x00", 2), none) && none.total == 0);
    R::Page cut;
    QByteArray shortPage = ratePage(0, 1, boat, 200, 21, -43, 4, { sgi, lgi });
    shortPage.chop(5);
    check("link rate: a cut page is refused", !R::parse(shortPage, cut));
    check("link rate: from firmware 0.18", R::supportFor(0, 17) == 0 && R::supportFor(0, 18) == 1 && R::supportFor(1, 0) == 1
          && R::supportFor(0, 0) == -1);
}

// TrafficTally: what a link carries, for the log.
void trafficRules()
{
    using T = TrafficTally;
    T t;
    T::Key proxy; proxy.proto = T::ProtoKp2; proxy.addr = 250; proxy.id = 0x40; proxy.proxy = true;
    T::Key att; att.proto = T::ProtoMavlink2; att.id = 30;
    T::Key wifi; wifi.proto = T::ProtoKp1; wifi.addr = 87; wifi.id = 0x57; wifi.ver = 1;
    for (int i = 0; i < 100; ++i) {
        t.add(proxy, 60, false);        // a proxy frame on the wire ...
        t.add(att, 40, true);           // ... carrying a MAVLink ATTITUDE: in the proxy's bytes already
    }
    for (int i = 0; i < 50; ++i)
        t.add(wifi, 35, false);
    check("traffic: nested frames are not added to the total", t.totalBytes() == 100 * 60 + 50 * 35,
          QString::number(t.totalBytes()));
    const QString s = t.summary(10.0);
    check("traffic: the summary names the heaviest first, what proxies carry apart",
          s.startsWith(QStringLiteral("0.76 KB/s: KP2 [250] STREAM v0 proxy 0.59 KB/s 10.0/s; KP1 [87] WIFI v1 0.17 KB/s 5.0/s"))
              && s.contains(QStringLiteral(" | in proxy frames: MAVLink2 msg 30 0.39 KB/s 10.0/s")), s);
    check("traffic: the summary starts over", t.empty() && t.totalBytes() == 0);
    T::Key none; none.proto = T::ProtoKp2; none.addr = -1; none.id = 0x1234; none.ver = 7; none.stream = true;
    check("traffic: a key without an address and a long ID keeps them",
          T::name(none) == QStringLiteral("KP2 [-1] id 0x1234 v7 stream"), T::name(none));
    T::Key um; um.proto = T::ProtoKp2; um.addr = 2; um.id = 0x7D; um.stream = true; um.unmarked = true;
    T::Key mk = um; mk.unmarked = false;
    T t2;
    t2.add(um, 100, false);
    t2.add(mk, 50, false);
    const QString s2 = t2.summary(1.0);
    check("traffic: frames without the mark are counted apart",
          s2.contains(QStringLiteral("KP2 [2] CHART_Z v0 stream unmarked 0.10 KB/s 1.0/s"))
              && s2.contains(QStringLiteral("KP2 [2] CHART_Z v0 stream 0.05 KB/s 1.0/s")), s2);
}

// A device without the mark (the search address 0 on a relay link, a device whose frames carry the mark only now and
// then) is asked at 5 Hz for 10 s, then every 5 s; 2 s of marked ticks or a frame after 1 s of silence start it over.
void unansweredRules()
{
    using U = DeviceLiveness::Unanswered;
    const int64_t t0 = 100000;
    {
        U u;
        int asks = 0;
        for (int64_t now = t0; now < t0 + 60000; now += 200)   // a minute of process() ticks, nothing from it
            asks += u.askDue(now) ? 1 : 0;
        // 10 s at 5 Hz = 50 asks, then one every 5 s over the other 50 s = 10
        check("unanswered: 5 Hz for 10 s, then every 5 s", asks == 60, QString::number(asks));
    }
    {
        U u;
        int asks = 0, late = 0;
        for (int64_t now = t0, i = 0; now < t0 + 60000; now += 200, ++i) {   // frames all the time, the mark
            u.heard(now);                                                   // on every other tick
            if (i % 2) {
                u.marked(now);
            } else if (u.askDue(now)) {
                ++asks;
                late += now >= t0 + 50000 ? 1 : 0;
            }
        }
        // 25 asks in the first 10 s, then every 5 s at the next unmarked tick (9); starting over on each frame asked all 150
        check("unanswered: frames whose mark comes and goes are slowed down too", asks == 34 && late == 2,
              QStringLiteral("%1 asks, %2 in the last 10 s").arg(asks).arg(late));
    }
    {
        U u;
        int64_t now = t0;
        for (; now < t0 + 20000; now += 200)                 // 20 s unanswered: slow
            u.askDue(now);
        for (const int64_t end = now + 2200; now < end; now += 200)   // 2.2 s of marked ticks: answers steadily
            u.marked(now);
        check("unanswered: 2 s of marks start it over, the next loss is asked at once and fast",
              u.askDue(now) && u.askDue(now + 200) && u.askDue(now + 400));
    }
    {
        U u;
        int64_t now = t0;
        for (; now < t0 + 20000; now += 200)                 // the last ask at t0 + 19800
            u.askDue(now);
        for (const int64_t end = now + 1200; now < end; now += 200)   // marked for 1 s only
            u.marked(now);
        check("unanswered: 1 s of marks does not start it over", !u.askDue(now));
    }
    {
        U u;
        int64_t now = t0;
        for (; now < t0 + 20000; now += 200)                 // 20 s of nothing: slow, the last ask at t0 + 19800
            u.askDue(now);
        const bool slow = !u.askDue(now);                     // t0 + 20000
        u.heard(now);                                         // the first frame (the baud rate search found it)
        check("unanswered: a first frame starts it over, asked at once and fast",
              slow && u.askDue(now + 200) && u.askDue(now + 400) && u.askDue(now + 600));
    }
    {
        U u;
        int64_t now = t0;
        for (; now < t0 + 20000; now += 200) {               // frames every tick, never the mark: slow
            u.heard(now);
            u.askDue(now);
        }
        const bool slow = !u.askDue(now);
        u.heard(now + 1200);                                  // 1.2 s without a frame, then one: a device back
        check("unanswered: a frame after 1 s of silence starts it over", slow && u.askDue(now + 1400));
    }
    check("unanswered: the first tick asks", U().askDue(t0));
}

// The line's search (address 0's driver while it has never had a frame) waits while another device of the line has
// been heard within kStaleMs, after its first kSearchFirstAsks asks of the connection; a device at address 0 is not
// held; a quiet line is searched again.
void searchHoldRules()
{
    using L = DeviceLiveness;
    const int64_t t0 = 100000;
    const int first = L::kSearchFirstAsks;
    check("search: held while another device was heard 1 s ago", L::searchHeld(0, t0, first, t0 + 1000));
    check("search: asks again after 3 s of a quiet line", !L::searchHeld(0, t0, first, t0 + L::kStaleMs));
    check("search: asks while no other device was ever heard", !L::searchHeld(0, 0, first, t0));
    check("search: a device at address 0 (it had frames) is never held", !L::searchHeld(t0, t0 + 900, first, t0 + 1000));
    check("search: the first asks of a connection are not held", !L::searchHeld(0, t0, 0, t0 + 100)
                                                                    && !L::searchHeld(0, t0, first - 1, t0 + 100));
    // The search's ticks every 200 ms from t0 against a line whose other devices send a frame every 50 ms from
    // lineFrom until lineTo; asks counted in the first second, while the line answers, and after it went quiet.
    struct Asks { int first = 0, during = 0, after = 0; };
    auto run = [&](int64_t lineFrom, int64_t lineTo) {
        DeviceLiveness::Unanswered u;
        Asks a;
        int made = 0;
        int64_t other = 0;
        for (int64_t now = t0 - 1000; now < t0 + 60000; now += 50) {
            if (now >= lineFrom && now < lineTo) {
                other = now;
            }
            if (now < t0 || (now - t0) % 200 != 0 || L::searchHeld(u.lastFrameMs, other, made, now) || !u.askDue(now)) {
                continue;
            }
            ++made;
            (now < t0 + 1000 ? a.first : now < lineTo ? a.during : a.after) += 1;
        }
        return a;
    };
    {
        // A GODIO ring: its two boards answer from t0 + 100 ms. The first asks; none while it answers; the line quiet
        // from t0 + 40 s: asked again from t0 + 43 s, every 5 s by then (its first ask was 43 s ago): 43, 48, 53, 58 s.
        const Asks a = run(t0 + 100, t0 + 40000);
        check("search: a ring heard: the first asks, no broadcast while it answers, asked again when it is gone",
              a.first == first && a.during == 0 && a.after == 4,
              QStringLiteral("first second %1, while answering %2, after %3").arg(a.first).arg(a.during).arg(a.after));
    }
    {
        // The logger (it answers, sends nothing by itself) with a sonar streaming through it before the search's first
        // tick (2026-10-28): the search still asks on connecting, so the logger answers and is found.
        const Asks a = run(t0 - 150, t0 + 40000);
        check("search: a sonar streaming through the logger before the first tick: the logger is still asked",
              a.first == first && a.during == 0,
              QStringLiteral("first second %1, while the sonar streams %2").arg(a.first).arg(a.during));
    }
}

// When a link asks every device on it (LinkAskAll, Link::onCheckedTimerEnd each 100 ms).
void linkAskAllRules()
{
    const int ticks = 600;   // a minute
    {
        LinkAskAll a;
        int asks = 0;
        for (int i = 0; i < ticks; ++i)                        // the boat link: frames every tick
            asks += a.tick(true, true, false) ? 1 : 0;
        check("link ask-all: a busy link is not asked", asks == 0, QString::number(asks));
    }
    {
        LinkAskAll a;
        int asks = 0;
        bool answer = false;
        for (int i = 0; i < ticks; ++i) {                      // a quiet sonar: only the answers to the asks come
            const bool ask = a.tick(answer, true, false);
            answer = ask;                                      // answered by the next tick
            asks += ask ? 1 : 0;
        }
        // kept alive as upstream: every requestAllCntBig ticks, 300 ms
        check("link ask-all: a quiet link is kept alive every 300 ms", asks == ticks / requestAllCntBig - 1,
              QString::number(asks));
    }
    {
        LinkAskAll a;
        int asks = 0;
        for (int i = 0; i < ticks; ++i)                        // nothing comes in: the search
            asks += a.tick(false, false, false) ? 1 : 0;
        check("link ask-all: nothing received, every tick", asks == ticks, QString::number(asks));
    }
    {
        LinkAskAll a;
        a.restart(requestAllCntSmall);
        int asks = 0;
        for (int i = 0; i < ticks; ++i)                        // a firmware update on a busy link: as upstream
            asks += a.tick(true, true, true) ? 1 : 0;
        check("link ask-all: a firmware update keeps upstream's cadence (every tick after the first)", asks == ticks - 1,
              QString::number(asks));
    }
    {
        LinkAskAll a;
        for (int i = 0; i < 50; ++i)
            a.tick(true, true, false);                         // busy ...
        int first = -1;
        for (int i = 0; i < 10 && first < 0; ++i)              // ... then silent
            if (a.tick(false, true, false))
                first = i;
        check("link ask-all: a link gone quiet is asked within 3 ticks", first >= 1 && first <= 3,
              QString::number(first));
    }
}

// A serial link's baud rate search (LinkRateSearch): it never sends below a line's rate before that rate was tried.
void linkRateSearchRules()
{
    const QList<uint32_t> list(baudrateSearchList.cbegin(), baudrateSearchList.cend());
    const QList<uint32_t> order{ 921600, 460800, 230400, 115200, 38400, 9600 };
    {
        LinkRateSearch s;
        s.setRates(list);
        QStringList got;
        bool ok = true;
        for (int pass = 0; pass < 3; ++pass)
            for (uint32_t r : order) {
                const LinkRateSearch::Step st = s.next();
                got << QStringLiteral("%1%2").arg(st.rate).arg(st.asks ? QStringLiteral("a") : QString());
                ok = ok && st.rate == r && st.asks == (pass == 1);
            }
        check("rate search: once each, fastest first; a listening pass, an asking one, listening again", ok,
              got.join(QLatin1Char(' ')));
    }
    // A line at each rate of the list, with a device that talks unasked (a ring) or only answers (a quiet one).
    for (const bool talks : { true, false }) {
        bool ok = true;
        QStringList bad;
        for (uint32_t line : order) {
            LinkRateSearch s;
            s.setRates(list);
            bool found = false;
            int sentBelow = 0, sent = 0;
            for (int i = 0; i < 100 && !found; ++i) {
                const LinkRateSearch::Step st = s.next();
                if (st.rate == line && (talks || st.asks))
                    found = true;                                 // heard, or answered
                else if (st.asks) {
                    ++sent;
                    if (st.rate < line)
                        ++sentBelow;
                }
            }
            const bool lineOk = found && sentBelow == 0 && (!talks || sent == 0);
            if (!lineOk)
                bad << QStringLiteral("%1: found %2, sent %3, below the line %4").arg(line).arg(found).arg(sent).arg(sentBelow);
            ok = ok && lineOk;
        }
        check(talks ? "rate search: a device that talks is found with nothing sent, at every rate"
                    : "rate search: a quiet device is asked only at rates above its own before it answers",
              ok, bad.isEmpty() ? QStringLiteral("all six rates") : bad.join(QStringLiteral("; ")));
    }
    {
        LinkRateSearch s;
        s.setRates(list);
        for (int i = 0; i < 8; ++i)
            s.next();                                             // into the asking pass
        s.reset();                                                // data came in
        const LinkRateSearch::Step st = s.next();
        LinkRateSearch none;
        none.setRates({});
        check("rate search: a reset starts a listening pass at the fastest rate; no rates, no step",
              st.rate == 921600 && !st.asks && none.empty() && none.next().rate == 0,
              QStringLiteral("%1 %2").arg(st.rate).arg(st.asks));
    }
}

// Mission progress (mission_progress.h): the way ahead, its time, the battery at its end, a boat along it.
void progressRules()
{
    namespace mp = mission_progress;
    const mp::Geo a{ 60.0, 30.0 }, b{ 60.001, 30.0 }, c{ 60.002, 30.0 };
    const double leg = mp::distanceM(a, b);
    check("progress: a degree of latitude is 111.2 km", std::abs(mp::distanceM({ 0, 0 }, { 1, 0 }) - 111195.0) < 50.0,
          QString::number(mp::distanceM({ 0, 0 }, { 1, 0 })));
    const QVector<mp::Geo> plan{ a, b, c };
    const mp::Geo home{ 59.999, 30.0 };
    const QVector<mp::Geo> ahead = mp::routeAhead(a, plan, 1, true, true, home);
    check("progress: the way ahead - the boat, the points from the target, home",
          ahead.size() == 4 && ahead[1].lat == b.lat && ahead[3].lat == home.lat);
    check("progress: no home when the mission does not return", mp::routeAhead(a, plan, 1, false, true, home).size() == 3);
    const double len = mp::lengthM(mp::routeAhead(a, plan, 1, false, true, home));
    check("progress: the way's length adds its legs", std::abs(len - 2 * leg) < 0.01, QString::number(len));
    check("progress: 100 m at 1 m/s is 100 s", std::abs(mp::etaS(100.0, 1.0) - 100.0) < 1e-9);
    check("progress: no time at a standstill", std::isnan(mp::etaS(100.0, 0.2)));
    bool ended = true;
    const mp::Geo p1 = mp::alongRoute(plan, 1.0, 50.0, &ended);
    check("progress: along the route after 50 s at 1 m/s - on the first leg",
          !ended && std::abs(mp::distanceM(a, p1) - 50.0) < 0.2, QString::number(mp::distanceM(a, p1)));
    const mp::Geo p2 = mp::alongRoute(plan, 1.0, leg + 30.0, &ended);
    check("progress: past the first point - on the second leg",
          !ended && std::abs(mp::distanceM(b, p2) - 30.0) < 0.2, QString::number(mp::distanceM(b, p2)));
    const mp::Geo p3 = mp::alongRoute(plan, 1.0, 10000.0, &ended);
    check("progress: past the end - stops at the last point", ended && p3.lat == c.lat && p3.lon == c.lon);
    check("progress: no speed - where it was", mp::alongRoute(plan, 0.0, 100.0).lat == a.lat);

    mp::SpeedSmoother sp;
    sp.add(0.0, 0);
    sp.add(2.0, 10000);
    check("progress: the speed settles with a 10 s time constant", std::abs(sp.value() - 2.0 * (1.0 - std::exp(-1.0))) < 1e-6,
          QString::number(sp.value()));

    mp::BatteryTrend bt;
    bt.add(0, 80);
    bt.add(30000, 79);
    check("progress: no battery trend within a minute", std::isnan(bt.ratePctPerS()) && bt.atEnd(600) == -1);
    bt.add(120000, 78);
    check("progress: 2 % in 120 s, 600 s more - 68 % at the end", bt.atEnd(600.0) == 68,
          QString::number(bt.atEnd(600.0)));
    check("progress: the end never below 0 %", bt.atEnd(1e6) == 0);
    mp::BatteryTrend none;
    none.add(0, -1);
    none.add(200000, -1);
    check("progress: no battery monitor (-1 %) - no trend", std::isnan(none.ratePctPerS()) && none.atEnd(60) == -1);
    mp::BatteryTrend flat;
    flat.add(0, 90);
    flat.add(300000, 90);
    check("progress: a charge that has not moved - no trend", flat.atEnd(60) == -1);
}

// The track by link quality (link_track.h).
void linkTrackRules()
{
    using T = LinkTrack;
    check("track: -50 good, -65 fair, -75 weak, -85 bad",
          T::qualityOf(-50) == T::Good && T::qualityOf(-65) == T::Fair && T::qualityOf(-75) == T::Weak && T::qualityOf(-85) == T::Bad);
    check("track: 0 (not connected) and none - unknown", T::qualityOf(0) == T::Unknown && T::qualityOf(T::kNoRssi) == T::Unknown);
    T t;
    const double m = 1.0 / 111195.0;              // a metre of latitude in degrees
    check("track: the first position", t.add(60.0, 30.0, -50, false) && t.samples().size() == 1);
    check("track: 1 m on, the same quality - skipped", !t.add(60.0 + m, 30.0, -52, false) && t.samples().size() == 1);
    check("track: 3 m on - added", t.add(60.0 + 3 * m, 30.0, -52, false) && t.samples().size() == 2);
    check("track: 1 m on, the quality changed - added",
          t.add(60.0 + 4 * m, 30.0, -75, false) && t.samples().last().quality == T::Weak);
    check("track: the first position after a loss - added as lost even close by",
          t.add(60.0 + 4.5 * m, 30.0, -50, true) && t.samples().last().quality == T::Lost);
    check("track: no position (0, 0 / NaN) - skipped", !t.add(0.0, 0.0, -50, false) && !t.add(qQNaN(), 30.0, -50, false));
    const int v = t.version();
    t.clear();
    check("track: cleared", t.samples().isEmpty() && t.version() != v);
    for (int i = 0; i < T::kMaxSamples + 100; ++i)
        t.add(60.0 + i * 3 * m, 30.0, -50, false);
    check("track: at most 3000 samples, the oldest dropped",
          t.samples().size() == T::kMaxSamples && std::abs(t.samples().first().lat - (60.0 + 100 * 3 * m)) < 1e-9);
}

// What the link carries, by kind (air_meter.h).
void airMeterRules()
{
    using A = AirMeter;
    using T = TrafficTally;
    T::Key chartZ; chartZ.proto = T::ProtoKp2; chartZ.id = 0x7D; chartZ.stream = true;
    T::Key chart; chart.proto = T::ProtoKp1; chart.id = 0x03;
    T::Key proxy; proxy.proto = T::ProtoKp2; proxy.id = 0x03; proxy.proxy = true;   // a proxy frame "CHART v4"
    T::Key mav; mav.proto = T::ProtoMavlink2; mav.id = 30;
    T::Key version; version.proto = T::ProtoKp1; version.id = 0x20;
    T::Key wifi; wifi.proto = T::ProtoKp1; wifi.id = 0x57;
    check("air: CHART_Z and CHART are the echogram", A::kindOf(chartZ) == A::Echogram && A::kindOf(chart) == A::Echogram);
    check("air: proxy frames (whatever their id reads) and MAVLink are telemetry",
          A::kindOf(proxy) == A::Telemetry && A::kindOf(mav) == A::Telemetry);
    check("air: VERSION, WIFI - service", A::kindOf(version) == A::Service && A::kindOf(wifi) == A::Service);

    A air;
    for (int sec = 0; sec < 5; ++sec)
        air.add(A::Echogram, 3072, sec * 1000 + 500);                 // 3 KB each second
    air.add(A::Telemetry, 1024, 4700);
    auto r = air.rates(5000);
    check("air: 3 KB/s of echogram over the last 3 whole seconds",
          std::abs(r[A::Echogram] - 3.0) < 1e-9, QString::number(r[A::Echogram]));
    check("air: 1 KB in one of 3 s - 0.33 KB/s", std::abs(r[A::Telemetry] - 1.0 / 3.0) < 1e-9);
    air.add(A::Service, 102400, 5200);
    r = air.rates(5500);
    check("air: the second under way is not counted yet", r[A::Service] == 0.0);
    r = air.rates(10000);
    check("air: quiet for 5 s - all zero", r[A::Echogram] == 0.0 && r[A::Telemetry] == 0.0 && r[A::Service] == 0.0);
}

QByteArray fakePosition(double lat, double lon, int vxCms)
{
    mavlink_message_t m;
    mavlink_msg_global_position_int_pack_chan(1, 1, kFakeChan, &m, 0, int32_t(std::lround(lat * 1e7)), int32_t(std::lround(lon * 1e7)),
                                              0, 0, int16_t(vxCms), 0, 0, 0);
    return frame(m);
}

QByteArray fakeMissionCurrent(int seq)
{
    mavlink_message_t m;
    mavlink_msg_mission_current_pack_chan(1, 1, kFakeChan, &m, uint16_t(seq), 0, 0, 0, 0, 0, 0);
    return frame(m);
}

QByteArray fakeHome(double lat, double lon)
{
    mavlink_message_t m;
    const float q[4] = { 1, 0, 0, 0 };
    mavlink_msg_home_position_pack_chan(1, 1, kFakeChan, &m, int32_t(std::lround(lat * 1e7)), int32_t(std::lround(lon * 1e7)),
                                        0, 0, 0, 0, q, 0, 0, 0, 0);
    return frame(m);
}

// MavVehicle's progress, ghost and track (mission_progress.h, link_track.h), driven by frames and its second tick.
void vehicleProgressRules()
{
    using V = MavVehicle;
    V v(nullptr, false);
    const double homeLat = 60.0, homeLon = 30.0;
    const double boatLat = 60.001;                           // 111.2 m north of home
    v.feed(fakeHeartbeat(true, V::ModeRtl));
    v.feed(fakeHeartbeat(true, V::ModeRtl));
    v.feed(fakeHome(homeLat, homeLon));
    v.setLinkRssi(-50);
    v.feed(fakePosition(boatLat, homeLon, 100));             // 1 m/s
    const bool valid = waitFor([&] { return v.missionProgressValid(); }, 2500);
    check("vehicle: RTL - progress to home", valid && std::abs(v.missionRemainingM() - 111.2) < 0.5,
          QString::number(v.missionRemainingM()));
    check("vehicle: RTL at 1 m/s - about 111 s left", std::abs(v.missionEtaS() - 111.2) < 1.0, QString::number(v.missionEtaS()));
    check("vehicle: no battery trend yet", v.missionBatteryAtEnd() == -1);
    check("vehicle: no ghost while the link is up", !v.ghostValid());
    // the track: the first sample, then a weak signal 5 m on
    v.setLinkRssi(-75);
    v.feed(fakeHeartbeat(true, V::ModeRtl));
    v.feed(fakePosition(boatLat - 5.0 / 111195.0, homeLon, 100));
    waitFor([&] { return v.linkTrack().size() == 2; }, 1500);
    const QVariantList t1 = v.linkTrack();
    check("vehicle: the track - two samples, the second weak",
          t1.size() == 2 && t1.last().toMap().value("q").toInt() == LinkTrack::Weak, QString::number(t1.size()));
    // the link goes: no frames for kConnectedMs, then the ghost walks home at 1 m/s
    const bool ghost = waitFor([&] { return v.ghostValid(); }, 6000);
    const double lastLat = boatLat - 5.0 / 111195.0;
    const double moved = V::distanceM(lastLat, homeLon, v.ghostLatitude(), v.ghostLongitude());
    const double age = v.ghostAgeS();
    check("vehicle: link lost - the ghost appears and moves home", ghost && v.ghostMoving(),
          QStringLiteral("age %1 s, moved %2 m").arg(age).arg(moved));
    check("vehicle: the ghost - about 1 m per second since the last position", std::abs(moved - age) <= 1.5,
          QStringLiteral("age %1 s, moved %2 m").arg(age).arg(moved));
    check("vehicle: the ghost - towards home, not away", v.ghostLatitude() < lastLat);
    // back: the ghost goes, the next position is a lost stretch on the track
    v.feed(fakeHeartbeat(true, V::ModeRtl));
    v.feed(fakePosition(lastLat - 1.0 / 111195.0, homeLon, 100));
    const bool gone = waitFor([&] { return !v.ghostValid(); }, 1500);
    waitFor([&] { return v.linkTrack().size() == 3; }, 1500);
    const QVariantList t2 = v.linkTrack();
    check("vehicle: link back - no ghost", gone);
    check("vehicle: the stretch over the loss is marked lost",
          t2.size() == 3 && t2.last().toMap().value("q").toInt() == LinkTrack::Lost, QString::number(t2.size()));
    v.clearLinkTrack();
    check("vehicle: the track cleared", v.linkTrack().isEmpty());
    // disarmed: no mission runs
    v.feed(fakeHeartbeat(false, V::ModeRtl));
    const bool stopped = waitFor([&] { return !v.missionProgressValid(); }, 2500);
    check("vehicle: disarmed - no progress", stopped);
}

// The system files' parsers (system_stats.h), on samples shaped like the head unit's (RK3588, 8 cores).
void systemStatsRules()
{
    namespace st = system_stats;
    const QString stat1 = QStringLiteral(
        "cpu  1000 0 500 8000 100 0 20 0 0 0\n"
        "cpu0 100 0 50 1000 10 0 5 0 0 0\n"
        "cpu1 200 0 100 900 10 0 5 0 0 0\n"
        "intr 12345\nctxt 999\n");
    const QString stat2 = QStringLiteral(
        "cpu  1100 0 550 8100 100 0 20 0 0 0\n"
        "cpu0 175 0 50 1025 10 0 5 0 0 0\n"   // +75 busy, +25 idle: 75 %
        "cpu1 200 0 100 1000 10 0 5 0 0 0\n"  // +0 busy, +100 idle: 0 %
        "intr 12345\n");
    const auto a = st::parseProcStat(stat1);
    const auto b = st::parseProcStat(stat2);
    check("system: /proc/stat - the cores, not the summary line", a.size() == 2 && b.size() == 2, QString::number(a.size()));
    const auto l = st::loads(a, b);
    check("system: a core's load between two samples", l.size() == 2 && std::abs(l[0] - 0.75) < 1e-9 && l[1] == 0.0,
          QStringLiteral("%1 %2").arg(l.value(0)).arg(l.value(1)));
    check("system: no earlier sample - 0", st::loads({}, b).value(0) == 0.0);
    qint64 total = 0, avail = 0;
    const bool mem = st::parseMeminfo(QStringLiteral("MemTotal:       16337488 kB\nMemFree:  1000 kB\nMemAvailable:   13104512 kB\n"),
                                      &total, &avail);
    check("system: /proc/meminfo - total and available", mem && total == 16337488 && avail == 13104512);
    check("system: /proc/meminfo without MemAvailable - no", !st::parseMeminfo(QStringLiteral("MemTotal: 100 kB\n"), &total, &avail));
    check("system: eMMC life_time 0x01 0x02 - wear up to 20 %", st::emmcWearPercent(QStringLiteral("0x01 0x02\n")) == 20);
    check("system: eMMC beyond its rated life (0x0B) - 110", st::emmcWearPercent(QStringLiteral("0x0B 0x01")) == 110);
    check("system: no life_time - unknown", st::emmcWearPercent(QString()) == -1 && st::emmcWearPercent(QStringLiteral("0x00 0x00")) == -1);
    check("system: thermal zones by type",
          st::thermalKind(QStringLiteral("bigcore0-thermal")) == st::ThermalCpu
              && st::thermalKind(QStringLiteral("littlecore-thermal")) == st::ThermalCpu
              && st::thermalKind(QStringLiteral("package-thermal")) == st::ThermalCpu
              && st::thermalKind(QStringLiteral("soc-thermal")) == st::ThermalCpu
              && st::thermalKind(QStringLiteral("x86_pkg_temp")) == st::ThermalCpu
              && st::thermalKind(QStringLiteral("gpu-thermal")) == st::ThermalGpu
              && st::thermalKind(QStringLiteral("npu-thermal")) == st::ThermalNpu
              && st::thermalKind(QStringLiteral("acpitz")) == st::ThermalOther
              && st::thermalKind(QStringLiteral("center-thermal")) == st::ThermalOther);
    check("system: a volume's disk",
          st::diskOf(QStringLiteral("/dev/mmcblk0p1")) == QLatin1String("mmcblk0")
              && st::diskOf(QStringLiteral("/dev/mmcblk1")) == QLatin1String("mmcblk1")
              && st::diskOf(QStringLiteral("/dev/nvme0n1p3")) == QLatin1String("nvme0n1")
              && st::diskOf(QStringLiteral("/dev/nvme0n1")) == QLatin1String("nvme0n1")
              && st::diskOf(QStringLiteral("/dev/sda1")) == QLatin1String("sda")
              && st::diskOf(QStringLiteral("/dev/mapper/root")).isEmpty() && st::diskOf(QStringLiteral("C:")).isEmpty(),
          st::diskOf(QStringLiteral("/dev/mmcblk0p1")) + QLatin1Char(' ') + st::diskOf(QStringLiteral("/dev/nvme0n1p3")));
    check("system: a disk's kind",
          st::diskKind(QStringLiteral("mmcblk0"), QStringLiteral("MMC\n"), false) == QLatin1String("eMMC")
              && st::diskKind(QStringLiteral("mmcblk1"), QStringLiteral("SD"), true) == QLatin1String("SD")
              && st::diskKind(QStringLiteral("nvme0n1"), QString(), false) == QLatin1String("NVMe")
              && st::diskKind(QStringLiteral("sda"), QString(), true) == QLatin1String("USB")
              && st::diskKind(QStringLiteral("sda"), QString(), false).isEmpty());
    check("system: a volume's name and the mounts left out",
          st::volumeName(QStringLiteral("/")) == QLatin1String("/") && st::volumeName(QStringLiteral("/data")) == QLatin1String("/data")
              && st::volumeName(QStringLiteral("/media/orangepi/KINGSTON")) == QLatin1String("KINGSTON")
              && st::hiddenMount(QStringLiteral("/boot")) && st::hiddenMount(QStringLiteral("/boot/efi"))
              && !st::hiddenMount(QStringLiteral("/")) && !st::hiddenMount(QStringLiteral("/bootstrap")));
    check("system: eMMC life_time as the head unit reads it (0x01 0x00) - up to 10 %",
          st::emmcWearPercent(QStringLiteral("0x01 0x00\n")) == 10);
    const st::DrmClient kiosk = st::parseDrmFdinfo(QStringLiteral(
        "pos:\t0\nflags:\t02100002\nmnt_id:\t26\nino:\t1060\ndrm-driver:\tpanthor\ndrm-client-id:\t9\n"
        "drm-engine-panthor:\t5165417 ns\ndrm-maxfreq-panthor:\t1000000000 Hz\ndrm-curfreq-panthor:\t300000000 Hz\n"
        "drm-total-memory:\t127676 KiB\ndrm-shared-memory:\t24504 KiB\ndrm-resident-memory:\t127676 KiB\n"));
    check("system: a DRM client's fdinfo (the kiosk's: panthor, profiling on)",
          kiosk.id == 9 && kiosk.driver == QLatin1String("panthor") && kiosk.residentKiB == 127676
              && kiosk.engineNs.value(QStringLiteral("panthor")) == 5165417 && kiosk.engineNs.size() == 1,
          QStringLiteral("id %1, %2 ns").arg(kiosk.id).arg(kiosk.engineNs.value(QStringLiteral("panthor"))));
    check("system: an fdinfo that is no DRM client's",
          st::parseDrmFdinfo(QStringLiteral("pos:\t0\nflags:\t02\nmnt_id:\t15\n")).id == -1);
    const QHash<QString, quint64> was{ { QStringLiteral("panthor"), 5165417 } };
    const QHash<QString, quint64> now{ { QStringLiteral("panthor"), 15511417 } };
    const double busy = st::drmBusy(was, now, {}, 2087000000LL);
    check("system: the GPU's busy share (10.346 ms in 2.087 s)", std::abs(busy - 10346000.0 / 2087000000.0) < 1e-12,
          QString::number(busy, 'f', 5));
    check("system: no engine time, or no time passed - unknown",
          st::drmBusy({}, now, {}, 1000000000LL) == -1.0 && st::drmBusy(was, now, {}, 0) == -1.0
              && st::drmBusy(now, was, {}, 1000000000LL) == -1.0);
    const QHash<QString, quint64> twoWas{ { QStringLiteral("gfx"), 0 }, { QStringLiteral("compute"), 0 } };
    const QHash<QString, quint64> twoNow{ { QStringLiteral("gfx"), 500000000 }, { QStringLiteral("compute"), 1600000000 } };
    const double busiest = st::drmBusy(twoWas, twoNow, { { QStringLiteral("compute"), 2 } }, 1000000000LL);
    check("system: the busiest engine class, over its engines", std::abs(busiest - 0.8) < 1e-12, QString::number(busiest));
}

// SystemMonitor on this computer, live: whatever it is, it has cores, memory, a volume and an uptime.
void systemMonitorLiveRules()
{
    SystemMonitor m;
    m.subscribe();
    waitFor([&] { return m.coreLoad().size() > 0 && !m.volumes().isEmpty(); }, 2500);
    QThread::msleep(1100);
    QCoreApplication::processEvents();
    const QString seen = QStringLiteral("%1 cores, max MHz %2, now %3 MHz, load %4, memory %5/%6 GB, %7 volumes, uptime %8 s, CPU %9 C")
        .arg(m.coreLoad().size()).arg(m.coreMaxMHz().value(0)).arg(m.cpuFreqMHz()).arg(m.cpuLoad(), 0, 'f', 2)
        .arg(m.memUsedGB(), 0, 'f', 1).arg(m.memTotalGB(), 0, 'f', 1).arg(m.volumes().size()).arg(m.uptimeS()).arg(m.cpuTempC());
    check("system live: available, cores, memory, a volume, the uptime",
          m.available() && m.coreLoad().size() >= 1 && m.memTotalGB() > 0.5 && m.memUsedGB() > 0.0
              && m.memUsedGB() < m.memTotalGB() && !m.volumes().isEmpty() && m.uptimeS() > 0, seen);
    check("system live: each core's frequency and maximum", m.coreFreqMHz().size() == m.coreLoad().size()
              && m.coreMaxMHz().value(0) > 0 && m.cpuFreqMHz() > 0, seen);
    check("system live: loads 0..1", std::all_of(m.coreLoad().cbegin(), m.coreLoad().cend(), [](double l) { return l >= 0.0 && l <= 1.0; }) && m.cpuLoad() >= 0.0, seen);
    m.unsubscribe();
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCommandLineParser cli;
    cli.addHelpOption();
    cli.addOption({ QStringLiteral("host"), QStringLiteral("SITL host"), QStringLiteral("host"), QStringLiteral("127.0.0.1") });
    cli.addOption({ QStringLiteral("port"), QStringLiteral("SITL TCP port"), QStringLiteral("port"), QStringLiteral("5760") });
    cli.addOption({ QStringLiteral("wall-timeout-scale"), QStringLiteral("multiply every wait by this"), QStringLiteral("k"),
                    QStringLiteral("1.0") });
    cli.addOption({ QStringLiteral("offline"), QStringLiteral("only the checks on made-up events (no SITL needed)") });
    cli.process(app);
    g_scale = qMax(0.1, cli.value(QStringLiteral("wall-timeout-scale")).toDouble());

    navRules();
    sequenceAndLogRules();
    wifiOpsRules();
    wifiPortsRules();
    wifiRadioRules();
    fieldTestRules();
    channelRules();
    surveyRules();
    linkLogRules();
    homeRules();
    plannerModeRules();
    plannerGhostRules();
    headingInUseRules();
    linkRateRules();
    trafficRules();
    unansweredRules();
    searchHoldRules();
    linkAskAllRules();
    linkRateSearchRules();
    progressRules();
    linkTrackRules();
    airMeterRules();
    vehicleProgressRules();
    systemStatsRules();
    systemMonitorLiveRules();
    if (cli.isSet(QStringLiteral("offline"))) {
        std::printf("\n%d checks, %d failed (offline)\nMISSION_CHECK_%s\n", g_checks, g_failed, g_failed ? "FAILED" : "DONE");
        return g_failed ? 1 : 0;
    }

    QTcpSocket sock;
    MavVehicle v(nullptr, false);
    // refuseAuto: the transport keeps DO_SET_MODE to AUTO from SITL and answers it with COMMAND_ACK FAILED, which is
    // what the real boat did when its EKF had no position ("Flight mode change failed"); HOLD needs no position.
    bool refuseAuto = false;
    // blockRx: nothing from SITL reaches the vehicle (a dropped link). dropUploadAck: the first MISSION_ACK from
    // the autopilot is lost (the upload's final one), everything else passes frame by frame.
    bool blockRx = false;
    bool dropUploadAck = false;
    int acksDropped = 0;
    mavlink_message_t inMsg;
    mavlink_status_t inStatus{};
    QObject::connect(&sock, &QTcpSocket::readyRead, &v, [&]() {
        const QByteArray bytes = sock.readAll();
        if (blockRx)
            return;
        if (!dropUploadAck) {
            v.feed(bytes);
            return;
        }
        for (char c : bytes) {
            if (!mavlink_parse_char(kFakeChan, uint8_t(c), &inMsg, &inStatus))
                continue;
            if (inMsg.msgid == MAVLINK_MSG_ID_MISSION_ACK && acksDropped == 0) {
                ++acksDropped;
                continue;
            }
            v.feed(frame(inMsg));
        }
    });
    QObject::connect(&v, &MavVehicle::sendBytes, &sock, [&](const QByteArray& b) {
        if (refuseAuto && isSetMode(b, MavVehicle::ModeAuto)) {
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
    // The link drops right after the autopilot is first heard (a Wi-Fi module restart did this on the head unit):
    // the settings read windows pass with nothing arriving; the settings have to be read once it is back.
    blockRx = true;
    check("link dropped right after the autopilot was found", waitFor([&] { return !v.connected(); }, 10000));
    waitFor([&] { return false; }, 40000);
    blockRx = false;
    check("link back", waitFor([&] { return v.connected(); }, 15000));
    check("GPS fix, position and home", waitFor([&] { return v.gpsFix() >= 3 && v.positionValid() && v.homeValid(); }, 120000),
          QStringLiteral("fix %1, home %2").arg(v.gpsFix()).arg(v.homeValid()));
    check("navigation ready (EKF status from the autopilot)", waitFor([&] { return v.navState() == MavVehicle::NavReady; }, 60000),
          v.navText());
    check("FS_EKF_THRESH read from the autopilot", waitFor([&] { return v.ekfThresholdKnown(); }, 30000));
    check("navigation settings read and logged", waitFor([&] { return v.settingsLogged(); }, 45000)
          && v.messages().join(QLatin1Char('\n')).contains(QStringLiteral("AHRS_EKF_TYPE"))
          && v.messages().join(QLatin1Char('\n')).contains(QStringLiteral("GPS_TYPE ")));
    check("GPS heading state known (SITL: no heading configured)",
          v.gpsHeadingState() == MavVehicle::GpsHeadingNotConfigured, v.gpsHeadingText());
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

    // The boat's mission back into the plan (the panel's "Load from boat").
    v.clearPlan();
    v.loadPlanFromVehicle();
    check("load from boat: the plan comes back from the boat's mission",
          waitFor([&] { return v.transferState() == MavVehicle::TransferDone
                               || v.transferState() == MavVehicle::TransferFailed; }, 30000)
              && v.transferState() == MavVehicle::TransferDone && v.planCount() == 3 && qAbs(v.planSpeed() - 2.0) < 1e-9
              && v.planReturnHome() && v.planOnVehicle(),
          v.transferText());

    // The autopilot's MISSION_ACK after the last item is lost: the read-back decides instead of a failure. The plan
    // is changed first, so a matching read-back proves the new mission is on the boat.
    v.movePoint(2, lat0 + 0.0001, lon0 + 0.0008);
    dropUploadAck = true;
    v.uploadPlan();
    const bool upDone = waitFor([&] { return v.transferState() == MavVehicle::TransferDone
                                             || v.transferState() == MavVehicle::TransferFailed; }, 40000);
    check("upload with the final MISSION_ACK lost: checked by reading back",
          upDone && acksDropped == 1 && v.transferState() == MavVehicle::TransferDone && v.planOnVehicle()
              && v.messages().join(QLatin1Char('\n')).contains(QStringLiteral("reading the mission back")),
          v.transferText());
    dropUploadAck = false;

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
    before = results;
    v.setHeadingSource(MavVehicle::HeadingGpsCompassFallback);
    check("heading source: refused while armed", results > before && !lastOk && v.headingSource() != 3, lastResult);
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

    // Heading source there and back (SITL starts with 1, compass).
    const int src0 = v.headingSource();
    const int other = src0 == MavVehicle::HeadingGpsCompassFallback ? MavVehicle::HeadingCompass
                                                                    : MavVehicle::HeadingGpsCompassFallback;
    before = results;
    v.setHeadingSource(other);
    check("heading source: written and confirmed by the autopilot",
          waitFor([&] { return results > before; }, 10000) && lastOk && v.headingSource() == other,
          QStringLiteral("%1 -> %2: %3").arg(src0).arg(v.headingSource()).arg(lastResult));
    before = results;
    v.setHeadingSource(src0 == MavVehicle::HeadingGpsCompassFallback ? src0 : MavVehicle::HeadingCompass);
    check("heading source: back again",
          waitFor([&] { return results > before; }, 10000) && lastOk && v.headingSource() == src0,
          QStringLiteral("now %1: %2").arg(v.headingSource()).arg(lastResult));

    // Field test 2026-09-28: a mission without the return home ends in AUTO ("Mission Complete"), and Start did not
    // run it again: the boat was in AUTO already, and ArduPilot starts a mission only when AUTO is entered.
    QStringList heard;   // the vehicle's messages from here on, oldest first (messages() is newest first and capped)
    QObject::connect(&v, &MavVehicle::messagesChanged, &v, [&]() { heard.append(v.messages().value(0)); });
    auto heardSince = [&](int from, const QString& text) {
        for (int i = from; i < heard.size(); ++i)
            if (heard[i].contains(text))
                return true;
        return false;
    };
    if (qEnvironmentVariableIsSet("MISSION_CHECK_REPRO_HOLD")) {   // one reproduction run: start from HOLD
        before = results;
        v.hold();
        waitFor([&] { return results > before; }, 10000);
    }
    v.clearPlan();
    v.addPoint(v.latitude() + 0.00015, v.longitude());   // about 17 m north
    v.setPlanSpeed(2.0);
    v.setPlanReturnHome(false);
    v.uploadPlan();
    check("one point, no return home: uploaded",
          waitFor([&] { return v.transferState() == MavVehicle::TransferDone || v.transferState() == MavVehicle::TransferFailed; }, 40000)
              && v.transferState() == MavVehicle::TransferDone && v.planOnVehicle(),
          v.transferText());
    int mark = heard.size();
    before = results;
    v.startMission();
    check("one point: started", waitFor([&] { return results > before; }, 20000) && lastOk && v.mode() == MavVehicle::ModeAuto,
          lastResult);
    check("one point: \"Mission Complete\", the boat stays in AUTO",
          waitFor([&] { return heardSince(mark, QStringLiteral("Mission Complete")); }, 120000) && v.mode() == MavVehicle::ModeAuto,
          QStringLiteral("mode %1").arg(v.modeName()));
    mark = heard.size();
    before = results;
    v.startMission();
    const bool again = waitFor([&] { return results > before; }, 20000);
    const QString againResult = lastResult;
    check("start again after the end: the mission runs from its first point",
          again && lastOk && waitFor([&] { return heardSince(mark, QStringLiteral("Mission: 2 WP")); }, 20000),
          againResult + QStringLiteral(" | heard: ") + heard.mid(mark).join(QStringLiteral(" / ")));
    before = results;
    v.hold();
    waitFor([&] { return results > before; }, 10000);
    before = results;
    v.disarm();
    check("disarmed at the end", waitFor([&] { return results > before; }, 10000) && !v.armed(), lastResult);

    std::printf("\n%d checks, %d failed\nMISSION_CHECK_%s\n", g_checks, g_failed, g_failed ? "FAILED" : "DONE");
    return g_failed ? 1 : 0;
}
