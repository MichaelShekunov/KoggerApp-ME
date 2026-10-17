// MosaicIndexProvider::tilesInQuadNed against the per-tile QPainterPath test it replaced (2026-10-17):
// the tiles a quad covers, at every zoom, for axis-aligned boxes (what Dataset::calcDimensionRects
// passes), convex quads (the view, tilted or not) and crossed ones (a sharp turn). The new cover must
// hold every tile the old one had, be the same for boxes and convex quads, and may add tiles only for
// crossed quads. Prints the counts and both times; exits 1 on a failure.
#include "mosaic_index_provider.h"

#include <QPainterPath>
#include <QPolygonF>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

namespace {

QSet<TileKey> oldCover(const std::array<QPointF, 4>& quad, int z, int padTiles, uint64_t maxTiles)
{
    const double S = double(tileSideMetersFromZoom(z, 256));
    QPolygonF poly;
    for (const auto& p : quad) {
        poly << p;
    }
    const QRectF bounds = poly.boundingRect().normalized();
    auto range1D = [S](double lo, double hi) {
        const int i0 = int(std::floor(lo / S));
        const int i1 = int(std::ceil(hi / S)) - 1;
        return std::make_pair(std::min(i0, i1), std::max(i0, i1));
    };
    auto [ix0, ix1] = range1D(bounds.left(), bounds.right());
    auto [iy0, iy1] = range1D(bounds.top(), bounds.bottom());
    ix0 -= padTiles; ix1 += padTiles;
    iy0 -= padTiles; iy1 += padTiles;
    const uint64_t count = uint64_t(ix1 - ix0 + 1) * uint64_t(iy1 - iy0 + 1);
    if (count == 0 || count > maxTiles) {
        return {};
    }
    QPainterPath path;
    path.addPolygon(poly);
    QSet<TileKey> out;
    for (int ty = iy0; ty <= iy1; ++ty) {
        for (int tx = ix0; tx <= ix1; ++tx) {
            if (path.intersects(QRectF(S * tx, S * ty, S, S))) {
                out.insert(TileKey{ tx, ty, z });
            }
        }
    }
    return out;
}

bool convex(const std::array<QPointF, 4>& q)
{
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
        const QPointF a = q[i], b = q[(i + 1) % 4], c = q[(i + 2) % 4];
        const double cr = (b.x() - a.x()) * (c.y() - b.y()) - (b.y() - a.y()) * (c.x() - b.x());
        const int s = cr > 0 ? 1 : (cr < 0 ? -1 : 0);
        if (s != 0) {
            if (sign != 0 && s != sign) {
                return false;
            }
            sign = s;
        }
    }
    return true;
}

} // namespace

int main()
{
    const uint64_t maxTiles = 6200;   // Core's provider for the epoch index
    MosaicIndexProvider mip(maxTiles);
    std::mt19937 rng(20261017);
    std::uniform_real_distribution<double> pos(-3000.0, 3000.0);
    std::uniform_real_distribution<double> size(0.0, 160.0);
    std::uniform_real_distribution<double> ang(0.0, 2.0 * M_PI);
    std::uniform_int_distribution<int> zoom(1, 7);
    std::uniform_int_distribution<int> pad(0, 1);

    struct Kind { const char* name; int cases = 0; int same = 0; int added = 0; int missing = 0; double tOld = 0, tNew = 0; };
    Kind kinds[3] = { {"box"}, {"convex"}, {"crossed"} };
    int failures = 0;

    for (int n = 0; n < 120000; ++n) {
        std::array<QPointF, 4> q;
        const int kind = n % 3;
        const double cx = pos(rng), cy = pos(rng);
        if (kind == 0) {   // as calcDimensionRects: the box of a swath segment
            const double w = size(rng), h = size(rng);
            q = { QPointF(cx, cy), QPointF(cx + w, cy), QPointF(cx + w, cy + h), QPointF(cx, cy + h) };
        }
        else if (kind == 1) {   // a rotated box, or a trapezoid like a tilted view
            const double w = size(rng), h = size(rng), a = ang(rng), t = (n % 2) ? 0.0 : 0.6 * w;
            const QPointF loc[4] = { {-w / 2, -h / 2}, {w / 2, -h / 2}, {w / 2 - t, h / 2}, {-w / 2 + t, h / 2} };
            for (int i = 0; i < 4; ++i) {
                q[i] = QPointF(cx + loc[i].x() * std::cos(a) - loc[i].y() * std::sin(a),
                               cy + loc[i].x() * std::sin(a) + loc[i].y() * std::cos(a));
            }
            if (!convex(q)) {
                std::swap(q[2], q[3]);
            }
        }
        else {   // any four points: crossed or not
            for (auto& p : q) {
                p = QPointF(cx + size(rng) - 80.0, cy + size(rng) - 80.0);
            }
        }
        const int z = zoom(rng), pd = pad(rng);
        auto t0 = std::chrono::steady_clock::now();
        const QSet<TileKey> o = oldCover(q, z, pd, maxTiles);
        auto t1 = std::chrono::steady_clock::now();
        const QSet<TileKey> nw = mip.tilesInQuadNed(q, z, pd);
        auto t2 = std::chrono::steady_clock::now();
        Kind& k = kinds[(kind == 2 && convex(q)) ? 1 : kind];
        k.cases++;
        k.tOld += std::chrono::duration<double>(t1 - t0).count();
        k.tNew += std::chrono::duration<double>(t2 - t1).count();
        int miss = 0;
        for (const auto& key : o) {
            if (!nw.contains(key)) {
                ++miss;
            }
        }
        if (miss) {
            k.missing++;
            if (failures++ < 5) {
                std::printf("MISSING %d tiles: %s z %d pad %d quad (%.3f,%.3f) (%.3f,%.3f) (%.3f,%.3f) (%.3f,%.3f)\n", miss,
                            k.name, z, pd, q[0].x(), q[0].y(), q[1].x(), q[1].y(), q[2].x(), q[2].y(), q[3].x(), q[3].y());
            }
        }
        else if (o.size() == nw.size()) {
            k.same++;
        }
        else {
            k.added++;
            if (&k != &kinds[2] && failures++ < 5) {
                std::printf("ADDED %d tiles to a %s quad, z %d\n", int(nw.size() - o.size()), k.name, z);
            }
        }
    }
    for (const auto& k : kinds) {
        std::printf("%-8s %6d cases: same %6d, more tiles %5d, tiles lost %d; old %.2f s, new %.3f s\n",
                    k.name, k.cases, k.same, k.added, k.missing, k.tOld, k.tNew);
    }
    std::printf("tiles_check: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
