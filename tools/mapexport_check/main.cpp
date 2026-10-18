// Offline check of the map export (src/map_export, docs/map-export.md).
//
// What it holds the code to:
//   - Projector gives the very doubles NED(const LLA*, const LLARef*) gives (bit for bit), so the
//     exported pixels sit where the app put the data;
//   - nedToLla undoes llaToNed to well under a millimetre;
//   - the mosaic sampler reads the byte MosaicProcessor's pixel() wrote for a scene point;
//   - the depth surface reproduces a plane exactly inside its triangles and leaves gaps wider than
//     the edge limit empty; the isobaths lie on their depth;
//   - the GeoTIFFs, read back by the TIFF parser below (not the writer's code), carry the pixels,
//     the georeference (EPSG:4326, pixel is area), the notice and the licence;
//   - the KMZ, read back by Qt's QZipReader, starts with doc.kml, has every file the KML names, the
//     notice, the author and the licence, and no track or times when they are hidden.
//
//     mapexport_check              all checks, exit code 0 when they pass
//     mapexport_check --keep DIR   also leaves the end-to-end files in DIR (to open by hand)

#include "map_export_formats.h"
#include "map_export_geo.h"
#include "map_export_meta.h"
#include "map_export_writer.h"
#include "map_package.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtCore/private/qzipreader_p.h>
#include <QtEndian>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace mapexport;

namespace {

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const QString& what)
{
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::fprintf(stderr, "FAIL: %s\n", qPrintable(what));
    }
}

struct Lcg {   // deterministic numbers, 64-bit wrap-around on purpose
    quint64 s;
    explicit Lcg(quint64 seed) : s(seed) {}
    double next()   // [0, 1)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return double(s >> 11) / double(1ULL << 53);
    }
    double in(double a, double b) { return a + (b - a) * next(); }
};

struct Ref { double lat, lon; const char* name; };
const Ref kRefs[] = {
    { 40.1792, 44.4991, "Yerevan" }, { 55.7558, 37.6173, "Moscow" }, { -33.8688, 151.2093, "Sydney" },
    { 69.6492, 18.9553, "Tromso" }, { 0.5, -78.5, "equator" }, { 42.5182, 124.0200, "the boat's lake" },
};

LLARef makeRef(double lat, double lon)
{
    return LLARef(LLA(lat, lon, 0.0));
}

// ── projection ───────────────────────────────────────────────────────────

void checkProjector()
{
    for (const Ref& rf : kRefs) {
        const LLARef ref = makeRef(rf.lat, rf.lon);
        GeoGrid g;
        g.west = rf.lon - 0.031;
        g.north = rf.lat + 0.021;
        g.dLon = 1.3e-5;
        g.dLat = 0.9e-5;
        g.width = 4000;
        g.height = 4000;
        Projector p(ref, g);
        int tested = 0, differ = 0;
        for (int r = 0; r < g.height; r += 37) {
            p.setRow(r);
            for (int c = 0; c < g.width; c += 41) {
                double n, e;
                p.toNed(c, n, e);
                const LLA lla(g.latAt(r + 0.5), g.lonAt(c + 0.5), 0.0);
                const NED app(&lla, &ref);
                ++tested;
                differ += (std::memcmp(&n, &app.n, sizeof n) != 0 || std::memcmp(&e, &app.e, sizeof e) != 0);
            }
        }
        check(differ == 0, QStringLiteral("Projector equals NED(LLA) bit for bit at %1: %2 of %3 differ").arg(rf.name).arg(differ).arg(tested));
    }
}

void checkInverse()
{
    Lcg rnd(20261018);
    for (const Ref& rf : kRefs) {
        const LLARef ref = makeRef(rf.lat, rf.lon);
        double worst = 0.0, worstApp = 0.0;
        for (int i = 0; i < 20000; ++i) {
            const double n = rnd.in(-20000.0, 20000.0), e = rnd.in(-20000.0, 20000.0);
            double lat, lon, n2, e2;
            nedToLla(ref, n, e, lat, lon);
            llaToNed(ref, lat, lon, n2, e2);
            worst = std::max(worst, std::hypot(n2 - n, e2 - e));
            // The app's own inverse, for the record: LLA(const NED*) divides by 180/pi.
            const NED ned(n, e, 0.0);
            const LLA back(&ned, &ref);
            double n3, e3;
            llaToNed(ref, back.latitude, back.longitude, n3, e3);
            worstApp = std::max(worstApp, std::hypot(n3 - n, e3 - e));
        }
        check(worst < 1e-6, QStringLiteral("nedToLla undoes llaToNed at %1: worst %2 m").arg(rf.name).arg(worst, 0, 'g', 3));
        std::printf("  %-16s round trip: nedToLla %.2e m, LLA(const NED*) %.3f m\n", rf.name, worst, worstApp);
    }
}

void checkGrid()
{
    for (const Ref& rf : kRefs) {
        const LLARef ref = makeRef(rf.lat, rf.lon);
        const double minN = -1200, maxN = 800, minE = -500, maxE = 2500, m = 0.16;
        const GeoGrid g = gridForNedBox(ref, minN, maxN, minE, maxE, m, 1000LL * 1000 * 1000);
        bool inside = g.isValid();
        for (int i = 0; i <= 20 && inside; ++i) {
            const double t = i / 20.0;
            const double pts[4][2] = { { minN + t * (maxN - minN), minE }, { minN + t * (maxN - minN), maxE },
                                       { minN, minE + t * (maxE - minE) }, { maxN, minE + t * (maxE - minE) } };
            for (const auto& p : pts) {
                double lat, lon;
                nedToLla(ref, p[0], p[1], lat, lon);
                inside = inside && lat < g.north && lat > g.south() && lon > g.west && lon < g.east();
            }
        }
        check(inside, QStringLiteral("the grid holds the whole box at %1").arg(rf.name));
        // A pixel is about `m` on each side in the middle of the grid.
        Projector p(ref, g);
        const int r = g.height / 2, c = g.width / 2;
        double n0, e0, n1, e1, n2, e2;
        p.setRow(r);
        p.toNed(c, n0, e0);
        p.toNed(c + 1, n1, e1);
        p.setRow(r + 1);
        p.toNed(c, n2, e2);
        const double dx = std::hypot(n1 - n0, e1 - e0), dy = std::hypot(n2 - n0, e2 - e0);
        check(std::fabs(dx - m) < 0.01 * m && std::fabs(dy - m) < 0.01 * m,
              QStringLiteral("pixel size at %1: %2 x %3 m for %4").arg(rf.name).arg(dx).arg(dy).arg(m));
    }
    check(!gridForNedBox(makeRef(40, 44), 0, 1e5, 0, 1e5, 0.02, 1000000).isValid(), QStringLiteral("a grid over the pixel limit is refused"));
}

