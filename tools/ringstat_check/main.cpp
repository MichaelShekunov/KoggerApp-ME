// Standalone check of a GODIO ring board's sync counters on the protocol side (ID_RING_STAT = 0x76, IDBinRingStat),
// with the application's own class, to the contract in L412_GODIO docs/RING_HOLES_2026-10-23.md, section 8:
//
//  1. CONTENT v0, 32 bytes little-endian: [address][role][a board before][spare][guard us u32][BREAK interval us u32]
//     [relayed u32][BREAKs u32][torn u32][leads on a timeout u32][skipped u32]; the mode byte may carry the mark (0x41).
//  2. A short frame and another version are refused and change nothing.
//  3. read() sends GETTING v0 with no payload; reset() SETTING v0 with the key 0xC96B5D4A; nothing is asked on its own.
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

constexpr uint32_t kKey = 0xC96B5D4Au;

// KP1: BB 55, route, mode (type in bits 0-1, version in bits 3-5, the mark in bit 6), id, length, payload, Fletcher-8.
QByteArray kp1(uint8_t route, uint8_t type, uint8_t ver, bool mark, uint8_t id, const QByteArray& payload)
{
    QByteArray body;
    body.append(char(route));
    body.append(char((type & 0x3) | ((ver & 0x7) << 3) | (mark ? 0x40 : 0)));
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

QByteArray state(uint8_t addr, uint8_t role, uint8_t up, uint32_t guard, uint32_t interval, uint32_t fwd, uint32_t brk,
                 uint32_t cut, uint32_t tmo, uint32_t skip)
{
    QByteArray p;
    p.append(char(addr));
    p.append(char(role));
    p.append(char(up));
    p.append(char(0));
    for (uint32_t v : { guard, interval, fwd, brk, cut, tmo, skip }) {
        le(p, v, 4);
    }
    return p;
}

Parsers::Resp feed(IDBinRingStat& rs, const QByteArray& frame)
{
    Parsers::Resp r = Parsers::respNone;
    QByteArray copy = frame;
    Parsers::FrameParser proto;
    proto.setContext(reinterpret_cast<uint8_t*>(copy.data()), static_cast<uint32_t>(copy.size()));
    while (proto.availContext() > 0) {
        proto.process();
        if (proto.isComplete() && proto.id() == Parsers::ID_RING_STAT) {
            r = rs.parse(proto);
        }
    }
    return r;
}

struct Sent {
    int route = -1, type = -1, ver = -1, id = -1, len = -1;
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
    s.route = b[2];
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

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ok("ID_RING_STAT is 0x76", Parsers::ID_RING_STAT == 0x76);

    std::printf("\nthe board's state\n");
    IDBinRingStat rs;
    ok("nothing known before a frame", !rs.state().known);
    const QByteArray s1 = state(2, 1, 1, 7600, 49887, 1203, 1197, 3, 1, 6);
    ok("the contract's frame is 32 bytes", s1.size() == 32, QString::number(s1.size()));
    ok("CONTENT v0 with the mark (mode 0x41): taken", feed(rs, kp1(2, Parsers::CONTENT, 0, true, 0x76, s1)) == Parsers::respOk);
    const IDBinRingStat::State& st = rs.state();
    ok("known; address 2, follower, a board before it", st.known && st.address == 2 && st.role == 1 && st.upstream == 1,
       QString("address %1 role %2 upstream %3").arg(st.address).arg(st.role).arg(st.upstream));
    ok("guard 7600 us, BREAK interval 49887 us", st.guardUs == 7600 && st.breakIntervalUs == 49887,
       QString("%1 / %2").arg(st.guardUs).arg(st.breakIntervalUs));
    ok("relayed 1203, BREAKs 1197, torn 3, leads 1, skipped 6",
       st.relayed == 1203 && st.breaks == 1197 && st.torn == 3 && st.timeouts == 1 && st.skipped == 6,
       QString("%1 %2 %3 %4 %5").arg(st.relayed).arg(st.breaks).arg(st.torn).arg(st.timeouts).arg(st.skipped));
    IDBinRingStat big;
    feed(big, kp1(1, Parsers::CONTENT, 0, false, 0x76, state(1, 0, 0, 344000, 0, 0xFFFFFFFFu, 0, 0, 0, 0)));
    ok("without the mark too; a guard past 16 bits (344 ms at 9600), a full u32",
       big.state().known && big.state().guardUs == 344000 && big.state().relayed == 0xFFFFFFFFu && big.state().role == 0);

    IDBinRingStat second;
    feed(second, kp1(2, Parsers::CONTENT, 0, true, 0x76, state(2, 0, 1, 7600, 0, 0, 0, 0, 3, 0)));
    ok("leader after a timeout, a board still before it: role 0, upstream 1",
       second.state().role == 0 && second.state().upstream == 1 && second.state().timeouts == 3,
       QString("role %1 upstream %2").arg(second.state().role).arg(second.state().upstream));

    std::printf("\nframes that are not right\n");
    IDBinRingStat bad;
    ok("31 bytes: payload refused", feed(bad, kp1(1, Parsers::CONTENT, 0, true, 0x76, s1.left(31))) == Parsers::respErrorPayload);
    ok("CONTENT v1: version refused", feed(bad, kp1(1, Parsers::CONTENT, 1, true, 0x76, s1)) == Parsers::respErrorVersion);
    ok("and neither taken", !bad.state().known);

    std::printf("\nwhat the app sends\n");
    QVector<QByteArray> out;
    QObject::connect(&rs, &IDBin::binFrameOut, [&out](Parsers::ProtoBinOut f) {
        out.append(QByteArray(reinterpret_cast<const char*>(f.frame()), f.frameLen()));
    });
    rs.setAddress(2);
    rs.requestAll();
    rs.startColdStartTimer();
    ok("nothing on its own (requestAll, cold start)", out.isEmpty(), QString("%1 frames").arg(out.size()));
    rs.read();
    Sent a = sentFrame(out.value(0));
    ok("read: GETTING v0 to 0x76 at route 2, no payload, checksum right",
       out.size() == 1 && a.type == Parsers::GETTING && a.ver == 0 && a.id == 0x76 && a.route == 2 && a.len == 0 && a.sum,
       QString("type %1 v%2 route %3 len %4").arg(a.type).arg(a.ver).arg(a.route).arg(a.len));
    out.clear();
    rs.reset();
    a = sentFrame(out.value(0));
    const uint32_t key = a.payload.size() == 4 ? uint32_t(uint8_t(a.payload[0])) | (uint32_t(uint8_t(a.payload[1])) << 8)
                                                     | (uint32_t(uint8_t(a.payload[2])) << 16) | (uint32_t(uint8_t(a.payload[3])) << 24)
                                               : 0;
    ok("reset: SETTING v0 to 0x76 at route 2, [the key], checksum right",
       out.size() == 1 && a.type == Parsers::SETTING && a.ver == 0 && a.id == 0x76 && a.route == 2 && a.len == 4
       && key == kKey && a.sum, QString("type %1 v%2 len %3 key 0x%4").arg(a.type).arg(a.ver).arg(a.len).arg(key, 8, 16, QChar('0')));

    std::printf("\nringstat_check: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
