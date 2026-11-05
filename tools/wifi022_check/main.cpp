// Standalone check of what the app does with the Wi-Fi module's profiles (firmware 0.22, ID_WIFI_PROFILE = 0x5B), with
// the application's own classes, to the contract in the module project's docs/SBP_WIFI.md 3г (contract 12):
//
//  1. The block (WifiProfile::encode) byte by byte against what the module project's own host/sbpframe.py profile_blob
//     makes of the same profile (a made-up one with made-up passwords, every field off its default); its CRC-32, the
//     begin payload, the pieces; the module's reasons for broken blocks (decode, in the module's order of checks).
//  2. The JSON form: the module project's file format read and written back; what a broken file is told.
//  3. What the app refuses before sending (problems): the passwords, a network twice, more than 8.
//  4. The three built-in profiles: read, no password in them, the module project's values (the -ModuleRepo run of
//     run.ps1 compares them byte by byte with the passwords put back).
//  5. The frames (IDBinWifiProfile) against sbpframe.encode's: begin, a piece, apply, the two requests; what it takes:
//     CONTENT v0 and v3, not a SETTING seen on the link; ID_WIFI_NET v3's translator bytes (0.21).
//  6. The upload (WifiProfileUpload) on made-up answers: every outcome, a piece sent again, a late OK, an apply whose
//     answer was lost.
//  7. What a load changes (WifiProfileDiff), the rules (version, CP2105, the port's rate after it), the user's files.
// Exit 0 = pass, 1 = fail. With "--blob <file.json>" it prints the block of that file as hex and exits (run.ps1).
// With "--builtin <key> <module file.json>": the built-in profile <key> with the module file's passwords, as hex.
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QTemporaryDir>
#include <QTimer>
#include <QVector>
#include <cstdio>

#include "id_binnary.h"
#include "wifi_profile.h"

namespace {

int fails = 0;
void ok(const char* name, bool good, const QString& detail = QString())
{
    std::printf("%s %-78s %s\n", good ? "ok  " : "FAIL", name, detail.toUtf8().constData());
    if (!good)
        ++fails;
}

QByteArray hex(const char* text)
{
    return QByteArray::fromHex(QByteArray(text).replace(' ', ""));
}

QString show(const QByteArray& b)
{
    return QString::fromLatin1(b.toHex(' ').toUpper());
}

void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QJsonObject json(const QByteArray& text)
{
    return QJsonDocument::fromJson(text).object();
}

// The made-up profile: every field off its default, a Cyrillic name, an open network and a 64-hex-digit key.
const char kTestJson[] = R"({"name": "Тест-1", "role": "ap", "address": 88, "x1_baud": 5000000,
 "radio": {"power_dbm": 19.5, "mode": "bgnlr", "bw": 40, "ps": "min"},
 "ap": {"ssid": "Test-AP", "password": "testpass1", "channel": 11, "auth": "wpa2wpa3", "hidden": true, "max_clients": 7},
 "ip": {"ip": "10.1.2.3", "mask": "255.255.0.0", "dhcp": false, "pool_start": "10.1.2.10", "pool_end": "10.1.2.40",
        "lease_min": 300, "offer_gw": true, "offer_dns": false},
 "line0": {"mode": "tcp", "dest": "fixed", "ip": "10.1.2.9", "rport": 5000, "lport": 0},
 "line1": {"mode": "udp", "dest": "broadcast", "ip": "0.0.0.0", "rport": 14445, "lport": 14446, "baud": 115200,
           "tx_pin": 6, "rx_pin": 7, "xlat": "telemetry"},
 "networks": [{"ssid": "Net-A", "password": "abcdefgh"}, {"ssid": "Open", "password": ""},
              {"ssid": "Hex", "password": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}]})";

// host/sbpframe.py profile_blob of kTestJson (the module project at 98857f8), 182 bytes.
const char kTestBlob[] =
    "010ad0a2d0b5d181d1822d310158404b4c004e0f02010b01070207546573742d4150097465737470617373310a010203ffff0000000a01"
    "020a0a0102282c010102000a010209881300000102000000006d386e3800c2010006070103054e65742d4108616263646566676804"
    "4f70656e00034865784030313233343536373839616263646566303132333435363738396162636465663031323334353637383961"
    "626364656630313233343536373839616263646566";

