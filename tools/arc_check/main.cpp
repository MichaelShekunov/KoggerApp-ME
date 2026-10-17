// Offline check of src/data_processor/mosaic_arc.h -- how the mosaic fills the gap between two pings.
//
//  1. Wherever walk() keeps the chord (no turn, a turn too small to matter, a turn too big to be
//     real), it is the old chord pixel for pixel: the Bresenham MosaicProcessor::updateData drew
//     before 2026-10-17 is copied below verbatim as the reference.
//  2. An arc stays on the circle around the sonar, starts and ends on the two samples, is
//     4-connected, rarely repeats a pixel (where its tangent runs along a pixel edge), turns the
//     short way across +-180 deg, and never leaves the mesh.
//  3. A whole turn on the spot with an echo every 20 px of range, painted as updateData paints it:
//     every painted ring pixel lies within 2 px of its ring -- pixel centres and 4-connected steps
//     alone give up to ~2 px, the old chord too where its bulge is under a pixel. From a ping every
//     12 deg the control -- the same picture with the old chord -- must FAIL (more than 3 px), or this
//     check proves nothing: it is the owner's polygon picture.
//  4. The same turn walked as updateData walks it (the longer ray's Bresenham, both pings at the same
//     fraction of their length): no pixel of the disc is left out that the old chord painted. Choosing
//     chord or arc per range instead of per pair left a seam of holes where the one met the other.
//
//     arc_check            all checks, exit code 0 when they pass

#include "mosaic_arc.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <utility>
#include <vector>

namespace {

constexpr float kMaxTurn = 1.0471976f;    // MosaicProcessor's kMaxArcTurnRad
constexpr float kDeg = 3.14159265358979323846f / 180.0f;

struct Pt { int x; int y; float u; int k; };

int g_fail = 0;
int g_pass = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        ++g_pass;
    }
    else {
        ++g_fail;
        std::printf("FAIL  %s\n", what);
    }
}

// The chord MosaicProcessor::updateData drew until 2026-10-17, verbatim (bresStep, the loop, iP).
std::vector<Pt> oldChord(float fx, float fy, float sx, float sy)
{
    std::vector<Pt> out;
    int x0 = int(fx);
    int y0 = int(fy);
    const int x1 = int(sx);
    const int y1 = int(sy);
    const int dx = std::abs(x1 - x0);
    const int dy = std::abs(y1 - y0);
    const int stx = (x0 < x1) ? 1 : -1;
    const int sty = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    const int stepsTot = dx + dy;
    auto bresStep = [&]() -> bool {
        if (x0 == x1 && y0 == y1) {
            return true;
        }
        int e2 = (err << 1);
        if (e2 > -dy) {
            err -= dy; x0 += stx;
        }
        else if (e2 < dx) {
            err += dx; y0 += sty;
        }
        return false;
    };
    if (!(stepsTot != 0 && stepsTot >= 0)) {   // checkLength(stepsTot)
        return out;
    }
    int stepsDone = 0;
    while (true) {
        const float iP = static_cast<float>(stepsDone) / float(stepsTot);
        out.push_back({x0, y0, iP, stepsDone});
        if (bresStep()) {
            break;
        }
        ++stepsDone;
    }
    return out;
}

std::vector<Pt> walk(float ofx, float ofy, float fx, float fy, float osx, float osy, float sx, float sy,
                     int w = 100000, int h = 100000)
{
    std::vector<Pt> out;
    const auto path = mosaic_arc::pathFor(ofx, ofy, fx, fy, osx, osy, sx, sy, kMaxTurn);
    const int n = mosaic_arc::walk(ofx, ofy, fx, fy, osx, osy, sx, sy, path, w, h,
                                   [&](int x, int y, float u, int k) { out.push_back({x, y, u, k}); });
    if (n != int(out.size())) {
        out.push_back({-999999, -999999, -1.0f, -1});   // a wrong count fails every comparison
    }
    return out;
}

bool usesArc(float ofx, float ofy, float fx, float fy, float osx, float osy, float sx, float sy)
{
    return mosaic_arc::pathFor(ofx, ofy, fx, fy, osx, osy, sx, sy, kMaxTurn) == mosaic_arc::Path::Arc;
}

