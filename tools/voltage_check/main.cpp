// Standalone check of the supply-voltage protocol side, with the application's own classes.
//
//  1. The "Supply voltage" switch (DevDriver::setDatasetVoltage) sets DATASET bit 24,
//     IDBinDataset::MASK_VOLTAGE_V0, on the channel it is given, clears it on the other, and
//     clears it everywhere when given 0 -- without touching the other bits of the mask.
//  2. commit() sends that mask in ID_DATASET SETTING v0: u8 channel, u32 period, u32 mask.
//  3. IDBinVoltage reads ID_VOLTAGE v0 CONTENT (u8 channel, s32 millivolts), the frames built here
//     byte by byte to the contract, not with the application's own frame writer.
//
// The rest of the path (DevDriver -> DeviceManager -> Dataset -> the widget) is checked live by
// voltage_live.ps1. Exit 0 = pass, 1 = fail.
#include <QCoreApplication>
#include <QObject>
#include <QVector>
#include <cstdio>

#include "id_binnary.h"

namespace {

int fails = 0;
void ok(const char* name, bool good, const QString& detail = QString())
{
    std::printf("  %-62s %s %s\n", name, good ? "ok" : "FAIL", detail.toUtf8().constData());
    if (!good) {
        ++fails;
    }
}

// KP1: BB 55, route, mode (type in bits 0-1, version in bits 3-5), id, length, payload, and the
// Fletcher-8 pair over route..payload.
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

QByteArray voltagePayload(uint8_t ch, int32_t value)
{
    QByteArray p;
    p.append(char(ch));
    for (int i = 0; i < 4; ++i) {
        p.append(char((uint32_t(value) >> (8 * i)) & 0xFF));
    }
    return p;
}

struct Parsed {
    int frames = 0;
    QVector<int> ids;
    QVector<int> raws;
    QVector<Parsers::Resp> resps;
};

Parsed feed(IDBinVoltage& v, const QByteArray& stream)
{
    Parsed r;
    QByteArray copy = stream;
    Parsers::FrameParser proto;
    proto.setContext(reinterpret_cast<uint8_t*>(copy.data()), static_cast<uint32_t>(copy.size()));
    while (proto.availContext() > 0) {
        proto.process();
        if (!proto.isComplete() || proto.id() != Parsers::ID_VOLTAGE) {
            continue;
        }
        ++r.frames;
        r.resps.append(v.parse(proto));
        r.ids.append(v.lastId());
        r.raws.append(v.lastRaw());
    }
    return r;
}

uint32_t u32le(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const uint32_t V = IDBinDataset::MASK_VOLTAGE_V0;

    std::printf("the mask bit\n");
    ok("MASK_VOLTAGE_V0 is bit 24", V == (1u << 24), QString("0x%1").arg(V, 8, 16, QChar('0')));
    const uint32_t others = 0xFFu | IDBinDataset::MASK_CHART_Z_V0 | IDBinDataset::MASK_TAP_IQ_V0
                            | IDBinDataset::MASK_TAP_RAW_V0;
    ok("no other dataset bit shares it", (V & others) == 0);

    IDBinDataset ds;
    ok("off at the start", !ds.getVoltage_v0(1) && !ds.getVoltage_v0(2));
    ds.setTemp_v0(1);
    ds.setVoltage_v0(1);
    ok("on channel 1: set there, not on 2", ds.getVoltage_v0(1) && !ds.getVoltage_v0(2));
    ok("the temperature bit set before stays", ds.getTemp_v0(1),
       QString("mask 0x%1").arg(ds.mask(1), 8, 16, QChar('0')));
    ok("channel 1 mask is exactly temperature | voltage", ds.mask(1) == (uint32_t(IDBinDataset::MASK_TEMP_V0) | V),
       QString("0x%1").arg(ds.mask(1), 8, 16, QChar('0')));
    ds.setVoltage_v0(2);
    ok("moved to channel 2: cleared on 1", ds.getVoltage_v0(2) && !ds.getVoltage_v0(1));
    ds.setVoltage_v0(0);
    ok("0 turns it off on both channels", !ds.getVoltage_v0(1) && !ds.getVoltage_v0(2));
    ok("and leaves the temperature bit", ds.getTemp_v0(1));

    std::printf("\nthe frame commit() sends\n");
    ds.setVoltage_v0(1);
    QVector<QByteArray> out;
    QObject::connect(&ds, &IDBin::binFrameOut, [&out](Parsers::ProtoBinOut f) {
        out.append(QByteArray(reinterpret_cast<const char*>(f.frame()), f.frameLen()));
    });
    ds.commit();
    ok("one SETTING frame a channel", out.size() == 2, QString("%1 frames").arg(out.size()));
    bool found = false;
    for (const QByteArray& f : out) {
        const auto* b = reinterpret_cast<const uint8_t*>(f.constData());
        if (f.size() != 6 + 9 + 2 || b[0] != 0xBB || b[1] != 0x55 || b[4] != Parsers::ID_DATASET) {
            continue;
        }
        uint8_t c1 = 0, c2 = 0;
        for (int i = 2; i < 6 + 9; ++i) {
            c1 = uint8_t(c1 + b[i]);
            c2 = uint8_t(c2 + c1);
        }
        const bool sum = (c1 == b[15] && c2 == b[16]);
        const int type = b[3] & 0x3, ver = (b[3] >> 3) & 0x7;
        const uint8_t ch = b[6];
        const uint32_t mask = u32le(b + 11);
        if (ch == 1) {
            found = true;
            ok("ID_DATASET SETTING v0, checksum right", type == Parsers::SETTING && ver == 0 && sum,
               QString("type %1 v%2").arg(type).arg(ver));
            ok("channel 1 carries bit 24", (mask & V) != 0, QString("mask 0x%1").arg(mask, 8, 16, QChar('0')));
        } else if (ch == 2) {
            ok("channel 2 does not", (mask & V) == 0, QString("mask 0x%1").arg(mask, 8, 16, QChar('0')));
        }
    }
    ok("the channel 1 frame is there", found);

    std::printf("\nID_VOLTAGE, built to the contract\n");
    IDBinVoltage v;
    QByteArray stream;
    stream.append(kp1(Parsers::CONTENT, 0, Parsers::ID_VOLTAGE, voltagePayload(1, 0x47)));
    stream.append(kp1(Parsers::CONTENT, 0, Parsers::ID_VOLTAGE, voltagePayload(0, 12345)));
    stream.append(kp1(Parsers::CONTENT, 0, Parsers::ID_VOLTAGE, voltagePayload(20, -936)));
    stream.append(kp1(Parsers::CONTENT, 0, Parsers::ID_VOLTAGE, voltagePayload(0, 4325)));
    const Parsed p = feed(v, stream);
    ok("four frames parsed", p.frames == 4, QString("%1").arg(p.frames));
    const QVector<int> wantIds{1, 0, 20, 0}, wantRaw{0x47, 12345, -936, 4325};
    ok("channels in order", p.ids == wantIds);
    ok("values in order, signed", p.raws == wantRaw);
    ok("the last supply reading is kept for channel 0", int(v.voltage(0)) == 4325, QString("%1").arg(v.voltage(0)));

    IDBinVoltage w;
    const Parsed q = feed(w, kp1(Parsers::CONTENT, 1, Parsers::ID_VOLTAGE, voltagePayload(0, 777)));
    ok("version 1 is refused, not misread", q.frames == 1 && !q.resps.isEmpty() && q.resps.first() == Parsers::respErrorVersion
                                                && w.lastRaw() != 777);

    std::printf("\n%s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
