#include "isobath_labels.h"

#include <algorithm>
#include <cmath>
#include <QColor>
#include <QHash>
#include <QRectF>
#include <QVector2D>
#include <QVector4D>
#include "draw_utils.h"
#include "math_defs.h"
#include "text_renderer.h"
#include "themes.h"

namespace isolabels {

namespace {

constexpr int   kLevelCount = 11;
constexpr float kTargetPx = 16.0f;
constexpr float kSpacingPx = 160.0f;
constexpr float kAdvanceRatio = 0.80f;
constexpr float kOverlapGapPx = 2.0f;

void edgeIntersection(const QVector3D& a, const QVector3D& b, float level, float zShift, QVector<QVector3D>& out)
{
    const float zA = a.z();
    const float zB = b.z();

    if (std::fabs(zA - level) < kmath::fltEps && std::fabs(zB - level) < kmath::fltEps) {
        out << QVector3D(a.x(), a.y(), level + zShift);
        out << QVector3D(b.x(), b.y(), level + zShift);
        return;
    }

    if ((zA - level) * (zB - level) > 0.0f) {
        return;
    }

    if (std::fabs(zA - level) < kmath::fltEps) {
        out << QVector3D(a.x(), a.y(), level + zShift);
        return;
    }

    if (std::fabs(zB - level) < kmath::fltEps) {
        out << QVector3D(b.x(), b.y(), level + zShift);
        return;
    }

    const float t = (level - zA) / (zB - zA);
    if (std::isfinite(t)) {
        QVector3D p = a + t * (b - a);
        p.setZ(level + zShift);
        out << p;
    }
}

void addFromSegment(const QVector3D& p1, const QVector3D& p2, float level, QVector<Label>& out)
{
    QVector3D dir = p2 - p1;
    dir.setZ(0.0f);
    const float len = dir.length();
    if (!std::isfinite(len) || len < kmath::fltEps) {
        return;
    }
    dir /= len;
    QVector3D pos = (p1 + p2) * 0.5f;
    const float depth = std::fabs(level);
    out.append(Label{ pos, dir, depth, QString::number(depth, 'f', 1) });
}

} // namespace

void addFromTriangle(const QVector3D& A, const QVector3D& B, const QVector3D& C,
                     float minZ, float lineStep, int levelCount, float zShift, QVector<Label>& out)
{
    const float maxZ = minZ + (levelCount - 1) * lineStep;
    const float triMin = std::min(A.z(), std::min(B.z(), C.z()));
    const float triMax = std::max(A.z(), std::max(B.z(), C.z()));
    if (triMax < minZ - kmath::fltEps || triMin > maxZ + kmath::fltEps) {
        return;
    }

    int lvlStart = int(std::floor((triMin - minZ) / lineStep));
    int lvlEnd   = int(std::ceil((triMax - minZ) / lineStep));
    if (lvlStart < 0) lvlStart = 0;
    if (lvlEnd > levelCount - 1) lvlEnd = levelCount - 1;

    for (int lvl = lvlStart; lvl <= lvlEnd; ++lvl) {
        const float level = minZ + lvl * lineStep;
        QVector<QVector3D> ip;
        edgeIntersection(A, B, level, zShift, ip);
        edgeIntersection(B, C, level, zShift, ip);
        edgeIntersection(C, A, level, zShift, ip);

        if (ip.size() == 2) {
            addFromSegment(ip[0], ip[1], level, out);
        }
        else if (ip.size() == 3) {
            if (!kmath::fuzzyEq(ip[0], ip[1])) {
                addFromSegment(ip[0], ip[1], level, out);
            }
            if (!kmath::fuzzyEq(ip[1], ip[2])) {
                addFromSegment(ip[1], ip[2], level, out);
            }
        }
    }
}

void assignLevels(const QVector<Label>& in, QVector<Label>& out, float baseSpacing)
{
    out.clear();
    if (in.isEmpty()) {
        return;
    }

    if (!std::isfinite(baseSpacing) || baseSpacing <= 0.0f) {
        out = in;
        return;
    }

    out.reserve(in.size());

    struct AcceptedPoint
    {
        QVector3D pos;
        int isoKey;
    };

    auto isoKeyOf = [](const Label& lbl) {
        return qRound(lbl.depth * 1000.0f);
    };

    QVector<int> pending(in.size());
    for (int i = 0; i < in.size(); ++i) {
        pending[i] = i;
    }

    QVector<AcceptedPoint> accepted;
    accepted.reserve(in.size());

    for (int level = kLevelCount - 1; level >= 0 && !pending.isEmpty(); --level) {
        const float spacing = baseSpacing * static_cast<float>(1 << level);
        const float inv = 1.0f / spacing;
        const float sameSq = spacing * spacing;

        QHash<QPair<int, int>, QVector<AcceptedPoint>> grid;
        grid.reserve(accepted.size() + pending.size());
        for (const auto& pt : accepted) {
            grid[qMakePair(int(std::floor(pt.pos.x() * inv)), int(std::floor(pt.pos.y() * inv)))].append(pt);
        }

        QVector<int> rejected;
        rejected.reserve(pending.size());

        for (const int idx : pending) {
            const Label& lbl = in[idx];
            const int isoKey = isoKeyOf(lbl);
            const int cx = int(std::floor(lbl.pos.x() * inv));
            const int cy = int(std::floor(lbl.pos.y() * inv));

            bool isNear = false;
            for (int dx = -1; dx <= 1 && !isNear; ++dx) {
                for (int dy = -1; dy <= 1 && !isNear; ++dy) {
                    auto it = grid.constFind(qMakePair(cx + dx, cy + dy));
                    if (it == grid.cend()) {
                        continue;
                    }
                    for (const auto& pt : it.value()) {
                        if (pt.isoKey == isoKey && (lbl.pos - pt.pos).lengthSquared() < sameSq) {
                            isNear = true;
                            break;
                        }
                    }
                }
            }

            if (isNear) {
                rejected.append(idx);
                continue;
            }

            Label acceptedLabel = lbl;
            acceptedLabel.level = level;
            out.append(acceptedLabel);

            const AcceptedPoint pt{ lbl.pos, isoKey };
            accepted.append(pt);
            grid[qMakePair(cx, cy)].append(pt);
        }

        pending = std::move(rejected);
    }
}

void render(QOpenGLFunctions* ctx, const QMatrix4x4& mvp,
            const QMap<QString, std::shared_ptr<QOpenGLShaderProgram>>& shaderProgramMap,
            const QVector<Label>& labels, float labelBaseSpacing, float cameraDist, float step)
{
    if (labels.isEmpty()) {
        return;
    }

    ctx->glDisable(GL_DEPTH_TEST);
    const QColor oldCol = TextRenderer::instance().getColor();
    TextRenderer::instance().setColor(QColor::fromRgbF(1.0f, 1.0f, 1.0f));

    const float baseScale = (std::isfinite(cameraDist) && cameraDist > 0.0f)
                            ? cameraDist * 0.0015f
                            : step * 0.20f;
    const float fallbackScale = qBound(0.12f, baseScale, 0.45f) / static_cast<float>(renderScale());

    const QRectF vport = DrawUtils::viewportRect(ctx);
    const float halfVpW = static_cast<float>(vport.width()) * 0.5f;
    const float halfVpH = static_cast<float>(vport.height()) * 0.5f;
    const QVector4D axisXClip = mvp * QVector4D(1.0f, 0.0f, 0.0f, 0.0f);
    const QVector4D axisYClip = mvp * QVector4D(0.0f, 1.0f, 0.0f, 0.0f);
    const float pxPerMetreX = QVector2D(axisXClip.x() * halfVpW, axisXClip.y() * halfVpH).length();
    const float pxPerMetreY = QVector2D(axisYClip.x() * halfVpW, axisYClip.y() * halfVpH).length();
    const float pxPerMetreW = qMax(pxPerMetreX, pxPerMetreY);
    const float glyphPx = static_cast<float>(TextRenderer::instance().getCharPixelHeight());
    const float targetPx = kTargetPx * static_cast<float>(renderScale());
    const float spacingPx = kSpacingPx * static_cast<float>(renderScale());
    const bool screenSized = std::isfinite(pxPerMetreW) && pxPerMetreW > kmath::fltEps && glyphPx > 0.0f;
    const bool levelCulling = screenSized && std::isfinite(labelBaseSpacing) && labelBaseSpacing > 0.0f;

    struct PlacedLabel
    {
        const Label* label;
        float scale;
        QVector2D screenPos;
        float radiusPx;
        int order;
    };

    QVector<PlacedLabel> placed;
    placed.reserve(labels.size());

    constexpr float ndcMargin = 0.2f;
    int order = -1;
    for (const auto& lbl : labels) {
        ++order;
        const QVector4D clip = mvp * QVector4D(lbl.pos, 1.0f);
        const float w = clip.w();
        if (!std::isfinite(w) || std::fabs(w) < kmath::fltEps || w <= 0.0f) {
            continue;
        }

        const float invW = 1.0f / w;
        const float x = clip.x() * invW;
        const float y = clip.y() * invW;
        const float z = clip.z() * invW;
        if (x < -1.0f - ndcMargin || x > 1.0f + ndcMargin ||
            y < -1.0f - ndcMargin || y > 1.0f + ndcMargin ||
            z < -1.0f - ndcMargin || z > 1.0f + ndcMargin) {
            continue;
        }

        if (levelCulling) {
            const float needSpacing = spacingPx * std::fabs(w) / pxPerMetreW;
            int needLevel = 0;
            if (needSpacing > labelBaseSpacing) {
                needLevel = int(std::ceil(std::log2(needSpacing / labelBaseSpacing)));
            }
            needLevel = std::clamp(needLevel, 0, kLevelCount - 1);
            if (lbl.level < needLevel) {
                continue;
            }
        }

        const float scale = screenSized ? (targetPx * std::fabs(w) / pxPerMetreW) / glyphPx
                                        : fallbackScale;

        const float textHeightPx = screenSized ? glyphPx * scale * pxPerMetreW / std::fabs(w) : 0.0f;
        const float textWidthPx = textHeightPx * kAdvanceRatio * static_cast<float>(lbl.text.size());
        const float radiusPx = screenSized
                               ? 0.5f * std::hypot(textWidthPx, textHeightPx) + kOverlapGapPx
                               : 0.0f;

        placed.append(PlacedLabel{ &lbl,
                                   scale,
                                   QVector2D((x + 1.0f) * halfVpW, (y + 1.0f) * halfVpH),
                                   radiusPx,
                                   order });
    }

    std::sort(placed.begin(), placed.end(), [](const PlacedLabel& a, const PlacedLabel& b) {
        if (a.label->level != b.label->level) {
            return a.label->level > b.label->level;
        }
        return a.order < b.order;
    });

    float maxRadiusPx = 0.0f;
    for (const auto& item : placed) {
        maxRadiusPx = qMax(maxRadiusPx, item.radiusPx);
    }

    const float overlapCell = qMax(2.0f * maxRadiusPx, 1.0f);
    const float overlapInv = 1.0f / overlapCell;
    QHash<QPair<int, int>, QVector<QPair<QVector2D, float>>> overlapGrid;
    overlapGrid.reserve(placed.size());

    QVector<TextRenderer::Text3DItem> labelBatch;
    labelBatch.reserve(placed.size());

    for (const auto& item : placed) {
        const int cx = int(std::floor(item.screenPos.x() * overlapInv));
        const int cy = int(std::floor(item.screenPos.y() * overlapInv));

        bool overlaps = false;
        for (int dx = -1; dx <= 1 && !overlaps; ++dx) {
            for (int dy = -1; dy <= 1 && !overlaps; ++dy) {
                auto it = overlapGrid.constFind(qMakePair(cx + dx, cy + dy));
                if (it == overlapGrid.cend()) {
                    continue;
                }
                for (const auto& taken : it.value()) {
                    const float limit = item.radiusPx + taken.second;
                    if ((item.screenPos - taken.first).lengthSquared() < limit * limit) {
                        overlaps = true;
                        break;
                    }
                }
            }
        }

        if (overlaps) {
            continue;
        }

        overlapGrid[qMakePair(cx, cy)].append(qMakePair(item.screenPos, item.radiusPx));
        labelBatch.append(TextRenderer::Text3DItem{QStringView{item.label->text},
                                                   item.scale,
                                                   item.label->pos,
                                                   item.label->dir});
    }

    if (!labelBatch.isEmpty()) {
        TextRenderer::instance().render3DBatch(labelBatch, ctx, mvp, shaderProgramMap);
    }

    TextRenderer::instance().setColor(oldCol);
    ctx->glEnable(GL_DEPTH_TEST);
}

} // namespace isolabels
