#include "map_export_formats.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <QBuffer>
#include <QDateTime>
#include <QImage>
#include <QRegularExpression>
#include <QtEndian>

namespace mapexport {

namespace {

// TIFF tag types
constexpr quint16 kAscii = 2, kShort = 3, kLong = 4, kDouble = 12;
constexpr int kTile = 256;

QByteArray xmlEscape(const QString& s)
{
    return s.toHtmlEscaped().toUtf8();
}

struct TiffTag {
    quint16 tag;
    quint16 type;
    quint32 count;
    QByteArray data;   // little-endian values
};

QByteArray shorts(const QVector<quint16>& v)
{
    QByteArray b;
    b.resize(v.size() * 2);
    for (int i = 0; i < v.size(); ++i) {
        qToLittleEndian<quint16>(v[i], b.data() + 2 * i);
    }
    return b;
}

QByteArray longs(const QVector<quint32>& v)
{
    QByteArray b;
    b.resize(v.size() * 4);
    for (int i = 0; i < v.size(); ++i) {
        qToLittleEndian<quint32>(v[i], b.data() + 4 * i);
    }
    return b;
}

QByteArray doubles(const QVector<double>& v)
{
    QByteArray b;
    b.resize(v.size() * 8);
    for (int i = 0; i < v.size(); ++i) {
        quint64 u;
        std::memcpy(&u, &v[i], 8);
        qToLittleEndian<quint64>(u, b.data() + 8 * i);
    }
    return b;
}

TiffTag asciiTag(quint16 tag, const QByteArray& text)
{
    QByteArray z = text;
    z.append('\0');
    return { tag, kAscii, quint32(z.size()), z };
}

TiffTag shortTag(quint16 tag, const QVector<quint16>& v) { return { tag, kShort, quint32(v.size()), shorts(v) }; }
TiffTag longTag(quint16 tag, const QVector<quint32>& v)  { return { tag, kLong, quint32(v.size()), longs(v) }; }
TiffTag doubleTag(quint16 tag, const QVector<double>& v) { return { tag, kDouble, quint32(v.size()), doubles(v) }; }

} // namespace

namespace {

// CRC-32 (ZIP's), fed in pieces: start with 0, pass the result back in.
quint32 crc32Update(quint32 crc, const char* data, qsizetype n)
{
    static const auto table = []() {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    const auto* p = reinterpret_cast<const uchar*>(data);
    for (qsizetype i = 0; i < n; ++i) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

} // namespace

quint32 crc32(const QByteArray& data)
{
    return crc32Update(0, data.constData(), data.size());
}

QByteArray zlibCompress(const QByteArray& data)
{
    // qCompress: 4 bytes of the size (big-endian), then a zlib stream.
    return qCompress(data, 6).mid(4);
}

QByteArray rawDeflate(const QByteArray& data)
{
    // A zlib stream is 2 bytes of header, the deflate data, 4 bytes of Adler-32.
    const QByteArray z = zlibCompress(data);
    return z.size() > 6 ? z.mid(2, z.size() - 6) : QByteArray();
}

QByteArray indexedPng(const uint8_t* idx, int w, int h, const QVector<QRgb>& palette)
{
    QImage img(w, h, QImage::Format_Indexed8);
    img.setColorTable(palette);
    for (int y = 0; y < h; ++y) {
        std::memcpy(img.scanLine(y), idx + size_t(y) * size_t(w), size_t(w));
    }
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return out;
}

bool writeGeoTiff(const QString& path, const GeoTiffSpec& spec,
                  const std::function<void(int, int, int, int, void*)>& fill,
                  const std::function<bool(double)>& progress, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    const GeoGrid& g = spec.grid;
    if (!g.isValid()) {
        return fail(QStringLiteral("empty grid"));
    }
    const bool isFloat = spec.kind == GeoTiffSpec::Kind::Float32;
    const int bytesPerSample = isFloat ? 4 : 1;
    const int tilesX = (g.width + kTile - 1) / kTile;
    const int tilesY = (g.height + kTile - 1) / kTile;
    const qint64 nTiles = qint64(tilesX) * tilesY;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(f.errorString());
    }
    // Header; the first IFD's offset is set at the end.
    QByteArray header("II\x2A\0\0\0\0\0", 8);
    f.write(header);

    QVector<quint32> offsets, counts;
    offsets.reserve(int(nTiles));
    counts.reserve(int(nTiles));
    QByteArray tile(kTile * kTile * bytesPerSample, Qt::Uninitialized);
    float noDataF = spec.noData.toFloat();
    const uint8_t noData8 = uint8_t(spec.noData.toInt());
    qint64 done = 0;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            if (isFloat) {
                auto* p = reinterpret_cast<float*>(tile.data());
                std::fill(p, p + kTile * kTile, noDataF);
            }
            else {
                std::memset(tile.data(), noData8, size_t(tile.size()));
            }
            const int x0 = tx * kTile, y0 = ty * kTile;
            fill(x0, y0, std::min(kTile, g.width - x0), std::min(kTile, g.height - y0), tile.data());
            if (isFloat) {   // stored little-endian
                auto* p = reinterpret_cast<quint32*>(tile.data());
                for (int i = 0; i < kTile * kTile; ++i) {
                    p[i] = qToLittleEndian(p[i]);
                }
            }
            const QByteArray z = zlibCompress(tile);
            const qint64 at = f.pos();
            if (at + z.size() > qint64(0xFFFFFFF0LL)) {
                f.close();
                f.remove();
                return fail(QStringLiteral("the GeoTIFF would be over 4 GB; choose a coarser detail"));
            }
            if (f.write(z) != z.size()) {
                return fail(f.errorString());
            }
            offsets.append(quint32(at));
            counts.append(quint32(z.size()));
            ++done;
            if (progress && (done % 64) == 0 && !progress(double(done) / double(nTiles))) {
                f.close();
                f.remove();
                return false;
            }
        }
    }

