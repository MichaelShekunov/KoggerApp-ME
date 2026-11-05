#include "wifi_profile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStandardPaths>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

namespace {

const char* const kRoles[] = { "sta", "ap" };
const char* const kAuths[] = { "open", "wpa2", "wpa2wpa3" };
const char* const kModes[] = { "off", "udp", "tcp" };
const char* const kDests[] = { "fixed", "senders", "broadcast" };
const char* const kXlats[] = { "none", "telemetry" };
const char* const kPowerSave[] = { "none", "min", "max" };
struct RadioMode { uint8_t code; const char* name; };
const RadioMode kRadioModes[] = { { 0x01, "b" }, { 0x03, "bg" }, { 0x07, "bgn" }, { 0x08, "lr" }, { 0x0F, "bgnlr" } };
const char* const kReasonNames[] = { "ok", "incomplete", "crc", "format", "name", "role", "address", "x1_baud", "radio",
                                     "ap", "ip", "line0", "line1", "lines", "xlat", "nets", "busy" };

template <size_t N>
int indexOf(const char* const (&names)[N], const QString& s)
{
    for (size_t i = 0; i < N; ++i)
        if (s == QLatin1String(names[i]))
            return int(i);
    return -1;
}

template <size_t N>
QJsonValue nameOr(const char* const (&names)[N], int v)
{
    return v >= 0 && size_t(v) < N ? QJsonValue(QString::fromLatin1(names[v])) : QJsonValue(v);
}

// The first problem of a JSON profile is kept; later reads return harmless values.
class JsonReader
{
public:
    QString error;

    void fail(const QString& path, const QString& what)
    {
        if (error.isEmpty())
            error = QStringLiteral("%1: %2").arg(path, what);
    }
    QJsonObject object(const QJsonObject& o, const char* key, const QString& path)
    {
        const QJsonValue v = o.value(QLatin1String(key));
        if (!v.isObject())
            fail(path + QLatin1String(key), QStringLiteral("an object is needed"));
        return v.toObject();
    }
    // An integer lo..hi; `def` when the key is absent (none: required). A boolean counts as 0/1 where `boolOk`.
    long long integer(const QJsonObject& o, const char* key, const QString& path, long long lo, long long hi,
                      std::optional<long long> def = std::nullopt, bool boolOk = false)
    {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined() || v.isNull()) {
            if (def)
                return *def;
            fail(path + QLatin1String(key), QStringLiteral("missing"));
            return lo;
        }
        if (boolOk && v.isBool())
            return v.toBool() ? 1 : 0;
        const double d = v.toDouble(std::nan(""));
        if (!v.isDouble() || std::floor(d) != d || d < double(lo) || d > double(hi)) {
            fail(path + QLatin1String(key), QStringLiteral("a whole number %1..%2 is needed").arg(lo).arg(hi));
            return lo;
        }
        return (long long)d;
    }
    // One of the names, or its number (0..255) as the module project's tools take it.
    template <size_t N>
    int named(const QJsonObject& o, const char* key, const QString& path, const char* const (&names)[N],
              std::optional<int> def = std::nullopt)
    {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined() || v.isNull()) {
            if (def)
                return *def;
            fail(path + QLatin1String(key), QStringLiteral("missing"));
            return 0;
        }
        if (v.isString()) {
            const int i = indexOf(names, v.toString());
            if (i < 0)
                fail(path + QLatin1String(key), QStringLiteral("unknown \"%1\"").arg(v.toString()));
            return qMax(0, i);
        }
        return int(integer(o, key, path, 0, 255));
    }
    QString text(const QJsonObject& o, const char* key, const QString& path, std::optional<QString> def = std::nullopt)
    {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined() || v.isNull()) {
            if (def)
                return *def;
            fail(path + QLatin1String(key), QStringLiteral("missing"));
            return QString();
        }
        if (!v.isString())
            fail(path + QLatin1String(key), QStringLiteral("text is needed"));
        return v.toString();
    }
    quint32 addr(const QJsonObject& o, const char* key, const QString& path, std::optional<QString> def = std::nullopt)
    {
        const QString s = text(o, key, path, def);
        quint32 a = 0;
        if (error.isEmpty() && !WifiProfile::addrValue(s, &a))
            fail(path + QLatin1String(key), QStringLiteral("a.b.c.d is needed"));
        return a;
    }
    bool flag(const QJsonObject& o, const char* key, bool def)
    {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined() || v.isNull())
            return def;
        return v.isBool() ? v.toBool() : v.toDouble() != 0.0;
    }
};

void putU16(QByteArray& b, uint16_t v)
{
    b.append(char(v & 0xFF));
    b.append(char(v >> 8));
}

void putU32(QByteArray& b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.append(char((v >> (8 * i)) & 0xFF));
}

void putAddr(QByteArray& b, quint32 a)
{
    b.append(char(a >> 24));
    b.append(char((a >> 16) & 0xFF));
    b.append(char((a >> 8) & 0xFF));
    b.append(char(a & 0xFF));
}

bool putStr(QByteArray& b, const QString& s, int lo, int hi, const char* what, QString* error)
{
    const QByteArray u = s.toUtf8();
    if (u.size() < lo || u.size() > hi) {
        if (error && error->isEmpty())
            *error = QStringLiteral("%1: %2 bytes, %3..%4 allowed").arg(QLatin1String(what)).arg(u.size()).arg(lo).arg(hi);
        return false;
    }
    b.append(char(u.size()));
    b.append(u);
    return true;
}

void putLine(QByteArray& b, const WifiProfile::Line& l)
{
    b.append(char(l.mode));
    b.append(char(l.dest));
    putAddr(b, l.ip);
    putU16(b, l.rport);
    putU16(b, l.lport);
}

