#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>
#include <QHash>
#include <QThreadPool>
#include <QVariantMap>

#include "dataset_defs.h"
#include "epoch.h"
#include "surface_tile.h"
#include "math_defs.h"


class ComputeWorker;
class Dataset;
class DataProcessor;
class SurfaceMesh;
class MosaicProcessor
{
public:
    // Источник амплитуды для отрисовки мозайки.
    // Значения должны совпадать с порядком в qml/MosaicExtraSettings.qml (CCombo mosaicSource).
    enum class Source : std::uint8_t {
        Amplitude = 0,  // Echogram::amplitude (сырое)
        SideScan  = 1,  // Echogram::compensated (gain-compensation)
        Tgc       = 2,  // Echogram::tgc (линейный TGC, параметры — Echogram::gTgc*)
    };

    explicit MosaicProcessor(DataProcessor* parent, ComputeWorker* computeWorker);
    ~MosaicProcessor();

    void clear();

    void setDatasetPtr(Dataset* datasetPtr);
    void setSurfaceMeshPtr(SurfaceMesh* surfaceMeshPtr);

    // PROCESSING
    void setChannels(const ChannelId& firstChId, uint8_t firstSubChId, const ChannelId& secondChId, uint8_t secondSubChId);
    void updateDataWrapper(const QVector<int>& indxs, bool batchEmit = false);
    void setLAngleOffset(float val);
    void setRAngleOffset(float val);
    void setTileResolution(float tileResolution);
    void setGenerageGridContour(bool state);
    void setSource(Source source);
    /// "Clear old data": remember, per pixel, the pair (by its older epoch) that painted it last.
    void setTrackOwners(bool on);
    /// Clears every pixel last painted by a pair whose older epoch is below keepFrom and sends the
    /// tiles that changed on, as a paint does. Only tiles that may hold such a pixel are scanned, so
    /// the cost follows what left the window, not its length.
    void eraseOwnersBelow(int keepFrom);
    /// A precomputing instance (MosaicPrecomputer): paints `tiles` only instead of the worker's
    /// visible ones, sends tiles and finished epochs to the data processor's precompute slots tagged
    /// with `generation`, and stops once `latest` has moved past it.
    void setBackground(const QSet<TileKey>* tiles, const std::atomic<quint64>* latest, quint64 generation);
    void setPaintThreads(int n);
    /// KOGGER_MOSAIC_THREADS when set (> 0), else the cores less two, at most 8.
    static int defaultPaintThreads();
    static bool paintThreadsForced();   // KOGGER_MOSAIC_THREADS is set: the settings do not change it

    QPair<ChannelId, uint8_t> getFirstChannelId()  const { return qMakePair(segFChannelId_, segFSubChannelId_); };
    QPair<ChannelId, uint8_t> getSecondChannelId() const { return qMakePair(segSChannelId_, segSSubChannelId_); };

    void fillDiagnostics(QVariantMap& stats, int probeWindow) const;

private:
    void postUpdate(const QSet<SurfaceTile*>& updatedIn, QSet<SurfaceTile*>& changedOut);
    void updateUnmarkedHeightVertices(SurfaceTile* tilePtr) const;
    void updateData(const QVector<int>& indxs, QSet<int>& usedEpochs, QSet<int>& blockedEpochs,
                    QVector<int>* newUsed = nullptr, QVector<int>* newBlocked = nullptr);
    inline int getColorIndx(Epoch::Echogram* charts, int ampIndx) const;
    bool canceled() const noexcept;

    // prepairing tiles
    QSet<TileKey> forecastTilesToTouch(const QVector<QVector3D>& meas, const QVector<char>& isOdds, const QVector<int>& epochIndxs, int marginTiles = 0) const;
    void putTilesIntoMesh(const TileMap& tiles);
    // *cut: cancelled while tiles were still on their way from the DB (they would be seeded blank).
    bool prefetchTiles(const QSet<TileKey>& keys, bool* cut = nullptr);
    QVector<QVector<int>> splitContinuousSegments(const QVector<int>& indxs, int minSegmentLen, int maxSegmentLen);

private:
    const int expandMargin_ = 1;
    ComputeWorker* computeWorker_;
    DataProcessor* dataProcessor_;
    Dataset* datasetPtr_;
    SurfaceMesh* surfaceMeshPtr_;
    kmath::MatrixParams lastMatParams_;
    float tileResolution_;
    int pixOnMeters_;
    int aliasWindow_;
    uint64_t currIndxSec_;
    ChannelId segFChannelId_;
    uint8_t segFSubChannelId_;
    ChannelId segSChannelId_;
    uint8_t segSSubChannelId_;
    int tileSidePixelSize_;
    int tileHeightMatrixRatio_;
    int lastCalcEpoch_;
    int lastAcceptedEpoch_;
    float lAngleOffset_;
    float rAngleOffset_;
    bool generateGridContour_;
    Source source_ = Source::SideScan;
    // When true, updateData buffers per-chunk tile updates into batchedTiles_ instead of
    // pushing each chunk to the renderer immediately. updateDataWrapper flushes once at the
    // end. Eliminates piece-by-piece flicker for multi-chunk batches (e.g. FAKE_COORDS+N
    // full-restart repaints of last-N epochs).
    bool inBatch_ = false;
    TileMap batchedTiles_;
    // Pixel owners for "Clear old data" (setTrackOwners), apart from the tiles so a tile copied to
    // the renderer carries none of it. oldest is a bound, not exact: a pixel painted over is not
    // taken off it until eraseOwnersBelow scans the tile.
    struct OwnedPixels {
        std::vector<int32_t> owner;   // per pixel, -1 for none
        int32_t oldest = std::numeric_limits<int32_t>::max();
    };
    bool trackOwners_ = false;
    QHash<TileKey, OwnedPixels> owners_;
    // updateData paints the pairs of a chunk on this many threads (KOGGER_MOSAIC_THREADS, else the
    // cores less two, at most 8; 1 paints in place as before), each into buffers merged in pair order.
    int paintThreads_ = 1;
    QThreadPool paintPool_;
    // setBackground
    bool background_ = false;
    const QSet<TileKey>* allowedTiles_ = nullptr;
    const std::atomic<quint64>* latestGeneration_ = nullptr;
    quint64 generation_ = 0;
    void postTiles(const TileMap& tiles);
    void reportEpochs(const QVector<int>& indxs, int zoom);
    int lastTraceLineEpoch_;
    QVector3D lastLeftBeg_;
    QVector3D lastLeftEnd_;
    QVector3D lastRightBeg_;
    QVector3D lastRightEnd_;
};