    // GeoKeys: geographic WGS 84, pixel is area, degrees.
    const QVector<quint16> geoKeys = {
        1, 1, 0, 4,
        1024, 0, 1, 2,      // GTModelTypeGeoKey = ModelTypeGeographic
        1025, 0, 1, 1,      // GTRasterTypeGeoKey = RasterPixelIsArea
        2048, 0, 1, 4326,   // GeographicTypeGeoKey = WGS 84
        2054, 0, 1, 9102,   // GeogAngularUnitsGeoKey = degree
    };

    QByteArray gdalMeta = "<GDALMetadata>\n";
    for (const auto& kv : spec.metadata) {
        gdalMeta += "  <Item name=\"" + xmlEscape(kv.first) + "\">" + xmlEscape(kv.second) + "</Item>\n";
    }
    if (!spec.bandDescription.isEmpty()) {
        gdalMeta += "  <Item name=\"DESCRIPTION\" sample=\"0\" role=\"description\">" + xmlEscape(spec.bandDescription) + "</Item>\n";
    }
    if (!spec.bandUnit.isEmpty()) {
        gdalMeta += "  <Item name=\"UNITTYPE\" sample=\"0\" role=\"unittype\">" + xmlEscape(spec.bandUnit) + "</Item>\n";
    }
    gdalMeta += "</GDALMetadata>";

