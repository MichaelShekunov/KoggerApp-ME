#pragma once

// Geometry of a map export (docs/map-export.md): the lat/lon grid the files are written on, the
// scene's metres of each of its pixels, the painted mosaic sampled there, and the depth surface with
// its isobaths. Nothing here touches the application's threads; tools/mapexport_check runs it alone.

#include <cstdint>
#include <functional>
#include <vector>
#include <QHash>
#include <QPointF>
#include <QVector>
#include <QVector3D>
#include "dataset_defs.h"

namespace mapexport {

/// A WGS 84 latitude/longitude raster (EPSG:4326), north up: row 0 is the northern edge, column 0
/// the western one. (west, north) is the outer corner of pixel (0, 0).
struct GeoGrid {
    double west = 0.0;
    double north = 0.0;
    double dLon = 0.0;   // degrees per pixel, > 0
    double dLat = 0.0;   // degrees per pixel, > 0
    int width = 0;
    int height = 0;

    double lonAt(double col) const { return west + col * dLon; }   // col + 0.5: a pixel's centre
    double latAt(double row) const { return north - row * dLat; }
    double south() const { return north - height * dLat; }
    double east() const { return west + width * dLon; }
    bool isValid() const { return width > 0 && height > 0 && dLon > 0.0 && dLat > 0.0; }
};

/// The scene's (north, east) metres of a latitude/longitude, the very doubles NED(const LLA*, const
/// LLARef*) gives (dataset_defs.h): the same expressions in the same order, with the sines and cosines
/// of a row's latitude and of a column's longitude worked out once per row and per column.
class Projector {
public:
    Projector(const LLARef& ref, const GeoGrid& grid);
    void setRow(int row);                               // the row of the following toNed() calls
    void toNed(int col, double& n, double& e) const;    // the centre of (row, col)
private:
    LLARef ref_;
    GeoGrid grid_;
    std::vector<double> cosDLon_, sinDLon_;
    double sinLat_ = 0.0, cosLat_ = 1.0;
};

/// NED(const LLA*, const LLARef*) of one point, for the tests and the odd conversion.
void llaToNed(const LLARef& ref, double latDeg, double lonDeg, double& n, double& e);
/// The inverse of llaToNed: the spherical branch of LLA(const NED*, const LLARef*) with degrees
/// taken back by the same float constant the forward direction uses (M_DEG_TO_RAD), so a point goes
/// there and back to within micrometres. LLA(const NED*) itself divides by 180/pi and lands about
/// 7.7e-9 of a degree value away (some 4 cm at 55 degrees north).
void nedToLla(const LLARef& ref, double n, double e, double& latDeg, double& lonDeg);

/// The lat/lon grid that holds the box [minN, maxN] x [minE, maxE] of the scene with pixels of
/// about `metres` on each side (the size at the box's middle latitude), plus one pixel all round.
/// pixelLimit caps width * height; past it the grid comes back invalid.
GeoGrid gridForNedBox(const LLARef& ref, double minN, double maxN, double minE, double maxE,
                      double metres, qint64 pixelLimit);

/// One painted mosaic tile: its south-west corner in the scene (the SurfaceTile origin) and its
/// side x side bytes, byte r * side + c being east offset r and north offset c (MosaicProcessor's
/// pixel(): col = x = north, row = y = east). 0 is "no data", as in the shader.
struct MosaicTileRef {
    double n0 = 0.0;
    double e0 = 0.0;
    const uint8_t* px = nullptr;
};

/// The tiles of one zoom level, sampled at scene points by nearest pixel.
class MosaicSampler {
public:
    MosaicSampler(const QHash<QPair<int, int>, MosaicTileRef>& tiles, double tileSideM, double pxPerMeter, int side);
    uint8_t sample(double n, double e) const;
    /// The box of the tiles with data, in the scene's metres; false when there are none.
    bool bounds(double& minN, double& maxN, double& minE, double& maxE) const;
private:
    QHash<QPair<int, int>, MosaicTileRef> tiles_;
    double tileSideM_;
    double pxPerMeter_;
    int side_;
};

/// The depth surface on a lat/lon grid: depth in metres, positive down, NaN where there is none.
struct DepthGrid {
    GeoGrid grid;
    std::vector<float> depth;
    float minDepth = 0.0f;
    float maxDepth = 0.0f;
    int soundings = 0;   // points that went into the triangulation (one per square metre cell)
    int triangles = 0;   // triangles drawn (none longer than the edge limit)
    int extrapolated = 0;   // cells given a depth by the extrapolation (each time one is, also again)
};

/// Builds the depth surface from the 3D bottom track as SurfaceProcessor sees it (scene metres,
/// z = -distance below the sonar): one point per 1 m cell (the last one, as SurfaceProcessor
/// keeps it), a Delaunay triangulation, triangles with an edge over edgeLimitM left out, the rest
/// interpolated linearly at each pixel centre. draftM is added to every depth. progress(0..1) is
/// called now and then; it returns false to stop, and then so does this (false, error empty).
/// extraWidthM > 0: then, as SurfaceProcessor::paintDiskExtrapolated does, every sounding in the
/// track's order gives its depth to the cells within extraWidthM / 2 that no triangle covers -- ahead
/// of it (along the track) over an earlier extrapolation, behind it only where there is none.
bool buildDepthGrid(const QVector<QVector3D>& bottomTrack, const LLARef& ref, double cellM,
                    double edgeLimitM, double extraWidthM, double draftM, qint64 pixelLimit, DepthGrid& out,
                    const std::function<bool(double)>& progress, QString* error);

/// One isobath: a depth and the polyline of (latitude, longitude) points along it.
struct Isobath {
    double depth = 0.0;
    QVector<QPointF> lonLat;   // x = longitude, y = latitude
    bool closed = false;
};

/// Isobaths of the depth grid every `step` metres (marching squares over pixel centres, the
/// segments joined into polylines along the grid edges they cross).
QVector<Isobath> traceIsobaths(const DepthGrid& g, double step);

} // namespace mapexport
