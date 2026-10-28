#include "data_processor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <QtConcurrent/QtConcurrentRun>
#include <QMetaObject>
#include <QEventLoop>
#include <QScopeGuard>
#include <QDebug>
#include <QDataStream>
#include <QFile>
#include <QtGlobal>
#include <cmath>
#include "bt_worker.h"
#include "compute_worker.h"
#include "mosaic_precomputer.h"
#include "dataset.h"
#include "down_view.h"
#include "data_processor_defs.h"


inline void markTilesBySource(TileMap& tiles, DataSource src)
{
    UpdateHint hint = UpdateHint::kUndefined;
    if (src == DataSource::kCalculation) {
        hint = UpdateHint::kAddOrUpdateTexture; // гарантированно обновить/создать текстуру
    } 
    else {
        hint = UpdateHint::kUndefined;          // кэш/БД: текстуру не трогаем
    }

    //const bool updated = (hint != UpdateHint::kUndefined);
    for (auto it = tiles.begin(); it != tiles.end(); ++it) {
        it.value().setUpdateHint(hint);
        //it.value().setIsUpdated(updated); // на всякий случай держим синхрон
    }
}

// static TileDiff diffTiles(const QSet<TileKey>& prev, const QSet<TileKey>& curr)
// {
//     TileDiff d;

//     d.stayed = prev;
//     d.stayed.intersect(curr);

//     d.added = curr;
//     d.added.subtract(prev);

//     d.removed = prev;
//     d.removed.subtract(curr);

//     return d;
// }

static inline uint32_t toMask(WorkSet s){ return static_cast<uint32_t>(s); }

// Mosaic quality: when the brightness model is measured (beamMaybeSchedule, beamEstimate).
static constexpr int   kBeamFirst      = 300;    // epochs with a bottom track before the first estimate
static constexpr uint32_t kBeamMaturePings = 2000;   // a model from fewer pings a side is measured again soon
static constexpr int   kBeamLast       = 20000;  // and the last re-estimate of live data
static constexpr int   kBeamMaxEpochs  = 4000;   // read for an estimate, evenly over the survey
static constexpr int   kBeamQuietMs    = 700;    // a file's bottom track comes in batches: after the last
static constexpr int   kBeamMaxWaitMs  = 2500;   // but no later than this after the first
static constexpr float kBeamRepaintDb  = 1.0f;   // a new model nearer than this to the one in use is not taken

DataProcessor::DataProcessor(QObject *parent, Dataset* datasetPtr)
    : QObject(parent),
    datasetPtr_(datasetPtr),
    worker_(nullptr),
    btWorker_(nullptr),
    state_(DataProcessorType::kUndefined),
    chartsCounter_(0),
    bottomTrackCounter_(0),
    epochCounter_(0),
    positionCounter_(0),
    attitudeCounter_(0),
    updateBottomTrackFromScene3d_(false),
    updateBottomTrackFromSettings_(false),
    updateBottomTrack_(false),
    updateSurface_(false),
    updateIsobaths_(false),
    updateMosaic_(false),
    isOpeningFile_(false),
    bottomTrackWindowCounter_(0),
    mosaicCounter_(0),
    tileResolution_(defaultTileResolution),
    surfaceEdgeLimitDirty_(false),
    bottomTrackFullRecalcPending_(false),
    pendingIsobathsWork_(false),
    cancelRequested_(false),
    shuttingDown_(false),
    suppressResults_(false),
    resetInProgress_(false),
    jobRunning_(false),
    nextRunPending_(false),
    requestedMask_(0),
    btBusy_(false),
    hotCache_(hotCacheMaxSize_, hotCacheMinSize_),
    dbReader_(nullptr),
    dbReaderInWork_(false),
    dbWriter_(nullptr),
    engineVer_(1),
    surfaceZoomChangedPending_(false),
    dbIsReady_(false),
    prefetchTick_(0),
    lastVisTilesZoom_(0),
    lastDataZoomIndx_(0)
{
    qRegisterMetaType<WorkBundle>("WorkBundle");
    qRegisterMetaType<DataProcessorType>("DataProcessorType");
    qRegisterMetaType<DatasetChannel>("DatasetChannel");
    qRegisterMetaType<ChannelId>("ChannelId");
    qRegisterMetaType<BottomTrackParam>("BottomTrackParam");
    qRegisterMetaType<BottomTrackUpdate>("BottomTrackUpdate");
    qRegisterMetaType<QVector<BottomTrackUpdate>>("QVector<BottomTrackUpdate>");
    qRegisterMetaType<QVector<IsobathUtils::LabelParameters>>("QVector<IsobathUtils::LabelParameters>");
    qRegisterMetaType<TileMap>("TileMap");
    qRegisterMetaType<Dataset*>("Dataset*");
    qRegisterMetaType<std::uint8_t>("std::uint8_t");
    qRegisterMetaType<std::vector<uint8_t>>("std::vector<uint8_t>");
    qRegisterMetaType<QSet<TileKey>>("QSet<TileKey>");
    qRegisterMetaType<QVector<int>>("QVector<int>");

    hotCache_.setDataProcessorPtr(this);

    pendingWorkTimer_.setParent(this);
    pendingWorkTimer_.setSingleShot(true);
    pendingWorkTimer_.setInterval(333);
    connect(&pendingWorkTimer_, &QTimer::timeout, this, &DataProcessor::runCoalescedWork);
    latClock_.start();

    cameraRectCoalesceTimer_.setParent(this);
    cameraRectCoalesceTimer_.setSingleShot(true);
    cameraRectCoalesceTimer_.setInterval(8);
    connect(&cameraRectCoalesceTimer_, &QTimer::timeout, this, [this]() {
        if (shuttingDown_.load() || !cameraRectPending_) {
            return;
        }

        const QRectF rect = pendingCameraRect_;
        cameraRectPending_ = false;
        cameraRectProcessing_ = true;
        onSendDataRectRequest(rect.left(), rect.top(), rect.right(), rect.bottom());
        cameraRectProcessing_ = false;

        if (cameraRectPending_ && !cameraRectCoalesceTimer_.isActive()) {
            cameraRectCoalesceTimer_.start();
        }
    });

    worker_ = new ComputeWorker(this, datasetPtr_);
    worker_->setDatasetPtr(datasetPtr_);

    worker_->moveToThread(&computeThread_);

    connect(worker_, &ComputeWorker::jobFinished,          this, &DataProcessor::onWorkerFinished,      Qt::QueuedConnection);

    computeThread_.setObjectName("ComputeWorkerThread");
    computeThread_.start();

    precomputer_ = new MosaicPrecomputer(this);
    precomputer_->setDatasetPtr(datasetPtr_);   // as the worker gets it: setDatasetPtr is not called later
    precomputer_->setLatestGeneration(precomputeGeneration_);
    precomputer_->moveToThread(&precomputeThread_);
    precomputeThread_.setObjectName("MosaicPrecomputeThread");
    precomputeThread_.start(QThread::LowPriority);
    precomputeTimer_.setParent(this);
    precomputeTimer_.setSingleShot(true);
    precomputeTimer_.setInterval(300);
    connect(&precomputeTimer_, &QTimer::timeout, this, &DataProcessor::maybeStartPrecompute);
    beamTimer_.setParent(this);
    beamTimer_.setSingleShot(true);
    connect(&beamTimer_, &QTimer::timeout, this, &DataProcessor::beamEstimate);

    editRebuildTimer_.setParent(this);
    editRebuildTimer_.setSingleShot(true);
    editRebuildTimer_.setInterval(250);
    connect(&editRebuildTimer_, &QTimer::timeout, this, &DataProcessor::runEditRebuild);

    btWorker_ = new BtWorker(this, datasetPtr_);
    btWorker_->moveToThread(&btThread_);

    connect(btWorker_, &BtWorker::bottomTrackStarted,  this, &DataProcessor::onBottomTrackStarted,  Qt::QueuedConnection);
    connect(btWorker_, &BtWorker::bottomTrackFinished, this, &DataProcessor::onBottomTrackFinished, Qt::QueuedConnection);

    btThread_.setObjectName("BtWorkerThread");
    btThread_.start();
}

DataProcessor::~DataProcessor()
{
    if (!shuttingDown_.load()) {
        shutdown();
    }

    if (precomputer_) {
        cancelPrecompute();
        if (precomputeThread_.isRunning()) {
            precomputeThread_.quit();
            precomputeThread_.wait();
        }
        delete precomputer_;
        precomputer_ = nullptr;
    }

    if (worker_) {
        if (computeThread_.isRunning()) {
            computeThread_.quit();
            computeThread_.wait();
        }
        delete worker_;
        worker_ = nullptr;
    }

    if (btWorker_) {
        if (btThread_.isRunning()) {
            btThread_.quit();
            btThread_.wait();
        }
        delete btWorker_;
        btWorker_ = nullptr;
    }
}

void DataProcessor::setDatasetPtr(Dataset *datasetPtr)
{
    datasetPtr_ = datasetPtr;

    QMetaObject::invokeMethod(worker_,   "setDatasetPtr", Qt::QueuedConnection, Q_ARG(Dataset*, datasetPtr_));
    QMetaObject::invokeMethod(precomputer_, "setDatasetPtr", Qt::QueuedConnection, Q_ARG(Dataset*, datasetPtr_));
    QMetaObject::invokeMethod(btWorker_, "setDatasetPtr", Qt::QueuedConnection, Q_ARG(Dataset*, datasetPtr_));
    updateDatasetSpatialIndexingState();
}

void DataProcessor::setSuppressResults(bool state) noexcept
{
    const bool wasSuppressed = suppressResults_.load();
    suppressResults_.store(state);
    if (state) {
        notifyPrefetchProgress();
        return;
    }
    if (wasSuppressed) {
        startTimerIfNeeded();
    }
}

void DataProcessor::updateIdle()
{
    const bool job   = jobRunning_.load();
    const bool timer = pendingWorkTimer_.isActive();
    const bool idle  = !job && !btBusy_ && !timer;

    QVariantMap activity;
    activity[QStringLiteral("idle")]            = idle;
    activity[QStringLiteral("jobRunning")]      = job;
    activity[QStringLiteral("btBusy")]          = btBusy_;
    activity[QStringLiteral("timerActive")]     = timer;
    activity[QStringLiteral("nextRunPending")]  = nextRunPending_.load();
    activity[QStringLiteral("suppressResults")] = suppressResults_.load();
    activity[QStringLiteral("resetInProgress")] = resetInProgress_.load();
    activity[QStringLiteral("state")]           = static_cast<int>(state_);
    if (activity != lastActivity_) {
        lastActivity_ = activity;
        emit activityChanged(activity);
    }

    if (idle == lastIdle_) {
        return;
    }
    lastIdle_ = idle;
    emit idleChanged(idle);
}

