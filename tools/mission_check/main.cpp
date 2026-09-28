// mission_check - drives MavVehicle (the app's MAVLink vehicle class) through a whole boat mission against
// ArduPilot SITL over TCP and reports PASS/FAIL per step. Exit code 0 = every check passed.
//
//   mission_check [--host 127.0.0.1] [--port 5760] [--wall-timeout-scale 1.0] [--offline]
//
// SITL (ArduRover, model motorboat) must already listen on the port; see docs/BOAT_MISSION.md.
// Before SITL it checks the navigation-readiness rules on made-up EKF reports and the Wi-Fi module's
// sequences and port-page decoding on made-up frames (no SITL needed); --offline stops after those.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <functional>

#include "mav_vehicle.h"
#include "wifi_ops.h"

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
    check("port: refuses a speed above 5000000", !port->start(5000001, 2000000));
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
    // A module whose firmware does not take the speed refuses ID_UART (at the old speed, before switching).
    calls.clear();
    done = 0;
    check("port: 5M starts", port->start(5000000, 3000000) && calls.value(0) == QStringLiteral("module 5000000"));
    port->moduleRefused();
    check("port: refused -> ends at once, the link never moves", done == 1 && !ok && port->refused() && !port->active()
          && calls == QStringList{ QStringLiteral("module 5000000") }, calls.join(QStringLiteral(", ")));
    check("port rules: 0.12 -> 4M, 0.13 -> 5M, 1.0 -> 5M, not known -> 4M", WifiPortRules::maxBaud(0, 12) == 4000000
          && WifiPortRules::maxBaud(0, 13) == 5000000 && WifiPortRules::maxBaud(1, 0) == 5000000
          && WifiPortRules::maxBaud(0, 0) == 4000000);
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
    tooFast.baud = 5000001;
    check("line: refuses line 2, 5000001 and an empty edit", !line.start(2, rateEd) && !line.start(1, tooFast)
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
    if (cli.isSet(QStringLiteral("offline"))) {
        std::printf("\n%d checks, %d failed (offline)\nMISSION_CHECK_%s\n", g_checks, g_failed, g_failed ? "FAILED" : "DONE");
        return g_failed ? 1 : 0;
    }

    QTcpSocket sock;
    MavVehicle v(nullptr, false);
    // refuseAuto: the transport keeps DO_SET_MODE from SITL and answers it with COMMAND_ACK FAILED, which is
    // what the real boat did when its EKF had no position ("Flight mode change failed").
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

    std::printf("\n%d checks, %d failed\nMISSION_CHECK_%s\n", g_checks, g_failed, g_failed ? "FAILED" : "DONE");
    return g_failed ? 1 : 0;
}
