#include "map_export_geo.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include "delaunay.h"
#include "math_defs.h"

namespace mapexport {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Metres of the scene's north per degree of latitude at the reference (llaToNed near its meridian).
double metresPerDegLat()
{
    return double(CONSTANTS_RADIUS_OF_EARTH) * double(M_DEG_TO_RAD);
}

} // namespace

Projector::Projector(const LLARef& ref, const GeoGrid& grid) :
    ref_(ref),
    grid_(grid)
{
    cosDLon_.resize(size_t(std::max(0, grid.width)));
    sinDLon_.resize(size_t(std::max(0, grid.width)));
    for (int c = 0; c < grid.width; ++c) {
        const double lon_rad = grid.lonAt(c + 0.5) * M_DEG_TO_RAD;
        cosDLon_[size_t(c)] = cos(lon_rad - ref_.refLonRad);
        sinDLon_[size_t(c)] = sin(lon_rad - ref_.refLonRad);
    }
}

void Projector::setRow(int row)
{
    const double lat_rad = grid_.latAt(row + 0.5) * M_DEG_TO_RAD;
    sinLat_ = sin(lat_rad);
    cosLat_ = cos(lat_rad);
}

void Projector::toNed(int col, double& n, double& e) const
{
    // NED::NED(const LLA*, const LLARef*, true), term by term.
    const double cos_d_lon = cosDLon_[size_t(col)];
    double arg = ref_.refLatSin * sinLat_ + ref_.refLatCos * cosLat_ * cos_d_lon;
    if (arg > 1.0) {
        arg = 1.0;
    }
    else if (arg < -1.0) {
        arg = -1.0;
    }
    const double c = acos(arg);
    const double k = (fabs(c) < kmath::dblEps) ? 1.0 : (c / sin(c));
    n = k * (ref_.refLatCos * sinLat_ - ref_.refLatSin * cosLat_ * cos_d_lon) * CONSTANTS_RADIUS_OF_EARTH;
    e = k * cosLat_ * sinDLon_[size_t(col)] * CONSTANTS_RADIUS_OF_EARTH;
}

void llaToNed(const LLARef& ref, double latDeg, double lonDeg, double& n, double& e)
{
    const LLA lla(latDeg, lonDeg, 0.0);
    const NED ned(&lla, &ref);
    n = ned.n;
    e = ned.e;
}

void nedToLla(const LLARef& ref, double n, double e, double& latDeg, double& lonDeg)
{
    const double R = CONSTANTS_RADIUS_OF_EARTH;
    const double c = sqrt(n * n + e * e) / R;
    if (c == 0.0) {
        latDeg = ref.refLla.latitude;
        lonDeg = ref.refLla.longitude;
        return;
    }
    const double lat0 = ref.refLatRad;
    const double sin_c = sin(c);
    const double cos_c = cos(c);
    const double alpha = atan2(e, n);
    const double sin_lat = sin(lat0) * cos_c + cos(lat0) * sin_c * cos(alpha);
    const double lat_rad = asin(sin_lat);
    const double y = sin(alpha) * sin_c * cos(lat0);
    const double x = cos_c - sin(lat0) * sin_lat;
    const double lon_rad = ref.refLonRad + atan2(y, x);
    latDeg = lat_rad / M_DEG_TO_RAD;
    lonDeg = lon_rad / M_DEG_TO_RAD;
}