// The module's reader (the contract's layout): a read past the end marks the block bad and yields zeros.
class BlobReader
{
public:
    explicit BlobReader(const QByteArray& b) : b_(b) {}
    bool bad = false;
    int at = 0;

    uint8_t u8()
    {
        if (at + 1 > b_.size()) {
            bad = true;
            return 0;
        }
        return uint8_t(b_[at++]);
    }
    uint16_t u16()
    {
        const uint16_t v = u8();
        return uint16_t(v | (u8() << 8));
    }
    uint32_t u32()
    {
        const uint32_t v = u16();
        return v | (uint32_t(u16()) << 16);
    }
    quint32 addr()
    {
        if (at + 4 > b_.size()) {
            bad = true;
            return 0;
        }
        const quint32 a = (quint32(uint8_t(b_[at])) << 24) | (quint32(uint8_t(b_[at + 1])) << 16)
                          | (quint32(uint8_t(b_[at + 2])) << 8) | quint32(uint8_t(b_[at + 3]));
        at += 4;
        return a;
    }
    std::optional<QString> str(int lo, int hi)
    {
        const int k = u8();
        if (bad || k < lo || k > hi || at + k > b_.size() || b_.mid(at, k).contains('\0')) {
            bad = true;
            return std::nullopt;
        }
        const QString s = QString::fromUtf8(b_.mid(at, k));
        at += k;
        return s;
    }
    int size() const { return int(b_.size()); }

private:
    const QByteArray& b_;
};

WifiProfile::Line readLine(BlobReader& r)
{
    WifiProfile::Line l;
    l.mode = r.u8();
    l.dest = r.u8();
    l.ip = r.addr();
    l.rport = r.u16();
    l.lport = r.u16();
    return l;
}

QJsonObject lineJson(const WifiProfile::Line& l)
{
    QJsonObject o;
    o.insert(QStringLiteral("mode"), nameOr(kModes, l.mode));
    o.insert(QStringLiteral("dest"), nameOr(kDests, l.dest));
    o.insert(QStringLiteral("ip"), WifiProfile::addrText(l.ip));
    o.insert(QStringLiteral("rport"), int(l.rport));
    o.insert(QStringLiteral("lport"), int(l.lport));
    return o;
}

WifiProfile::Line lineFrom(JsonReader& r, const QJsonObject& o, const QString& path)
{
    WifiProfile::Line l;
    l.mode = uint8_t(r.named(o, "mode", path, kModes));
    l.dest = uint8_t(r.named(o, "dest", path, kDests));
    l.ip = r.addr(o, "ip", path, QStringLiteral("0.0.0.0"));
    l.rport = uint16_t(r.integer(o, "rport", path, 0, 65535));
    l.lport = uint16_t(r.integer(o, "lport", path, 0, 65535, 0));
    return l;
}

} // namespace

QString WifiProfile::addrText(quint32 a)
{
    return QStringLiteral("%1.%2.%3.%4").arg(a >> 24).arg((a >> 16) & 0xFF).arg((a >> 8) & 0xFF).arg(a & 0xFF);
}

bool WifiProfile::addrValue(const QString& s, quint32* out)
{
    const QStringList parts = s.trimmed().split(QLatin1Char('.'));
    if (parts.size() != 4)
        return false;
    quint32 a = 0;
    for (const QString& p : parts) {
        bool ok = false;
        const int v = p.toInt(&ok);
        if (!ok || v < 0 || v > 255)
            return false;
        a = (a << 8) | quint32(v);
    }
    if (out)
        *out = a;
    return true;
}

