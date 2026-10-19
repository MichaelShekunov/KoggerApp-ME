#include "mosaic_processor.h"

#include <QtMath>
#include <QDebug>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>
#include <functional>
#include <limits>
#include <memory>
#include <queue>
#include <unordered_map>
#include "data_processor.h"
#include "data_processor_defs.h"
#include "dataset.h"
#include "compute_worker.h"
#include "mosaic_arc.h"


static constexpr int sampleLimiter    = 2;
static constexpr int colorTableSize_  = 255;
static constexpr float kMaxArcTurnRad  = 1.0471976f; // 60 deg: a bigger turn between two pings is a heading glitch

static bool checkLength(float dist)
{
    return !qFuzzyIsNull(dist) && dist >= 0.0f;
}

static int sampleIndex(Epoch::Echogram *echogramPtr, float dist)
{
    if (!echogramPtr) {
        return -1;
    }

    const float resolution = echogramPtr->resolution;

    if (resolution <= 0.0f) {
        return -1;
    }

    float resDist = dist / resolution;
    if (resDist < 0.f) {
        return -1;
    }

    int indx = static_cast<int>(std::round(resDist));
    if (indx >= static_cast<int>(echogramPtr->amplitude.size()) - sampleLimiter) {
        return -1;
    }

    return indx;
}

MosaicProcessor::MosaicProcessor(DataProcessor* parent, ComputeWorker* computeWorker)
    : computeWorker_(computeWorker),
    dataProcessor_(parent),
    datasetPtr_(nullptr),
    surfaceMeshPtr_(nullptr),
    tileResolution_(defaultTileResolution),
    pixOnMeters_(std::pow(tileResolution_, -1)),
    aliasWindow_(100/pixOnMeters_),
    currIndxSec_(0),
    segFSubChannelId_(0),
    segSSubChannelId_(0),
    tileSidePixelSize_(defaultTileSidePixelSize),
    tileHeightMatrixRatio_(defaultTileHeightMatrixRatio),
    lastCalcEpoch_(0),
    lastAcceptedEpoch_(0),
    lAngleOffset_(0.0f),
    rAngleOffset_(0.0f),
    generateGridContour_(false),
    lastTraceLineEpoch_(-1)
{
    qRegisterMetaType<TileMap>("TileMap");

    // The cores less two (the GUI and the data processor threads), at most 8; the worker's own thread
    // paints one run, the pool the others.
    paintThreads_ = defaultPaintThreads();
    paintPool_.setMaxThreadCount(std::max(1, paintThreads_ - 1));
    paintPool_.setExpiryTimeout(-1);
}

MosaicProcessor::~MosaicProcessor()
{
}

void MosaicProcessor::clear()
{
    //qDebug() << "MosaicProcessor::clear";

    lastMatParams_ = kmath::MatrixParams();
    currIndxSec_ = 0;

    lastCalcEpoch_ = 0;
    lastAcceptedEpoch_ = 0;
    lastTraceLineEpoch_ = -1;
    owners_.clear();
}

void MosaicProcessor::setTrackOwners(bool on)
{
    trackOwners_ = on;
    if (!on) {
        owners_.clear();
        owners_.squeeze();
    }
}

void MosaicProcessor::eraseOwnersBelow(int keepFrom)
{
    if (!trackOwners_ || owners_.isEmpty() || !surfaceMeshPtr_ || !surfaceMeshPtr_->getIsInited()) {
        return;
    }

    // By the owners, not by walking the pairs again: a walk depends on the bottom track, and that
    // can change after the paint.
    QVector<TileKey> due;
    QSet<TileKey> evicted;
    for (auto it = owners_.cbegin(); it != owners_.cend(); ++it) {
        if (it.value().oldest >= keepFrom) {
            continue;
        }
        due.append(it.key());
        if (const auto* t = surfaceMeshPtr_->getTilePtrByKey(it.key()); t && !t->getIsInited()) {
            evicted.insert(it.key());
        }
    }
    if (due.isEmpty()) {
        return;
    }
    if (!evicted.isEmpty()) {   // let go by the mesh (LRU): back from the hot cache, as prefetchTiles does
        TileMap got;
        QSet<TileKey> missing;
        if (QMetaObject::invokeMethod(dataProcessor_, "fetchFromHotCache", Qt::BlockingQueuedConnection,
                                      Q_RETURN_ARG(TileMap, got), Q_ARG(QSet<TileKey>, evicted), Q_ARG(QSet<TileKey>*, &missing))) {
            putTilesIntoMesh(got);
        }
    }

    TileMap res;
    for (const TileKey& key : std::as_const(due)) {
        auto it = owners_.find(key);
        if (it == owners_.end()) {
            continue;
        }
        auto& owner = it.value().owner;
        SurfaceTile* t = surfaceMeshPtr_->getTilePtrByKey(key);
        if (!t || !t->getIsInited() || t->getMosaicImageDataRef().size() != owner.size()) {
            owners_.erase(it);   // gone from the mesh and the cache: nothing left to clear
            continue;
        }
        uint8_t* img = t->getMosaicImageDataRef().data();
        auto& quality = t->getMosaicQualityRef();
        uint8_t* q = quality.size() == owner.size() ? quality.data() : nullptr;
        int32_t oldest = std::numeric_limits<int32_t>::max();
        bool changed = false;
        for (size_t p = 0; p < owner.size(); ++p) {
            const int32_t o = owner[p];
            if (o < 0) {
                continue;
            }
            if (o < keepFrom) {
                owner[p] = -1;
                img[p] = 0;
                if (q) {
                    q[p] = 0;
                }
                changed = true;
            }
            else if (o < oldest) {
                oldest = o;
            }
        }
        if (oldest == std::numeric_limits<int32_t>::max()) {
            owners_.erase(it);
        }
        else {
            it.value().oldest = oldest;
        }
        if (changed) {
            res.insert(key, *t);
        }
    }

    if (!res.isEmpty()) {
        postTiles(res);
    }
}

void MosaicProcessor::setDatasetPtr(Dataset *datasetPtr)
{
    datasetPtr_ = datasetPtr;
}

void MosaicProcessor::setSurfaceMeshPtr(SurfaceMesh *surfaceMeshPtr)
{
    surfaceMeshPtr_ = surfaceMeshPtr;
}

void MosaicProcessor::setChannels(const ChannelId& firstChId, uint8_t firstSubChId, const ChannelId& secondChId, uint8_t secondSubChId)
{
    //qDebug() << "MosaicProcessor::setChannels" << firstChId.toShortName() << firstSubChId << secondChId.toShortName() << secondSubChId;

    segFChannelId_    = firstChId;
    segFSubChannelId_ = firstSubChId;
    segSChannelId_    = secondChId;
    segSSubChannelId_ = secondSubChId;
}

void MosaicProcessor::updateDataWrapper(const QVector<int>& indxs, bool batchEmit)
{
    //qDebug() << "";
    //qDebug() << "MosaicProcessor::updateDataWrapper" << indxs;

    if (!datasetPtr_ || indxs.isEmpty()) {
        return;
    }

    if (!background_ && !sink_) {
        QMetaObject::invokeMethod(dataProcessor_, "postState", Qt::QueuedConnection, Q_ARG(DataProcessorType, DataProcessorType::kMosaic));
    }

    // Batch chunk emissions only when the caller opted in (FAKE_COORDS+N radical repaints).
    // Default off preserves the original incremental streaming behavior for real-time paint.
    inBatch_ = batchEmit;
    if (inBatch_) {
        batchedTiles_.clear();
    }

    // чанкование задачи
    // A big job (a file, a zoom) in chunks of 64: painted on several threads, a chunk of 10 is three
    // pairs a thread and the buffers and the merge cost more than they save. Live, 10 as before.
    const int kStep = (paintThreads_ > 1 && indxs.size() >= 128) ? 64 : 10;
    const int kProgressStep = 100; // batch sending completed indxs
    QSet<int> usedEpochs;
    QSet<int> blockedEpochs;
    QSet<int> reportedEpochs;
    QSet<int> pendingUsedSet;
    std::priority_queue<int, std::vector<int>, std::greater<int>> pendingUsedMin;
    QVector<int> progressBatch;
    progressBatch.reserve(static_cast<size_t>(kProgressStep) * static_cast<size_t>(2));
    QVector<int> chunk;

    const int progressZoom = zoomFromMpp(tileResolution_);

    auto emitProgress = [&](bool force) {
        if (progressBatch.isEmpty()) {
            return;
        }
        if (!force && progressBatch.size() < kProgressStep) {
            return;
        }
        // A precompute reports what it posted even when cancelled: the data processor keeps or drops
        // the two together (DataProcessor::cancelPrecompute's keepProduced), and painted tiles
        // without their epochs could not be finished later (the head index rejects them again).
        if (canceled() && !background_) {
            return;
        }

        std::sort(progressBatch.begin(), progressBatch.end());
        reportEpochs(progressBatch, progressZoom);
        for (int idx : std::as_const(progressBatch)) {
            reportedEpochs.insert(idx);
        }
        progressBatch.clear();
    };

    auto drainSafeProgress = [&](int cutoff) {
        while (!pendingUsedMin.empty() && pendingUsedMin.top() <= cutoff) {
            const int idx = pendingUsedMin.top();
            pendingUsedMin.pop();
            if (!pendingUsedSet.contains(idx)) {
                continue;
            }
            pendingUsedSet.remove(idx);
            if (blockedEpochs.contains(idx) || reportedEpochs.contains(idx)) {
                continue;
            }
            progressBatch.append(idx);
        }
        emitProgress(false);
    };

    auto expandAndUpdate = [&]() {
        if (chunk.empty()) {
            return;
        }

        const int firstIndx = chunk.front();
        QVector<int> expandedChunk = { firstIndx - 3, firstIndx - 2, firstIndx - 1 };
        expandedChunk.reserve(chunk.size() + 3);

        for (const auto& itm : std::as_const(chunk)) {
            expandedChunk.push_back(itm);
        }

        QVector<int> newUsed;
        QVector<int> newBlocked;
        updateData(expandedChunk, usedEpochs, blockedEpochs, &newUsed, &newBlocked);

        for (int idx : std::as_const(newBlocked)) {
            pendingUsedSet.remove(idx);
        }
        for (int idx : std::as_const(newUsed)) {
            if (blockedEpochs.contains(idx) || reportedEpochs.contains(idx)) {
                continue;
            }
            if (pendingUsedSet.contains(idx)) {
                continue;
            }
            pendingUsedSet.insert(idx);
            pendingUsedMin.push(idx);
        }
    };

    int prev = 0;
    bool chunkHasNew = false;
    for (int idx : std::as_const(indxs)) {
        if (!chunk.isEmpty() && idx != prev + 1) {
            if (chunkHasNew) {
                expandAndUpdate();
                drainSafeProgress(prev - 3);
                if (canceled()) {
                    break;
                }
            }
            chunk.clear();
        }
        chunk.push_back(idx);
        chunkHasNew = true;
        prev = idx;
        if (chunk.size() >= kStep) {
            expandAndUpdate();
            drainSafeProgress(prev - 3);
            if (canceled()) {
                break;
            }
            const int lastIdx = chunk.back();
            chunk.clear();
            chunk.push_back(lastIdx);
            chunkHasNew = false;
        }
    }
    if (!canceled() && chunkHasNew && !chunk.isEmpty()) {
        expandAndUpdate();
        drainSafeProgress(prev - 3);
    }

    // TODO
    // constexpr int minSegmentLen = 2;
    // constexpr int maxSegmentLen = 100;
    // const auto segments = splitContinuousSegments(indxs, minSegmentLen, maxSegmentLen);
    // for (const auto& seg : segments) {
    //     updateData(seg);
    // }

    drainSafeProgress(std::numeric_limits<int>::max());
    emitProgress(true);

    if (!blockedEpochs.isEmpty()) {
        usedEpochs.subtract(blockedEpochs);
    }
    if (!reportedEpochs.isEmpty()) {
        usedEpochs.subtract(reportedEpochs);
    }

    if (!usedEpochs.isEmpty() && (!canceled() || background_)) {
        QVector<int> usedVec;
        usedVec.reserve(usedEpochs.size());
        for (int idx : std::as_const(usedEpochs)) {
            usedVec.append(idx);
        }
        std::sort(usedVec.begin(), usedVec.end());
        const int zoom = zoomFromMpp(tileResolution_);
        reportEpochs(usedVec, zoom);
    }

    // Flush all accumulated chunks as a single tile delta — renderer transitions in one shot.
    inBatch_ = false;
    if (!batchedTiles_.isEmpty()) {
        postTiles(batchedTiles_);
        batchedTiles_.clear();
    }

    if (!background_ && !sink_) {
        QMetaObject::invokeMethod(dataProcessor_, "postState", Qt::QueuedConnection, Q_ARG(DataProcessorType, DataProcessorType::kUndefined));
    }
}