GeoGrid gridForNedBox(const LLARef& ref, double minN, double maxN, double minE, double maxE,
                      double metres, qint64 pixelLimit)
{
    GeoGrid g;
    if (!ref.isInit || !(metres > 0.0) || !(maxN >= minN) || !(maxE >= minE)) {
        return g;
    }
    // The box's edges are curves in lat/lon (the projection is azimuthal), so the edges are walked.
    double minLat = 90.0, maxLat = -90.0, minLon = 360.0, maxLon = -360.0;
    constexpr int kSteps = 64;
    auto take = [&](double n, double e) {
        double lat = 0.0, lon = 0.0;
        nedToLla(ref, n, e, lat, lon);
        minLat = std::min(minLat, lat);
        maxLat = std::max(maxLat, lat);
        minLon = std::min(minLon, lon);
        maxLon = std::max(maxLon, lon);
    };
    for (int i = 0; i <= kSteps; ++i) {
        const double t = double(i) / kSteps;
        take(minN + t * (maxN - minN), minE);
        take(minN + t * (maxN - minN), maxE);
        take(minN, minE + t * (maxE - minE));
        take(maxN, minE + t * (maxE - minE));
    }
    if (maxLon - minLon > 180.0) {   // across the antimeridian: not handled
        return g;
    }
    const double midLat = 0.5 * (minLat + maxLat);
    g.dLat = metres / metresPerDegLat();
    g.dLon = metres / (metresPerDegLat() * cos(midLat * M_DEG_TO_RAD));
    const double w = std::ceil((maxLon - minLon) / g.dLon) + 2.0;
    const double h = std::ceil((maxLat - minLat) / g.dLat) + 2.0;
    if (w * h > double(pixelLimit) || w > double(std::numeric_limits<int>::max()) || h > double(std::numeric_limits<int>::max())) {
        return GeoGrid();
    }
    g.west = minLon - g.dLon;
    g.north = maxLat + g.dLat;
    g.width = int(w);
    g.height = int(h);
    return g;
}

MosaicSampler::MosaicSampler(const QHash<QPair<int, int>, MosaicTileRef>& tiles, double tileSideM, double pxPerMeter, int side) :
    tiles_(tiles),
    tileSideM_(tileSideM),
    pxPerMeter_(pxPerMeter),
    side_(side)
{
}

uint8_t MosaicSampler::sample(double n, double e) const
{
    // The scene's pixel grid is anchored at (0, 0) (worldOriginFromKey): pixel gx covers north
    // [gx, gx + 1) / pxPerMeter, as the renderer draws a texel (u = north, v = east); its tile is
    // gx / side rounded down and its column in the tile the rest.
    const qint64 gx = qint64(std::floor(n * pxPerMeter_));
    const qint64 gy = qint64(std::floor(e * pxPerMeter_));
    const qint64 kx = gx >= 0 ? gx / side_ : -((-gx + side_ - 1) / side_);
    const qint64 ky = gy >= 0 ? gy / side_ : -((-gy + side_ - 1) / side_);
    const auto it = tiles_.constFind(qMakePair(int(kx), int(ky)));
    if (it == tiles_.cend() || !it->px) {
        return 0;
    }
    const int c = int(gx - kx * side_);
    const int r = int(gy - ky * side_);
    return it->px[size_t(r) * size_t(side_) + size_t(c)];
}

bool MosaicSampler::bounds(double& minN, double& maxN, double& minE, double& maxE) const
{
    bool any = false;
    for (auto it = tiles_.cbegin(); it != tiles_.cend(); ++it) {
        if (!it->px) {
            continue;
        }
        const double n0 = it->n0, e0 = it->e0, s = tileSideM_;
        if (!any) {
            minN = n0; maxN = n0 + s; minE = e0; maxE = e0 + s;
            any = true;
        }
        else {
            minN = std::min(minN, n0); maxN = std::max(maxN, n0 + s);
            minE = std::min(minE, e0); maxE = std::max(maxE, e0 + s);
        }
    }
    return any;
}

