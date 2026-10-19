#include "plot2D_bottom_processing.h"
#include "plot2D.h"
#include "math_defs.h"
#include "themes.h"
#include <algorithm>
#include <cmath>
#include <QFontMetrics>

Plot2DBottomProcessing::Plot2DBottomProcessing()
{}

bool Plot2DBottomProcessing::draw(Plot2D* parent, Dataset* dataset)
{
    if (!parent || !dataset) {
        return false;
    }

    auto& canvas = parent->canvas();
    auto& cursor = parent->cursor();

    if (!isVisible() || !cursor.distance.isValid()) {
        return false;
    }

    const bool needDrawLine = (themeId_ == 1 || themeId_ == 2);
    if (needDrawLine) {
        QVector<float> distance1(canvas.width());
        QVector<float> distance2(canvas.width());

        distance1.fill(NAN);
        distance2.fill(NAN);

        if (!cursor.channel2.isValid()) {
            for (int i = 0; i < canvas.width(); i++) {
                int pool_index = cursor.getIndex(i);
                Epoch *data = dataset->fromIndex(pool_index);
                if (data != nullptr) {
                    distance1[i] = data->distProccesing(cursor.channel1);
                }
            }
        } else {
            for (int i = 0; i < canvas.width(); i++) {
                int pool_index = cursor.getIndex(i);
                Epoch *data = dataset->fromIndex(pool_index);
                if (data != nullptr) {
                    distance1[i] = -data->distProccesing(cursor.channel1);
                    distance2[i] = data->distProccesing(cursor.channel2);
                }
            }
        }

        const PlotPen::LineStyle lineStyle = (themeId_ == 2)
            ? PlotPen::LineStylePoint
            : PlotPen::LineStyleSolid;
        PlotPen linePen1 = _penLine;
        PlotPen linePen2 = _penLine2;
        linePen1.lineStyle = lineStyle;
        linePen2.lineStyle = lineStyle;

        drawY(canvas, distance1, cursor.distance.from, cursor.distance.to, linePen1);
        if (cursor.channel2.isValid()) {
            drawY(canvas, distance2, cursor.distance.from, cursor.distance.to, linePen2);
        }

        drawEdits(parent, dataset);
    }

    return true;
}

void Plot2DBottomProcessing::drawEdits(Plot2D* parent, Dataset* dataset) const
{
    // Over the line: the points set by hand (the pencil, a file of edits), and the search limits the
    // "raise" and "lower" tools left, dashed. A side-scan pair is drawn on both halves.
    auto& canvas = parent->canvas();
    auto& cursor = parent->cursor();
    QPainter* p = canvas.painter();
    if (!p) {
        return;
    }
    const int w = canvas.width();
    const float h = float(canvas.height());
    const float range = cursor.distance.to - cursor.distance.from;
    if (w <= 0 || !(std::fabs(range) > 0.0f)) {
        return;
    }
    const float scale = h / range;
    const bool doubled = cursor.channel2.isValid();
    const double s = renderScale();

    QVector<float> hand(w, NAN), lo(w, NAN), hi(w, NAN);
    auto pass = [&](const ChannelId& ch, uint8_t sub, float sign) {
        bool any = false;
        hand.fill(NAN);
        lo.fill(NAN);
        hi.fill(NAN);
        for (int x = 0; x < w; ++x) {
            const Epoch* ep = dataset->fromIndex(cursor.getIndex(x));
            const Epoch::DistProcessing* bp = (ep && cursor.getIndex(x) >= 0) ? ep->bottomProcessingOf(ch, sub) : nullptr;
            if (!bp) {
                continue;
            }
            auto toY = [&](float v) { return std::clamp((sign * v - cursor.distance.from) * scale, 0.0f, h); };
            if (bp->source == Epoch::DistProcessing::DistanceSource::DistanceSourceDirectHand && std::isfinite(bp->distance)) {
                hand[x] = toY(bp->distance);
                any = true;
            }
            if (std::isfinite(bp->min)) {
                lo[x] = toY(bp->min);
                any = true;
            }
            if (std::isfinite(bp->max)) {
                hi[x] = toY(bp->max);
                any = true;
            }
        }
        return any;
    };
    auto drawRuns = [&](const QVector<float>& y, const QPen& pen) {
        p->setPen(pen);
        for (int x = 0; x < w; ++x) {
            if (!std::isfinite(y[x])) {
                continue;
            }
            if (x + 1 < w && std::isfinite(y[x + 1])) {
                p->drawLine(QPointF(x, y[x]), QPointF(x + 1, y[x + 1]));
            }
            else if (x == 0 || !std::isfinite(y[x - 1])) {
                p->drawLine(QPointF(x - 1, y[x]), QPointF(x + 2, y[x]));   // a lone point: a short tick
            }
        }
    };

    QPen handPen(QColor(255, 128, 0), std::max(2.0, 3.0 * s));
    handPen.setCapStyle(Qt::FlatCap);
    QPen limitPen(QColor(90, 200, 255, 210), std::max(1.0, 1.0 * s), Qt::DashLine);

    const auto prevMode = p->compositionMode();
    p->setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (int side = 0; side < (doubled ? 2 : 1); ++side) {
        const ChannelId& ch = side == 0 ? cursor.channel1 : cursor.channel2;
        const uint8_t sub = side == 0 ? cursor.subChannel1 : cursor.subChannel2;
        const float sign = (doubled && side == 0) ? -1.0f : 1.0f;   // as the line: the first channel up
        if (!pass(ch, sub, sign)) {
            continue;
        }
        drawRuns(lo, limitPen);
        drawRuns(hi, limitPen);
        drawRuns(hand, handPen);
    }
    p->setCompositionMode(prevMode);
}