// ── mosaic sampling ──────────────────────────────────────────────────────

constexpr int kSide = 256;

// The byte a tile holds at north pixel c, east pixel r of tile (kx, ky): made up, never 0.
uint8_t tileValue(int kx, int ky, int r, int c)
{
    return uint8_t(1 + ((kx * 7 + ky * 13 + r * 3 + c * 5) % 250 + 250) % 250);
}

struct Tiles {
    std::map<std::pair<int, int>, std::vector<uint8_t>> data;
    QHash<QPair<int, int>, MosaicTileRef> refs;
    double ppm = 6.25;   // zoom 5
    double side() const { return kSide / ppm; }
    void add(int kx, int ky)
    {
        auto& v = data[{ kx, ky }];
        v.resize(size_t(kSide) * kSide);
        for (int r = 0; r < kSide; ++r) {
            for (int c = 0; c < kSide; ++c) {
                // MosaicProcessor's pixel(): pix = (y % side) * side + (x % side), x = north, y = east
                v[size_t(r) * kSide + size_t(c)] = tileValue(kx, ky, r, c);
            }
        }
        refs.insert(qMakePair(kx, ky), MosaicTileRef{ kx * side(), ky * side(), v.data() });
    }
};

Tiles makeTiles()
{
    Tiles t;
    for (int kx = -2; kx <= 1; ++kx) {
        for (int ky = -1; ky <= 2; ++ky) {
            if (!(kx == 0 && ky == 0)) {   // one hole
                t.add(kx, ky);
            }
        }
    }
    return t;
}

void checkSampler()
{
    Tiles t = makeTiles();
    const MosaicSampler ms(t.refs, t.side(), t.ppm, kSide);
    Lcg rnd(7);
    int bad = 0, n = 0;
    for (int i = 0; i < 200000; ++i) {
        const qint64 gx = qint64(std::floor(rnd.in(-3 * kSide, 3 * kSide)));
        const qint64 gy = qint64(std::floor(rnd.in(-2 * kSide, 4 * kSide)));
        const double fx = rnd.in(0.01, 0.99), fy = rnd.in(0.01, 0.99);   // anywhere inside the pixel
        const int kx = int(std::floor(double(gx) / kSide)), ky = int(std::floor(double(gy) / kSide));
        const int c = int(gx - qint64(kx) * kSide), r = int(gy - qint64(ky) * kSide);
        const bool have = t.data.count({ kx, ky }) > 0;
        const uint8_t want = have ? tileValue(kx, ky, r, c) : 0;
        const uint8_t got = ms.sample((double(gx) + fx) / t.ppm, (double(gy) + fy) / t.ppm);
        bad += got != want;
        ++n;
    }
    check(bad == 0, QStringLiteral("the sampler reads the painted byte: %1 of %2 wrong").arg(bad).arg(n));
    double minN, maxN, minE, maxE;
    check(ms.bounds(minN, maxN, minE, maxE) && std::fabs(minN - (-2 * t.side())) < 1e-9 && std::fabs(maxN - 2 * t.side()) < 1e-9
          && std::fabs(minE - (-1 * t.side())) < 1e-9 && std::fabs(maxE - 3 * t.side()) < 1e-9,
          QStringLiteral("the sampler's bounds are its tiles'"));
}

// ── depths ───────────────────────────────────────────────────────────────

double plane(double n, double e) { return 5.0 + 0.01 * n + 0.02 * e; }

QVector<QVector3D> surveyTrack()
{
    QVector<QVector3D> bt;
    for (int line = 0; line <= 20; ++line) {           // e = 0 .. 200, every 10 m
        const double e = line * 10.0;
        // The line at e = 100 is first run half a metre too deep and run again later: the second
        // run's depth is the one kept (SurfaceProcessor keeps a cell's newest depth).
        const double wrong = line == 10 ? 0.5 : 0.0;
        for (int i = 0; i <= 300; ++i) {               // n = 0 .. 300, one point a metre
            const double n = (line % 2) ? 300 - i : i;
            bt.append(QVector3D(float(n), float(e), float(-plane(n, e) - wrong)));
        }
    }
    for (int i = 0; i <= 300; ++i) {
        bt.append(QVector3D(float(i), 100.0f, float(-plane(i, 100.0))));
    }
    for (int line = 0; line <= 2; ++line) {            // a second patch 400 m east: a gap past the limit
        const double e = 600.0 + line * 10.0;
        for (int i = 0; i <= 100; ++i) {
            bt.append(QVector3D(float(i), float(e), float(-plane(i, e))));
        }
    }
    bt.append(QVector3D(5.0f, 5.0f, 0.0f));            // no bottom: left out
    bt.append(QVector3D(6.0f, 5.0f, qQNaN()));
    return bt;
}

DepthGrid makeDepth(const LLARef& ref, double draft)
{
    DepthGrid dg;
    QString err;
    const bool ok = buildDepthGrid(surveyTrack(), ref, 1.0, 100.0, 0.0, draft, 16LL * 1000 * 1000, dg, {}, &err);
    check(ok, QStringLiteral("the depth surface is built: %1").arg(err));
    return dg;
}