void MosaicProcessor::setBackground(const QSet<TileKey>* tiles, const std::atomic<quint64>* latest, quint64 generation)
{
    background_ = true;
    allowedTiles_ = tiles;
    latestGeneration_ = latest;
    generation_ = generation;
}

void MosaicProcessor::setSink(MosaicTileSink* sink)
{
    sink_ = sink;
}

int MosaicProcessor::defaultPaintThreads()
{
    const int forced = qEnvironmentVariableIntValue("KOGGER_MOSAIC_THREADS");
    return forced > 0 ? forced : std::clamp(QThread::idealThreadCount() - 2, 1, 8);
}

bool MosaicProcessor::paintThreadsForced()
{
    return qEnvironmentVariableIntValue("KOGGER_MOSAIC_THREADS") > 0;
}

void MosaicProcessor::setPaintThreads(int n)
{
    paintThreads_ = std::max(1, n);
    paintPool_.setMaxThreadCount(std::max(1, paintThreads_ - 1));
}

void MosaicProcessor::postTiles(const TileMap& tiles)
{
    if (sink_) {
        sink_->putTiles(tiles);
        return;
    }
    if (background_) {   // into the cache only, and only if this run is still the one wanted
        QMetaObject::invokeMethod(dataProcessor_, "postPrecomputedTiles", Qt::QueuedConnection,
                                  Q_ARG(TileMap, tiles), Q_ARG(quint64, generation_));
        return;
    }
    QMetaObject::invokeMethod(dataProcessor_, "postSurfaceTiles", Qt::QueuedConnection, Q_ARG(TileMap, tiles), Q_ARG(bool, true));
}

void MosaicProcessor::reportEpochs(const QVector<int>& indxs, int zoom)
{
    if (sink_) {
        return;
    }
    if (background_) {
        QMetaObject::invokeMethod(dataProcessor_, "onMosaicEpochsPrecomputed", Qt::QueuedConnection,
                                  Q_ARG(QVector<int>, indxs), Q_ARG(int, zoom), Q_ARG(quint64, generation_));
        return;
    }
    QMetaObject::invokeMethod(dataProcessor_, "onMosaicEpochsProcessed", Qt::QueuedConnection,
                              Q_ARG(QVector<int>, indxs), Q_ARG(int, zoom));
}

void MosaicProcessor::setLAngleOffset(float val)
{
    if (val < -90.0f || val > 90.0f) {
        return;
    }

    lAngleOffset_ = val;
}

void MosaicProcessor::setRAngleOffset(float val)
{
    if (val < -90.0f || val > 90.0f) {
        return;
    }

    rAngleOffset_ = val;
}

void MosaicProcessor::setTileResolution(float tileResolution)
{
    owners_.clear();   // keyed by the tiles of the old zoom
    tileResolution_ = tileResolution;
    pixOnMeters_ = std::pow(tileResolution_, -1);
    aliasWindow_ = 100 / pixOnMeters_;
}

void MosaicProcessor::setSource(Source source)
{
    source_ = source;
}

void MosaicProcessor::setBeamModel(std::shared_ptr<const mosaic_beam::Model> model)
{
    beam_ = std::move(model);
}

void MosaicProcessor::setPaintRules(const mosaic_beam::PaintRules& rules)
{
    rules_ = rules;
    rules_.prepare();
}

void MosaicProcessor::fillDiagnostics(QVariantMap& stats, int probeWindow) const
{
    stats["mosaicFirstChannel"]  = segFChannelId_.isValid() ? segFChannelId_.toShortName() : QString();
    stats["mosaicFirstSub"]      = static_cast<int>(segFSubChannelId_);
    stats["mosaicSecondChannel"] = segSChannelId_.isValid() ? segSChannelId_.toShortName() : QString();
    stats["mosaicSecondSub"]     = static_cast<int>(segSSubChannelId_);
    stats["mosaicSource"]        = static_cast<int>(source_);
    stats["mosaicLAngleOffset"]  = lAngleOffset_;
    stats["mosaicRAngleOffset"]  = rAngleOffset_;
    stats["mosaicTileResolution"] = tileResolution_;
    stats["mosaicLastCalcEpoch"] = lastCalcEpoch_;
    stats["mosaicLastAccepted"]  = lastAcceptedEpoch_;
    stats["mosaicLastTraceLine"] = lastTraceLineEpoch_;

    if (!datasetPtr_) {
        stats["probeChecked"] = 0;
        return;
    }

    const auto probes = datasetPtr_->probeMosaicEpochs(probeWindow,
                                                       segFChannelId_, segFSubChannelId_,
                                                       segSChannelId_, segSSubChannelId_);

    int valid = 0;
    int withPos = 0;
    int withYaw = 0;
    int firstBeam = 0;
    int secondBeam = 0;
    int firstBottom = 0;
    int secondBottom = 0;
    int usableFirst = 0;
    int usableSecond = 0;
    int pairsFirst = 0;
    int pairsSecond = 0;
    bool prevUsableFirst = false;
    bool prevUsableSecond = false;

    for (const auto& probe : probes) {
        if (probe.valid) {
            ++valid;
        }
        if (probe.posFinite) {
            ++withPos;
        }
        if (probe.yawFinite) {
            ++withYaw;
        }
        if (probe.firstBeam) {
            ++firstBeam;
        }
        if (probe.secondBeam) {
            ++secondBeam;
        }
        if (probe.firstBottom) {
            ++firstBottom;
        }
        if (probe.secondBottom) {
            ++secondBottom;
        }

        const bool geoOk = probe.valid && probe.posFinite && probe.yawFinite;
        const bool okFirst = geoOk && probe.firstBottom;
        const bool okSecond = geoOk && probe.secondBottom;

        if (okFirst) {
            ++usableFirst;
            if (prevUsableFirst) {
                ++pairsFirst;
            }
        }
        if (okSecond) {
            ++usableSecond;
            if (prevUsableSecond) {
                ++pairsSecond;
            }
        }

        prevUsableFirst = okFirst;
        prevUsableSecond = okSecond;
    }

    stats["probeChecked"]      = probes.size();
    stats["probeValid"]        = valid;
    stats["probeWithPos"]      = withPos;
    stats["probeWithYaw"]      = withYaw;
    stats["probeFirstBeam"]    = firstBeam;
    stats["probeSecondBeam"]   = secondBeam;
    stats["probeFirstBottom"]  = firstBottom;
    stats["probeSecondBottom"] = secondBottom;
    stats["probeUsableFirst"]  = usableFirst;
    stats["probeUsableSecond"] = usableSecond;
    stats["probePairsFirst"]   = pairsFirst;
    stats["probePairsSecond"]  = pairsSecond;
}

void MosaicProcessor::setGenerageGridContour(bool state)
{
    generateGridContour_ = state;
}