void DataProcessor::prepareForFileClose(int timeoutMs)
{
    auto idleGuard = qScopeGuard([this] { updateIdle(); });
    if (timeoutMs < 0) {
        timeoutMs = 0;
    }

    suppressResults_.store(true);
    pendingWorkTimer_.stop();
    requestCancel();
    notifyPrefetchProgress();

    if (!jobRunning_.load() && !btBusy_) {
        return;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(worker_,   &ComputeWorker::jobFinished,         &loop, &QEventLoop::quit);
    QObject::connect(btWorker_, &BtWorker::bottomTrackFinished,      &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);

    while ((jobRunning_.load() || btBusy_) && timer.isActive()) {
        loop.exec();
    }
}

void DataProcessor::setBottomTrackPtr(BottomTrack *bottomTrackPtr)
{
    QMetaObject::invokeMethod(worker_, "setBottomTrackPtr", Qt::QueuedConnection, Q_ARG(BottomTrack*, bottomTrackPtr));
}

void DataProcessor::clearProcessing(DataProcessorType procType)
{
    // TODO

    requestCancel();

    switch (procType) {
    case DataProcessorType::kUndefined:   clearAllProcessings();        emit allProcessingCleared();         break;
    case DataProcessorType::kBottomTrack: clearBottomTrackProcessing(); emit bottomTrackProcessingCleared(); break;
    case DataProcessorType::kIsobaths:    clearIsobathsProcessing();    emit isobathsProcessingCleared();    break;
    case DataProcessorType::kMosaic:      clearMosaicProcessing();      emit mosaicProcessingCleared();      break;
    case DataProcessorType::kSurface:     clearSurfaceProcessing();     emit surfaceProcessingCleared();     break;
    default: break;
    }

    // prefetch db
    clearDbNotFoundCache();
    prefetchTick_ = 0;

    chartsCounter_ = 0;
    bottomTrackCounter_ = 0;
    epochCounter_ = 0;
    positionCounter_ = 0;
    attitudeCounter_ = 0;
    mosaicCounter_ = 0;

    tileEpochIndxsByZoom_.clear();
    latencyReset();
    tileEpochIndxsByZoom_.shrink_to_fit();
    indexedEpochs_.clear();
    precomputeAllTiles_.clear();
    precomputeScanFrom_.clear();
    precomputeTried_.clear();
    surfaceTaskEpochIndxsByZoom_.clear();
    surfaceManualEpochIndxsByZoom_.clear();
    mosaicTaskEpochIndxsByZoom_.clear();
    mosaicInFlightIndxs_.clear();

    postState(DataProcessorType::kUndefined); //
}

void DataProcessor::resetProcessingPipeline()
{
    auto idleGuard = qScopeGuard([this] { updateIdle(); });
    if (resetInProgress_.exchange(true)) {
        return;
    }

    // Prevent scheduling of new work while reset drains active surface/mosaic jobs.
    pendingWorkTimer_.stop();
    cameraRectCoalesceTimer_.stop();
    requestedMask_.store(0);
    nextRunPending_.store(false);
    if (jobRunning_.load() && !btBusy_) {
        requestCancel();
    }

    tryFinalizeResetProcessing();
}

void DataProcessor::tryFinalizeResetProcessing()
{
    auto idleGuard = qScopeGuard([this] { updateIdle(); });
    if (!resetInProgress_.load()) {
        return;
    }

    if (jobRunning_.load()) {
        return;
    }

    // Reset only surface/mosaic/isobaths pipeline and caches.
    // Bottom-track data/processing must stay untouched.
    pendingWorkTimer_.stop();
    cameraRectCoalesceTimer_.stop();
    requestedMask_.store(0);
    nextRunPending_.store(false);
    cancelRequested_.store(false);

    pendingSurfaceIndxs_.clear();
    pendingMosaicIndxs_.clear();
    mosaicInFlightIndxs_.clear();
    clearIsobathsProcessing();
    clearSurfaceProcessing();
    clearMosaicProcessing();

    hotCache_.clear();
    renderedKeys_.clear();
    dbPendingKeys_.clear();
    dbInWorkKeys_.clear();
    dbReaderInWork_ = false;
    clearDbNotFoundCache();

    // Reset worker-side surface/mosaic context (including mesh cache), not bottom-track.
    QMetaObject::invokeMethod(worker_, "clearSurfaceMosaicContext", Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(worker_, "setVisibleTileKeys", Qt::BlockingQueuedConnection,
                              Q_ARG(QSet<TileKey>, QSet<TileKey>()));
    requeueViewRect();   // the worker's tiles are gone, and with an unchanged view no rect comes
    cancelPrecompute();
    QMetaObject::invokeMethod(precomputer_, "clear", Qt::QueuedConnection);

    closeDB();

    state_ = DataProcessorType::kUndefined;
    postState(DataProcessorType::kUndefined);
    emit surfaceProcessingCleared();
    emit mosaicProcessingCleared();
    emit isobathsProcessingCleared();

    resetInProgress_.store(false);
}

void DataProcessor::requeueViewRect()
{
    // The view sends its rect only when its camera moves, and after a clear nothing has moved: the
    // last rect is still the screen. Dropped with the visible tiles, a live mosaic did not start
    // after connecting until the user touched the zoom (2026-10-17, the owner's report).
    lastViewRect_ = QRectF();
    lastVisTileKeys_.clear();
    lastVisTilesZoom_ = 0;
    if (!haveViewRect_) {
        return;
    }
    pendingCameraRect_ = viewRectFromView_;
    cameraRectPending_ = true;
    if (!cameraRectCoalesceTimer_.isActive()) {
        cameraRectCoalesceTimer_.start();
    }
}

void DataProcessor::cancelPrecompute(bool keepProduced)
{
    // A run cancelled already keeps what that first cancel decided (kept, or revoked by one that does
    // not keep): a second zoom while it winds down must not move the kept generation to one no run
    // sends with.
    if (!keepProduced || !precomputeRunning_) {
        precomputeKeepGen_ = 0;
    }
    else if (precomputeGeneration_ == precomputeRunGen_) {
        precomputeKeepGen_ = precomputeRunGen_;
    }
    ++precomputeGeneration_;
    precomputeLastTarget_ = -1;
    // The running run's epochs go to be sent again; those it reports before it stops (kept) are done
    // by then and are not.
    if (precomputeRunning_ && precomputeRunTarget_ >= 0) {
        auto& tried = precomputeTried_[precomputeRunTarget_];
        for (const int idx : std::as_const(precomputeRunEpochs_)) {
            tried.remove(idx);
        }
        if (!precomputeRunEpochs_.isEmpty()) {
            int& from = precomputeScanFrom_[precomputeRunTarget_];
            from = std::min(from, precomputeRunEpochs_.front());
        }
    }
    if (precomputer_) {
        precomputer_->setLatestGeneration(precomputeGeneration_);
    }
}

bool DataProcessor::precomputeAllowed() const
{
    static const QString env = qEnvironmentVariable("KOGGER_MOSAIC_PRECOMPUTE");
    if (env == QStringLiteral("0")) {
        return false;
    }
    return !env.isEmpty() || precomputeSetting_;
}

void DataProcessor::setMosaicPrecomputeEnabled(bool on)
{
    if (precomputeSetting_ == on) {
        return;
    }
    precomputeSetting_ = on;
    qInfo().noquote() << QStringLiteral("mosaic precompute: %1 in the settings%2").arg(on ? QStringLiteral("on") : QStringLiteral("off"))
                         .arg(qEnvironmentVariableIsSet("KOGGER_MOSAIC_PRECOMPUTE") ? QStringLiteral(" (KOGGER_MOSAIC_PRECOMPUTE decides)") : QString());
    if (!precomputeAllowed()) {
        // What it painted stays: correct, and dropped as before when it goes stale (dropPrecomputed).
        precomputeTimer_.stop();
        cancelPrecompute(true);
    }
    else {
        precomputeLastTarget_ = -1;
        precomputeTimer_.start();
    }
}

void DataProcessor::setMosaicMultiThreaded(bool on)
{
    qInfo().noquote() << QStringLiteral("mosaic: painted on %1").arg(on ? QStringLiteral("several threads") : QStringLiteral("one thread"));
    QMetaObject::invokeMethod(worker_, "setMosaicPaintThreads", Qt::QueuedConnection, Q_ARG(int, on ? 0 : 1));
    if (precomputer_) {
        QMetaObject::invokeMethod(precomputer_, "setPaintThreads", Qt::QueuedConnection, Q_ARG(int, on ? 0 : 1));
    }
}

void DataProcessor::forgetPrecomputeSent(int zoom)
{
    if (zoom < 0) {
        precomputeTried_.clear();
        precomputeScanFrom_.clear();
        return;
    }
    precomputeTried_.remove(zoom);
    precomputeScanFrom_.remove(zoom);
}

void DataProcessor::resetMosaicProcessed()
{
    mosaicTaskEpochIndxsByZoom_.clear();
    forgetPrecomputeSent(-1);
    invalidateDbAll();
}

void DataProcessor::onDbWriterFailed()
{
    // Every tile evicted from now on would stay in RAM, blocked for an ACK that never comes: a level
    // painted ahead is thousands of them (2026-10-18). The view paints on a zoom, as before there was
    // a precompute.
    if (dbWriterFailed_) {
        return;
    }
    dbWriterFailed_ = true;
    qWarning().noquote() << QStringLiteral("mosaic precompute: off, the mosaic cache cannot be written");
    dropPrecomputed();
}

int DataProcessor::bottomTrackSettledBefore() const
{
    // Quiet a while: all of it (a file once read, a stream once stopped). While data streams it never
    // is, and before 2026-10-17 nothing was precomputed during a survey. A streaming run reworks only
    // its window [indexFrom, indexTo], and the next run's starts no earlier, so the epochs before the
    // latest run's start keep their track. Less a margin: a run's results reach onBottomTrack3DAdded
    // through the 3D scene and may come after the next run has started. A manual run reaching back
    // drops what was painted ahead anyway (dropPrecomputed).
    static const qint64 kQuietMs = qEnvironmentVariableIsSet("KOGGER_PRECOMPUTE_QUIET_MS")
        ? qEnvironmentVariableIntValue("KOGGER_PRECOMPUTE_QUIET_MS") : 1500;
    constexpr int kMinMarginEpochs = 32;   // 1 to 3 s of pings: painted at once after a zoom
    const int window = datasetPtr_ ? datasetPtr_->getBottomTrackParam().windowSize : 1;
    if (!btBusy_ && (!bottomTrackChanged_.isValid() || bottomTrackChanged_.elapsed() >= kQuietMs)) {
        // A paused live stream: when pings resume, the next run reworks from its counter less the
        // window, and the 3D bottom track re-reports max(3, window) before that
        // (Dataset::onLastBottomTrackEpochChanged); painting past it dropped every level ahead.
        if (datasetState_ == 2 && !isOpeningFile_ && bottomTrackWindowCounter_ > 0) {
            return bottomTrackWindowCounter_ - window - std::max(window, kMinMarginEpochs);
        }
        return std::numeric_limits<int>::max();
    }
    return btAutoFrom_ - std::max(window, kMinMarginEpochs);
}

void DataProcessor::dropPrecomputed()
{
    // Painted ahead with an older bottom track: not kept, a zoom there paints afresh (or the precompute
    // again, once the track has settled). Their keys go to the DB's not-found list, so a copy saved
    // there earlier is not brought back either.
    precomputeMaxEpoch_ = -1;
    if (!precomputeRunning_ && precomputedZooms_.isEmpty()) {
        return;
    }
    cancelPrecompute();
    for (const int zoom : std::as_const(precomputedZooms_)) {
        if (zoom == lastDataZoomIndx_) {
            continue;
        }
        for (const TileKey& k : hotCache_.removeZoom(zoom)) {
            nfTouch(k);
        }
        mosaicTaskEpochIndxsByZoom_.remove(zoom);
        forgetPrecomputeSent(zoom);
        invalidateDbZoom(zoom);   // most of a level painted ahead is only in the DB
    }
    precomputedZooms_.clear();
}

void DataProcessor::maybeStartPrecompute()
{
    // The view's own work first, then the other levels (below). Not with FAKE_COORDS+N, where a zoom
    // change restarts the mosaic anyway.
    constexpr int kMinZoom = 2;   // the levels pickZoomByDistance gives
    constexpr int kMaxZoom = 6;
    static const bool kTrace = qEnvironmentVariableIsSet("KOGGER_PRECOMPUTE_TRACE");
    static QString lastWait;
    auto waiting = [](const QString& why) {
        if (kTrace && why != lastWait) {
            lastWait = why;
            qInfo().noquote() << QStringLiteral("mosaic precompute: waits, %1").arg(why);
        }
    };
    if (!precomputeAllowed() || !precomputer_ || precomputeRunning_ || shuttingDown_.load()) {
        return;   // a running one arms the timer when it ends
    }
    if (dbWriterFailed_) {
        waiting(QStringLiteral("the mosaic cache cannot be written"));
        return;
    }
    if (resetInProgress_.load() || suppressResults_.load() || isOpeningFile_) {
        waiting(QStringLiteral("a reset, a file opening or results held back"));   // the next job's end arms it again
        return;
    }
    if (beamHolding()) {
        waiting(QStringLiteral("the brightness model"));   // the view's first job, after it, arms it again
        return;
    }
    if (!updateMosaic_ || (activeZeroing_ && mosaicFakeCoordsLastN_ > 0)) {
        return;
    }
    // Epochs queued past the mosaic horizon wait for it, not the view's worker: in a file the last
    // few stay there for good, and waiting for them never let the precompute start.
    int ready = 0;
    for (const int idx : std::as_const(pendingMosaicIndxs_)) {
        ready += idx <= mosaicCounter_;
    }
    if (jobRunning_.load() || ready > 0 || !mosaicInFlightIndxs_.isEmpty()) {
        waiting(QStringLiteral("job %1, %2 queued, %3 in flight").arg(jobRunning_.load()).arg(ready).arg(mosaicInFlightIndxs_.size()));
        precomputeTimer_.start();
        return;
    }
    if (!haveViewRect_ || !isValidZoomIndx(lastDataZoomIndx_)) {
        waiting(QStringLiteral("no view"));
        return;
    }
    // Only epochs whose bottom track will not change: the colours follow it (the slant range), and a
    // tile painted before a change keeps its older pings.
    const int settledBefore = bottomTrackSettledBefore();
    if (settledBefore <= 0) {
        waiting(QStringLiteral("the bottom track is changing"));
        precomputeTimer_.start();
        return;
    }
    lastWait.clear();

    // Every level but the view's, nearest first, at the same distance the finer one; each over the
    // whole survey, every tile with data and every epoch not painted there, in order. The owner,
    // 2026-10-18, of the build before: near zooms still repainted. That one took 320 tiles a level
    // about the cursor, and an epoch reaching past them stayed unfinished -- at level 2 from an
    // overview 4 413 of 5 252 -- so the view painted them again; and painting one level in pieces of
    // tiles is not painting it whole: a tile's head index (MosaicProcessor's pixel()) rejects a pair
    // older than what was painted into it, and the pieces do not reach a tile in order. The whole level
    // in one run is what the view does over a view as large. Results that go past the hot cache's
    // 2 048 tiles are saved in the session's DB and come back from there on a zoom.
    const int zoom = lastDataZoomIndx_;
    QVector<int> targets;
    QStringList idle;   // KOGGER_PRECOMPUTE_TRACE: why each level had nothing to paint
    for (int d = 1; d <= kMaxZoom - kMinZoom; ++d) {
        if (zoom - d >= kMinZoom) {
            targets.append(zoom - d);
        }
        if (zoom + d <= kMaxZoom) {
            targets.append(zoom + d);
        }
    }
    // From the level after the last one run, round: while data streams the nearest always has new
    // epochs, and the others never got a run (2026-10-17: 100 runs of 101 on the next level).
    if (const int at = targets.indexOf(precomputeLastTarget_); at >= 0) {
        std::rotate(targets.begin(), targets.begin() + at + 1, targets.end());
    }
    const int upTo = std::min(mosaicCounter_, settledBefore == std::numeric_limits<int>::max() ? mosaicCounter_ : settledBefore - 1);
    const int indexed = int(indexedEpochs_.size());
    for (const int target : std::as_const(targets)) {
        static const QSet<int> kNoEpochs;
        const auto doneIt = mosaicTaskEpochIndxsByZoom_.constFind(target);
        const QSet<int>& done = doneIt != mosaicTaskEpochIndxsByZoom_.cend() ? doneIt.value() : kNoEpochs;
        auto& tried = precomputeTried_[target];
        int& from = precomputeScanFrom_[target];   // back to 0 where the processed sets are reset (resetMosaicProcessed)
        // The leading epochs painted, sent or without tiles are not looked at again; one without tiles
        // only well behind the newest, which may be indexed yet (its segment needs the next ping).
        auto settled = [&](int e) {
            return done.contains(e) || tried.contains(e) || (!indexedEpochs_[size_t(e)] && e < upTo - 64);
        };
        while (from <= upTo && from < indexed && settled(from)) {
            ++from;
        }
        QVector<int> list;
        for (int e = from; e <= upTo && e < indexed; ++e) {
            if (indexedEpochs_[size_t(e)] && !done.contains(e) && !tried.contains(e)) {
                list.append(e);
            }
        }
        if (list.isEmpty()) {
            if (kTrace) {
                idle << QStringLiteral("%1: all painted or sent up to %2 (the horizon %3, the bottom track settled before %4)")
                        .arg(target).arg(from).arg(mosaicCounter_).arg(settledBefore);
            }
            continue;
        }
        const QSet<TileKey> tiles = precomputeAllTiles_.value(target);
        if (tiles.isEmpty()) {
            if (kTrace) {
                idle << QStringLiteral("%1: no tiles with data").arg(target);
            }
            continue;
        }
        for (const int idx : std::as_const(list)) {
            tried.insert(idx);
        }
        precomputeRunning_ = true;
        precomputeRunGen_ = precomputeGeneration_;
        precomputeLastTarget_ = target;
        precomputeRunTarget_ = target;
        precomputeRunEpochs_ = list;
        precomputeClock_.start();
        precomputeMaxEpoch_ = std::max(precomputeMaxEpoch_, list.back());
        precomputeRunInfo_ = QStringLiteral("zoom %1 (the view is on %2), %3 epochs over %4 tiles")
                             .arg(target).arg(zoom).arg(list.size()).arg(tiles.size());
        if (settledBefore != std::numeric_limits<int>::max()) {
            precomputeRunInfo_ += QStringLiteral(", streaming: epochs before %1").arg(settledBefore);
        }
        QMetaObject::invokeMethod(precomputer_, "run", Qt::QueuedConnection, Q_ARG(int, target),
                                  Q_ARG(QSet<TileKey>, tiles), Q_ARG(QVector<int>, list), Q_ARG(quint64, precomputeGeneration_));
        return;
    }
    if (kTrace) {
        waiting(QStringLiteral("nothing to paint (view on %1): %2").arg(zoom).arg(idle.join(QStringLiteral("; "))));
    }
}

void DataProcessor::postPrecomputedTiles(TileMap tiles, quint64 generation)
{
    const bool wanted = generation == precomputeGeneration_ || (precomputeKeepGen_ != 0 && generation == precomputeKeepGen_);
    if (!wanted || tiles.isEmpty() || suppressResults_.load()) {
        return;
    }
    for (auto it = tiles.cbegin(); it != tiles.cend(); ++it) {
        nfErase(it.key());
    }
    if (const int zoom = tiles.cbegin().key().zoom; zoom != lastDataZoomIndx_) {
        precomputedZooms_.insert(zoom);   // another level: nothing to render
    }
    else if (updateMosaic_) {
        // A run kept over a zoom to its own level (cancelPrecompute(true)): its tiles on screen may
        // be older copies already rendered, and its epochs are done now, so the view repaints nothing.
        TileMap visible;
        for (auto it = tiles.cbegin(); it != tiles.cend(); ++it) {
            if (lastVisTileKeys_.contains(it.key())) {
                visible.insert(it.key(), it.value());
            }
        }
        if (!visible.isEmpty()) {
            emitDelta(std::move(visible), DataSource::kCalculation);
        }
    }
    hotCache_.putBatch(std::move(tiles), DataSource::kCalculation);
    notifyPrefetchProgress();
}

void DataProcessor::onMosaicEpochsPrecomputed(const QVector<int>& indxs, int zoom, quint64 generation)
{
    const bool wanted = generation == precomputeGeneration_ || (precomputeKeepGen_ != 0 && generation == precomputeKeepGen_);
    if (!wanted || indxs.isEmpty() || suppressResults_.load()) {
        return;
    }
    auto& processed = mosaicTaskEpochIndxsByZoom_[zoom];
    for (const int idx : indxs) {
        processed.insert(idx);
    }
}

void DataProcessor::onPrecomputeFinished(quint64 generation)
{
    qInfo().noquote() << QStringLiteral("mosaic precompute: %1, %2 ms%3").arg(precomputeRunInfo_).arg(precomputeClock_.elapsed())
                         .arg(generation == precomputeGeneration_ ? QString() : QStringLiteral(", cancelled"));
    precomputeRunning_ = false;
    precomputeRunTarget_ = -1;
    precomputeRunEpochs_.clear();
    if (generation == precomputeKeepGen_) {
        precomputeKeepGen_ = 0;   // all it sent is in by now (one thread, in order)
    }
    handleSurfaceZoomChangeIfReady(lastDataZoomIndx_, lastVisTileKeys_);   // waited for it (above)
    if (requestedMask_.load() != 0) {
        startTimerIfNeeded();   // the view's job waited for it (runCoalescedWork)
    }
    precomputeTimer_.start();   // the next level, or the view it has moved to
}

void DataProcessor::setRealtimePaused(bool paused)
{
    if (realtimePaused_ == paused) {
        return;
    }
    realtimePaused_ = paused;
    applyUpdateBottomTrackSourceState(wantedBottomTrackScene3d_ && !paused, false);
    applyUpdateBottomTrackSourceState(wantedBottomTrackSettings_ && !paused, true);
    applyUpdateSurface(wantedSurface_ && !paused);
    applyUpdateIsobaths(wantedIsobaths_ && !paused);
    applyUpdateMosaic(wantedMosaic_ && !paused);
}

void DataProcessor::setUpdateBottomTrackSourceState(bool state, bool fromSettings)
{
    if (fromSettings) {
        wantedBottomTrackSettings_ = state;
    }
    else {
        wantedBottomTrackScene3d_ = state;
    }
    applyUpdateBottomTrackSourceState(state && !realtimePaused_, fromSettings);
}

void DataProcessor::setUpdateSurface(bool state)
{
    wantedSurface_ = state;
    applyUpdateSurface(state && !realtimePaused_);
}

void DataProcessor::setUpdateIsobaths(bool state)
{
    wantedIsobaths_ = state;
    applyUpdateIsobaths(state && !realtimePaused_);
}

void DataProcessor::setUpdateMosaic(bool state)
{
    wantedMosaic_ = state;
    applyUpdateMosaic(state && !realtimePaused_);
}

void DataProcessor::setUpdateBottomTrack(bool state)
{
    setUpdateBottomTrackSourceState(state, false);
}

void DataProcessor::setUpdateBottomTrackFromSettings(bool state)
{
    setUpdateBottomTrackSourceState(state, true);
}

void DataProcessor::applyUpdateBottomTrackSourceState(bool state, bool fromSettings)
{
    if (fromSettings) {
        updateBottomTrackFromSettings_ = state;
    }
    else {
        updateBottomTrackFromScene3d_ = state;
    }

    updateBottomTrack_ = updateBottomTrackFromScene3d_ || updateBottomTrackFromSettings_;
    updateDatasetSpatialIndexingState();

    if (resetInProgress_.load()) {
        return;
    }

    if ((updateBottomTrack_ || updateSurface_ || updateIsobaths_ || updateMosaic_) && !pendingSurfaceIndxs_.empty()) {
        scheduleLatest(WorkSet(WF_Surface));
    }
}

void DataProcessor::applyUpdateSurface(bool state)
{
    const bool wasSurface = updateSurface_;
    updateSurface_ = state;
    updateDatasetSpatialIndexingState();

    if (resetInProgress_.load()) {
        return;
    }

    if (updateSurface_ && !wasSurface) {
        surfaceCameraPassPending_ = true;
        if (!lastVisTileKeys_.isEmpty()) {
            QMetaObject::invokeMethod(worker_, "setVisibleTileKeys", Qt::QueuedConnection, Q_ARG(QSet<TileKey>, lastVisTileKeys_));
        }

        int zoom = lastVisTilesZoom_;
        if (!isValidZoomIndx(zoom)) {
            zoom = lastDataZoomIndx_;
        }
        if (zoom <= 0) {
            zoom = zoomFromMpp(tileResolution_);
        }
        if (zoom > 0) {
            auto& processedSurface = surfaceTaskEpochIndxsByZoom_[zoom];
            const auto& manualPending = surfaceManualEpochIndxsByZoom_[zoom];

            auto isPendingAnyType = [this](int idx) -> bool {
                return pendingSurfaceIndxs_.contains(qMakePair('0', idx)) ||
                       pendingSurfaceIndxs_.contains(qMakePair('1', idx)) ||
                       pendingSurfaceIndxs_.contains(qMakePair('2', idx));
            };

            bool addedVisibleSeeds = false;
            const QSet<int> visibleSurfaceEpochs = collectVisibleSurfaceEpochsSet(zoom);
            for (int idx : std::as_const(visibleSurfaceEpochs)) {
                if (processedSurface.contains(idx) || manualPending.contains(idx) || isPendingAnyType(idx)) {
                    continue;
                }
                pendingSurfaceIndxs_.insert(qMakePair('0', idx));
                addedVisibleSeeds = true;
            }

            // Bootstrap fallback: if visible mapping is not ready yet, force triangulation refresh
            // from currently known bottom-track vertices (single toggle-time operation).
            if (!addedVisibleSeeds && !vertIndxsFromBottomTrack_.isEmpty()) {
                for (int idx : std::as_const(vertIndxsFromBottomTrack_)) {
                    if (isPendingAnyType(idx)) {
                        continue;
                    }
                    pendingSurfaceIndxs_.insert(qMakePair('2', idx));
                }
            }
        }
        onCameraMoved();
        scheduleLatest(WorkSet(WF_Surface));
    }
    if (!updateSurface_) {
        surfaceCameraPassPending_ = false;
    }
}

void DataProcessor::applyUpdateIsobaths(bool state)
{
    updateIsobaths_ = state;
    updateDatasetSpatialIndexingState();

    if (resetInProgress_.load()) {
        return;
    }

    if (updateIsobaths_) {
        scheduleLatest(WorkSet(WF_Isobaths));
    }
}

void DataProcessor::applyUpdateMosaic(bool state)
{
    const bool wasMosaic = updateMosaic_;
    updateMosaic_ = state;
    updateDatasetSpatialIndexingState();

    if (resetInProgress_.load()) {
        return;
    }

    //return;

    if (updateMosaic_) {
        if (!wasMosaic) {
            pendingMosaicIndxs_.clear();
            mosaicInFlightIndxs_.clear();
            mosaicBootstrapPending_ = true;
        }

        if (hotCache_.checkAnyTileForZoom(lastDataZoomIndx_)) {
            pumpVisible();
        }
        else {
            if (dbIsReady_) {
                emit dbCheckAnyTileForZoom(lastDataZoomIndx_);
            }
        }
        onCameraMoved();

        if (!wasMosaic && !epIndxsFromBottomTrack_.isEmpty()) {
            const bool hadPending = !pendingMosaicIndxs_.isEmpty();
            for (int ep : std::as_const(epIndxsFromBottomTrack_)) {
                pendingMosaicIndxs_.insert(ep);
            }
            if (!hadPending) {
                scheduleLatest(WorkSet(WF_Mosaic));
            }
        }
    }
    else {
        pendingMosaicIndxs_.clear();
        mosaicInFlightIndxs_.clear();
        mosaicBootstrapPending_ = false;
        pendingIsobathsWork_ = true; // мозаика могла изменить поверхность
        scheduleLatest(WorkSet(WF_Isobaths));
    }
}

void DataProcessor::setIsOpeningFile(bool state)
{
    isOpeningFile_ = state;
    updateDatasetSpatialIndexingState();

    updateDataProcType();
}

void DataProcessor::onCameraMoved()
{
    if (resetInProgress_.load()) {
        return;
    }

    if (!updateMosaic_ && !updateSurface_) {
        return;
    }

    if (!lastVisTileKeys_.isEmpty()) {
        mosaicBootstrapPending_ = false;
    }

    // Keep already queued mosaic epochs (e.g. from realtime bottom-track updates).
    // Camera movement should only add visible work, not drop pending work.
    //if (updateSurface_) {
    //    pendingSurfaceIndxs_.clear();
    //}
    //if (updateIsobaths_) {
    //    updateIsobaths_ = false;
    //}

    int zoom = lastVisTilesZoom_;
    if (!isValidZoomIndx(zoom)) {
        zoom = lastDataZoomIndx_;
    }
    if (zoom <= 0) {
        zoom = zoomFromMpp(tileResolution_);
    }

    const QSet<int> visibleSurfaceEpochs = collectVisibleSurfaceEpochsSet(zoom);
    const QSet<int> visibleMosaicEpochs  = updateMosaic_ ? collectEpochsForTilesSet(zoom, lastVisTileKeys_) : QSet<int>();

    const int mosaicLowAllowed = (activeZeroing_ && mosaicFakeCoordsLastN_ > 0)
        ? std::max(0, mosaicCounter_ - mosaicFakeCoordsLastN_ + 1)
        : 0;

    if (updateMosaic_ && !pendingMosaicIndxs_.isEmpty() && !lastVisTileKeys_.isEmpty()) {
        // Keep mosaic queue focused on current viewport to avoid processing stale backlog
        // accumulated during rapid camera moves.
        QSet<int> filteredPending;
        filteredPending.reserve(qMin(pendingMosaicIndxs_.size(), visibleMosaicEpochs.size()));
        for (int idx : std::as_const(pendingMosaicIndxs_)) {
            if (visibleMosaicEpochs.contains(idx) && idx >= mosaicLowAllowed) {
                filteredPending.insert(idx);
            }
        }
        pendingMosaicIndxs_.swap(filteredPending);
    }

    auto& processedSurface = surfaceTaskEpochIndxsByZoom_[zoom];
    auto& processedMosaic = mosaicTaskEpochIndxsByZoom_[zoom];

    bool addedSurface = false;
    bool addedMosaic = false;
    const bool needCameraPass = updateSurface_ && surfaceCameraPassPending_;

    for (int itm : std::as_const(visibleSurfaceEpochs)) {
        const QPair<char, int> pair('0', itm);
        if (!processedSurface.contains(itm) && !pendingSurfaceIndxs_.contains(pair)) {
            pendingSurfaceIndxs_.insert(pair);
            addedSurface = true;
        }
    }

    if (updateMosaic_) {
        for (int itm : std::as_const(visibleMosaicEpochs)) {
            if (itm < mosaicLowAllowed) {
                continue;
            }
            if (!processedMosaic.contains(itm) && !pendingMosaicIndxs_.contains(itm)) {
                pendingMosaicIndxs_.insert(itm);
                addedMosaic = true;
            }
        }
    }

    if (!addedSurface && !addedMosaic && !needCameraPass) {
        return;
    }

    WorkSet mask = WF_None;
    if (addedSurface || needCameraPass) {
        mask |= WF_Surface;
    }
    if (addedMosaic) {
        mask |= WF_Mosaic;
    }
    if (toMask(mask) != 0) {
        scheduleLatest(mask);
    }
}

void DataProcessor::onChartsAdded(uint64_t indx)
{
    chartsCounter_ = indx;
    latencyStamp(LatStage::Chart, latLastChart_ + 1, int(indx));
    latLastChart_ = std::max(latLastChart_, int(indx));
    // Fallback trigger: charts may appear after epochs, when channels list becomes available.
    tryScheduleAutoBottomTrack(indx);
}

void DataProcessor::tryScheduleAutoBottomTrack(uint64_t indx)
{
    Q_UNUSED(indx);
    if (resetInProgress_.load()) {
        return;
    }

    if (isOpeningFile_) {
        return;
    }

    if (!(updateMosaic_ || updateSurface_ || updateBottomTrack_)) {
        return;
    }

    auto btP = datasetPtr_->getBottomTrackParam();
    const int windowSize = btP.windowSize;
    if (windowSize <= 0) {
        return;
    }

    const int datasetSize = datasetPtr_ ? datasetPtr_->sizeThreadSafe() : 0;
    if (datasetSize <= 0) {
        return;
    }

    const int availableEpochs = datasetSize;
    const int availableCharts = static_cast<int>(chartsCounter_) + 1;
    const int currTo = std::min(availableEpochs, availableCharts);

    if (currTo < windowSize) {
        return;
    }

    if (currTo <= bottomTrackWindowCounter_) {
        return;
    }

    if (btBusy_) {
        return;
    }

    const int prevTo = std::clamp(bottomTrackWindowCounter_, 0, currTo);
    btP.indexFrom = std::max(0, prevTo - windowSize);
    btP.indexTo   = currTo;

    const auto channels = datasetPtr_->channelsList();
    if (channels.isEmpty()) {
        return;
    }

    const DatasetChannel& ch1 = channels[0];
    const DatasetChannel ch2 = (channels.size() >= 2) ? channels[1] : DatasetChannel();

    btBusy_ = true;
    btAutoFrom_ = btP.indexFrom;   // the precompute stays before it while data streams
    updateIdle();

    QMetaObject::invokeMethod(btWorker_, "bottomTrackProcessing", Qt::QueuedConnection,
                              Q_ARG(DatasetChannel, ch1),
                              Q_ARG(DatasetChannel, ch2),
                              Q_ARG(BottomTrackParam, btP),
                              Q_ARG(bool, false),/*manual*/
                              Q_ARG(bool, false)/*redrawAll*/);
    bottomTrackWindowCounter_ = currTo;
}

void DataProcessor::onBottomTrack3DAdded(const QVector<int>& epIndxs, const QVector<int>& vertIndxs, bool isManual)
{
    //return;

    if (resetInProgress_.load()) {
        return;
    }

    if (epIndxs.isEmpty() || vertIndxs.isEmpty()) {
        return;
    }

    latencyStamp(LatStage::Pending, epIndxs);

    if (bottomTrackFullRecalcPending_) {
        surfaceTaskEpochIndxsByZoom_.clear();
        surfaceManualEpochIndxsByZoom_.clear();
        pendingSurfaceIndxs_.clear();
        // Repainted from blank tiles: a tile kept in the cache, the worker's mesh or the DB holds its
        // head index and would refuse the new bottom track's pairs (2026-10-18; the mosaic kept the
        // old one before).
        resetMosaicProcessed();
        hotCache_.clear();
        QMetaObject::invokeMethod(worker_, "clearMosaic", Qt::QueuedConnection);
        if (jobRunning_.load()) {
            dropRunningMosaicJob_ = true;   // its reports and tiles would bring the old paint back
        qInfo().noquote() << QStringLiteral("mosaic: the running job's results dropped (%1)").arg(QLatin1String(Q_FUNC_INFO));
        }
        pendingMosaicIndxs_.clear();
        mosaicInFlightIndxs_.clear();
        vertIndxsFromBottomTrack_.clear();
        epIndxsFromBottomTrack_.clear();
        epochToBottomTrackVertIndx_.clear();
        bottomTrackFullRecalcPending_ = false;
        beamResetEstimate(true);   // the altitudes it was measured with are gone
        beamPush();
        clearOldTracking_ = false;   // "Clear old data" rebuilt every job before: a new bottom track showed at once
        cancelPrecompute();
    }

    QSet<int> changedEpochs;
    changedEpochs.reserve(epIndxs.size());
    int oldestChanged = std::numeric_limits<int>::max();
    for (int itm : epIndxs) {
        epIndxsFromBottomTrack_.insert(itm);
        changedEpochs.insert(itm);
        oldestChanged = std::min(oldestChanged, itm);
    }
    bottomTrackChanged_.start();
    // A new ping's track leaves alone what was painted ahead before it; a change reaching back to
    // painted epochs drops it all (a precomputed tile cannot be repainted under its head index).
    if (oldestChanged <= precomputeMaxEpoch_) {
        if (precomputeRunning_ || !precomputedZooms_.isEmpty()) {
            qInfo().noquote() << QStringLiteral("mosaic precompute: dropped, the bottom track reworked epoch %1 (painted ahead up to %2)")
                                 .arg(oldestChanged).arg(precomputeMaxEpoch_);
        }
        dropPrecomputed();
    }

    if (!changedEpochs.isEmpty()) {
        constexpr int kBulkMosaicInvalidateThreshold = 4096;
        if (changedEpochs.size() >= kBulkMosaicInvalidateThreshold) {
            // As the branch below, wholesale: the tiles keep their paint in the cache, the mesh and the
            // DB alike. Hiding only the DB rows started those tiles blank under a running job.
            mosaicTaskEpochIndxsByZoom_.clear();
            forgetPrecomputeSent(-1);
            pendingMosaicIndxs_.clear();
            mosaicInFlightIndxs_.clear();
        }
        else {
            for (auto it = mosaicTaskEpochIndxsByZoom_.begin(); it != mosaicTaskEpochIndxsByZoom_.end(); ++it) {
                auto& processed = it.value();
                for (int ep : std::as_const(changedEpochs)) {
                    processed.remove(ep);
                }
            }
            // A queued epoch is not painted yet and will be painted with the new depth, so it stays
            // queued. Taken out, it came back only if already indexed -- and the newest epoch is
            // indexed with the next ping, so its 3D bottom track, which arrives first, sent it back
            // a ping (onSendTilesByZoom's look-ahead undone; 2026-10-17, ~33 ms at 30 pings/s).
            for (int ep : std::as_const(changedEpochs)) {
                mosaicInFlightIndxs_.remove(ep);
            }
        }
    }

    for (int itm : vertIndxs) {
        vertIndxsFromBottomTrack_.insert(itm);
    }

    beamMaybeSchedule();

    const int pairCount = qMin(epIndxs.size(), vertIndxs.size());
    for (int i = 0; i < pairCount; ++i) {
        epochToBottomTrackVertIndx_[epIndxs[i]] = vertIndxs[i];
    }

    for (int itm : vertIndxs) {
        // '2' = realtime update: raster only the triangles triangulation actually changed.
        pendingSurfaceIndxs_.insert(qMakePair(isManual ? '1' : '2', itm));
    }

    if (updateMosaic_ && !changedEpochs.isEmpty() && !lastVisTileKeys_.isEmpty()) {
        int zoom = lastVisTilesZoom_;
        if (!isValidZoomIndx(zoom)) {
            zoom = lastDataZoomIndx_;
        }
        if (zoom <= 0) {
            zoom = zoomFromMpp(tileResolution_);
        }

        if (zoom > 0) {
            const QSet<int> visibleEpochs = collectEpochsForTilesSet(zoom, lastVisTileKeys_);
            auto& processedMosaic = mosaicTaskEpochIndxsByZoom_[zoom];
            for (int ep : std::as_const(visibleEpochs)) {
                if (!changedEpochs.contains(ep) ||
                    processedMosaic.contains(ep) ||
                    pendingMosaicIndxs_.contains(ep) ||
                    mosaicInFlightIndxs_.contains(ep)) {
                    continue;
                }
                pendingMosaicIndxs_.insert(ep);
            }
        }
    }

    if (mosaicBootstrapPending_ && updateMosaic_) {
        onCameraMoved();
    }

    scheduleLatest(WorkSet(WF_All));
}

void DataProcessor::onEpochAdded(uint64_t indx)
{
    epochCounter_    = indx;
    latencyStamp(LatStage::Created, latLastEpoch_ + 1, int(indx));
    latLastEpoch_ = std::max(latLastEpoch_, int(indx));
    tryScheduleAutoBottomTrack(indx);
}

void DataProcessor::onPositionAdded(uint64_t indx)
{
    positionCounter_ = indx;
    // Pos zeroing opens its epochs in Dataset::addAtt, which emits no epochAdded but a position:
    // that is when the heading pointer moves.
    latencyStamp(LatStage::Created, latLastPosition_ + 1, int(indx));
    latLastPosition_ = std::max(latLastPosition_, int(indx));
}

void DataProcessor::onAttitudeAdded(uint64_t indx)
{
    attitudeCounter_ = indx;
}

void DataProcessor::onMosaicCanCalc(uint64_t indx)
{
    mosaicCounter_   = indx;
    latencyStamp(LatStage::Released, latLastReleased_ + 1, int(indx));
    latLastReleased_ = std::max(latLastReleased_, int(indx));

    // Pending mosaic epochs may be blocked only by horizon counter.
    // When counter advances, immediately retry scheduling.
    if (updateMosaic_ && !pendingMosaicIndxs_.isEmpty()) {
        scheduleLatest(WorkSet(WF_Mosaic));
    }
}

void DataProcessor::bottomTrackProcessing(const DatasetChannel &shown1, const DatasetChannel &shown2, const BottomTrackParam &p, bool manual, bool redrawAll)
{
    if (resetInProgress_.load()) {
        return;
    }

    // A down view (DownView) shown is made of its channel's sides 0 and 1, and its bottom is theirs: they are tracked.
    auto side = [](DatasetChannel ch, uint8_t sub) { ch.subChannelId_ = sub; return ch; };
    DatasetChannel ch1 = shown1;
    DatasetChannel ch2 = shown2;
    if (DownView::isSub(ch1.subChannelId_)) {
        const bool pairOfItsOwn = !ch2.channelId_.isValid() || ch2.channelId_ == ch1.channelId_;
        ch1 = side(ch1, 0);
        if (pairOfItsOwn) {
            ch2 = side(ch1, 1);
        }
    }
    if (DownView::isSub(ch2.subChannelId_)) {
        ch2 = side(ch2, ch2.channelId_ == ch1.channelId_ && ch1.subChannelId_ == 0 ? 1 : 0);
    }

    PendingBtRun run;
    run.ch1 = ch1;
    run.ch2 = ch2;
    run.param = p;
    run.manual = manual;
    run.redrawAll = redrawAll;

    if (btBusy_) {
        auto sameTarget = [&](const PendingBtRun& w) {
            const BottomTrackParam& a = w.param;
            return w.manual == manual
                && w.ch1.channelId_ == ch1.channelId_ && w.ch1.subChannelId_ == ch1.subChannelId_
                && w.ch2.channelId_ == ch2.channelId_ && w.ch2.subChannelId_ == ch2.subChannelId_
                && a.preset == p.preset && a.windowSize == p.windowSize && a.gainSlope == p.gainSlope
                && a.threshold == p.threshold && a.verticalGap == p.verticalGap
                && a.minDistance == p.minDistance && a.maxDistance == p.maxDistance;
        };
        for (PendingBtRun& w : pendingManualBt_) {
            if (sameTarget(w)) {
                w.param.indexFrom = std::min(w.param.indexFrom, p.indexFrom);
                w.param.indexTo = std::max(w.param.indexTo, p.indexTo);
                w.redrawAll = w.redrawAll || redrawAll;
                return;
            }
        }
        pendingManualBt_.append(run);
        return;
    }

    dispatchBottomTrack(run);
}

void DataProcessor::dispatchBottomTrack(const PendingBtRun& run)
{
    if (run.redrawAll) {
        bottomTrackFullRecalcPending_ = true;
    }
    if (run.manual || run.redrawAll) {
        forceVisibleRefreshAfterBottomTrack_ = true;
    }
    btBusy_ = true;   // as an automatic run sets it: what comes meanwhile waits in pendingManualBt_

    QMetaObject::invokeMethod(btWorker_, "bottomTrackProcessing", Qt::QueuedConnection,
                              Q_ARG(DatasetChannel, run.ch1),
                              Q_ARG(DatasetChannel, run.ch2),
                              Q_ARG(BottomTrackParam, run.param),
                              Q_ARG(bool, run.manual),
                              Q_ARG(bool, run.redrawAll));
}

void DataProcessor::rebuildAfterEdit(bool withSurface)
{
    editRebuildPending_ = true;
    editRebuildSurface_ = editRebuildSurface_ || withSurface;
    editRebuildTimer_.start();   // restarted: strokes one after another make one rebuild
}

void DataProcessor::runEditRebuild()
{
    if (!editRebuildPending_ || shuttingDown_.load()) {
        return;
    }
    // After the bottom track and its echo through the 3D view: a "raise" stroke's run, an undo's.
    // The 3D bottom track reports a run's depths after the run has finished, so the quiet is counted
    // from either (onBottomTrackFinished, onBottomTrack3DAdded).
    constexpr qint64 kQuietMs = 500;
    if (resetInProgress_.load() || btBusy_ || !pendingManualBt_.isEmpty()
        || (bottomTrackChanged_.isValid() && bottomTrackChanged_.elapsed() < kQuietMs)) {
        editRebuildTimer_.start();
        return;
    }
    const bool withSurface = editRebuildSurface_;
    editRebuildPending_ = false;
    editRebuildSurface_ = false;

    beamRemeasureAfterEdit();
    if (!withSurface) {
        qInfo().noquote() << QStringLiteral("echogram edit: the mosaic painted again");
        restartMosaic();
        return;
    }

    qInfo().noquote() << QStringLiteral("echogram edit: the bottom surface and the mosaic built again (%1 points, %2 epochs)")
                         .arg(vertIndxsFromBottomTrack_.size()).arg(epIndxsFromBottomTrack_.size());
    cancelPrecompute();
    hotCache_.clear();
    clearOldTracking_ = false;
    QMetaObject::invokeMethod(worker_, "clearForEditRebuild", Qt::QueuedConnection);
    if (jobRunning_.load()) {
        dropRunningMosaicJob_ = true;   // its reports and tiles would bring the old paint back
        qInfo().noquote() << QStringLiteral("mosaic: the running job's results dropped (%1)").arg(QLatin1String(Q_FUNC_INFO));
    }

    resetMosaicProcessed();
    pendingMosaicIndxs_.clear();
    mosaicInFlightIndxs_.clear();
    pendingSurfaceIndxs_.clear();
    surfaceTaskEpochIndxsByZoom_.clear();
    surfaceManualEpochIndxsByZoom_.clear();
    surfaceEdgeLimitUpdatedZooms_.clear();
    renderedKeys_.clear();

    emit isobathsProcessingCleared();
    emit surfaceProcessingCleared();
    emit mosaicProcessingCleared();

    const int mosaicLowAllowed = (activeZeroing_ && mosaicFakeCoordsLastN_ > 0)
        ? std::max(0, mosaicCounter_ - mosaicFakeCoordsLastN_ + 1)
        : 0;
    if (updateMosaic_) {
        for (auto it = epIndxsFromBottomTrack_.cbegin(); it != epIndxsFromBottomTrack_.cend(); ++it) {
            if (*it >= mosaicLowAllowed) {
                pendingMosaicIndxs_.insert(*it);
            }
        }
    }
    for (auto it = vertIndxsFromBottomTrack_.cbegin(); it != vertIndxsFromBottomTrack_.cend(); ++it) {
        pendingSurfaceIndxs_.insert(qMakePair('0', *it));
    }
    pendingIsobathsWork_ = true;
    surfaceCameraPassPending_ = updateSurface_;

    scheduleLatest(WorkSet(WF_All), /*replace*/true);
}

void DataProcessor::setBottomTrackZeroDepth(bool state)
{
    QMetaObject::invokeMethod(btWorker_, "setBottomTrackZeroDepth", Qt::QueuedConnection, Q_ARG(bool, state));
}

void DataProcessor::setSurfaceColorTableThemeById(int id)
{
    exportConfig_.surfaceTheme = id;
    QMetaObject::invokeMethod(worker_, "setSurfaceThemeId", Qt::QueuedConnection, Q_ARG(int, id));
}

void DataProcessor::setSurfaceEdgeLimit(int val)
{
    exportConfig_.surfaceEdgeLimit = float(val);
    QMetaObject::invokeMethod(worker_, "setSurfaceEdgeLimit", Qt::QueuedConnection, Q_ARG(float, float(val)));

    surfaceTaskEpochIndxsByZoom_.clear();
    surfaceManualEpochIndxsByZoom_.clear();
    surfaceEdgeLimitDirty_ = true;
    surfaceEdgeLimitUpdatedZooms_.clear();
    pendingSurfaceIndxs_.clear();
    pendingIsobathsWork_ = true;

    for (auto it = vertIndxsFromBottomTrack_.cbegin(); it != vertIndxsFromBottomTrack_.cend(); ++it) {
        pendingSurfaceIndxs_.insert(qMakePair('0', *it));
    }

    scheduleLatest(WorkSet(WF_All), /*replace*/true);
}

void DataProcessor::setExtraWidth(int val)
{
    exportConfig_.surfaceExtraWidth = val;
    QMetaObject::invokeMethod(worker_, "setSurfaceExtraWidth", Qt::QueuedConnection, Q_ARG(int, val));

    // REBUILD?
    //surfaceTaskEpochIndxsByZoom_.clear();
    //surfaceManualEpochIndxsByZoom_.clear();
    //pendingSurfaceIndxs_.clear();
    //pendingIsobathsWork_ = true;

    //for (auto it = vertIndxsFromBottomTrack_.cbegin(); it != vertIndxsFromBottomTrack_.cend(); ++it) {
    //    pendingSurfaceIndxs_.insert(qMakePair('0', *it));
    //}

    //scheduleLatest(WorkSet(WF_All), /*replace*/true);
}

void DataProcessor::setIsobathsLabelStepSize(float val)
{
    QMetaObject::invokeMethod(worker_, "setIsobathsLabelStepSize", Qt::QueuedConnection, Q_ARG(float, val));
}

void DataProcessor::setSurfaceIsobathsStepSize(float val)
{
    exportConfig_.surfaceStep = val;
    QMetaObject::invokeMethod(worker_, "setSurfaceIsobathsStepSize", Qt::QueuedConnection, Q_ARG(float, val));

    surfaceTaskEpochIndxsByZoom_.clear();
    surfaceManualEpochIndxsByZoom_.clear();
    pendingSurfaceIndxs_.clear();
    pendingIsobathsWork_ = true;

    for (auto it = vertIndxsFromBottomTrack_.cbegin(); it != vertIndxsFromBottomTrack_.cend(); ++it) {
        pendingSurfaceIndxs_.insert(qMakePair('0', *it));
    }

    scheduleLatest(WorkSet(WF_All), /*replace*/true);
}

void DataProcessor::setMosaicChannels(const ChannelId &ch1, uint8_t sub1, const ChannelId &ch2, uint8_t sub2)
{
    exportConfig_.ch1 = ch1;
    exportConfig_.sub1 = sub1;
    exportConfig_.ch2 = ch2;
    exportConfig_.sub2 = sub2;
    const bool channelsChanged = !(mosaicCh1_ == ch1) || mosaicSub1_ != sub1 || !(mosaicCh2_ == ch2) || mosaicSub2_ != sub2;
    mosaicCh1_ = ch1;
    mosaicSub1_ = sub1;
    mosaicCh2_ = ch2;
    mosaicSub2_ = sub2;
    if (channelsChanged) {
        beamResetEstimate(true);   // measured on the other channels
        beamPush();
    }

    hotCache_.clear();

    QMetaObject::invokeMethod(worker_, "clearAll", Qt::QueuedConnection);
    if (jobRunning_.load()) {
        dropRunningMosaicJob_ = true;   // its reports and tiles would bring the old channels back
        qInfo().noquote() << QStringLiteral("mosaic: the running job's results dropped (%1)").arg(QLatin1String(Q_FUNC_INFO));
    }

    resetMosaicProcessed();
    mosaicInFlightIndxs_.clear();
    surfaceTaskEpochIndxsByZoom_.clear();
    surfaceManualEpochIndxsByZoom_.clear();

    emit isobathsProcessingCleared();
    emit surfaceProcessingCleared();
    emit mosaicProcessingCleared();

    QMetaObject::invokeMethod(worker_, "setMosaicChannels", Qt::QueuedConnection,
                              Q_ARG(ChannelId, ch1), Q_ARG(uint8_t, sub1),
                              Q_ARG(ChannelId, ch2), Q_ARG(uint8_t, sub2));
    cancelPrecompute();
    QMetaObject::invokeMethod(precomputer_, "setChannels", Qt::QueuedConnection,
                              Q_ARG(ChannelId, ch1), Q_ARG(uint8_t, sub1),
                              Q_ARG(ChannelId, ch2), Q_ARG(uint8_t, sub2));

    //return;

    for (auto it = epIndxsFromBottomTrack_.cbegin(); it != epIndxsFromBottomTrack_.cend(); ++it) {
        pendingMosaicIndxs_.insert(*it);
    }
    for (auto it = vertIndxsFromBottomTrack_.cbegin(); it != vertIndxsFromBottomTrack_.cend(); ++it) {
        pendingSurfaceIndxs_.insert(qMakePair('0', *it));
    }

    pendingIsobathsWork_ = true;
    beamMaybeSchedule();

    scheduleLatest(WorkSet(WF_All), /*replace*/true);
}

void DataProcessor::setMosaicTheme(int indx)
{
    if (mosaicColorTable_.getTheme() == indx) {
        return;
    }

    mosaicColorTable_.setTheme(indx);
    emitMosaicColorTable();
}

void DataProcessor::setMosaicLAngleOffset(float val)
{
    if (val >= -90.0f && val <= 90.0f) {   // what MosaicProcessor::setLAngleOffset takes
        exportConfig_.lAngleOffset = val;
    }
    QMetaObject::invokeMethod(worker_, "setMosaicLAngleOffset", Qt::QueuedConnection, Q_ARG(float, val));
    cancelPrecompute();
    QMetaObject::invokeMethod(precomputer_, "setLAngleOffset", Qt::QueuedConnection, Q_ARG(float, val));
}

void DataProcessor::setMosaicRAngleOffset(float val)
{
    if (val >= -90.0f && val <= 90.0f) {
        exportConfig_.rAngleOffset = val;
    }
    QMetaObject::invokeMethod(worker_, "setMosaicRAngleOffset", Qt::QueuedConnection, Q_ARG(float, val));
    cancelPrecompute();
    QMetaObject::invokeMethod(precomputer_, "setRAngleOffset", Qt::QueuedConnection, Q_ARG(float, val));
}

void DataProcessor::setMosaicTileResolution(float val)
{
    return;

    //qDebug() << "DataProcessor::setMosaicTileResolution" << val;
    if (qFuzzyIsNull(val)) {
        return;
    }

    const float convertedResolution = 1.0f / val;
    if (qFuzzyCompare(1.0f + tileResolution_, 1.0f + convertedResolution)) {
        return;
    }

    hotCache_.clear(); // db?

    tileResolution_ = convertedResolution;

    emit isobathsProcessingCleared();
    emit surfaceProcessingCleared();
    emit mosaicProcessingCleared();

    for (auto it = epIndxsFromBottomTrack_.cbegin(); it != epIndxsFromBottomTrack_.cend(); ++it) {
        pendingMosaicIndxs_.insert(*it);
    }
    for (auto it = vertIndxsFromBottomTrack_.cbegin(); it != vertIndxsFromBottomTrack_.cend(); ++it) {
        pendingSurfaceIndxs_.insert(qMakePair('0', *it));
    }

    pendingIsobathsWork_ = true;

    QMetaObject::invokeMethod(worker_, "setMosaicTileResolution", Qt::QueuedConnection, Q_ARG(float, tileResolution_));
    scheduleLatest(WorkSet(WF_All), /*replace*/true);
}

void DataProcessor::setMosaicLevels(float lowLevel, float highLevel)
{
    const auto levels = mosaicColorTable_.getLevels();
    if (qFuzzyCompare(1.0f + levels.first, 1.0f + lowLevel) &&
        qFuzzyCompare(1.0f + levels.second, 1.0f + highLevel)) {
        return;
    }

    mosaicColorTable_.setLevels(lowLevel, highLevel);
    emitMosaicColorTable();
}

void DataProcessor::setMosaicLowLevel(float val)
{
    if (qFuzzyCompare(1.0f + mosaicColorTable_.getLowLevel(), 1.0f + val)) {
        return;
    }

    mosaicColorTable_.setLowLevel(val);
    emitMosaicColorTable();
}

void DataProcessor::setMosaicHighLevel(float val)
{
    if (qFuzzyCompare(1.0f + mosaicColorTable_.getHighLevel(), 1.0f + val)) {
        return;
    }

    mosaicColorTable_.setHighLevel(val);
    emitMosaicColorTable();
}

void DataProcessor::askColorTableForMosaic()
{
    emitMosaicColorTable();
}

void DataProcessor::setMosaicSource(int source)
{
    exportConfig_.mosaicSource = source;
    QMetaObject::invokeMethod(worker_, "setMosaicSource", Qt::QueuedConnection, Q_ARG(int, source));
    QMetaObject::invokeMethod(precomputer_, "setSource", Qt::QueuedConnection, Q_ARG(int, source));
    if (mosaicSource_ != source) {
        mosaicSource_ = source;
        // Measured on the other source's levels: measured again, the mosaic of a file waiting for it.
        beamResetEstimate(true);
        beamPush();
        beamMaybeSchedule();
    }
    restartMosaic();
}

void DataProcessor::setMosaicQuality(bool correction, float nadirCropDeg, float farCropDeg, int overlap)
{
    mosaic_beam::PaintRules rules;
    rules.nadirCropDeg = std::clamp(nadirCropDeg, 0.0f, 45.0f);
    rules.farCropDeg = std::clamp(farCropDeg, 45.0f, 90.0f);
    rules.overlap = mosaic_beam::Overlap(std::clamp(overlap, 0, 2));
    if (correction == beamCorrection_ && rules == paintRules_) {
        return;
    }
    const bool correctionChanged = correction != beamCorrection_;
    beamCorrection_ = correction;
    paintRules_ = rules;
    static const char* const kOverlap[] = { "newest", "newest, edges under", "nearest the track" };
    qInfo().noquote() << QStringLiteral("mosaic quality: correction %1, crop %2..%3 deg, overlaps %4")
                         .arg(correction ? "on" : "off").arg(rules.nadirCropDeg).arg(rules.farCropDeg)
                         .arg(kOverlap[int(rules.overlap)]);
    if (correctionChanged && correction && !beamModel_) {
        beamResetEstimate(true);
        beamMaybeSchedule();
    }
    beamPush();
    beamEmitState();
    restartMosaic();
}

void DataProcessor::beamResetEstimate(bool hold)
{
    beamModel_.reset();
    beamMeasuredAt_ = 0;
    beamFirstAsk_ = -1;
    beamTimer_.stop();
    // A file's mosaic waits for its model; live data does not wait (kBeamFirst epochs would be some
    // seconds of nothing): it is repainted once the model comes.
    beamAwaitFirst_ = hold;
}

bool DataProcessor::beamHolding() const
{
    return beamAwaitFirst_ && beamCorrection_ && datasetState_ == int(Dataset::DatasetState::kFile);
}

void DataProcessor::beamMaybeSchedule()
{
    if (!beamCorrection_) {
        return;
    }
    const int n = epIndxsFromBottomTrack_.size();
    // With no model, or a young one -- the pings so far too shallow or too few, a survey starting at
    // the shore -- again every kBeamFirst epochs for as long as it takes (2026-10-18: a first model
    // from 149 shallow pings stayed for the rest of a live run); then as the epochs double, up to
    // kBeamLast.
    uint32_t pings = 0;
    if (beamModel_) {
        pings = std::numeric_limits<uint32_t>::max();
        for (const auto& s : beamModel_->side) {
            if (s.valid) {
                pings = std::min(pings, s.pings);
            }
        }
    }
    bool due = false;
    if (beamMeasuredAt_ == 0) {
        due = n >= kBeamFirst;
    }
    else if (pings < kBeamMaturePings) {
        due = n >= beamMeasuredAt_ + kBeamFirst;
    }
    else {
        due = beamMeasuredAt_ < kBeamLast && n >= 2 * beamMeasuredAt_;
    }
    if (!due) {
        if (beamHolding() && n > 0 && n < kBeamFirst && !btBusy_ && !beamTimer_.isActive()) {
            // A file shorter than kBeamFirst epochs: no model, and the mosaic must not wait for one.
            beamTimer_.start(kBeamQuietMs);
        }
        return;
    }
    const qint64 now = latClock_.elapsed();
    if (beamFirstAsk_ < 0) {
        beamFirstAsk_ = now;
    }
    // A file's bottom track for (nearly) every epoch is all there will be: no batch left to wait for.
    const int epochs = datasetPtr_ ? datasetPtr_->sizeThreadSafe() : 0;
    if (beamHolding() && epochs > 0 && n >= epochs - std::max(16, epochs / 100)) {
        beamTimer_.start(0);
        return;
    }
    const qint64 left = std::max<qint64>(0, kBeamMaxWaitMs - (now - beamFirstAsk_));
    beamTimer_.start(int(std::min<qint64>(kBeamQuietMs, left)));
}

void DataProcessor::beamEstimate()
{
    beamFirstAsk_ = -1;
    if (!beamCorrection_ || !datasetPtr_) {
        return;
    }
    QVector<int> eps(epIndxsFromBottomTrack_.cbegin(), epIndxsFromBottomTrack_.cend());
    if (eps.size() < kBeamFirst) {
        if (beamAwaitFirst_) {   // too little for a model (a short file): the mosaic goes on without one
            beamAwaitFirst_ = false;
            qInfo().noquote() << QStringLiteral("mosaic beam: %1 epochs with a bottom track, too few to measure; painted as they are")
                                 .arg(eps.size());
            beamEmitState();
            scheduleLatest(WorkSet(WF_Mosaic));
        }
        return;
    }
    std::sort(eps.begin(), eps.end());

    // KOGGER_MOSAIC_PING_DUMP: up to 16 000 of the epochs, evenly, in a pass of its own (denser than the
    // estimate's, for straight runs of a line). "KPD2", then per record, little-endian -- epoch (int32),
    // side (uint8), altitude, resolution, offset, north, east, yaw, pitch, roll (float32 each; NaN when
    // there is no attitude), count (uint32) and the raw levels.
    QFile dump(qEnvironmentVariable("KOGGER_MOSAIC_PING_DUMP"));
    if (!dump.fileName().isEmpty() && dump.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QDataStream out(&dump);
        out.setByteOrder(QDataStream::LittleEndian);
        out.setFloatingPointPrecision(QDataStream::SinglePrecision);
        out.writeRawData("KPD2", 4);
        const int dumpStride = std::max(1, int((eps.size() + 15999) / 16000));
        for (int k = 0; k < eps.size(); k += dumpStride) {
            Epoch ep = datasetPtr_->fromIndexMosaicCopy(eps[k]);
            if (!ep.isValid()) {
                continue;
            }
            const auto ned = ep.getSonarPosition().ned;
            const bool att = ep.isAttAvail();
            for (int side = 0; side < 2; ++side) {
                const ChannelId& dch = side ? mosaicCh2_ : mosaicCh1_;
                Epoch::Echogram* deg = dch.isValid() ? ep.chart(dch, side ? mosaicSub2_ : mosaicSub1_) : nullptr;
                if (!deg || deg->amplitude.isEmpty()) {
                    continue;
                }
                out << qint32(eps[k]) << quint8(side) << float(deg->bottomProcessing.getDistance())
                    << deg->resolution << deg->offset << float(ned.n) << float(ned.e) << ep.tryRetValidYaw()
                    << (att ? ep.pitch() : NAN) << (att ? ep.roll() : NAN) << quint32(deg->amplitude.size());
                out.writeRawData(reinterpret_cast<const char*>(deg->amplitude.constData()), int(deg->amplitude.size()));
            }
        }
        qInfo().noquote() << QStringLiteral("mosaic beam: %1 epochs (every %2th) dumped to %3")
                             .arg((eps.size() + dumpStride - 1) / dumpStride).arg(dumpStride).arg(dump.fileName());
    }

    std::shared_ptr<mosaic_beam::Model> model = beamMeasure(eps, QString());
    const int measuredAt = eps.size();

    beamMeasuredAt_ = measuredAt;
    const bool wasHeld = beamHolding();
    beamAwaitFirst_ = false;
    if (!model->valid()) {
        if (wasHeld) {
            scheduleLatest(WorkSet(WF_Mosaic));
        }
        beamEmitState();
        return;
    }
    if (beamModel_) {
        const float diff = std::max(mosaic_beam::maxDifferenceDb(beamModel_->side[0], model->side[0]),
                                    mosaic_beam::maxDifferenceDb(beamModel_->side[1], model->side[1]));
        if (diff <= kBeamRepaintDb) {
            qInfo().noquote() << QStringLiteral("mosaic beam: within %1 dB of the model in use, kept").arg(double(diff), 0, 'f', 2);
            beamEmitState();
            return;
        }
        qInfo().noquote() << QStringLiteral("mosaic beam: %1 dB from the model in use, repainted").arg(double(diff), 0, 'f', 2);
    }
    beamModel_ = std::move(model);
    beamPush();
    beamEmitState();
    if (wasHeld) {
        scheduleLatest(WorkSet(WF_Mosaic));   // nothing painted yet: no restart
    }
    else {
        restartMosaic();
    }
}

std::shared_ptr<mosaic_beam::Model> DataProcessor::beamMeasure(const QVector<int>& eps, const QString& why)
{
    QElapsedTimer clock;
    clock.start();
    const int stride = std::max(1, int((eps.size() + kBeamMaxEpochs - 1) / kBeamMaxEpochs));
    auto tables = std::make_unique<std::array<mosaic_beam::Table, 2>>();
    int read = 0;
    for (int k = 0; k < eps.size(); k += stride) {
        Epoch ep = datasetPtr_->fromIndexMosaicCopy(eps[k]);
        if (!ep.isValid()) {
            continue;
        }
        ++read;
        for (int side = 0; side < 2; ++side) {
            const ChannelId& ch = side ? mosaicCh2_ : mosaicCh1_;
            if (!ch.isValid()) {
                continue;
            }
            Epoch::Echogram* eg = ep.chart(ch, side ? mosaicSub2_ : mosaicSub1_);
            if (!eg || eg->amplitude.isEmpty()) {
                continue;
            }
            // The levels the mosaic paints (MosaicProcessor::getColorIndx), not always the raw ones.
            const QVector<uint8_t>* levels = &eg->amplitude;
            if (mosaicSource_ == int(MosaicProcessor::Source::SideScan)) {
                eg->updateCompesated();
                levels = &eg->compensated;
            }
            else if (mosaicSource_ == int(MosaicProcessor::Source::Tgc)) {
                eg->updateTgc();
                levels = &eg->tgc;
            }
            (*tables)[size_t(side)].add(levels->constData(), int(levels->size()), eg->resolution,
                                        float(eg->bottomProcessing.getDistance()));
        }
    }
    auto model = std::make_shared<mosaic_beam::Model>();
    model->epochs = uint32_t(eps.size());
    for (int side = 0; side < 2; ++side) {
        model->side[size_t(side)] = mosaic_beam::fit((*tables)[size_t(side)]);
    }
    const qint64 ms = clock.elapsed();
    const int measuredAt = eps.size();

    auto sideLine = [](const mosaic_beam::SideModel& m) {
        if (!m.valid) {
            return m.angleTo >= 0 ? QStringLiteral("none (%1 pings, data to %2 deg)").arg(m.pings).arg(m.angleTo + 1)
                                  : QStringLiteral("none (%1 pings)").arg(m.pings);
        }
        auto edge = [&m](float db) {
            const float a = m.edgeAngle(db);
            return a < 0.0f ? QStringLiteral("-") : QString::number(double(a), 'f', 1);
        };
        return QStringLiteral("%1 pings, data to %2 deg, peak %3 deg, -3 dB to %4, -6 dB to %5, -10 dB to %6; %7 dB of %8 dB left")
            .arg(m.pings).arg(m.angleTo + 1).arg(double(m.peakAngle()), 0, 'f', 1).arg(edge(-3.0f)).arg(edge(-6.0f))
            .arg(edge(-10.0f)).arg(double(m.residualDb), 0, 'f', 2).arg(double(m.spreadDb), 0, 'f', 2);
    };
    qInfo().noquote() << QStringLiteral("mosaic beam: measured%6 on %1 of %2 epochs in %3 ms; left: %4; right: %5")
                         .arg(read).arg(measuredAt).arg(ms).arg(sideLine(model->side[0])).arg(sideLine(model->side[1]))
                         .arg(why.isEmpty() ? QString() : QStringLiteral(" ") + why);
    return model;
}

// An edit changes what the model is measured on -- a ping left out has no bottom in fromIndexMosaicCopy --
// and the recording reopened with its edits is measured with them applied. Measured again here so that both
// paint alike; taken as it comes (no kBeamRepaintDb), as the rebuild after it repaints everything anyway.
// Before the first measurement nothing is done: that one will see the edits. With the correction off too, as
// turning it on uses the model kept (setMosaicQuality); beamPush sends none meanwhile.
void DataProcessor::beamRemeasureAfterEdit()
{
    if (!datasetPtr_ || beamMeasuredAt_ == 0 || beamHolding()) {
        return;
    }
    QVector<int> eps(epIndxsFromBottomTrack_.cbegin(), epIndxsFromBottomTrack_.cend());
    if (eps.size() < kBeamFirst) {
        return;
    }
    std::sort(eps.begin(), eps.end());
    std::shared_ptr<mosaic_beam::Model> model = beamMeasure(eps, QStringLiteral("after an echogram edit"));
    beamMeasuredAt_ = eps.size();
    if (model->valid()) {
        beamModel_ = std::move(model);
    }
    else {
        beamModel_.reset();   // as a reopen would have none
    }
    beamPush();   // queued before the rebuild's own work, so the worker and the precompute paint with it
    beamEmitState();
}

mosaic_beam::PaintRules DataProcessor::beamRulesNow() const
{
    mosaic_beam::PaintRules rules = paintRules_;
    // EdgesUnder: a side's far edge where its measured beam is 6 dB down, else the default.
    if (beamModel_) {
        for (int side = 0; side < 2; ++side) {
            const mosaic_beam::SideModel& m = beamModel_->side[size_t(side)];
            const float edge = m.valid ? m.edgeAngle(-6.0f) : -1.0f;
            if (edge > 0.0f) {   // else not fallen 6 dB within the data: the default stays
                rules.edgeFarDeg[size_t(side)] = std::max(rules.edgeNadirDeg + 10.0f, edge);
            }
        }
    }
    return rules;
}

void DataProcessor::beamPush()
{
    std::shared_ptr<const mosaic_beam::Model> model = beamCorrection_ ? beamModel_ : nullptr;
    const mosaic_beam::PaintRules rules = beamRulesNow();
    QMetaObject::invokeMethod(worker_, [w = worker_, model, rules]() { w->setMosaicBeam(model, rules); }, Qt::QueuedConnection);
    cancelPrecompute();
    QMetaObject::invokeMethod(precomputer_, [p = precomputer_, model, rules]() { p->setBeam(model, rules); }, Qt::QueuedConnection);
}

void DataProcessor::beamEmitState()
{
    QVariantMap st;
    st["correction"] = beamCorrection_;
    st["holding"] = beamHolding();
    st["valid"] = bool(beamModel_ && beamModel_->valid());
    st["epochs"] = beamModel_ ? int(beamModel_->epochs) : 0;
    st["nadirCrop"] = double(paintRules_.nadirCropDeg);
    st["farCrop"] = double(paintRules_.farCropDeg);
    st["minPings"] = int(mosaic_beam::kMinPings);          // what a model needs, for the "not yet" line
    st["minCover"] = int(mosaic_beam::kMinAngleCover);
    for (int side = 0; side < 2; ++side) {
        QVariantMap s;
        if (beamModel_ && beamModel_->side[size_t(side)].valid) {
            const mosaic_beam::SideModel& m = beamModel_->side[size_t(side)];
            QVariantList curve;
            for (int k = 0; k < mosaic_beam::kAngleBins; ++k) {
                curve.append(k >= m.angleFrom && k <= m.angleTo ? QVariant(double(m.angleDb[size_t(k)])) : QVariant());
            }
            s["curve"] = curve;
            s["pings"] = int(m.pings);
            s["peak"] = double(m.peakAngle());
            s["edge3"] = double(m.edgeAngle(-3.0f));
            s["edge6"] = double(m.edgeAngle(-6.0f));
            s["edge10"] = double(m.edgeAngle(-10.0f));   // -1: not fallen so far within the data
            s["dataTo"] = m.angleTo + 1;
            s["residual"] = double(m.residualDb);
            s["spread"] = double(m.spreadDb);
            // How much the range part changes over its data: the near-to-far fall a correction undoes.
            float lo = 1e9f, hi = -1e9f;
            for (float v : m.rangeDb) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            s["rangeSpan"] = m.rangeDb.empty() ? 0.0 : double(hi - lo);
        }
        st[side ? "right" : "left"] = s;
    }
    emit mosaicBeamState(st);
}

void DataProcessor::setActiveZeroing(bool state)
{
    if (activeZeroing_ == state) {
        return;
    }
    activeZeroing_ = state;

    if (!state) {
        return;
    }
    // Toggling FAKE_COORDS on with a bounded window: pre-toggle paint (possibly real-coords
    // history) would otherwise remain stuck visible on the same zoom. Restart so the next
    // radical recalc replays only the last N onto a clean slate.
    if (mosaicFakeCoordsLastN_ > 0) {
        restartMosaic();
    }
}

void DataProcessor::setMosaicFakeCoordsLastN(int n)
{
    mosaicFakeCoordsLastN_ = std::max(0, n);
}

void DataProcessor::setMosaicFakeCoordsClearOldData(bool state)
{
    mosaicFakeCoordsClearOldData_ = state;
}

void DataProcessor::restartMosaic()
{
    cancelPrecompute();
    hotCache_.clear();
    clearOldTracking_ = false;

    QMetaObject::invokeMethod(worker_, "clearMosaic", Qt::QueuedConnection);
    if (jobRunning_.load()) {
        dropRunningMosaicJob_ = true;   // its reports and tiles would bring the old paint back
        qInfo().noquote() << QStringLiteral("mosaic: the running job's results dropped (%1)").arg(QLatin1String(Q_FUNC_INFO));
    }

    pendingMosaicIndxs_.clear();
    resetMosaicProcessed();
    mosaicInFlightIndxs_.clear();

    emit mosaicProcessingCleared();

    const int mosaicLowAllowed = (activeZeroing_ && mosaicFakeCoordsLastN_ > 0)
        ? std::max(0, mosaicCounter_ - mosaicFakeCoordsLastN_ + 1)
        : 0;
    for (auto it = epIndxsFromBottomTrack_.cbegin(); it != epIndxsFromBottomTrack_.cend(); ++it) {
        if (*it < mosaicLowAllowed) {
            continue;
        }
        pendingMosaicIndxs_.insert(*it);
    }

    scheduleLatest(WorkSet(WF_Mosaic), /*replace*/true);
}

void DataProcessor::emitMosaicColorTable()
{
    const auto table = mosaicColorTable_.getRgbaColors();
    postMosaicColorTable(table);
}

void DataProcessor::onIsobathsUpdated()
{
    if (!updateIsobaths_) {
        return;
    }

    pendingIsobathsWork_ = true;

    scheduleLatest(WorkSet(WF_Isobaths));
}

void DataProcessor::onMosaicUpdated() {
    if (!updateMosaic_ || pendingMosaicIndxs_.isEmpty()) {
        return;
    }

    scheduleLatest(WorkSet(WF_Mosaic));
}

void DataProcessor::onMosaicEpochsProcessed(const QVector<int>& indxs, int zoom)
{
    if (indxs.isEmpty() || dropRunningMosaicJob_) {
        return;   // a job painting before a reset: its epochs stay queued for the next
    }
    if (zoom <= 0) {
        zoom = zoomFromMpp(tileResolution_);
    }

    latencyFirstPaint(indxs);

    auto& processed = mosaicTaskEpochIndxsByZoom_[zoom];
    for (int idx : indxs) {
        processed.insert(idx);
        pendingMosaicIndxs_.remove(idx);
        mosaicInFlightIndxs_.remove(idx);
    }
}

void DataProcessor::runCoalescedWork()
{
    auto idleGuard = qScopeGuard([this] { updateIdle(); });

    if (resetInProgress_.load()) {
        return;
    }

    if (shuttingDown_.load()) {
        return;
    }

    if (jobRunning_.load()) {
        return;
    }

    if (suppressResults_.load()) {
        return;
    }

    if (!isCanStartCalculations()) {
        return;
    }

    // A precompute stopping on the view's new level still sends what it painted (kept, see
    // cancelPrecompute): the view's job starts after it, on its tiles and its epochs, not beside it.
    if (precomputeRunning_ && precomputeRunTarget_ == lastDataZoomIndx_) {
        return;   // onPrecomputeFinished starts it
    }

    const uint32_t maskNow = requestedMask_.exchange(0);
    const bool wantSurface  = maskNow & WF_Surface;
    const bool wantMosaic   = maskNow & WF_Mosaic;
    const bool wantIsobaths = maskNow & WF_Isobaths;

    WorkBundle wb;

    if (wantSurface && updateSurface_) {
        int zoom = lastDataZoomIndx_;
        if (zoom <= 0) {
            zoom = zoomFromMpp(tileResolution_);
        }

        if (!pendingSurfaceIndxs_.isEmpty()) {
            for (auto it = pendingSurfaceIndxs_.cbegin(); it != pendingSurfaceIndxs_.cend(); ++it) {
                if (it->first != '1') {
                    continue;
                }
                for (const auto& z : ZL) {
                    surfaceManualEpochIndxsByZoom_[z.zoom].insert(it->second);
                }
            }
        }

        auto& processed = surfaceTaskEpochIndxsByZoom_[zoom];
        auto& manualPending = surfaceManualEpochIndxsByZoom_[zoom];
        bool cameraPassRequested = surfaceCameraPassPending_;

        if (!pendingSurfaceIndxs_.isEmpty() || !manualPending.isEmpty() || cameraPassRequested) {
            wb.surfaceVec.reserve(pendingSurfaceIndxs_.size() + manualPending.size() + (cameraPassRequested ? 1 : 0));

            if (!manualPending.isEmpty()) {
                for (int idx : std::as_const(manualPending)) {
                    processed.insert(idx);
                    wb.surfaceVec.append(qMakePair('1', idx));
                }
                manualPending.clear();
            }

            if (!pendingSurfaceIndxs_.isEmpty()) {
                for (auto it = pendingSurfaceIndxs_.cbegin(); it != pendingSurfaceIndxs_.cend(); ++it) {
                    if (it->first == '1') {
                        continue;
                    }

                    if (it->first == 'c') {
                        cameraPassRequested = true;
                        continue;
                    }

                    if (it->first == '2') {
                        if (it->second < 0) {
                            continue;
                        }
                        wb.surfaceVec.append(*it);
                        continue;
                    }

                    if (processed.contains(it->second)) {
                        continue;
                    }
                    processed.insert(it->second);
                    wb.surfaceVec.append(*it);
                }

                pendingSurfaceIndxs_.clear();
            }

            if (cameraPassRequested) {
                wb.surfaceVec.append(qMakePair('c', -1));
                surfaceCameraPassPending_ = false;
            }

            if (!wb.surfaceVec.isEmpty()) {
                std::sort(wb.surfaceVec.begin(), wb.surfaceVec.end(), [](const QPair<char,int>& a, const QPair<char,int>& b){ return a.second < b.second; });
            }
        }
    }

    // A file's mosaic waits for its brightness model (beamEstimate asks again when it is there).
    const bool clearOld = updateMosaic_ && activeZeroing_ && mosaicFakeCoordsLastN_ > 0 && mosaicFakeCoordsClearOldData_ && !beamHolding();
    if (!clearOld && workerTracksOwners_) {   // the mode is off: the worker lets the owners go
        QMetaObject::invokeMethod(worker_, "setMosaicTrackOwners", Qt::QueuedConnection, Q_ARG(bool, false));
        workerTracksOwners_ = false;
        clearOldTracking_ = false;
    }

    if (wantMosaic && clearOld && (!clearOldTracking_ || clearOldN_ != mosaicFakeCoordsLastN_)) {
        // FAKE_COORDS+N with "Clear old data", once: wipe caches, queue worker clearMosaic, paint
        // the last N epochs from epIndxsFromBottomTrack_ afresh, the worker noting each pixel's
        // pair from here on. The next jobs paint new epochs as usual and erase what left the
        // window (wb.mosaicEraseBelow below).
        hotCache_.clear();
        if (!workerTracksOwners_) {
            QMetaObject::invokeMethod(worker_, "setMosaicTrackOwners", Qt::QueuedConnection, Q_ARG(bool, true));
            workerTracksOwners_ = true;
        }
        QMetaObject::invokeMethod(worker_, "clearMosaic", Qt::QueuedConnection);
        resetMosaicProcessed();
        // An epoch queued ahead of the horizon (onSendTilesByZoom) stays queued: the rebuild
        // paints up to the horizon, and the jobs after it paint only what is queued.
        QSet<int> ahead;
        for (int idx : std::as_const(pendingMosaicIndxs_)) {
            if (idx > mosaicCounter_) {
                ahead.insert(idx);
            }
        }
        pendingMosaicIndxs_.swap(ahead);
        mosaicInFlightIndxs_.clear();

        const int mosaicLowAllowed = std::max(0, mosaicCounter_ - mosaicFakeCoordsLastN_ + 1);
        wb.mosaicVec.reserve(mosaicFakeCoordsLastN_);
        // The last N by index, in order: walking the whole set grew with the session, every ping.
        for (int idx = mosaicLowAllowed; idx <= mosaicCounter_; ++idx) {
            if (epIndxsFromBottomTrack_.contains(idx)) {
                wb.mosaicVec.append(idx);
            }
        }
        for (int idx : std::as_const(wb.mosaicVec)) {
            mosaicInFlightIndxs_.insert(idx);
        }
        wb.batchMosaicEmit = true;
        clearOldTracking_ = true;
        clearOldN_ = mosaicFakeCoordsLastN_;
    }
    else if (wantMosaic && !pendingMosaicIndxs_.isEmpty() && updateMosaic_ && !beamHolding()) {
        int mosaicZoom = lastDataZoomIndx_;
        if (mosaicZoom <= 0) {
            mosaicZoom = zoomFromMpp(tileResolution_);
        }
        auto& processedMosaic = mosaicTaskEpochIndxsByZoom_[mosaicZoom];

        // FAKE_COORDS+N without "Clear old data": still cap work to the last-N window so old
        // backlog doesn't drown the worker. Old paint already on tiles stays as-is.
        const int mosaicLowAllowed = (activeZeroing_ && mosaicFakeCoordsLastN_ > 0)
            ? std::max(0, mosaicCounter_ - mosaicFakeCoordsLastN_ + 1)
            : 0;

        auto it = pendingMosaicIndxs_.begin();
        while (it != pendingMosaicIndxs_.end()) {
            const int idx = *it;
            if (idx < mosaicLowAllowed) {
                it = pendingMosaicIndxs_.erase(it);
                continue;
            }
            if (processedMosaic.contains(idx)) {
                it = pendingMosaicIndxs_.erase(it);
                continue;
            }
            if (mosaicInFlightIndxs_.contains(idx)) {
                ++it;
                continue;
            }
            if (idx <= mosaicCounter_) {
                wb.mosaicVec.append(idx);
                mosaicInFlightIndxs_.insert(idx);
                it = pendingMosaicIndxs_.erase(it);
            }
            else {
                ++it;
            }
        }
        if (!wb.mosaicVec.isEmpty()) {
            std::sort(wb.mosaicVec.begin(), wb.mosaicVec.end());
        }
    }
    if (clearOld && clearOldTracking_) {
        // What the rebuild shows: the last N and the three pairs before them that
        // MosaicProcessor::updateDataWrapper walks for context.
        const int mosaicLowAllowed = std::max(0, mosaicCounter_ - mosaicFakeCoordsLastN_ + 1);
        wb.mosaicEraseBelow = std::max(0, mosaicLowAllowed - 3);
    }

    if (wantIsobaths && updateIsobaths_ && !updateMosaic_) {
        wb.doIsobaths = pendingIsobathsWork_ || !wb.surfaceVec.isEmpty();
        if (pendingIsobathsWork_) {
            pendingIsobathsWork_ = false;
        }
    }

    if (wb.surfaceVec.isEmpty() && wb.mosaicVec.isEmpty() && !wb.doIsobaths) {
        pendingIsobathsWork_ = false;
        return;
    }

    if (updateMosaic_) {
        wb.doIsobaths = false;
        pendingIsobathsWork_ = false;
    }

    //wb.doIsobaths = false;
    //pendingIsobathsWork_ = false;

    if (!wb.surfaceVec.isEmpty()) {
        // Keep worker-side visible tiles in sync before surface pass starts.
        // Without this ordering, surface job may run with stale/empty visibility
        // until the next camera movement event.
        QMetaObject::invokeMethod(worker_, "setVisibleTileKeys", Qt::BlockingQueuedConnection, Q_ARG(QSet<TileKey>, lastVisTileKeys_));
    }

    nextRunPending_.store(false);
    cancelRequested_.store(false);
    jobRunning_.store(true);

    latencyStamp(LatStage::Sent, wb.mosaicVec);

    // queue в воркер в его поток
    QMetaObject::invokeMethod(worker_, "processBundle", Qt::QueuedConnection, Q_ARG(WorkBundle, wb));
}

void DataProcessor::startTimerIfNeeded()
{
    if (defProcType_) {
        auto startOrRestart = [this]() {
            pendingWorkTimer_.stop();
            pendingWorkTimer_.start();
            updateIdle();
        };

        if (QThread::currentThread() == this->thread()) {
            startOrRestart();
        }
        else {
            QMetaObject::invokeMethod(this, startOrRestart, Qt::QueuedConnection);
        }
    }
    else {
        if (QThread::currentThread() == this->thread()) {
            if (!pendingWorkTimer_.isActive()) pendingWorkTimer_.start();
            updateIdle();
        }
        else {
            QMetaObject::invokeMethod(this, [this](){
                if (!pendingWorkTimer_.isActive()) pendingWorkTimer_.start();
                updateIdle();
            }, Qt::QueuedConnection);
        }
    }
}

void DataProcessor::onWorkerFinished()
{
    auto idleGuard = qScopeGuard([this] { updateIdle(); });
    dropRunningMosaicJob_ = false;   // all it sent came before this (one thread, in order)
    const bool wasCanceled = cancelRequested_.load();

    jobRunning_.store(false);

    if (!mosaicInFlightIndxs_.isEmpty()) {
        if (wasCanceled) {
            for (int idx : std::as_const(mosaicInFlightIndxs_)) {
                pendingMosaicIndxs_.insert(idx);
            }
        }
        mosaicInFlightIndxs_.clear();
    }
    if (!wasCanceled) {
        latencyReport();
    }

    cancelRequested_.store(false);

    if (resetInProgress_.load()) {
        tryFinalizeResetProcessing();
        return;
    }

    if (wasCanceled && updateMosaic_ && !pendingMosaicIndxs_.isEmpty()
            && !suppressResults_.load()) {
        requestedMask_.fetch_or(WF_Mosaic);
        nextRunPending_.store(true);
    }

    if (nextRunPending_.load() && !shuttingDown_.load()) {
        startTimerIfNeeded();
    }
    // While data streams a next run is always pending: armed only in the else branch, the precompute
    // was looked at again only on a change of view (2026-10-17: a 60 s stream, no run in it). Not
    // restarted when armed: jobs end every few hundred ms and would put it off for good.
    if (!precomputeTimer_.isActive()) {
        precomputeTimer_.start();
    }
}

namespace {
int latMedian(QVector<int> v)
{
    if (v.isEmpty()) {
        return -1;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
int latPercentile(QVector<int> v, int pct)
{
    if (v.isEmpty()) {
        return -1;
    }
    std::sort(v.begin(), v.end());
    return v[std::min(int(v.size()) - 1, int(v.size()) * pct / 100)];
}
} // namespace

void DataProcessor::latencyStamp(LatStage stage, int fromIndx, int toIndx)
{
    // Only a live connection with the mosaic on: a file is read far faster than it was recorded,
    // and with the mosaic off nothing would ever take an epoch out of the table.
    if (datasetState_ != 2 || isOpeningFile_ || !updateMosaic_ || toIndx < fromIndx) {
        return;
    }
    fromIndx = std::max(fromIndx, toIndx - 4096);   // a jump (new session, catching up): keep it bounded
    const qint64 now = latClock_.elapsed();
    for (int i = fromIndx; i <= toIndx; ++i) {
        // An entry starts when its epoch appears -- with Pos zeroing, the attitude frame that also
        // moves the heading pointer -- or else when its chart is released.
        if (stage == LatStage::Created || stage == LatStage::Chart) {
            qint64& t = (stage == LatStage::Created) ? latStamps_[i].created : latStamps_[i].chart;
            if (t == 0) {
                t = now;
            }
            continue;
        }
        auto it = latStamps_.find(i);
        if (it == latStamps_.end()) {
            continue;
        }
        qint64& t = (stage == LatStage::Pending) ? it->pending : (stage == LatStage::Released) ? it->released : it->sent;
        if (t == 0) {
            t = now;
        }
    }
    if (latStamps_.size() > 8192) {                 // epochs that were never painted
        for (auto it = latStamps_.begin(); it != latStamps_.end();) {
            it = (now - std::max(it->created, it->chart) > 30000) ? latStamps_.erase(it) : std::next(it);
        }
    }
}

void DataProcessor::latencyStamp(LatStage stage, const QVector<int>& indxs)
{
    for (int i : indxs) {
        latencyStamp(stage, i, i);
    }
}

// An epoch counts when the mosaic first reports it painted (MosaicProcessor posts its tiles before
// that report). 76c1e271 counted it at the end of a job that still had it in flight -- but this
// very report takes a painted epoch out of flight first, so it counted only epochs a job left
// unpainted, and those late: "painting 100-150 ms" was not the time to the first paint.
void DataProcessor::latencyFirstPaint(const QVector<int>& indxs)
{
    if (datasetState_ != 2 || isOpeningFile_ || !updateMosaic_) {
        return;
    }
    latPaintReports_ += indxs.size();
    if (latStamps_.isEmpty()) {
        return;
    }
    const qint64 now = latClock_.elapsed();
    for (int idx : indxs) {
        auto it = latStamps_.find(idx);
        if (it == latStamps_.end()) {
            continue;
        }
        const EpochStamps s = it.value();
        latStamps_.erase(it);
        if (s.chart <= 0) {
            continue;
        }
        // From the epoch's appearance when known: the chart's release can itself trail it -- the
        // black-stripe filter holds the newest charts back by its backward steps (the owner's FBS
        // 5/5, ~250 ms at 20 pings/s, unseen by this line while it started at the release).
        latTotal_.append(int(now - (s.created > 0 ? s.created : s.chart)));
        if (s.created > 0)  latChartWait_.append(int(s.chart - s.created));
        if (s.pending > 0)  latBottom_.append(int(s.pending - s.chart));
        if (s.released > 0) latHorizon_.append(int(s.released - s.chart));
        if (s.sent > 0) {
            latQueue_.append(int(s.sent - std::max({s.chart, s.pending, s.released})));
            latPaint_.append(int(now - s.sent));
        }
    }
}

void DataProcessor::latencyReport()
{
    const qint64 now = latClock_.elapsed();
    if (now - latLastReport_ < 10000 || latTotal_.isEmpty()) {
        return;
    }
    const auto params = datasetPtr_ ? datasetPtr_->getBottomTrackParam() : BottomTrackParam();
    qInfo().noquote() << QStringLiteral("mosaic latency: %1 pings in %2 s, ping -> first painted median %3 ms (p90 %4, max %5); "
                                        "chart released after %6 ms, then waiting for the bottom track %7 ms, for the horizon %8 ms, "
                                        "in the queue %9 ms, painting %10 ms; painted %11 times a ping; bottom track window %12, "
                                        "black-stripe filter holds %13 pings")
                             .arg(latTotal_.size())
                             .arg((now - latLastReport_) / 1000.0, 0, 'f', 1)
                             .arg(latMedian(latTotal_))
                             .arg(latPercentile(latTotal_, 90))
                             .arg(*std::max_element(latTotal_.cbegin(), latTotal_.cend()))
                             .arg(latMedian(latChartWait_))
                             .arg(latMedian(latBottom_))
                             .arg(latMedian(latHorizon_))
                             .arg(latMedian(latQueue_))
                             .arg(latMedian(latPaint_))
                             .arg(double(latPaintReports_) / latTotal_.size(), 0, 'f', 1)
                             .arg(params.windowSize)
                             .arg(datasetPtr_ ? datasetPtr_->chartHoldEpochs() : 0);
    qInfo().noquote() << QStringLiteral("mosaic latency, per job: surface %1 ms median (max %2) in %3 of %4 jobs, of which "
                                        "waiting for the GUI thread %10 ms median (max %11), %5 new points, %6 points in the "
                                        "surface; mosaic %7 ms median (max %8), %9 epochs")
                             .arg(latMedian(latSurfaceMs_))
                             .arg(latSurfaceMs_.isEmpty() ? -1 : *std::max_element(latSurfaceMs_.cbegin(), latSurfaceMs_.cend()))
                             .arg(latSurfaceMs_.size())
                             .arg(latJobs_)
                             .arg(latSurfaceItems_)
                             .arg(latSurfacePoints_)
                             .arg(latMedian(latMosaicMs_))
                             .arg(latMosaicMs_.isEmpty() ? -1 : *std::max_element(latMosaicMs_.cbegin(), latMosaicMs_.cend()))
                             .arg(latMosaicEpochs_)
                             .arg(latMedian(latSurfaceWaitMs_))
                             .arg(latSurfaceWaitMs_.isEmpty() ? -1 : *std::max_element(latSurfaceWaitMs_.cbegin(), latSurfaceWaitMs_.cend()));
    latSurfaceMs_.clear();
    latSurfaceWaitMs_.clear();
    latPaintReports_ = 0;
    latMosaicMs_.clear();
    latSurfaceItems_ = 0;
    latMosaicEpochs_ = 0;
    latJobs_ = 0;
    latTotal_.clear();
    latChartWait_.clear();
    latBottom_.clear();
    latHorizon_.clear();
    latQueue_.clear();
    latPaint_.clear();
    latLastReport_ = now;
}

void DataProcessor::postBundleTiming(int surfaceMs, int surfaceWaitMs, int surfaceItems, int surfacePoints, int mosaicMs, int mosaicEpochs)
{
    if (datasetState_ != 2 || isOpeningFile_ || !updateMosaic_) {
        return;
    }
    ++latJobs_;
    if (surfaceMs >= 0) {
        latSurfaceMs_.append(surfaceMs);
        latSurfaceItems_ += surfaceItems;
    }
    if (surfaceWaitMs >= 0) {
        latSurfaceWaitMs_.append(surfaceWaitMs);
    }
    latSurfacePoints_ = surfacePoints;
    if (mosaicMs >= 0) {
        latMosaicMs_.append(mosaicMs);
        latMosaicEpochs_ += mosaicEpochs;
    }
}

void DataProcessor::latencyReset()
{
    latStamps_.clear();
    latLastChart_ = -1;
    latLastEpoch_ = -1;
    latLastPosition_ = -1;
    latLastReleased_ = -1;
    latChartWait_.clear();
    latTotal_.clear();
    latBottom_.clear();
    latHorizon_.clear();
    latQueue_.clear();
    latPaint_.clear();
    latSurfaceWaitMs_.clear();
    latPaintReports_ = 0;
}

void DataProcessor::onDbTilesLoaded(const QList<DbTile> &dbTiles)
{
    if (shuttingDown_.load() || suppressResults_.load()) {
        dbReaderInWork_ = false;
        dbInWorkKeys_.clear();
        notifyPrefetchProgress();
        return;
    }

    dbReaderInWork_ = false;
    const QSet<TileKey> requestedNow = dbInWorkKeys_;
    dbInWorkKeys_.clear();

    TileMap outTiles; // for HotCache
    outTiles.reserve(dbTiles.size());
    QVector<TileKey> loadedKeys;
    loadedKeys.reserve(dbTiles.size());

    for (const DbTile& dt : dbTiles) {
        if (dt.gen < dbValidFrom(dt.key.zoom)) {
            continue;   // saved before its level was invalidated: "not found", painted afresh
        }
        SurfaceTile tile(dt.key, QVector3D(dt.originX, dt.originY, 0.0f));
        tile.init(dt.tilePx, dt.hmRatio, mppFromZoom(dt.key.zoom));
        if (dt.headIndx != -1 &&!dt.mosaicBlob.isEmpty()) {
            auto bytes = MosaicDB::unpackRaw8(dt.mosaicBlob);
            tile.initImageData(dt.tilePx, dt.hmRatio);
            auto& img = tile.getMosaicImageDataRef();
            if (static_cast<int>(bytes.size()) == int(img.size())) {
                memcpy(img.data(), bytes.data(), size_t(bytes.size()));
                if (!dt.qualityBlob.isEmpty()) {
                    auto quality = MosaicDB::unpackRaw8(dt.qualityBlob);
                    if (quality.size() == img.size()) {
                        tile.getMosaicQualityRef() = std::move(quality);
                    }
                }
            }
        }
        if (dt.hasHeight && !dt.heightBlob.isEmpty()) {
            auto& verts = tile.getHeightVerticesRef();
            MosaicDB::unpackFloat32(dt.heightBlob, verts, dt.hmRatio);
        }
        if (dt.hasMarks && !dt.marksBlob.isEmpty()) {
            auto& marks = tile.getHeightMarkVerticesRef();
            MosaicDB::unpackMarksU8(dt.marksBlob, marks);
        }
        tile.setHeadIndx(dt.headIndx);
        tile.updateHeightIndices();
        tile.setIsUpdated(false);

        outTiles.insert(dt.key, tile);
        loadedKeys.push_back(dt.key);

        nfErase(dt.key);
    }

    if (!outTiles.isEmpty()) {
        hotCache_.putBatch(std::move(outTiles), DataSource::kDataBase);
        notifyPrefetchProgress(); // сообщить префетчерам
    }

    if (!loadedKeys.isEmpty()) {
        QSet<TileKey> addNow;
        for (const auto& k : loadedKeys) {
            if (lastVisTileKeys_.contains(k) && !renderedKeys_.contains(k)) {
                addNow.insert(k);
            }
        }

        if (!addNow.isEmpty()) {
            QSet<TileKey> dummy;
            TileMap delta = hotCache_.getForKeys(addNow, &dummy);
            if (!delta.isEmpty()) {
                emitDelta(std::move(delta), DataSource::kDataBase);
            }
        }
    }

    if (!requestedNow.isEmpty()) {
        QSet<TileKey> loadedSet(loadedKeys.cbegin(), loadedKeys.cend());
        QSet<TileKey> notFoundNow = requestedNow;
        notFoundNow.subtract(loadedSet);
        if (!notFoundNow.isEmpty()) {
            for (const auto& k : notFoundNow) {
                // Not one painted meanwhile: marked "not found" while in the cache, it was taken
                // for empty once evicted and saved, and a painter seeded it blank and overwrote
                // the saved row (2026-10-18).
                if (!hotCache_.contains(k)) {
                    nfTouch(k);
                }
            }
            notifyPrefetchProgress(); // сообщить префетчерам
        }
    }

    if (!dbPendingKeys_.isEmpty()) {
        flushPendingDbKeys();
    }
}

void DataProcessor::postState(DataProcessorType s)
{
    emit sendState(state_ = s);
}

void DataProcessor::postDistCompletedByProcessing(int epIndx, const ChannelId &channelId, float dist)
{
    emit distCompletedByProcessing(epIndx, channelId, dist);
}

void DataProcessor::postDistCompletedByProcessingBatch(const QVector<BottomTrackUpdate>& updates)
{
    if (updates.isEmpty()) {
        return;
    }

    emit distCompletedByProcessingBatch(updates);
}

void DataProcessor::postLastBottomTrackEpochChanged(const ChannelId &channelId, int val, const BottomTrackParam &btP, bool manual, bool redrawAll)
{
    emit lastBottomTrackEpochChanged(channelId, val, btP, manual, redrawAll);
}

void DataProcessor::postMosaicColorTable(const std::vector<uint8_t>& t)
{
    emit sendMosaicColorTable(t);
}

void DataProcessor::postIsobathsLineSegments(const QVector<QVector3D>& lineSegments)
{
    emit sendIsobathsLineSegments(lineSegments);
}

void DataProcessor::requestPipelineStats()
{
    if (!worker_) {
        return;
    }

    QMetaObject::invokeMethod(worker_, "reportPipelineStats", Qt::QueuedConnection);
}

void DataProcessor::postPipelineStats(const QVariantMap& stats)
{
    QVariantMap out = stats;

    out["state"]             = static_cast<int>(state_);
    out["updateSurface"]     = updateSurface_;
    out["updateIsobaths"]    = updateIsobaths_;
    out["updateBottomTrack"] = updateBottomTrack_;
    out["updateMosaic"]      = updateMosaic_;
    out["openingFile"]       = isOpeningFile_;
    out["bottomTrackBusy"]   = btBusy_;
    out["dbReady"]           = isDbReady();
    out["lastEpochIndx"]     = static_cast<qulonglong>(epochCounter_);
    out["lastChartIndx"]     = static_cast<qulonglong>(chartsCounter_);
    out["queuedSurface"]     = pendingSurfaceIndxs_.size();
    out["queuedBtVertices"]  = epIndxsFromBottomTrack_.size();

    emit pipelineStats(out);
}

void DataProcessor::requestMosaicStats(int probeWindow)
{
    if (!worker_) {
        return;
    }

    QMetaObject::invokeMethod(worker_, "reportMosaicStats", Qt::QueuedConnection, Q_ARG(int, probeWindow));
}

MapExportConfig DataProcessor::mapExportConfig() const
{
    MapExportConfig c = exportConfig_;
    c.mosaicTheme = mosaicColorTable_.getTheme();
    c.mosaicLow = mosaicColorTable_.getLowLevel();
    c.mosaicHigh = mosaicColorTable_.getHighLevel();
    c.mosaicColorTable = mosaicColorTable_.getRgbaColors();
    c.beamModel = beamCorrection_ ? beamModel_ : nullptr;
    c.paintRules = beamRulesNow();
    return c;
}

void DataProcessor::postMosaicStats(const QVariantMap& stats)
{
    QVariantMap out = stats;

    out["state"]            = static_cast<int>(state_);
    out["updateMosaic"]     = updateMosaic_;
    out["updateBottomTrack"]= updateBottomTrack_;
    out["openingFile"]      = isOpeningFile_;
    out["dbReady"]          = isDbReady();
    out["zeroing"]          = activeZeroing_;
    out["fakeCoordsLastN"]  = mosaicFakeCoordsLastN_;
    out["lastMosaicIndx"]   = mosaicCounter_;
    out["queuedMosaic"]     = pendingMosaicIndxs_.size();

    emit mosaicStats(out);
}

void DataProcessor::onBottomTrackStarted()
{
    btBusy_ = true;
    updateIdle();
}

void DataProcessor::onBottomTrackFinished()
{
    auto idleGuard = qScopeGuard([this] { updateIdle(); });
    btBusy_ = false;
    bottomTrackChanged_.start();   // its depths reach the 3D bottom track after this (runEditRebuild)

    if (resetInProgress_.load()) {
        pendingManualBt_.clear();
        tryFinalizeResetProcessing();
        return;
    }

    if (!pendingManualBt_.isEmpty()) {
        dispatchBottomTrack(pendingManualBt_.takeFirst());   // before the automatic one, which follows it
        return;
    }

    tryScheduleAutoBottomTrack(epochCounter_);

    if (!forceVisibleRefreshAfterBottomTrack_) {
        return;
    }
    forceVisibleRefreshAfterBottomTrack_ = false;

    QTimer::singleShot(0, this, [this]() {
        if (shuttingDown_.load()) {
            return;
        }
        if (!updateSurface_ && !updateMosaic_) {
            return;
        }

        if (updateSurface_) {
            // After manual/redrawAll bottom-track runs, force one visible camera pass
            // so updated triangulation is rasterized without requiring camera movement.
            surfaceCameraPassPending_ = true;
        }

        onCameraMoved();
        if (updateMosaic_) {
            pumpVisible();
        }
    });
}

void DataProcessor::onSendSavedKeys(QVector<TileKey> savedKeys, quint64 saveId)
{
    // In the DB now, whatever an earlier read said; not a level dropped meanwhile (dropPrecomputed
    // marked its keys "not found" so that the rows painted before do not come back). A read whose
    // SELECT ran before this commit must not mark them "not found" either.
    for (const TileKey& k : std::as_const(savedKeys)) {
        if (hotCache_.contains(k)) {
            nfErase(k);
            dbInWorkKeys_.remove(k);
        }
    }
    hotCache_.onSendSavedTiles(savedKeys, saveId);
}

void DataProcessor::requestTilesFromDB(const QSet<TileKey>& keys)
{
    if (shuttingDown_.load() || suppressResults_.load() || !dbReader_ || keys.isEmpty()) {
        return;
    }
    // FAKE_COORDS + bounded window: DB holds pre-erase paint that doesn't match the rolling
    // window state. Showing it would leak "old garbage" into the renderer.
    if (activeZeroing_ && mosaicFakeCoordsLastN_ > 0) {
        return;
    }

    dbPendingKeys_.unite(keys);

    flushPendingDbKeys();
}

quint64 DataProcessor::prefetchProgressTick() const
{
    return prefetchTick_.load(std::memory_order_relaxed);
}

void DataProcessor::prefetchWait(quint64 lastTick)
{
    QMutexLocker lk(&prefetchMu_);

    // ждём пока тик изменится (или разбудят)
    while (prefetchTick_.load(std::memory_order_relaxed) == lastTick) {
        if (shuttingDown_.load() || isCancelRequested()) {
            break;
        }
        prefetchCv_.wait(&prefetchMu_, 200);
    }
}

void DataProcessor::flushPendingDbKeys()
{
    if (shuttingDown_.load() || suppressResults_.load() || !dbReader_ || dbPendingKeys_.isEmpty() || dbReaderInWork_) {
        return;
    }

    QSet<TileKey> dbReq;
    dbReq.reserve(dbPendingKeys_.size());

    for (auto it = dbPendingKeys_.begin(); it != dbPendingKeys_.end(); ) {
        const auto& k = *it;
        if (!dbInWorkKeys_.contains(k)) {
            dbReq.insert(k);
            dbInWorkKeys_.insert(k);
            it = dbPendingKeys_.erase(it);
        }
        else {
            ++it;
        }
    }

    if (dbReq.isEmpty()) {
        return;
    }

    if (dbIsReady_.load(std::memory_order_relaxed)) {
        dbReaderInWork_ = true;
        emit dbLoadTilesForKeys(dbReq);
    }
}

void DataProcessor::postIsobathsLabels(const QVector<IsobathUtils::LabelParameters>& labels)
{
    emit sendIsobathsLabels(labels);
}

void DataProcessor::postSurfaceTiles(TileMap tiles, bool isMosaic)
{
    if (tiles.isEmpty()) {
        return;
    }
    if (suppressResults_.load() || (isMosaic && dropRunningMosaicJob_)) {
        notifyPrefetchProgress();
        return;
    }
    if (!isMosaic && dropRunningMosaicJob_) {
        // The heights are current; the paint and its head index are from before the reset (the worker
        // clears its mesh after this job): keep only what that clear leaves.
        for (auto it = tiles.begin(); it != tiles.end(); ++it) {
            it.value().setHeadIndx(-1);
            auto& img = it.value().getMosaicImageDataRef();
            std::fill(img.begin(), img.end(), uint8_t(0));
            it.value().dropMosaicQuality();
        }
    }

    for (auto it = tiles.cbegin(); it != tiles.cend(); ++it) { // удалить из не найдено
        nfErase(it.key());
    }

    bool canSendToRender = false;
    if (isMosaic) {
        canSendToRender = updateMosaic_;
    } else {
        canSendToRender = updateSurface_;
    }

    if (canSendToRender) { // в рендер — только видимые
        TileMap prepaired;
        for (auto it = tiles.cbegin(); it != tiles.cend(); ++it) {
            const auto& k = it.key();
            if (lastVisTileKeys_.contains(k)) {
                prepaired.insert(k, it.value());
            }
        }

        if (!prepaired.isEmpty()) {
            emitDelta(std::move(prepaired), DataSource::kCalculation);
        }
    }

    hotCache_.putBatch(std::move(tiles), DataSource::kCalculation);
    notifyPrefetchProgress(); // сообщить префетчерам
}

void DataProcessor::postMinZ(float val)
{
    QMetaObject::invokeMethod(worker_, "setMinZ", Qt::QueuedConnection, Q_ARG(float, val));

    emit sendSurfaceMinZ(val); // to surface view

    if (!updateMosaic_) {
        pendingIsobathsWork_ = true;
        scheduleLatest(WorkSet(WF_Isobaths));
    }
}

void DataProcessor::postMaxZ(float val)
{
    QMetaObject::invokeMethod(worker_, "setMaxZ", Qt::QueuedConnection, Q_ARG(float, val));

    emit sendSurfaceMaxZ(val); // to surface view

    if (!updateMosaic_) {
        pendingIsobathsWork_ = true;
        scheduleLatest(WorkSet(WF_Isobaths));
    }
}

void DataProcessor::postSurfaceColorTable(const std::vector<uint8_t> &t)
{
    lastSurfaceColorTable_ = t;
    emit sendSurfaceTextureTask(t);
}

void DataProcessor::postSurfaceColorIntervalsSize(int size)
{
    lastSurfaceColorIntervalsSize_ = size;
    emit sendSurfaceColorIntervalsSize(size);
}

void DataProcessor::postSurfaceStepSize(float lineStepSize)
{
    lastSurfaceStepSize_ = lineStepSize;
    emit sendSurfaceStepSize(lineStepSize);
}

void DataProcessor::resendSurfaceColorTable()
{
    if (!lastSurfaceColorTable_.empty()) {
        emit sendSurfaceTextureTask(lastSurfaceColorTable_);
    }
    if (lastSurfaceColorIntervalsSize_ > 0) {
        emit sendSurfaceColorIntervalsSize(lastSurfaceColorIntervalsSize_);
    }
    if (lastSurfaceStepSize_ > 0.0f) {
        emit sendSurfaceStepSize(lastSurfaceStepSize_);
    }
}

void DataProcessor::changeState(const DataProcessorType& state)
{
    emit sendState(state_ = state);
}

void DataProcessor::clearBottomTrackProcessing()
{
    bottomTrackWindowCounter_ = 0;
    btAutoFrom_ = 0;
    btBusy_ = false;
    pendingManualBt_.clear();
    forceVisibleRefreshAfterBottomTrack_ = false;
    updateIdle();

    QMetaObject::invokeMethod(btWorker_, "clearBottomTrack", Qt::QueuedConnection);
}

void DataProcessor::clearIsobathsProcessing()
{
    pendingIsobathsWork_ = false;

    QMetaObject::invokeMethod(worker_, "clearIsobaths", Qt::QueuedConnection);
}

void DataProcessor::clearMosaicProcessing()
{
    cancelPrecompute();
    clearOldTracking_ = false;
    pendingMosaicIndxs_.clear();
    resetMosaicProcessed();
    mosaicInFlightIndxs_.clear();

    QMetaObject::invokeMethod(worker_, "clearMosaic", Qt::QueuedConnection);
}

void DataProcessor::clearSurfaceProcessing()
{
    pendingSurfaceIndxs_.clear();
    surfaceCameraPassPending_ = false;
    surfaceTaskEpochIndxsByZoom_.clear();
    surfaceManualEpochIndxsByZoom_.clear();
    surfaceEdgeLimitUpdatedZooms_.clear();

    QMetaObject::invokeMethod(worker_, "clearSurface", Qt::QueuedConnection);
}

void DataProcessor::clearAllProcessings()
{
    auto idleGuard = qScopeGuard([this] { updateIdle(); });
    closeDB();
    pendingWorkTimer_.stop();
    cameraRectCoalesceTimer_.stop();
    pendingIsobathsWork_ = false;
    pendingMosaicIndxs_.clear();
    resetMosaicProcessed();
    mosaicInFlightIndxs_.clear();
    pendingSurfaceIndxs_.clear();
    surfaceCameraPassPending_ = false;
    surfaceTaskEpochIndxsByZoom_.clear();
    surfaceManualEpochIndxsByZoom_.clear();
    surfaceEdgeLimitDirty_ = false;
    surfaceEdgeLimitUpdatedZooms_.clear();
    epIndxsFromBottomTrack_.clear();
    vertIndxsFromBottomTrack_.clear();
    epochToBottomTrackVertIndx_.clear();
    beamResetEstimate(true);   // another recording or connection: measured afresh
    beamPush();
    beamEmitState();
    clearOldTracking_ = false;
    bottomTrackWindowCounter_ = 0;
    btAutoFrom_ = 0;
    btBusy_ = false;
    pendingManualBt_.clear();
    editRebuildPending_ = false;
    editRebuildSurface_ = false;
    editRebuildTimer_.stop();
    forceVisibleRefreshAfterBottomTrack_ = false;

    QMetaObject::invokeMethod(worker_, "clearAll", Qt::QueuedConnection);
    cancelPrecompute();
    QMetaObject::invokeMethod(precomputer_, "clear", Qt::QueuedConnection);

    state_ = DataProcessorType::kUndefined;

    chartsCounter_ = 0;
    bottomTrackCounter_ = 0;
    epochCounter_ = 0;
    positionCounter_ = 0;
    attitudeCounter_ = 0;
    mosaicCounter_ = 0;
    cancelRequested_ = false;
    latencyReset();
    jobRunning_ = false;
    nextRunPending_ = false;
    requestedMask_ = 0;
    hotCache_.clear();
    dbReaderInWork_ = false;
    pendingCameraRect_ = QRectF();
    cameraRectPending_ = false;
    cameraRectProcessing_ = false;
    requeueViewRect();
    //lastDataZoomIndx_ = 0;
    dbPendingKeys_.clear();
    dbInWorkKeys_.clear();
    renderedKeys_.clear();

    dbNotFoundIndxs_.clear();
    dbNotFoundOrder_.clear();
    dbNotFoundPos_.clear();

    emit isobathsProcessingCleared();
    emit surfaceProcessingCleared();
    emit mosaicProcessingCleared();
}

void DataProcessor::scheduleLatest(WorkSet mask, bool replace, bool clearUnrequestedPending) noexcept
{
    if (shuttingDown_.load()) {
        return;
    }
    if (resetInProgress_.load()) {
        return;
    }

    const uint32_t m = toMask(mask);
    if (defProcType_) { // interrupt previous task
        if (jobRunning_.load()) {
            cancelRequested_.store(true);
            if (m & WF_Mosaic) {
                for (int idx : std::as_const(mosaicInFlightIndxs_)) {
                    pendingMosaicIndxs_.insert(idx);
                }
                mosaicInFlightIndxs_.clear();
            }
        }
    }
    if (replace) {
        requestedMask_.store(m);
    }
    else {
        requestedMask_.fetch_or(m);
    }

    if (clearUnrequestedPending) {
        if (!(m & WF_Surface)) {
            pendingSurfaceIndxs_.clear();
        }
        if (!(m & WF_Mosaic)) {
            pendingMosaicIndxs_.clear();
        }
        if (!(m & WF_Isobaths)) {
            pendingIsobathsWork_ = false;
        }
    }

    nextRunPending_.store(true);

    if (replace && jobRunning_.load()) {
        cancelRequested_.store(true);
    }

    startTimerIfNeeded();
}

void DataProcessor::openDB()
{
    if (shuttingDown_.load() || dbReader_ || dbWriter_) {
        return;
    }

    if (const QString dbPath = MosaicDB::surfaceDbPath(); !MosaicDB::removeDbFiles(dbPath)) {
        qWarning() << "Failed to cleanup previous mosaic cache" << dbPath;
    }

    // writer
    dbWriter_ = new MosaicDB(DbRole::Writer, true /*delete temp db files*/);
    MosaicDB* const writer = dbWriter_;
    writer->moveToThread(&dbWriteThread_);

    QMetaObject::invokeMethod(writer, [writer]() {
        if (!writer->open()) {
            qWarning() << "DB Writer open failed";
        }
    }, Qt::QueuedConnection);

    // схема готова - запускаем reader
    connect(writer, &MosaicDB::schemaReady, this, [this, writer]() {
        if (writer != dbWriter_ || shuttingDown_.load()) {
            return;
        }

        dbIsReady_.store(true, std::memory_order_relaxed);

        // reader
        dbReader_ = new MosaicDB(DbRole::Reader);
        MosaicDB* const reader = dbReader_;
        reader->moveToThread(&dbReadThread_);
        QMetaObject::invokeMethod(reader, [reader]() {
            if (!reader->open()) {
                qWarning() << "DB Reader open failed";
            }
        }, Qt::QueuedConnection);

        // reader conns
        connect(this,   &DataProcessor::dbLoadTilesForKeys,    reader, &MosaicDB::loadTilesForKeys,        Qt::QueuedConnection);
        connect(reader, &MosaicDB::tilesLoadedForKeys,         this,   &DataProcessor::onDbTilesLoaded,    Qt::QueuedConnection);
        connect(this,   &DataProcessor::dbCheckAnyTileForZoom, reader, &MosaicDB::checkAnyTileForZoom,     Qt::QueuedConnection);
        connect(reader, &MosaicDB::anyTileForZoom,             this,   &DataProcessor::onDbAnyTileForZoom, Qt::QueuedConnection);

        dbReadThread_.setObjectName("MosaicDBReaderThread");
        dbReadThread_.start();

        flushPendingDbKeys(); // были запросы
    }, Qt::QueuedConnection);

    // writer conns
    connect(this,   &DataProcessor::dbSaveTiles,  writer, &MosaicDB::saveTiles,        Qt::QueuedConnection);
    connect(writer, &MosaicDB::sendSavedKeys,     this,   &DataProcessor::onSendSavedKeys, Qt::QueuedConnection);
    connect(writer, &MosaicDB::writerFailed, this, [this, writer]() {
        if (writer != dbWriter_ || shuttingDown_.load()) {
            return;   // from a writer closeDB has deleted already
        }
        onDbWriterFailed();
    }, Qt::QueuedConnection);
    dbWriterFailed_ = false;

    dbWriteThread_.setObjectName("MosaicDBWriterThread");
    dbWriteThread_.start();
}

void DataProcessor::closeDB()
{
    dbReadThread_.requestInterruption();
    dbWriteThread_.requestInterruption();

    auto finalizeAndDelete = [](MosaicDB* db, QThread& thread) {
        if (!db) {
            return;
        }

        if (db->thread() == QThread::currentThread()) {
            db->finalizeAndClose();
            delete db;
            return;
        }

        if (thread.isRunning()) {
            QMetaObject::invokeMethod(db, [db]() {
                db->finalizeAndClose();
                delete db;
            }, Qt::BlockingQueuedConnection);
            return;
        }

        db->finalizeAndClose();
        delete db;
    };

    MosaicDB* const reader = dbReader_;
    dbReader_ = nullptr;
    finalizeAndDelete(reader, dbReadThread_);
    if (dbReadThread_.isRunning()) {
        dbReadThread_.quit();
        dbReadThread_.wait();
    }

    MosaicDB* const writer = dbWriter_;
    dbWriter_ = nullptr;
    finalizeAndDelete(writer, dbWriteThread_);
    if (dbWriteThread_.isRunning()) {
        dbWriteThread_.quit();
        dbWriteThread_.wait();
    }
    dbWriterFailed_ = false;   // the next DB gets a writer of its own; the precompute may run again

    dbIsReady_.store(false, std::memory_order_relaxed);
    notifyPrefetchProgress(); // сообщить префетчерам

    if (const QString dbPath = MosaicDB::surfaceDbPath(); !MosaicDB::removeDbFiles(dbPath)) {
        qWarning() << "Failed to remove mosaic cache" << dbPath;
    }

    //qDebug() << "DB closed";
}

void DataProcessor::emitDelta(TileMap &&upserts, DataSource src)
{
    if (upserts.isEmpty()) {
        return;
    }

    markTilesBySource(upserts, src);
    for (auto it = upserts.begin(); it != upserts.end(); ++it) {
        it.value().dropMosaicQuality();   // painting's business, not the renderer's (64 KB a tile)
    }

    // в рендер: сам удалит всё, чего нет
    emit sendSurfaceTilesIncremental(upserts, lastVisTileKeys_);

    // «что отрисовано»
    renderedKeys_.intersect(lastVisTileKeys_); // выкидываем то, что больше не видно (проверить)
    for (auto it = upserts.cbegin(); it != upserts.cend(); ++it) {
        renderedKeys_.insert(it.key());
    }
}

void DataProcessor::pumpVisible()
{
    QSet<TileKey> needAdd = lastVisTileKeys_;

    // check RENDER, PENDING DB/PROC
    needAdd.subtract(renderedKeys_);
    needAdd.subtract(dbPendingKeys_);
    if (needAdd.empty()) {
        //qDebug() << "needAdd.empty() - return";
        return;
    }

    // CHECK IN HOT CACHE
    QSet<TileKey> missFromHotCache;
    TileMap fromHotCache = hotCache_.getForKeys(needAdd, &missFromHotCache);

    if (!fromHotCache.isEmpty()) {
        emitDelta(std::move(fromHotCache), DataSource::kHotCache);
    }
    if (missFromHotCache.isEmpty()) {
        return;
    }

    missFromHotCache.subtract(dbNotFoundIndxs_); // убрать те, которых нет в бд

    if (missFromHotCache.isEmpty()) {
        return;
    }

    requestTilesFromDB(missFromHotCache);
}

bool DataProcessor::isValidZoomIndx(int zoomIndx) const
{
    return zoomIndx >= mosaicIndexProvider_.getMaxZoom() && zoomIndx <= mosaicIndexProvider_.getMinZoom();
}

void DataProcessor::handleSurfaceZoomChangeIfReady(int zoom, const QSet<TileKey>& keys)
{
    if (!surfaceZoomChangedPending_) {
        return;
    }

    if (!updateSurface_) {
        surfaceZoomChangedPending_ = false;
        return;
    }

    // A precompute kept over a zoom to this level still sends its tiles (cancelPrecompute(true)):
    // copied into the worker before they arrive, the restored copies would be what the next job paints
    // onto and posts. onPrecomputeFinished calls this again.
    if (precomputeRunning_ && precomputeRunTarget_ == zoom) {
        return;   // surfaceZoomChangedPending_ stays set
    }

    if (keys.isEmpty()) {
        return;
    }

    for (const TileKey& k : keys) {
        if (k.zoom != zoom) {
            return;
        }
    }

    QSet<TileKey> missing;
    TileMap cached = hotCache_.getForKeys(keys, &missing);

    const bool fullCoverage = missing.isEmpty() && !cached.isEmpty();
    QMetaObject::invokeMethod(worker_, "applySurfaceZoomChange", Qt::QueuedConnection,
                              Q_ARG(TileMap, cached),
                              Q_ARG(bool, fullCoverage));

    if (surfaceEdgeLimitDirty_ && !surfaceEdgeLimitUpdatedZooms_.contains(zoom)) {
        surfaceEdgeLimitUpdatedZooms_.insert(zoom);
        if (fullCoverage) {
            QMetaObject::invokeMethod(worker_, "reapplySurfaceEdgeLimit", Qt::QueuedConnection);
        }
    }

    if (updateIsobaths_ && !updateMosaic_) {
        pendingIsobathsWork_ = true;
        scheduleLatest(WorkSet(WF_Isobaths));
    }

    surfaceZoomChangedPending_ = false;
}

void DataProcessor::nfTouch(const TileKey &k)
{
    auto it = dbNotFoundPos_.find(k);
    if (it != dbNotFoundPos_.end()) {
        dbNotFoundOrder_.splice(dbNotFoundOrder_.end(), dbNotFoundOrder_, it.value()); // в хвост
        return;
    }

    dbNotFoundOrder_.push_back(k); // новый
    auto newIt = dbNotFoundOrder_.end();
    --newIt; // только что добавленный
    dbNotFoundPos_.insert(k, newIt);
    dbNotFoundIndxs_.insert(k);

    if (static_cast<int>(dbNotFoundOrder_.size()) > dbNotFoundLimit_) { // переполнение
        const TileKey& old = dbNotFoundOrder_.front();
        dbNotFoundPos_.remove(old);
        dbNotFoundIndxs_.remove(old);
        dbNotFoundOrder_.pop_front();
    }
}

void DataProcessor::nfErase(const TileKey &k)
{
    auto it = dbNotFoundPos_.find(k);
    if (it == dbNotFoundPos_.end()) {
        return;
    }

    dbNotFoundOrder_.erase(it.value());
    dbNotFoundPos_.erase(it);
    dbNotFoundIndxs_.remove(k);
}

bool DataProcessor::isCanStartCalculations() const
{
    return chartsCounter_ || bottomTrackCounter_ || epochCounter_ || positionCounter_ || attitudeCounter_ || mosaicCounter_;
}

void DataProcessor::requestCancel() noexcept
{
    nextRunPending_.store(true);
    cancelRequested_.store(true);
    notifyPrefetchProgress(); // сообщить префетчерам
}

void DataProcessor::onUpdateDataZoom(int zoom) // calc or db
{

    if (!isValidZoomIndx(zoom)) {
        return;
    }

    if (zoom == lastDataZoomIndx_) {
        //qDebug() << "Skip: zoom already applied" << zoom;
        return;
    }

    //
    pendingIsobathsWork_ = false;
    pendingMosaicIndxs_.clear();
    mosaicInFlightIndxs_.clear();
    pendingSurfaceIndxs_.clear();

    qInfo().noquote() << QStringLiteral("mosaic zoom: %1 -> %2").arg(lastDataZoomIndx_).arg(zoom);
    lastDataZoomIndx_ = zoom;

    const float pxPerMeter = ZL[zoom - 1].pxPerMeter;
    if (!std::isfinite(pxPerMeter)) {
        return;
    }
    //qDebug() << "pxPerMeter" << pxPerMeter;

    tileResolution_ = 1.0f / pxPerMeter;
    QMetaObject::invokeMethod(worker_, "setMosaicTileResolution", Qt::QueuedConnection, Q_ARG(float, tileResolution_));
    // The visible tiles at the new level, from the view's last rect: the view sends a level once the
    // camera has kept it a moment (GraphicsScene3dView::applyPendingDataZoom), by then the rect itself
    // need not change, and an unchanged rect is not recomputed.
    requeueViewRect();
    cancelPrecompute(true);   // what it painted stays; on this level the view's job waits for it (runCoalescedWork)
    precomputedZooms_.remove(zoom);

    surfaceZoomChangedPending_ = updateSurface_;
    surfaceCameraPassPending_ = updateSurface_;
    handleSurfaceZoomChangeIfReady(lastDataZoomIndx_, lastVisTileKeys_);

    if (!updateMosaic_) {
        return;
    }

    emit mosaicProcessingCleared();
    emit surfaceProcessingCleared();

    // FAKE_COORDS + bounded last-N: wipe mosaic state on zoom change and replay only the last N
    // epochs. Without this the new zoom would either lazy-erase huge backlogs as soon as the
    // first new epoch arrives, or inherit stale paint from a prior visit.
    if (activeZeroing_ && mosaicFakeCoordsLastN_ > 0) {
        restartMosaic();
        return;
    }

    if (hotCache_.checkAnyTileForZoom(lastDataZoomIndx_)) {
        pumpVisible();
    }
    else {
        if (dbIsReady_) {
            emit dbCheckAnyTileForZoom(lastDataZoomIndx_);
        }
    }
}

void DataProcessor::setFilePath(QString filePath)
{
    Q_UNUSED(filePath);
}

void DataProcessor::onSendDataRectRequest(float minX, float minY, float maxX, float maxY)
{
    if (resetInProgress_.load()) {
        return;
    }

    if (!cameraRectProcessing_) {
        pendingCameraRect_ = QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
        viewRectFromView_ = pendingCameraRect_;
        haveViewRect_ = true;
        cameraRectPending_ = true;
        if (!cameraRectCoalesceTimer_.isActive()) {
            cameraRectCoalesceTimer_.start();
        }
        return;
    }

    int reqZoom = lastDataZoomIndx_;
    if (!isValidZoomIndx(reqZoom)) {
        reqZoom = zoomFromMpp(tileResolution_);
    }
    if (!isValidZoomIndx(reqZoom)) {
        const int minZoom = mosaicIndexProvider_.getMaxZoom();
        const int maxZoom = mosaicIndexProvider_.getMinZoom();
        if (minZoom <= maxZoom) {
            if (reqZoom <= 0) {
                reqZoom = minZoom;
            }
            reqZoom = std::clamp(reqZoom, minZoom, maxZoom);
        }
    }
    if (!isValidZoomIndx(reqZoom)) {
        return;
    }

    if (!isValidZoomIndx(lastDataZoomIndx_)) {
        lastDataZoomIndx_ = reqZoom;
        const float mpp = mppFromZoom(reqZoom);
        if (std::isfinite(mpp) && mpp > 0.0f) {
            tileResolution_ = mpp;
            QMetaObject::invokeMethod(worker_, "setMosaicTileResolution", Qt::QueuedConnection, Q_ARG(float, tileResolution_));
        }
    }

    // rect
    const QRectF viewRect(QPointF(minX, minY), QPointF(maxX, maxY));
    if (viewRect == lastViewRect_) {
        return;
    }

    // tiles
    //const auto tiles = mosaicIndexProvider_.tilesInRectNed(viewRect, zoomIndx, /*padTiles*/1); // been
    std::array<QPointF, 4> visQuad = { viewRect.topLeft(), viewRect.topRight(), viewRect.bottomRight(), viewRect.bottomLeft() };
    const auto tiles = mosaicIndexProvider_.tilesInQuadNed(visQuad, reqZoom, 1); // new
    if (tiles == lastVisTileKeys_) {
        lastViewRect_ = viewRect;
        return;
    }

    QSet<TileKey> addedTiles;
    if (lastVisTilesZoom_ == reqZoom) {
        addedTiles = tiles;
        addedTiles.subtract(lastVisTileKeys_);
    }

    QSet<TileKey> reopenTiles = addedTiles;
    if (!addedTiles.isEmpty()) {
        for (const TileKey& key : std::as_const(addedTiles)) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const TileKey neighbor(key.x + dx, key.y + dy, key.zoom);
                    if (tiles.contains(neighbor)) {
                        reopenTiles.insert(neighbor);
                    }
                }
            }
        }
    }

    if (lastVisTileKeys_.isEmpty() != tiles.isEmpty()) {
        qInfo().noquote() << QStringLiteral("mosaic view: %1 visible tiles at zoom %2, %3 epochs indexed without any")
                             .arg(tiles.size()).arg(reqZoom).arg(epochsWithoutView_);
        epochsWithoutView_ = 0;
    }
    QMetaObject::invokeMethod(worker_, "setVisibleTileKeys", Qt::QueuedConnection, Q_ARG(QSet<TileKey>, tiles));
    precomputeLastTarget_ = -1;   // another view: its nearest levels first again
    precomputeTimer_.start();

    // store new request
    lastViewRect_ = viewRect;
    lastVisTileKeys_ = tiles;
    lastVisTilesZoom_ = reqZoom;

    if (updateSurface_ && !reopenTiles.isEmpty()) {
        int zoom = reqZoom;
        if (zoom <= 0) {
            zoom = zoomFromMpp(tileResolution_);
        }
        if (zoom > 0) {
            auto& processedSurface = surfaceTaskEpochIndxsByZoom_[zoom];
            const QSet<int> addedSurfaceEpochs = collectSurfaceEpochsForTilesSet(zoom, reopenTiles);
            for (int idx : std::as_const(addedSurfaceEpochs)) {
                processedSurface.remove(idx);
            }
        }
        // When view enters previously unseen tiles, some crossing triangles may not be reseeded
        // by visible-vertex logic alone. Request a one-shot camera pass to fill these gaps.
        surfaceCameraPassPending_ = true;
    }

    handleSurfaceZoomChangeIfReady(reqZoom, tiles);

    if (!updateSurface_ && !updateMosaic_) {
        return;
    }

    pumpVisible();

    onCameraMoved();
}

void DataProcessor::tryCalcTiles()
{
    //qDebug() << "DataProcessor::tryCalcTiles";

    if (shuttingDown_.load()) {
        return;
    }

    emit isobathsProcessingCleared();
    emit surfaceProcessingCleared();
    emit mosaicProcessingCleared();

    // замена на то что пришло с камеры?
    for (auto it = epIndxsFromBottomTrack_.cbegin(); it != epIndxsFromBottomTrack_.cend(); ++it) {
        pendingMosaicIndxs_.insert(*it);
    }
    enqueueSurfaceMissingForZoom(lastDataZoomIndx_);
    pendingIsobathsWork_ = true;

    QMetaObject::invokeMethod(worker_, "setMosaicTileResolution", Qt::QueuedConnection, Q_ARG(float, tileResolution_));
    scheduleLatest(WorkSet(WF_All), /*replace*/true);
}

TileMap DataProcessor::fetchFromHotCache(const QSet<TileKey> &keys, QSet<TileKey> *missing)
{
    auto retVal = hotCache_.getForKeys(keys, missing);

    //if (!missing->empty())
    //{
    //    qDebug() << "perhaps marginTiles" << missing->size();
    //}

    return retVal;
}

void DataProcessor::filterNotFoundOut(const QSet<TileKey> &in, QSet<TileKey> *out)
{
    if (!out) {
        return;
    }

    QSet<TileKey> res = in;
    res.subtract(dbNotFoundIndxs_);

    *out = std::move(res);
}

void DataProcessor::shutdown()
{
    //qDebug() << "DataProcessor::shutdown()";

    if (shuttingDown_.exchange(true)) {
        return;
    }

    pendingWorkTimer_.stop();
    notifyPrefetchProgress();

    updateBottomTrackFromScene3d_ = false;
    updateBottomTrackFromSettings_ = false;
    updateBottomTrack_ = false;
    updateSurface_ = false;
    updateIsobaths_ = false;
    updateMosaic_ = false;
    updateDatasetSpatialIndexingState();

    pendingSurfaceIndxs_.clear();
    pendingMosaicIndxs_.clear();
    pendingIsobathsWork_ = false;
    requestedMask_.store(0);
    nextRunPending_.store(false);

    requestCancel();
    computeThread_.requestInterruption();
    btThread_.requestInterruption();
    dbReadThread_.requestInterruption();
    dbWriteThread_.requestInterruption();

    auto waitForThread = [](QThread& thread, int timeoutMs) -> bool {
        if (!thread.isRunning()) {
            return true;
        }

        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&thread, &QThread::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
        if (!thread.isRunning()) {
            return true;
        }
        loop.exec();
        return !thread.isRunning();
    };

    cancelPrecompute();
    precomputeTimer_.stop();
    precomputeThread_.requestInterruption();
    precomputeThread_.quit();
    if (!waitForThread(precomputeThread_, 3000)) {
        qWarning() << "MosaicPrecomputeThread did not stop in time; retrying graceful wait";
        precomputeThread_.quit();
        waitForThread(precomputeThread_, 7000);
    }

    computeThread_.quit();
    if (!waitForThread(computeThread_, 3000)) {
        qWarning() << "ComputeWorkerThread did not stop in time; retrying graceful wait";
        computeThread_.requestInterruption();
        computeThread_.quit();
        waitForThread(computeThread_, 7000);
    }

    btThread_.quit();
    if (!waitForThread(btThread_, 3000)) {
        qWarning() << "BtWorkerThread did not stop in time; retrying graceful wait";
        btThread_.requestInterruption();
        btThread_.quit();
        waitForThread(btThread_, 7000);
    }

    if (dbReader_) {
        if (dbReadThread_.isRunning()) {
            QMetaObject::invokeMethod(dbReader_, "finalizeAndClose", Qt::BlockingQueuedConnection);
        } else {
            dbReader_->finalizeAndClose();
        }
        dbReadThread_.quit();
        if (!waitForThread(dbReadThread_, 2000)) {
            qWarning() << "MosaicDBReaderThread did not stop in time; waiting until it exits";
            dbReadThread_.wait();
        }
        delete dbReader_;
        dbReader_ = nullptr;
    }

    if (dbWriter_) {
        if (dbWriteThread_.isRunning()) {
            QMetaObject::invokeMethod(dbWriter_, "finalizeAndClose", Qt::BlockingQueuedConnection);
        } else {
            dbWriter_->finalizeAndClose();
        }
        dbWriteThread_.quit();
        if (!waitForThread(dbWriteThread_, 2000)) {
            qWarning() << "MosaicDBWriterThread did not stop in time; waiting until it exits";
            dbWriteThread_.wait();
        }
        delete dbWriter_;
        dbWriter_ = nullptr;
    }

    if (precomputer_) {
        delete precomputer_;
        precomputer_ = nullptr;
    }

    if (worker_) {
        delete worker_;
        worker_ = nullptr;
    }

    dbIsReady_ = false;

    //qDebug() << "DataProcessor::shutdown()";
}

void DataProcessor::onSendTilesByZoom(int epochIndx, const QMap<int, QSet<TileKey> > &tilesByZoom)
{
    //qDebug() << "DataProcessor::onSendDimRects" << epochIndx;

    const bool skipSchedulingForDisabledLayers = datasetState_ != 2 && !updateMosaic_ && !updateSurface_;

    const int minZoom = 7;
    if (tileEpochIndxsByZoom_.size() < minZoom) {
        tileEpochIndxsByZoom_.resize(minZoom);
    }

    for (auto it = tilesByZoom.cbegin(); it != tilesByZoom.cend(); ++it) {
        const int zoom = it.key() - 1;
        if (zoom < 0 || zoom >= tileEpochIndxsByZoom_.size()) {
            continue;
        }

        auto& indexForZoom = tileEpochIndxsByZoom_[zoom];
        const QSet<TileKey>& tileSet = it.value();
        for (const TileKey& tk : tileSet) {
            auto& epochList = indexForZoom[tk];
            if (epochList.isEmpty() || epochList.back() != epochIndx) {
                epochList.push_back(epochIndx);
            }
        }
        precomputeAllTiles_[it.key()].unite(tileSet);
    }
    if (epochIndx >= 0 && !tilesByZoom.isEmpty()) {
        if (size_t(epochIndx) >= indexedEpochs_.size()) {
            indexedEpochs_.resize(size_t(epochIndx) + 1, 0);
        }
        indexedEpochs_[size_t(epochIndx)] = 1;
    }

    if (skipSchedulingForDisabledLayers) {
        return;
    }

    if (lastVisTileKeys_.isEmpty()) {   // the view has sent nothing since the last clear
        if (epochsWithoutView_++ == 0) {
            qInfo().noquote() << QStringLiteral("mosaic view: no visible tiles, epoch %1 not queued").arg(epochIndx);
        }
        return;
    }

    int zoom = lastDataZoomIndx_;
    if (zoom <= 0) {
        zoom = zoomFromMpp(tileResolution_);
    }
    if (zoom <= 0) {
        return;
    }

    const auto zoomTilesIt = tilesByZoom.constFind(zoom);
    if (zoomTilesIt == tilesByZoom.cend() || zoomTilesIt->isEmpty()) {
        return;
    }

    bool intersectsVisible = false;
    for (const TileKey& key : zoomTilesIt.value()) {
        if (lastVisTileKeys_.contains(key)) {
            intersectsVisible = true;
            break;
        }
    }
    if (!intersectsVisible) {
        return;
    }

    bool addedMosaic = false;
    bool addedSurface = false;

    if (updateMosaic_) {
        auto& processed = mosaicTaskEpochIndxsByZoom_[zoom];
        // An epoch's footprint is the stretch to the next epoch (Dataset::calcDimensionRects), so
        // it is indexed only when the next ping is in -- and a job paints up to its last epoch, so
        // the live mosaic ended one ping behind the newest (2026-10-17, the owner's circular
        // mosaic). Live, the next epoch is queued with it: its job paints up to the newest ping.
        // It is dispatched only once the mosaic horizon has reached it (runCoalescedWork).
        const bool live = (datasetState_ == 2) && !isOpeningFile_;
        for (const int ep : { epochIndx, epochIndx + 1 }) {
            if (ep != epochIndx && !live) {
                break;
            }
            if (!processed.contains(ep) &&
                !pendingMosaicIndxs_.contains(ep) &&
                !mosaicInFlightIndxs_.contains(ep)) {
                pendingMosaicIndxs_.insert(ep);
                addedMosaic = true;
            }
        }
    }

    if (updateSurface_) {
        auto& processedSurface = surfaceTaskEpochIndxsByZoom_[zoom];
        const auto& manualPending = surfaceManualEpochIndxsByZoom_[zoom];
        auto tryAddSurfaceByEpoch = [&](int epIdx) {
            const auto vertIt = epochToBottomTrackVertIndx_.constFind(epIdx);
            if (vertIt == epochToBottomTrackVertIndx_.cend()) {
                return;
            }
            const int vertIdx = vertIt.value();
            if (!vertIndxsFromBottomTrack_.contains(vertIdx)) {
                return;
            }
            if (processedSurface.contains(vertIdx) || manualPending.contains(vertIdx)) {
                return;
            }
            const QPair<char, int> pair('0', vertIdx);
            if (pendingSurfaceIndxs_.contains(pair)) {
                return;
            }
            pendingSurfaceIndxs_.insert(pair);
            addedSurface = true;
        };

        // epoch coverage is segment-based (epoch i -> i+1), surface is vertex-based.
        // Add both endpoints of the segment.
        tryAddSurfaceByEpoch(epochIndx);
        tryAddSurfaceByEpoch(epochIndx + 1);

    }

    WorkSet mask = WF_None;
    if (addedMosaic) {
        mask |= WF_Mosaic;
    }
    if (addedSurface) {
        mask |= WF_Surface;
    }
    if (toMask(mask) != 0) {
        scheduleLatest(mask);
    }
}

void DataProcessor::updateDatasetSpatialIndexingState()
{
    if (!datasetPtr_) {
        return;
    }

    bool sonarEnabled = true;
    bool dimRectEnabled = true;
    bool chunkedCatchup = false;

    if (datasetState_ != 2) {
        sonarEnabled = updateBottomTrack_ || updateSurface_ || updateIsobaths_ || updateMosaic_;
        dimRectEnabled = updateSurface_ || updateIsobaths_ || updateMosaic_;
        chunkedCatchup = true;
    }

    QMetaObject::invokeMethod(datasetPtr_, "setSpatialIndexingEnabled", Qt::QueuedConnection,
                              Q_ARG(bool, sonarEnabled),
                              Q_ARG(bool, dimRectEnabled),
                              Q_ARG(bool, chunkedCatchup));
}

void DataProcessor::onDatasetStateChanged(int state)
{
    if (datasetState_ == state) {
        return;
    }

    datasetState_ = state;

    updateDatasetSpatialIndexingState();
    updateDataProcType();
    if (beamAwaitFirst_) {
        scheduleLatest(WorkSet(WF_Mosaic));   // a mosaic held for a file's model goes on if this is not one
    }
}

bool DataProcessor::isDbReady() const noexcept
{
    return dbIsReady_.load(std::memory_order_relaxed);
}

void DataProcessor::onDbSaveTiles(const QHash<TileKey, SurfaceTile> &tiles, quint64 saveId)
{
    if (shuttingDown_.load() || suppressResults_.load()) {
        return;
    }
    // FAKE_COORDS + bounded window: don't persist the rolling-window state. Without this the
    // DB would gradually accept (and later replay) snapshots that contradict the gate logic.
    // Hot cache needs an ACK to release the blocked nodes, so we still drive the save-cycle —
    // just without actually writing anywhere.
    if (activeZeroing_ && mosaicFakeCoordsLastN_ > 0) {
        QVector<TileKey> ackKeys;
        ackKeys.reserve(tiles.size());
        for (auto it = tiles.cbegin(); it != tiles.cend(); ++it) {
            ackKeys.append(it.key());
        }
        hotCache_.onSendSavedTiles(ackKeys, saveId);
        return;
    }

    openDB();

    emit dbSaveTiles(mosaicDbGen_, tiles, true, defaultTileSidePixelSize, defaultTileHeightMatrixRatio, saveId);
}

void DataProcessor::notifyPrefetchProgress()
{
    QMutexLocker lk(&prefetchMu_);
    prefetchTick_.fetch_add(1, std::memory_order_relaxed);
    prefetchCv_.wakeAll();
}

void DataProcessor::clearDbNotFoundCache()
{
    dbNotFoundIndxs_.clear();
    dbNotFoundOrder_.clear();
    dbNotFoundPos_.clear();
}

void DataProcessor::enqueueSurfaceMissingForZoom(int zoom)
{
    if (!updateSurface_) {
        return;
    }

    int actualZoom = zoom;
    if (actualZoom <= 0) {
        actualZoom = zoomFromMpp(tileResolution_);
    }
    if (actualZoom <= 0) {
        return;
    }

    auto& processed = surfaceTaskEpochIndxsByZoom_[actualZoom];
    const auto& manualPending = surfaceManualEpochIndxsByZoom_[actualZoom];

    for (int idx : std::as_const(vertIndxsFromBottomTrack_)) {
        if (processed.contains(idx) || manualPending.contains(idx)) {
            continue;
        }
        pendingSurfaceIndxs_.insert(qMakePair('0', idx));
    }
}

QVector<QPair<int, QSet<TileKey>>> DataProcessor::collectEpochsForTiles(int zoom, const QSet<TileKey> &tiles) const
{
    int cZoom = zoom - 1;

    QVector<QPair<int, QSet<TileKey>>> result;
    if (tiles.isEmpty()) {
        return result;
    }

    if (cZoom < 0 || cZoom >= tileEpochIndxsByZoom_.size()) {
        return result;
    }

    const auto& indexForZoom = tileEpochIndxsByZoom_.at(cZoom);
    if (indexForZoom.isEmpty()) {
        return result;
    }

    QMap<int, QSet<TileKey>> tilesByEpoch;

    for (const TileKey& tk : tiles) {
        auto it = indexForZoom.constFind(tk);
        if (it == indexForZoom.cend()) {
            continue;
        }

        const QVector<int>& epochList = it.value();
        for (int epochIndx : epochList) {
                tilesByEpoch[epochIndx].insert(tk);
        }
    }

    result.reserve(tilesByEpoch.size());
    for (auto it = tilesByEpoch.cbegin(); it != tilesByEpoch.cend(); ++it) {
        result.push_back(QPair<int, QSet<TileKey>>(it.key(), it.value()));
    }

    return result;
}

QSet<int> DataProcessor::collectEpochsForTilesSet(int zoom, const QSet<TileKey> &tiles) const
{
    int cZoom = zoom - 1;

    QSet<int> result;
    if (tiles.isEmpty()) {
        return result;
    }

    if (cZoom < 0 || cZoom >= tileEpochIndxsByZoom_.size()) {
        return result;
    }

    const auto& indexForZoom = tileEpochIndxsByZoom_.at(cZoom);
    if (indexForZoom.isEmpty()) {
        return result;
    }

    for (const TileKey& tk : tiles) {
        auto it = indexForZoom.constFind(tk);
        if (it == indexForZoom.cend()) {
            continue;
        }
        const QVector<int>& epochList = it.value();
        for (int epochIndx : epochList) {
            result.insert(epochIndx);
        }
    }

    return result;
}

QSet<int> DataProcessor::collectSurfaceEpochsForTilesSet(int zoom, const QSet<TileKey>& tiles) const
{
    QSet<int> visibleEpochs = collectEpochsForTilesSet(zoom, tiles);
    if (visibleEpochs.isEmpty() || epochToBottomTrackVertIndx_.isEmpty()) {
        visibleEpochs.clear();
        return visibleEpochs;
    }

    QSet<int> result;
    result.reserve(visibleEpochs.size() * 2);
    for (int epochIdx : std::as_const(visibleEpochs)) {
        // tileEpochIndxsByZoom_ is segment-based (epoch i stores coverage for segment i -> i+1),
        // while surface processing is vertex-based. Include both segment endpoints.
        const auto vertIt0 = epochToBottomTrackVertIndx_.constFind(epochIdx);
        if (vertIt0 != epochToBottomTrackVertIndx_.cend()) {
            const int vertIdx = vertIt0.value();
            if (vertIndxsFromBottomTrack_.contains(vertIdx)) {
                result.insert(vertIdx);
            }
        }
        const auto vertIt1 = epochToBottomTrackVertIndx_.constFind(epochIdx + 1);
        if (vertIt1 != epochToBottomTrackVertIndx_.cend()) {
            const int vertIdx = vertIt1.value();
            if (vertIndxsFromBottomTrack_.contains(vertIdx)) {
                result.insert(vertIdx);
            }
        }
    }
    return result;
}

QSet<int> DataProcessor::collectVisibleSurfaceEpochsSet(int zoom) const
{
    int actualZoom = zoom;
    if (actualZoom <= 0) {
        actualZoom = zoomFromMpp(tileResolution_);
    }
    return collectSurfaceEpochsForTilesSet(actualZoom, lastVisTileKeys_);
}

void DataProcessor::updateDataProcType()
{
    if (datasetState_ == 0 || datasetState_ == 1) {
        defProcType_ = true;
    }
    if (datasetState_ == 2) {
        defProcType_ = false;
    }

    pendingWorkTimer_.setInterval(defProcType_ ? 333 : 10);
}

void DataProcessor::onDbTilesLoadedForZoom(int zoom, const QList<DbTile>& dbTiles)
{
    return; // TODO: delete this

    if (shuttingDown_.load() || suppressResults_.load()) {
        dbReaderInWork_ = false;
        dbInWorkKeys_.clear();
        notifyPrefetchProgress();
        return;
    }

    qDebug() << "DataProcessor::onDbTilesLoadedForZoom size" << dbTiles.size();
    dbReaderInWork_ = false;

    if (zoom != lastDataZoomIndx_) {
        return;
    }

    if (!dbTiles.isEmpty()) {
        // можно реализовать массовую подгрузку по zoom, но сейчас используем по-ключевую
        return;
    }

    //  иначе расчёт по всем эпохам
    emit isobathsProcessingCleared();
    emit surfaceProcessingCleared();
    emit mosaicProcessingCleared();

    for (auto it = epIndxsFromBottomTrack_.cbegin(); it != epIndxsFromBottomTrack_.cend(); ++it) {
        pendingMosaicIndxs_.insert(*it);
    }
    enqueueSurfaceMissingForZoom(lastDataZoomIndx_);
    pendingIsobathsWork_ = true;

    QMetaObject::invokeMethod(worker_, "setMosaicTileResolution", Qt::QueuedConnection, Q_ARG(float, tileResolution_));
    scheduleLatest(WorkSet(WF_All), /*replace*/true);
}

void DataProcessor::onDbAnyTileForZoom(int zoom, bool exists)
{
    Q_UNUSED(zoom)

    if (shuttingDown_.load() || suppressResults_.load()) {
        return;
    }

    //qDebug() << "DataProcessor::onDbAnyTileForZoom, zoom" << zoom << "exist" << exists;

    if (exists) {
        pumpVisible();
    }
    else {
        //tryCalcTiles();
    }
}

void DataProcessor::postTraceLines(const QVector3D &leftBeg, const QVector3D &leftEnd, const QVector3D &rightBeg, const QVector3D &rightEnd, int epochIndex)
{
    emit sendTraceLines(leftBeg, leftEnd, rightBeg, rightEnd, epochIndex);
}