bool same(const std::vector<Pt>& a, const std::vector<Pt>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].u != b[i].u || a[i].k != b[i].k) {
            return false;
        }
    }
    return true;
}

// A small deterministic generator, so a failure repeats.
unsigned g_seed = 12345u;
float rnd(float lo, float hi)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return lo + (hi - lo) * float((g_seed >> 8) & 0xFFFFFF) / float(0xFFFFFF);
}

void checkChordKept()
{
    int arcs = 0;
    bool allSame = true;
    for (int i = 0; i < 20000; ++i) {
        const float ox = rnd(2000.0f, 3000.0f), oy = rnd(2000.0f, 3000.0f);
        const float r = rnd(0.0f, 900.0f), th = rnd(-3.2f, 3.2f);
        const int kind = i % 3;
        // 0: moving straight (same heading); 1: a turn under half a pixel of bulge; 2: a turn above the limit
        float turn = 0.0f;
        if (kind == 1) {
            turn = std::sqrt(4.0f / std::max(r, 1.0f)) * rnd(0.0f, 0.99f) * (i % 2 ? 1.0f : -1.0f);
        }
        else if (kind == 2) {
            turn = rnd(kMaxTurn + 0.01f, 3.1f) * (i % 2 ? 1.0f : -1.0f);
        }
        const float mx = (kind == 0) ? rnd(-40.0f, 40.0f) : 0.0f;
        const float my = (kind == 0) ? rnd(-40.0f, 40.0f) : 0.0f;
        const float fx = ox + r * std::cos(th), fy = oy + r * std::sin(th);
        const float osx = ox + mx, osy = oy + my;
        const float sx = osx + r * std::cos(th + turn), sy = osy + r * std::sin(th + turn);
        if (usesArc(ox, oy, fx, fy, osx, osy, sx, sy)) {
            ++arcs;
            continue;
        }
        if (!same(walk(ox, oy, fx, fy, osx, osy, sx, sy), oldChord(fx, fy, sx, sy))) {
            allSame = false;
        }
    }
    check(allSame, "chord kept: identical to the old Bresenham, pixel, u and step");
    check(arcs == 0, "chord kept: no arc chosen for a straight move, a sub-pixel bulge or a glitch turn");
    std::printf("      chord cases: 20000, arcs chosen among them: %d\n", arcs);
}

void checkArcShape()
{
    bool onCircle = true, ends = true, connected = true, monotone = true, chosen = true;
    long pixels = 0, repeats = 0;
    float worst = 0.0f;
    for (int i = 0; i < 20000; ++i) {
        const float ox = rnd(2000.0f, 3000.0f), oy = rnd(2000.0f, 3000.0f);
        const float r = rnd(20.0f, 900.0f), th = rnd(-3.2f, 3.2f);
        const float minTurn = std::sqrt(4.0f / r) * 1.05f;
        if (minTurn >= kMaxTurn) {
            continue;
        }
        const float turn = rnd(minTurn, kMaxTurn - 0.01f) * (i % 2 ? 1.0f : -1.0f);
        const float fx = ox + r * std::cos(th), fy = oy + r * std::sin(th);
        const float sx = ox + r * std::cos(th + turn), sy = oy + r * std::sin(th + turn);
        if (!usesArc(ox, oy, fx, fy, ox, oy, sx, sy)) {
            chosen = false;
            continue;
        }
        const auto p = walk(ox, oy, fx, fy, ox, oy, sx, sy);
        // The end pixel may be entered before u = 1, and is then not called again at u = 1.
        if (p.empty() || p.front().x != int(fx) || p.front().y != int(fy) || p.back().x != int(sx) || p.back().y != int(sy)
                || p.front().u != 0.0f || p.back().u > 1.0f || p.back().u < 0.9f) {
            ends = false;
        }
        std::set<std::pair<int, int>> seen;
        for (size_t j = 0; j < p.size(); ++j) {
            const float d = std::hypot(p[j].x + 0.5f - ox, p[j].y + 0.5f - oy) - r;
            worst = std::max(worst, std::fabs(d));
            if (std::fabs(d) > 1.5f) {
                onCircle = false;
            }
            ++pixels;
            if (!seen.insert({p[j].x, p[j].y}).second) {
                ++repeats;
            }
            if (j > 0) {
                if (std::abs(p[j].x - p[j - 1].x) + std::abs(p[j].y - p[j - 1].y) != 1) {
                    connected = false;
                }
                if (p[j].u < p[j - 1].u || p[j].k != p[j - 1].k + 1) {
                    monotone = false;
                }
            }
        }
    }
    check(chosen, "arc chosen for every turn with half a pixel of bulge or more, up to the limit");
    check(onCircle, "arc on the circle around the sonar (pixel centre within 1.5 px)");
    check(ends, "arc starts on the F sample with u = 0 and ends on the S sample with u near 1");
    check(connected, "arc 4-connected");
    check(repeats * 200 < pixels, "arc repeats under 0.5 % of its pixels");
    check(monotone, "arc: u never decreases, k counts the calls");
    std::printf("      worst distance off the circle: %.2f px; repeated pixels %ld of %ld\n", worst, repeats, pixels);
}