// Шьём только ВВЕРХ и ВЛЕВО и в тот угол
// Причина: у каждого шва/угла ровно один владелец, это исключает двойную запись, гарантируя одинаковые значения по обе стороны шва
void MosaicProcessor::postUpdate(const QSet<SurfaceTile*>& updatedIn, QSet<SurfaceTile*>& changedOut)
{
    if (!surfaceMeshPtr_ || !surfaceMeshPtr_->getIsInited() || updatedIn.isEmpty()) {
        return;
    }

    auto& matrix = surfaceMeshPtr_->getTileMatrixRef();
    const int tilesY = matrix.size();
    if (tilesY == 0) {
        return;
    }

    const int tilesX = matrix.at(0).size();
    if (tilesX == 0) {
        return;
    }

    // helpers
    auto findIJ = [&](SurfaceTile* t, int& oi, int& oj)->bool {
        for (int i = 0; i < tilesY; ++i) {
            for (int j = 0; j < tilesX; ++j) {
                if (matrix[i][j] == t) { oi = i; oj = j; return true; }
            }
        }
        return false;
    };
    auto copyRow = [&](QVector<QVector3D>& from,
                       const QVector<HeightType>& mFrom, int fromRow,
                       QVector<QVector3D>& to, QVector<HeightType>& mTo,
                       int toRow, int hvSide)
    {
        const int total = from.size();
        if (total == 0 || to.size() != total || mTo.size() != total || mFrom.size() != total) {
            return;
        }
        const int fromStart = fromRow * hvSide;
        const int toStart   = toRow   * hvSide;
        for (int k = 0; k < hvSide; ++k) {
            const int si = fromStart + k;
            const int di = toStart   + k;
            if (!qFuzzyIsNull(from[si][2])) {
                const auto srcMark = mFrom[si];
                if (!canOverwriteHeight(srcMark, mTo[di])) {
                    continue;
                }
                to[di][2] = from[si][2];
                mTo[di]   = srcMark;
            }
        }
    };
    auto copyCol = [&](QVector<QVector3D>& from,
                       const QVector<HeightType>& mFrom, int fromCol,
                       QVector<QVector3D>& to, QVector<HeightType>& mTo,
                       int toCol, int hvSide)
    {
        const int total = from.size();
        if (total == 0 || to.size() != total || mTo.size() != total || mFrom.size() != total) {
            return;
        }
        for (int k = 0; k < hvSide; ++k) {
            const int si = k * hvSide + fromCol;
            const int di = k * hvSide + toCol;
            if (!qFuzzyIsNull(from[si][2])) {
                const auto srcMark = mFrom[si];
                if (!canOverwriteHeight(srcMark, mTo[di])) {
                    continue;
                }
                to[di][2] = from[si][2];
                mTo[di]   = srcMark;
            }
        }
    };
    auto copyCorner = [&](QVector<QVector3D>& from, const QVector<HeightType>& mFrom, int si,
                          QVector<QVector3D>& to,   QVector<HeightType>& mTo,   int di)
    {
        if (si < 0 || si >= from.size() || si >= mFrom.size() || di < 0 || di >= to.size() || di >= mTo.size()) {
            return;
        }
        if (!qFuzzyIsNull(from[si][2])) {
            const auto srcMark = mFrom[si];
            if (!canOverwriteHeight(srcMark, mTo[di])) {
                return;
            }
            to[di][2] = from[si][2];
            mTo[di]   = srcMark;
        }
    };

    for (SurfaceTile* tile : updatedIn) {
        if (!tile || !tile->getIsUpdated()) {
            continue;
        }

        int i = -1, j = -1;
        if (!findIJ(tile, i, j)) {
            continue;
        }

        auto& vSrc = tile->getHeightVerticesRef();
        auto& mSrc = tile->getHeightMarkVerticesRef();
        const int n = vSrc.size();
        const int hvSide = int(std::sqrt(double(n)));
        if (hvSide * hvSide != n || hvSide <= 1) {
            continue;
        }

        // Индексы углов текущего тайла
        const int TL = 0;
        // const int TR = hvSide - 1;
        // const int BL = hvSide * (hvSide - 1);
        const int BR = hvSide * hvSide - 1;

        // 4 стороны
        // TOP: текущая верхняя строка -> нижняя строка top-соседа (i+1, j)
        if (i + 1 < tilesY) {
            SurfaceTile* top = matrix[i + 1][j];
            if (top && top->getIsInited()) {
                auto& vTop = top->getHeightVerticesRef();
                auto& mTop = top->getHeightMarkVerticesRef();
                copyRow(vSrc, mSrc,
                        /*fromRow=*/0, vTop, mTop,
                        /*toRow=*/hvSide - 1, hvSide);
                top->setIsUpdated(true);
                changedOut.insert(top);
            }
        }
        // // BOTTOM: текущая нижняя строка -> верхняя строка bottom-соседа (i-1, j)
        // if (i - 1 >= 0) {
        //     SurfaceTile* bottom = matrix[i - 1][j];
        //     if (bottom && bottom->getIsInited()) {
        //         auto& vBtm = bottom->getHeightVerticesRef();
        //         auto& mBtm = bottom->getHeightMarkVerticesRef();
        //         copyRow(vSrc,
        //                 /*fromRow=*/hvSide - 1, vBtm, mBtm,
        //                 /*toRow=*/0, hvSide);
        //         bottom->setIsUpdated(true);
        //         changedOut.insert(bottom);
        //     }
        // }
        // LEFT: текущий левый столбец -> правый столбец left-соседа (i, j-1)
        if (j - 1 >= 0) {
            SurfaceTile* left = matrix[i][j - 1];
            if (left && left->getIsInited()) {
                auto& vLeft = left->getHeightVerticesRef();
                auto& mLeft = left->getHeightMarkVerticesRef();
                copyCol(vSrc, mSrc,
                        /*fromCol=*/0, vLeft, mLeft,
                        /*toCol=*/hvSide - 1, hvSide);
                left->setIsUpdated(true);
                changedOut.insert(left);
            }
        }

        // // RIGHT: текущий правый столбец -> левый столбец right-соседа (i, j+1)
        // if (j + 1 < tilesX) {
        //     SurfaceTile* right = matrix[i][j + 1];
        //     if (right && right->getIsInited()) {
        //         auto& vRight = right->getHeightVerticesRef();
        //         auto& mRight = right->getHeightMarkVerticesRef();
        //         copyCol(vSrc,
        //                 /*fromCol=*/hvSide - 1, vRight, mRight,
        //                 /*toCol=*/0, hvSide);
        //         right->setIsUpdated(true);
        //         changedOut.insert(right);
        //     }
        // }

        // 4 угла
        // TOP-LEFT (i+1, j-1): TL -> BR
        if (i + 1 < tilesY && j - 1 >= 0) {
            SurfaceTile* diag = matrix[i + 1][j - 1];
            if (diag && diag->getIsInited()) {
                auto& vD = diag->getHeightVerticesRef();
                auto& mD = diag->getHeightMarkVerticesRef();
                copyCorner(vSrc, mSrc, TL, vD, mD, BR);
                diag->setIsUpdated(true);
                changedOut.insert(diag);
            }
        }
        // // TOP-RIGHT (i+1, j+1): TR -> BL
        // if (i + 1 < tilesY && j + 1 < tilesX) {
        //     SurfaceTile* diag = matrix[i + 1][j + 1];
        //     if (diag && diag->getIsInited()) {
        //         auto& vD = diag->getHeightVerticesRef();
        //         auto& mD = diag->getHeightMarkVerticesRef();
        //         copyCorner(vSrc, TR, vD, mD, BL);
        //         diag->setIsUpdated(true);
        //         changedOut.insert(diag);
        //     }
        // }
        // // BOTTOM-LEFT (i-1, j-1): BL -> TR
        // if (i - 1 >= 0 && j - 1 >= 0) {
        //     SurfaceTile* diag = matrix[i - 1][j - 1];
        //     if (diag && diag->getIsInited()) {
        //         auto& vD = diag->getHeightVerticesRef();
        //         auto& mD = diag->getHeightMarkVerticesRef();
        //         copyCorner(vSrc, BL, vD, mD, TR);
        //         diag->setIsUpdated(true);
        //         changedOut.insert(diag);
        //     }
        // }
        // // BOTTOM-RIGHT (i-1, j+1): BR -> TL
        // if (i - 1 >= 0 && j + 1 < tilesX) {
        //     SurfaceTile* diag = matrix[i - 1][j + 1];
        //     if (diag && diag->getIsInited()) {
        //         auto& vD = diag->getHeightVerticesRef();
        //         auto& mD = diag->getHeightMarkVerticesRef();
        //         copyCorner(vSrc, BR, vD, mD, TL);
        //         diag->setIsUpdated(true);
        //         changedOut.insert(diag);
        //     }
        // }
    }
}

void MosaicProcessor::updateUnmarkedHeightVertices(SurfaceTile* tilePtr) const
{
    if (!tilePtr) {
        return;
    }

    auto& heightVerticesRef = tilePtr->getHeightVerticesRef();
    auto& heightMarkVerticesRef = tilePtr->getHeightMarkVerticesRef();
    int hVSize = heightVerticesRef.size();
    int hMVSize = heightMarkVerticesRef.size();

    if (hVSize != hMVSize) {
        return;
    }

    auto writeHeight = [&](int toIndx, int fromIndx) -> bool {
        if (fromIndx >= 0 && fromIndx < hVSize) {
            if (heightMarkVerticesRef[fromIndx] == HeightType::kMosaic) {
                heightVerticesRef[toIndx][2] = heightVerticesRef[fromIndx][2];
                return true;
            }
        }
        return false;
    };

    int sideSize = std::sqrt(hVSize);
    for (int i = 0; i < hVSize; ++i) {
        if (heightMarkVerticesRef[i] == HeightType::kUndefined) {
            if (i % sideSize) {
                if (writeHeight(i, i - 1) ||
                    writeHeight(i, (i - 1) - sideSize) ||
                    writeHeight(i, (i - 1) + sideSize)) {
                    continue;
                }
            }
            if ((i % sideSize + 1) < sideSize) {
                if (writeHeight(i, i + 1) ||
                    writeHeight(i, (i + 1) - sideSize)||
                    writeHeight(i, (i + 1) + sideSize)) {
                    continue;
                }
            }
            if (writeHeight(i, i - sideSize) ||
                writeHeight(i, i + sideSize)) {
                continue;
            }
        }
    }
}