QString WifiProfile::fromJson(const QJsonObject& o, WifiProfile* out)
{
    JsonReader r;
    WifiProfile p;
    const QString top;
    p.name = r.text(o, "name", top);
    p.role = uint8_t(r.named(o, "role", top, kRoles));
    p.address = uint8_t(r.integer(o, "address", top, 0, 255));
    p.x1Baud = uint32_t(r.integer(o, "x1_baud", top, 0, 0xFFFFFFFFLL));

    const QJsonObject radio = r.object(o, "radio", top);
    const QString rp = QStringLiteral("radio.");
    const QJsonValue power = radio.value(QStringLiteral("power_dbm"));
    if (!power.isDouble()) {
        r.fail(rp + QStringLiteral("power_dbm"), QStringLiteral("a number is needed"));
    } else {
        // The module project's tools write round(power * 4): the nearest quarter, a half to the even one.
        const double q = std::nearbyint(power.toDouble() * 4.0);
        if (q < 0 || q > 255)
            r.fail(rp + QStringLiteral("power_dbm"), QStringLiteral("0..63.75 dBm"));
        else
            p.powerQ = uint8_t(q);
    }
    const QJsonValue mode = radio.value(QStringLiteral("mode"));
    if (mode.isString()) {
        bool found = false;
        for (const RadioMode& m : kRadioModes)
            if (mode.toString() == QLatin1String(m.name)) {
                p.protocols = m.code;
                found = true;
            }
        if (!found)
            r.fail(rp + QStringLiteral("mode"), QStringLiteral("unknown \"%1\"").arg(mode.toString()));
    } else {
        p.protocols = uint8_t(r.integer(radio, "mode", rp, 0, 255));
    }
    const long long bw = r.integer(radio, "bw", rp, 0, 255);
    if (r.error.isEmpty() && bw != 20 && bw != 40)
        r.fail(rp + QStringLiteral("bw"), QStringLiteral("20 or 40"));
    p.bandwidth = bw == 40 ? 2 : 1;
    p.powerSave = uint8_t(r.named(radio, "ps", rp, kPowerSave));

    const QJsonObject ap = r.object(o, "ap", top);
    const QString ap_ = QStringLiteral("ap.");
    p.apSsid = r.text(ap, "ssid", ap_);
    p.apPassword = r.text(ap, "password", ap_, QString());
    p.apChannel = uint8_t(r.integer(ap, "channel", ap_, 0, 255));
    p.apAuth = uint8_t(r.named(ap, "auth", ap_, kAuths));
    p.apHidden = uint8_t(r.integer(ap, "hidden", ap_, 0, 255, 0, true));
    p.apMaxClients = uint8_t(r.integer(ap, "max_clients", ap_, 0, 255, 4));

    const QJsonObject ip = r.object(o, "ip", top);
    const QString ipp = QStringLiteral("ip.");
    p.ip = r.addr(ip, "ip", ipp);
    p.mask = r.addr(ip, "mask", ipp);
    p.dhcp = uint8_t(r.integer(ip, "dhcp", ipp, 0, 255, 1, true));
    p.poolStart = r.addr(ip, "pool_start", ipp);
    p.poolEnd = r.addr(ip, "pool_end", ipp);
    p.leaseMin = uint16_t(r.integer(ip, "lease_min", ipp, 0, 65535, 120));
    p.offer = uint8_t((r.flag(ip, "offer_gw", false) ? 1 : 0) | (r.flag(ip, "offer_dns", false) ? 2 : 0));

    p.line0 = lineFrom(r, r.object(o, "line0", top), QStringLiteral("line0."));
    const QJsonObject l1 = r.object(o, "line1", top);
    const QString l1p = QStringLiteral("line1.");
    p.line1 = lineFrom(r, l1, l1p);
    p.x2Baud = uint32_t(r.integer(l1, "baud", l1p, 0, 0xFFFFFFFFLL));
    p.txPin = int8_t(r.integer(l1, "tx_pin", l1p, -128, 127));
    p.rxPin = int8_t(r.integer(l1, "rx_pin", l1p, -128, 127));
    p.xlat = uint8_t(r.named(l1, "xlat", l1p, kXlats, 0));

    const QJsonValue nets = o.value(QStringLiteral("networks"));
    if (!nets.isUndefined() && !nets.isNull() && !nets.isArray())
        r.fail(QStringLiteral("networks"), QStringLiteral("a list is needed"));
    const QJsonArray list = nets.toArray();
    for (int i = 0; i < list.size(); ++i) {
        const QString np = QStringLiteral("networks[%1].").arg(i);
        if (!list[i].isObject()) {
            r.fail(np, QStringLiteral("an object is needed"));
            break;
        }
        const QJsonObject n = list[i].toObject();
        Net net;
        net.ssid = r.text(n, "ssid", np);
        net.password = r.text(n, "password", np, QString());
        net.secure = r.flag(n, "secure", false);
        p.networks.append(net);
    }
    if (!r.error.isEmpty())
        return r.error;
    if (out)
        *out = p;
    return QString();
}

QJsonObject WifiProfile::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("role"), nameOr(kRoles, role));
    o.insert(QStringLiteral("address"), int(address));
    o.insert(QStringLiteral("x1_baud"), qint64(x1Baud));

    QJsonObject radio;
    radio.insert(QStringLiteral("power_dbm"), powerQ / 4.0);
    QJsonValue mode = int(protocols);
    for (const RadioMode& m : kRadioModes)
        if (m.code == protocols)
            mode = QString::fromLatin1(m.name);
    radio.insert(QStringLiteral("mode"), mode);
    radio.insert(QStringLiteral("bw"), bandwidth == 2 ? 40 : 20);
    radio.insert(QStringLiteral("ps"), nameOr(kPowerSave, powerSave));
    o.insert(QStringLiteral("radio"), radio);

    QJsonObject ap;
    ap.insert(QStringLiteral("ssid"), apSsid);
    ap.insert(QStringLiteral("password"), apPassword);
    ap.insert(QStringLiteral("channel"), int(apChannel));
    ap.insert(QStringLiteral("auth"), nameOr(kAuths, apAuth));
    ap.insert(QStringLiteral("hidden"), apHidden < 2 ? QJsonValue(apHidden != 0) : QJsonValue(int(apHidden)));
    ap.insert(QStringLiteral("max_clients"), int(apMaxClients));
    o.insert(QStringLiteral("ap"), ap);

    QJsonObject ipo;
    ipo.insert(QStringLiteral("ip"), addrText(ip));
    ipo.insert(QStringLiteral("mask"), addrText(mask));
    ipo.insert(QStringLiteral("dhcp"), dhcp < 2 ? QJsonValue(dhcp != 0) : QJsonValue(int(dhcp)));
    ipo.insert(QStringLiteral("pool_start"), addrText(poolStart));
    ipo.insert(QStringLiteral("pool_end"), addrText(poolEnd));
    ipo.insert(QStringLiteral("lease_min"), int(leaseMin));
    ipo.insert(QStringLiteral("offer_gw"), (offer & 1) != 0);
    ipo.insert(QStringLiteral("offer_dns"), (offer & 2) != 0);
    o.insert(QStringLiteral("ip"), ipo);

    o.insert(QStringLiteral("line0"), lineJson(line0));
    QJsonObject l1 = lineJson(line1);
    l1.insert(QStringLiteral("baud"), qint64(x2Baud));
    l1.insert(QStringLiteral("tx_pin"), int(txPin));
    l1.insert(QStringLiteral("rx_pin"), int(rxPin));
    l1.insert(QStringLiteral("xlat"), nameOr(kXlats, xlat));
    o.insert(QStringLiteral("line1"), l1);

    QJsonArray nets;
    for (const Net& n : networks) {
        QJsonObject j;
        j.insert(QStringLiteral("ssid"), n.ssid);
        j.insert(QStringLiteral("password"), n.password);
        if (n.secure)
            j.insert(QStringLiteral("secure"), true);
        nets.append(j);
    }
    o.insert(QStringLiteral("networks"), nets);
    return o;
}