bool buildDepthGrid(const QVector<QVector3D>& bottomTrack, const LLARef& ref, double cellM,
                    double edgeLimitM, double draftM, qint64 pixelLimit, DepthGrid& out,
                    const std::function<bool(double)>& progress, QString* error)
{
    out = DepthGrid();
    auto fail = [&](const QString& why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    auto keepGoing = [&](double f) { return !progress || progress(f); };

    // One point per square metre cell: the first point's place, the last one's depth
    // (SurfaceProcessor::onUpdatedBottomTrackData, processOneCenter, cellPx_ = 1).
    struct Pt { double n, e, z; };
    std::vector<Pt> pts;
    QHash<QPair<int, int>, int> cellOf;
    bool haveOrigin = false;
    double on = 0.0, oe = 0.0;
    for (const QVector3D& p : bottomTrack) {
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || !std::isfinite(p.z()) || !(p.z() < 0.0f)) {
            continue;   // z = -(distance to the bottom): none, or zero, is no bottom
        }
        if (!haveOrigin) {
            on = p.x();
            oe = p.y();
            haveOrigin = true;
        }
        const QPair<int, int> cid(qRound((p.x() - on) / 1.0), qRound((p.y() - oe) / 1.0));
        const auto it = cellOf.constFind(cid);
        if (it != cellOf.cend()) {
            pts[size_t(it.value())].z = p.z();
            continue;
        }
        cellOf.insert(cid, int(pts.size()));
        pts.push_back({ p.x(), p.y(), p.z() });
    }
    if (pts.size() < 3) {
        return fail(QStringLiteral("no depths: the bottom track has fewer than three points"));
    }

    double minN = pts[0].n, maxN = minN, minE = pts[0].e, maxE = minE;
    for (const Pt& p : pts) {
        minN = std::min(minN, p.n); maxN = std::max(maxN, p.n);
        minE = std::min(minE, p.e); maxE = std::max(maxE, p.e);
    }
    // Triangulated about the middle of the points: the super-rectangle is ±super_size around 0.
    const double cn = 0.5 * (minN + maxN), ce = 0.5 * (minE + maxE);
    const double half = std::max(maxN - minN, maxE - minE);
    delaunay::Delaunay del(std::max(delaunay::SUPER_SIZE, 8.0 * half));
    for (size_t i = 0; i < pts.size(); ++i) {
        del.addPoint({ pts[i].n - cn, pts[i].e - ce, pts[i].z });
        if ((i & 1023) == 0 && !keepGoing(0.7 * double(i) / double(pts.size()))) {
            return false;
        }
    }

    // The grid; coarser cells while it would be over the limit.
    GeoGrid grid;
    double cell = cellM;
    for (int tries = 0; tries < 16 && !grid.isValid(); ++tries) {
        grid = gridForNedBox(ref, minN, maxN, minE, maxE, cell, pixelLimit);
        if (!grid.isValid()) {
            cell *= 1.5;
        }
    }
    if (!grid.isValid()) {
        return fail(QStringLiteral("the depth grid does not fit (area too large or across the antimeridian)"));
    }
    out.grid = grid;
    out.depth.assign(size_t(grid.width) * size_t(grid.height), std::numeric_limits<float>::quiet_NaN());
    out.soundings = int(pts.size());

    const auto& tri = del.getTriangles();
    const auto& dp = del.getPoints();
    Projector proj(ref, grid);
    float minD = std::numeric_limits<float>::max();
    float maxD = std::numeric_limits<float>::lowest();
    for (size_t ti = 0; ti < tri.size(); ++ti) {
        if ((ti & 4095) == 0 && !keepGoing(0.7 + 0.3 * double(ti) / double(tri.size()))) {
            return false;
        }
        const auto& t = tri[ti];
        if (t.is_bad || t.a < 4 || t.b < 4 || t.c < 4 || t.longest_edge_dist > edgeLimitM) {
            continue;
        }
        const double ax = dp[t.a].x + cn, ay = dp[t.a].y + ce, az = dp[t.a].z;
        const double bx = dp[t.b].x + cn, by = dp[t.b].y + ce, bz = dp[t.b].z;
        const double cx = dp[t.c].x + cn, cy = dp[t.c].y + ce, cz = dp[t.c].z;
        const double det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
        if (std::fabs(det) < 1e-9) {
            continue;
        }
        double latA, lonA, latB, lonB, latC, lonC;
        nedToLla(ref, ax, ay, latA, lonA);
        nedToLla(ref, bx, by, latB, lonB);
        nedToLla(ref, cx, cy, latC, lonC);
        const double lonMin = std::min({ lonA, lonB, lonC }), lonMax = std::max({ lonA, lonB, lonC });
        const double latMin = std::min({ latA, latB, latC }), latMax = std::max({ latA, latB, latC });
        const int c0 = std::max(0, int(std::floor((lonMin - grid.west) / grid.dLon - 0.5)) - 1);
        const int c1 = std::min(grid.width - 1, int(std::ceil((lonMax - grid.west) / grid.dLon - 0.5)) + 1);
        const int r0 = std::max(0, int(std::floor((grid.north - latMax) / grid.dLat - 0.5)) - 1);
        const int r1 = std::min(grid.height - 1, int(std::ceil((grid.north - latMin) / grid.dLat - 0.5)) + 1);
        bool drawn = false;
        for (int r = r0; r <= r1; ++r) {
            proj.setRow(r);
            for (int c = c0; c <= c1; ++c) {
                double n, e;
                proj.toNed(c, n, e);
                const double l1 = ((by - cy) * (n - cx) + (cx - bx) * (e - cy)) / det;
                const double l2 = ((cy - ay) * (n - cx) + (ax - cx) * (e - cy)) / det;
                const double l3 = 1.0 - l1 - l2;
                constexpr double kEps = -1e-9;
                if (l1 < kEps || l2 < kEps || l3 < kEps) {
                    continue;
                }
                const float d = float(-(l1 * az + l2 * bz + l3 * cz) + draftM);
                out.depth[size_t(r) * size_t(grid.width) + size_t(c)] = d;
                minD = std::min(minD, d);
                maxD = std::max(maxD, d);
                drawn = true;
            }
        }
        out.triangles += drawn ? 1 : 0;
    }
    if (out.triangles == 0) {
        return fail(QStringLiteral("no depths: no triangle within the edge limit (%1 m)").arg(edgeLimitM));
    }
    out.minDepth = minD;
    out.maxDepth = maxD;
    keepGoing(1.0);
    return true;
}