bool Plot2DBottomProcessing::drawDepthValue(Plot2D* parent, Dataset* dataset)
{
    if (!parent || !dataset || !isVisible()) {
        return false;
    }

    const float bottomTrackDepth = dataset->getLastBottomTrackDepth();
    if (!drawDepthText_ || !std::isfinite(bottomTrackDepth)) {
        return false;
    }

    auto& canvas = parent->canvas();
    QPainter* p = canvas.painter();
    if (!p) {
        return false;
    }

    const double s = renderScale();
    const bool vertical = !parent->isHorizontal();
    const QString depthText = formatDepthText(bottomTrackDepth);
    int x;
    if (vertical) {
        const int slot = (parent->hasTemperatureValue() ? 1 : 0) + (parent->hasRangefinderDepthTextValue() ? 1 : 0);
        x = qRound((15 + slot * 64) * s);
    } else {
        x = qRound(370 * s);
        if (!parent->hasTemperatureValue()) {
            x -= qRound(150 * s);
        }
        if (!parent->hasRangefinderDepthTextValue()) {
            x -= qRound(150 * s);
        }
    }
    const int y = canvas.height() - qRound(15 * s);
    drawValueWithBackdrop(p, x, y, depthText, QColor(50, 255, 0), vertical);
    return true;
}

void Plot2DBottomProcessing::setTheme(int theme_id)
{
    themeId_ = theme_id;
}

QString Plot2DBottomProcessing::formatDepthText(float distance) const
{
    const float val = std::round(distance * 100.f) / 100.f;
    const bool isInteger = std::abs(val - std::round(val)) < kmath::fltEps;
    return QString::number(val, 'f', isInteger ? 0 : 2) + QObject::tr(" m");
}

void Plot2DBottomProcessing::drawValueWithBackdrop(QPainter* painter, int x, int baselineY, const QString& text, const QColor& textColor, bool vertical) const
{
    if (!painter) {
        return;
    }

    const double s = renderScale();
    const QFont font("Asap", qRound(30 * s), QFont::Normal);
    const QFontMetrics fm(font);
    const int padX = qRound(8 * s);
    const int padY = qRound(4 * s);
    const int radius = qRound(5 * s);
    const int textW = fm.horizontalAdvance(text);

    const auto prevMode = painter->compositionMode();
    painter->setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Vertical mode: canvas globally rotated -90 (Plot2D::getImage); counter-rotate
    // +90 to keep the value upright, right-aligned so it stays inside the deep edge.
    painter->save();
    if (vertical) {
        painter->translate(x, baselineY);
        painter->rotate(90);
    }
    const int ax = vertical ? -textW : x;
    const int ay = vertical ? 0 : baselineY;
    const QRect bgRect(ax - padX,
                       ay - fm.ascent() - padY,
                       textW + padX * 2,
                       fm.height() + padY * 2);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, 115));
    painter->drawRoundedRect(bgRect, radius, radius);
    painter->setFont(font);
    painter->setPen(QPen(textColor));
    painter->drawText(ax, ay, text);
    painter->restore();
    painter->setCompositionMode(prevMode);
}