// The extrapolation the view draws around each sounding (SurfaceProcessor::paintDiskExtrapolated):
// lines at e = 0, 10 and 30, run north, the depth 5 + n / 10, edge limit 15 m. Between the first two the triangles stay; west of the
// first line, within half the width, a cell takes the depth of the sounding the boat has just passed
// (ahead of a sounding it overwrites, behind it only fills); in the middle of the 20 m gap, nothing.
void checkExtrapolation()
{
    const LLARef ref = makeRef(40.1792, 44.4991);
    const double draft = 0.2;
    auto depthAt = [](double n) { return 5.0 + n / 10.0; };
    QVector<QVector3D> bt;
    for (double e : { 0.0, 10.0, 30.0 }) {   // 0-10 triangulated; 10-30 wider than the edge limit
        for (int n = 0; n <= 100; ++n) {
            bt.append(QVector3D(float(n), float(e), float(-depthAt(n))));
        }
    }
    for (double width : { 10.0, 0.0 }) {
        DepthGrid dg;
        QString err;
        const bool ok = buildDepthGrid(bt, ref, 1.0, 15.0, width, draft, 16LL * 1000 * 1000, dg, {}, &err);
        const QString tag = QStringLiteral("[extrapolation %1 m] ").arg(width);
        check(ok, tag + QStringLiteral("built: %1").arg(err));
        if (!ok) {
            continue;
        }
        Projector p(ref, dg.grid);
        int inside = 0, insideOff = 0, near = 0, nearFinite = 0, nearOff = 0, far = 0, farFinite = 0;
        for (int r = 0; r < dg.grid.height; ++r) {
            p.setRow(r);
            for (int c = 0; c < dg.grid.width; ++c) {
                double n, e;
                p.toNed(c, n, e);
                const float d = dg.depth[size_t(r) * size_t(dg.grid.width) + size_t(c)];
                if (n < 5.0 || n > 95.0) {
                    continue;
                }
                if (e > 0.5 && e < 9.5) {                 // the triangles
                    ++inside;
                    insideOff += !(std::isfinite(d) && std::fabs(d - (depthAt(n) + draft)) < 2e-3);
                }
                else if (e < -0.5 && e > -4.5) {          // within reach of the first line
                    ++near;
                    if (std::isfinite(d)) {
                        ++nearFinite;
                        // the sounding at or just behind: n_p in [floor(n) - 1, floor(n)]
                        const double lo = depthAt(std::floor(n) - 1.0) + draft - 1e-3, hi = depthAt(std::floor(n)) + draft + 1e-3;
                        nearOff += !(d >= lo && d <= hi);
                    }
                }
                else if (e > 15.8 && e < 24.2) {          // between the last two lines, out of reach
                    ++far;
                    farFinite += std::isfinite(d);
                }
            }
        }
        check(inside > 0 && insideOff == 0, tag + QStringLiteral("between the lines the triangles stay: %1 of %2 off").arg(insideOff).arg(inside));
        check(far > 0 && farFinite == 0, tag + QStringLiteral("past half the width nothing: %1 of %2 filled").arg(farFinite).arg(far));
        if (width > 0.0) {
            check(near > 0 && nearFinite == near && nearOff == 0 && dg.extrapolated > 0,
                  tag + QStringLiteral("within half the width the depth of the sounding just passed: %1 of %2 filled, %3 off")
                            .arg(nearFinite).arg(near).arg(nearOff));
        }
        else {
            check(near > 0 && nearFinite == 0 && dg.extrapolated == 0, tag + QStringLiteral("without it, outside the lines nothing: %1 of %2 filled").arg(nearFinite).arg(near));
        }
    }
}

void checkDepth()
{
    const LLARef ref = makeRef(40.1792, 44.4991);
    const double draft = 0.3;
    const DepthGrid dg = makeDepth(ref, draft);
    if (!dg.grid.isValid()) {
        return;
    }
    Projector p(ref, dg.grid);
    int finite = 0, bad = 0, insideHull = 0, insideHullFinite = 0, gapFinite = 0;
    double worst = 0.0;
    for (int r = 0; r < dg.grid.height; ++r) {
        p.setRow(r);
        for (int c = 0; c < dg.grid.width; ++c) {
            double n, e;
            p.toNed(c, n, e);
            const float d = dg.depth[size_t(r) * size_t(dg.grid.width) + size_t(c)];
            const bool hull = n > 0.5 && n < 299.5 && e > 0.5 && e < 199.5;
            insideHull += hull;
            if (std::isfinite(d)) {
                ++finite;
                insideHullFinite += hull;
                const double err = std::fabs(double(d) - (plane(n, e) + draft));
                worst = std::max(worst, err);
                bad += err > 2e-3;
                gapFinite += (e > 250.0 && e < 550.0);
            }
        }
    }
    check(bad == 0, QStringLiteral("depths inside the triangles are the plane's: %1 of %2 off, worst %3 m").arg(bad).arg(finite).arg(worst));
    check(insideHull > 0 && insideHullFinite == insideHull, QStringLiteral("the surveyed area is covered: %1 of %2").arg(insideHullFinite).arg(insideHull));
    check(gapFinite == 0, QStringLiteral("a gap wider than the edge limit stays empty: %1 pixels filled").arg(gapFinite));
    check(dg.soundings == 21 * 301 + 3 * 101, QStringLiteral("one sounding per metre cell, no-bottom points left out: %1").arg(dg.soundings));
    // The shallowest sounding is at (0, 0) of the first patch, the deepest at (100, 620) of the second;
    // pixel centres sit a little inside.
    const double shallow = plane(0, 0) + draft, deep = plane(100, 620) + draft;
    check(dg.minDepth >= shallow - 1e-3 && dg.minDepth < shallow + 0.05 && dg.maxDepth <= deep + 1e-3 && dg.maxDepth > deep - 0.05,
          QStringLiteral("min/max depth %1 / %2, soundings %3 / %4").arg(dg.minDepth).arg(dg.maxDepth).arg(shallow).arg(deep));

    // Isobaths lie on their depth.
    const QVector<Isobath> isos = traceIsobaths(dg, 1.0);
    int pts = 0, off = 0;
    double worstIso = 0.0;
    QSet<int> levels;
    for (const Isobath& iso : isos) {
        levels.insert(int(std::lround(iso.depth)));
        for (const QPointF& q : iso.lonLat) {
            double n, e;
            llaToNed(ref, q.y(), q.x(), n, e);
            const double err = std::fabs(plane(n, e) + draft - iso.depth);
            worstIso = std::max(worstIso, err);
            off += err > 0.01;
            ++pts;
        }
    }
    check(pts > 0 && off == 0, QStringLiteral("isobath points lie on their depth: %1 of %2 off, worst %3 m").arg(off).arg(pts).arg(worstIso));
    // Every whole metre the patches span (6..12 m the first, 18 m the second), none between them.
    bool allLevels = true;
    for (int L : { 6, 7, 8, 9, 10, 11, 12, 18 }) {
        allLevels = allLevels && levels.contains(L);
    }
    for (int L : { 14, 15, 16 }) {
        allLevels = allLevels && !levels.contains(L);
    }
    check(allLevels, QStringLiteral("an isobath at every metre the depths span and none elsewhere: %1 levels").arg(levels.size()));

    // Saddles: a checkerboard of 0 and 1 cut at 0.5 crosses every edge between pixel centres, and
    // each cell is a saddle -- every crossing must be on an isobath.
    {
        DepthGrid cb;
        cb.grid.west = 44.0;
        cb.grid.north = 40.0;
        cb.grid.dLon = cb.grid.dLat = 1e-5;
        cb.grid.width = cb.grid.height = 5;
        for (int r = 0; r < 5; ++r) {
            for (int c = 0; c < 5; ++c) {
                cb.depth.push_back(float((r + c) % 2));
            }
        }
        cb.minDepth = 0.0f;
        cb.maxDepth = 1.0f;
        // 0.5 and 1.0 are levels; at 0.5 every one of the 2 * 5 * 4 edges is crossed once.
        int at05 = 0;
        for (const Isobath& iso : traceIsobaths(cb, 0.5)) {
            at05 += std::fabs(iso.depth - 0.5) < 1e-9 ? int(iso.lonLat.size() - (iso.closed ? 1 : 0)) : 0;
        }
        check(at05 == 40, QStringLiteral("saddle cells: every crossed edge is on the isobath (%1 of 40)").arg(at05));
    }

    // Depth at a point (under the boat, under the cursor): the plane's inside, nothing over the gap
    // or off the map.
    {
        Lcg rnd(11);
        int bad = 0, n = 0;
        double worstAt = 0.0;
        for (int i = 0; i < 5000; ++i) {
            const double nn = rnd.in(2.0, 298.0), ee = rnd.in(2.0, 198.0);
            double lat, lon;
            nedToLla(ref, nn, ee, lat, lon);
            const double d = depthAt(dg, lat, lon);
            const double err = std::fabs(d - (plane(nn, ee) + draft));
            worstAt = std::max(worstAt, std::isfinite(err) ? err : 1e9);
            bad += !(err < 0.02);
            ++n;
        }
        double lat, lon;
        nedToLla(ref, 150.0, 400.0, lat, lon);
        const bool gap = std::isnan(depthAt(dg, lat, lon));
        nedToLla(ref, -500.0, -500.0, lat, lon);
        const bool off = std::isnan(depthAt(dg, lat, lon));
        check(bad == 0 && gap && off, QStringLiteral("depth at a point: %1 of %2 off (worst %3 m), gap empty %4, off the map empty %5")
              .arg(bad).arg(n).arg(worstAt).arg(gap).arg(off));
    }

    check(autoIsobathStep(0.37, 20.05) == 1.0 && autoIsobathStep(5.0, 7.0) == 0.1 && autoIsobathStep(2.0, 80.0) == 5.0
          && autoIsobathStep(3.0, 3.0) == 0.1, QStringLiteral("automatic isobath step: at most 25 levels"));

    // The colour bands, as the surface view's shader picks them.
    check(depthBands(2.0, 12.0, 1.0) == 11 && depthBand(12.0, 12.0, 1.0, 11) == 0 && depthBand(2.0, 12.0, 1.0, 11) == 10
          && depthBand(11.5, 12.0, 1.0, 11) == 0 && depthBand(10.9, 12.0, 1.0, 11) == 1, QStringLiteral("depth bands: deepest first"));
}