    QList<TiffTag> tags;
    tags << longTag(256, { quint32(g.width) })
         << longTag(257, { quint32(g.height) })
         << shortTag(258, { quint16(isFloat ? 32 : 8) })
         << shortTag(259, { 8 })                            // Adobe deflate
         << shortTag(262, { quint16(isFloat ? 1 : 3) });    // BlackIsZero / Palette
    if (!spec.description.isEmpty()) {
        tags << asciiTag(270, spec.description.toUtf8());
    }
    tags << shortTag(277, { 1 });
    tags << shortTag(284, { 1 });
    if (!spec.software.isEmpty()) {
        tags << asciiTag(305, spec.software.toUtf8());
    }
    if (!isFloat) {
        QVector<quint16> cmap(3 * 256, 0);
        for (int i = 0; i < 256; ++i) {
            const QRgb c = i < spec.palette.size() ? spec.palette[i] : qRgb(i, i, i);
            cmap[i]       = quint16(qRed(c) * 257);
            cmap[256 + i] = quint16(qGreen(c) * 257);
            cmap[512 + i] = quint16(qBlue(c) * 257);
        }
        tags << shortTag(320, cmap);
    }
    tags << longTag(322, { quint32(kTile) })
         << longTag(323, { quint32(kTile) })
         << longTag(324, offsets)
         << longTag(325, counts)
         << shortTag(339, { quint16(isFloat ? 3 : 1) });   // IEEE float / unsigned
    if (!spec.copyright.isEmpty()) {
        tags << asciiTag(33432, spec.copyright.toUtf8());
    }
    tags << doubleTag(33550, { g.dLon, g.dLat, 0.0 })
         << doubleTag(33922, { 0.0, 0.0, 0.0, g.west, g.north, 0.0 })
         << shortTag(34735, geoKeys)
         << asciiTag(42112, gdalMeta);
    if (!spec.noData.isEmpty()) {
        tags << asciiTag(42113, spec.noData.toUtf8());
    }
    std::sort(tags.begin(), tags.end(), [](const TiffTag& a, const TiffTag& b) { return a.tag < b.tag; });

    // The IFD, word aligned, and after it the values that do not fit in 4 bytes.
    if (f.pos() & 1) {
        f.write("\0", 1);
    }
    const qint64 ifdAt = f.pos();
    const qint64 ifdSize = 2 + qint64(tags.size()) * 12 + 4;
    qint64 extraAt = ifdAt + ifdSize;
    QByteArray ifd, extra;
    QByteArray n2(2, '\0');
    qToLittleEndian<quint16>(quint16(tags.size()), n2.data());
    ifd += n2;
    for (const TiffTag& t : std::as_const(tags)) {
        QByteArray e(12, '\0');
        qToLittleEndian<quint16>(t.tag, e.data());
        qToLittleEndian<quint16>(t.type, e.data() + 2);
        qToLittleEndian<quint32>(t.count, e.data() + 4);
        if (t.data.size() <= 4) {
            std::memcpy(e.data() + 8, t.data.constData(), size_t(t.data.size()));
        }
        else {
            const qint64 off = extraAt + extra.size();
            qToLittleEndian<quint32>(quint32(off), e.data() + 8);
            extra += t.data;
            if (extra.size() & 1) {
                extra += '\0';
            }
        }
        ifd += e;
    }
    ifd += QByteArray(4, '\0');   // no next IFD
    if (extraAt + extra.size() > qint64(0xFFFFFFF0LL)) {
        f.close();
        f.remove();
        return fail(QStringLiteral("the GeoTIFF would be over 4 GB; choose a coarser detail"));
    }
    f.write(ifd);
    f.write(extra);
    QByteArray at4(4, '\0');
    qToLittleEndian<quint32>(quint32(ifdAt), at4.data());
    f.seek(4);
    f.write(at4);
    if (f.error() != QFileDevice::NoError) {
        return fail(f.errorString());
    }
    f.close();
    if (progress) {
        progress(1.0);
    }
    return true;
}

ZipWriter::~ZipWriter()
{
    if (file_.isOpen()) {
        file_.close();
    }
}

bool ZipWriter::open(const QString& path, QString* error)
{
    file_.setFileName(path);
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = file_.errorString();
        }
        return false;
    }
    const QDateTime now = QDateTime::currentDateTime();
    dosTime_ = quint16((now.time().hour() << 11) | (now.time().minute() << 5) | (now.time().second() / 2));
    dosDate_ = quint16(((std::max(1980, now.date().year()) - 1980) << 9) | (now.date().month() << 5) | now.date().day());
    entries_.clear();
    return true;
}