QByteArray WifiProfile::encode(QString* error) const
{
    QString err;
    QByteArray b;
    b.append(char(kFormat));
    putStr(b, name, 1, 32, "name", &err);
    b.append(char(role));
    b.append(char(address));
    putU32(b, x1Baud);
    b.append(char(powerQ));
    b.append(char(protocols));
    b.append(char(bandwidth));
    b.append(char(powerSave));
    b.append(char(apChannel));
    b.append(char(apHidden));
    b.append(char(apMaxClients));
    b.append(char(apAuth));
    putStr(b, apSsid, 1, 32, "ap ssid", &err);
    putStr(b, apPassword, 0, 63, "ap password", &err);
    putAddr(b, ip);
    putAddr(b, mask);
    b.append(char(dhcp));
    putAddr(b, poolStart);
    putAddr(b, poolEnd);
    putU16(b, leaseMin);
    b.append(char(offer));
    putLine(b, line0);
    putLine(b, line1);
    putU32(b, x2Baud);
    b.append(char(txPin));
    b.append(char(rxPin));
    b.append(char(xlat));
    if (networks.size() > kMaxNets && err.isEmpty())
        err = QStringLiteral("networks: %1, at most %2").arg(networks.size()).arg(kMaxNets);
    b.append(char(networks.size()));
    for (const Net& n : networks) {
        putStr(b, n.ssid, 1, 32, "network ssid", &err);
        putStr(b, n.password, 0, 64, "network password", &err);
    }
    if (b.size() > kMaxBytes && err.isEmpty())
        err = QStringLiteral("profile of %1 bytes, at most %2").arg(b.size()).arg(kMaxBytes);
    if (error)
        *error = err;
    return err.isEmpty() ? b : QByteArray();
}

int WifiProfile::decode(const QByteArray& blob, WifiProfile* out)
{
    BlobReader r(blob);
    WifiProfile p;
    if (r.u8() != kFormat)
        return ReasonFormat;
    const std::optional<QString> name = r.str(1, 32);
    if (!name)
        return ReasonName;
    p.name = *name;
    p.role = r.u8();
    p.address = r.u8();
    p.x1Baud = r.u32();
    p.powerQ = r.u8();
    p.protocols = r.u8();
    p.bandwidth = r.u8();
    p.powerSave = r.u8();
    p.apChannel = r.u8();
    p.apHidden = r.u8();
    p.apMaxClients = r.u8();
    p.apAuth = r.u8();
    const std::optional<QString> ssid = r.str(1, 32);
    const std::optional<QString> pw = ssid ? r.str(0, 63) : std::nullopt;
    if (!pw)
        return ReasonFormat;
    p.apSsid = *ssid;
    p.apPassword = *pw;
    p.ip = r.addr();
    p.mask = r.addr();
    p.dhcp = r.u8();
    p.poolStart = r.addr();
    p.poolEnd = r.addr();
    p.leaseMin = r.u16();
    p.offer = r.u8();
    p.line0 = readLine(r);
    p.line1 = readLine(r);
    p.x2Baud = r.u32();
    p.txPin = int8_t(r.u8());
    p.rxPin = int8_t(r.u8());
    p.xlat = r.u8();
    const int nn = r.u8();
    if (r.bad)
        return ReasonFormat;
    if (nn > kMaxNets)
        return ReasonNets;
    for (int i = 0; i < nn; ++i) {
        const std::optional<QString> s = r.str(1, 32);
        const std::optional<QString> k = s ? r.str(0, 64) : std::nullopt;
        if (!k)
            return ReasonNets;
        p.networks.append({ *s, *k, false });
    }
    if (r.bad || r.at != r.size())
        return ReasonFormat;
    if (p.role > 1)
        return ReasonRole;
    if (p.address == 0 || p.address == 255)
        return ReasonAddress;
    if (out)
        *out = p;
    return ReasonOk;
}

QStringList WifiProfile::problems() const
{
    QStringList out;
    const int apLen = int(apPassword.toUtf8().size());
    if (apAuth != 0 && (apLen < 8 || apLen > 63))
        out << QCoreApplication::translate("WifiProfile", "the access point's password: 8 to 63 characters");
    if (networks.size() > kMaxNets)
        out << QCoreApplication::translate("WifiProfile", "more than %1 networks").arg(kMaxNets);
    for (int i = 0; i < networks.size(); ++i) {
        const Net& n = networks[i];
        const QByteArray pw = n.password.toUtf8();
        bool hex = pw.size() == 64;
        for (const char c : pw)
            hex = hex && std::isxdigit(uint8_t(c));
        if (pw.isEmpty() && n.secure)
            out << QCoreApplication::translate("WifiProfile", "network \"%1\": a password is needed").arg(n.ssid);
        else if (!pw.isEmpty() && !hex && (pw.size() < 8 || pw.size() > 63))
            out << QCoreApplication::translate("WifiProfile", "network \"%1\": a password is 8 to 63 characters or 64 hex digits").arg(n.ssid);
        for (int k = 0; k < i; ++k)
            if (networks[k].ssid == n.ssid) {
                out << QCoreApplication::translate("WifiProfile", "network \"%1\" is listed twice").arg(n.ssid);
                break;
            }
    }
    return out;
}