// ── TIFF read back ───────────────────────────────────────────────────────

struct Tiff {
    QByteArray f;
    struct Entry { quint16 type; quint32 count; quint32 at; };
    std::map<quint16, Entry> tags;
    bool ok = false;

    explicit Tiff(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return;
        }
        f = file.readAll();
        if (f.size() < 8 || f.left(4) != QByteArray("II\x2A\0", 4)) {
            return;
        }
        const quint32 ifd = u32(4);
        const quint16 n = u16(ifd);
        for (quint16 i = 0; i < n; ++i) {
            const quint32 e = ifd + 2 + 12u * i;
            const quint16 tag = u16(e), type = u16(e + 2);
            const quint32 count = u32(e + 4);
            const int size = (type == 3 ? 2 : type == 4 ? 4 : type == 12 ? 8 : 1) * int(count);
            tags[tag] = { type, count, size <= 4 ? e + 8 : u32(e + 8) };
        }
        ok = true;
    }
    quint16 u16(quint32 at) const { return qFromLittleEndian<quint16>(f.constData() + at); }
    quint32 u32(quint32 at) const { return qFromLittleEndian<quint32>(f.constData() + at); }
    std::vector<double> num(quint16 tag) const
    {
        std::vector<double> v;
        const auto it = tags.find(tag);
        if (it == tags.end()) {
            return v;
        }
        for (quint32 i = 0; i < it->second.count; ++i) {
            switch (it->second.type) {
            case 3: v.push_back(u16(it->second.at + 2 * i)); break;
            case 4: v.push_back(u32(it->second.at + 4 * i)); break;
            case 12: {
                const quint64 b = qFromLittleEndian<quint64>(f.constData() + it->second.at + 8 * i);
                double d;
                std::memcpy(&d, &b, 8);
                v.push_back(d);
                break;
            }
            default: break;
            }
        }
        return v;
    }
    QString str(quint16 tag) const
    {
        const auto it = tags.find(tag);
        if (it == tags.end() || it->second.type != 2) {
            return {};
        }
        return QString::fromUtf8(f.constData() + it->second.at, int(it->second.count) - 1);
    }
    // The tile's samples, inflated (zlib stream: qUncompress wants the size in front).
    QByteArray tile(int i, int bytes) const
    {
        const auto off = num(324), cnt = num(325);
        if (i >= int(off.size())) {
            return {};
        }
        QByteArray z(4, '\0');
        qToBigEndian<quint32>(quint32(bytes), z.data());
        z += f.mid(qsizetype(off[size_t(i)]), qsizetype(cnt[size_t(i)]));
        return qUncompress(z);
    }
};

bool hasGeoKey(const Tiff& t, int key, int value)
{
    const auto k = t.num(34735);
    for (size_t i = 4; i + 3 < k.size(); i += 4) {
        if (int(k[i]) == key) {
            return int(k[i + 3]) == value;
        }
    }
    return false;
}