bool ZipWriter::add(const QString& name, const QByteArray& data, bool deflate, QString* error)
{
    Entry e;
    e.name = name.toUtf8();
    e.crc = crc32(data);
    e.size = quint32(data.size());
    QByteArray body = data;
    e.method = 0;
    if (deflate) {
        const QByteArray d = rawDeflate(data);
        if (!d.isEmpty() && d.size() < data.size()) {
            body = d;
            e.method = 8;
        }
    }
    e.compSize = quint32(body.size());
    if (!writeLocalHeader(e, error)) {
        return false;
    }
    if (file_.write(body) != body.size()) {
        if (error) {
            *error = file_.errorString();
        }
        return false;
    }
    entries_.append(e);
    return true;
}

bool ZipWriter::addFile(const QString& name, const QString& path, QString* error)
{
    QFile src(path);
    if (!src.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = src.errorString();
        }
        return false;
    }
    // The CRC first (the local header comes before the data), then the copy.
    quint32 crc = 0;
    QByteArray buf;
    while (!(buf = src.read(1 << 20)).isEmpty()) {
        crc = crc32Update(crc, buf.constData(), buf.size());
    }
    Entry e;
    e.name = name.toUtf8();
    e.crc = crc;
    e.size = e.compSize = quint32(src.size());
    e.method = 0;
    if (src.size() > qint64(0xFFFFFFF0LL) || !writeLocalHeader(e, error)) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("a file over 4 GB cannot go into the archive");
        }
        return false;
    }
    src.seek(0);
    while (!(buf = src.read(1 << 20)).isEmpty()) {
        if (file_.write(buf) != buf.size()) {
            if (error) {
                *error = file_.errorString();
            }
            return false;
        }
    }
    entries_.append(e);
    return true;
}

bool ZipWriter::writeLocalHeader(Entry& e, QString* error)
{
    const qint64 at = file_.pos();
    if (at + 30 + e.name.size() + qint64(e.compSize) > qint64(0xFFFFFFF0LL) || entries_.size() >= 65535) {
        if (error) {
            *error = QStringLiteral("the archive would be over 4 GB or 65 535 files; choose a coarser detail");
        }
        return false;
    }
    e.offset = quint32(at);

    QByteArray h(30, '\0');
    char* p = h.data();
    qToLittleEndian<quint32>(0x04034b50u, p);
    qToLittleEndian<quint16>(20, p + 4);          // version needed
    qToLittleEndian<quint16>(0x0800, p + 6);      // names in UTF-8
    qToLittleEndian<quint16>(e.method, p + 8);
    qToLittleEndian<quint16>(dosTime_, p + 10);
    qToLittleEndian<quint16>(dosDate_, p + 12);
    qToLittleEndian<quint32>(e.crc, p + 14);
    qToLittleEndian<quint32>(e.compSize, p + 18);
    qToLittleEndian<quint32>(e.size, p + 22);
    qToLittleEndian<quint16>(quint16(e.name.size()), p + 26);
    qToLittleEndian<quint16>(0, p + 28);
    if (file_.write(h) != h.size() || file_.write(e.name) != e.name.size()) {
        if (error) {
            *error = file_.errorString();
        }
        return false;
    }
    return true;
}

