// Standalone check of what the app does with the Wi-Fi module's firmware 0.20 additions, with the application's own
// classes, to the contract in the module project's docs/SBP_WIFI.md (contract 10):
//
//  1. ID_WIFI_RF = 0x5A. What the app sends (IDBinWifiRf) byte by byte against the contract's example frames to the
//     module at 87 -- off for 60 s, on, the state request -- and more built here with a frame writer of this file's
//     own: SETTING v0 {KEY 0xC96B5D4A, U1 on, U2 seconds LE}, GETTING v0 without data.
//  2. What it takes: CONTENT v0 {U1 on, U2 seconds left}; the module's refusals (ERR_KEY 7, ERR_PAYLOAD 3,
//     ERR_RUNTIME 8) as response codes; frames that are not the module's state are not taken.
//  3. WifiRf: firmware 0.20 and later; off over the network only for 1..600 s, over a wire 0..65535 (0 = until on);
//     the seconds left counted on from an answer; a status's flag bit3 against the module's answer.
//  4. ID_WIFI v0 flag bit3 = the radio off.
//  5. Retransmissions in the link rate report (ID_WIFI_SURVEY CONTENT v1): U2 after 0.19's n x U4 bounds, the page
//     16 + 13 n + 2 bytes; 0.18 and 0.19 pages carry none. WifiRetryWindow sums the last reports per other end.
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
    std::printf("%s %-72s %s\n", good ? "ok  " : "FAIL", name, detail.toUtf8().constData());
    if (!good) {
        ++fails;
    }
}

QByteArray hex(const char* text)
{
    return QByteArray::fromHex(QByteArray(text).replace(' ', ""));
}

QString show(const QByteArray& b)
{
    return QString::fromLatin1(b.toHex(' ').toUpper());
}