void MosaicProcessor::updateData(const QVector<int>& indxs, QSet<int>& usedEpochs, QSet<int>& blockedEpochs,
                                 QVector<int>* newUsed, QVector<int>* newBlocked)
{
    //qDebug() << "MosaicProcessor::updateData"
    //         << "thread =" << QThread::currentThread()
    //         << "size =" << indxs.size();
    //qDebug() << "MosaicProcessor::updateData";
    //qDebug() << indxs;
    //qDebug() << "";

    bool bench = false;
    QElapsedTimer et; // test
    if (bench) {
        et.start();
    }

    if (!datasetPtr_ || indxs.empty()) {
        return;
    }
    const bool segFIsValid = segFChannelId_.isValid();
    const bool segSIsValid = segSChannelId_.isValid();
    if (!segFIsValid && !segSIsValid) {
        return;
    }

    kmath::MatrixParams actualMatParams(lastMatParams_);
    kmath::MatrixParams newMatrixParams;
    QVector<QVector3D>  measLinesVertices;
    QVector<int>        measLinesEvenIndices;
    QVector<int>        measLinesOddIndices;
    QVector<char>       isOdds; // 0 - even, 1 - odd
    QVector<int>        epochIndxs;
    QVector3D           lastLeftBeg, lastLeftEnd, lastRightBeg, lastRightEnd;
    bool                haveNewTraceLine = false;

    // update matrix
    for (const auto& i : indxs) {
        auto epoch = datasetPtr_->fromIndexMosaicCopy(i);
        if (!epoch.isValid()) {
            continue;
        }

        auto pos = epoch.getSonarPosition().ned;
        auto yaw = epoch.tryRetValidYaw();
        if (isfinite(pos.n) && isfinite(pos.e) && isfinite(yaw)) {
            bool acceptedEven = false, acceptedOdd = false;
            double azRad = qDegreesToRadians(yaw);
            if (segFIsValid) {
                if (auto segFCharts = epoch.chart(segFChannelId_, segFSubChannelId_); segFCharts) {
                    double leftAzRad = azRad - M_PI_2 + qDegreesToRadians(lAngleOffset_);
                    float lDist = segFCharts->range();
                    float lDepth = static_cast<float>(segFCharts->bottomProcessing.getDistance());
                    float lZ = (std::isfinite(lDepth) && lDepth > 0.0f) ? -lDepth : 0.0f;
                    lastLeftBeg = QVector3D(pos.n + lDist * qCos(leftAzRad), pos.e + lDist * qSin(leftAzRad), lZ);
                    lastLeftEnd = QVector3D(pos.n, pos.e, lZ);
                    measLinesVertices.append(lastLeftBeg);
                    measLinesVertices.append(lastLeftEnd);
                    measLinesEvenIndices.append(currIndxSec_++);
                    measLinesEvenIndices.append(currIndxSec_++);
                    isOdds.append('0');
                    epochIndxs.append(i);
                    acceptedEven = true;
                }
            }

            if (segSIsValid) {
                if (auto segSCharts = epoch.chart(segSChannelId_, segSSubChannelId_); segSCharts) {
                    double rightAzRad = azRad + M_PI_2 - qDegreesToRadians(rAngleOffset_);
                    float rDist = segSCharts->range();
                    float rDepth = static_cast<float>(segSCharts->bottomProcessing.getDistance());
                    float rZ = (std::isfinite(rDepth) && rDepth > 0.0f) ? -rDepth : 0.0f;
                    lastRightBeg = QVector3D(pos.n, pos.e, rZ);
                    lastRightEnd = QVector3D(pos.n + rDist * qCos(rightAzRad), pos.e + rDist * qSin(rightAzRad), rZ);
                    measLinesVertices.append(lastRightBeg);
                    measLinesVertices.append(lastRightEnd);
                    measLinesOddIndices.append(currIndxSec_++);
                    measLinesOddIndices.append(currIndxSec_++);
                    isOdds.append('1');
                    epochIndxs.append(i);
                    acceptedOdd = true;
                }
            }

            if (acceptedEven || acceptedOdd) {
                lastAcceptedEpoch_ = std::max(lastAcceptedEpoch_, i);
                haveNewTraceLine = true;
            }
        }
    }

    if (measLinesVertices.empty()) {
        return;
    }

    bool traceLineUpdated = false;
    if (haveNewTraceLine && lastAcceptedEpoch_ > lastTraceLineEpoch_) {
        lastLeftBeg_  = lastLeftBeg;
        lastLeftEnd_  = lastLeftEnd;
        lastRightBeg_ = lastRightBeg;
        lastRightEnd_ = lastRightEnd;
        lastTraceLineEpoch_ = lastAcceptedEpoch_;
        traceLineUpdated = true;
    }

    const float tileSideMeters = tileSidePixelSize_ * tileResolution_;
    newMatrixParams = kmath::getMatrixParams(measLinesVertices, tileSideMeters);
    if (!newMatrixParams.isValid()) {
        return;
    }
    concatenateMatrixParameters(actualMatParams, newMatrixParams);

    // expand surface mesh
    const float marginMeters  = expandMargin_ * tileSideMeters;
    kmath::MatrixParams expanded = actualMatParams;
    expanded.originX -= marginMeters;
    expanded.originY -= marginMeters;
    expanded.width   += 2 * marginMeters;
    expanded.height  += 2 * marginMeters;
    const double S = double(tileSidePixelSize_ * tileResolution_);
    auto snapDown = [&](double v){ return std::floor(v / S) * S; };
    auto snapUp   = [&](double v){ return std::ceil (v / S) * S; };
    {
        const double x0  = snapDown(expanded.originX);
        const double y0  = snapDown(expanded.originY);
        const double x1  = snapUp  (expanded.originX + expanded.width);
        const double y1  = snapUp  (expanded.originY + expanded.height);
        expanded.originX = float(x0);
        expanded.originY = float(y0);
        expanded.width   = int(std::lround(x1 - x0));
        expanded.height  = int(std::lround(y1 - y0));
    }
    surfaceMeshPtr_->concatenate(expanded);

    QSet<TileKey> visAndPrefTileKeys = allowedTiles_ ? *allowedTiles_
                                     : computeWorker_ ? computeWorker_->getVisibleTileKeysCPtr() : QSet<TileKey>();
    { // prefetch tiles
        QSet<TileKey> toRestore = forecastTilesToTouch(measLinesVertices, isOdds, epochIndxs, expandMargin_/*for prefetch*/);
        if (sink_ && !allowedTiles_) {
            visAndPrefTileKeys = toRestore;   // a map export paints everything
        }
        visAndPrefTileKeys.intersect(toRestore);

        if (!toRestore.isEmpty()) {
            QSet<TileKey> need;
            need.reserve(toRestore.size());
            for (auto it = toRestore.cbegin(); it != toRestore.cend(); ++it) {
                auto cKey = *it;
                if (auto* t = surfaceMeshPtr_->getTilePtrByKey(cKey); t) {
                    if (!t->getIsInited()) {
                        need.insert(cKey);
                    }
                    // if before was surf proc on changing zoom
                    //if (t->getHeadIndx() == -1) {
                    //    need.insert(cKey);
                    //}
                }
            }

            if (!need.isEmpty()) {
                bool cut = false;
                prefetchTiles(need, &cut);
                if (cut && background_) {
                    // Tiles still on their way from the DB would be seeded blank below, painted, and
                    // kept over their saved rows (a precompute cancelled by a zoom keeps what it
                    // sends); the chunk is not painted, its epochs are sent again (2026-10-18). Only
                    // here: the run ends now and its state with it. The view's processor keeps this
                    // call's state (the epochs and the box it has taken in above) for its next job,
                    // which then skipped the chunk's pings -- two runs alike came out 36 672 pixels apart.
                    return;
                }
            }

            for (auto it = need.cbegin(); it != need.cend(); ++it) {
                if (auto* tile = surfaceMeshPtr_->getTilePtrByKey(*it); tile) {
                    if (!tile->getIsInited()) {
                        tile->init(tileSidePixelSize_, tileHeightMatrixRatio_, tileResolution_);
                        tile->initImageData(tileSidePixelSize_, tileHeightMatrixRatio_);
                    }
                }
            }
        }
    }

    { // mark tiles
        for (auto it = visAndPrefTileKeys.cbegin(); it != visAndPrefTileKeys.cend(); ++it) {
            if (auto* t = surfaceMeshPtr_->getTilePtrByKey(*it); t) {
                if (t->getIsInited()) {
                    if (t->getMosaicImageDataCRef().empty()) {
                        t->initImageData(t->sidePixelSize(), t->heightMatrixRatio());
                    }
                }
                t->setInFov(true);
            }
        }
    }

    uint64_t traceCnt = 0;

    // after expand by martix
    const int numHeightTiles = surfaceMeshPtr_->getNumHeightTiles();
    const int stepSizeHeightMatrix = surfaceMeshPtr_->getStepSizeHeightMatrix();
    const int meshPixW = surfaceMeshPtr_->getPixelWidth();
    const int meshPixH = surfaceMeshPtr_->getPixelHeight();
    const int heightSide = tileSidePixelSize_ / stepSizeHeightMatrix + 1;   // height points along a tile side

    if (bench) {
        qDebug() << "prep time,    ms" << et.elapsed(); // test
        et.restart();
        et.start();
    }

    // The pairs: a line and the next line of its side, in the order the lines came -- by epoch, the
    // left side before the right. A later pair paints over an earlier one, so that order is the result.
    struct PairJob {
        int segFIndx;
        int segSIndx;
        int segFBegVertIndx;
        int segFEndVertIndx;
        int segSBegVertIndx;
        int segSEndVertIndx;
        int epochF;
        int epochS;
        char side;
    };
    QVector<PairJob> jobs;
    jobs.reserve(measLinesVertices.size() / 2);
    for (int i = 0; i < measLinesVertices.size(); i += 2) {
        if (i + 5 > measLinesVertices.size() - 1) {
            break;
        }

        int segFBegVertIndx = i;
        int segFEndVertIndx = i + 1;
        int segSBegVertIndx = i + 2;
        int segSEndVertIndx = i + 3;

        int segFIndx = i / 2;
        int segSIndx = (i + 2) / 2;

        if (epochIndxs[segFIndx] == epochIndxs[segSIndx] ||
            isOdds[segFIndx] != isOdds[segSIndx]) {
            ++segSIndx;
            segSBegVertIndx += 2;
            segSEndVertIndx += 2;
        }
        if (segSIndx >= epochIndxs.size()) {
            break;
        }

        if (epochIndxs[segFIndx] == epochIndxs[segSIndx] ||
            isOdds[segFIndx] != isOdds[segSIndx]) {
            continue;
        }

        jobs.append(PairJob{ segFIndx, segSIndx, segFBegVertIndx, segFEndVertIndx, segSBegVertIndx, segSEndVertIndx,
                             epochIndxs[segFIndx], epochIndxs[segSIndx], isOdds[segFIndx] });
    }

    struct PairResult {
        bool used = false;
        bool outOfFov = false;
    };
    QVector<PairResult> results(jobs.size());

    // One pair, as the loop painted it before 2026-10-17; where a pixel and a height go is the sink's:
    // in place (DirectSink), or into a thread's own buffers merged afterwards (BufferSink). Returns
    // false for a pair skipped before painting (no epoch, chart or bottom, too short).
    auto paintPair = [&](const PairJob& job, int seq, PairResult& res, auto& sink) -> bool {
        auto segFEpoch = datasetPtr_->fromIndexMosaicCopy(job.epochF);
        auto segSEpoch = datasetPtr_->fromIndexMosaicCopy(job.epochS);
        if (!segFEpoch.isValid() || !segSEpoch.isValid()) {
            return false;
        }

        bool segFIsOdd = isOdds[job.segFIndx] == '1';
        bool segSIsOdd = isOdds[job.segSIndx] == '1';
        if (segFIsOdd != segSIsOdd) {
            return false;
        }

        auto segFCh  = segFIsOdd ? segSChannelId_    : segFChannelId_;
        auto segFSCh = segFIsOdd ? segSSubChannelId_ : segFSubChannelId_;
        auto segSCh  = segSIsOdd ? segSChannelId_    : segFChannelId_;
        auto segSSCh = segSIsOdd ? segSSubChannelId_ : segFSubChannelId_;
        auto* segFCharts = segFEpoch.chart(segFCh, segFSCh);
        auto* segSCharts = segSEpoch.chart(segSCh, segSSCh);
        if (!segFCharts || !segSCharts) {
            return false;
        }
        if (!isfinite(segFCharts->bottomProcessing.getDistance()) ||
            !isfinite(segSCharts->bottomProcessing.getDistance())) {
            return false;
        }
        switch (source_) {
        case Source::Amplitude:
            // ничего готовить не надо — читаем сырое
            break;
        case Source::Tgc:
            if (segFCharts->amplitude.size() != segFCharts->tgc.size()) {
                segFCharts->updateTgc();
            }
            if (segSCharts->amplitude.size() != segSCharts->tgc.size()) {
                segSCharts->updateTgc();
            }
            break;
        case Source::SideScan:
            if (segFCharts->amplitude.size() != segFCharts->compensated.size()) {
                segFCharts->updateCompesated();
            }
            if (segSCharts->amplitude.size() != segSCharts->compensated.size()) {
                segSCharts->updateCompesated();
            }
            break;
        }

        // Bresenham, first segment
        QVector3D segFPhBegPnt = segFIsOdd ? measLinesVertices[job.segFBegVertIndx] : measLinesVertices[job.segFEndVertIndx];
        QVector3D segFPhEndPnt = segFIsOdd ? measLinesVertices[job.segFEndVertIndx] : measLinesVertices[job.segFBegVertIndx];
        auto segFBegPixPos = surfaceMeshPtr_->convertPhToPixCoords(segFPhBegPnt);
        auto segFEndPixPos = surfaceMeshPtr_->convertPhToPixCoords(segFPhEndPnt);
        int segFPixX1 = segFBegPixPos.x();
        int segFPixY1 = segFBegPixPos.y();
        int segFPixX2 = segFEndPixPos.x();
        int segFPixY2 = segFEndPixPos.y();
        float segFPixTotDist = std::sqrt(std::pow(segFPixX2 - segFPixX1, 2) + std::pow(segFPixY2 - segFPixY1, 2));
        int segFPixDx = std::abs(segFPixX2 - segFPixX1);
        int segFPixDy = std::abs(segFPixY2 - segFPixY1);
        int segFPixSx = (segFPixX1 < segFPixX2) ? 1 : -1;
        int segFPixSy = (segFPixY1 < segFPixY2) ? 1 : -1;
        int segFPixErr = segFPixDx - segFPixDy;
        // second segment
        QVector3D segSPhBegPnt = segSIsOdd ? measLinesVertices[job.segSBegVertIndx] : measLinesVertices[job.segSEndVertIndx];
        QVector3D segSPhEndPnt = segSIsOdd ? measLinesVertices[job.segSEndVertIndx] : measLinesVertices[job.segSBegVertIndx];
        auto segSBegPixPos = surfaceMeshPtr_->convertPhToPixCoords(segSPhBegPnt);
        auto segSEndPixPos = surfaceMeshPtr_->convertPhToPixCoords(segSPhEndPnt);
        int segSPixX1 = segSBegPixPos.x();
        int segSPixY1 = segSBegPixPos.y();
        int segSPixX2 = segSEndPixPos.x();
        int segSPixY2 = segSEndPixPos.y();
        float segSPixTotDist = std::sqrt(std::pow(segSPixX2 - segSPixX1, 2) + std::pow(segSPixY2 - segSPixY1, 2));
        int segSPixDx = std::abs(segSPixX2 - segSPixX1);
        int segSPixDy = std::abs(segSPixY2 - segSPixY1);
        int segSPixSx = (segSPixX1 < segSPixX2) ? 1 : -1;
        int segSPixSy = (segSPixY1 < segSPixY2) ? 1 : -1;
        int segSPixErr = segSPixDx - segSPixDy;
        // pixel length checking
        if (!checkLength(segFPixTotDist) ||
            !checkLength(segSPixTotDist)) {
            return false;
        }

        float segFDistProc = -1.0f * static_cast<float>(segFCharts->bottomProcessing.getDistance());
        float segSDistProc = -1.0f * static_cast<float>(segSCharts->bottomProcessing.getDistance());
        float segFPhDistX = segFPhEndPnt.x() - segFPhBegPnt.x();
        float segFPhDistY = segFPhEndPnt.y() - segFPhBegPnt.y();
        float segSPhDistX = segSPhEndPnt.x() - segSPhBegPnt.x();
        float segSPhDistY = segSPhEndPnt.y() - segSPhBegPnt.y();

        auto segFInterpNED = segFEpoch.getSonarPosition().ned;
        auto segSInterpNED = segSEpoch.getSonarPosition().ned;
        QVector3D segFBoatPos(segFInterpNED.n, segFInterpNED.e, 0.0f);
        QVector3D segSBoatPos(segSInterpNED.n, segSInterpNED.e, 0.0f);

        // select longest ray
        const bool bigIsF = (segFPixTotDist >= segSPixTotDist);
        int       bigX1  = bigIsF ? segFPixX1  :  segSPixX1;
        int       bigY1  = bigIsF ? segFPixY1  :  segSPixY1;
        const int bigX2  = bigIsF ? segFPixX2  :  segSPixX2;
        const int bigY2  = bigIsF ? segFPixY2  :  segSPixY2;
        int       bigErr = bigIsF ? segFPixErr :  segSPixErr;
        const int bigDx  = bigIsF ? segFPixDx  :  segSPixDx;
        const int bigDy  = bigIsF ? segFPixDy  :  segSPixDy;
        const int bigSx  = bigIsF ? segFPixSx  :  segSPixSx;
        const int bigSy  = bigIsF ? segFPixSy  :  segSPixSy;
        const float bigTot = std::max(1.0f, bigIsF ? segFPixTotDist : segSPixTotDist);

        auto bresStep = [&](int& x0, int& y0, const int& x1, const int& y1, int& err, const int& dx, const int& dy, const int& sx,  const int& sy) -> bool {
            if (x0 == x1 && y0 == y1) {
                return true;
            }

            int e2 = (err << 1);
            if (e2 > -dy) {
                err -= dy; x0 += sx;
            }
            else if (e2 < dx) {
                err += dx; y0 += sy;
            }

            return false;
        };

        const int freshEpIndx = job.epochS;
        // Chord or arc, chosen once for the pair by its far ends (mosaic_arc.h).
        const auto pairPath = mosaic_arc::pathFor(segFBegPixPos.x(), segFBegPixPos.y(), segFEndPixPos.x(), segFEndPixPos.y(),
                                                  segSBegPixPos.x(), segSBegPixPos.y(), segSEndPixPos.x(), segSEndPixPos.y(),
                                                  kMaxArcTurnRad);

        while (true) { // follow the biggest segment
            const float rem = std::sqrt(float((bigX2 - bigX1) * (bigX2 - bigX1) + (bigY2 - bigY1) * (bigY2 - bigY1))); // moving on longest line
            const float t   = std::clamp(rem / bigTot, 0.0f, 1.0f);
            QVector3D segFCurrPhPos(segFPhBegPnt.x() + t * segFPhDistX, segFPhBegPnt.y() + t * segFPhDistY, segFDistProc);
            QVector3D segSCurrPhPos(segSPhBegPnt.x() + t * segSPhDistX, segSPhBegPnt.y() + t * segSPhDistY, segSDistProc);

            uint8_t segFQuality = 0;
            uint8_t segSQuality = 0;
            const int segFColorIndx = sampleColor(segFCharts, segFIsOdd ? 1 : 0, -segFDistProc,
                                                  segFCurrPhPos.distanceToPoint(segFBoatPos), &segFQuality);
            const int segSColorIndx = sampleColor(segSCharts, segSIsOdd ? 1 : 0, -segSDistProc,
                                                  segSCurrPhPos.distanceToPoint(segSBoatPos), &segSQuality);
            if (!segFColorIndx && !segSColorIndx) {
                if (bresStep(bigX1, bigY1, bigX2, bigY2, bigErr, bigDx, bigDy, bigSx, bigSy)) {
                    break;
                }
                continue;
            }
            auto segFCurrPixPos = surfaceMeshPtr_->convertPhToPixCoords(segFCurrPhPos);
            auto segSCurrPixPos = surfaceMeshPtr_->convertPhToPixCoords(segSCurrPhPos);

            // Between the two pings: the chord as before, or the arc of a turning sonar (mosaic_arc.h).
            mosaic_arc::walk(segFBegPixPos.x(), segFBegPixPos.y(), segFCurrPixPos.x(), segFCurrPixPos.y(),
                             segSBegPixPos.x(), segSBegPixPos.y(), segSCurrPixPos.x(), segSCurrPixPos.y(),
                             pairPath, meshPixW, meshPixH,
                             [&](int x0, int y0, float iP, int stepsDone) {
                // Where one ping has no sample (out of its range, or cropped) the other's is taken as it
                // is: blended with that 0 it darkened the swath's edge, and painted a 0 -- nothing --
                // over what was there at the far end (until 2026-10-18).
                uint8_t iClrIndx = 0;
                uint8_t iQuality = 0;
                if (!segFColorIndx) {
                    iClrIndx = uint8_t(segSColorIndx);
                    iQuality = segSQuality;
                }
                else if (!segSColorIndx) {
                    iClrIndx = uint8_t(segFColorIndx);
                    iQuality = segFQuality;
                }
                else {
                    iClrIndx = static_cast<uint8_t>((1 - iP) * segFColorIndx + iP * segSColorIndx);
                    iQuality = static_cast<uint8_t>(std::lround((1 - iP) * segFQuality + iP * segSQuality));
                }

                // рисуем текущий пиксель
                SurfaceTile* t = sink.pixel(x0, y0, iClrIndx, iQuality, freshEpIndx, job.epochF, seq, res);

                if (t) {
                    res.used = true;
                    if (!(stepsDone % tileHeightMatrixRatio_)) {
                        const int tileX = x0 % tileSidePixelSize_;
                        const int tileY = y0 % tileSidePixelSize_;
                        const int hIdx = (tileY / stepSizeHeightMatrix) * heightSide + (tileX / stepSizeHeightMatrix);
                        sink.height(t, hIdx, segFCurrPhPos[2], job.epochF, seq);
                    }
                }
            });

            if (bresStep(bigX1, bigY1, bigX2, bigY2, bigErr, bigDx, bigDy, bigSx, bigSy)) {
                break;
            }
        } // while interp line
        return true;
    };

    // Writes in place, pair after pair: the loop as it was.
    struct DirectSink {
        MosaicProcessor* self;
        int numHeightTiles;
        uint64_t& traceCnt;
        QSet<SurfaceTile*>& usedTiles;
        int& lastHead;
        // The owners of the tile painted last: a walk stays in a tile for many pixels, so the hash is
        // looked up once per tile change. A lookup that inserts may move the other entries, and none
        // but this one is held.
        const SurfaceTile* ownTile = nullptr;
        OwnedPixels* ownPix = nullptr;

        SurfaceTile* pixel(int x, int y, uint8_t color, uint8_t quality, int headIndx, int owner, int, PairResult& res)
        {
            const int side = self->tileSidePixelSize_;
            const int meshX = x / side;
            const int meshY = (numHeightTiles - 1) - (y / side);
            SurfaceTile* t  = self->surfaceMeshPtr_->getTileByXYIndxs(meshY, meshX);
            if (!t || !t->getInFov()) {
                res.outOfFov = true;
                return nullptr;
            }
            if (headIndx <= t->getHeadIndx()) {
                return nullptr;
            }
            const int pix = (y % side) * side + (x % side);
            auto& img = t->getMosaicImageDataRef();
            if (self->rules_.usesQuality()) {
                auto& q = t->getMosaicQualityRef();
                if (q.size() != img.size()) {
                    q.assign(img.size(), 0);
                }
                if (quality < q[size_t(pix)]) {
                    return t;   // a better sample is there: kept (the height is still this pair's)
                }
                q[size_t(pix)] = quality;
            }
            img.data()[pix] = color;
            t->setIsUpdated(true);
            traceCnt++;
            if (self->trackOwners_) {
                if (t != ownTile) {
                    ownPix = &self->owners_[t->getKey()];
                    ownTile = t;
                    if (ownPix->owner.size() != img.size()) {
                        ownPix->owner.assign(img.size(), -1);
                    }
                }
                ownPix->owner[size_t(pix)] = owner;
                ownPix->oldest = std::min(ownPix->oldest, int32_t(owner));
            }
            return t;
        }

        void height(SurfaceTile* t, int hIdx, float z, int owner, int)
        {
            usedTiles.insert(t);
            lastHead = owner;
            auto& hT = t->getHeightMarkVerticesRef()[hIdx];
            if (hT != HeightType::kTriangulation) {
                t->getHeightVerticesRef()[hIdx][2] = z;
                hT = HeightType::kMosaic;
            }
        }
    };

    // A thread's writes, each pixel and height point with the number of the pair that wrote it last.
    // A pixel's stamp holds the sample's quality above the pair number (0 without an overlap rule that
    // weighs it), so the largest stamp is the best sample, the last of equals -- one comparison, no
    // buffer of qualities (2026-10-18, a profile of 00017).
    struct TileBuf {
        std::vector<uint8_t> color;
        std::vector<uint32_t> stamp;    // quality << 24 | (pair number + 1), 0: not written
        std::vector<float> hz;
        std::vector<uint32_t> hstamp;
        int x0 = std::numeric_limits<int>::max();
        int y0 = std::numeric_limits<int>::max();
        int x1 = -1;
        int y1 = -1;
    };
    struct BufferSink {
        MosaicProcessor* self;
        int numHeightTiles;
        int heightCells;
        std::unordered_map<SurfaceTile*, TileBuf> bufs;   // node-based: a held reference stays valid
        SurfaceTile* lastTile = nullptr;
        TileBuf* lastBuf = nullptr;
        QSet<SurfaceTile*> usedTiles;
        int lastHead = -1;
        uint64_t traceCnt = 0;

        TileBuf& buf(SurfaceTile* t)
        {
            if (t != lastTile) {
                TileBuf& b = bufs[t];
                if (b.stamp.empty()) {
                    const size_t n = size_t(self->tileSidePixelSize_) * size_t(self->tileSidePixelSize_);
                    b.color.resize(n);
                    b.stamp.assign(n, 0);
                    b.hz.resize(size_t(heightCells));
                    b.hstamp.assign(size_t(heightCells), 0);
                }
                lastTile = t;
                lastBuf = &b;
            }
            return *lastBuf;
        }

        SurfaceTile* pixel(int x, int y, uint8_t color, uint8_t quality, int headIndx, int, int seq, PairResult& res)
        {
            const int side = self->tileSidePixelSize_;
            const int meshX = x / side;
            const int meshY = (numHeightTiles - 1) - (y / side);
            SurfaceTile* t  = self->surfaceMeshPtr_->getTileByXYIndxs(meshY, meshX);
            if (!t || !t->getInFov()) {
                res.outOfFov = true;
                return nullptr;
            }
            if (headIndx <= t->getHeadIndx()) {
                return nullptr;
            }
            const int tx = x % side;
            const int ty = y % side;
            const int pix = ty * side + tx;
            TileBuf& b = buf(t);
            // As DirectSink in pair order: the last of the best stays (the pairs of a thread come in
            // order). The tile's own pixel is weighed in the merge.
            const uint32_t key = (uint32_t(quality) << 24) | (uint32_t(seq) + 1u);
            if (key < b.stamp[size_t(pix)]) {
                return t;
            }
            b.color[size_t(pix)] = color;
            b.stamp[size_t(pix)] = key;
            b.x0 = std::min(b.x0, tx);
            b.x1 = std::max(b.x1, tx);
            b.y0 = std::min(b.y0, ty);
            b.y1 = std::max(b.y1, ty);
            traceCnt++;
            return t;
        }

        void height(SurfaceTile* t, int hIdx, float z, int owner, int seq)
        {
            usedTiles.insert(t);
            lastHead = std::max(lastHead, owner);
            TileBuf& b = buf(t);
            b.hz[size_t(hIdx)] = z;
            b.hstamp[size_t(hIdx)] = uint32_t(seq) + 1u;
        }
    };

    QSet<SurfaceTile*> usedTiles;
    int lastHead = -1;

    const int threads = paintThreads_;
    // (the stamps keep a pair number in 24 bits: a chunk is some hundred pairs)
    const bool parallel = threads > 1 && jobs.size() >= 2 * threads && jobs.size() < (1 << 24);
    if (!parallel) {
        DirectSink sink{ this, numHeightTiles, traceCnt, usedTiles, lastHead };
        bool cancelPending = false;
        int cancelEpochF = -1;
        int cancelEpochS = -1;
        char cancelSide = 0;
        for (int seq = 0; seq < jobs.size(); ++seq) {
            const PairJob& job = jobs[seq];
            bool stopAfterThis = false;
            if (cancelPending) {
                const bool isCounterpart = (job.epochF == cancelEpochF &&
                                            job.epochS == cancelEpochS &&
                                            job.side != cancelSide);
                if (!isCounterpart) {
                    break;
                }
                stopAfterThis = true;
            }
            if (!paintPair(job, seq, results[seq], sink)) {
                continue;
            }
            if (!cancelPending && canceled()) {
                cancelPending = true;
                cancelEpochF = job.epochF;
                cancelEpochS = job.epochS;
                cancelSide = job.side;
            }
            if (stopAfterThis) {
                break;
            }
        }
    }
    else {
        // Contiguous runs of pairs, one a thread, each into buffers of its own; a run is finished even
        // when the job is cancelled meanwhile (it is a chunk of updateDataWrapper, a few ms).
        const int heightCells = heightSide * heightSide;
        std::vector<std::unique_ptr<BufferSink>> sinks;
        for (int k = 0; k < threads; ++k) {
            sinks.push_back(std::make_unique<BufferSink>(BufferSink{ this, numHeightTiles, heightCells, {}, nullptr, nullptr, {}, -1, 0 }));
        }
        auto runSlice = [&](int k) {
            const int from = int((qint64(jobs.size()) * k) / threads);
            const int to = int((qint64(jobs.size()) * (k + 1)) / threads);
            for (int seq = from; seq < to; ++seq) {
                paintPair(jobs[seq], seq, results[seq], *sinks[size_t(k)]);
            }
        };
        QSemaphore done(0);
        for (int k = 1; k < threads; ++k) {
            paintPool_.start([&runSlice, &done, k]() {
                runSlice(k);
                done.release();
            });
        }
        runSlice(0);
        done.acquire(threads - 1);

        // Merge: each pixel and height point from the pair numbered last, as painting in order leaves it.
        std::unordered_map<SurfaceTile*, std::vector<const TileBuf*>> byTile;
        for (const auto& s : sinks) {
            for (const auto& kv : s->bufs) {
                byTile[kv.first].push_back(&kv.second);
            }
            usedTiles.unite(s->usedTiles);
            lastHead = std::max(lastHead, s->lastHead);
            traceCnt += s->traceCnt;
        }
        const int side = tileSidePixelSize_;
        for (const auto& kv : byTile) {
            SurfaceTile* t = kv.first;
            const auto& bs = kv.second;
            int x0 = std::numeric_limits<int>::max();
            int y0 = std::numeric_limits<int>::max();
            int x1 = -1;
            int y1 = -1;
            for (const TileBuf* b : bs) {
                x0 = std::min(x0, b->x0);
                y0 = std::min(y0, b->y0);
                x1 = std::max(x1, b->x1);
                y1 = std::max(y1, b->y1);
            }
            auto& img = t->getMosaicImageDataRef();
            OwnedPixels* own = nullptr;
            if (trackOwners_ && x1 >= x0) {
                own = &owners_[t->getKey()];
                if (own->owner.size() != img.size()) {
                    own->owner.assign(img.size(), -1);
                }
            }
            // An overlap rule with qualities: the pair of the best quality, the last of them on a tie -- what DirectSink
            // leaves -- and only if not worse than what the tile holds.
            uint8_t* tileQ = nullptr;
            if (rules_.usesQuality() && x1 >= x0) {
                auto& q = t->getMosaicQualityRef();
                if (q.size() != img.size()) {
                    q.assign(img.size(), 0);
                }
                tileQ = q.data();
            }
            bool written = false;
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const size_t pix = size_t(y * side + x);
                    uint32_t best = 0;
                    const TileBuf* from = nullptr;
                    for (const TileBuf* b : bs) {
                        if (b->stamp[pix] > best) {
                            best = b->stamp[pix];
                            from = b;
                        }
                    }
                    if (!from) {
                        continue;
                    }
                    if (tileQ) {
                        const uint8_t q = uint8_t(best >> 24);
                        if (q < tileQ[pix]) {
                            continue;
                        }
                        tileQ[pix] = q;
                    }
                    img.data()[pix] = from->color[pix];
                    written = true;
                    if (own) {
                        const int32_t ow = jobs[int((best & 0xFFFFFFu) - 1u)].epochF;
                        own->owner[pix] = ow;
                        own->oldest = std::min(own->oldest, ow);
                    }
                }
            }
            if (written) {
                t->setIsUpdated(true);
            }
            auto& verts = t->getHeightVerticesRef();
            auto& marks = t->getHeightMarkVerticesRef();
            for (int h = 0; h < heightCells && h < verts.size() && h < marks.size(); ++h) {
                uint32_t best = 0;
                const TileBuf* from = nullptr;
                for (const TileBuf* b : bs) {
                    if (b->hstamp[size_t(h)] > best) {
                        best = b->hstamp[size_t(h)];
                        from = b;
                    }
                }
                if (!from) {
                    continue;
                }
                auto& hT = marks[h];
                if (hT != HeightType::kTriangulation) {
                    verts[h][2] = from->hz[size_t(h)];
                    hT = HeightType::kMosaic;
                }
            }
        }
    }

    for (int seq = 0; seq < jobs.size(); ++seq) {
        const PairJob& job = jobs[seq];
        const PairResult& res = results[seq];
        if (res.outOfFov) {
            if (!blockedEpochs.contains(job.epochF)) {
                blockedEpochs.insert(job.epochF);
                if (newBlocked) {
                    newBlocked->append(job.epochF);
                }
            }
            if (!blockedEpochs.contains(job.epochS)) {
                blockedEpochs.insert(job.epochS);
                if (newBlocked) {
                    newBlocked->append(job.epochS);
                }
            }
        }
        else if (res.used) {
            if (!usedEpochs.contains(job.epochF)) {
                usedEpochs.insert(job.epochF);
                if (newUsed) {
                    newUsed->append(job.epochF);
                }
            }
            if (!usedEpochs.contains(job.epochS)) {
                usedEpochs.insert(job.epochS);
                if (newUsed) {
                    newUsed->append(job.epochS);
                }
            }
        }
    }

    { // unmark tiles
        for (auto it = visAndPrefTileKeys.cbegin(); it != visAndPrefTileKeys.cend(); ++it) {
            if (auto* t = surfaceMeshPtr_->getTilePtrByKey(*it); t) {
                t->setInFov(false);
            }
        }
    }

    lastMatParams_ = actualMatParams;

    if (bench) {
        qDebug() << "trace time,   ms" << et.elapsed();
        et.restart();
        et.start();
    }

    //qDebug() << usedTiles.size();
    if (usedTiles.isEmpty()) {
        return;
    }

    if (lastHead != -1) {
        for (auto it = usedTiles.cbegin(); it != usedTiles.cend(); ++it) {
            auto* t = *it;
            t->setHeadIndx(lastHead);
        }
    }

    // collect changed tiles
    QSet<SurfaceTile*> updTiles = surfaceMeshPtr_->getUpdatedTiles();
    QSet<SurfaceTile*> changedOut;
    postUpdate(updTiles, changedOut);
    updTiles.unite(changedOut);
    TileMap res;
    res.reserve(updTiles.size());
    for (SurfaceTile* itm : std::as_const(updTiles)) {
        if (!itm || !itm->getIsInited()) {
            continue;
        }
        updateUnmarkedHeightVertices(itm);
        itm->updateHeightIndices();
        itm->setIsUpdated(false);
        res.insert((itm)->getKey(), (*itm));
    }
    surfaceMeshPtr_->setTileUsed(updTiles, true); // метка тайлов/ужимка

    if (bench) {
        qDebug() << "post up time, ms" << et.elapsed();
    }

    // emit data — buffer if we're in a batched run, else push immediately
    if (inBatch_) {
        for (auto it = res.cbegin(); it != res.cend(); ++it) {
            batchedTiles_.insert(it.key(), it.value());
        }
    } else {
        postTiles(res);
    }

    if (traceLineUpdated && !background_ && !sink_) {
        QMetaObject::invokeMethod(dataProcessor_, "postTraceLines", Qt::QueuedConnection,
            Q_ARG(QVector3D, lastLeftBeg_),
            Q_ARG(QVector3D, lastLeftEnd_),
            Q_ARG(QVector3D, lastRightBeg_),
            Q_ARG(QVector3D, lastRightEnd_),
            Q_ARG(int, lastTraceLineEpoch_)
        );
    }
}