quint32 WifiProfile::crc32(const QByteArray& data)
{
    static quint32 table[256];
    static bool ready = false;
    if (!ready) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    quint32 crc = 0xFFFFFFFFu;
    for (const char b : data)
        crc = table[(crc ^ uint8_t(b)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

QByteArray WifiProfile::beginPayload(const QByteArray& blob)
{
    QByteArray p;
    putU32(p, kKey);
    putU16(p, uint16_t(blob.size()));
    putU32(p, crc32(blob));
    return p;
}

QVector<QByteArray> WifiProfile::chunkPayloads(const QByteArray& blob, int size)
{
    QVector<QByteArray> out;
    size = qBound(1, size, kMaxChunk);
    for (int o = 0; o < blob.size(); o += size) {
        QByteArray p;
        putU16(p, uint16_t(o));
        p.append(blob.mid(o, size));
        out.append(p);
    }
    return out;
}

QByteArray WifiProfile::applyPayload()
{
    QByteArray p;
    putU32(p, kKey);
    return p;
}

QString WifiProfile::reasonName(int reason)
{
    if (reason >= 0 && reason < kReasons)
        return QString::fromLatin1(kReasonNames[reason]);
    return QStringLiteral("reason %1").arg(reason);
}

bool WifiProfileStatus::parse(const uint8_t* data, int size, WifiProfileStatus* out)
{
    if (!data || size < 10 || size < 10 + data[9])
        return false;
    WifiProfileStatus s;
    s.valid = true;
    s.loaded = (data[0] & 1) != 0;
    s.modified = (data[0] & 2) != 0;
    s.fingerprint = quint32(data[1]) | (quint32(data[2]) << 8) | (quint32(data[3]) << 16) | (quint32(data[4]) << 24);
    s.received = uint16_t(data[5] | (data[6] << 8));
    s.announced = uint16_t(data[7] | (data[8] << 8));
    s.name = QString::fromUtf8(reinterpret_cast<const char*>(data + 10), data[9]);
    if (out)
        *out = s;
    return true;
}

// ---- the upload ------------------------------------------------------------------------------------------------

WifiProfileUpload::WifiProfileUpload(Hooks hooks, Timing timing, QObject* parent)
    : QObject(parent), hooks_(std::move(hooks)), timing_(timing)
{
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &WifiProfileUpload::onTimeout);
}

bool WifiProfileUpload::start(const QByteArray& blob)
{
    if (active() || blob.isEmpty() || blob.size() > WifiProfile::kMaxBytes)
        return false;
    blob_ = blob;
    WifiProfile p;
    name_ = WifiProfile::decode(blob, &p) == WifiProfile::ReasonOk ? p.name : QString();
    pieces_ = WifiProfile::chunkPayloads(blob);
    index_ = 0;
    tries_ = 1;
    extraOks_ = 0;
    sent_ = 0;
    resp_ = 0;
    reason_ = -1;
    runtime_ = false;
    askedReason_ = false;
    appliedAgain_ = false;
    statusAsks_ = 0;
    phase_ = Begin;
    hooks_.send(1, WifiProfile::beginPayload(blob_));
    timer_.start(timing_.answerMs);
    emit progressChanged();
    return true;
}

void WifiProfileUpload::sendPiece()
{
    hooks_.send(2, pieces_[index_]);
    timer_.start(timing_.answerMs);
}

void WifiProfileUpload::onAnswer(int ver, int resp)
{
    switch (phase_) {
    case Begin:
        if (ver != 1)
            return;
        timer_.stop();
        if (resp != 1) {
            resp_ = resp;
            finish(Rejected);
            return;
        }
        phase_ = Pieces;
        index_ = 0;
        tries_ = 1;
        extraOks_ = 0;
        sendPiece();
        emit progressChanged();
        return;
    case Pieces:
        if (ver != 2)
            return;
        // An OK for a copy of the piece before (sent again after a lost answer) is not this piece's: skipped. If it
        // was this piece's after all, its answer times out and the piece goes again (the module takes the repeat).
        if (resp == 1 && extraOks_ > 0) {
            --extraOks_;
            return;
        }
        timer_.stop();
        if (resp != 1) {
            resp_ = resp;
            finish(Rejected);
            return;
        }
        extraOks_ = tries_ - 1;
        sent_ += int(pieces_[index_].size()) - 2;
        ++index_;
        tries_ = 1;
        if (index_ < pieces_.size()) {
            sendPiece();
        } else {
            phase_ = Apply;
            hooks_.send(3, WifiProfile::applyPayload());
            timer_.start(timing_.answerMs);
        }
        emit progressChanged();
        return;
    case Apply:
    case Status:
        if (ver != 3)
            return;
        timer_.stop();
        if (resp == 1) {
            finish(Saved);
            return;
        }
        resp_ = resp;
        if (resp == 3 || resp == 8) {             // ERR_PAYLOAD, ERR_RUNTIME: CONTENT v3 follows (or not)
            runtime_ = resp == 8;
            phase_ = Reason;
            timer_.start(timing_.reasonMs);
            emit progressChanged();
            return;
        }
        finish(Rejected);
        return;
    default:
        return;
    }
}

void WifiProfileUpload::onReason(int reason)
{
    if (phase_ == Status) {
        // The apply's answer was lost: CONTENT v3 still tells. 0 pushed after an apply follows an OK; 0 as the answer
        // to this app's question (no upload held, not loaded) means no refusal is known: not an answer.
        timer_.stop();
        if (reason == WifiProfile::ReasonOk) {
            finish(askedReason_ ? NoAnswer : Saved);
        } else {
            resp_ = reason == WifiProfile::ReasonBusy ? 8 : 3;
            reason_ = reason;
            finish(reason == WifiProfile::ReasonBusy ? Busy : Refused);
        }
        return;
    }
    if (phase_ != Reason)
        return;
    timer_.stop();
    reason_ = reason;
    if (runtime_)
        finish(reason == WifiProfile::ReasonBusy ? Busy : WriteFailed);
    else
        finish(Refused);
}

void WifiProfileUpload::onStatus(const WifiProfileStatus& st)
{
    if (phase_ != Status || !st.valid)
        return;
    // The upload still held whole: the apply never arrived; sent once more.
    if (!appliedAgain_ && st.announced == total() && st.received == total()) {
        appliedAgain_ = true;
        timer_.stop();
        phase_ = Apply;
        hooks_.send(3, WifiProfile::applyPayload());
        timer_.start(timing_.answerMs);
        return;
    }
    if (st.loaded && !st.modified && st.announced == 0 && !name_.isEmpty() && st.name == name_) {
        timer_.stop();
        finish(Saved);
        return;
    }
    if (st.announced == 0 && !askedReason_) {   // no upload held, not loaded: ask why the last apply was refused
        askedReason_ = true;
        hooks_.ask(3);
    }
}

void WifiProfileUpload::onTimeout()
{
    switch (phase_) {
    case Begin:
        if (tries_ < timing_.tries) {
            ++tries_;
            hooks_.send(1, WifiProfile::beginPayload(blob_));   // a new begin drops what the module held
            timer_.start(timing_.answerMs);
        } else {
            finish(NoAnswer);
        }
        return;
    case Pieces:
        if (tries_ < timing_.tries) {
            ++tries_;
            sendPiece();
        } else {
            finish(NoAnswer);
        }
        return;
    case Apply:
        // Not sent again blindly: the module may have saved and be rebooting. Its status tells.
        phase_ = Status;
        statusAsks_ = 1;
        hooks_.ask(0);
        timer_.start(timing_.reasonMs);
        emit progressChanged();
        return;
    case Status:
        if (statusAsks_ < timing_.tries) {
            ++statusAsks_;
            hooks_.ask(0);
            timer_.start(timing_.reasonMs);
        } else {
            finish(NoAnswer);
        }
        return;
    case Reason:
        if (!runtime_ && !askedReason_) {
            askedReason_ = true;
            hooks_.ask(3);
            timer_.start(timing_.reasonMs);
            return;
        }
        finish(runtime_ ? WriteFailed : Refused);
        return;
    default:
        return;
    }
}

void WifiProfileUpload::cancel()
{
    if (!active())
        return;
    timer_.stop();
    phase_ = Idle;
    emit progressChanged();
}

void WifiProfileUpload::finish(int outcome)
{
    timer_.stop();
    phase_ = Idle;
    emit progressChanged();
    emit finished(outcome);
}

// ---- what a load changes ---------------------------------------------------------------------------------------

QString WifiProfileDiff::baudText(uint32_t baud)
{
    QString s = QString::number(baud);
    for (int i = int(s.size()) - 3; i > 0; i -= 3)
        s.insert(i, QChar(0x202F));               // a narrow no-break space between the thousands
    return s;
}

QString WifiProfileDiff::roleText(int role)
{
    return role == 1 ? QCoreApplication::translate("WifiProfile", "access point") : role == 0 ? QCoreApplication::translate("WifiProfile", "station") : QString::number(role);
}

QString WifiProfileDiff::apText(const WifiProfile& p)
{
    const QString auth = p.apAuth == 0 ? QCoreApplication::translate("WifiProfile", "open") : p.apAuth == 1 ? QStringLiteral("WPA2")
                         : p.apAuth == 2 ? QStringLiteral("WPA2+WPA3") : QString::number(p.apAuth);
    QString s = QCoreApplication::translate("WifiProfile", "%1 · channel %2 · %3 · up to %4 clients").arg(p.apSsid).arg(p.apChannel).arg(auth).arg(p.apMaxClients);
    if (p.apHidden)
        s += QCoreApplication::translate("WifiProfile", " · hidden");
    return s;
}

QString WifiProfileDiff::netsText(const WifiProfile& p)
{
    QStringList names;
    for (const WifiProfile::Net& n : p.networks)
        names << n.ssid;
    return names.isEmpty() ? QCoreApplication::translate("WifiProfile", "none") : names.join(QStringLiteral(", "));
}

QString WifiProfileDiff::lineText(const WifiProfile::Line& l)
{
    switch (l.mode) {
    case 0:
        return QCoreApplication::translate("WifiProfile", "off");
    case 1: {
        const QString to = l.dest == 0 ? WifiProfile::addrText(l.ip) : l.dest == 1 ? QCoreApplication::translate("WifiProfile", "senders")
                           : l.dest == 2 ? QCoreApplication::translate("WifiProfile", "broadcast") : QString::number(l.dest);
        return QCoreApplication::translate("WifiProfile", "UDP to %1:%2, own port %3").arg(to).arg(l.rport).arg(l.lport ? l.lport : l.rport);
    }
    case 2:
        return QCoreApplication::translate("WifiProfile", "TCP to %1:%2").arg(WifiProfile::addrText(l.ip)).arg(l.rport);
    default:
        return QCoreApplication::translate("WifiProfile", "mode %1").arg(l.mode);
    }
}

QString WifiProfileDiff::line1Text(const WifiProfile& p)
{
    return QCoreApplication::translate("WifiProfile", "%1 · TX %2, RX %3").arg(lineText(p.line1)).arg(p.txPin).arg(p.rxPin);
}

QString WifiProfileDiff::radioText(const WifiProfile& p)
{
    QString proto;
    switch (p.protocols) {
    case 0x01: proto = QStringLiteral("b"); break;
    case 0x03: proto = QStringLiteral("b/g"); break;
    case 0x07: proto = QStringLiteral("b/g/n"); break;
    case 0x08: proto = QStringLiteral("LR"); break;
    case 0x0F: proto = QStringLiteral("b/g/n+LR"); break;
    default: proto = QStringLiteral("0x%1").arg(p.protocols, 2, 16, QLatin1Char('0')); break;
    }
    const QString ps = p.powerSave == 0 ? QCoreApplication::translate("WifiProfile", "power save off") : p.powerSave == 1 ? QCoreApplication::translate("WifiProfile", "power save min")
                       : p.powerSave == 2 ? QCoreApplication::translate("WifiProfile", "power save max") : QString::number(p.powerSave);
    // The module saves more than 20 dBm (81..84) as 20 (the contract).
    const double dbm = qMin<int>(p.powerQ, 80) / 4.0;
    return QCoreApplication::translate("WifiProfile", "%1 dBm · %2 · %3 MHz · %4").arg(dbm).arg(proto).arg(p.bandwidth == 2 ? 40 : 20).arg(ps);
}

QString WifiProfileDiff::ipText(const WifiProfile& p)
{
    int bits = 0;
    for (quint32 m = p.mask; m & 0x80000000u; m <<= 1)
        ++bits;
    QString s = QStringLiteral("%1/%2").arg(WifiProfile::addrText(p.ip)).arg(bits);
    if (p.dhcp)
        s += QCoreApplication::translate("WifiProfile", " · DHCP %1–%2, %3 min").arg(WifiProfile::addrText(p.poolStart), WifiProfile::addrText(p.poolEnd)).arg(p.leaseMin);
    else
        s += QCoreApplication::translate("WifiProfile", " · no DHCP");
    if (p.offer & 1)
        s += QCoreApplication::translate("WifiProfile", " · offers a gateway");
    if (p.offer & 2)
        s += QCoreApplication::translate("WifiProfile", " · offers DNS");
    return s;
}

QVector<WifiProfileChange> WifiProfileDiff::changes(const WifiProfile& now, uint32_t known, const WifiProfile& next)
{
    QVector<WifiProfileChange> out;
    const auto add = [&out](const char* key, const QString& what, const QString& from, const QString& to, bool primary) {
        if (from != to)
            out.append({ QString::fromLatin1(key), what, from, to, primary });
    };
    if (known & KnownRole)
        add("role", QCoreApplication::translate("WifiProfile", "Role"), roleText(now.role), roleText(next.role), true);
    if (known & KnownAp)
        add("ap", QCoreApplication::translate("WifiProfile", "Access point network"), apText(now), apText(next), now.role == 1 || next.role == 1);
    if (known & KnownNets)
        add("networks", QCoreApplication::translate("WifiProfile", "Station networks"), netsText(now), netsText(next), now.role == 0 || next.role == 0);
    if (known & KnownAddress)
        add("address", QCoreApplication::translate("WifiProfile", "Own address"), QString::number(now.address), QString::number(next.address), true);
    if (known & KnownX1)
        add("x1", QCoreApplication::translate("WifiProfile", "X1 rate"), baudText(now.x1Baud), baudText(next.x1Baud), true);
    if (known & KnownX2)
        add("x2", QCoreApplication::translate("WifiProfile", "X2 rate"), baudText(now.x2Baud), baudText(next.x2Baud), true);
    if (known & KnownLine0)
        add("line0", QCoreApplication::translate("WifiProfile", "X1 line"), lineText(now.line0), lineText(next.line0), false);
    if (known & KnownLine1)
        add("line1", QCoreApplication::translate("WifiProfile", "X2 line"), line1Text(now), line1Text(next), false);
    if (known & KnownXlat)
        add("xlat", QCoreApplication::translate("WifiProfile", "X2 input"), now.xlat ? QCoreApplication::translate("WifiProfile", "GODIO telemetry translator") : QCoreApplication::translate("WifiProfile", "as received"),
            next.xlat ? QCoreApplication::translate("WifiProfile", "GODIO telemetry translator") : QCoreApplication::translate("WifiProfile", "as received"), false);
    if (known & KnownRadio)
        add("radio", QCoreApplication::translate("WifiProfile", "Radio"), radioText(now), radioText(next), false);
    if (known & KnownIp)
        add("ip", QCoreApplication::translate("WifiProfile", "Address and DHCP"), ipText(now), ipText(next), false);
    std::stable_sort(out.begin(), out.end(), [](const WifiProfileChange& a, const WifiProfileChange& b) {
        return a.primary && !b.primary;
    });
    return out;
}

// ---- where the profiles come from ------------------------------------------------------------------------------

namespace {

QString& folderOverride()
{
    static QString path;
    return path;
}

// The module project's presets (profiles/*.json, its commit 0a52275) without their passwords; "secure" marks the
// networks whose password has to be typed. The names are Cyrillic: escaped for the JSON reader.
const char kHeadUnit[] = R"({"name": "Бошка", "role": "sta", "address": 87, "x1_baud": 2000000,
 "radio": {"power_dbm": 20, "mode": "bgn", "bw": 20, "ps": "none"},
 "ap": {"ssid": "KoggerBridge", "password": "", "channel": 6, "auth": "wpa2", "hidden": false, "max_clients": 4},
 "ip": {"ip": "10.0.0.10", "mask": "255.255.255.0", "dhcp": true, "pool_start": "10.0.0.11", "pool_end": "10.0.0.30",
        "lease_min": 120, "offer_gw": false, "offer_dns": false},
 "line0": {"mode": "udp", "dest": "fixed", "ip": "10.0.0.10", "rport": 14444, "lport": 14444},
 "line1": {"mode": "off", "dest": "senders", "ip": "0.0.0.0", "rport": 14445, "lport": 14445, "baud": 921600,
           "tx_pin": 5, "rx_pin": 4, "xlat": "none"},
 "networks": [{"ssid": "KoggerBoat", "password": "", "secure": true}, {"ssid": "KoggerBridge", "password": "", "secure": true}]})";

const char kBoatKogger[] = R"({"name": "Лодка Kogger", "role": "ap", "address": 88, "x1_baud": 5000000,
 "radio": {"power_dbm": 20, "mode": "bgn", "bw": 20, "ps": "none"},
 "ap": {"ssid": "KoggerBoat", "password": "", "channel": 10, "auth": "wpa2", "hidden": false, "max_clients": 4},
 "ip": {"ip": "10.0.0.10", "mask": "255.255.255.0", "dhcp": true, "pool_start": "10.0.0.11", "pool_end": "10.0.0.30",
        "lease_min": 120, "offer_gw": false, "offer_dns": false},
 "line0": {"mode": "udp", "dest": "senders", "ip": "0.0.0.0", "rport": 14444, "lport": 14444},
 "line1": {"mode": "udp", "dest": "senders", "ip": "0.0.0.0", "rport": 14445, "lport": 14445, "baud": 921600,
           "tx_pin": 5, "rx_pin": 4, "xlat": "none"},
 "networks": []})";