bool ZipWriter::close(QString* error)
{
    const qint64 cdAt = file_.pos();
    QByteArray cd;
    for (const Entry& e : std::as_const(entries_)) {
        QByteArray h(46, '\0');
        char* p = h.data();
        qToLittleEndian<quint32>(0x02014b50u, p);
        qToLittleEndian<quint16>(20, p + 4);       // made by
        qToLittleEndian<quint16>(20, p + 6);       // version needed
        qToLittleEndian<quint16>(0x0800, p + 8);
        qToLittleEndian<quint16>(e.method, p + 10);
        qToLittleEndian<quint16>(dosTime_, p + 12);
        qToLittleEndian<quint16>(dosDate_, p + 14);
        qToLittleEndian<quint32>(e.crc, p + 16);
        qToLittleEndian<quint32>(e.compSize, p + 20);
        qToLittleEndian<quint32>(e.size, p + 24);
        qToLittleEndian<quint16>(quint16(e.name.size()), p + 28);
        qToLittleEndian<quint32>(e.offset, p + 42);
        cd += h;
        cd += e.name;
    }
    QByteArray end(22, '\0');
    char* p = end.data();
    qToLittleEndian<quint32>(0x06054b50u, p);
    qToLittleEndian<quint16>(quint16(entries_.size()), p + 8);
    qToLittleEndian<quint16>(quint16(entries_.size()), p + 10);
    qToLittleEndian<quint32>(quint32(cd.size()), p + 12);
    qToLittleEndian<quint32>(quint32(cdAt), p + 16);
    const bool ok = file_.write(cd) == cd.size() && file_.write(end) == end.size();
    if (!ok && error) {
        *error = file_.errorString();
    }
    file_.close();
    return ok;
}