int MosaicProcessor::getColorIndx(Epoch::Echogram* charts, int ampIndx) const
{
    int retVal{ 0 };

    if (!charts || ampIndx == -1) {
        return retVal;
    }

    const QVector<uint8_t>& srcArr =
        (source_ == Source::Tgc)       ? charts->tgc       :
        (source_ == Source::Amplitude) ? charts->amplitude :
                                         charts->compensated;
    const auto ampSize = srcArr.size();

    if (ampSize > ampIndx) {
        if (aliasWindow_ == 1) {
            int cVal = srcArr[ampIndx];
            cVal = std::min(colorTableSize_, cVal);
            retVal = cVal;
        }
        else { // aliasing
            const int half  = aliasWindow_ / 2;
            const int lIndx = ampIndx - half;
            const int rIndx = ampIndx + half;

            if (lIndx > 0 && ampSize > lIndx &&
                rIndx > 0 && ampSize > rIndx) {
                int newColorIndx = 0;
                for (int i = lIndx; i < rIndx; ++i) {
                    int cVal = srcArr[i];
                    cVal = std::min(colorTableSize_, cVal);
                    newColorIndx += cVal;
                }
                retVal = float(newColorIndx) / float(aliasWindow_);
            }
            else {
                int cVal = srcArr[ampIndx];
                cVal = std::min(colorTableSize_, cVal);
                retVal = cVal;
            }
        }
    }
    else {
        return retVal; // если вышел за предел - ноль
    }

    if (!retVal) {
        return 1;
    }

    return retVal;
}