const char kBoatGodio[] = R"({"name": "Лодка GODIO", "role": "ap", "address": 88, "x1_baud": 921600,
 "radio": {"power_dbm": 20, "mode": "bgn", "bw": 20, "ps": "none"},
 "ap": {"ssid": "KoggerBridge", "password": "", "channel": 6, "auth": "wpa2", "hidden": false, "max_clients": 4},
 "ip": {"ip": "10.0.0.10", "mask": "255.255.255.0", "dhcp": true, "pool_start": "10.0.0.11", "pool_end": "10.0.0.30",
        "lease_min": 120, "offer_gw": false, "offer_dns": false},
 "line0": {"mode": "udp", "dest": "senders", "ip": "0.0.0.0", "rport": 14444, "lport": 14444},
 "line1": {"mode": "udp", "dest": "senders", "ip": "0.0.0.0", "rport": 14445, "lport": 14445, "baud": 9600,
           "tx_pin": 10, "rx_pin": 5, "xlat": "telemetry"},
 "networks": []})";

QString fileNameFor(const QString& name)
{
    QString s;
    for (const QChar c : name)
        s += (c.isLetterOrNumber() || c == QLatin1Char(' ') || c == QLatin1Char('-') || c == QLatin1Char('_')) ? c : QLatin1Char('_');
    s = s.trimmed();
    return s.isEmpty() ? QStringLiteral("profile") : s;
}

} // namespace