// Moving and turning at once, the two pings' ranges differing: each pixel lies on the interpolated
// pose's sample point for its u (origin, range and heading all interpolated), and the path joins.
void checkPosePath()
{
    bool onPath = true, ends = true, connected = true;
    float worst = 0.0f;
    for (int i = 0; i < 20000; ++i) {
        const float ox = rnd(2000.0f, 3000.0f), oy = rnd(2000.0f, 3000.0f);
        const float rf = rnd(40.0f, 900.0f), rs = rf * rnd(0.8f, 1.2f);
        const float th = rnd(-3.2f, 3.2f), turn = rnd(0.1f, kMaxTurn - 0.01f) * (i % 2 ? 1.0f : -1.0f);
        const float osx = ox + rnd(-15.0f, 15.0f), osy = oy + rnd(-15.0f, 15.0f);
        const float fx = ox + rf * std::cos(th), fy = oy + rf * std::sin(th);
        const float sx = osx + rs * std::cos(th + turn), sy = osy + rs * std::sin(th + turn);
        if (!usesArc(ox, oy, fx, fy, osx, osy, sx, sy)) {
            continue;
        }
        const auto p = walk(ox, oy, fx, fy, osx, osy, sx, sy);
        if (p.empty() || p.front().x != int(fx) || p.front().y != int(fy) || p.back().x != int(sx) || p.back().y != int(sy)) {
            ends = false;
        }
        for (size_t j = 0; j < p.size(); ++j) {
            // Distance to the pose path itself, near the pixel's u: a corner pixel carries the next
            // step's u, so P(u) alone would count the step along the path as an error off it.
            float d = 1e9f;
            for (int q = -100; q <= 100; ++q) {
                const float u = std::clamp(p[j].u + q * 0.0005f, 0.0f, 1.0f);
                const float r = rf + u * (rs - rf), a = th + u * turn;
                const float px = ox + u * (osx - ox) + r * std::cos(a), py = oy + u * (osy - oy) + r * std::sin(a);
                d = std::min(d, std::hypot(p[j].x + 0.5f - px, p[j].y + 0.5f - py));
            }
            worst = std::max(worst, d);
            if (d > 1.5f) {
                onPath = false;
            }
            if (j > 0 && std::abs(p[j].x - p[j - 1].x) + std::abs(p[j].y - p[j - 1].y) != 1) {
                connected = false;
            }
        }
    }
    check(onPath, "moving and turning, ranges differing: every pixel on the interpolated pose (within 1.5 px)");
    check(ends && connected, "moving and turning: starts and ends on the samples, 4-connected");
    std::printf("      worst distance off the pose path: %.2f px\n", worst);

    // The S sample exactly on a pixel corner: a recomputed end point a hair short of it would land in
    // the next pixel over.
    bool onCorner = true;
    for (int i = 0; i < 20000; ++i) {
        const float ox = std::floor(rnd(2000.0f, 3000.0f)), oy = std::floor(rnd(2000.0f, 3000.0f));
        const float rf = rnd(40.0f, 900.0f), th = rnd(-3.2f, 3.2f);
        const float turn = rnd(0.2f, kMaxTurn - 0.01f) * (i % 2 ? 1.0f : -1.0f);
        const float fx = ox + rf * std::cos(th), fy = oy + rf * std::sin(th);
        const float sx = std::round(ox + rf * std::cos(th + turn)), sy = std::round(oy + rf * std::sin(th + turn));
        const auto p = walk(ox, oy, fx, fy, ox, oy, sx, sy);
        if (!p.empty() && (p.back().x != int(sx) || p.back().y != int(sy))) {
            onCorner = false;
        }
    }
    check(onCorner, "an S sample on a pixel corner ends the path in its own pixel");
}

