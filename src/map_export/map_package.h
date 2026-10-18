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

/// Depth at a latitude/longitude, bilinear between the four nearest cells with a depth (nearest
/// when fewer); NaN outside the map or over a gap.
double depthAt(const DepthGrid& g, double latDeg, double lonDeg);

} // namespace mapexport