QString WifiProfileStore::folder()
{
    if (!folderOverride().isEmpty())
        return folderOverride();
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/wifi_profiles");
}

void WifiProfileStore::setFolderForCheck(const QString& path)
{
    folderOverride() = path;
}

QVector<QPair<QString, QByteArray>> WifiProfileStore::builtinJson()
{
    return { { QStringLiteral("headunit"), QByteArray(kHeadUnit) },
             { QStringLiteral("boat_kogger"), QByteArray(kBoatKogger) },
             { QStringLiteral("boat_godio"), QByteArray(kBoatGodio) } };
}

QVector<WifiProfileStore::Entry> WifiProfileStore::list()
{
    QVector<Entry> out;
    for (const auto& b : builtinJson()) {
        Entry e;
        e.id = QStringLiteral("builtin:") + b.first;
        e.builtin = true;
        const QJsonDocument doc = QJsonDocument::fromJson(b.second);
        e.error = WifiProfile::fromJson(doc.object(), &e.profile);
        out.append(e);
    }
    const QDir dir(folder());
    const QFileInfoList files = dir.entryInfoList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& fi : files) {
        Entry e;
        e.id = QStringLiteral("file:") + fi.fileName();
        e.file = fi.absoluteFilePath();
        QFile f(e.file);
        if (!f.open(QIODevice::ReadOnly)) {
            e.error = QStringLiteral("cannot be read");
        } else {
            QJsonParseError pe;
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
            e.error = doc.isObject() ? WifiProfile::fromJson(doc.object(), &e.profile)
                                     : QStringLiteral("not JSON: %1").arg(pe.errorString());
        }
        out.append(e);
    }
    return out;
}

