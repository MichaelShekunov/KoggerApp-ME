// Standalone check of the sonar presets' protocol side (ID_PRESET = 0x7C, IDBinPreset), with the application's own
// classes, to the contract of the l412_adc project's docs/PRESETS_CONCEPT_2026-10-21.md, section 5:
//
//  1. CONTENT v0, 41 bytes, one a slot: [slot][flags: bit 0 used, bit 1 factory][freq kHz u16][pulses][boost][dsp]
//     [cells u16][cell mm u16][offset u16][period ms u32][name 24 B UTF-8, padded with zeros] -- the frames built
//     here byte by byte, not with the application's frame writer.
//  2. What the app sends: recall SETTING v0 [slot]; save SETTING v1 [slot][key][name 24 B] with the key for the
//     factory slots 0..1 and zeros for the others; delete SETTING v2 [slot][key].
//  3. The name cut to 24 bytes on a character boundary.
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

void le(QByteArray& p, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i) {
        p.append(char((v >> (8 * i)) & 0xFF));
    }
}

QByteArray slotPayload(uint8_t slot, uint8_t flags, uint16_t freq, uint8_t pulses, uint8_t boost, uint8_t dsp,
                       uint16_t cells, uint16_t cellMm, uint16_t offset, uint32_t period, const QByteArray& name)
{
    QByteArray p;
    p.append(char(slot));
    p.append(char(flags));
    le(p, freq, 2);
    p.append(char(pulses));
    p.append(char(boost));
    p.append(char(dsp));
    le(p, cells, 2);
    le(p, cellMm, 2);
    le(p, offset, 2);
    le(p, period, 4);
    QByteArray n = name.left(24);
    n.append(QByteArray(24 - n.size(), '\0'));
    p.append(n);
    return p;
}

