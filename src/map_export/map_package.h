#pragma once

// The KOGGER map file, .kmap (docs/map-export.md): what one user saves and sends and another opens in
// KoggerApp as a layer under the boat. A ZIP archive, every entry stored:
//   map.json     the passport (the fields of the KMZ's metadata.json; "format": "KOGGER map")
//   depth.tif    depths, float32 metres positive down, -9999 = none (the GIS export's GeoTIFF)
//   mosaic.tif   the side-scan mosaic, 8-bit intensity index with the maker's palette, 0 = none
//   preview.png  a small picture for the list of maps
//   README.txt   the notice and the licence in plain words
// Either picture may be missing. Renamed to .zip, the GeoTIFFs open in QGIS.

#include <vector>
#include <QImage>
#include <QJsonObject>
#include <QRgb>
#include <QString>
#include <QVector>
#include "map_export_geo.h"

namespace mapexport {

inline const QString kMapSuffix = QStringLiteral("kmap");
inline const QString kMapFormat = QStringLiteral("KOGGER map");
// The type a .kmap is shared under (Android's share sheet). Not registered: this app's own label.
// Android takes it, application/octet-stream and application/zip as a map to open
// (platform/android/AndroidManifest.xml); what the file is, is read from its content.
inline const QString kMapMimeType = QStringLiteral("application/vnd.kogger.map+zip");

struct MapPackage {
    QJsonObject meta;                // map.json
    bool hasDepth = false;
    DepthGrid depth;                 // NaN where there is none
    bool hasMosaic = false;
    GeoGrid mosaicGrid;
    std::vector<uint8_t> mosaic;     // row-major, north up
    QVector<QRgb> mosaicPalette;     // the maker's, 256 entries
    QImage preview;

    QString title() const;
    QString author() const;
    QString licenseId() const;
    QString licenseName() const;
    QString copyright() const;
    QString disclaimer() const;
    /// west, south, east, north from map.json, else from the grids
    void bounds(double& west, double& south, double& east, double& north) const;
};

/// Reads a .kmap. False with a reason in plain words when it is not one or is damaged.
bool readMapPackage(const QString& path, MapPackage& out, QString* error);

enum class MapCopy { Copied, CannotRead, NotAMap, CannotWrite };
/// Copies a map file -- a path, or on Android a content:// URL another app handed over -- to `dst`
/// in pieces, read once front to back, and gives its SHA-1 as hex. A file that does not start as a
/// ZIP archive is NotAMap before anything is written: a messenger's file may come without a name or
/// as application/octet-stream, so the content decides, not the name. On any failure `dst` is not
/// left behind; `error` gets the system's reason for CannotRead / CannotWrite.
MapCopy copyMapFile(const QString& src, const QString& dst, QByteArray* sha1Hex, QString* error);
/// True when the file starts as every .kmap does (a ZIP archive): how a map is told where its name
/// says nothing, as with an Android content:// URL. A log (KLF, XTF, UBX) does not start so.
bool startsLikeMap(const QString& src);

/// Depth at a latitude/longitude, bilinear between the four nearest cells with a depth (nearest
/// when fewer); NaN outside the map or over a gap.
double depthAt(const DepthGrid& g, double latDeg, double lonDeg);

} // namespace mapexport