void checkGeoTiff(const QString& dir)
{
    GeoGrid g;
    g.west = 44.5;
    g.north = 40.2;
    g.dLon = 2e-6;
    g.dLat = 1.5e-6;
    g.width = 600;
    g.height = 300;
    {   // 8-bit with a palette
        GeoTiffSpec spec;
        spec.kind = GeoTiffSpec::Kind::Palette8;
        spec.grid = g;
        for (int i = 0; i < 256; ++i) {
            spec.palette << qRgb(i, 255 - i, i / 2);
        }
        spec.noData = QStringLiteral("0");
        spec.description = QStringLiteral("test description");
        spec.copyright = QStringLiteral("© test");
        const QString path = dir + QStringLiteral("/p8.tif");
        QString err;
        auto val = [](int x, int y) { return uint8_t((x + 3 * y) % 256); };
        const bool ok = writeGeoTiff(path, spec, [&](int x0, int y0, int w, int h, void* dst) {
            auto* d = static_cast<uint8_t*>(dst);
            for (int r = 0; r < h; ++r) for (int c = 0; c < w; ++c) d[r * 256 + c] = val(x0 + c, y0 + r);
        }, {}, &err);
        check(ok, QStringLiteral("8-bit GeoTIFF written: %1").arg(err));
        const Tiff t(path);
        check(t.ok, QStringLiteral("8-bit GeoTIFF parses"));
        check(t.num(256) == std::vector<double>{ 600 } && t.num(257) == std::vector<double>{ 300 } && t.num(259) == std::vector<double>{ 8 }
              && t.num(262) == std::vector<double>{ 3 } && t.num(258) == std::vector<double>{ 8 } && t.num(320).size() == 768,
              QStringLiteral("8-bit GeoTIFF: size, deflate, palette"));
        check(t.num(320).size() == 768 && t.num(320)[10] == 10 * 257 && t.num(320)[256 + 10] == 245 * 257,
              QStringLiteral("8-bit GeoTIFF: the colour map is the palette"));
        check(t.num(33550) == std::vector<double>{ g.dLon, g.dLat, 0 } && t.num(33922) == std::vector<double>{ 0, 0, 0, g.west, g.north, 0 },
              QStringLiteral("GeoTIFF: pixel scale and tie point"));
        check(hasGeoKey(t, 1024, 2) && hasGeoKey(t, 1025, 1) && hasGeoKey(t, 2048, 4326) && hasGeoKey(t, 2054, 9102),
              QStringLiteral("GeoTIFF: geographic WGS 84, pixel is area, degrees"));
        check(t.str(42113) == QStringLiteral("0") && t.str(270) == QStringLiteral("test description") && t.str(33432) == QStringLiteral("© test"),
              QStringLiteral("GeoTIFF: nodata, description, copyright"));
        int bad = 0;
        for (int ty = 0; ty < 2; ++ty) {
            for (int tx = 0; tx < 3; ++tx) {
                const QByteArray s = t.tile(ty * 3 + tx, 65536);
                if (s.size() != 65536) {
                    ++bad;
                    continue;
                }
                for (int r = 0; r < 256 && ty * 256 + r < 300; ++r) {
                    for (int c = 0; c < 256 && tx * 256 + c < 600; ++c) {
                        bad += uint8_t(s[r * 256 + c]) != val(tx * 256 + c, ty * 256 + r);
                    }
                }
            }
        }
        check(bad == 0 && t.num(324).size() == 6, QStringLiteral("8-bit GeoTIFF pixels read back: %1 wrong").arg(bad));
        // The app's own reader gives the same pixels, grid and palette.
        QFile file(path);
        file.open(QIODevice::ReadOnly);
        GeoRaster gr;
        QString rerr;
        const bool rd = readGeoTiff(file.readAll(), gr, &rerr);
        int bad2 = 0;
        for (int y = 0; rd && y < 300; ++y) for (int x = 0; x < 600; ++x) bad2 += gr.u8[size_t(y) * 600 + size_t(x)] != val(x, y);
        check(rd && !gr.isFloat && gr.grid.width == 600 && gr.grid.west == g.west && gr.grid.dLat == g.dLat && bad2 == 0
              && gr.palette.size() == 256 && gr.palette[10] == qRgb(10, 245, 5) && gr.noData == QStringLiteral("0"),
              QStringLiteral("own GeoTIFF reader, 8-bit: %1, %2 wrong").arg(rerr).arg(bad2));
    }
    {   // float32
        GeoTiffSpec spec;
        spec.kind = GeoTiffSpec::Kind::Float32;
        spec.grid = g;
        spec.grid.width = 300;
        spec.grid.height = 260;
        spec.noData = QStringLiteral("-9999");
        const QString path = dir + QStringLiteral("/f32.tif");
        QString err;
        auto val = [](int x, int y) { return (x + y) % 17 == 0 ? -9999.0f : float(y) * 1000.0f + float(x) + 0.25f; };
        const bool ok = writeGeoTiff(path, spec, [&](int x0, int y0, int w, int h, void* dst) {
            auto* d = static_cast<float*>(dst);
            for (int r = 0; r < h; ++r) for (int c = 0; c < w; ++c) d[r * 256 + c] = val(x0 + c, y0 + r);
        }, {}, &err);
        check(ok, QStringLiteral("float GeoTIFF written: %1").arg(err));
        const Tiff t(path);
        check(t.ok && t.num(339) == std::vector<double>{ 3 } && t.num(258) == std::vector<double>{ 32 } && t.str(42113) == QStringLiteral("-9999"),
              QStringLiteral("float GeoTIFF: IEEE float, 32 bits, nodata"));
        int bad = 0;
        for (int ty = 0; ty < 2; ++ty) {
            for (int tx = 0; tx < 2; ++tx) {
                const QByteArray s = t.tile(ty * 2 + tx, 65536 * 4);
                if (s.size() != 65536 * 4) {
                    ++bad;
                    continue;
                }
                for (int r = 0; r < 256 && ty * 256 + r < 260; ++r) {
                    for (int c = 0; c < 256 && tx * 256 + c < 300; ++c) {
                        const quint32 u = qFromLittleEndian<quint32>(s.constData() + 4 * (r * 256 + c));
                        float v;
                        std::memcpy(&v, &u, 4);
                        bad += v != val(tx * 256 + c, ty * 256 + r);
                    }
                }
            }
        }
        check(bad == 0, QStringLiteral("float GeoTIFF pixels read back: %1 wrong").arg(bad));
    }
}

