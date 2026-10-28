#pragma once

#include <vector>
#include <QElapsedTimer>
#include <QMutex>
#include <QVector>
#include <QVector3D>
#include <QString>
#include "surface_tile.h"
#include "scene_object.h"
#include "dataset_defs.h"
#include "isobath_labels.h"


class SurfaceView : public SceneObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(SurfaceView)

public:
    using IsoLabel = isolabels::Label;

    class SurfaceViewRenderImplementation : public SceneObject::RenderImplementation
    {
    public:
        SurfaceViewRenderImplementation();
        void render(QOpenGLFunctions* ctx,
                    const QMatrix4x4& mvp,
                    const QMap<QString,
                    std::shared_ptr<QOpenGLShaderProgram>>& shaderProgramMap) const final;
        void setVerticalScale(float scale);
        void setShadowSettings(bool enabled,
                               const QVector3D& lightDir,
                               float ambient,
                               float intensity,
                               float highlightIntensity) override;
    private:
        using VertexXYKey = QPair<long long, long long>;
        QVector2D projectToScreen(const QVector3D& p, const QMatrix4x4& mvp, const QRect& viewport) const;
        VertexXYKey makeVertexXYKey(const QVector3D& point) const;
        QVector<QVector3D> buildTileNormalSums(const SurfaceTile& tile) const;
        QVector<QVector3D> normalizeNormals(const QVector<QVector3D>& normalSums) const;
        QVector<QVector3D> buildTileNormals(const SurfaceTile& tile) const;
        float depthRangeMin() const;
        float depthRangeMax() const;
        QVector3D samplePalette(float norm) const;
        void rebuildSeamlessTileNormals(const QHash<TileKey, SurfaceTile>& tiles,
                                        QHash<TileKey, QVector<QVector3D>>& outNormals) const;
        // The same for the tiles about `changed` (new content or gone) only: their 3x3 neighbourhood,
        // from the sums of the ring around it. Rebuilding every tile for every delivery of mosaic tiles
        // was a third of the GUI thread after a zoom (2026-10-17).
        void updateSeamlessTileNormals(const QHash<TileKey, SurfaceTile>& tiles,
                                       const QSet<TileKey>& changed,
                                       QHash<TileKey, QVector<QVector3D>>& outNormals) const;
        QVector<QVector3D> seamNormals(const QVector<QVector3D>& verts,
                                       const QVector<QVector3D>& localSums,
                                       const QHash<VertexXYKey, QVector3D>& sharedNormalSums) const;
        void updateBounds() final;

        friend class SurfaceView;

        QHash<TileKey, SurfaceTile> tiles_; // from dataProcessor
        // The level on screen before a zoom, drawn under the new one until it covers them
        // (SurfaceView::keepAsFallback); before 2026-10-17 the view emptied on the new level's first tile.
        QHash<TileKey, SurfaceTile> fallbackTiles_;
        QHash<TileKey, QVector<QVector3D>> fallbackNormals_;
        GLuint surfaceColorTableTextureId_;
        GLuint mosaicColorTableTextureId_;
        float minZ_; // from dataprocessor
        float maxZ_; // from dataprocessor
        float surfaceStep_; // from dataprocessor
        int colorIntervalsSize_; // from dataprocessor
        std::vector<uint8_t> isoPalette_;
        bool iVis_;
        bool mVis_;
        bool bandedColors_;
        QHash<TileKey, QVector<QVector3D>> tileNormals_;
        mutable QHash<TileKey, QVector<QVector3D>> tileNormalSums_;   // buildTileNormalSums per tile, for updateSeamlessTileNormals
        QVector<IsoLabel> isoLabels_;
        float labelStep_;
        float labelBaseSpacing_;
        float cameraDist_;
        QVector<QVector3D> lastLeftLine_;
        QVector<QVector3D> lastRightLine_;
        int   lastTraceEpoch_;
        float traceWidth_;
        bool  traceVisible_;
        float verticalScale_;
    };

    explicit SurfaceView(QObject* parent = nullptr);
    ~SurfaceView() override;

    void   setMosaicTextureIdByTileId(const TileKey& tileId, GLuint textureId);
    void   setMosaicColorTableTextureId(GLuint value);
    void   setSurfaceColorTableTextureId(GLuint textureId);
    void   setIVisible(bool state);
    void   setMVisible(bool state);
    void   setBandedColors(bool state);
    void   setIsobathsLabelStepSize(float val);
    void   setCameraDistToFocusPoint(float val);
    GLuint getMosaicTextureIdByTileId(const TileKey& tileId) const;
    GLuint getMosaicColorTableTextureId() const;
    GLuint getSurfaceColorTableTextureId() const;
    bool   getMVisible() const;
    bool   getIVisible() const;
    int    getRenderTilesCount() const;
    int    getRenderIsoLabelsCount() const;
    float  getRenderSurfaceStep() const;

    QVector<GLuint>                                   takeMosaicTileTextureToDelete();
    QVector<std::pair<TileKey, std::vector<uint8_t>>> takeMosaicTileTextureToAppend();
    std::vector<uint8_t>                              takeMosaicColorTableToAppend();
    GLuint                                            takeMosaicColorTableToDelete();
    std::vector<uint8_t>                              takeSurfaceColorTableToAppend();
    GLuint                                            takeSurfaceColorTableToDelete();

    void setLlaRef(LLARef llaRef);
    bool saveVerticesToFile(const QString& path);

    bool trySetMosaicTextureId(const TileKey& key, GLuint texId);
    bool hasTile(const TileKey& key) const;

    void setTraceLines(const QVector3D& leftBeg, const QVector3D& leftEnd, const QVector3D& rightBeg, const QVector3D& rightEnd, int epochIndex);
    void setTraceVisible(bool state);