QVector<Parsers::Resp> feed(IDBinPreset& pr, const QByteArray& stream)
{
    QVector<Parsers::Resp> r;
    QByteArray copy = stream;
    Parsers::FrameParser proto;
    proto.setContext(reinterpret_cast<uint8_t*>(copy.data()), static_cast<uint32_t>(copy.size()));
    while (proto.availContext() > 0) {
        proto.process();
        if (proto.isComplete() && proto.id() == Parsers::ID_PRESET) {
            r.append(pr.parse(proto));
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

uint32_t u32le(const QByteArray& p, int at)
{
    const auto* b = reinterpret_cast<const uint8_t*>(p.constData()) + at;
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString narrow = QString::fromUtf16(u"Узкий луч");                // 9 letters
    const QString wide = QString::fromUtf16(u"Широкий луч");     // 21 bytes
    ok("ID_PRESET is 0x7C", Parsers::ID_PRESET == 0x7C);

    std::printf("\nthe eight slot frames of a GET, built to the contract\n");
    IDBinPreset pr;
    QByteArray stream;
    stream.append(kp1(Parsers::CONTENT, 0, 0x7C, slotPayload(0, 0x03, 815, 25, 0, 5, 4000, 10, 0, 50, narrow.toUtf8())));
    stream.append(kp1(Parsers::CONTENT, 0, 0x7C, slotPayload(1, 0x03, 450, 30, 1, 0, 3000, 20, 100, 100, wide.toUtf8())));
    for (int s = 2; s < 8; ++s) {
        stream.append(kp1(Parsers::CONTENT, 0, 0x7C, slotPayload(uint8_t(s), 0, 0, 0, 0, 0, 0, 0, 0, 0, QByteArray())));
    }
    const QVector<Parsers::Resp> r = feed(pr, stream);
    ok("eight frames, every one parsed", r.size() == 8 && !r.contains(Parsers::respErrorPayload) && !r.contains(Parsers::respErrorVersion),
       QString("%1 frames").arg(r.size()));
    const IDBinPreset::Slot& s0 = pr.slot(0);
    const IDBinPreset::Slot& s1 = pr.slot(1);
    ok("slot 0: used, factory", s0.known && s0.used && s0.factory);
    ok("slot 0: 815 kHz, 25 pulses, boost 0, DSP 5", s0.freqKhz == 815 && s0.pulses == 25 && s0.boost == 0 && s0.dsp == 5,
       QString("%1 kHz %2 pulses boost %3 DSP %4").arg(s0.freqKhz).arg(s0.pulses).arg(s0.boost).arg(s0.dsp));
    ok("slot 0: 4000 cells of 10 mm, offset 0, period 50 ms", s0.cells == 4000 && s0.cellMm == 10 && s0.offset == 0 && s0.periodMs == 50,
       QString("%1 cells %2 mm offset %3 period %4").arg(s0.cells).arg(s0.cellMm).arg(s0.offset).arg(s0.periodMs));
    ok("slot 0: the name", s0.name == narrow, s0.name);
    ok("slot 1: 450 kHz, 30 pulses, boost 1, DSP 0", s1.freqKhz == 450 && s1.pulses == 30 && s1.boost == 1 && s1.dsp == 0);
    ok("slot 1: 3000 cells of 20 mm, offset 100, period 100 ms", s1.cells == 3000 && s1.cellMm == 20 && s1.offset == 100 && s1.periodMs == 100);
    ok("slot 1: the name (21 bytes of UTF-8)", s1.name == wide && wide.toUtf8().size() == 21, s1.name);
    bool emptyOk = true;
    for (int s = 2; s < 8; ++s) {
        emptyOk = emptyOk && pr.slot(s).known && !pr.slot(s).used && !pr.slot(s).factory && pr.slot(s).name.isEmpty();
    }
    ok("slots 2..7: known, unused, no name", emptyOk);
    ok("the last slot parsed is 7", pr.lastSlot() == 7, QString::number(pr.lastSlot()));

    std::printf("\nframes that are not right\n");
    IDBinPreset bad;
    ok("CONTENT v1: version refused", feed(bad, kp1(Parsers::CONTENT, 1, 0x7C, slotPayload(0, 1, 1, 1, 1, 1, 1, 1, 1, 1, "x"))).value(0) == Parsers::respErrorVersion);
    ok("40 bytes: payload refused", feed(bad, kp1(Parsers::CONTENT, 0, 0x7C, slotPayload(0, 1, 1, 1, 1, 1, 1, 1, 1, 1, "x").left(40))).value(0) == Parsers::respErrorPayload);
    ok("slot 8: payload refused", feed(bad, kp1(Parsers::CONTENT, 0, 0x7C, slotPayload(8, 1, 1, 1, 1, 1, 1, 1, 1, 1, "x"))).value(0) == Parsers::respErrorPayload);
    ok("and none of them taken", bad.lastSlot() == -1 && !bad.slot(0).known);
    IDBinPreset full;
    feed(full, kp1(Parsers::CONTENT, 0, 0x7C, slotPayload(4, 1, 1, 1, 1, 1, 1, 1, 1, 1, "ABCDEFGHIJKLMNOPQRSTUVWX")));
    ok("a name of all 24 bytes, no zero after it", full.slot(4).name == QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWX"), full.slot(4).name);

    std::printf("\nwhat the app sends\n");
    QVector<QByteArray> out;
    QObject::connect(&pr, &IDBin::binFrameOut, [&out](Parsers::ProtoBinOut f) {
        out.append(QByteArray(reinterpret_cast<const char*>(f.frame()), f.frameLen()));
    });
    pr.recall(1);
    Sent a = sentFrame(out.value(0));
    ok("recall 1: SETTING v0 to 0x7C, [1], checksum right", a.type == Parsers::SETTING && a.ver == 0 && a.id == 0x7C && a.len == 1
       && a.payload == QByteArray(1, char(1)) && a.sum, QString("type %1 v%2 len %3").arg(a.type).arg(a.ver).arg(a.len));

    out.clear();
    pr.save(0, narrow);
    a = sentFrame(out.value(0));
    QByteArray name0 = narrow.toUtf8();
    name0.append(QByteArray(24 - name0.size(), '\0'));
    ok("save 0: SETTING v1, 29 bytes, checksum right", a.type == Parsers::SETTING && a.ver == 1 && a.id == 0x7C && a.len == 29 && a.sum,
       QString("type %1 v%2 len %3").arg(a.type).arg(a.ver).arg(a.len));
    ok("save 0: [slot 0][the key][the name padded to 24]", a.payload.size() == 29 && a.payload.at(0) == 0 && u32le(a.payload, 1) == kKey
       && a.payload.mid(5) == name0, QString("key 0x%1").arg(a.payload.size() == 29 ? u32le(a.payload, 1) : 0, 8, 16, QChar('0')));
    out.clear();
    pr.save(1, wide);
    a = sentFrame(out.value(0));
    ok("save 1: the key too (factory slot)", a.payload.size() == 29 && a.payload.at(0) == 1 && u32le(a.payload, 1) == kKey);
    out.clear();
    pr.save(3, QStringLiteral("x"));
    a = sentFrame(out.value(0));
    ok("save 3: no key, zeros in its place", a.payload.size() == 29 && a.payload.at(0) == 3 && u32le(a.payload, 1) == 0
       && a.payload.mid(5, 2) == QByteArray("x\0", 2));

    // "A" and 15 Cyrillic letters: 31 bytes; the 12th letter takes bytes 23..24, so the cut at 24 keeps 11.
    QString longName = QStringLiteral("A");
    for (int i = 0; i < 15; ++i) {
        longName += QChar(0x0430 + i);
    }
    const QByteArray cut = IDBinPreset::nameBytes(longName);
    ok("a long name: cut to 23 bytes, before the letter that would not fit", cut.size() == 23 && cut == longName.left(12).toUtf8(),
       QString("%1 bytes").arg(cut.size()));
    ok("and it reads back as text", QString::fromUtf8(cut) == longName.left(12));
    out.clear();
    pr.save(2, longName);
    a = sentFrame(out.value(0));
    ok("save of it: the 23 bytes and one zero", a.payload.size() == 29 && a.payload.mid(5) == cut + QByteArray(1, '\0'));
    ok("spaces round a name are not kept", IDBinPreset::nameBytes(QStringLiteral("  x  ")) == QByteArray("x"));

    out.clear();
    pr.remove(5);
    a = sentFrame(out.value(0));
    ok("delete 5: SETTING v2, [5][the key]", a.type == Parsers::SETTING && a.ver == 2 && a.len == 5 && a.sum && a.payload.at(0) == 5
       && u32le(a.payload, 1) == kKey);
    out.clear();
    pr.recall(8);
    pr.save(-1, QStringLiteral("x"));
    pr.remove(9);
    ok("no frame for a slot outside 0..7", out.isEmpty(), QString("%1 frames").arg(out.size()));

    std::printf("\npreset_check: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