void checkWrapAndBounds()
{
    // From 179 deg to -179 deg is 2 deg the short way, not 358 the long one.
    const float ox = 1000.0f, oy = 1000.0f, r = 600.0f;
    const auto p = walk(ox, oy, ox + r * std::cos(179.0f * kDeg), oy + r * std::sin(179.0f * kDeg),
                        ox, oy, ox + r * std::cos(-179.0f * kDeg), oy + r * std::sin(-179.0f * kDeg));
    bool shortWay = !p.empty();
    for (const auto& q : p) {
        if (q.x > int(ox)) {   // the long way would pass the east side
            shortWay = false;
        }
    }
    check(shortWay && p.size() < 40, "arc turns the short way across 180 deg");

    // A mesh 1000 px wide: the arc's bulge past the edge is skipped, never passed on.
    bool inside = true;
    int calls = 0;
    mosaic_arc::walk(500.0f, 500.0f, 995.0f, 400.0f, 500.0f, 500.0f, 995.0f, 600.0f, mosaic_arc::Path::Arc, 1000, 1000,
                     [&](int x, int y, float, int) { ++calls; if (x < 0 || y < 0 || x >= 1000 || y >= 1000) inside = false; });
    check(inside && calls > 0, "arc pixels outside the mesh are skipped");
}

// The owner's picture: a sonar turning on the spot, a ping every `step` deg, an echo ring every 20 px
// of range. The gap between pings is painted as updateData paints it -- along both pings at the same
// fraction of their length, the pixel's colour interpolated -- and every painted ring pixel is
// checked against its ring.
float ringError(bool arc, float stepDeg)
{
    const int W = 1400, H = 1400;
    const float ox = 700.0f, oy = 700.0f, R = 600.0f;
    std::vector<unsigned char> img(size_t(W) * H, 0);
    std::vector<float> rad(size_t(W) * H, -1.0f);
    const int pings = int(std::round(360.0f / stepDeg));
    for (int pIdx = 0; pIdx < pings; ++pIdx) {
        const float a0 = pIdx * stepDeg * kDeg, a1 = (pIdx + 1) * stepDeg * kDeg;
        const auto path = mosaic_arc::pathFor(ox, oy, ox + R * std::cos(a0), oy + R * std::sin(a0),
                                              ox, oy, ox + R * std::cos(a1), oy + R * std::sin(a1), kMaxTurn);
        for (int s = 1; s <= int(R); ++s) {   // one pixel of range per step, as the outer loop walks
            const float rr = float(s);
            const float fx = ox + rr * std::cos(a0), fy = oy + rr * std::sin(a0);
            const float sx = ox + rr * std::cos(a1), sy = oy + rr * std::sin(a1);
            const unsigned char c = (s % 20 == 0) ? 255 : 0;
            auto put = [&](int x, int y, float, int) {
                img[size_t(y) * W + x] = c;
                rad[size_t(y) * W + x] = rr;
            };
            if (arc) {
                mosaic_arc::walk(ox, oy, fx, fy, ox, oy, sx, sy, path, W, H, put);
            }
            else {
                for (const auto& q : oldChord(fx, fy, sx, sy)) {
                    put(q.x, q.y, q.u, q.k);
                }
            }
        }
    }
    float worst = 0.0f;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (img[size_t(y) * W + x] == 255) {
                const float d = std::hypot(x + 0.5f - ox, y + 0.5f - oy) - rad[size_t(y) * W + x];
                worst = std::max(worst, std::fabs(d));
            }
        }
    }
    return worst;
}

