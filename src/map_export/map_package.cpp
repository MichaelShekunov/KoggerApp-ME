#include "map_package.h"

#include <cmath>
#include <limits>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QtEndian>
#include "map_export_formats.h"

namespace mapexport {

QString MapPackage::title() const
{
    return meta.value(QStringLiteral("title")).toString();
}

QString MapPackage::author() const
{
    return meta.value(QStringLiteral("provider")).toObject().value(QStringLiteral("name")).toString();
}

QString MapPackage::licenseId() const
{
    return meta.value(QStringLiteral("license")).toObject().value(QStringLiteral("id")).toString();
}

QString MapPackage::licenseName() const
{
    return meta.value(QStringLiteral("license")).toObject().value(QStringLiteral("name")).toString();
}

QString MapPackage::copyright() const
{
    return meta.value(QStringLiteral("license")).toObject().value(QStringLiteral("notice")).toString();
}

QString MapPackage::disclaimer() const
{
    return meta.value(QStringLiteral("disclaimer")).toString();
}

void MapPackage::bounds(double& west, double& south, double& east, double& north) const
{
    const QJsonObject b = meta.value(QStringLiteral("bounds")).toObject();
    if (b.contains(QStringLiteral("west"))) {
        west = b.value(QStringLiteral("west")).toDouble();
        south = b.value(QStringLiteral("south")).toDouble();
        east = b.value(QStringLiteral("east")).toDouble();
        north = b.value(QStringLiteral("north")).toDouble();
        return;
    }
    west = 1e9; south = 1e9; east = -1e9; north = -1e9;
    auto take = [&](const GeoGrid& g) {
        west = std::min(west, g.west); east = std::max(east, g.east());
        south = std::min(south, g.south()); north = std::max(north, g.north);
    };
    if (hasDepth) {
        take(depth.grid);
    }
    if (hasMosaic) {
        take(mosaicGrid);
    }
}

bool readMapPackage(const QString& path, MapPackage& out, QString* error)
{
    out = MapPackage();
    auto fail = [&](const QString& why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    ZipReader z;
    QString err;
    if (!z.open(path, &err)) {
        return fail(err);
    }
    QByteArray data;
    if (!z.read(QStringLiteral("map.json"), data, &err)) {
        return fail(QStringLiteral("not a KOGGER map: %1").arg(err));
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
    if (!doc.isObject() || doc.object().value(QStringLiteral("format")).toString() != kMapFormat) {
        return fail(QStringLiteral("not a KOGGER map: map.json is not one"));
    }
    out.meta = doc.object();
    if (out.meta.value(QStringLiteral("formatVersion")).toInt() > 1) {
        return fail(QStringLiteral("the map was made by a newer KoggerApp (format %1)").arg(out.meta.value(QStringLiteral("formatVersion")).toInt()));
    }

    if (z.has(QStringLiteral("depth.tif"))) {
        GeoRaster r;
        if (!z.read(QStringLiteral("depth.tif"), data, &err) || !readGeoTiff(data, r, &err) || !r.isFloat) {
            return fail(QStringLiteral("the depths cannot be read: %1").arg(err.isEmpty() ? QStringLiteral("not float32") : err));
        }
        const float noData = r.noData.isEmpty() ? -9999.0f : r.noData.toFloat();
        out.depth.grid = r.grid;
        out.depth.depth = std::move(r.f32);
        float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
        for (float& v : out.depth.depth) {
            if (v == noData || !std::isfinite(v)) {
                v = std::numeric_limits<float>::quiet_NaN();
                continue;
            }
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        out.hasDepth = lo <= hi;
        out.depth.minDepth = out.hasDepth ? lo : 0.0f;
        out.depth.maxDepth = out.hasDepth ? hi : 0.0f;
        out.depth.soundings = out.meta.value(QStringLiteral("products")).toObject()
                                  .value(QStringLiteral("bathymetry")).toObject().value(QStringLiteral("soundings")).toInt();
    }
    if (z.has(QStringLiteral("mosaic.tif"))) {
        GeoRaster r;
        if (!z.read(QStringLiteral("mosaic.tif"), data, &err) || !readGeoTiff(data, r, &err) || r.isFloat) {
            return fail(QStringLiteral("the mosaic cannot be read: %1").arg(err.isEmpty() ? QStringLiteral("not 8-bit") : err));
        }
        out.mosaicGrid = r.grid;
        out.mosaic = std::move(r.u8);
        out.mosaicPalette = r.palette;
        out.hasMosaic = true;
    }
    if (!out.hasDepth && !out.hasMosaic) {
        return fail(QStringLiteral("the map has neither depths nor a mosaic"));
    }
    if (z.has(QStringLiteral("preview.png")) && z.read(QStringLiteral("preview.png"), data, nullptr)) {
        out.preview = QImage::fromData(data, "PNG");
    }
    return true;
}

namespace {

// A .kmap is a ZIP archive whose first entry starts the file: "PK\3\4".
bool isZipStart(const QByteArray& head)
{
    return head.size() >= 4 && qFromLittleEndian<quint32>(head.constData()) == 0x04034b50u;
}

} // namespace

bool startsLikeMap(const QString& src)
{
    QFile in(src);
    return in.open(QIODevice::ReadOnly) && isZipStart(in.read(4));
}

MapCopy copyMapFile(const QString& src, const QString& dst, QByteArray* sha1Hex, QString* error)
{
    QFile in(src);
    if (!in.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = in.errorString();
        }
        return MapCopy::CannotRead;
    }
    QByteArray piece = in.read(4);
    if (!isZipStart(piece)) {
        return MapCopy::NotAMap;
    }
    QFile out(dst);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = out.errorString();
        }
        return MapCopy::CannotWrite;
    }
    auto abandon = [&](MapCopy why, const QString& reason) {
        out.close();
        QFile::remove(dst);
        if (error) {
            *error = reason;
        }
        return why;
    };
    QCryptographicHash hash(QCryptographicHash::Sha1);
    while (!piece.isEmpty()) {
        hash.addData(piece);
        if (out.write(piece) != piece.size()) {
            return abandon(MapCopy::CannotWrite, out.errorString());
        }
        piece = in.read(1 << 20);
    }
    if (in.error() != QFileDevice::NoError) {   // read() gives nothing both at the end and on an error
        return abandon(MapCopy::CannotRead, in.errorString());
    }
    if (!out.flush()) {
        return abandon(MapCopy::CannotWrite, out.errorString());
    }
    out.close();
    if (sha1Hex) {
        *sha1Hex = hash.result().toHex();
    }
    return MapCopy::Copied;
}

double depthAt(const DepthGrid& g, double latDeg, double lonDeg)
{
    const GeoGrid& gr = g.grid;
    if (!gr.isValid() || g.depth.size() != size_t(gr.width) * size_t(gr.height)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // Pixel centres are at (c + 0.5, r + 0.5).
    const double x = (lonDeg - gr.west) / gr.dLon - 0.5;
    const double y = (gr.north - latDeg) / gr.dLat - 0.5;
    if (x < -0.5 || y < -0.5 || x > gr.width - 0.5 || y > gr.height - 0.5) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const int c0 = int(std::floor(x)), r0 = int(std::floor(y));
    const double fx = x - c0, fy = y - r0;
    double sum = 0.0, wsum = 0.0, nearest = std::numeric_limits<double>::quiet_NaN(), nearestW = -1.0;
    for (int dr = 0; dr <= 1; ++dr) {
        for (int dc = 0; dc <= 1; ++dc) {
            const int r = r0 + dr, c = c0 + dc;
            if (r < 0 || c < 0 || r >= gr.height || c >= gr.width) {
                continue;
            }
            const float v = g.depth[size_t(r) * size_t(gr.width) + size_t(c)];
            const double w = (dc ? fx : 1.0 - fx) * (dr ? fy : 1.0 - fy);
            if (!std::isfinite(v)) {
                continue;
            }
            sum += w * v;
            wsum += w;
            if (w > nearestW) {
                nearestW = w;
                nearest = v;
            }
        }
    }
    if (wsum <= 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // All four: bilinear. At a gap's edge: the nearest one with a depth, not an average leaning into it.
    return wsum > 0.999 ? sum / wsum : nearest;
}

} // namespace mapexport
