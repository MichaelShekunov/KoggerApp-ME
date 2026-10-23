// Standalone check of a Recorder's port table on the protocol side (ID_BUS = 0x17, ID_PIN = 0x16; IDBinBus, IDBinPin),
// with the application's own classes, to the recorder fork's docs/Port-Config-Spec.md. The frames are the device's as
// its session read them on the device (2026-10-24): SONAR1 and IN3.
//
//  1. ID_BUS CONTENT v0: [bus][count][supply][function caps][function][saved][UART caps][UART][saved][TX][RX][baud u32]
//     [pin count][pins][label length][label]; a frame cut short is refused.
//  2. ID_PIN CONTENT v0: [pin][bus][role caps u32][role][saved][timer][param][value s32][label][MCU pin]; v1: [count] then
//     count x [pin][role][value s32], taken for pins already described.
//  3. Requests: ID_BUS GETTING v0 [bus], ID_PIN GETTING v0 [pin], ID_PIN GETTING v1 empty; nothing on a cold start.
// Exit 0 = pass, 1 = fail.
#include <QCoreApplication>
#include <QObject>
#include <QVector>
#include <cstdio>

#include "id_binnary.h"

namespace {

int fails = 0;
void ok(const char* name, bool good, const QString& detail = QString())
{
    std::printf("%s %-66s %s\n", good ? "ok  " : "FAIL", name, detail.toUtf8().constData());
    if (!good) {
        ++fails;
    }
}

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

void le(QByteArray& p, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i) {
        p.append(char((v >> (8 * i)) & 0xFF));
    }
}

void label(QByteArray& p, const QByteArray& s)
{
    p.append(char(s.size()));
    p.append(s);
}

template <class T>
Parsers::Resp feed(T& bin, uint8_t id, const QByteArray& frame)
{
    Parsers::Resp r = Parsers::respNone;
    QByteArray copy = frame;
    Parsers::FrameParser proto;
    proto.setContext(reinterpret_cast<uint8_t*>(copy.data()), static_cast<uint32_t>(copy.size()));
    while (proto.availContext() > 0) {
        proto.process();
        if (proto.isComplete() && proto.id() == id) {
            r = bin.parse(proto);
        }
    }
    return r;
}

struct Sent {
    int type = -1, ver = -1, id = -1, len = -1;
    bool sum = false;
    QByteArray payload;
};

Sent sentFrame(const QByteArray& f)
{
    Sent s;
    const auto* b = reinterpret_cast<const uint8_t*>(f.constData());
    if (f.size() < 8 || b[0] != 0xBB || b[1] != 0x55) {
        return s;
    }
    s.type = b[3] & 0x3;
    s.ver = (b[3] >> 3) & 0x7;
    s.id = b[4];
    s.len = b[5];
    if (f.size() != 6 + s.len + 2) {
        return s;
    }
    uint8_t c1 = 0, c2 = 0;
    for (int i = 2; i < 6 + s.len; ++i) {
        c1 = uint8_t(c1 + b[i]);
        c2 = uint8_t(c2 + c1);
    }
    s.sum = (c1 == b[6 + s.len] && c2 == b[7 + s.len]);
    s.payload = f.mid(6, s.len);
    return s;
}

QByteArray sonar1()
{
    QByteArray p;
    for (int v : { 0, 8, 1, 0x1F, 1, 1, 0x0A, 2, 2, 0, 1 }) {
        p.append(char(v));
    }
    le(p, 115200, 4);
    p.append(char(2));
    p.append(char(0));
    p.append(char(1));
    label(p, "SONAR1");
    return p;
}