void checkZip(const QString& dir)
{
    check(crc32(QByteArray("123456789")) == 0xCBF43926u, QStringLiteral("CRC-32 check value"));
    const QString path = dir + QStringLiteral("/t.zip");
    QByteArray bin(100000, '\0');
    Lcg rnd(3);
    for (char& ch : bin) {
        ch = char(int(rnd.next() * 256));
    }
    const QByteArray text = QString(QStringLiteral("Глубина, depth ")).repeated(2000).toUtf8();
    ZipWriter z;
    QString err;
    const bool ok = z.open(path, &err) && z.add(QStringLiteral("doc.kml"), text, true, &err)
                    && z.add(QStringLiteral("files/a.bin"), bin, false, &err) && z.add(QStringLiteral("b.txt"), QByteArray("x"), true, &err)
                    && z.close(&err);
    check(ok, QStringLiteral("zip written: %1").arg(err));
    QZipReader r(path);
    const auto list = r.fileInfoList();
    check(r.status() == QZipReader::NoError && list.size() == 3 && list[0].filePath == QStringLiteral("doc.kml"),
          QStringLiteral("zip lists its entries in order"));
    check(r.fileData(QStringLiteral("doc.kml")) == text && r.fileData(QStringLiteral("files/a.bin")) == bin
          && r.fileData(QStringLiteral("b.txt")) == QByteArray("x"), QStringLiteral("zip entries read back"));
    // The app's own reader: stored entries yes, a deflated one refused with a reason.
    mapexport::ZipReader own;
    QByteArray got, kml;
    QString e1, e2;
    check(own.open(path, &e1) && own.names().size() == 3 && own.read(QStringLiteral("files/a.bin"), got, &e1) && got == bin
          && !own.read(QStringLiteral("doc.kml"), kml, &e2) && e2.contains(QStringLiteral("compressed")),
          QStringLiteral("own ZIP reader: stored read, deflated refused (%1 / %2)").arg(e1, e2));
}

// ── the whole export ─────────────────────────────────────────────────────