int MosaicProcessor::sampleColor(Epoch::Echogram* charts, int side, float altitude, float slantRange, uint8_t* quality) const
{
    int color = getColorIndx(charts, sampleIndex(charts, slantRange));
    if (!color || (!beam_ && !rules_.crops() && !rules_.usesQuality())) {
        return color;   // as before 2026-10-18
    }
    // The cosine of the angle from straight down, compared and looked up as it is: an acos and a pow a
    // sample were half the painting (2026-10-18, a profile of 00017).
    const float cosA = (altitude > 0.0f && slantRange > altitude) ? altitude / slantRange : (altitude > 0.0f ? 1.0f : 0.0f);
    if (rules_.cropped(cosA)) {
        return 0;
    }
    if (beam_) {
        const mosaic_beam::SideModel& m = beam_->side[size_t(side)];
        if (m.valid) {
            color = std::clamp(int(float(color) * m.gainFast(cosA, slantRange) + 0.5f), 1, colorTableSize_);
        }
    }
    if (rules_.usesQuality()) {
        *quality = rules_.quality(side, cosA);   // else 0: the parallel painter keys pixels on it
    }
    return color;
}
bool MosaicProcessor::canceled() const noexcept
{
    if (sink_) {
        return sink_->cancelled();
    }
    if (background_) {   // a newer precompute, a zoom change, a reset
        return latestGeneration_ && latestGeneration_->load() != generation_;
    }
    return dataProcessor_ && dataProcessor_->isCancelRequested();
}