QByteArray in3(int value)
{
    QByteArray p;
    p.append(char(12));
    p.append(char(6));
    le(p, 0x79, 4);
    for (int v : { 3, 3, 2, 0 }) {
        p.append(char(v));
    }
    le(p, uint32_t(value), 4);
    label(p, "IN3");
    label(p, "PA0");
    return p;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ok("ID_BUS is 0x17, ID_PIN is 0x16", Parsers::ID_BUS == 0x17 && Parsers::ID_PIN == 0x16);

    std::printf("\nID_BUS\n");
    IDBinBus bus;
    ok("SONAR1: taken", feed(bus, 0x17, kp1(Parsers::CONTENT, 0, 0x17, sonar1())) == Parsers::respOk);
    const IDBinBus::Bus b = bus.buses().value(0);
    ok("count 8, last bus 0", bus.count() == 8 && bus.lastBus() == 0, QString("%1 / %2").arg(bus.count()).arg(bus.lastBus()));
    ok("supply VBAT, function SonarA (1) applied and saved, caps 0x1F",
       b.supply == 1 && b.function == 1 && b.functionSaved == 1 && b.functionCaps == 0x1F,
       QString("supply %1 function %2/%3 caps %4").arg(b.supply).arg(b.function).arg(b.functionSaved).arg(b.functionCaps));
    ok("UART USART2 (2) applied and saved, caps 0x0A; TX pin 0, RX pin 1",
       b.uart == 2 && b.uartSaved == 2 && b.uartCaps == 0x0A && b.txPin == 0 && b.rxPin == 1);
    ok("baud 115200, pins [0, 1], label SONAR1", b.baud == 115200 && b.pins == QVector<U1>({ 0, 1 }) && b.label == "SONAR1",
       QString("%1, %2 pins, \"%3\"").arg(b.baud).arg(b.pins.size()).arg(b.label));
    // I2C with Nav saved for the next boot (a phase 3 move pending): function 0 now, 4 saved, LPUART1 saved.
    QByteArray i2c;
    for (int v : { 4, 8, 2, 0x17, 0, 4, 0x0C, 0, 4, 0xFF, 0xFF }) {
        i2c.append(char(v));
    }
    le(i2c, 0, 4);
    i2c.append(char(2));
    i2c.append(char(8));
    i2c.append(char(9));
    label(i2c, "I2C");
    ok("I2C: taken", feed(bus, 0x17, kp1(Parsers::CONTENT, 0, 0x17, i2c)) == Parsers::respOk);
    const IDBinBus::Bus b4 = bus.buses().value(4);
    ok("I2C: function 0 now, Nav (4) saved; UART none now, LPUART1 (4) saved; no TX/RX",
       b4.function == 0 && b4.functionSaved == 4 && b4.uart == 0 && b4.uartSaved == 4 && b4.txPin == 0xFF && b4.rxPin == 0xFF,
       QString("function %1/%2 uart %3/%4").arg(b4.function).arg(b4.functionSaved).arg(b4.uart).arg(b4.uartSaved));
    IDBinBus shortBus;
    ok("cut before its label: refused", feed(shortBus, 0x17, kp1(Parsers::CONTENT, 0, 0x17, sonar1().left(18))) == Parsers::respErrorPayload);
    ok("cut in its pin list: refused", feed(shortBus, 0x17, kp1(Parsers::CONTENT, 0, 0x17, sonar1().left(16))) == Parsers::respErrorPayload);
    ok("and neither kept", shortBus.buses().isEmpty());

    std::printf("\nID_PIN\n");
    IDBinPin pin;
    ok("IN3 v0: taken", feed(pin, 0x16, kp1(Parsers::CONTENT, 0, 0x16, in3(1000))) == Parsers::respOk);
    const IDBinPin::Pin p = pin.pins().value(12);
    ok("bus 6, caps 0x79, role Pwm (3) applied and saved, timer 2, param 0 (OUT1), 1000 us",
       p.bus == 6 && p.roleCaps == 0x79 && p.role == 3 && p.roleSaved == 3 && p.timer == 2 && p.param == 0 && p.value == 1000,
       QString("bus %1 caps %2 role %3/%4 timer %5 param %6 value %7").arg(p.bus).arg(p.roleCaps).arg(p.role).arg(p.roleSaved)
           .arg(p.timer).arg(p.param).arg(p.value));
    ok("labels IN3, PA0", p.label == "IN3" && p.mcuPin == "PA0", p.label + " " + p.mcuPin);
    QByteArray o1;
    o1.append(char(14));
    o1.append(char(7));
    le(o1, 0x79, 4);
    for (int v : { 3, 3, 16, 0 }) {
        o1.append(char(v));
    }
    le(o1, 1000, 4);
    label(o1, "O1");
    label(o1, "PA6");
    ok("O1 v0: taken", feed(pin, 0x16, kp1(Parsers::CONTENT, 0, 0x16, o1)) == Parsers::respOk);
    QByteArray v1;
    v1.append(char(3));
    v1.append(char(12));
    v1.append(char(6));
    le(v1, 100000, 4);          // IN3 now analog, a value past 16 bits
    v1.append(char(13));        // a pin not described yet
    v1.append(char(3));
    le(v1, 1500, 4);
    v1.append(char(14));
    v1.append(char(3));
    le(v1, uint32_t(-5), 4);    // O1, a negative value through
    ok("v1: taken", feed(pin, 0x16, kp1(Parsers::CONTENT, 1, 0x16, v1)) == Parsers::respOk);
    ok("v1: IN3 role 6, value 100000; its description kept", pin.pins().value(12).role == 6 && pin.pins().value(12).value == 100000
       && pin.pins().value(12).label == "IN3" && pin.pins().value(12).roleSaved == 3,
       QString("role %1 value %2").arg(pin.pins().value(12).role).arg(pin.pins().value(12).value));
    ok("v1: O1 after the undescribed one, value -5", pin.pins().value(14).role == 3 && pin.pins().value(14).value == -5,
       QString("role %1 value %2").arg(pin.pins().value(14).role).arg(pin.pins().value(14).value));
    ok("v1: a pin not described is not invented", !pin.pins().contains(13));
    ok("v1 short of its count: refused", feed(pin, 0x16, kp1(Parsers::CONTENT, 1, 0x16, v1.left(13))) == Parsers::respErrorPayload);
    IDBinPin shortPin;
    ok("v0 cut before the MCU pin: refused", feed(shortPin, 0x16, kp1(Parsers::CONTENT, 0, 0x16, in3(1).left(18))) == Parsers::respErrorPayload);

    std::printf("\nwhat the app sends\n");
    QVector<QByteArray> out;
    auto grab = [&out](Parsers::ProtoBinOut f) { out.append(QByteArray(reinterpret_cast<const char*>(f.frame()), f.frameLen())); };
    QObject::connect(&bus, &IDBin::binFrameOut, grab);
    QObject::connect(&pin, &IDBin::binFrameOut, grab);
    bus.requestAll();
    bus.startColdStartTimer();
    pin.requestAll();
    pin.startColdStartTimer();
    ok("nothing on its own", out.isEmpty(), QString("%1 frames").arg(out.size()));
    bus.request(5);
    pin.request(12);
    pin.requestValues();
    const Sent a = sentFrame(out.value(0)), c = sentFrame(out.value(1)), d = sentFrame(out.value(2));
    ok("ID_BUS GETTING v0 [5]", a.type == Parsers::GETTING && a.ver == 0 && a.id == 0x17 && a.payload == QByteArray(1, char(5)) && a.sum);
    ok("ID_PIN GETTING v0 [12]", c.type == Parsers::GETTING && c.ver == 0 && c.id == 0x16 && c.payload == QByteArray(1, char(12)) && c.sum);
    ok("ID_PIN GETTING v1, empty", d.type == Parsers::GETTING && d.ver == 1 && d.id == 0x16 && d.len == 0 && d.sum,
       QString("v%1 len %2").arg(d.ver).arg(d.len));

    std::printf("\nports_check: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
