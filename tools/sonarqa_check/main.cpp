// Standalone check of the sonar acceptance module (src/sonar_qa; docs/SONAR_QA.md) against a simulated nanoSSS.
//
//  1. analysePing on hand-made pings: the ringing's end, the reflections and their repeats, a plateau, saturation.
//  2. Whole runs with the owner's sweep (150..900 kHz by 5, 10 pings, off and 25 pulses): the baseline of the
//     reference, sonars that pass and sonars with one fault each (weak, resonance moved, long ringing, noisy, low
//     supply) that must fail on that fault; another firmware and the reference itself refused as units.
//  3. The files: per serial number, the raw pings (every record carries the transmitter state the simulator had for
//     that very ping, so a ping taken before a change took effect is caught), the journal, the state.
//  4. The reference: a check that agrees resets the count, a drifted one blocks units until accepted; ten units per
//     check, the eleventh refused. A stopped run and a silent sonar leave no file and put the settings back.
// Exit 0 = pass, 1 = fail.
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <cstdio>

#include "sonar_qa_demo.h"
#include "sonar_qa_runner.h"

using namespace sonarqa;

// The application's factory, not linked here: the check gives the runner its simulated sonar directly.
SonarQaDevice* makeSonarQaDevice(QObject*, QObject*) { return nullptr; }

namespace {

int fails = 0;
void ok(const QString& name, bool good, const QString& detail = QString())
{
    std::printf("%s %-70s %s\n", good ? "ok  " : "FAIL", name.toUtf8().constData(), detail.toUtf8().constData());
    if (!good) {
        ++fails;
    }
}

using Model = SonarQaDemoDevice::Model;

// The simulated sonar with its pings tagged, so the raw file can be checked against what each ping was.
Model tagged()
{
    Model m;
    m.tagPings = true;
    return m;
}

// Runs one kind and waits for its end; false when it did not finish.
bool runOnce(SonarQaRunner& r, const QString& kind, QString* status = nullptr)
{
    bool finished = false, ended = false;
    auto c = QObject::connect(&r, &SonarQaRunner::runEnded, [&](bool f) { finished = f; ended = true; });
    if (r.start(kind)) {
        QElapsedTimer t;
        t.start();
        while (!ended && t.elapsed() < 120000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
    }
    QObject::disconnect(c);
    if (status) {
        *status = r.status();
    }
    return finished;
}

QString verdict(const SonarQaRunner& r) { return r.result().value("verdict").toString(); }

QString failedChecks(const SonarQaRunner& r)
{
    QStringList f;
    for (const QVariant& v : r.result().value("checks").toList()) {
        const QVariantMap c = v.toMap();
        if (c.value("level").toString() == QLatin1String("fail")) {
            f.append(c.value("id").toString() + (c.value("side").toInt() >= 0 ? QString::number(c.value("side").toInt())
                                                                              : QString()));
        }
    }
    f.removeDuplicates();
    return f.join(',');
}

// Every record of a raw file: its header's transmitter state against the one the simulator put in the ping.
bool rawConsistent(const QString& path, int* records, QString* detail)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *detail = QStringLiteral("cannot open");
        return false;
    }
    const QByteArray all = f.readAll();
    auto u = [&all](int at, int bytes) {
        quint32 v = 0;
        for (int i = 0; i < bytes; ++i) {
            v |= quint32(uchar(all[at + i])) << (8 * i);
        }
        return v;
    };
    if (!all.startsWith("KSQA") || u(4, 4) != 1) {
        *detail = QStringLiteral("bad header");
        return false;
    }
    int at = 12 + int(u(8, 4));
    *records = 0;
    int bad = 0;
    while (at + 7 <= all.size()) {
        const int freq = int(u(at, 2)), pulses = int(u(at + 2, 1)), n = int(u(at + 5, 2));
        if (at + 7 + n > all.size() || n < 2) {
            *detail = QStringLiteral("cut record");
            return false;
        }
        const int tagF = uchar(all[at + 7 + n - 2]) * 5, tagP = uchar(all[at + 7 + n - 1]);
        bad += (tagF != freq || tagP != pulses) ? 1 : 0;
        ++*records;
        at += 7 + n;
    }
    *detail = QStringLiteral("%1 records, %2 from another transmitter state").arg(*records).arg(bad);
    return bad == 0 && at == all.size();
}

