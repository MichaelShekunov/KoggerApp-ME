// Standalone check of the servo's manual mode on the protocol side (ID_SERVO_CONTROL = 0x27, IDBinServoControl),
// with the application's own class, to the contract in docs/SERVO_MANUAL.md:
//
//  1. Manual mode is bit 1 of general; while it is set center_deg carries the angle held, cut to +-angle_range/2.
//  2. Into manual mode: the angle starts at the scan's center; out of it: center_deg is the scan's center again,
//     with any change made to it meanwhile.
//  3. The frames of manual mode are not tracked by the acknowledgement check (a drag sends tens a second); the
//     other settings stay tracked and, in manual mode, carry bit 1 and the angle held.
//  4. The device's own frames: in manual mode center_deg is read as the angle held and the scan's center is kept.
//  5. The PWM frequency (ID_SERVO_CONTROL v1 [U2 hz], Recorder firmware from 2026-10-28): read with v0 at the start,
//     sent as SETTING v1 and read back; unknown (0) until the device answers v1; the period is 1e6 / hz us, as the
//     firmware counts it.
// Exit 0 = pass, 1 = fail.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QObject>
#include <QVector>
#include <cstdio>

#include "id_binnary.h"

namespace {

int fails = 0;
void ok(const char* name, bool good, const QString& detail = QString())
{
    std::printf("%s %-70s %s\n", good ? "ok  " : "FAIL", name, detail.toUtf8().constData());
    if (!good) {
        ++fails;
    }
}

using Servo = IDBinServoControl;

// A tracked frame with no answer: after IDBin's 1.5 s the class reports the servo control lost
// (notifyDevDriver(false)), and the servo panel drops the device until a read-back comes. That is what the frames of
// manual mode must not cause, a drag's answers never matching the checksum of the frame sent last.
bool reportedLost(Servo& sv, int ms = 1700)
{
    bool lost = false;
    const auto c = QObject::connect(&sv, &IDBin::notifyDevDriver, [&lost](bool state) {
        if (!state) {
            lost = true;
        }
    });
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    QObject::disconnect(c);
    return lost;
}

// KP1: BB 55, route, mode (type in bits 0-1, version in bits 3-5), id, length, payload, Fletcher-8 over route..payload.
QByteArray kp1(uint8_t type, uint8_t ver, uint8_t id, const QByteArray& payload)
{
    QByteArray body;
    body.append(char(0));
    body.append(char((type & 0x3) | ((ver & 0x7) << 3)));
    body.append(char(id));
    body.append(char(payload.size()));
    body.append(payload);
    uint8_t c1 = 0, c2 = 0;
    for (const char b : body) {
        c1 = uint8_t(c1 + uint8_t(b));
        c2 = uint8_t(c2 + c1);
    }
    QByteArray f;
    f.append(char(0xBB));
    f.append(char(0x55));
    f.append(body);
    f.append(char(c1));
    f.append(char(c2));
    return f;
}

void le16(QByteArray& p, int v)
{
    p.append(char(v & 0xFF));
    p.append(char((v >> 8) & 0xFF));
}

struct State {
    int general = 0, options = 0, pwmMin = 0, pwmMax = 0, angleRange = 0, step = 0, range = 0, center = 0;
};

QByteArray statePayload(const State& s)
{
    QByteArray p;
    for (int v : { s.general, s.options, s.pwmMin, s.pwmMax, s.angleRange, s.step, s.range, s.center }) {
        le16(p, v);
    }
    return p;
}

void feed(Servo& sv, const QByteArray& frame)
{
    QByteArray copy = frame;
    Parsers::FrameParser proto;
    proto.setContext(reinterpret_cast<uint8_t*>(copy.data()), static_cast<uint32_t>(copy.size()));
    while (proto.availContext() > 0) {
        proto.process();
        if (proto.isComplete() && proto.id() == Parsers::ID_SERVO_CONTROL) {
            sv.parse(proto);
        }
    }
}

struct Sent {
    int type = -1, ver = -1, id = -1, len = -1;
    bool sum = false;
    State s;
};

int s16(const uint8_t* b) { return int16_t(uint16_t(b[0]) | (uint16_t(b[1]) << 8)); }
int u16(const uint8_t* b) { return int(uint16_t(b[0]) | (uint16_t(b[1]) << 8)); }

Sent sentFrame(const QByteArray& f)
{
    Sent r;
    const auto* b = reinterpret_cast<const uint8_t*>(f.constData());
    if (f.size() < 8 || b[0] != 0xBB || b[1] != 0x55) {
        return r;
    }
    r.type = b[3] & 0x3;
    r.ver = (b[3] >> 3) & 0x7;
    r.id = b[4];
    r.len = b[5];
    if (f.size() != 6 + r.len + 2) {
        return r;
    }
    uint8_t c1 = 0, c2 = 0;
    for (int i = 2; i < 6 + r.len; ++i) {
        c1 = uint8_t(c1 + b[i]);
        c2 = uint8_t(c2 + c1);
    }
    r.sum = (c1 == b[6 + r.len] && c2 == b[7 + r.len]);
    if (r.len == 16) {
        const uint8_t* p = b + 6;
        r.s = { u16(p), u16(p + 2), u16(p + 4), u16(p + 6), s16(p + 8), s16(p + 10), s16(p + 12), s16(p + 14) };
    }
    return r;
}

QString show(const Sent& a)
{
    return QString("general %1 options %2 pwm %3..%4 angle_range %5 step %6 range %7 center %8")
        .arg(a.s.general).arg(a.s.options).arg(a.s.pwmMin).arg(a.s.pwmMax).arg(a.s.angleRange).arg(a.s.step)
        .arg(a.s.range).arg(a.s.center);
}

bool isSetting16(const Sent& a)
{
    return a.type == Parsers::SETTING && a.ver == 0 && a.id == 0x27 && a.len == 16 && a.sum;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ok("ID_SERVO_CONTROL is 0x27", Parsers::ID_SERVO_CONTROL == 0x27);
    ok("manual mode is bit 1 of general", IDBinServoControl::GenManual == 0x0002 && IDBinServoControl::GenEnable == 0x0001);

    Servo sv;
    QVector<QByteArray> out;
    QObject::connect(&sv, &IDBin::binFrameOut, [&out](Parsers::ProtoBinOut f) {
        out.append(QByteArray(reinterpret_cast<const char*>(f.frame()), f.frameLen()));
    });

    std::printf("\nthe device's state, scanning\n");
    const State dev { 0x0001, 0, 600, 2400, 18000, 450, 9000, 1500 };
    feed(sv, kp1(Parsers::CONTENT, 0, 0x27, statePayload(dev)));
    ok("taken: scanning, not manual, center 15.00", sv.enabled() && !sv.manual() && sv.centerDeg() == 1500
       && sv.scanCenterDeg() == 1500, QString("center %1").arg(sv.centerDeg()));
    ok("nothing sent for it", out.isEmpty());

    std::printf("\ninto manual mode\n");
    sv.setManual(true);
    Sent a = sentFrame(out.value(0));
    ok("one frame: SETTING v0 to 0x27, 16 bytes, checksum right", out.size() == 1 && isSetting16(a),
       QString("%1 frames, type %2 v%3 len %4").arg(out.size()).arg(a.type).arg(a.ver).arg(a.len));
    ok("general 0x0003: the scan's enable kept, manual set", a.s.general == 0x0003, show(a));
    ok("center_deg is the angle held: the scan's center, 15.00", a.s.center == 1500, show(a));
    ok("the other fields as the device has them", a.s.options == 0 && a.s.pwmMin == 600 && a.s.pwmMax == 2400
       && a.s.angleRange == 18000 && a.s.step == 450 && a.s.range == 9000, show(a));
    ok("not tracked: unanswered, the servo control is not reported lost", !reportedLost(sv));
    ok("manual, holding 15.00, the scan's center kept", sv.manual() && sv.manualDeg() == 1500 && sv.scanCenterDeg() == 1500);

    std::printf("\na drag\n");
    out.clear();
    sv.setManualDeg(-4321);
    a = sentFrame(out.value(0));
    ok("-43.21: one frame, manual, center_deg -4321", out.size() == 1 && isSetting16(a) && a.s.general == 0x0003
       && a.s.center == -4321, show(a));
    ok("not tracked", !reportedLost(sv));
    out.clear();
    sv.setManualDeg(12000);
    a = sentFrame(out.value(0));
    ok("120.00 cut to +90.00 (angle_range 180 / 2)", a.s.center == 9000 && sv.manualDeg() == 9000, show(a));
    out.clear();
    sv.setManualDeg(-12000);
    a = sentFrame(out.value(0));
    ok("-120.00 cut to -90.00", a.s.center == -9000 && sv.manualDeg() == -9000, show(a));
    ok("the scan's center still 15.00", sv.scanCenterDeg() == 1500);

    std::printf("\nthe scan's center changed meanwhile\n");
    out.clear();
    sv.setCenterDeg(2000);
    ok("nothing sent: the device's center_deg is the angle held", out.isEmpty(), QString("%1 frames").arg(out.size()));
    ok("kept as the scan's center 20.00; the angle held is still -90.00", sv.scanCenterDeg() == 2000 && sv.centerDeg() == -9000
       && sv.manualDeg() == -9000);

    std::printf("\nanother setting in manual mode (a second servo, its timer kept apart)\n");
    {
        Servo sv2;
        QVector<QByteArray> out2;
        QObject::connect(&sv2, &IDBin::binFrameOut, [&out2](Parsers::ProtoBinOut f) {
            out2.append(QByteArray(reinterpret_cast<const char*>(f.frame()), f.frameLen()));
        });
        feed(sv2, kp1(Parsers::CONTENT, 0, 0x27, statePayload(dev)));
        sv2.setManual(true);
        sv2.setManualDeg(-2500);
        out2.clear();
        sv2.setReverse(true);
        const Sent r = sentFrame(out2.value(0));
        ok("reverse: one frame, manual kept, center_deg the angle held", out2.size() == 1 && isSetting16(r)
           && r.s.general == 0x0003 && r.s.options == 1 && r.s.center == -2500, show(r));
        ok("tracked, as every other setting: unanswered, the control is reported lost", reportedLost(sv2));
    }

    std::printf("\nthe device reads back in manual mode\n");
    out.clear();
    State back = dev;
    back.general = 0x0003;
    back.center = 777;
    feed(sv, kp1(Parsers::CONTENT, 0, 0x27, statePayload(back)));
    ok("taken as the angle held, 7.77", sv.manual() && sv.manualDeg() == 777 && sv.centerDeg() == 777);
    ok("the scan's center not taken from it, still 20.00", sv.scanCenterDeg() == 2000);

    std::printf("\nout of manual mode\n");
    out.clear();
    sv.setManual(false);
    a = sentFrame(out.value(0));
    ok("one frame, general 0x0001: manual off, the scan's enable kept", out.size() == 1 && isSetting16(a) && a.s.general == 0x0001,
       show(a));
    ok("center_deg the scan's center with the change, 20.00", a.s.center == 2000, show(a));
    ok("not tracked", !reportedLost(sv));
    ok("scanning around 20.00", !sv.manual() && sv.centerDeg() == 2000 && sv.scanCenterDeg() == 2000);
    out.clear();
    sv.setManualDeg(100);
    ok("an angle outside manual mode: nothing sent", out.isEmpty(), QString("%1 frames").arg(out.size()));
    sv.setManual(false);
    ok("manual off again: nothing sent", out.isEmpty());

    std::printf("\na scan's center outside the servo's range\n");
    sv.setCenterDeg(10000);
    out.clear();
    sv.setManual(true);
    a = sentFrame(out.value(0));
    ok("manual starts at +90.00, not at 100.00", a.s.center == 9000 && sv.manualDeg() == 9000, show(a));

    std::printf("\nrefused by the device\n");
    out.clear();
    sv.dropManual();
    ok("dropped: nothing sent, scanning around 100.00 as before", out.isEmpty() && !sv.manual() && sv.centerDeg() == 10000
       && sv.scanCenterDeg() == 10000, QString("center %1, %2 frames").arg(sv.centerDeg()).arg(out.size()));

    std::printf("\nthe PWM frequency (v1)\n");
    {
        Servo sv3;
        QVector<QByteArray> out3;
        QObject::connect(&sv3, &IDBin::binFrameOut, [&out3](Parsers::ProtoBinOut f) {
            out3.append(QByteArray(reinterpret_cast<const char*>(f.frame()), f.frameLen()));
        });
        ok("unknown before the device answers v1: 0", sv3.pwmHz() == 0);
        sv3.requestAll();
        const Sent g0 = sentFrame(out3.value(0)), g1 = sentFrame(out3.value(1));
        ok("the start reads v0 and v1: two GETTING to 0x27", out3.size() == 2 && g0.type == Parsers::GETTING
           && g0.ver == 0 && g0.id == 0x27 && g1.type == Parsers::GETTING && g1.ver == 1 && g1.id == 0x27 && g1.sum,
           QString("%1 frames, v%2 v%3").arg(out3.size()).arg(g0.ver).arg(g1.ver));
        feed(sv3, kp1(Parsers::CONTENT, 0, 0x27, statePayload(dev)));
        ok("a v0 answer leaves the frequency unknown", sv3.pwmHz() == 0, QString("%1").arg(sv3.pwmHz()));
        QByteArray p;
        le16(p, 1000);
        feed(sv3, kp1(Parsers::CONTENT, 1, 0x27, p));
        ok("a v1 answer: 1000 Hz", sv3.pwmHz() == 1000, QString("%1").arg(sv3.pwmHz()));
        ok("the v1 answer leaves the v0 fields", sv3.pwmMaxUs() == 2400 && sv3.enabled() && sv3.centerDeg() == 1500);

        out3.clear();
        sv3.setPwmHz(333);
        const auto* b = reinterpret_cast<const uint8_t*>(out3.value(0).constData());
        const Sent s = sentFrame(out3.value(0)), r = sentFrame(out3.value(1));
        ok("sent: SETTING v1 to 0x27, 2 bytes, 333, checksum right", out3.size() == 2 && s.type == Parsers::SETTING
           && s.ver == 1 && s.id == 0x27 && s.len == 2 && s.sum && u16(b + 6) == 333,
           QString("%1 frames, type %2 v%3 len %4").arg(out3.size()).arg(s.type).arg(s.ver).arg(s.len));
        ok("then read back: GETTING v1", r.type == Parsers::GETTING && r.ver == 1 && r.id == 0x27,
           QString("type %1 v%2").arg(r.type).arg(r.ver));
        ok("shown as the device has it until the read-back: 1000", sv3.pwmHz() == 1000);
        ok("not tracked", !reportedLost(sv3));
        ok("period: 50 Hz 20000 us, 333 Hz 3003 us, 1000 Hz 1000 us", Servo::periodUs(50) == 20000
           && Servo::periodUs(333) == 3003 && Servo::periodUs(1000) == 1000,
           QString("%1 %2 %3").arg(Servo::periodUs(50)).arg(Servo::periodUs(333)).arg(Servo::periodUs(1000)));
        ok("range 50..1000 Hz", Servo::PwmHzMin == 50 && Servo::PwmHzMax == 1000);
    }

    std::printf("\nservo_check: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
