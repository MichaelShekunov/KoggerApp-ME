#pragma once

// Writes the shared map's files from what the exporter gathered (docs/map-export.md): a KMZ for
// Google Earth (mosaic, depth colours, isobaths, track when allowed, legend with the notice,
// metadata.json, README.txt) and GeoTIFFs for GIS (the mosaic as 8-bit with its palette, the depths
// as float32). Pure: no application threads, so tools/mapexport_check drives it on made-up data.

#include <functional>
#include <QPointF>
#include <QRgb>
#include <QString>
#include <QStringList>
#include <QVector>
#include "map_export_geo.h"
#include "map_export_meta.h"

namespace mapexport {

struct ExportInputs {
    LLARef ref;

    const MosaicSampler* mosaic = nullptr;   // null: no mosaic
    double mosaicMetresPerPixel = 0.16;
    QVector<QRgb> mosaicPalette;             // 256 entries as on screen; index 0 is made transparent

    const DepthGrid* depth = nullptr;        // null: no depths
    double isobathStepM = 1.0;
    int depthTheme = 0;                      // IsobathUtils::colorPalette(theme), as the surface view

    QVector<QPointF> trackNe;                // the boat's track (x = north, y = east); empty: none

    MapMetadata meta;                        // files, bounds and created are filled in here
    bool writeKmap = true;                   // the KOGGER map, for KoggerApp (map_package.h)
    bool writeKmz = true;
    bool writeGeoTiff = true;
    QString folder;
    QString baseName;                        // files are <baseName>.kmz, <baseName>_mosaic.tif, ...
    qint64 mosaicPixelLimit = 400LL * 1000 * 1000;
};

struct ExportResult {
    QStringList files;   // full paths
    QString error;       // empty when it went well or was cancelled
    bool cancelled = false;
};

/// progress(fraction 0..1 of this stage, stage name) returns false to stop.
bool writeMapProducts(ExportInputs in, const std::function<bool(double, const QString&)>& progress, ExportResult& out);

/// The palette of the depth bands: index 0 transparent, index 1 + band the colour of a band
/// (deepest band 0), as SurfaceProcessor::generateExpandedPalette spreads the theme.
QVector<QRgb> depthPalette(int theme, int bands);
/// The isobath step from {0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50} m giving at most 25 isobaths over the range.
double autoIsobathStep(double minDepth, double maxDepth);
/// The band count and the band of a depth, as the surface view colours (isobaths_colored.fsh).
int depthBands(double minDepth, double maxDepth, double step);
int depthBand(double depth, double maxDepth, double step, int bands);

} // namespace mapexport