bool WifiProfileStore::find(const QString& id, Entry* out)
{
    for (const Entry& e : list())
        if (e.id == id) {
            if (out)
                *out = e;
            return true;
        }
    return false;
}

QString WifiProfileStore::save(const WifiProfile& p, QString* idOut)
{
    QDir dir(folder());
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
        return QStringLiteral("the folder %1 cannot be made").arg(folder());
    const QString base = fileNameFor(p.name);
    QString name = base + QStringLiteral(".json");
    for (int n = 2; dir.exists(name); ++n)
        name = QStringLiteral("%1 (%2).json").arg(base).arg(n);
    QFile f(dir.filePath(name));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QStringLiteral("%1 cannot be written").arg(f.fileName());
    f.write(QJsonDocument(p.toJson()).toJson(QJsonDocument::Indented));
    if (!f.flush())
        return QStringLiteral("%1 cannot be written").arg(f.fileName());
    if (idOut)
        *idOut = QStringLiteral("file:") + name;
    return QString();
}

QString WifiProfileStore::import(const QString& path, QString* idOut)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("%1 cannot be read").arg(path);
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject())
        return QStringLiteral("not JSON: %1").arg(pe.errorString());
    WifiProfile p;
    const QString err = WifiProfile::fromJson(doc.object(), &p);
    if (!err.isEmpty())
        return err;
    return save(p, idOut);
}

bool WifiProfileStore::remove(const QString& id)
{
    if (!id.startsWith(QStringLiteral("file:")))
        return false;
    const QString name = id.mid(5);
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return false;
    return QFile::remove(QDir(folder()).filePath(name));
}