public slots: // from dataprocessor
    void clear();
    void setTiles(const QHash<TileKey, SurfaceTile>& tiles, bool useTextures); // TODO: separate (now from mosaic)
    void setTilesIncremental(const QHash<TileKey, SurfaceTile>& tiles, const QSet<TileKey>& fullVisibleNow /*все видимые на кадре*/);
    void setMosaicColorTableTextureTask(const std::vector<uint8_t>& colorTableTextureTask);
    void setMinZ(float minZ);
    void setMaxZ(float maxZ);
    void setSurfaceStep(float surfaceStep);
    void setTextureTask(const std::vector<uint8_t>& textureTask);
    void setColorIntervalsSize(int size);
    void removeTiles(const QSet<TileKey>& ids);

private:
    // KOGGER_MOSAIC_DIGEST=1: once the tiles stop coming for 1.5 s, the log gets a hash of what is on
    // screen (keys, mosaic pixels, heights), to compare two ways of painting the same data.
    void logMosaicDigest();
    class QTimer* digestTimer_ = nullptr;

    // A zoom: the textured tiles of the other level leave `toRemove` for fallbackTiles_ (the level kept
    // before goes); `moved` gets their keys. Each leaves once the new level's visible tiles over it have
    // come and the new level has stopped changing a while (pruneFallback: a tile comes in pieces while
    // its job paints), all of them kFallbackMs after the zoom at the latest.
    void keepAsFallback(QSet<TileKey>& toRemove, int newZoom, QSet<TileKey>& moved);
    void scheduleFallbackPrune(const QSet<TileKey>& fullVisibleNow, int newZoom);
    void pruneFallback(const QSet<TileKey>& fullVisibleNow, int newZoom);
    void dropFallback(const char* why);
public:
    // Settings, Performance: the old level kept under the new one after a zoom (on by default).
    void setZoomFallbackEnabled(bool on);
private:
    bool zoomFallbackEnabled_ = true;
    class QTimer* fallbackTimer_ = nullptr;
    class QTimer* fallbackQuietTimer_ = nullptr;   // the new level unchanged this long: pruneFallback
    QSet<TileKey> fallbackVisible_;                // the new level's visible tiles at its latest change
    QElapsedTimer fallbackClock_;
    int fallbackKept_ = 0;
    int fallbackFromZoom_ = 0;
    int fallbackToZoom_ = 0;
    void updateMosaicTileTextureTask(const QHash<TileKey, SurfaceTile>& newTiles);
    void rebuildIsobathLabels();

private:
    QMutex mosaicTexTasksMutex_;

    static constexpr float kLabelZShift = 0.03f;

    std::vector<uint8_t>                            mosaicColorTableToAppend_;
    GLuint                                          mosaicColorTableToDelete_;
    QHash<TileKey, std::vector<uint8_t>>            mosaicTileTextureToAppend_; // по ключу хранится последнее изображение
    QVector<GLuint>                                 mosaicTileTextureToDelete_;
    std::vector<uint8_t>                            surfaceColorTableToAppend_;
    GLuint                                          surfaceColorTableToDelete_;
    LLARef llaRef_;
};
