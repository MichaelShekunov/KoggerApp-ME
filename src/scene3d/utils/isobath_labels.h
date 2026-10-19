#pragma once

// Depth labels on isobath lines, one way for the live surface (SurfaceView) and for the loaded maps
// ("My maps", MapLayer): a candidate where a line at minZ + k * step crosses a triangle, the candidates
// spread over levels of detail with doubling spacing, and on screen only the levels the zoom asks for,
// without overlaps. Moved out of SurfaceView unchanged (2026-10-18).

#include <memory>
#include <QMap>
#include <QMatrix4x4>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QString>
#include <QVector>
#include <QVector3D>

namespace isolabels {

struct Label
{
    QVector3D pos;
    QVector3D dir;
    float depth;
    QString text;
    int level = 0;
};

/// The lines at z = minZ + k * step, k = 0 .. levelCount - 1, across the triangle a b c: a candidate
/// for each piece of line, at its middle raised by zShift, along it, its text the depth to 0.1 m.
void addFromTriangle(const QVector3D& a, const QVector3D& b, const QVector3D& c,
                     float minZ, float step, int levelCount, float zShift, QVector<Label>& out);

/// Levels of detail: at level L the labels of one isobath stand at least baseSpacing * 2^L scene
/// metres apart; `out` gets every candidate with the highest level it keeps.
void assignLevels(const QVector<Label>& in, QVector<Label>& out, float baseSpacing);

/// Draws in white, over everything, the labels whose level the zoom asks for and that do not overlap
/// on screen. cameraDist and step size the text where the screen scale cannot be had.
void render(QOpenGLFunctions* ctx, const QMatrix4x4& mvp,
            const QMap<QString, std::shared_ptr<QOpenGLShaderProgram>>& programs,
            const QVector<Label>& labels, float baseSpacing, float cameraDist, float step);

} // namespace isolabels
