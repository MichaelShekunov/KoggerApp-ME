// Standalone check of src/scene3d/link_track_geometry.h: the triangles LinkTrackItem draws the boat's track by link
// quality with over the 3D view (2026-10-22: it replaced a Repeater of Shapes that held a drag of the view).
//  1. a stretch: a band 18 vertices, as wide as the line plus a 1 px fringe, its ends half a width beyond the points,
//     in its quality's colour, premultiplied; the stretch from i-1 to i in quality[i]'s colour;
//  2. a stretch over a loss as dashes 2 widths long, 2 apart, not beyond its ends;
//  3. left out: a stretch with an end not finite (behind the camera), one wholly outside the item;
//  4. a quality with no colour of its own takes the last;
//  5. a long session's track (3000 samples) built in a small fraction of a frame.
// Exit 0 = pass, 1 = fail.
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <cmath>
#include <cstdio>
#include <limits>

#include "link_track_geometry.h"

namespace {

using link_track_geometry::Vertex;
using link_track_geometry::build;

int fails = 0;
void ok(const char* name, bool good, const QString& detail = QString())
{
    std::printf("%s %-70s %s\n", good ? "ok  " : "FAIL", name, detail.toUtf8().constData());
    if (!good) {
        ++fails;
    }
}

constexpr int kLost = 5;
const QList<QColor> kColors = { QColor("#22C55E"), QColor("#A3E635"), QColor("#F59E0B"), QColor("#EF4444"),
                                QColor("#94A3B8"), QColor("#EF4444") };
const QRectF kRoom(0, 0, 800, 600);

struct Extent { float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f; };
Extent extentOf(const QList<Vertex>& v, qsizetype from = 0, qsizetype to = -1)
{
    Extent e;
    if (to < 0) to = v.size();
    for (qsizetype i = from; i < to; ++i) {
        e.x0 = std::min(e.x0, v[i].x); e.x1 = std::max(e.x1, v[i].x);
        e.y0 = std::min(e.y0, v[i].y); e.y1 = std::max(e.y1, v[i].y);
    }
    return e;
}
bool near(double a, double b) { return std::fabs(a - b) < 1e-3; }
// the solid colour of a band (its first vertex is solid: the core quad comes first)
bool colourIs(const Vertex& v, int r, int g, int b, int a) { return v.r == r && v.g == g && v.b == b && v.a == a; }

} // namespace