namespace {

// A crossing of a grid edge between two pixel centres: kind 0 joins (r, c)-(r, c+1), kind 1 joins
// (r, c)-(r+1, c). Two cells share an edge, so a crossing has at most two segments.
inline qint64 edgeKey(int kind, int r, int c)
{
    return (qint64(r) << 33) | (qint64(c) << 1) | qint64(kind);
}

} // namespace

QVector<Isobath> traceIsobaths(const DepthGrid& g, double step)
{
    QVector<Isobath> out;
    const int W = g.grid.width, H = g.grid.height;
    if (!(step > 0.0) || W < 2 || H < 2 || g.depth.size() != size_t(W) * size_t(H)) {
        return out;
    }
    const double first = std::ceil(double(g.minDepth) / step) * step;
    const int levels = int(std::floor((double(g.maxDepth) - first) / step)) + 1;
    if (levels <= 0 || levels > 10000) {
        return out;
    }
    struct Level {
        QHash<qint64, QPointF> at;                // crossing -> (col, row) in pixel-centre units
        QHash<qint64, QVector<qint64>> next;      // crossing -> the crossings it is joined to
    };
    std::vector<Level> lv(static_cast<size_t>(levels));
    auto v = [&](int r, int c) { return double(g.depth[size_t(r) * size_t(W) + size_t(c)]); };

    for (int r = 0; r + 1 < H; ++r) {
        for (int c = 0; c + 1 < W; ++c) {
            const double tl = v(r, c), tr = v(r, c + 1), br = v(r + 1, c + 1), bl = v(r + 1, c);
            if (!std::isfinite(tl) || !std::isfinite(tr) || !std::isfinite(br) || !std::isfinite(bl)) {
                continue;
            }
            const double lo = std::min({ tl, tr, br, bl }), hi = std::max({ tl, tr, br, bl });
            const int iFrom = std::max(0, int(std::ceil((lo - first) / step)));
            const int iTo = std::min(levels - 1, int(std::floor((hi - first) / step)));
            for (int i = iFrom; i <= iTo; ++i) {
                const double L = first + i * step;
                const int code = (tl >= L ? 8 : 0) | (tr >= L ? 4 : 0) | (br >= L ? 2 : 0) | (bl >= L ? 1 : 0);
                if (code == 0 || code == 15) {
                    continue;
                }
                Level& lev = lv[size_t(i)];
                // The four edges: top (tl-tr), right (tr-br), bottom (bl-br), left (tl-bl).
                const qint64 kTop = edgeKey(0, r, c), kBottom = edgeKey(0, r + 1, c);
                const qint64 kLeft = edgeKey(1, r, c), kRight = edgeKey(1, r, c + 1);
                auto cross = [&](qint64 k) {
                    if (lev.at.contains(k)) {
                        return;
                    }
                    double x = 0.0, y = 0.0, a = 0.0, b = 0.0;
                    if (k == kTop)         { a = tl; b = tr; }
                    else if (k == kBottom) { a = bl; b = br; }
                    else if (k == kLeft)   { a = tl; b = bl; }
                    else                   { a = tr; b = br; }
                    const double t = (b != a) ? std::clamp((L - a) / (b - a), 0.0, 1.0) : 0.5;
                    if (k == kTop)         { x = c + t;     y = r; }
                    else if (k == kBottom) { x = c + t;     y = r + 1; }
                    else if (k == kLeft)   { x = c;         y = r + t; }
                    else                   { x = c + 1;     y = r + t; }
                    lev.at.insert(k, QPointF(x + 0.5, y + 0.5));
                };
                auto seg = [&](qint64 a, qint64 b) {
                    cross(a);
                    cross(b);
                    lev.next[a].append(b);
                    lev.next[b].append(a);
                };
                const bool centreIn = 0.25 * (tl + tr + br + bl) >= L;
                switch (code) {
                case 1: case 14: seg(kLeft, kBottom); break;
                case 2: case 13: seg(kBottom, kRight); break;
                case 3: case 12: seg(kLeft, kRight); break;
                case 4: case 11: seg(kTop, kRight); break;
                case 6: case 9:  seg(kTop, kBottom); break;
                case 7: case 8:  seg(kLeft, kTop); break;
                case 5:   // tr and bl deep
                    if (centreIn) { seg(kLeft, kTop); seg(kBottom, kRight); }
                    else          { seg(kTop, kRight); seg(kLeft, kBottom); }
                    break;
                case 10:  // tl and br deep
                    if (centreIn) { seg(kTop, kRight); seg(kLeft, kBottom); }
                    else          { seg(kLeft, kTop); seg(kBottom, kRight); }
                    break;
                default: break;
                }
            }
        }
    }

    auto toLonLat = [&](const QPointF& p) {
        return QPointF(g.grid.lonAt(p.x()), g.grid.latAt(p.y()));
    };
    for (int i = 0; i < levels; ++i) {
        Level& lev = lv[size_t(i)];
        QSet<qint64> used;
        auto walk = [&](qint64 start) {
            Isobath iso;
            iso.depth = first + i * step;
            qint64 k = start;
            qint64 prev = -1;
            for (;;) {
                used.insert(k);
                iso.lonLat.append(toLonLat(lev.at.value(k)));
                qint64 nxt = -1;
                for (qint64 cand : lev.next.value(k)) {
                    if (cand != prev && !used.contains(cand)) {
                        nxt = cand;
                        break;
                    }
                }
                if (nxt < 0) {
                    // A loop closes on its start.
                    if (lev.next.value(k).contains(start) && k != start && iso.lonLat.size() > 2) {
                        iso.lonLat.append(iso.lonLat.first());
                        iso.closed = true;
                    }
                    break;
                }
                prev = k;
                k = nxt;
            }
            if (iso.lonLat.size() >= 2) {
                out.append(iso);
            }
        };
        // Open lines from their ends first, then the loops.
        for (auto it = lev.next.cbegin(); it != lev.next.cend(); ++it) {
            if (!used.contains(it.key()) && it.value().size() == 1) {
                walk(it.key());
            }
        }
        for (auto it = lev.next.cbegin(); it != lev.next.cend(); ++it) {
            if (!used.contains(it.key())) {
                walk(it.key());
            }
        }
    }
    return out;
}

} // namespace mapexport