void checkProducts(const QString& dir)
{
    const LLARef ref = makeRef(40.1792, 44.4991);
    Tiles tiles = makeTiles();
    const MosaicSampler ms(tiles.refs, tiles.side(), tiles.ppm, kSide);
    const DepthGrid dg = makeDepth(ref, 0.0);

    for (int pass = 0; pass < 2; ++pass) {
        const bool hidden = pass == 0;
        ExportInputs in;
        in.ref = ref;
        in.mosaic = &ms;
        in.mosaicMetresPerPixel = 1.0 / tiles.ppm;
        for (int i = 0; i < 256; ++i) {
            in.mosaicPalette << qRgb(i, i, 255 - i);
        }
        in.depth = &dg;
        in.isobathStepM = 1.0;
        in.depthTheme = 1;
        if (!hidden) {
            in.trackNe = { QPointF(0, 0), QPointF(100, 0), QPointF(100, 50) };
        }
        in.meta.title = QStringLiteral("Озеро test");
        in.meta.authorName = QStringLiteral("Тест Автор");
        in.meta.authorContact = QStringLiteral("test@example.org");
        in.meta.licenseId = QStringLiteral("CC-BY-4.0");
        in.meta.software = QStringLiteral("mapexport_check");
        in.meta.hideTrackAndTime = hidden;
        in.meta.surveyFrom = hidden ? QStringLiteral("2026-10") : QStringLiteral("2026-10-07T08:00:00Z");
        in.meta.surveyTo = hidden ? QStringLiteral("2026-10") : QStringLiteral("2026-10-07T09:30:00Z");
        in.meta.disclaimerLocal = QStringLiteral("НЕ ДЛЯ НАВИГАЦИИ. Проверка.");
        in.folder = dir;
        in.baseName = hidden ? QStringLiteral("hidden") : QStringLiteral("open");
        ExportResult res;
        const bool ok = writeMapProducts(in, {}, res);
        const QString tag = hidden ? QStringLiteral("[hidden] ") : QStringLiteral("[open] ");
        check(ok && res.error.isEmpty() && res.files.size() == 4, tag + QStringLiteral("export written: %1, %2 files").arg(res.error).arg(res.files.size()));
        if (!ok) {
            continue;
        }

        // The KOGGER map reads back as it was written.
        {
            MapPackage pkg;
            QString err;
            const QString kmap = dir + QLatin1Char('/') + in.baseName + QStringLiteral(".kmap");
            const bool read = readMapPackage(kmap, pkg, &err);
            check(read, tag + QStringLiteral(".kmap reads: %1").arg(err));
            check(pkg.title() == in.meta.title && pkg.author() == QStringLiteral("Тест Автор") && pkg.licenseId() == QStringLiteral("CC-BY-4.0")
                  && pkg.disclaimer().startsWith(QStringLiteral("NOT FOR NAVIGATION")) && !pkg.preview.isNull(),
                  tag + QStringLiteral(".kmap: title, author, licence, notice, preview"));
            int depthBad = 0;
            const bool sameGrid = pkg.hasDepth && pkg.depth.grid.width == dg.grid.width && pkg.depth.grid.height == dg.grid.height
                                  && pkg.depth.grid.west == dg.grid.west && pkg.depth.grid.north == dg.grid.north
                                  && pkg.depth.grid.dLon == dg.grid.dLon && pkg.depth.grid.dLat == dg.grid.dLat;
            if (sameGrid) {
                for (size_t i = 0; i < dg.depth.size(); ++i) {
                    const float a = dg.depth[i], b = pkg.depth.depth[i];
                    depthBad += std::isfinite(a) ? !(a == b) : std::isfinite(b);
                }
            }
            check(sameGrid && depthBad == 0, tag + QStringLiteral(".kmap depths equal the ones written: %1 differ").arg(depthBad));
            int mosaicBad = 0, mosaicN = 0;
            if (pkg.hasMosaic) {
                Projector p(ref, pkg.mosaicGrid);
                for (int r = 0; r < pkg.mosaicGrid.height; r += 3) {
                    p.setRow(r);
                    for (int c = 0; c < pkg.mosaicGrid.width; c += 3) {
                        double nn, ee;
                        p.toNed(c, nn, ee);
                        mosaicBad += pkg.mosaic[size_t(r) * size_t(pkg.mosaicGrid.width) + size_t(c)] != ms.sample(nn, ee);
                        ++mosaicN;
                    }
                }
            }
            check(pkg.hasMosaic && mosaicN > 0 && mosaicBad == 0 && pkg.mosaicPalette.size() == 256 && pkg.mosaicPalette[77] == qRgb(77, 77, 255 - 77),
                  tag + QStringLiteral(".kmap mosaic and palette as written: %1 of %2 differ").arg(mosaicBad).arg(mosaicN));

            // Opening it (MapLibrary::importFile): the copy is the file, byte for byte, with its SHA-1.
            QFile f(kmap);
            const QByteArray bytes = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
            const QString copy = dir + QLatin1Char('/') + in.baseName + QStringLiteral("_copy.part");
            QByteArray sha;
            const MapCopy r = copyMapFile(kmap, copy, &sha, &err);
            QFile c(copy);
            const bool same = c.open(QIODevice::ReadOnly) && c.readAll() == bytes;
            c.close();
            check(r == MapCopy::Copied && same && !bytes.isEmpty() && sha == QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex(),
                  tag + QStringLiteral(".kmap copied for My maps byte for byte, with its SHA-1"));
            QFile::remove(copy);

            // A shared map holds the depths for the isobaths and the mosaic, nothing of the boat's track
            // (owner, 2026-10-18), whatever the privacy switch says: the same five parts in either mode,
            // not a word of a track, the survey's date to the month in every part.
            QZipReader kz(kmap);
            QStringList parts;
            for (const auto& fi : kz.fileInfoList()) {
                parts << fi.filePath;
            }
            parts.sort();
            check(parts == QStringList{ QStringLiteral("README.txt"), QStringLiteral("depth.tif"), QStringLiteral("map.json"),
                                        QStringLiteral("mosaic.tif"), QStringLiteral("preview.png") },
                  tag + QStringLiteral(".kmap holds only map.json, README.txt, preview.png, depth.tif, mosaic.tif: %1").arg(parts.join(QLatin1Char(' '))));
            const QByteArray mj = kz.fileData(QStringLiteral("map.json"));
            check(!mj.isEmpty() && !mj.toLower().contains("track"), tag + QStringLiteral(".kmap map.json: not a word of a track"));
            int fullTimes = 0;
            for (const QString& part : std::as_const(parts)) {
                fullTimes += kz.fileData(part).contains("2026-10-07");
            }
            check(fullTimes == 0 && QJsonDocument::fromJson(mj).object().value(QStringLiteral("survey")).toObject()
                                        .value(QStringLiteral("timeFrom")).toString() == QStringLiteral("2026-10"),
                  tag + QStringLiteral(".kmap: the survey's date to the month only, %1 parts with the day").arg(fullTimes));
        }

        const QString kmz = dir + QLatin1Char('/') + in.baseName + QStringLiteral(".kmz");
        QZipReader z(kmz);
        const auto list = z.fileInfoList();
        check(!list.isEmpty() && list[0].filePath == QStringLiteral("doc.kml"), tag + QStringLiteral("doc.kml comes first"));
        const QString kml = QString::fromUtf8(z.fileData(QStringLiteral("doc.kml")));
        QSet<QString> names;
        for (const auto& fi : list) {
            names.insert(fi.filePath);
        }
        const QRegularExpression href(QStringLiteral("<href>([^<]+)</href>"));
        int hrefs = 0, missing = 0, broken = 0;
        for (auto it = href.globalMatch(kml); it.hasNext();) {
            const QString h = it.next().captured(1);
            ++hrefs;
            if (!names.contains(h)) {
                ++missing;
                continue;
            }
            broken += QImage::fromData(z.fileData(h), "PNG").isNull();
        }
        check(hrefs > 3 && missing == 0 && broken == 0, tag + QStringLiteral("every picture the KML names is there and loads: %1 named, %2 missing, %3 broken").arg(hrefs).arg(missing).arg(broken));
        check(kml.contains(QStringLiteral("NOT FOR NAVIGATION")) && kml.contains(QStringLiteral("НЕ ДЛЯ НАВИГАЦИИ"))
              && kml.contains(QStringLiteral("Тест Автор")) && kml.contains(QStringLiteral("CC BY 4.0"))
              && kml.contains(QStringLiteral("https://creativecommons.org/licenses/by/4.0/")),
              tag + QStringLiteral("the KML carries the notice, the author and the licence"));
        check(kml.contains(QStringLiteral("<name>Isobaths, m</name>")) && kml.contains(QStringLiteral("<name>Side-scan mosaic</name>")),
              tag + QStringLiteral("the KML has the mosaic and the isobaths"));
        check(hidden ? !kml.contains(QStringLiteral("<name>Track</name>")) : kml.contains(QStringLiteral("<name>Track</name>")),
              tag + QStringLiteral("the track is there only when it is not hidden"));
        check(hidden ? !kml.contains(QStringLiteral("T08:00")) && !kml.contains(QStringLiteral("2026-10-07")) : true,
              tag + QStringLiteral("no times in the KML when they are hidden"));
        const QJsonObject meta = QJsonDocument::fromJson(z.fileData(QStringLiteral("metadata.json"))).object();
        check(meta.value(QStringLiteral("license")).toObject().value(QStringLiteral("id")).toString() == QStringLiteral("CC-BY-4.0")
              && meta.value(QStringLiteral("coordinateSystem")).toString() == QStringLiteral("EPSG:4326")
              && meta.value(QStringLiteral("verticalReference")).toString() == QStringLiteral("Transducer")
              && meta.value(QStringLiteral("survey")).toObject().value(QStringLiteral("trackIncluded")).toBool() == !hidden,
              tag + QStringLiteral("metadata.json: licence, CRS, vertical reference, track"));
        const QByteArray metaRaw = z.fileData(QStringLiteral("metadata.json"));
        check(hidden ? !metaRaw.contains("2026-10-07") : metaRaw.contains("2026-10-07T08:00:00Z"),
              tag + QStringLiteral("metadata.json: the survey's time as allowed"));
        const QString readme = QString::fromUtf8(z.fileData(QStringLiteral("README.txt")));
        check(readme.contains(QStringLiteral("NOT FOR NAVIGATION")) && readme.contains(QStringLiteral("creativecommons.org")),
              tag + QStringLiteral("README.txt: notice and licence"));

        const Tiff mt(dir + QLatin1Char('/') + in.baseName + QStringLiteral("_mosaic.tif"));
        const Tiff dt(dir + QLatin1Char('/') + in.baseName + QStringLiteral("_depth.tif"));
        check(mt.ok && dt.ok, tag + QStringLiteral("both GeoTIFFs parse"));
        for (const Tiff* t : { &mt, &dt }) {
            check(t->str(270).contains(QStringLiteral("NOT FOR NAVIGATION")) && t->str(33432).contains(QStringLiteral("Тест Автор"))
                  && t->str(42112).contains(QStringLiteral("CC-BY-4.0")) && t->str(42112).contains(QStringLiteral("DISCLAIMER")),
                  tag + QStringLiteral("a GeoTIFF carries the notice, the author and the licence"));
        }
        // The mosaic GeoTIFF's pixels are the sampler's at their centres.
        if (mt.ok) {
            GeoGrid g;
            g.width = int(mt.num(256)[0]);
            g.height = int(mt.num(257)[0]);
            g.dLon = mt.num(33550)[0];
            g.dLat = mt.num(33550)[1];
            g.west = mt.num(33922)[3];
            g.north = mt.num(33922)[4];
            Projector p(ref, g);
            const int tilesX = (g.width + 255) / 256;
            int bad = 0, nonzero = 0, n = 0;
            for (int ti = 0; ti < int(mt.num(324).size()); ti += 3) {
                const QByteArray s = mt.tile(ti, 65536);
                const int tx = ti % tilesX, ty = ti / tilesX;
                for (int r = 0; r < 256 && ty * 256 + r < g.height; r += 7) {
                    p.setRow(ty * 256 + r);
                    for (int c = 0; c < 256 && tx * 256 + c < g.width; c += 5) {
                        double nn, ee;
                        p.toNed(tx * 256 + c, nn, ee);
                        const uint8_t want = ms.sample(nn, ee);
                        bad += uint8_t(s[r * 256 + c]) != want;
                        nonzero += want != 0;
                        ++n;
                    }
                }
            }
            check(n > 0 && nonzero > n / 3 && bad == 0, tag + QStringLiteral("mosaic GeoTIFF pixels are the sampled mosaic: %1 of %2 wrong (%3 with data)").arg(bad).arg(n).arg(nonzero));
            // The hole (tile 0,0) stays empty.
            double lat, lon;
            nedToLla(ref, 0.5 * tiles.side(), 0.5 * tiles.side(), lat, lon);
            const int col = int((lon - g.west) / g.dLon), row = int((g.north - lat) / g.dLat);
            const QByteArray s = mt.tile((row / 256) * tilesX + col / 256, 65536);
            check(s.size() == 65536 && s[(row % 256) * 256 + col % 256] == 0, tag + QStringLiteral("no data stays 0 in the mosaic GeoTIFF"));
        }
    }
}

