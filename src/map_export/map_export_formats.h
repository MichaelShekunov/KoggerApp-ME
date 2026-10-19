#pragma once

// File formats of a map export (docs/map-export.md), written without GDAL, libtiff or minizip:
// a GeoTIFF (tiled, deflate) on a WGS 84 lat/lon grid, a ZIP archive (the KMZ), indexed PNGs.

#include <cstdint>
#include <functional>
#include <vector>
#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QList>
#include <QStringList>
#include <QPair>
#include <QRgb>
#include <QString>
#include <QVector>
#include "map_export_geo.h"

namespace mapexport {

/// One GeoTIFF band on `grid` (EPSG:4326, pixel is area), 256 x 256 tiles compressed with deflate.
struct GeoTiffSpec {
    enum class Kind { Palette8, Float32 };
    Kind kind = Kind::Palette8;
    GeoGrid grid;
    QVector<QRgb> palette;          // Palette8: 256 entries (alpha is not stored; nodata marks the empty)
    QString noData;                 // GDAL_NODATA, e.g. "0" or "-9999"
    QString description;            // ImageDescription
    QString copyright;              // Copyright
    QString software;               // Software
    QList<QPair<QString, QString>> metadata;   // GDAL_METADATA dataset items
    QString bandDescription;        // GDAL_METADATA band 0 description
    QString bandUnit;               // GDAL_METADATA band 0 unit type
};

/// Writes the GeoTIFF tile by tile: fill(x0, y0, w, h, dst) puts a tile's samples, row-major and
/// north up, into dst (256 x 256 samples, already set to nodata; w, h are its part inside the grid).
/// progress(0..1) returns false to stop. Classic TIFF: over 4 GB it fails with an error.
bool writeGeoTiff(const QString& path, const GeoTiffSpec& spec,
                  const std::function<void(int, int, int, int, void*)>& fill,
                  const std::function<bool(double)>& progress, QString* error);

/// One band of a GeoTIFF as writeGeoTiff writes it, read back (readGeoTiff): the grid, the samples
/// row-major and north up, the palette of an 8-bit one, GDAL_NODATA and GDAL_METADATA's items.
struct GeoRaster {
    GeoGrid grid;
    bool isFloat = false;
    std::vector<uint8_t> u8;
    std::vector<float> f32;
    QVector<QRgb> palette;
    QString noData;
    QList<QPair<QString, QString>> metadata;
};

/// Reads what writeGeoTiff writes (classic little-endian TIFF, 256 x 256 tiles, deflate or none,
/// 8-bit palette or float32, EPSG:4326 pixel-is-area) -- not GeoTIFF in general.
bool readGeoTiff(const QByteArray& data, GeoRaster& out, QString* error);

/// A ZIP archive written front to back. KML viewers read the first .kml entry as the document.
class ZipWriter {
public:
    bool open(const QString& path, QString* error);
    /// deflate: compressed with deflate (worth it for text; a PNG is stored as it is).
    bool add(const QString& name, const QByteArray& data, bool deflate, QString* error);
    /// A file on disk, stored as it is, copied in pieces (a large GeoTIFF need not be in memory).
    bool addFile(const QString& name, const QString& path, QString* error);
    bool close(QString* error);
    ~ZipWriter();
private:
    struct Entry {
        QByteArray name;
        quint32 crc = 0;
        quint32 compSize = 0;
        quint32 size = 0;
        quint16 method = 0;
        quint32 offset = 0;
    };
    bool writeLocalHeader(Entry& e, QString* error);
    QFile file_;
    QList<Entry> entries_;
    quint16 dosTime_ = 0;
    quint16 dosDate_ = 0;
};

/// Reads the stored entries of a ZIP archive (the .kmap package is written stored only; a deflated
/// entry is reported, not inflated: Qt has no public raw-deflate reader).
class ZipReader {
public:
    bool open(const QString& path, QString* error);
    QStringList names() const;
    bool has(const QString& name) const { return entries_.contains(name); }
    bool read(const QString& name, QByteArray& out, QString* error);
private:
    struct Entry { quint32 offset = 0; quint32 size = 0; quint32 compSize = 0; quint16 method = 0; quint32 crc = 0; };
    QFile file_;
    QHash<QString, Entry> entries_;
    QStringList order_;
};

quint32 crc32(const QByteArray& data);
/// zlib stream (RFC 1950) of data, as TIFF compression 8 wants it.
QByteArray zlibCompress(const QByteArray& data);
/// raw deflate (RFC 1951), as a ZIP entry wants it.
QByteArray rawDeflate(const QByteArray& data);

/// An 8-bit indexed PNG: palette[i] for index i, alpha included (tRNS).
QByteArray indexedPng(const uint8_t* idx, int w, int h, const QVector<QRgb>& palette);

} // namespace mapexport
