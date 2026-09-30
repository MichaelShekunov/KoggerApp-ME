#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdint>

/**
 * @brief What a link carries, per second, for the log (owner 2026-09-30: with the sonar off the boat still sent
 * 17-18 KB/s - what is it?).
 *
 * Every complete frame the parser gives is counted under its kind: the protocol, the device address and the message
 * ID (KP2 with its proxy and stream flags). A frame parsed out of a proxy frame's content (FrameParser::isNested) is
 * counted apart, as what the proxies carry: its bytes are in the proxy frame's already. summary() names the heaviest
 * kinds and starts over.
 */
class TrafficTally
{
public:
    enum Proto { ProtoKp1 = 1, ProtoKp2, ProtoMavlink1, ProtoMavlink2, ProtoNmea, ProtoUbx, ProtoOther };
    struct Key {
        int proto = ProtoOther;
        int addr = -1;          // the device address (KP1 route, KP2 address option), -1 none
        uint32_t id = 0;        // the Kogger ID, the MAVLink message ID
        int ver = 0;
        bool proxy = false;     // a KP2 frame carrying other frames
        bool stream = false;    // a KP2 stream frame
    };

    void add(const Key& k, int bytes, bool nested)
    {
        Sum& s = (nested ? inner_ : outer_)[pack(k)];
        s.bytes += bytes;
        ++s.frames;
        if (!nested)
            total_ += bytes;
    }

    bool empty() const { return outer_.isEmpty() && inner_.isEmpty(); }
    qint64 totalBytes() const { return total_; }

    /** "5.1 KB/s: KP2 [250] id 12 v0 proxy 3.9 KB/s 31/s; ... | in proxy frames: MAVLink2 msg 30 1.2 KB/s 10/s; ...",
     *  the top kinds by bytes; then everything counted starts over. */
    QString summary(double seconds, int top = 10)
    {
        const double s = seconds > 0 ? seconds : 1.0;
        QString out = QStringLiteral("%1 KB/s").arg(total_ / s / 1024.0, 0, 'f', 2);
        const QString a = list(outer_, s, top);
        if (!a.isEmpty())
            out += QStringLiteral(": ") + a;
        const QString b = list(inner_, s, top);
        if (!b.isEmpty())
            out += QStringLiteral(" | in proxy frames: ") + b;
        outer_.clear();
        inner_.clear();
        total_ = 0;
        return out;
    }

    static QString name(const Key& k)
    {
        switch (k.proto) {
        case ProtoKp1:
            return QStringLiteral("KP1 [%1] %2 v%3").arg(k.addr).arg(kogger(k.id)).arg(k.ver);
        case ProtoKp2:
            return QStringLiteral("KP2 [%1] %2 v%3%4%5").arg(k.addr).arg(kogger(k.id)).arg(k.ver)
                .arg(k.proxy ? QStringLiteral(" proxy") : QString(), k.stream ? QStringLiteral(" stream") : QString());
        case ProtoMavlink1:
            return QStringLiteral("MAVLink1 msg %1").arg(k.id);
        case ProtoMavlink2:
            return QStringLiteral("MAVLink2 msg %1").arg(k.id);
        case ProtoNmea:
            return QStringLiteral("NMEA");
        case ProtoUbx:
            return QStringLiteral("UBX %1-%2").arg(k.id >> 8, 2, 16, QLatin1Char('0')).arg(k.id & 0xFF, 2, 16, QLatin1Char('0'));
        }
        return QStringLiteral("other");
    }

private:
    struct Sum { qint64 bytes = 0; qint64 frames = 0; };

    // proto 4 bits, flags 2, ver 3, addr 9 (+1: -1 is 0), id 32
    static quint64 pack(const Key& k)
    {
        return (quint64(k.proto & 0xF) << 50) | (quint64(k.proxy) << 49) | (quint64(k.stream) << 48)
             | (quint64(k.ver & 0x7) << 45) | (quint64((k.addr + 1) & 0x1FF) << 36) | quint64(k.id);
    }
    static Key unpack(quint64 v)
    {
        Key k;
        k.proto = int((v >> 50) & 0xF);
        k.proxy = (v >> 49) & 1;
        k.stream = (v >> 48) & 1;
        k.ver = int((v >> 45) & 0x7);
        k.addr = int((v >> 36) & 0x1FF) - 1;
        k.id = uint32_t(v & 0xFFFFFFFFu);
        return k;
    }
    static QString kogger(uint32_t id)
    {
        switch (id) {
        case 0x01: return QStringLiteral("TIMESTAMP");
        case 0x02: return QStringLiteral("DIST");
        case 0x03: return QStringLiteral("CHART");
        case 0x04: return QStringLiteral("ATTITUDE");
        case 0x05: return QStringLiteral("TEMP");
        case 0x06: return QStringLiteral("IMU");
        case 0x10: return QStringLiteral("DATASET");
        case 0x20: return QStringLiteral("VERSION");
        case 0x21: return QStringLiteral("MARK");
        case 0x26: return QStringLiteral("RECORDER_STATUS");
        case 0x31: return QStringLiteral("VOLTAGE");
        case 0x40: return QStringLiteral("STREAM");
        case 0x57: return QStringLiteral("WIFI");
        case 0x58: return QStringLiteral("WIFI_NET");
        case 0x59: return QStringLiteral("WIFI_SURVEY");
        case 0x64: return QStringLiteral("NAV");
        case 0x7D: return QStringLiteral("CHART_Z");
        }
        return QStringLiteral("id 0x%1").arg(id, 2, 16, QLatin1Char('0'));
    }
    static QString list(const QHash<quint64, Sum>& h, double s, int top)
    {
        QList<quint64> keys = h.keys();
        std::sort(keys.begin(), keys.end(), [&h](quint64 a, quint64 b) { return h[a].bytes > h[b].bytes; });
        QStringList parts;
        for (int i = 0; i < keys.size() && i < top; ++i) {
            const Sum& x = h[keys[i]];
            parts << QStringLiteral("%1 %2 KB/s %3/s").arg(name(unpack(keys[i])))
                         .arg(x.bytes / s / 1024.0, 0, 'f', 2).arg(x.frames / s, 0, 'f', 1);
        }
        return parts.join(QStringLiteral("; "));
    }

    QHash<quint64, Sum> outer_, inner_;
    qint64 total_ = 0;
};