// Holes: a whole turn painted as updateData walks it -- along the longer of the two rays with its
// Bresenham, both pings sampled at the same fraction of their length -- and the pixels inside the
// disc that no pair painted counted. Returns the holes for the arc (or the old chord).
long coverageHoles(bool arc, float stepDeg, float ox, float oy, float R)
{
    const int W = 1400, H = 1400;
    std::vector<unsigned char> img(size_t(W) * H, 0);
    const int pings = int(std::round(360.0f / stepDeg));
    for (int pIdx = 0; pIdx < pings; ++pIdx) {
        const float a0 = pIdx * stepDeg * kDeg, a1 = (pIdx + 1) * stepDeg * kDeg;
        const float fEx = ox + R * std::cos(a0), fEy = oy + R * std::sin(a0);
        const float sEx = ox + R * std::cos(a1), sEy = oy + R * std::sin(a1);
        const auto path = mosaic_arc::pathFor(ox, oy, fEx, fEy, ox, oy, sEx, sEy, kMaxTurn);
        // the big ray: F's (equal lengths), Bresenham from its origin pixel to its end pixel
        int bx = int(ox), by = int(oy);
        const int ex = int(fEx), ey = int(fEy);
        const int dx = std::abs(ex - bx), dy = std::abs(ey - by);
        const int sx = (bx < ex) ? 1 : -1, sy = (by < ey) ? 1 : -1;
        int err = dx - dy;
        const float tot = std::max(1.0f, std::hypot(float(ex - bx), float(ey - by)));
        while (true) {
            const float rem = std::hypot(float(ex - bx), float(ey - by));
            const float t = std::clamp(rem / tot, 0.0f, 1.0f);
            const float fx = ox + t * (fEx - ox), fy = oy + t * (fEy - oy);
            const float qx = ox + t * (sEx - ox), qy = oy + t * (sEy - oy);
            auto put = [&](int x, int y, float, int) {
                if (x >= 0 && y >= 0 && x < W && y < H) {
                    img[size_t(y) * W + x] = 1;
                }
            };
            if (arc) {
                mosaic_arc::walk(ox, oy, fx, fy, ox, oy, qx, qy, path, W, H, put);
            }
            else {
                for (const auto& q : oldChord(fx, fy, qx, qy)) {
                    put(q.x, q.y, q.u, q.k);
                }
            }
            if (bx == ex && by == ey) {
                break;
            }
            const int e2 = err << 1;
            if (e2 > -dy) { err -= dy; bx += sx; }
            else if (e2 < dx) { err += dx; by += sy; }
        }
    }
    long holes = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float d = std::hypot(x + 0.5f - ox, y + 0.5f - oy);
            if (d > 3.0f && d < R - 3.0f && !img[size_t(y) * W + x]) {
                ++holes;
            }
        }
    }
    return holes;
}

void checkCoverage()
{
    for (float step : {6.0f, 12.0f}) {
        const long a = coverageHoles(true, step, 700.3f, 700.6f, 600.0f);
        const long c = coverageHoles(false, step, 700.3f, 700.6f, 600.0f);
        std::printf("      ping every %4.0f deg: holes inside the disc, arc %ld, old chord %ld\n", step, a, c);
        char what[160];
        std::snprintf(what, sizeof what, "no more holes with the arc than with the old chord, a ping every %.0f deg (%ld vs %ld)", step, a, c);
        check(a <= c, what);
    }
}

void checkRings()
{
    for (float step : {6.0f, 12.0f, 20.0f}) {
        const float a = ringError(true, step);
        const float c = ringError(false, step);
        char what[160];
        std::snprintf(what, sizeof what, "rings round with the arc, a ping every %.0f deg (worst %.2f px)", step, a);
        check(a <= 2.0f, what);
        if (step >= 12.0f) {
            std::snprintf(what, sizeof what, "control: the old chord leaves the rings, a ping every %.0f deg (worst %.2f px)", step, c);
            check(c > 3.0f, what);
        }
        std::printf("      ping every %4.0f deg: ring error arc %.2f px, old chord %.2f px\n", step, a, c);
    }
}

} // namespace

int main()
{
    checkChordKept();
    checkArcShape();
    checkPosePath();
    checkWrapAndBounds();
    checkRings();
    checkCoverage();
    std::printf("arc_check: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
