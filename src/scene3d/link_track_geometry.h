#pragma once

#include <QColor>
#include <QList>
#include <QPointF>
#include <QRectF>

#include <algorithm>
#include <cmath>

/**
 * @brief Triangles for the boat's track by link quality on screen (LinkTrackItem): no Qt Quick here, so the
 * checks build it alone (tools/linktrack_check).
 */
namespace link_track_geometry {

/** The layout of QSGGeometry::ColoredPoint2D: a position and a premultiplied colour. */
struct Vertex {
    float x, y;
    unsigned char r, g, b, a;
};

/**
 * A polyline whose stretch from point i-1 to point i has quality[i], as triangles: a band lineWidth wide in that
 * quality's colour (colors[quality], straight alpha in, premultiplied out; a quality without a colour takes the last),
 * its ends half a width beyond the points so stretches meet without a notch, and on both sides a fringe 1 px wide
 * fading out (antialiasing without multisampling). A stretch of quality `dashed` (a loss) goes as dashes 2 widths
 * long, 2 apart. Left out: a stretch with an end not finite (behind the camera) or wholly outside clip grown by a
 * width. 18 vertices a band.
 */
inline QList<Vertex> build(const QList<QPointF>& pts, const QList<int>& quality, const QList<QColor>& colors,
                           float lineWidth, int dashed, const QRectF& clip)
{
    QList<Vertex> out;
    const qsizetype n = std::min(pts.size(), quality.size());
    if (n < 2 || colors.isEmpty() || !(lineWidth > 0.0f)) {
        return out;
    }
    const double half = lineWidth * 0.5;
    const double inner = std::max(0.0, half - 0.5);
    const double outer = half + 0.5;
    const QRectF room = clip.adjusted(-lineWidth, -lineWidth, lineWidth, lineWidth);
    out.reserve(n * 18);

    auto band = [&out, inner, outer](const QPointF& p0, const QPointF& p1, const QPointF& nrm, const QColor& c) {
        const unsigned char a = static_cast<unsigned char>(c.alpha());
        const auto pre = [a](int v) { return static_cast<unsigned char>((v * a + 127) / 255); };
        const Vertex solid{ 0.0f, 0.0f, pre(c.red()), pre(c.green()), pre(c.blue()), a };
        const Vertex clear{ 0.0f, 0.0f, 0, 0, 0, 0 };
        const auto at = [](const QPointF& p, const QPointF& off, double k, Vertex v) {
            v.x = static_cast<float>(p.x() + off.x() * k);
            v.y = static_cast<float>(p.y() + off.y() * k);
            return v;
        };
        // a quad from offset k0 to k1 across the band, each side its colour, as two triangles
        const auto quad = [&](double k0, const Vertex& c0, double k1, const Vertex& c1) {
            const Vertex a0 = at(p0, nrm, k0, c0), a1 = at(p0, nrm, k1, c1);
            const Vertex b0 = at(p1, nrm, k0, c0), b1 = at(p1, nrm, k1, c1);
            out << a0 << a1 << b0 << b0 << a1 << b1;
        };
        quad(-inner, solid, inner, solid);
        quad(inner, solid, outer, clear);
        quad(-outer, clear, -inner, solid);
    };

    for (qsizetype i = 1; i < n; ++i) {
        const QPointF a = pts[i - 1], b = pts[i];
        if (!std::isfinite(a.x()) || !std::isfinite(a.y()) || !std::isfinite(b.x()) || !std::isfinite(b.y())) {
            continue;
        }
        if (std::max(a.x(), b.x()) < room.left() || std::min(a.x(), b.x()) > room.right()
            || std::max(a.y(), b.y()) < room.top() || std::min(a.y(), b.y()) > room.bottom()) {
            continue;
        }
        const double dx = b.x() - a.x(), dy = b.y() - a.y();
        const double len = std::hypot(dx, dy);
        if (!(len > 1e-6)) {
            continue;
        }
        const QPointF d(dx / len, dy / len);
        const QPointF nrm(-d.y(), d.x());
        const int q = quality[i];
        const QColor& c = (q >= 0 && q < colors.size()) ? colors[q] : colors.last();
        if (q == dashed) {
            const double dash = 2.0 * lineWidth, step = 4.0 * lineWidth;
            for (double t = 0.0; t < len; t += step) {
                band(a + d * t, a + d * std::min(t + dash, len), nrm, c);
            }
        }
        else {
            band(a - d * half, b + d * half, nrm, c);
        }
    }
    return out;
}

} // namespace link_track_geometry
