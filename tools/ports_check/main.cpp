// Standalone check of a Recorder's port table on the protocol side (ID_BUS = 0x17, ID_PIN = 0x16; IDBinBus, IDBinPin),
// with the application's own classes, to the recorder fork's docs/Port-Config-Spec.md. The frames are the device's as
// its session read them on the device (2026-10-24): SONAR1 and IN3.
//
//  1. ID_BUS CONTENT v0: [bus][count][supply][function caps][function][saved][UART caps][UART][saved][TX][RX][baud u32]
//     [pin count][pins][label length][label]; a frame cut short is refused.
//  2. ID_PIN CONTENT v0: [pin][bus][role caps u32][role][saved][timer][param][value s32][label][MCU pin]; v1: [count] then
//     count x [pin][role][value s32], taken for pins already described.
//  3. Requests: ID_BUS GETTING v0 [bus], ID_PIN GETTING v0 [pin], ID_PIN GETTING v1 empty; nothing on a cold start.
//  4. The UART connectors' settings (the spec's ID_BUS V0 SETTING and V2..V4): v2/v3/v4 answers taken by function
//     (baud = the setting, 0 auto; the connector's rate in use stays the v0 one), wrong function or length refused;
//     GETTING [function], SETTING with the other fields as read (defaults when never read), SETTING v0 [bus][f][uart].
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

    std::printf("\nthe UART connectors' settings (ID_BUS V0 SETTING, V2..V4)\n");
    // TELEM: function 3 (telemetry), USART1, 115200 in use; caps 0x1E (the four UART functions) and its own UART only.
    QByteArray telem;
    for (int v : { 2, 8, 2, 0x1E, 3, 3, 0x01, 1, 1, 6, 7 }) {
        telem.append(char(v));
    }
    le(telem, 115200, 4);
    telem.append(char(2));
    telem.append(char(6));
    telem.append(char(7));
    label(telem, "TELEM");
    ok("TELEM: taken", feed(bus, 0x17, kp1(Parsers::CONTENT, 0, 0x17, telem)) == Parsers::respOk);
    ok("versions: sonar A/B v2, telemetry v3, navigation v4, none for 0", IDBinBus::functionVersion(1) == Parsers::v2
       && IDBinBus::functionVersion(2) == Parsers::v2 && IDBinBus::functionVersion(3) == Parsers::v3
       && IDBinBus::functionVersion(4) == Parsers::v4 && IDBinBus::functionVersion(0) == Parsers::v0);
    ok("no settings before an answer", !bus.functionSettings(3).valid && !bus.functionSettings(1).valid);
    QByteArray v3;
    v3.append(char(3));
    le(v3, 0, 4);               // auto
    v3.append(char(0));
    ok("v3 [3][0][0]: taken", feed(bus, 0x17, kp1(Parsers::CONTENT, 3, 0x17, v3)) == Parsers::respOk);
    ok("telemetry on auto (0); the TELEM connector keeps the rate in use, 115200",
       bus.functionSettings(3).valid && bus.functionSettings(3).baud == 0 && bus.buses().value(2).baud == 115200,
       QString("%1 / %2").arg(bus.functionSettings(3).baud).arg(bus.buses().value(2).baud));
    QByteArray v2;
    v2.append(char(2));
    le(v2, 115200, 4);
    for (int v : { 0, 0, 1 }) {     // log 0, to host 1: the two told apart
        v2.append(char(v));
    }
    ok("v2 [2][115200][0][0][1]: taken", feed(bus, 0x17, kp1(Parsers::CONTENT, 2, 0x17, v2)) == Parsers::respOk);
    const IDBinBus::FunctionSettings sb = bus.functionSettings(2);
    ok("sonar B: 115200, protocol 0, log 0, to host 1", sb.valid && sb.baud == 115200 && sb.protocol == 0 && sb.log == 0 && sb.toHost == 1,
       QString("%1 %2 %3 %4").arg(sb.baud).arg(sb.protocol).arg(sb.log).arg(sb.toHost));
    QByteArray v4;
    v4.append(char(4));
    le(v4, 9600, 4);
    v4.append(char(0));
    v4.append(char(5));
    ok("v4 [4][9600][0][5]: taken", feed(bus, 0x17, kp1(Parsers::CONTENT, 4, 0x17, v4)) == Parsers::respOk);
    ok("navigation: 9600, rate 5 Hz", bus.functionSettings(4).baud == 9600 && bus.functionSettings(4).rateHz == 5,
       QString("%1 %2").arg(bus.functionSettings(4).baud).arg(bus.functionSettings(4).rateHz));
    QByteArray v2tele = v2;
    v2tele[0] = char(3);
    ok("v2 carrying telemetry: refused, its settings kept",
       feed(bus, 0x17, kp1(Parsers::CONTENT, 2, 0x17, v2tele)) == Parsers::respErrorPayload && bus.functionSettings(3).baud == 0);
    QByteArray v3sonar = v3;
    v3sonar[0] = char(1);
    ok("v3 carrying sonar A: refused", feed(bus, 0x17, kp1(Parsers::CONTENT, 3, 0x17, v3sonar)) == Parsers::respErrorPayload
       && !bus.functionSettings(1).valid);
    ok("v4 cut short: refused", feed(bus, 0x17, kp1(Parsers::CONTENT, 4, 0x17, v4.left(6))) == Parsers::respErrorPayload);
    ok("v5: refused", feed(bus, 0x17, kp1(Parsers::CONTENT, 5, 0x17, v3)) == Parsers::respErrorVersion);
    const QVector<U4>& bauds = IDBinBus::functionBauds();
    ok("11 rates: 0 (auto), then 9600 .. 5000000, with 2000000", bauds.size() == 11 && bauds.first() == 0 && bauds.at(1) == 9600
       && bauds.last() == 5000000 && bauds.contains(2000000) && !bauds.contains(1000000), QString("%1 rates").arg(bauds.size()));

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
    bus.requestFunction(1);
    bus.requestFunction(3);
    bus.requestFunction(4);
    bus.setFunctionBaud(2, 2000000);
    bus.setFunctionBaud(3, 0);
    bus.setFunctionBaud(4, 921600);
    bus.setBusFunction(2, 3, 1);
    const char* sentNames[] = { "GETTING v2 [1]", "GETTING v3 [3]", "GETTING v4 [4]",
                                "SETTING v2 [2][2000000][0][0][1] (the rest as read)", "SETTING v3 [3][0][0] (auto)",
                                "SETTING v4 [4][921600][0][5] (the rate as read)", "SETTING v0 [2][3][1] (TELEM gets telemetry)" };
    const int sentTypes[] = { Parsers::GETTING, Parsers::GETTING, Parsers::GETTING, Parsers::SETTING, Parsers::SETTING,
                              Parsers::SETTING, Parsers::SETTING };
    const int sentVers[] = { 2, 3, 4, 2, 3, 4, 0 };
    const char* sentHex[] = { "01", "03", "04", "0280841e00000001", "030000000000", "0400100e000005", "020301" };
    for (int i = 0; i < 7; ++i) {
        const Sent f = sentFrame(out.value(3 + i));
        ok(QString("ID_BUS %1").arg(sentNames[i]).toUtf8().constData(),
           f.type == sentTypes[i] && f.ver == sentVers[i] && f.id == 0x17 && f.sum && f.payload == QByteArray::fromHex(sentHex[i])
           && (uchar(out.value(3 + i).at(3)) & 0x80),
           QString("v%1 %2").arg(f.ver).arg(QString(f.payload.toHex(' '))));
    }
    IDBinBus fresh;
    QVector<QByteArray> out2;
    QObject::connect(&fresh, &IDBin::binFrameOut, [&out2](Parsers::ProtoBinOut f) {
        out2.append(QByteArray(reinterpret_cast<const char*>(f.frame()), f.frameLen()));
    });
    fresh.setFunctionBaud(3, 115200);
    fresh.setFunctionBaud(1, 57600);
    ok("never read: telemetry goes with to host 0, sonar A with protocol 0, log 1, to host 1",
       sentFrame(out2.value(0)).payload == QByteArray::fromHex("0300c2010000")
       && sentFrame(out2.value(1)).payload == QByteArray::fromHex("0100e10000000101"),
       QString(sentFrame(out2.value(0)).payload.toHex(' ')) + " | " + QString(sentFrame(out2.value(1)).payload.toHex(' ')));

    std::printf("\nports_check: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