// KP1 written here, not with the application's frame writer: BB 55, route, mode, id, length, payload, Fletcher-8 over
// route..payload. mode = type | ver << 3 | resp << 7.
QByteArray kp1(uint8_t route, uint8_t mode, uint8_t id, const QByteArray& payload)
{
    QByteArray body;
    body.append(char(route));
    body.append(char(mode));
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

constexpr uint8_t kSetting = 0x82;              // SETTING v0, an answer asked for
constexpr uint8_t kGetting = 0x83;              // GETTING v0
constexpr uint8_t kContent = 0x01;              // CONTENT v0
constexpr uint8_t kAck = 0x81;                  // CONTENT v0 with the response bit: {code, checksum}

QByteArray rfSetting(uint8_t on, uint16_t seconds)
{
    QByteArray p = hex("4A 5D 6B C9");
    p.append(char(on));
    p.append(char(seconds & 0xFF));
    p.append(char(seconds >> 8));
    return p;
}

QByteArray rfState(uint8_t on, uint16_t seconds)
{
    QByteArray p;
    p.append(char(on));
    p.append(char(seconds & 0xFF));
    p.append(char(seconds >> 8));
    return p;
}

struct Fed {
    QVector<Parsers::Resp> parsed;              // what parse() returned per frame
    QVector<int> codes;                         // the response code of each frame the class passed on (updateContent)
};

Fed feed(IDBin& id, const QByteArray& stream)
{
    Fed out;
    QByteArray copy = stream;
    QMetaObject::Connection c = QObject::connect(&id, &IDBin::updateContent,
        [&out](Parsers::Type, Parsers::Version, Parsers::Resp resp, uint8_t) { out.codes.append(int(resp)); });
    Parsers::FrameParser proto;
    proto.setContext(reinterpret_cast<uint8_t*>(copy.data()), static_cast<uint32_t>(copy.size()));
    while (proto.availContext() > 0) {
        proto.process();
        if (proto.isComplete() && proto.id() == id.id()) {
            out.parsed.append(id.parse(proto));
        }
    }
    QObject::disconnect(c);
    return out;
}

// ID_WIFI v0, 39 bytes: state, flags, RSSI, channel, security, disconnect code, reason, clients, BSSID, four
// addresses, two U4 rates, SSID with its length.
QByteArray wifiStatus(uint8_t state, uint8_t flags)
{
    QByteArray p;
    p.append(char(state));
    p.append(char(flags));
    p.append(char(-60));
    p.append(char(6));
    p.append(char(3));
    p.append(char(0));
    p.append(char(0));
    p.append(char(0));
    p.append(QByteArray(6, char(0x11)));
    p.append(QByteArray(16, char(0)));
    p.append(QByteArray(8, char(0)));
    p.append(char(0));
    return p;
}

// One link rate page: n rates, then (0.19) n bounds and (0.20) the retransmissions.
QByteArray ratePage(int frames, int n, int stage, int retries)
{
    QByteArray p;
    p.append(char(0));
    p.append(char(1));
    p.append(hex("24 6F 28 AA BB CC"));
    p.append(char(200));
    p.append(char(0));
    p.append(char(frames & 0xFF));
    p.append(char(frames >> 8));
    p.append(char(-55));
    p.append(char(4));
    p.append(char(n));
    p.append(char(0));
    for (int k = 0; k < n; ++k) {
        p.append(char(3));
        p.append(char(7 - k));
        p.append(char(1));
        p.append(hex("08 1A 01 00"));           // 72200 kbit/s
        p.append(char(frames / n));
        p.append(char(0));
    }
    if (stage >= 19) {
        for (int k = 0; k < n; ++k) {
            p.append(hex("00 00 00 00"));
        }
    }
    if (stage >= 20) {
        p.append(char(retries & 0xFF));
        p.append(char(retries >> 8));
    }
    return p;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    std::printf("ID_WIFI_RF: the frames the app sends\n");
    ok("ID_WIFI_RF is 0x5A", Parsers::ID_WIFI_RF == 0x5A);
    // The contract's examples, checked against this file's own frame writer first (the third reference).
    const QByteArray exOff = hex("BB 55 57 82 5A 07 4A 5D 6B C9 00 3C 00 51 1A");
    const QByteArray exOn = hex("BB 55 57 82 5A 07 4A 5D 6B C9 01 00 00 16 A5");
    const QByteArray exGet = hex("BB 55 57 83 5A 00 34 99");
    ok("the contract's examples agree with this file's writer",
       kp1(87, kSetting, 0x5A, rfSetting(0, 60)) == exOff && kp1(87, kSetting, 0x5A, rfSetting(1, 0)) == exOn
       && kp1(87, kGetting, 0x5A, QByteArray()) == exGet);

    IDBinWifiRf rf;
    rf.setAddress(87);
    QVector<QByteArray> out;
    QObject::connect(&rf, &IDBin::binFrameOut, [&out](Parsers::ProtoBinOut f) {
        out.append(QByteArray(reinterpret_cast<const char*>(f.frame()), f.frameLen()));
    });
    rf.switchOff(60);
    ok("off for 60 s to 87: the contract's example", out.value(0) == exOff, show(out.value(0)));
    out.clear();
    rf.switchOn();
    ok("on to 87: the contract's example", out.value(0) == exOn, show(out.value(0)));
    out.clear();
    rf.requestAll();
    ok("the state request to 87: the contract's example", out.value(0) == exGet, show(out.value(0)));
    out.clear();
    rf.switchOff(0);
    ok("off until on (0 s) to 87", out.value(0) == kp1(87, kSetting, 0x5A, rfSetting(0, 0)), show(out.value(0)));
    out.clear();
    rf.switchOff(65535);
    ok("off for 65535 s: the seconds little-endian", out.value(0) == kp1(87, kSetting, 0x5A, rfSetting(0, 65535)),
       show(out.value(0)));
    IDBinWifiRf boat;
    boat.setAddress(88);
    QVector<QByteArray> outBoat;
    QObject::connect(&boat, &IDBin::binFrameOut, [&outBoat](Parsers::ProtoBinOut f) {
        outBoat.append(QByteArray(reinterpret_cast<const char*>(f.frame()), f.frameLen()));
    });
    boat.switchOff(600);
    ok("off for 600 s to the boat's module (88): 58 02", outBoat.value(0) == kp1(88, kSetting, 0x5A, rfSetting(0, 600)),
       show(outBoat.value(0)));

    std::printf("\nID_WIFI_RF: what the app takes\n");
    IDBinWifiRf in;
    ok("before an answer: no state", !in.state().valid);
    Fed f = feed(in, kp1(87, kContent, 0x5A, rfState(0, 60)));
    ok("CONTENT v0 {0, 60}: taken", f.parsed.value(0) == Parsers::respOk && f.codes.value(0) == Parsers::respNone);
    ok("  off, 60 s left", in.state().valid && !in.state().on && in.state().secondsLeft == 60,
       QString("on %1 left %2").arg(in.state().on).arg(in.state().secondsLeft));
    feed(in, kp1(87, kContent, 0x5A, rfState(0, 0)));
    ok("CONTENT v0 {0, 0}: off until on", in.state().valid && !in.state().on && in.state().secondsLeft == 0);
    feed(in, kp1(87, kContent, 0x5A, rfState(1, 0)));
    ok("CONTENT v0 {1, 0}: on", in.state().valid && in.state().on && in.state().secondsLeft == 0);
    feed(in, kp1(87, kContent, 0x5A, rfState(0, 300)));
    feed(in, kp1(87, kContent, 0x5A, rfState(1, 5)));
    ok("on: seconds are not kept", in.state().on && in.state().secondsLeft == 0);
    feed(in, kp1(87, kContent, 0x5A, rfState(0, 0x1234)));
    ok("the seconds little-endian (0x1234)", in.state().secondsLeft == 0x1234, QString::number(in.state().secondsLeft));
    QByteArray longer = rfState(0, 120);
    longer.append(char(0x7F));
    f = feed(in, kp1(87, kContent, 0x5A, longer));
    ok("a longer CONTENT (a later firmware's tail): the 3 bytes taken", f.parsed.value(0) == Parsers::respOk
       && !in.state().on && in.state().secondsLeft == 120);

    IDBinWifiRf bad;
    f = feed(bad, kp1(87, kContent, 0x5A, rfState(0, 60).left(2)));
    ok("2 bytes: payload refused", f.parsed.value(0) == Parsers::respErrorPayload);
    f = feed(bad, kp1(87, kContent, 0x5A, rfState(2, 60)));
    ok("on = 2: payload refused", f.parsed.value(0) == Parsers::respErrorPayload);
    f = feed(bad, kp1(87, kContent | (1 << 3), 0x5A, rfState(0, 60)));
    ok("CONTENT v1: version refused", f.parsed.value(0) == Parsers::respErrorVersion);
    f = feed(bad, kp1(87, kSetting, 0x5A, rfSetting(0, 60)));
    ok("a SETTING seen on the link (another host's): not taken", f.parsed.value(0) != Parsers::respOk);
    ok("and none of them made a state", !bad.state().valid);
    feed(in, kp1(87, kContent, 0x5A, rfState(0, 90)));
    feed(in, kp1(87, kSetting, 0x5A, rfSetting(1, 0)));
    ok("a SETTING does not overwrite the state", in.state().valid && !in.state().on && in.state().secondsLeft == 90);
    // Without its key a SETTING's 3 bytes look like a state; it is still not the module's.
    f = feed(in, kp1(87, kSetting, 0x5A, rfState(1, 0)));
    ok("nor a keyless SETTING shaped like a state", f.parsed.value(0) != Parsers::respOk && !in.state().on
       && in.state().secondsLeft == 90);

    IDBinWifiRf acks;
    const struct { int code; const char* name; } refusals[] = {
        { 7, "ERR_KEY (7)" }, { 3, "ERR_PAYLOAD (3)" }, { 8, "ERR_RUNTIME (8)" }, { 1, "OK (1)" } };
    for (const auto& r : refusals) {
        QByteArray p;
        p.append(char(r.code));
        p.append(char(0));
        p.append(char(0));
        f = feed(acks, kp1(87, kAck, 0x5A, p));
        ok(QString("the module's answer %1 passed on as its code").arg(r.name).toUtf8().constData(),
           f.codes.size() == 1 && f.codes.value(0) == r.code, QString("codes %1").arg(f.codes.value(0, -1)));
    }
    ok("an answer is not a state", !acks.state().valid);

    std::printf("\nWifiRf: the rules\n");
    ok("firmware 0.20 and later; 0.19 not; unknown -1", WifiRf::supportFor(0, 20) == 1 && WifiRf::supportFor(0, 21) == 1
       && WifiRf::supportFor(1, 0) == 1 && WifiRf::supportFor(0, 19) == 0 && WifiRf::supportFor(0, 0) == -1);
    ok("over the network: 1..600 s only", WifiRf::offAllowed(1, false) && WifiRf::offAllowed(600, false)
       && !WifiRf::offAllowed(0, false) && !WifiRf::offAllowed(601, false) && !WifiRf::offAllowed(-1, false));
    ok("over a wire: 0 (until on)..65535 s", WifiRf::offAllowed(0, true) && WifiRf::offAllowed(65535, true)
       && WifiRf::offAllowed(601, true) && !WifiRf::offAllowed(65536, true) && !WifiRf::offAllowed(-1, true));
    ok("left: 60 s at the answer", WifiRf::leftS(60, 1000, 1000) == 60);
    ok("left: rounded up (59.999 s -> 60, 59.0 -> 59)", WifiRf::leftS(60, 0, 1) == 60 && WifiRf::leftS(60, 0, 1000) == 59);
    ok("left: the last second, then 0", WifiRf::leftS(60, 0, 59001) == 1 && WifiRf::leftS(60, 0, 60000) == 0
       && WifiRf::leftS(60, 0, 90000) == 0);
    ok("left: no timer -> 0", WifiRf::leftS(0, 0, 5) == 0);
    ok("status agrees: kept", WifiRf::onStatus(true, false, true, 5000) == WifiRf::StatusKeep
       && WifiRf::onStatus(true, true, false, 5000) == WifiRf::StatusKeep);
    ok("status on after an answer 'off' over 1 s old: back on", WifiRf::onStatus(true, false, false, 1001) == WifiRf::StatusBackOn);
    ok("status off after an answer 'on' over 1 s old: off, ask", WifiRf::onStatus(true, true, true, 1001) == WifiRf::StatusOffAsk);
    ok("a contradicting status within 1 s of the answer: kept", WifiRf::onStatus(true, false, false, 1000) == WifiRf::StatusKeep
       && WifiRf::onStatus(true, true, true, 200) == WifiRf::StatusKeep);
    ok("no answer yet, status off: ask; status on: kept", WifiRf::onStatus(false, true, true, 0) == WifiRf::StatusAsk
       && WifiRf::onStatus(false, true, false, 0) == WifiRf::StatusKeep);

    std::printf("\nID_WIFI v0: flag bit3\n");
    ok("FlagRadioOff is bit3", IDBinWifi::FlagRadioOff == 0x08);
    IDBinWifi wifi;
    f = feed(wifi, kp1(87, kContent, 0x57, wifiStatus(0, 0x08 | 0x01)));
    ok("status with bit3 (and auto-connect): taken, the radio off", f.parsed.value(0) == Parsers::respOk
       && (wifi.status().flags & IDBinWifi::FlagRadioOff) && (wifi.status().flags & IDBinWifi::FlagAutoConnect),
       QString("flags 0x%1").arg(wifi.status().flags, 2, 16, QChar('0')));
    feed(wifi, kp1(87, kContent, 0x57, wifiStatus(3, 0x01)));
    ok("status without it: on", !(wifi.status().flags & IDBinWifi::FlagRadioOff));

    std::printf("\nlink rate report: retransmissions (firmware 0.20)\n");
    WifiLinkRate::Page p;
    ok("0.18 page (16 + 9 n): parsed, no retransmissions", WifiLinkRate::parse(ratePage(371, 2, 18, 0), p)
       && p.frames == 371 && p.rates.size() == 2 && p.retries == -1, QString("retries %1").arg(p.retries));
    ok("0.19 page (16 + 13 n): parsed, no retransmissions", WifiLinkRate::parse(ratePage(371, 2, 19, 0), p)
       && p.rates.size() == 2 && p.retries == -1, QString("retries %1").arg(p.retries));
    ok("0.20 page (16 + 13 n + 2): 9 of 371 came again", WifiLinkRate::parse(ratePage(371, 2, 20, 9), p)
       && p.rates.size() == 2 && p.retries == 9 && p.frames == 371, QString("retries %1 frames %2").arg(p.retries).arg(p.frames));
    ok("0.20 page: the retransmissions little-endian (0x0102)", WifiLinkRate::parse(ratePage(1000, 1, 20, 0x0102), p)
       && p.retries == 0x0102, QString::number(p.retries));
    ok("0.20 page with no rate (n = 0): read after the 16 bytes", WifiLinkRate::parse(ratePage(0, 0, 20, 0), p)
       && p.rates.isEmpty() && p.retries == 0, QString("retries %1").arg(p.retries));
    ok("a 0.20 page is 16 + 13 n + 2 bytes", ratePage(371, 3, 20, 9).size() == 16 + 13 * 3 + 2);

    std::printf("\nlink rate report: LR (0.19 proof, the code calibrated 2026-10-02)\n");
    // The module session's two pages, as host/sbpframe.parse_linkrate takes them (0.20 layout, one other end, one LR
    // rate): proven in the window (flag 0x04, 500, bound 376, 1 came again), and not proven (flag 0, 0, bound 240).
    const QByteArray lrProven = hex("00 01 d8 3b da 29 cf d1 c8 00 14 00 d2 00 01 00 04 1a 04 f4 01 00 00 14 00 78 01 00 00 01 00");
    const QByteArray lrBare = hex("00 01 d8 3b da 29 cf d1 c8 00 14 00 d2 00 01 00 04 1a 00 00 00 00 00 14 00 f0 00 00 00 01 00");
    ok("LR proven: parsed, 20 frames at -46 dBm, 1 came again", WifiLinkRate::parse(lrProven, p) && p.frames == 20
       && p.rssi == -46 && p.retries == 1 && p.rates.size() == 1 && p.rates[0].kind == WifiLinkRate::KindLr
       && p.rates[0].code == 0x1A && p.rates[0].flags == 0x04, QString("%1 frames %2 dBm").arg(p.frames).arg(p.rssi));
    ok("LR proven: 500 kbit/s, \"0.5 Mbit/s · LR\"", p.rates.size() == 1 && p.rates[0].kbps == 500
       && WifiLinkRate::text(p.rates[0]) == QString::fromUtf8("0.5 Mbit/s \xC2\xB7 LR"),
       p.rates.isEmpty() ? QString() : WifiLinkRate::text(p.rates[0]));
    ok("LR 0x1A not proven in the window: 500 by the code", WifiLinkRate::parse(lrBare, p) && p.rates.size() == 1
       && p.rates[0].flags == 0 && p.rates[0].kbps == 500 && WifiLinkRate::text(p.rates[0]) == QString::fromUtf8("0.5 Mbit/s \xC2\xB7 LR"),
       p.rates.isEmpty() ? QString() : WifiLinkRate::text(p.rates[0]));
    QByteArray lrOther = lrBare;
    lrOther[17] = char(0x1B);
    ok("LR with another code: not known, the code shown", WifiLinkRate::parse(lrOther, p) && p.rates.size() == 1
       && p.rates[0].kbps == 0 && WifiLinkRate::text(p.rates[0]) == QStringLiteral("LR, rate not known (code 0x1B)"),
       p.rates.isEmpty() ? QString() : WifiLinkRate::text(p.rates[0]));
    QByteArray gWith1A = lrBare;
    gWith1A[16] = char(WifiLinkRate::Kind11g);
    ok("code 0x1A of 11g: not LR's 500", WifiLinkRate::parse(gWith1A, p) && p.rates.size() == 1 && p.rates[0].kbps == 0);

    WifiRetryWindow w;
    const QString a = QStringLiteral("24:6f:28:aa:bb:cc"), b = QStringLiteral("24:6f:28:11:22:33");
    w.add(a, 3, 40, 0);
    w.add(a, 1, 50, 3000);
    w.add(b, 7, 10, 3000);
    WifiRetryWindow::Sum s = w.sum(a, 3000);
    ok("sums per other end: 4 of 90 in 2 reports", s.retries == 4 && s.frames == 90 && s.reports == 2,
       QString("%1 of %2 in %3").arg(s.retries).arg(s.frames).arg(s.reports));
    s = w.sum(b, 3000);
    ok("the other one apart: 7 of 10", s.retries == 7 && s.frames == 10 && s.reports == 1);
    w.add(a, 0, 0, 6000);
    w.add(a, 9, 5, 6000);
    w.add(a, -1, 30, 6000);
    ok("no frames, more retries than frames, or none sent: nothing added", w.sum(a, 6000).reports == 2);
    s = w.sum(a, 30500);
    ok("older than 30 s: dropped from the sum", s.reports == 1 && s.retries == 1 && s.frames == 50,
       QString("%1 of %2 in %3").arg(s.retries).arg(s.frames).arg(s.reports));
    WifiRetryWindow ten;
    for (int i = 0; i < 12; ++i) {
        ten.add(a, i, 100, 1000 + i * 100);
    }
    s = ten.sum(a, 2200);
    ok("at most the last 10 reports", s.reports == 10 && s.retries == (2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11) && s.frames == 1000,
       QString("%1 of %2 in %3").arg(s.retries).arg(s.frames).arg(s.reports));
    ten.clear();
    ok("clear", ten.sum(a, 2200).reports == 0);

    std::printf("\nwifi020_check: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