bool ZipReader::open(const QString& path, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    entries_.clear();
    order_.clear();
    file_.setFileName(path);
    if (!file_.open(QIODevice::ReadOnly)) {
        return fail(file_.errorString());
    }
    // The end record is in the last 64 KB + 22 bytes (a comment may follow it).
    const qint64 size = file_.size();
    const qint64 tail = std::min<qint64>(size, 65557);
    file_.seek(size - tail);
    const QByteArray t = file_.read(tail);
    qsizetype at = -1;
    for (qsizetype i = t.size() - 22; i >= 0; --i) {
        if (qFromLittleEndian<quint32>(t.constData() + i) == 0x06054b50u) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        return fail(QStringLiteral("not a ZIP archive"));
    }
    const quint16 count = qFromLittleEndian<quint16>(t.constData() + at + 10);
    const quint32 cdSize = qFromLittleEndian<quint32>(t.constData() + at + 12);
    const quint32 cdAt = qFromLittleEndian<quint32>(t.constData() + at + 16);
    if (qint64(cdAt) + cdSize > size) {
        return fail(QStringLiteral("a damaged ZIP archive"));
    }
    file_.seek(cdAt);
    const QByteArray cd = file_.read(cdSize);
    qsizetype p = 0;
    for (int i = 0; i < count; ++i) {
        if (p + 46 > cd.size() || qFromLittleEndian<quint32>(cd.constData() + p) != 0x02014b50u) {
            return fail(QStringLiteral("a damaged ZIP directory"));
        }
        const char* h = cd.constData() + p;
        Entry e;
        e.method = qFromLittleEndian<quint16>(h + 10);
        e.crc = qFromLittleEndian<quint32>(h + 16);
        e.compSize = qFromLittleEndian<quint32>(h + 20);
        e.size = qFromLittleEndian<quint32>(h + 24);
        const quint16 nameLen = qFromLittleEndian<quint16>(h + 28);
        const quint16 extraLen = qFromLittleEndian<quint16>(h + 30);
        const quint16 commentLen = qFromLittleEndian<quint16>(h + 32);
        e.offset = qFromLittleEndian<quint32>(h + 42);
        if (p + 46 + nameLen > cd.size()) {
            return fail(QStringLiteral("a damaged ZIP directory"));
        }
        const QString name = QString::fromUtf8(h + 46, nameLen);
        entries_.insert(name, e);
        order_ << name;
        p += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

QStringList ZipReader::names() const
{
    return order_;
}

bool ZipReader::read(const QString& name, QByteArray& out, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    const auto it = entries_.constFind(name);
    if (it == entries_.cend()) {
        return fail(QStringLiteral("%1 is not in the archive").arg(name));
    }
    if (it->method != 0) {
        return fail(QStringLiteral("%1 is compressed; only stored entries are read").arg(name));
    }
    file_.seek(it->offset);
    const QByteArray h = file_.read(30);
    if (h.size() != 30 || qFromLittleEndian<quint32>(h.constData()) != 0x04034b50u) {
        return fail(QStringLiteral("a damaged ZIP entry: %1").arg(name));
    }
    const quint16 nameLen = qFromLittleEndian<quint16>(h.constData() + 26);
    const quint16 extraLen = qFromLittleEndian<quint16>(h.constData() + 28);
    file_.seek(qint64(it->offset) + 30 + nameLen + extraLen);
    out = file_.read(it->compSize);
    if (out.size() != qsizetype(it->compSize) || crc32(out) != it->crc) {
        return fail(QStringLiteral("a damaged ZIP entry (CRC): %1").arg(name));
    }
    return true;
}

bool readGeoTiff(const QByteArray& f, GeoRaster& out, QString* error)
{
    out = GeoRaster();
    auto fail = [&](const QString& why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    if (f.size() < 8 || f.left(4) != QByteArray("II\x2A\0", 4)) {
        return fail(QStringLiteral("not a little-endian TIFF"));
    }
    auto u16 = [&](qint64 at) { return qFromLittleEndian<quint16>(f.constData() + at); };
    auto u32 = [&](qint64 at) { return qFromLittleEndian<quint32>(f.constData() + at); };
    struct Tag { quint16 type; quint32 count; qint64 at; };
    std::map<quint16, Tag> tags;
    const qint64 ifd = u32(4);
    if (ifd + 2 > f.size()) {
        return fail(QStringLiteral("a damaged TIFF"));
    }
    const quint16 n = u16(ifd);
    if (ifd + 2 + 12LL * n > f.size()) {
        return fail(QStringLiteral("a damaged TIFF"));
    }
    for (quint16 i = 0; i < n; ++i) {
        const qint64 e = ifd + 2 + 12LL * i;
        const quint16 type = u16(e + 2);
        const quint32 count = u32(e + 4);
        const int unit = type == 3 ? 2 : type == 4 ? 4 : type == 12 ? 8 : 1;
        const qint64 bytes = qint64(unit) * count;
        const qint64 at = bytes <= 4 ? e + 8 : qint64(u32(e + 8));
        if (at + bytes > f.size()) {
            return fail(QStringLiteral("a damaged TIFF tag %1").arg(u16(e)));
        }
        tags[u16(e)] = { type, count, at };
    }
    auto nums = [&](quint16 tag) {
        std::vector<double> v;
        const auto it = tags.find(tag);
        if (it == tags.end()) {
            return v;
        }
        for (quint32 i = 0; i < it->second.count; ++i) {
            switch (it->second.type) {
            case 3: v.push_back(u16(it->second.at + 2LL * i)); break;
            case 4: v.push_back(u32(it->second.at + 4LL * i)); break;
            case 12: {
                const quint64 b = qFromLittleEndian<quint64>(f.constData() + it->second.at + 8LL * i);
                double d;
                std::memcpy(&d, &b, 8);
                v.push_back(d);
                break;
            }
            default: break;
            }
        }
        return v;
    };
    auto text = [&](quint16 tag) {
        const auto it = tags.find(tag);
        if (it == tags.end() || it->second.type != 2 || it->second.count == 0) {
            return QString();
        }
        return QString::fromUtf8(f.constData() + it->second.at, int(it->second.count) - 1);
    };
    auto one = [&](quint16 tag, double def) { const auto v = nums(tag); return v.empty() ? def : v[0]; };

    const int w = int(one(256, 0)), h = int(one(257, 0));
    const int bits = int(one(258, 0)), compression = int(one(259, 1)), fmt = int(one(339, 1));
    const int tw = int(one(322, 0)), th = int(one(323, 0));
    const auto offsets = nums(324), counts = nums(325);
    const auto scale = nums(33550), tie = nums(33922), keys = nums(34735);
    out.isFloat = bits == 32 && fmt == 3;
    if (w <= 0 || h <= 0 || tw <= 0 || th <= 0 || !(out.isFloat || (bits == 8 && fmt == 1)) || (compression != 1 && compression != 8)) {
        return fail(QStringLiteral("a TIFF this app does not write (size, tiles, sample format or compression)"));
    }
    bool wgs84 = false;
    for (size_t i = 4; i + 3 < keys.size(); i += 4) {
        wgs84 = wgs84 || (int(keys[i]) == 2048 && int(keys[i + 3]) == 4326);
    }
    if (scale.size() < 2 || tie.size() < 6 || !wgs84 || tie[0] != 0.0 || tie[1] != 0.0) {
        return fail(QStringLiteral("not georeferenced on WGS 84 latitude/longitude"));
    }
    out.grid.width = w;
    out.grid.height = h;
    out.grid.dLon = scale[0];
    out.grid.dLat = scale[1];
    out.grid.west = tie[3];
    out.grid.north = tie[4];
    out.noData = text(42113);
    const QString meta = text(42112);
    static const QRegularExpression item(QStringLiteral("<Item name=\"([^\"]+)\"(?: [^>]*)?>([^<]*)</Item>"));
    for (auto it = item.globalMatch(meta); it.hasNext();) {
        const auto m = it.next();
        if (!m.captured(0).contains(QStringLiteral("sample="))) {
            QString v = m.captured(2);   // what toHtmlEscaped did, undone
            v.replace(QStringLiteral("&lt;"), QStringLiteral("<")).replace(QStringLiteral("&gt;"), QStringLiteral(">"))
             .replace(QStringLiteral("&quot;"), QStringLiteral("\"")).replace(QStringLiteral("&amp;"), QStringLiteral("&"));
            out.metadata << qMakePair(m.captured(1), v);
        }
    }
    if (!out.isFloat) {
        const auto cmap = nums(320);
        if (cmap.size() == 768) {
            out.palette.resize(256);
            for (int i = 0; i < 256; ++i) {
                out.palette[i] = qRgb(int(cmap[i]) / 257, int(cmap[256 + i]) / 257, int(cmap[512 + i]) / 257);
            }
        }
    }

    const int bps = out.isFloat ? 4 : 1;
    const int tilesX = (w + tw - 1) / tw, tilesY = (h + th - 1) / th;
    if (offsets.size() != size_t(tilesX) * size_t(tilesY) || counts.size() != offsets.size()) {
        return fail(QStringLiteral("a damaged TIFF (tiles)"));
    }
    if (out.isFloat) {
        out.f32.assign(size_t(w) * size_t(h), 0.0f);
    }
    else {
        out.u8.assign(size_t(w) * size_t(h), 0);
    }
    const qsizetype tileBytes = qsizetype(tw) * th * bps;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            const size_t i = size_t(ty) * size_t(tilesX) + size_t(tx);
            const qint64 at = qint64(offsets[i]), len = qint64(counts[i]);
            if (at + len > f.size()) {
                return fail(QStringLiteral("a damaged TIFF (tile past the end)"));
            }
            QByteArray s;
            if (compression == 8) {
                QByteArray z(4, '\0');
                qToBigEndian<quint32>(quint32(tileBytes), z.data());
                z += f.mid(at, len);
                s = qUncompress(z);
            }
            else {
                s = f.mid(at, len);
            }
            if (s.size() != tileBytes) {
                return fail(QStringLiteral("a damaged TIFF (tile data)"));
            }
            for (int r = 0; r < th && ty * th + r < h; ++r) {
                const int cw = std::min(tw, w - tx * tw);
                const size_t dst = size_t(ty * th + r) * size_t(w) + size_t(tx * tw);
                if (out.isFloat) {
                    for (int c = 0; c < cw; ++c) {
                        const quint32 u = qFromLittleEndian<quint32>(s.constData() + 4 * (r * tw + c));
                        std::memcpy(&out.f32[dst + size_t(c)], &u, 4);
                    }
                }
                else {
                    std::memcpy(&out.u8[dst], s.constData() + r * tw, size_t(cw));
                }
            }
        }
    }
    return true;
}

} // namespace mapexport
