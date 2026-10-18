#pragma once

// "My maps" in the 3D scene (docs/map-export.md): the visible KOGGER maps under the boat -- depths as
// a mesh at their depth, coloured in bands with isobath lines by the surface view's own shader
// ("isobaths") and labelled as the surface is (isobath_labels.h), and mosaics as textured pieces by
// the mosaic shader ("mosaic"). The isobath step and colour theme are the isobath tool's
// (IsobathsViewControlMenuController, through setIsobathStyle), not the maker's: one depth scale for
// all the visible maps, so maps side by side agree, its lines at whole multiples of the step. The
// mosaics likewise take the mosaic tool's theme and levels (MosaicViewControlMenuController, through
// setMosaicStyle): a .kmap keeps the raw echo intensity, 0 = none, as the view's tiles do, and the
// colours are the live mosaic's table (mosaic::PlotColorTable), not the maker's. Kept in lat/lon and
// projected into the view's frame (GraphicsScene3dView::Camera::viewLlaRef_, spherical in
// perspective, Mercator in the far top-down view) whenever that frame changes, as the boat's own
// positions are: the map and the boat agree where the frame is the dataset's.
//
// MapLayer lives on the GUI thread and makes immutable frames; MapLayerGl lives in the renderer and
// draws the latest frame, uploading its textures once per key.

#include <memory>
#include <QHash>
#include <QImage>
#include <QMap>
#include <QMatrix4x4>
#include <QObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <QVector2D>
#include <QVector3D>
#include "dataset_defs.h"
#include "isobath_labels.h"

class MapLibrary;

struct MapLayerFrame {
    struct Mesh {                   // a map's depths
        QVector<QVector3D> pos;     // scene metres, z = -depth
        QVector<QVector3D> nrm;     // slope, steepened: the shader draws isobath lines only on slopes
        QVector<GLuint> idx;
        float depthMin = 0.0f;      // z of the deepest band's bottom: -(the deepest depth, rounded up to a multiple of the step)
        float step = 1.0f;
        int levels = 1;
        QString paletteKey;
    };
    struct Piece {                  // a piece of a map's mosaic
        QVector<QVector3D> pos;
        QVector<QVector2D> uv;
        QVector<GLuint> idx;
        QString textureKey;         // the 8-bit index picture
        QString paletteKey;         // 256 x 1 colours
    };
    QVector<Mesh> meshes;
    QVector<Piece> pieces;
    QVector<isolabels::Label> labels;  // the isobaths' depths, while the mosaic is not shown (as SurfaceView)
    float labelSpacing = 0.0f;         // isolabels::assignLevels' base spacing, m
    float labelStep = 1.0f;
    QHash<QString, QImage> textures;   // key -> picture (Grayscale8 indices, or RGBA colours)
};

class MapLayer : public QObject
{
    Q_OBJECT
    // QML "mapLayer": the layer on or off, and what of the maps it shows; kept between runs.
    Q_PROPERTY(bool visible READ visible WRITE setVisible NOTIFY settingsChanged)
    Q_PROPERTY(bool depthVisible READ depthVisible WRITE setDepthVisible NOTIFY settingsChanged)
    Q_PROPERTY(bool mosaicVisible READ mosaicVisible WRITE setMosaicVisible NOTIFY settingsChanged)
public:
    explicit MapLayer(QObject* parent = nullptr);

    void setLibrary(MapLibrary* library);
    void setVisible(bool on);
    void setDepthVisible(bool on);
    void setMosaicVisible(bool on);
    /// The isobath tool's colour theme (IsobathUtils::colorPalette) and step, m; 0 or less: a step
    /// that gives at most ~25 lines over the maps' depths (mapexport::autoIsobathStep).
    void setIsobathStyle(int themeId, float stepM);
    /// The mosaic tool's colour theme (mosaic::PlotColorTable's id) and levels.
    void setMosaicStyle(int themeId, float lowLevel, float highLevel);
    bool visible() const { return visible_; }
    bool depthVisible() const { return depthVisible_; }
    bool mosaicVisible() const { return mosaicVisible_; }

    /// The frame for this view frame and projection; made again only when they or the maps changed.
    std::shared_ptr<const MapLayerFrame> frameFor(const LLARef& viewRef, bool perspective);

signals:
    void changed();
    void settingsChanged();

private:
    void saveSettings() const;
    // A map in lat/lon, made once when the library changes.
    struct GeoMesh {
        QVector<double> lat, lon;
        QVector<float> depth;
        QVector<QVector3D> nrm;
        QVector<GLuint> idx;
        double minDepth = 0.0, maxDepth = 0.0;
        double lat0 = 0.0, lon0 = 0.0;       // a local frame in metres for the labels: north = (lat - lat0) * mLat
        double mLat = 1.0, mLon = 1.0;
    };
    struct GeoLabel {
        double lat, lon;
        isolabels::Label label;              // pos.z() its height; pos.x(), pos.y() unused
    };
    struct GeoPiece {
        QVector<double> lat, lon;
        QVector<QVector2D> uv;
        QVector<GLuint> idx;
        QString textureKey, paletteKey;
    };
    void rebuildGeo();
    void rebuildStyle();   // the depth scale, its palette and the labels, from the meshes and the tool's style
    void rebuildMosaicStyle();   // the mosaics' colour table, from the mosaic tool's style

    QPointer<MapLibrary> library_;
    bool visible_ = true;
    bool depthVisible_ = true;
    bool mosaicVisible_ = false;
    bool geoDirty_ = true;
    QVector<GeoMesh> geoMeshes_;
    QVector<GeoPiece> geoPieces_;
    QVector<GeoLabel> geoLabels_;
    int themeId_ = 1;
    float toolStep_ = 0.0f;
    float styleStep_ = 1.0f;          // what is drawn: the tool's, doubled while the palette would be too long
    float styleDepthMin_ = 0.0f;
    int styleLevels_ = 1;
    QString stylePaletteKey_;
    int mosaicTheme_ = 0;             // PlotColorTable's default (Classic) until the tool says
    float mosaicLow_ = 10.0f, mosaicHigh_ = 90.0f;
    QString mosaicPaletteKey_;
    QHash<QString, QImage> textures_;
    std::shared_ptr<const MapLayerFrame> frame_;
    double frameLat_ = 0.0, frameLon_ = 0.0;
    bool framePerspective_ = true;
};

/// The renderer's side: draws a frame, keeps its textures (render thread only).
class MapLayerGl
{
public:
    void setFrame(std::shared_ptr<const MapLayerFrame> frame) { frame_ = std::move(frame); }
    void render(QOpenGLFunctions* ctx, const QMatrix4x4& mvp,
                const QMap<QString, std::shared_ptr<QOpenGLShaderProgram>>& programs);
private:
    void syncTextures(QOpenGLFunctions* ctx);
    std::shared_ptr<const MapLayerFrame> frame_;
    std::shared_ptr<const MapLayerFrame> uploadedFor_;
    QHash<QString, GLuint> textures_;
};