QSet<TileKey> MosaicProcessor::forecastTilesToTouch(const QVector<QVector3D>& meas, const QVector<char>& isOdds, const QVector<int>& epochIndxs, int marginTiles) const
{
    QSet<TileKey> retVal;

    if (!surfaceMeshPtr_ || meas.size() < 4) {
        return retVal;
    }

    const int tilePx = surfaceMeshPtr_->getTileSidePixelSize();
    const int H      = surfaceMeshPtr_->getNumHeightTiles();
    const int W      = surfaceMeshPtr_->getNumWidthTiles();
    auto clamp = [](int v, int lo, int hi) -> int {
        return std::min(std::max(v, lo), hi);
    };

    const int stridePx = std::max(8, tilePx / 4);
    const int m = std::max(0, marginTiles);

    auto pushIdxWithMargin = [&](int mx, int my){
        const int x0 = clamp(mx - m, 0, W - 1);
        const int x1 = clamp(mx + m, 0, W - 1);
        const int y0 = clamp(my - m, 0, H - 1);
        const int y1 = clamp(my + m, 0, H - 1);
        for (int ty = y0; ty <= y1; ++ty) {
            for (int tx = x0; tx <= x1; ++tx) {
                if (auto* t = surfaceMeshPtr_->getTileByXYIndxs(ty, tx)) {
                    retVal.insert(t->getKey());
                }
            }
        }
    };

    auto addPix = [&](int pixX, int pixY){
        int mx = clamp(pixX / tilePx,          0, W - 1);
        int my = clamp((H - 1) - pixY / tilePx,0, H - 1);
        pushIdxWithMargin(mx, my);
    };

    auto addLineTiles = [&](int x1, int y1, int x2, int y2){
        int ax1 = clamp(x1 / tilePx,           0, W - 1);
        int ay1 = clamp((H - 1) - y1 / tilePx, 0, H - 1);
        int ax2 = clamp(x2 / tilePx,           0, W - 1);
        int ay2 = clamp((H - 1) - y2 / tilePx, 0, H - 1);

        int dx = std::abs(ax2 - ax1), sx = ax1 < ax2 ? 1 : -1;
        int dy = -std::abs(ay2 - ay1), sy = ay1 < ay2 ? 1 : -1;
        int err = dx + dy, x = ax1, y = ay1;

        while (true) {
            pushIdxWithMargin(x, y);
            if (x == ax2 && y == ay2) {
                break;
            }

            int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x += sx;
            }

            if (e2 <= dx) {
                err += dx;
                y += sy;
            }
        }
    };

    for (int i = 0; i + 3 < meas.size(); i += 2) {
        int segFBegVertIndx = i,     segFEndVertIndx = i + 1;
        int segSBegVertIndx = i + 2, segSEndVertIndx = i + 3;

        int segFIndx = i / 2;
        int segSIndx = (i + 2) / 2;

        if (epochIndxs[segFIndx] == epochIndxs[segSIndx] || isOdds[segFIndx] != isOdds[segSIndx]) {
            ++segSIndx; segSBegVertIndx += 2; segSEndVertIndx += 2;
        }
        if (segSIndx >= epochIndxs.size()) {
            break;
        }

        if (epochIndxs[segFIndx] == epochIndxs[segSIndx] || isOdds[segFIndx] != isOdds[segSIndx]) {
            continue;
        }

        const bool segFIsOdd = isOdds[segFIndx] == '1';
        const bool segSIsOdd = isOdds[segSIndx] == '1';
        if (segFIsOdd != segSIsOdd) {
            continue;
        }

        auto Fbeg = surfaceMeshPtr_->convertPhToPixCoords(segFIsOdd ? meas[segFBegVertIndx] : meas[segFEndVertIndx]);
        auto Fend = surfaceMeshPtr_->convertPhToPixCoords(segFIsOdd ? meas[segFEndVertIndx] : meas[segFBegVertIndx]);
        auto Sbeg = surfaceMeshPtr_->convertPhToPixCoords(segSIsOdd ? meas[segSBegVertIndx] : meas[segSEndVertIndx]);
        auto Send = surfaceMeshPtr_->convertPhToPixCoords(segSIsOdd ? meas[segSEndVertIndx] : meas[segSBegVertIndx]);

        const int Fx1 = int(Fbeg.x()), Fy1 = int(Fbeg.y());
        const int Fx2 = int(Fend.x()), Fy2 = int(Fend.y());
        const int Sx1 = int(Sbeg.x()), Sy1 = int(Sbeg.y());
        const int Sx2 = int(Send.x()), Sy2 = int(Send.y());

        const float Ftot = std::hypot(Fx2 - Fx1, Fy2 - Fy1);
        const float Stot = std::hypot(Sx2 - Sx1, Sy2 - Sy1);
        const float longest = std::max(Ftot, Stot);
        if (longest <= 0.0f) {
            continue;
        }

        const int steps = std::max(1, int(std::ceil(longest / float(stridePx))));
        for (int s = 0; s <= steps; ++s) {
            float tt = float(s) / float(steps);
            const int Fx = int(std::lround(Fx1 + tt * (Fx2 - Fx1)));
            const int Fy = int(std::lround(Fy1 + tt * (Fy2 - Fy1)));
            const int Sx = int(std::lround(Sx1 + tt * (Sx2 - Sx1)));
            const int Sy = int(std::lround(Sy1 + tt * (Sy2 - Sy1)));
            addPix(Fx, Fy);
            addPix(Sx, Sy);
            addLineTiles(Fx, Fy, Sx, Sy);
        }
    }

    return retVal;
}