int main()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();

    // 1. one stretch along x, width 4
    QList<Vertex> v = build({ QPointF(100, 100), QPointF(200, 100) }, { 0, 0 }, kColors, 4.0f, kLost, kRoom);
    ok("a stretch: one band, 18 vertices", v.size() == 18, QString("%1").arg(v.size()));
    Extent e = extentOf(v);
    ok("its ends half a width beyond the points: x 98..202", near(e.x0, 98) && near(e.x1, 202),
       QString("%1..%2").arg(e.x0).arg(e.x1));
    ok("as wide as the line and a 1 px fringe: y 97.5..102.5", near(e.y0, 97.5) && near(e.y1, 102.5),
       QString("%1..%2").arg(e.y0).arg(e.y1));
    ok("in its quality's colour (good: #22C55E), opaque", !v.isEmpty() && colourIs(v[0], 0x22, 0xC5, 0x5E, 255));
    int clear = 0, solid = 0;
    for (const Vertex& x : v) { if (x.a == 0) ++clear; else if (x.a == 255) ++solid; }
    ok("the fringe fades out: 6 vertices clear, 12 solid", clear == 6 && solid == 12, QString("%1 clear, %2 solid").arg(clear).arg(solid));

    // the stretch from i-1 to i takes quality[i]
    v = build({ QPointF(100, 100), QPointF(150, 100), QPointF(200, 100) }, { 3, 0, 2 }, kColors, 4.0f, kLost, kRoom);
    ok("two stretches: two bands", v.size() == 36, QString("%1").arg(v.size()));
    ok("the first in quality[1]'s colour (good)", v.size() == 36 && colourIs(v[0], 0x22, 0xC5, 0x5E, 255));
    ok("the second in quality[2]'s colour (weak: #F59E0B)", v.size() == 36 && colourIs(v[18], 0xF5, 0x9E, 0x0B, 255));

    // premultiplied
    v = build({ QPointF(100, 100), QPointF(200, 100) }, { 0, 0 }, { QColor(200, 100, 50, 128) }, 4.0f, kLost, kRoom);
    ok("a colour with alpha drawn premultiplied: (100, 50, 25, 128)", !v.isEmpty() && colourIs(v[0], 100, 50, 25, 128),
       v.isEmpty() ? QString() : QString("(%1, %2, %3, %4)").arg(v[0].r).arg(v[0].g).arg(v[0].b).arg(v[0].a));

    // 2. a loss: 40 px, width 4 -> dashes of 8 every 16: 0..8, 16..24, 32..40
    v = build({ QPointF(100, 100), QPointF(140, 100) }, { 0, kLost }, kColors, 4.0f, kLost, kRoom);
    ok("a loss as dashes: 3 over 40 px at width 4", v.size() == 54, QString("%1 vertices").arg(v.size()));
    if (v.size() == 54) {
        const Extent d0 = extentOf(v, 0, 18), d1 = extentOf(v, 18, 36), d2 = extentOf(v, 36, 54);
        ok("each 2 widths long, 2 apart", near(d0.x0, 100) && near(d0.x1, 108) && near(d1.x0, 116) && near(d1.x1, 124)
           && near(d2.x0, 132) && near(d2.x1, 140),
           QString("%1..%2, %3..%4, %5..%6").arg(d0.x0).arg(d0.x1).arg(d1.x0).arg(d1.x1).arg(d2.x0).arg(d2.x1));
    }
    else {
        ok("each 2 widths long, 2 apart", false, "no three dashes");
    }
    ok("in the loss colour (#EF4444)", !v.isEmpty() && colourIs(v[0], 0xEF, 0x44, 0x44, 255));

    // 3. left out
    v = build({ QPointF(100, 100), QPointF(110, 100), QPointF(nan, nan), QPointF(130, 100), QPointF(140, 100) },
              { 0, 0, 0, 0, 0 }, kColors, 4.0f, kLost, kRoom);
    ok("a stretch with an end not finite left out: 2 of 4 bands", v.size() == 36, QString("%1 vertices").arg(v.size()));
    bool finite = true;
    for (const Vertex& x : v) finite = finite && std::isfinite(x.x) && std::isfinite(x.y);
    ok("no vertex not finite", finite);
    const double inf = std::numeric_limits<double>::infinity();
    v = build({ QPointF(100, 100), QPointF(inf, 100), QPointF(120, 100) }, { 0, 0, 0 }, kColors, 4.0f, kLost, kRoom);
    finite = true;
    for (const Vertex& x : v) finite = finite && std::isfinite(x.x) && std::isfinite(x.y);
    ok("an end at infinity left out as well", v.isEmpty() && finite, QString("%1 vertices").arg(v.size()));
    v = build({ QPointF(900, 700), QPointF(1000, 700) }, { 0, 0 }, kColors, 4.0f, kLost, kRoom);
    ok("a stretch wholly outside the item left out", v.isEmpty(), QString("%1 vertices").arg(v.size()));
    int kept = 0;
    for (const auto& s : { std::pair{ QPointF(-100, 300), QPointF(-50, 310) }, std::pair{ QPointF(900, 300), QPointF(950, 310) },
                           std::pair{ QPointF(300, -100), QPointF(310, -50) }, std::pair{ QPointF(300, 700), QPointF(310, 750) } }) {
        kept += int(build({ s.first, s.second }, { 0, 0 }, kColors, 4.0f, kLost, kRoom).size());
    }
    ok("outside on any one side (left, right, top, bottom): left out", kept == 0, QString("%1 vertices").arg(kept));
    v = build({ QPointF(-100, 300), QPointF(900, 300) }, { 0, 0 }, kColors, 4.0f, kLost, kRoom);
    ok("one across the item kept", v.size() == 18, QString("%1 vertices").arg(v.size()));
    v = build({ QPointF(801, 300), QPointF(900, 300) }, { 0, 0 }, kColors, 4.0f, kLost, kRoom);
    ok("one just outside, within a width, kept (its band shows)", v.size() == 18, QString("%1 vertices").arg(v.size()));

    // 4. no colour of its own
    v = build({ QPointF(100, 100), QPointF(200, 100) }, { 0, 9 }, kColors, 4.0f, kLost, kRoom);
    ok("a quality with no colour takes the last", !v.isEmpty() && colourIs(v[0], 0xEF, 0x44, 0x44, 255));
    ok("nothing for fewer than 2 points, no colours or no width",
       build({ QPointF(1, 1) }, { 0 }, kColors, 4.0f, kLost, kRoom).isEmpty()
       && build({ QPointF(1, 1), QPointF(2, 2) }, { 0, 0 }, {}, 4.0f, kLost, kRoom).isEmpty()
       && build({ QPointF(1, 1), QPointF(2, 2) }, { 0, 0 }, kColors, 0.0f, kLost, kRoom).isEmpty());

    // 5. a long session's track: 3000 samples, the quality changing every 6, a loss every tenth change
    QList<QPointF> pts;
    QList<int> q;
    QRandomGenerator rng(20261022);
    QPointF p(400, 300);
    for (int i = 0; i < 3000; ++i) {
        p += QPointF(rng.bounded(6.0) - 3.0, rng.bounded(6.0) - 3.0);
        pts << p;
        const int run = i / 6;
        q << ((i % 6 == 0 && run % 10 == 9) ? kLost : run % 4);
    }
    QElapsedTimer t;
    t.start();
    int built = 0;
    for (int k = 0; k < 20; ++k) built += int(build(pts, q, kColors, 4.0f, kLost, kRoom).size());
    const double ms = double(t.nsecsElapsed()) / 1e6 / 20.0;
    ok("3000 samples built in under 5 ms (a frame is 16)", ms < 5.0, QString("%1 ms, %2 vertices").arg(ms, 0, 'f', 2).arg(built / 20));

    std::printf("\nlinktrack_check: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