// A file another app hands over (docs/map-export.md, Android) is told by its content: the copy goes in
// pieces and stops before writing anything when the file is not a ZIP archive.
void checkMapCopy(const QString& dir)
{
    auto write = [&](const QString& name, const QByteArray& bytes) {
        QFile f(dir + QLatin1Char('/') + name);
        return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() ? f.fileName() : QString();
    };
    QByteArray big("PK\x03\x04", 4);   // a ZIP's start, then 2.5 MB: three pieces of 1 MB
    big.reserve(2621444);
    quint32 x = 12345;
    while (big.size() < 2621444) {
        x = x * 1664525u + 1013904223u;
        big.append(char(x >> 24));
    }
    const QString bigPath = write(QStringLiteral("big.bin"), big);
    const QString dst = dir + QLatin1Char('/') + QStringLiteral("copy.part");
    QByteArray sha;
    QString err;
    const MapCopy r = copyMapFile(bigPath, dst, &sha, &err);
    QFile c(dst);
    const bool same = c.open(QIODevice::ReadOnly) && c.readAll() == big;
    c.close();
    check(!bigPath.isEmpty() && r == MapCopy::Copied && same && sha == QCryptographicHash::hash(big, QCryptographicHash::Sha1).toHex(),
          QStringLiteral("a 2.5 MB file is copied whole in pieces, with its SHA-1"));
    QFile::remove(dst);

    const QString klf = write(QStringLiteral("survey.kmap"), QByteArray::fromHex("bb550102") + "a log, not a map");
    check(copyMapFile(klf, dst, &sha, &err) == MapCopy::NotAMap && !QFile::exists(dst),
          QStringLiteral("a file that is not a ZIP is not a map, whatever its name, and nothing is written"));
    check(startsLikeMap(bigPath) && !startsLikeMap(klf) && !startsLikeMap(dir + QStringLiteral("/no_such.kmap")),
          QStringLiteral("startsLikeMap: a ZIP's start yes, a log named .kmap no, a missing file no"));
    const QString tiny = write(QStringLiteral("tiny.kmap"), QByteArray("PK", 2));
    check(copyMapFile(tiny, dst, &sha, &err) == MapCopy::NotAMap && !QFile::exists(dst), QStringLiteral("a file shorter than a ZIP's start is not a map"));
    err.clear();
    check(copyMapFile(dir + QStringLiteral("/no_such.kmap"), dst, &sha, &err) == MapCopy::CannotRead && !err.isEmpty() && !QFile::exists(dst),
          QStringLiteral("a file that cannot be read says why"));
    check(copyMapFile(bigPath, dir + QStringLiteral("/no_such_folder/copy.part"), &sha, &err) == MapCopy::CannotWrite,
          QStringLiteral("a copy that cannot be written says so"));
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");   // the legend is drawn with a font
    QGuiApplication app(argc, argv);

    QString keep;
    for (int i = 1; i + 1 < argc; ++i) {
        if (QByteArray(argv[i]) == "--keep") {
            keep = QString::fromLocal8Bit(argv[i + 1]);
        }
    }
    QTemporaryDir tmp;
    const QString dir = keep.isEmpty() ? tmp.path() : keep;
    QDir().mkpath(dir);

    std::printf("projection\n");
    checkProjector();
    checkInverse();
    checkGrid();
    std::printf("mosaic sampling\n");
    checkSampler();
    std::printf("depths and isobaths\n");
    checkDepth();
    checkExtrapolation();
    std::printf("files\n");
    checkGeoTiff(dir);
    checkZip(dir);
    checkProducts(dir);
    checkMapCopy(dir);

    std::printf("%d checks, %d failed%s\n", g_checks, g_fails, keep.isEmpty() ? "" : qPrintable(QStringLiteral(" (files in %1)").arg(dir)));
    return g_fails == 0 ? 0 : 1;
}