void MosaicProcessor::putTilesIntoMesh(const TileMap &tiles) // может вызвать только prefetch, т.к. инитит тайл
{
    if (!surfaceMeshPtr_ || tiles.isEmpty()) {
        return;
    }

    QSet<SurfaceTile*> initedNow;

    for (auto it = tiles.cbegin(); it != tiles.cend(); ++it) {
        const TileKey tKey = it.key();
        SurfaceTile* dst = surfaceMeshPtr_->getTilePtrByKey(tKey);
        if (!dst) {
            continue;
        }
        const auto& src = it.value();

        if (!dst->getIsInited()) {
            dst->setOrigin(src.getOrigin()); //
            dst->init(tileSidePixelSize_, tileHeightMatrixRatio_, tileResolution_);
            dst->initImageData(tileSidePixelSize_, tileHeightMatrixRatio_);
        }

        dst->setHeadIndx(src.getHeadIndx());

        // image
        auto& di = dst->getMosaicImageDataRef();
        const auto& si = src.getMosaicImageDataCRef();
        if (!si.empty()) {
            if (di.size() != si.size()) {
                dst->initImageData(dst->sidePixelSize(), dst->heightMatrixRatio());
            }
            if (di.size() == si.size()) {
                memcpy(di.data(), si.data(), size_t(di.size()));
            }
        }
        dst->getMosaicQualityRef() = src.getMosaicQualityCRef();
        // heights & marks
        const auto& srcHeights = src.getHeightVerticesCRef();
        const auto& srcMarks = src.getHeightMarkVerticesCRef();
        auto& dstHeights = dst->getHeightVerticesRef();
        auto& dstMarks = dst->getHeightMarkVerticesRef();
        if (!srcHeights.isEmpty() && srcHeights.size() == dstHeights.size()) {
            dstHeights = srcHeights;
        }
        if (!srcMarks.isEmpty() && srcMarks.size() == dstMarks.size()) {
            dstMarks = srcMarks;
        }

        dst->updateHeightIndices();
        dst->setIsUpdated(false);

        if (dst->getIsInited()) {
            initedNow.insert(dst);
        }
    }

    if (!initedNow.isEmpty()) {
        surfaceMeshPtr_->setTileUsed(initedNow, false); // поднимет счетчики, эвикт выключен чтобы не выгрузить возможно нужные тайлы
    }

}

bool MosaicProcessor::prefetchTiles(const QSet<TileKey> &keys, bool* cut) // подгрузка тайлов с hotCache, db (dataprocessor)
{
    if (sink_ && !keys.isEmpty()) {
        const TileMap got = sink_->takeBack(keys);
        if (!got.isEmpty()) {
            putTilesIntoMesh(got);
        }
        return !got.isEmpty();
    }
    if (!dataProcessor_ || keys.isEmpty()) {
        return false;
    }

    Q_ASSERT(QThread::currentThread() != dataProcessor_->thread());

    TileMap allGot;
    QSet<TileKey> missing;

    auto putAllGotTiles = [&]() -> bool {
        if (!allGot.isEmpty()) {
            putTilesIntoMesh(allGot);
        }

        return !allGot.isEmpty();
    };

    // забрать с хот кэша
    if (!QMetaObject::invokeMethod(dataProcessor_, "fetchFromHotCache", Qt::BlockingQueuedConnection, Q_RETURN_ARG(TileMap, allGot), Q_ARG(QSet<TileKey>, keys), Q_ARG(QSet<TileKey>*, &missing))) {
        qWarning() << "[prefetch] invoke fetchFromHotCache failed";
    }

    // вычесть то, чего нет в БД
    QSet<TileKey> waiting;
    if (!QMetaObject::invokeMethod(dataProcessor_, "filterNotFoundOut", Qt::BlockingQueuedConnection, Q_ARG(QSet<TileKey>, missing), Q_ARG(QSet<TileKey>*, &waiting))) {
        qWarning() << "[prefetch] invoke filterNotFoundOut failed";
    }

    // нечего ждать,  бд не готова - выход
    if (waiting.isEmpty() || !dataProcessor_->isDbReady()) {
        return putAllGotTiles();
    }

    // цикл ожидания поступлений из БД
    while (!waiting.isEmpty()) {
        // попросить БД
        QMetaObject::invokeMethod(dataProcessor_, "requestTilesFromDB", Qt::QueuedConnection, Q_ARG(QSet<TileKey>, waiting));

        // засечь тик и уснуть до прогресса
        quint64 t0 = 0;
        if (!QMetaObject::invokeMethod(dataProcessor_, "prefetchProgressTick", Qt::BlockingQueuedConnection, Q_RETURN_ARG(quint64, t0))) {
            qWarning() << "[prefetch] invoke prefetchProgressTick failed";
        }

        // корректное ожидание через паблик-API
        dataProcessor_->prefetchWait(t0);

        if (canceled()) {
            if (cut) {
                *cut = true;   // what is still in `waiting` was not fetched
            }
            break;
        }

        // попробуем снова забрать с горячего кеша (в него пишет БД)
        QSet<TileKey> stillMissing;
        TileMap newlyGot;

        if (!QMetaObject::invokeMethod(dataProcessor_, "fetchFromHotCache", Qt::BlockingQueuedConnection, Q_RETURN_ARG(TileMap, newlyGot), Q_ARG(QSet<TileKey>, waiting), Q_ARG(QSet<TileKey>*, &stillMissing))) {
            qWarning() << "[prefetch] invoke fetchFromHotCache (retry) failed";
        }

        // что осталось уйдёт в ожидание
        if (!QMetaObject::invokeMethod(dataProcessor_, "filterNotFoundOut", Qt::BlockingQueuedConnection, Q_ARG(QSet<TileKey>, stillMissing), Q_ARG(QSet<TileKey>*, &stillMissing))) {
            qWarning() << "[prefetch] invoke filterNotFoundOut (retry) failed";
        }

        // аккумуляция результата
        for (auto it = newlyGot.cbegin(); it != newlyGot.cend(); ++it) {
            allGot.insert(it.key(), it.value());
        }

        // на следующий поиск
        waiting.swap(stillMissing);
    }

    // выход
    return putAllGotTiles();
}

QVector<QVector<int> > MosaicProcessor::splitContinuousSegments(const QVector<int> &indxs, int minSegmentLen, int maxSegmentLen)
{
    QVector<QVector<int>> result;
    if (indxs.isEmpty()) {
        return result;
    }

    if (maxSegmentLen < minSegmentLen) {
        maxSegmentLen = minSegmentLen;
    }

    const int n = indxs.size();
    int startPos = 0;

    while (startPos < n) {
        int endPos = startPos;

        while (endPos + 1 < n &&
               indxs[endPos + 1] == indxs[endPos] + 1) {
            ++endPos;
        }

        const int runLen = endPos - startPos + 1;

        if (runLen >= minSegmentLen) {
            int segStartPos = startPos;

            while (segStartPos <= endPos) {
                int segEndPos = std::min(segStartPos + maxSegmentLen - 1, endPos);
                int segLen    = segEndPos - segStartPos + 1;

                if (segLen >= minSegmentLen) {
                    QVector<int> segment;
                    segment.reserve(segLen);
                    for (int i = segStartPos; i <= segEndPos; ++i) {
                        segment.push_back(indxs[i]);
                    }
                    result.append(std::move(segment));
                }

                if (segEndPos >= endPos) {
                    break;
                }

                segStartPos = segEndPos;
            }
        }

        startPos = endPos + 1;
    }

    return result;
}
