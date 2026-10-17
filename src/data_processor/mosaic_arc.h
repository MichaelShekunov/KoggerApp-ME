#pragma once

// How the mosaic fills the gap between two consecutive pings of one side (MosaicProcessor). For each
// range the two pings' samples are joined along the path of an interpolated sonar pose: the origin
// moves on the straight line between the two pings' origins and the heading turns the shorter way,
// so a sample at range r(u) sits at origin(u) + r(u) * dir(heading(u)), u from 0 to 1. Moving
// straight that path is the chord between the two samples; turning on the spot it is the arc around
// the sonar. Until 2026-10-17 the chord was drawn always, and a circular mosaic (a sonar turned on
// the spot) showed its range rings as polygons (the owner's screenshot).
//
// The choice is made once for a pair of pings, from their far ends (pathFor): the chord stays where
// the arc would not leave it by half a pixel even there -- the result is then the old one pixel for
// pixel -- and for a turn above maxTurnRad, a glitch in the heading rather than a turn. Chosen per
// range instead, the last chords and the first arcs left a seam of holes between them. Everything is
// in mesh pixels; no Qt, so tools/arc_check builds it alone.

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace mosaic_arc {

constexpr float kPi = 3.14159265358979323846f;

enum class Path { Chord, Arc };

/// The turn from heading a to heading b the shorter way, in (-pi, pi].
inline float shortTurn(float a, float b)
{
    float d = std::fmod(b - a, 2.0f * kPi);
    if (d > kPi) {
        d -= 2.0f * kPi;
    }
    else if (d <= -kPi) {
        d += 2.0f * kPi;
    }
    return d;
}

/// The path for a pair of pings: o* their origins, f/s the far ends of their lines.
inline Path pathFor(float ofx, float ofy, float fx, float fy,
                    float osx, float osy, float sx, float sy, float maxTurnRad)
{
    const float rf = std::hypot(fx - ofx, fy - ofy);
    const float rs = std::hypot(sx - osx, sy - osy);
    if (rf < 1.0f || rs < 1.0f) {
        return Path::Chord;
    }
    const float turn = std::fabs(shortTurn(std::atan2(fy - ofy, fx - ofx), std::atan2(sy - osy, sx - osx)));
    if (turn > maxTurnRad) {
        return Path::Chord;
    }
    // How far the arc's middle stands off the chord: r (1 - cos(turn / 2)) ~ r turn^2 / 8.
    return std::max(rf, rs) * turn * turn / 8.0f >= 0.5f ? Path::Arc : Path::Chord;
}

/// Calls put(x, y, u, k) for every pixel of the path from the F sample to the S sample: k counts the
/// calls from 0, u grows from 0 at F to 1 at S. Returns the number of calls. The chord is the 4-
/// connected Bresenham of the code it replaces, start and end included, nothing when both samples
/// fall on one pixel. The arc is sampled at most half a pixel apart, a diagonal step gets the corner
/// pixel so the path stays 4-connected, two calls in a row never repeat a pixel (one can come back
/// where the arc runs along a pixel edge), and pixels outside [0, width) x [0, height) are skipped;
/// within a pixel of the sonar it is the chord.
template <typename Put>
int walk(float ofx, float ofy, float fx, float fy,
         float osx, float osy, float sx, float sy,
         Path path, int width, int height, Put&& put)
{
    int x0 = int(fx);
    int y0 = int(fy);
    const int x1 = int(sx);
    const int y1 = int(sy);
    const int dx = std::abs(x1 - x0);
    const int dy = std::abs(y1 - y0);
    const int stepsTot = dx + dy;
    if (stepsTot == 0) {
        return 0;
    }

    const float rf = std::hypot(fx - ofx, fy - ofy);
    const float rs = std::hypot(sx - osx, sy - osy);
    if (path == Path::Chord || rf < 1.0f || rs < 1.0f) {
        const int stepX = (x0 < x1) ? 1 : -1;
        const int stepY = (y0 < y1) ? 1 : -1;
        int err = dx - dy;
        int k = 0;
        while (true) {
            put(x0, y0, float(k) / float(stepsTot), k);
            if (x0 == x1 && y0 == y1) {
                break;
            }
            const int e2 = err << 1;
            if (e2 > -dy) {
                err -= dy;
                x0 += stepX;
            }
            else if (e2 < dx) {
                err += dx;
                y0 += stepY;
            }
            ++k;
        }
        return k + 1;
    }

    const float thf = std::atan2(fy - ofy, fx - ofx);
    const float turn = shortTurn(thf, std::atan2(sy - osy, sx - osx));
    // |P'(u)| <= |dO| + |dr| + r_max |turn|: n steps of at most half a pixel each.
    const float bound = std::hypot(osx - ofx, osy - ofy) + std::fabs(rs - rf) + std::max(rf, rs) * std::fabs(turn);
    const int n = std::max(1, int(std::ceil(2.0f * bound)));

    int k = 0;
    int px = x0;
    int py = y0;
    auto visit = [&](int x, int y, float u) {
        if (x >= 0 && y >= 0 && x < width && y < height) {
            put(x, y, u, k++);
        }
    };
    visit(px, py, 0.0f);
    for (int i = 1; i <= n; ++i) {
        const float u = float(i) / float(n);
        int ix = x1;
        int iy = y1;
        if (i < n) {
            const float r = rf + u * (rs - rf);
            const float th = thf + u * turn;
            ix = int(ofx + u * (osx - ofx) + r * std::cos(th));
            iy = int(ofy + u * (osy - ofy) + r * std::sin(th));
        }
        if (ix == px && iy == py) {
            continue;
        }
        if (ix != px && iy != py) {
            visit(ix, py, u);   // the corner, so the path stays 4-connected
        }
        visit(ix, iy, u);
        px = ix;
        py = iy;
    }
    return k;
}

} // namespace mosaic_arc
