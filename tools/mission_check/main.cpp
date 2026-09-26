// mission_check - drives MavVehicle (the app's MAVLink vehicle class) through a whole boat mission against
// ArduPilot SITL over TCP and reports PASS/FAIL per step. Exit code 0 = every check passed.
//
//   mission_check [--host 127.0.0.1] [--port 5760] [--wall-timeout-scale 1.0]
//
// SITL (ArduRover, model motorboat) must already listen on the port; see docs/BOAT_MISSION.md.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpSocket>
#include <QThread>
#include <cstdio>
#include <functional>

#include "mav_vehicle.h"

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

    QTcpSocket sock;
    MavVehicle v(nullptr, false);
    QObject::connect(&sock, &QTcpSocket::readyRead, &v, [&]() { v.feed(sock.readAll()); });
    QObject::connect(&v, &MavVehicle::sendBytes, &sock, [&](const QByteArray& b) { sock.write(b); });
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

    check("start allowed", v.startBlocker().isEmpty(), v.startBlocker());
    int before = results;
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