struct Sent { int ver; QByteArray payload; bool ask; };

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() >= 3 && args[1] == QLatin1String("--blob")) {
        QFile f(args[2]);
        WifiProfile p;
        QString err = f.open(QIODevice::ReadOnly) ? WifiProfile::fromJson(QJsonDocument::fromJson(f.readAll()).object(), &p)
                                                  : QStringLiteral("cannot read");
        const QByteArray b = err.isEmpty() ? p.encode(&err) : QByteArray();
        std::printf("%s\n", err.isEmpty() ? b.toHex().constData() : ("ERROR " + err).toUtf8().constData());
        return err.isEmpty() ? 0 : 1;
    }
    if (args.size() >= 4 && args[1] == QLatin1String("--builtin")) {
        WifiProfile p, src;
        bool found = false;
        for (const auto& b : WifiProfileStore::builtinJson())
            if (b.first == args[2])
                found = WifiProfile::fromJson(json(b.second), &p).isEmpty();
        QFile f(args[3]);
        if (!found || !f.open(QIODevice::ReadOnly)
            || !WifiProfile::fromJson(QJsonDocument::fromJson(f.readAll()).object(), &src).isEmpty()) {
            std::printf("ERROR no built-in %s or no module file\n", qPrintable(args[2]));
            return 1;
        }
        p.apPassword = src.apPassword;            // the passwords are what the built-in profiles leave out
        for (WifiProfile::Net& n : p.networks)
            for (const WifiProfile::Net& s : src.networks)
                if (s.ssid == n.ssid)
                    n.password = s.password;
        QString err;
        const QByteArray b = p.encode(&err);
        std::printf("%s\n", err.isEmpty() ? b.toHex().constData() : ("ERROR " + err).toUtf8().constData());
        return err.isEmpty() ? 0 : 1;
    }

    // ---- 1. the block ----------------------------------------------------------------------------------------
    std::printf("1. The block against the module project's profile_blob\n");
    WifiProfile t;
    const QString readErr = WifiProfile::fromJson(json(kTestJson), &t);
    ok("the made-up profile is read", readErr.isEmpty(), readErr);
    QString encErr;
    const QByteArray blob = t.encode(&encErr);
    const QByteArray want = QByteArray::fromHex(kTestBlob);
    ok("its block is profile_blob's, byte by byte (182 bytes)", blob == want && blob.size() == 182,
       blob == want ? QString() : show(blob));
    ok("CRC-32 of the block = zlib.crc32's 24F0014B", WifiProfile::crc32(blob) == 0x24F0014Bu,
       QString::number(WifiProfile::crc32(blob), 16));
    ok("CRC-32 of \"123456789\" = CBF43926 (ISO-HDLC)", WifiProfile::crc32("123456789") == 0xCBF43926u);
    ok("begin: {KEY, U2 182, U4 crc} = profile_begin_payload",
       WifiProfile::beginPayload(blob) == hex("4a5d6bc9 b600 4b01f024"), show(WifiProfile::beginPayload(blob)));
    ok("apply: {KEY}", WifiProfile::applyPayload() == hex("4a5d6bc9"));
    const QVector<QByteArray> by40 = WifiProfile::chunkPayloads(blob, 40);
    ok("pieces of 40: profile_chunk_payloads(blob, 40)",
       by40.size() == 5 && by40[0] == hex("0000") + blob.mid(0, 40) && by40[1].left(2) == hex("2800")
       && by40[4] == hex("a000") + blob.mid(160) && by40[4].size() == 24);
    const QVector<QByteArray> by200 = WifiProfile::chunkPayloads(blob);
    ok("pieces of 200 (the default): one, offset 0", by200.size() == 1 && by200[0] == hex("0000") + blob);
    ok("a piece is never over 253 bytes", WifiProfile::chunkPayloads(QByteArray(600, 'x'), 400)[0].size() == 2 + 253);

    WifiProfile back;
    ok("decode(block) = ok and the same profile", WifiProfile::decode(blob, &back) == 0 && back == t);
    const int nameLen = uint8_t(blob[1]);
    const auto broken = [&](int at, uint8_t v) { QByteArray b = blob; b[at] = char(v); return b; };
    struct Case { const char* name; QByteArray b; int reason; } cases[] = {
        { "format 2: format", broken(0, 2), WifiProfile::ReasonFormat },
        { "name length 0: name", broken(1, 0), WifiProfile::ReasonName },
        { "name length 33: name", broken(1, 33), WifiProfile::ReasonName },
        { "a byte past the end: format", blob + QByteArray(1, '\0'), WifiProfile::ReasonFormat },
        { "the last byte missing: nets (the last network's password)", blob.left(blob.size() - 1), WifiProfile::ReasonNets },
        { "role 2: role", broken(2 + nameLen, 2), WifiProfile::ReasonRole },
        { "address 0: address", broken(3 + nameLen, 0), WifiProfile::ReasonAddress },
        { "address 255: address", broken(3 + nameLen, 255), WifiProfile::ReasonAddress },
        { "9 networks: nets", broken(91, 9), WifiProfile::ReasonNets },
    };
    for (const Case& c : cases)
        ok(c.name, WifiProfile::decode(c.b, nullptr) == c.reason, WifiProfile::reasonName(WifiProfile::decode(c.b, nullptr)));
    ok("reason names as the contract's table", WifiProfile::reasonName(13) == QLatin1String("lines")
       && WifiProfile::reasonName(16) == QLatin1String("busy") && WifiProfile::reasonName(7) == QLatin1String("x1_baud"));

    // ---- 2. the JSON form ------------------------------------------------------------------------------------
    std::printf("2. The JSON form\n");
    WifiProfile again;
    ok("toJson -> fromJson gives the same profile", WifiProfile::fromJson(t.toJson(), &again).isEmpty() && again == t);
    const QJsonObject tj = t.toJson();
    ok("written as the module project's files: role \"ap\", mode \"bgnlr\", bw 40, ps \"min\", auth \"wpa2wpa3\"",
       tj.value("role").toString() == "ap" && tj.value("radio").toObject().value("mode").toString() == "bgnlr"
       && tj.value("radio").toObject().value("bw").toInt() == 40 && tj.value("radio").toObject().value("ps").toString() == "min"
       && tj.value("ap").toObject().value("auth").toString() == "wpa2wpa3"
       && tj.value("x1_baud").toInteger() == 5000000 && tj.value("line1").toObject().value("xlat").toString() == "telemetry");
    const auto readErrOf = [](QJsonObject o) { return WifiProfile::fromJson(o, nullptr); };
    QJsonObject noBw = json(kTestJson);
    QJsonObject r = noBw.value("radio").toObject();
    r.remove("bw");
    noBw.insert("radio", r);
    ok("radio.bw missing: said so", readErrOf(noBw).startsWith("radio.bw"), readErrOf(noBw));
    r.insert("bw", 30);
    noBw.insert("radio", r);
    ok("radio.bw 30: only 20 or 40", readErrOf(noBw).startsWith("radio.bw"), readErrOf(noBw));
    QJsonObject badMode = json(kTestJson);
    QJsonObject l0 = badMode.value("line0").toObject();
    l0.insert("mode", "udpx");
    badMode.insert("line0", l0);
    ok("line0.mode \"udpx\": unknown", readErrOf(badMode).startsWith("line0.mode"), readErrOf(badMode));
    QJsonObject numbers = json(kTestJson);
    numbers.insert("role", 1);
    WifiProfile pn;
    ok("a number where a name goes (role 1), as the module project's tools take it",
       WifiProfile::fromJson(numbers, &pn).isEmpty() && pn.role == 1);
    QJsonObject quarter = json(kTestJson);
    QJsonObject rq = quarter.value("radio").toObject();
    rq.insert("power_dbm", 19.625);               // 78.5 quarters: to the even 78, as Python's round()
    quarter.insert("radio", rq);
    WifiProfile pq;
    WifiProfile::fromJson(quarter, &pq);
    rq.insert("power_dbm", 19.875);               // 79.5: to the even 80
    quarter.insert("radio", rq);
    WifiProfile pq2;
    WifiProfile::fromJson(quarter, &pq2);
    ok("power to the quarter, a half to the even one (19.625 -> 78, 19.875 -> 80)", pq.powerQ == 78 && pq2.powerQ == 80);
    QJsonObject defaults = json(kTestJson);
    QJsonObject ap = defaults.value("ap").toObject();
    ap.remove("hidden");
    ap.remove("max_clients");
    defaults.insert("ap", ap);
    QJsonObject l1 = defaults.value("line1").toObject();
    l1.remove("xlat");
    l1.remove("lport");
    defaults.insert("line1", l1);
    defaults.remove("networks");
    WifiProfile pd;
    ok("sbpframe's defaults: hidden false, 4 clients, xlat none, lport 0, no networks",
       WifiProfile::fromJson(defaults, &pd).isEmpty() && pd.apHidden == 0 && pd.apMaxClients == 4 && pd.xlat == 0
       && pd.line1.lport == 0 && pd.networks.isEmpty());

    // ---- 3. what the app refuses before sending ---------------------------------------------------------------
    std::printf("3. Refused before sending\n");
    ok("the made-up profile: nothing", t.problems().isEmpty(), t.problems().join("; "));
    WifiProfile pp = t;
    pp.apPassword = "1234567";
    ok("the access point's password of 7 characters (WPA2+WPA3)", pp.problems().size() == 1);
    pp.apAuth = 0;
    pp.apPassword.clear();
    ok("an open access point without a password: nothing", pp.problems().isEmpty());
    pp = t;
    pp.networks[1].secure = true;
    ok("a \"secure\" network without a password", pp.problems().size() == 1);
    pp = t;
    pp.networks[0].password = "abcdefg";
    ok("a network's password of 7 characters", pp.problems().size() == 1);
    pp = t;
    pp.networks[2].password = QString(63, QLatin1Char('a')) + QLatin1Char('g');
    ok("64 characters that are not all hex digits", pp.problems().size() == 1);
    pp = t;
    pp.networks[1].ssid = "Net-A";
    ok("a network listed twice", pp.problems().size() == 1);
    pp = t;
    for (int i = 0; i < 6; ++i)
        pp.networks.append({ QStringLiteral("N%1").arg(i), QString(), false });
    ok("9 networks", pp.problems().size() == 1 && pp.encode().isEmpty());

    // ---- 4. the built-in profiles ------------------------------------------------------------------------------
    std::printf("4. The built-in profiles\n");
    const auto builtins = WifiProfileStore::builtinJson();
    ok("three: headunit, boat_kogger, boat_godio", builtins.size() == 3 && builtins[0].first == "headunit"
       && builtins[1].first == "boat_kogger" && builtins[2].first == "boat_godio");
    const QString names[] = { QString::fromUtf8("\xD0\x91\xD0\xBE\xD1\x88\xD0\xBA\xD0\xB0"),
                              QString::fromUtf8("\xD0\x9B\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xB0 Kogger"),
                              QString::fromUtf8("\xD0\x9B\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xB0 GODIO") };
    for (int i = 0; i < builtins.size() && i < 3; ++i) {
        WifiProfile b;
        const QString e = WifiProfile::fromJson(json(builtins[i].second), &b);
        bool noPassword = b.apPassword.isEmpty();
        for (const WifiProfile::Net& n : b.networks)
            noPassword = noPassword && n.password.isEmpty() && n.secure;
        ok(qPrintable(QStringLiteral("%1: read, named as the module's preset, no password in it, the passwords asked")
                          .arg(builtins[i].first)),
           e.isEmpty() && b.name == names[i] && noPassword && !b.problems().isEmpty(), e);
    }

    // ---- 5. the frames ----------------------------------------------------------------------------------------
    std::printf("5. The frames\n");
    ok("ID_WIFI_PROFILE is 0x5B", Parsers::ID_WIFI_PROFILE == 0x5B);
    IDBinWifiProfile idp;
    idp.setAddress(87);
    QVector<QByteArray> out;
    QObject::connect(&idp, &IDBin::binFrameOut, [&out](Parsers::ProtoBinOut f) {
        out.append(QByteArray(reinterpret_cast<const char*>(f.frame()), f.frameLen()));
    });
    idp.sendSetting(1, WifiProfile::beginPayload(blob));
    idp.sendSetting(2, by200[0]);
    idp.sendSetting(3, WifiProfile::applyPayload());
    idp.ask(0);
    idp.ask(3);
    ok("begin to 87 = sbpframe.encode's", out.value(0) == hex("BB 55 57 8A 5B 0A 4A 5D 6B C9 B6 00 4B 01 F0 24 37 ED"),
       show(out.value(0)));
    ok("the piece to 87 (184 bytes) = sbpframe.encode's",
       out.value(1) == hex("BB 55 57 92 5B B8 00 00") + blob + hex("65 0E"), show(out.value(1).right(4)));
    ok("apply to 87 = sbpframe.encode's", out.value(2) == hex("BB 55 57 9A 5B 04 4A 5D 6B C9 2B 02"), show(out.value(2)));
    ok("GETTING v0 / v3 to 87 = sbpframe.encode's",
       out.value(3) == hex("BB 55 57 83 5B 00 35 9B") && out.value(4) == hex("BB 55 57 9B 5B 00 4D E3"));

    // What it takes, through the app's frame parser.
    const auto kp1 = [](uint8_t mode, const QByteArray& payload, uint8_t id = 0x5B) {
        QByteArray body;
        body.append(char(87));
        body.append(char(mode));
        body.append(char(id));
        body.append(char(payload.size()));
        body.append(payload);
        uint8_t c1 = 0, c2 = 0;
        for (const char b : body) {
            c1 = uint8_t(c1 + uint8_t(b));
            c2 = uint8_t(c2 + c1);
        }
        return hex("BB 55") + body + QByteArray(1, char(c1)) + QByteArray(1, char(c2));
    };
    const auto feed = [](IDBin& id, const QByteArray& stream) {
        QVector<int> codes;
        QByteArray copy = stream;
        QMetaObject::Connection c = QObject::connect(&id, &IDBin::updateContent,
            [&codes](Parsers::Type, Parsers::Version, Parsers::Resp resp, uint8_t) { codes.append(int(resp)); });
        Parsers::FrameParser proto;
        proto.setContext(reinterpret_cast<uint8_t*>(copy.data()), static_cast<uint32_t>(copy.size()));
        while (proto.availContext() > 0) {
            proto.process();
            if (proto.isComplete() && proto.id() == id.id())
                id.parse(proto);
        }
        QObject::disconnect(c);
        return codes;
    };
    // CONTENT v0: flags 3, fingerprint 11223344, 400 of 977, "Бошка" (sbpframe.parse_profile_status's example)
    const QByteArray status = hex("03 44332211 9001 d103 0a d091d0bed188d0bad0b0");
    feed(idp, kp1(0x01, status));
    const IDBinWifiProfile::Status& st = idp.status();
    ok("CONTENT v0: loaded, changed, fingerprint, 400 of 977, the name",
       st.valid && st.flags == 3 && st.fingerprint == 0x11223344u && st.received == 400 && st.announced == 977
       && st.name == names[0]);
    WifiProfileStatus ws;
    ok("WifiProfileStatus::parse reads it the same", WifiProfileStatus::parse(reinterpret_cast<const uint8_t*>(status.constData()),
       int(status.size()), &ws) && ws.loaded && ws.modified && ws.name == names[0] && ws.received == 400);
    ok("a status shorter than its name: not taken",
       !WifiProfileStatus::parse(reinterpret_cast<const uint8_t*>(status.constData()), int(status.size()) - 1, nullptr));
    idp.reset();
    feed(idp, kp1(0x01, status.left(status.size() - 1)));
    ok("... nor by IDBinWifiProfile", !idp.status().valid);
    feed(idp, kp1(0x19, hex("0D")));
    ok("CONTENT v3 {13}: the reason", idp.lastReason() == 13);
    idp.reset();
    feed(idp, kp1(0x82 | (3 << 3), hex("4a5d6bc9")));
    ok("a SETTING v3 seen on the link is not a reason", idp.lastReason() == -1);
    const QVector<int> acks = feed(idp, kp1(0x81 | (2 << 3), hex("03 00 00")));
    ok("a confirmation of a piece: its code (ERR_PAYLOAD 3) passed on", acks == QVector<int>{ 3 });

    IDBinWifiNet net;
    net.setAddress(87);
    QByteArray line = hex("01 01 01 00000000 6d38 6e38 00c20100 0a 05 01");
    feed(net, kp1(0x01 | (3 << 3), line + hex("01 01 01"), 0x58));
    ok("ID_WIFI_NET v3 of 21 bytes (0.21): the translator saved, running, in the build",
       net.line(1).valid && net.line(1).xlatSaved == 1 && net.line(1).xlatRunning == 1 && net.line(1).xlatBuild == 1);
    feed(net, kp1(0x01 | (3 << 3), line, 0x58));
    ok("ID_WIFI_NET v3 of 18 bytes (before 0.21): the translator not known", net.line(1).valid && net.line(1).xlatSaved == -1);

    // ---- 6. the upload ----------------------------------------------------------------------------------------
    std::printf("6. The upload\n");
    QVector<Sent> sent;
    WifiProfileUpload::Timing timing;
    timing.answerMs = 100;
    timing.reasonMs = 100;
    timing.tries = 3;
    WifiProfileUpload up({ [&sent](int ver, const QByteArray& p) { sent.append({ ver, p, false }); },
                           [&sent](int ver) { sent.append({ ver, QByteArray(), true }); } }, timing);
    int outcome = -1;
    QObject::connect(&up, &WifiProfileUpload::finished, [&outcome](int o) { outcome = o; });
    const auto versions = [&sent]() {
        QString s;
        for (const Sent& x : sent)
            s += (x.ask ? QStringLiteral("g") : QStringLiteral("s")) + QString::number(x.ver)
                 + (x.ver == 2 && !x.ask ? QStringLiteral("@%1").arg(uint8_t(x.payload[0]) | (uint8_t(x.payload[1]) << 8)) : QString())
                 + QLatin1Char(' ');
        return s.trimmed();
    };
    const QByteArray big(450, 'z');               // three pieces: 200, 200, 50
    const auto begin = [&](const QByteArray& b) { sent.clear(); outcome = -1; return up.start(b); };

    begin(big);
    up.onAnswer(1, 1);
    up.onAnswer(2, 1);
    up.onAnswer(2, 1);
    up.onAnswer(2, 1);
    ok("all answered: begin, 3 pieces in order, apply", versions() == "s1 s2@0 s2@200 s2@400 s3", versions());
    up.onAnswer(3, 1);
    ok("... OK to apply: saved, 450 of 450 bytes confirmed", outcome == WifiProfileUpload::Saved && up.sent() == 450 && !up.active());
    ok("a second start while one runs is refused", begin(big) && !up.start(big));
    up.cancel();
    ok("an empty block or one over 1024 bytes is not started", !up.start(QByteArray()) && !up.start(QByteArray(1025, 'x')));

    begin(big);
    up.onAnswer(1, 1);
    up.onAnswer(2, 1);                           // piece 0
    spin(150);                                    // piece 1's answer lost: sent again
    up.onAnswer(2, 1);                           // piece 1 (its second copy, or the first one late)
    up.onAnswer(2, 1);                           // the other copy's OK, late: not piece 2's
    ok("a lost answer: the same piece again; the late OK of the copy is not the next piece's",
       versions() == "s1 s2@0 s2@200 s2@200 s2@400" && up.sent() == 400, versions());
    up.onAnswer(2, 1);
    up.onAnswer(3, 1);
    ok("... and it ends saved", outcome == WifiProfileUpload::Saved);

    begin(blob);
    up.onAnswer(1, 1);
    up.onAnswer(2, 1);
    up.onAnswer(3, 3);
    up.onReason(13);
    ok("ERR_PAYLOAD and CONTENT v3 {13}: refused, reason 13 (both UDP lines on one port)",
       outcome == WifiProfileUpload::Refused && up.reason() == 13 && up.resp() == 3);

    begin(blob);
    up.onAnswer(1, 1);
    up.onAnswer(2, 1);
    up.onAnswer(3, 3);
    spin(150);
    ok("ERR_PAYLOAD without CONTENT v3: the last reason asked (GETTING v3)", sent.last().ask && sent.last().ver == 3, versions());
    up.onReason(9);
    ok("... answered 9: refused, reason 9", outcome == WifiProfileUpload::Refused && up.reason() == 9);

    begin(blob);
    up.onAnswer(1, 1);
    up.onAnswer(2, 1);
    up.onAnswer(3, 8);
    up.onReason(16);
    ok("ERR_RUNTIME and reason 16: busy (a firmware update)", outcome == WifiProfileUpload::Busy);

    begin(blob);
    up.onAnswer(1, 1);
    up.onAnswer(2, 1);
    up.onAnswer(3, 8);
    spin(150);
    ok("ERR_RUNTIME without CONTENT v3: written halfway", outcome == WifiProfileUpload::WriteFailed);

    begin(blob);
    up.onAnswer(1, 7);
    ok("the begin refused (ERR_KEY 7): rejected, code 7", outcome == WifiProfileUpload::Rejected && up.resp() == 7);
    begin(blob);
    up.onAnswer(1, 1);
    up.onAnswer(2, 3);
    ok("a piece refused (ERR_PAYLOAD): rejected", outcome == WifiProfileUpload::Rejected && up.resp() == 3);
    begin(blob);
    spin(450);
    ok("no answer to the begin: sent 3 times, then no answer", outcome == WifiProfileUpload::NoAnswer
       && versions() == "s1 s1 s1", versions());

    WifiProfileStatus held;
    held.valid = true;
    held.received = held.announced = uint16_t(blob.size());
    begin(blob);
    up.onAnswer(1, 1);
    up.onAnswer(2, 1);
    spin(150);
    ok("apply unanswered: not sent again blindly, the status asked", versions() == "s1 s2@0 s3 g0", versions());
    up.onStatus(held);
    ok("... the upload still held whole: apply sent once more", versions() == "s1 s2@0 s3 g0 s3", versions());
    up.onAnswer(3, 1);
    ok("... OK: saved", outcome == WifiProfileUpload::Saved);

    WifiProfileStatus loaded;
    loaded.valid = true;
    loaded.loaded = true;
    loaded.name = t.name;
    begin(blob);
    up.onAnswer(1, 1);
    up.onAnswer(2, 1);
    spin(150);
    up.onStatus(loaded);
    ok("apply unanswered, the status names the profile, unchanged: saved", outcome == WifiProfileUpload::Saved);

    begin(blob);
    up.onAnswer(1, 1);
    up.onAnswer(2, 1);
    spin(150);
    up.onReason(0);
    ok("apply unanswered, CONTENT v3 {0} pushed: saved", outcome == WifiProfileUpload::Saved);

    WifiProfileStatus none;
    none.valid = true;
    begin(blob);
    up.onAnswer(1, 1);
    up.onAnswer(2, 1);
    spin(150);
    up.onStatus(none);
    ok("apply unanswered, not loaded and nothing held: the last reason asked", sent.last().ask && sent.last().ver == 3, versions());
    up.onReason(0);
    ok("... answered 0 (no refusal known): no answer, not saved", outcome == WifiProfileUpload::NoAnswer);

    begin(blob);
    up.onAnswer(1, 1);
    up.onAnswer(2, 1);
    spin(550);
    ok("apply unanswered and the module silent: no answer, the status asked 3 times",
       outcome == WifiProfileUpload::NoAnswer && versions() == "s1 s2@0 s3 g0 g0 g0", versions());

    // ---- 7. what a load changes, the rules, the files --------------------------------------------------------
    std::printf("7. Changes, rules, files\n");
    WifiProfile head, boat;
    WifiProfile::fromJson(json(builtins[0].second), &head);
    WifiProfile::fromJson(json(builtins[1].second), &boat);
    const uint32_t all = 0x7FF;
    const QVector<WifiProfileChange> ch = WifiProfileDiff::changes(head, all, boat);
    QStringList keys;
    for (const WifiProfileChange& c : ch)
        keys << c.key + (c.primary ? QStringLiteral("*") : QString());
    ok("head unit -> boat (Kogger): role, access point, networks, address, X1 first; then lines, radio",
       keys.join(' ') == "role* ap* networks* address* x1* line0 line1 radio", keys.join(' '));
    ok("nothing changes against itself", WifiProfileDiff::changes(boat, all, boat).isEmpty());
    WifiProfile boat84 = boat;
    boat84.powerQ = 84;                          // the module saves 21 dBm as 20
    ok("21 dBm against 20: no change (the module keeps 20)", WifiProfileDiff::changes(boat84, all, boat).isEmpty());
    ok("what was not read is not compared", WifiProfileDiff::changes(head, WifiProfileDiff::KnownRole, boat).size() == 1);
    ok("rate written with thin spaces", WifiProfileDiff::baudText(5000000) == QString::fromUtf8("5\xE2\x80\xAF" "000\xE2\x80\xAF" "000"));

    ok("support: 0.21 no, 0.22 yes, unknown -1, 1.0 yes",
       WifiProfileRules::supportFor(0, 21) == 0 && WifiProfileRules::supportFor(0, 22) == 1
       && WifiProfileRules::supportFor(0, 0) == -1 && WifiProfileRules::supportFor(1, 0) == 1);
    ok("CP2105 (10C4:EA70) 2 Mbaud; CP2102 (10C4:EA60) not known",
       WifiProfileRules::adapterMaxBaud(0x10C4, 0xEA70) == 2000000 && WifiProfileRules::adapterMaxBaud(0x10C4, 0xEA60) == 0);
    ok("the port's rate after: X1 the profile's x1_baud, X2 line 1's, the network none",
       WifiProfileRules::portBaudAfter(boat, 0) == 5000000 && WifiProfileRules::portBaudAfter(boat, 1) == 921600
       && WifiProfileRules::portBaudAfter(boat, -1) == 0);

    QTemporaryDir tmp;
    WifiProfileStore::setFolderForCheck(tmp.path() + QStringLiteral("/wifi_profiles"));
    ok("the store: the three built in, no file yet", WifiProfileStore::list().size() == 3);
    QString id;
    ok("a profile saved as a file", WifiProfileStore::save(t, &id).isEmpty() && id == QStringLiteral("file:") + t.name + ".json", id);
    WifiProfileStore::Entry e;
    ok("... read back the same", WifiProfileStore::find(id, &e) && e.error.isEmpty() && e.profile == t);
    QString id2;
    WifiProfileStore::save(t, &id2);
    ok("the same name again: another file", id2 != id && WifiProfileStore::list().size() == 5, id2);
    const QString moduleFile = tmp.path() + QStringLiteral("/from_module.json");
    QFile mf(moduleFile);
    mf.open(QIODevice::WriteOnly);
    mf.write(kTestJson);
    mf.close();
    QString id3;
    ok("a file in the module project's form added", WifiProfileStore::import(QDir::toNativeSeparators(moduleFile), &id3).isEmpty()
       && WifiProfileStore::list().size() == 6);
    QFile junk(tmp.path() + QStringLiteral("/wifi_profiles/junk.json"));
    junk.open(QIODevice::WriteOnly);
    junk.write("{\"name\": 1}");
    junk.close();
    bool junkSaid = false;
    for (const WifiProfileStore::Entry& x : WifiProfileStore::list())
        junkSaid = junkSaid || (x.id == "file:junk.json" && x.error.startsWith("name"));
    ok("a broken file is listed with what is wrong", junkSaid);
    ok("a user's file removed; a built-in one cannot be", WifiProfileStore::remove(id2) && !WifiProfileStore::remove("builtin:headunit")
       && WifiProfileStore::list().size() == 6);
    ok("a file name with a path in it is not removed", !WifiProfileStore::remove("file:../x.json"));

    std::printf("%s: %d failed\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