void checkPings()
{
    std::printf("\nanalysePing\n");
    Params p;
    QVector<uint8_t> a(1000, 30);
    for (int i = 0; i < 40; ++i) {
        a[i] = uint8_t(230 - 5 * i);
    }
    a[200] = 200; a[199] = 190; a[201] = 190;
    a[400] = 175;
    a[600] = 150; a[601] = 150;          // a plateau: its first sample
    a[800] = 255; a[801] = 255;
    const PingMetrics m = analysePing(a.constData(), a.size(), true, p);
    ok("floor 30", m.floor == 30, QString::number(m.floor));
    ok("ringing ends where the bytes drop under the floor + 15 for 5 samples", m.ringdown >= 37 && m.ringdown <= 41,
       QString::number(m.ringdown));
    ok("ringing peak 230", m.ringPeak == 230);
    QStringList pos;
    for (const Echo& e : m.echoes) {
        pos.append(QStringLiteral("%1:%2").arg(e.pos).arg(e.amp));
    }
    ok("reflections at 200, 400, 600 (plateau's first), 800, nearest first",
       pos.join(' ') == QLatin1String("200:200 400:175 600:150 800:255"), pos.join(' '));
    ok("two saturated bytes", m.saturated == 2, QString::number(m.saturated));
    QVector<uint8_t> q(1000, 30);
    q[100] = 50;                          // under the floor + 25: not a reflection
    const PingMetrics mq = analysePing(q.constData(), q.size(), true, p);
    ok("a bump under the margin is no reflection; no ringing on a quiet ping", mq.echoes.isEmpty() && mq.ringdown == 0,
       QStringLiteral("%1 echoes, ringdown %2").arg(mq.echoes.size()).arg(mq.ringdown));
    const PingMetrics off = analysePing(q.constData(), q.size(), false, p);
    ok("transmitter off: no ringing, no reflections measured", off.ringdown == 0 && off.echoes.isEmpty());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    checkPings();

    std::printf("\nruns with the owner's sweep\n");
    QTemporaryDir dir;
    SonarQaDemoDevice sonar(false);
    sonar.m = tagged();
    SonarQaRunner r(dir.path());
    r.resetParams();
    r.setAdapter(&sonar);
    const Params defaults;
    ok("the owner's sweep: 151 steps, 150..900 by 5, 10 pings, 25 pulses, 50 ms, 2000 samples",
       defaults.stepCount() == 151 && defaults.freqAt(150) == 900 && defaults.pingsPerStep == 10
           && defaults.txPulses == 25 && defaults.periodMs == 50 && defaults.samples == 2000);

    QString why = r.canStart("unit");
    ok("no unit before a reference", !why.isEmpty(), why);
    why = r.canStart("check");
    ok("no check before a baseline", !why.isEmpty(), why);
    const QVariantMap before = sonar.settings;
    ok("baseline of the reference (1001)", runOnce(r, "baseline") && verdict(r) == QLatin1String("baseline"),
       r.status());
    ok("the sonar's settings put back", sonar.restored == before);
    const QString baseFile = QDir(dir.path()).filePath(r.result().value("file").toString());
    ok("saved under reference/1001", baseFile.contains("reference/1001/") && QFile::exists(baseFile), baseFile);
    int records = 0;
    QString detail;
    QString raw = baseFile;
    raw.replace(".json", ".ksqa");
    ok("raw pings: 151 steps x 2 states x 10 pings x 2 sides, none from another transmitter state",
       rawConsistent(raw, &records, &detail) && records == 151 * 2 * 10 * 2, detail);
    ok("reference serial 1001, 10 units allowed", r.reference().value("serial").toDouble() == 1001
                                                      && r.reference().value("unitsLeft").toInt() == 10);
    QFile bf(baseFile);
    bf.open(QIODevice::ReadOnly);
    const Run base = Run::fromJson(QJsonDocument::fromJson(bf.readAll()).object());
    const Resonance res = resonance(base, 0, 6.0);
    ok("baseline resonance near 600 kHz and its band seen", std::fabs(res.freqKhz - 600) <= 10 && res.bandwidthKhz > 100,
       QStringLiteral("%1 kHz, band %2 kHz").arg(res.freqKhz).arg(res.bandwidthKhz));
    ok("baseline supply and temperature kept", base.supply.valid() && std::fabs(base.supply.median - 12.0) < 0.1
                                                   && base.temperature.valid());
    ok("the reference as a unit is refused", !r.canStart("unit").isEmpty(), r.canStart("unit"));

    struct Case {
        quint32 serial;
        const char* what;
        std::function<void(Model&)> change;
        const char* expect;   // "" = pass, else a check id that must fail
    };
    const QVector<Case> cases = {
        { 2001, "a good sonar (1 dB weaker, resonance +5 kHz)", [](Model& m) { m.gainDb = -1; m.f0 += 5; }, "" },
        { 2002, "a weak sonar (-8 dB)", [](Model& m) { m.gainDb = -8; }, "echo_band_mean" },
        { 2003, "resonance moved +60 kHz", [](Model& m) { m.f0 += 60; }, "resonance" },
        { 2004, "ringing 2.5 times longer", [](Model& m) { m.ringScale = 2.5; }, "ringdown" },
        { 2005, "noisy receiver (+12 codes, ~5 dB)", [](Model& m) { m.floorOff += 12; }, "noise" },
        { 2007, "supply 1.5 V low", [](Model& m) { m.supply -= 1.5; }, "supply" },
    };
    for (const Case& c : cases) {
        sonar.m = tagged();
        sonar.m.serial = c.serial;
        c.change(sonar.m);
        r.refreshDevice();
        const bool done = runOnce(r, "unit");
        const QString failed = failedChecks(r);
        const bool good = done && (QString(c.expect).isEmpty() ? verdict(r) == QLatin1String("pass")
                                                               : verdict(r) == QLatin1String("fail")
                                                                     && failed.contains(c.expect));
        ok(QStringLiteral("%1: %2").arg(c.serial).arg(c.what), good,
           QStringLiteral("%1, failed: %2").arg(verdict(r), failed.isEmpty() ? QStringLiteral("none") : failed));
    }
    ok("files by serial: units/2001/..._pass.json, units/2002/..._fail.json",
       !QDir(dir.filePath("units/2001")).entryList({ "*_pass.json" }).isEmpty()
           && !QDir(dir.filePath("units/2002")).entryList({ "*_fail.json" }).isEmpty());

    sonar.m = tagged();
    sonar.m.serial = 2006;
    sonar.m.firmware = QStringLiteral("11.20");
    r.refreshDevice();
    why = r.canStart("unit");
    ok("another firmware refused", why.contains("11.20"), why);

    ok("6 units tested, 4 left", r.reference().value("unitsSinceCheck").toInt() == 6
                                     && r.reference().value("unitsLeft").toInt() == 4);
    sonar.m = tagged();
    r.refreshDevice();
    ok("the reference's check agrees with its baseline", runOnce(r, "check") && verdict(r) == QLatin1String("check_ok"),
       failedChecks(r));
    ok("the check resets the count", r.reference().value("unitsLeft").toInt() == 10);

    sonar.m.gainDb = -6;   // the stand changed (the target moved, water level...)
    ok("a drifted check is told so", runOnce(r, "check") && verdict(r) == QLatin1String("check_drift"), failedChecks(r));
    sonar.m = tagged();
    sonar.m.serial = 2010;
    r.refreshDevice();
    why = r.canStart("unit");
    ok("units blocked after a drifted check", !why.isEmpty(), why);
    r.acceptCheck();
    ok("... and allowed once it is accepted", r.canStart("unit").isEmpty(), r.canStart("unit"));

    std::printf("\nstopping\n");
    const int journalBefore = r.journal().size();
    sonar.restored.clear();
    bool ended = false;
    auto c = QObject::connect(&r, &SonarQaRunner::runEnded, [&](bool) { ended = true; });
    r.start("unit");
    QElapsedTimer t;
    t.start();
    while (r.progress() < 0.2 && t.elapsed() < 30000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    r.stop();
    QObject::disconnect(c);
    ok("a stopped run: nothing saved, the settings put back, no leftover raw file",
       ended && r.journal().size() == journalBefore && !sonar.restored.isEmpty()
           && !QFile::exists(QDir(dir.path()).filePath("running.ksqa")) && !QDir(dir.filePath("units/2010")).exists(),
       r.status());
    sonar.silent = true;
    QString st;
    const bool fin = runOnce(r, "unit", &st);
    ok("a silent sonar ends the run", !fin && st.contains("No echogram"), st);
    sonar.silent = false;

    std::printf("\none check per ten units\n");
    QTemporaryDir dir2;
    SonarQaDemoDevice s2(false);
    s2.m = tagged();
    SonarQaRunner r2(dir2.path());
    QVariantMap p = r2.params();
    p["freqFromKhz"] = 400;
    p["freqToKhz"] = 800;
    p["freqStepKhz"] = 50;
    p["pingsPerStep"] = 3;
    p["discardPings"] = 1;
    r2.setParams(p);
    r2.setAdapter(&s2);
    runOnce(r2, "baseline");
    int passed = 0;
    for (int i = 0; i < 10; ++i) {
        s2.m = tagged();
        s2.m.serial = 3000 + i;
        r2.refreshDevice();
        passed += runOnce(r2, "unit") && verdict(r2) == QLatin1String("pass") ? 1 : 0;
    }
    ok("ten units pass against one check", passed == 10, QString::number(passed));
    s2.m.serial = 3010;
    r2.refreshDevice();
    why = r2.canStart("unit");
    ok("the eleventh is refused until the reference is checked", why.contains("10 sonars"), why);
    s2.m = tagged();
    r2.refreshDevice();
    runOnce(r2, "check");
    s2.m.serial = 3010;
    r2.refreshDevice();
    ok("... and allowed after the check", r2.canStart("unit").isEmpty(), r2.canStart("unit"));
    ok("journal: a line a run, newest first", r2.journal().size() == 12
                                                  && r2.journal().first().toMap().value("kind").toString() == "check",
       QString::number(r2.journal().size()));

    std::printf("\nsonarqa_check: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
