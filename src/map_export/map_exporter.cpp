#include "map_exporter.h"

#include <algorithm>
#include <numeric>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTimer>
#include <QMutex>
#include <QSettings>
#include <QStandardPaths>
#include <QTimeZone>
#include "app_profile.h"
#include "bottom_track.h"
#include "build_version.h"
#include "data_processor.h"
#include "dataset.h"
#include "map_export_config.h"
#include "map_export_geo.h"
#include "map_export_meta.h"
#include "map_export_writer.h"
#include "mosaic_processor.h"
#include "scene3d_view.h"
#include "surface_mesh.h"

namespace {

constexpr int kMinZoom = 2;   // the levels the view shows (pickZoomByDistance)
constexpr int kMaxZoom = 6;
#if defined(Q_OS_ANDROID)
constexpr int kMaxTiles = 2000;           // painted tiles kept for the export, ~66 KB each
constexpr qint64 kMosaicPixels = 150LL * 1000 * 1000;
#else
constexpr int kMaxTiles = 8000;
constexpr qint64 kMosaicPixels = 600LL * 1000 * 1000;
#endif
constexpr qint64 kDepthPixels = 4096LL * 4096;
constexpr double kDepthCellM = 1.0;       // SurfaceProcessor keeps one sounding per square metre

QString settingsApp()
{
    return koggerAppName();
}

// What MosaicProcessor paints, kept for the export: every tile a chunk changed, given back when the
// mesh asks for one it let go (as the hot cache does for the view).
class ExportSink : public MosaicTileSink {
public:
    explicit ExportSink(const std::atomic_bool* cancel) : cancel_(cancel) {}
    void putTiles(const TileMap& tiles) override
    {
        QMutexLocker lock(&mu_);
        for (auto it = tiles.cbegin(); it != tiles.cend(); ++it) {
            tiles_.insert(it.key(), it.value());
        }
    }
    TileMap takeBack(const QSet<TileKey>& keys) override
    {
        QMutexLocker lock(&mu_);
        TileMap out;
        for (const TileKey& k : keys) {
            const auto it = tiles_.constFind(k);
            if (it != tiles_.cend()) {
                out.insert(k, it.value());
            }
        }
        return out;
    }
    bool cancelled() const override { return cancel_->load(); }
    int count() const
    {
        QMutexLocker lock(&mu_);
        return int(tiles_.size());
    }
    const TileMap& tiles() const { return tiles_; }   // after the painting only
private:
    mutable QMutex mu_;
    TileMap tiles_;
    const std::atomic_bool* cancel_;
};

// KOGGER_MAP_EXPORT_DUMP=<file>: the painted tiles in SurfaceView's KOGGER_MOSAIC_DUMP layout ("KMD1"),
// for tools/perf/mosaic_dump_compare.ps1 against what the view drew (docs/map-export.md).
void dumpTiles(const QString& path, const TileMap& tiles)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return;
    }
    auto put = [&f](const void* p, qint64 n) { f.write(static_cast<const char*>(p), n); };
    const qint32 count = qint32(tiles.size());
    put("KMD1", 4);
    put(&count, 4);
    for (auto it = tiles.cbegin(); it != tiles.cend(); ++it) {
        const SurfaceTile& t = it.value();
        const qint32 head[4] = { qint32(it.key().x), qint32(it.key().y), qint32(it.key().zoom), qint32(t.getHeadIndx()) };
        put(head, sizeof head);
        const auto& img = t.getMosaicImageDataCRef();
        const qint32 nImg = qint32(img.size());
        put(&nImg, 4);
        put(img.data(), nImg);
        const auto& verts = t.getHeightVerticesCRef();
        const qint32 nH = qint32(verts.size());
        put(&nH, 4);
        for (const QVector3D& v : verts) {
            const float z = v.z();
            put(&z, 4);
        }
        const auto& marks = t.getHeightMarkVerticesCRef();
        const qint32 nM = qint32(marks.size());
        put(&nM, 4);
        for (const HeightType& m : marks) {
            const quint8 b = quint8(m);
            put(&b, 1);
        }
    }
}

QString sourceName(int s)
{
    switch (s) {
    case 0: return QStringLiteral("Amplitude");
    case 2: return QStringLiteral("Tgc");
    default: return QStringLiteral("SideScan");
    }
}

// Everything the export needs from the GUI thread, taken when it starts.
struct Snapshot {
    LLARef ref;
    int epochs = 0;
    QVector<QVector3D> bottomTrack;
    QVector<QPointF> track;
    qint64 timeFrom = 0, timeTo = 0;   // seconds since the epoch, UTC; 0 when unknown
};

} // namespace

MapExporter::MapExporter(QObject* parent) :
    QObject(parent)
{
    QSettings s(QStringLiteral("KOGGER"), settingsApp());
    s.beginGroup(QStringLiteral("mapExport"));
    title_ = s.value(QStringLiteral("title")).toString();
    authorName_ = s.value(QStringLiteral("authorName")).toString();
    authorContact_ = s.value(QStringLiteral("authorContact")).toString();
    licenseId_ = mapexport::licenseById(s.value(QStringLiteral("licenseId")).toString()).id;
    detailZoom_ = std::clamp(s.value(QStringLiteral("detailZoom"), 5).toInt(), kMinZoom, kMaxZoom);
    draftM_ = s.value(QStringLiteral("draftM"), 0.0).toDouble();
    fillGapsM_ = std::clamp(s.value(QStringLiteral("fillGapsM"), 50.0).toDouble(), 5.0, 500.0);
    isobathStepM_ = std::max(0.0, s.value(QStringLiteral("isobathStepM"), 0.0).toDouble());
    writeKmap_ = s.value(QStringLiteral("writeKmap"), true).toBool();
    writeKmz_ = s.value(QStringLiteral("writeKmz"), false).toBool();
    writeGeoTiff_ = s.value(QStringLiteral("writeGeoTiff"), false).toBool();
    includeMosaic_ = s.value(QStringLiteral("includeMosaic"), true).toBool();
    includeDepth_ = s.value(QStringLiteral("includeDepth"), true).toBool();
    hideTrackAndTime_ = s.value(QStringLiteral("hideTrackAndTime"), true).toBool();
    s.endGroup();
}

MapExporter::~MapExporter()
{
    cancel_ = true;
    if (thread_) {
        thread_->wait();
        delete thread_;
    }
}

void MapExporter::setSources(Dataset* dataset, DataProcessor* dataProcessor, GraphicsScene3dView* scene)
{
    dataset_ = dataset;
    dataProcessor_ = dataProcessor;
    scene_ = scene;

    // A run without the GUI (tools, docs/map-export.md): KOGGER_MAP_EXPORT=<folder> exports into it
    // KOGGER_MAP_EXPORT_DELAY_MS after start (default 60 s, for the file to open and the bottom track
    // to settle), at KOGGER_MAP_EXPORT_ZOOM if set, and with KOGGER_MAP_EXPORT_QUIT=1 quits after.
    static bool armed = false;
    const QString testDir = qEnvironmentVariable("KOGGER_MAP_EXPORT");
    if (testDir.isEmpty() || armed) {
        return;
    }
    armed = true;
    const int delay = qEnvironmentVariableIntValue("KOGGER_MAP_EXPORT_DELAY_MS");
    QTimer::singleShot(delay > 0 ? delay : 60000, this, [this, testDir]() {
        if (const int z = qEnvironmentVariableIntValue("KOGGER_MAP_EXPORT_ZOOM"); z > 0) {
            detailZoom_ = std::clamp(z, kMinZoom, kMaxZoom);   // this run only, not saved
        }
        if (qEnvironmentVariableIntValue("KOGGER_MAP_EXPORT_QUIT") == 1) {
            connect(this, &MapExporter::resultChanged, this, [this]() {
                if (!busy_ && !resultText_.isEmpty()) {
                    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
                }
            });
        }
        start(testDir);
    });
}

void MapExporter::save() const
{
    QSettings s(QStringLiteral("KOGGER"), settingsApp());
    s.beginGroup(QStringLiteral("mapExport"));
    s.setValue(QStringLiteral("title"), title_);
    s.setValue(QStringLiteral("authorName"), authorName_);
    s.setValue(QStringLiteral("authorContact"), authorContact_);
    s.setValue(QStringLiteral("licenseId"), licenseId_);
    s.setValue(QStringLiteral("detailZoom"), detailZoom_);
    s.setValue(QStringLiteral("draftM"), draftM_);
    s.setValue(QStringLiteral("fillGapsM"), fillGapsM_);
    s.setValue(QStringLiteral("isobathStepM"), isobathStepM_);
    s.setValue(QStringLiteral("writeKmap"), writeKmap_);
    s.setValue(QStringLiteral("writeKmz"), writeKmz_);
    s.setValue(QStringLiteral("writeGeoTiff"), writeGeoTiff_);
    s.setValue(QStringLiteral("includeMosaic"), includeMosaic_);
    s.setValue(QStringLiteral("includeDepth"), includeDepth_);
    s.setValue(QStringLiteral("hideTrackAndTime"), hideTrackAndTime_);
    s.endGroup();
}

#define MAPEXPORT_SETTER(Name, member, Type)   \
    void MapExporter::Name(Type v)             \
    {                                          \
        if (member == v) {                     \
            return;                            \
        }                                      \
        member = v;                            \
        save();                                \
        emit settingsChanged();                \
    }

MAPEXPORT_SETTER(setTitle, title_, const QString&)
MAPEXPORT_SETTER(setAuthorName, authorName_, const QString&)
MAPEXPORT_SETTER(setAuthorContact, authorContact_, const QString&)
MAPEXPORT_SETTER(setDraftM, draftM_, double)
MAPEXPORT_SETTER(setIsobathStepM, isobathStepM_, double)
MAPEXPORT_SETTER(setWriteKmap, writeKmap_, bool)
MAPEXPORT_SETTER(setWriteKmz, writeKmz_, bool)
MAPEXPORT_SETTER(setWriteGeoTiff, writeGeoTiff_, bool)
MAPEXPORT_SETTER(setIncludeMosaic, includeMosaic_, bool)
MAPEXPORT_SETTER(setIncludeDepth, includeDepth_, bool)
MAPEXPORT_SETTER(setHideTrackAndTime, hideTrackAndTime_, bool)
#undef MAPEXPORT_SETTER

void MapExporter::setLicenseId(const QString& v)
{
    const QString id = mapexport::licenseById(v).id;
    if (id == licenseId_) {
        return;
    }
    licenseId_ = id;
    save();
    emit settingsChanged();
}

void MapExporter::setFillGapsM(double v)
{
    v = std::clamp(v, 5.0, 500.0);
    if (v == fillGapsM_) {
        return;
    }
    fillGapsM_ = v;
    save();
    emit settingsChanged();
}

QVariantList MapExporter::isobathStepOptions() const
{
    QVariantList out;
    out << QVariantMap{ { QStringLiteral("step"), 0.0 }, { QStringLiteral("name"), tr("Auto") } };
    for (double s : { 0.5, 1.0, 2.0, 5.0, 10.0 }) {
        out << QVariantMap{ { QStringLiteral("step"), s }, { QStringLiteral("name"), tr("%1 m").arg(s) } };
    }
    return out;
}

void MapExporter::setDetailZoom(int v)
{
    v = std::clamp(v, kMinZoom, kMaxZoom);
    if (v == detailZoom_) {
        return;
    }
    detailZoom_ = v;
    save();
    emit settingsChanged();
}

QVariantList MapExporter::licenseOptions() const
{
    QVariantList out;
    const QString names[] = {
        tr("All rights reserved"),
        tr("CC BY 4.0: free use with credit to the author"),
        tr("CC0: public domain, no conditions"),
    };
    const auto all = mapexport::licenses();
    for (int i = 0; i < all.size(); ++i) {
        out << QVariantMap{ { QStringLiteral("id"), all[i].id },
                            { QStringLiteral("name"), i < 3 ? names[i] : all[i].name },
                            { QStringLiteral("url"), all[i].url } };
    }
    return out;
}

QVariantList MapExporter::detailOptions() const
{
    QVariantList out;
    for (int z = kMinZoom; z <= kMaxZoom; ++z) {
        const double cm = 100.0 / ZL[z - 1].pxPerMeter;
        out << QVariantMap{ { QStringLiteral("zoom"), z },
                            { QStringLiteral("name"), tr("%1 cm per pixel").arg(cm, 0, 'f', 0) } };
    }
    return out;
}

void MapExporter::setProgress(double p, const QString& stage)
{
    progress_ = p;
    stage_ = stage;
    emit progressChanged();
}

void MapExporter::finish(bool ok, const QString& text, const QStringList& files)
{
    if (thread_) {
        thread_->wait();
        delete thread_;
        thread_ = nullptr;
    }
    busy_ = false;
    resultOk_ = ok;
    resultText_ = text;
    resultFiles_ = files;
    qInfo().noquote() << QStringLiteral("map export: %1").arg(text);
    emit busyChanged();
    emit resultChanged();
}

void MapExporter::cancel()
{
    cancel_ = true;
}

void MapExporter::stopAndWait()
{
    if (!thread_) {
        return;
    }
    cancel_ = true;
    thread_->wait();   // its finish() is queued to this thread and comes after
}

void MapExporter::start(const QString& folder)
{
    if (busy_) {
        return;
    }
    if (!dataset_) {
        finish(false, tr("Not saved: no survey is open."), {});
        return;
    }

    // From the GUI thread, once: the reference, the bottom track, the boat's track, the survey's time.
    Snapshot snap;
    snap.ref = dataset_->getLlaRef();
    if (!snap.ref.isInit) {
        finish(false, tr("Not saved: the survey has no GNSS position."), {});
        return;
    }
    snap.epochs = dataset_->size();
    if (scene_ && scene_->bottomTrack()) {
        snap.bottomTrack = scene_->bottomTrack()->data();
    }
    QPointF last(qQNaN(), qQNaN());
    for (int i = 0; i < snap.epochs; ++i) {
        Epoch* ep = dataset_->fromIndex(i);
        if (!ep) {
            continue;
        }
        // The ping's time, else its GNSS fix's; only a calendar date counts (after 2000): 00014 carries
        // no ping time and a GNSS time near 1970 (time since power-on, not a date), and a wrong date
        // in a shared map is worse than none.
        constexpr qint64 kYear2000 = 946684800;
        qint64 t = qint64(ep->time()->sec);
        if (t <= kYear2000) {
            t = qint64(ep->getPositionGNSSCRef().time.sec);
        }
        if (t > kYear2000) {
            snap.timeFrom = snap.timeFrom ? std::min(snap.timeFrom, t) : t;
            snap.timeTo = std::max(snap.timeTo, t);
        }
        const NED& ned = ep->getSonarPositionCRef().ned;
        if (!hideTrackAndTime_ && ned.isCoordinatesValid()) {
            const QPointF p(ned.n, ned.e);
            if (!(std::hypot(p.x() - last.x(), p.y() - last.y()) < 2.0)) {   // a point every 2 m
                snap.track.append(p);
                last = p;
            }
        }
    }

    mapexport::MapMetadata meta;
    meta.title = title_.trimmed().isEmpty() ? tr("Map") : title_.trimmed();
    meta.authorName = authorName_.trimmed();
    meta.authorContact = authorContact_.trimmed();
    meta.licenseId = licenseId_;
    meta.software = QStringLiteral("KoggerApp %1").arg(koggerVersionFull());
    if (!koggerGitRevision().isEmpty()) {
        meta.software += QStringLiteral(" (%1)").arg(koggerGitRevision());
    }
    meta.hideTrackAndTime = hideTrackAndTime_;
    meta.draftM = draftM_;
    if (snap.timeFrom > 0) {
        const QDateTime a = QDateTime::fromSecsSinceEpoch(snap.timeFrom, QTimeZone::UTC);
        const QDateTime b = QDateTime::fromSecsSinceEpoch(snap.timeTo, QTimeZone::UTC);
        meta.surveyFrom = hideTrackAndTime_ ? a.toString(QStringLiteral("yyyy-MM")) : a.toString(Qt::ISODate);
        meta.surveyTo = hideTrackAndTime_ ? b.toString(QStringLiteral("yyyy-MM")) : b.toString(Qt::ISODate);
    }
    const QString disclaimerSource = QStringLiteral("NOT FOR NAVIGATION. Depths and positions are from an amateur survey and may contain errors. Use at your own risk.");
    const QString disclaimerLocal = tr("NOT FOR NAVIGATION. Depths and positions are from an amateur survey and may contain errors. Use at your own risk.");
    if (disclaimerLocal != disclaimerSource) {
        meta.disclaimerLocal = disclaimerLocal;
    }

    const QString outFolder = folder.trimmed().isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/KoggerApp/exports")
        : folder.trimmed();
    const QString baseName = meta.title + QStringLiteral("_") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));

    const int zoom = detailZoom_;
    const bool wantMosaic = includeMosaic_;
    const bool wantDepth = includeDepth_;
    const bool kmap = writeKmap_;
    const bool kmz = writeKmz_;
    const bool tiff = writeGeoTiff_;
    const double fillGaps = fillGapsM_;
    const double isoStep = isobathStepM_;
    if (!kmap && !kmz && !tiff) {
        finish(false, tr("Not saved: choose at least one file to save."), {});
        return;
    }
    if (!wantMosaic && !wantDepth) {
        finish(false, tr("Not saved: choose the mosaic, the depths or both."), {});
        return;
    }
    Dataset* dataset = dataset_.data();
    DataProcessor* dp = dataProcessor_.data();

    // Stage names in the user's language, from this thread (tr).
    const QString sPaint = tr("Painting the mosaic");
    const QString sDepth = tr("Building the depths");
    const QString sWrite = tr("Writing the files");
    const QString noBottom = tr("No depths: the survey has no bottom track.");
    const QString noSideScan = tr("No mosaic: no side-scan channels are chosen for it.");
    const QString emptyMosaic = tr("No mosaic: nothing was painted.");
    const QString tooManyTiles = tr("the mosaic is too large for this detail; choose a coarser one");
    const QString savedFmt = tr("Saved to %1: %2.");
    const QString notSavedFmt = tr("Not saved: %1");
    const QString cancelledText = tr("Cancelled.");

    cancel_ = false;
    busy_ = true;
    resultText_.clear();
    resultFiles_.clear();
    emit busyChanged();
    emit resultChanged();
    setProgress(0.0, sPaint);

    thread_ = QThread::create([=, this]() {
        QString lastStage;
        double lastP = -1.0;
        auto report = [&](double p, const QString& stage) {
            if (cancel_.load()) {
                return false;
            }
            if (stage != lastStage || p - lastP >= 0.005 || p >= 1.0) {
                lastStage = stage;
                lastP = p;
                QMetaObject::invokeMethod(this, [this, p, stage]() { setProgress(p, stage); }, Qt::QueuedConnection);
            }
            return true;
        };
        auto done = [this](bool ok, const QString& text, const QStringList& files) {
            QMetaObject::invokeMethod(this, [this, ok, text, files]() { finish(ok, text, files); }, Qt::QueuedConnection);
        };

        MapExportConfig cfg;
        if (dp) {
            QMetaObject::invokeMethod(dp, [&cfg, dp]() { cfg = dp->mapExportConfig(); }, Qt::BlockingQueuedConnection);
        }
        QStringList notes;
        QElapsedTimer clock;
        clock.start();
        qInfo().noquote() << QStringLiteral("map export: %1 epochs, %2 bottom-track points, zoom %3, mosaic %4, depths %5")
                             .arg(snap.epochs).arg(snap.bottomTrack.size()).arg(zoom).arg(wantMosaic).arg(wantDepth);

        // The mosaic: painted again over the whole survey with the view's settings, as the background
        // precompute paints a level (MosaicPrecomputer::run), into a sink of our own.
        ExportSink sink(&cancel_);
        QHash<QPair<int, int>, mapexport::MosaicTileRef> refs;
        const float res = 1.0f / ZL[zoom - 1].pxPerMeter;
        if (wantMosaic && !cfg.ch1.isValid() && !cfg.ch2.isValid()) {
            notes << noSideScan;
        }
        else if (wantMosaic) {
            SurfaceMesh mesh(defaultTileSidePixelSize, defaultTileHeightMatrixRatio, defaultTileResolution);
            mesh.setLRUWatermarks(512, 256);
            MosaicProcessor mp(nullptr, nullptr);
            mp.setSurfaceMeshPtr(&mesh);
            mp.setDatasetPtr(dataset);
            mp.setChannels(cfg.ch1, cfg.sub1, cfg.ch2, cfg.sub2);
            mp.setSource(static_cast<MosaicProcessor::Source>(cfg.mosaicSource));
            mp.setLAngleOffset(cfg.lAngleOffset);
            mp.setRAngleOffset(cfg.rAngleOffset);
            mp.setSink(&sink);
            mp.clear();
            mesh.reinit(defaultTileSidePixelSize, defaultTileHeightMatrixRatio, res);
            mp.setTileResolution(res);
            constexpr int kSlice = 1024;
            constexpr qint64 kMaxCells = 128 * 1024;
            for (int at = 0; at < snap.epochs; at += kSlice) {
                if (qint64(mesh.getNumWidthTiles()) * mesh.getNumHeightTiles() > kMaxCells) {
                    mp.clear();
                    mesh.reinit(defaultTileSidePixelSize, defaultTileHeightMatrixRatio, res);
                }
                QVector<int> slice(std::min(kSlice, snap.epochs - at));
                std::iota(slice.begin(), slice.end(), at);
                mp.updateDataWrapper(slice, false);
                if (!report(double(at + slice.size()) / double(std::max(1, snap.epochs)), sPaint)) {
                    done(false, cancelledText, {});
                    return;
                }
                if (sink.count() > kMaxTiles) {
                    done(false, notSavedFmt.arg(tooManyTiles), {});
                    return;
                }
            }
            for (auto it = sink.tiles().cbegin(); it != sink.tiles().cend(); ++it) {
                const auto& img = it.value().getMosaicImageDataCRef();
                if (img.size() != size_t(defaultTileSidePixelSize) * defaultTileSidePixelSize
                    || std::none_of(img.begin(), img.end(), [](uint8_t b) { return b != 0; })) {
                    continue;
                }
                const QVector3D o = it.value().getOrigin();
                refs.insert(qMakePair(it.key().x, it.key().y), mapexport::MosaicTileRef{ o.x(), o.y(), img.data() });
            }
            if (refs.isEmpty()) {
                notes << emptyMosaic;
            }
            qInfo().noquote() << QStringLiteral("map export: mosaic painted, %1 tiles (%2 with data) in %3 s")
                                 .arg(sink.count()).arg(refs.size()).arg(clock.elapsed() / 1000.0, 0, 'f', 1);
            if (const QString dump = qEnvironmentVariable("KOGGER_MAP_EXPORT_DUMP"); !dump.isEmpty()) {
                dumpTiles(dump, sink.tiles());
            }
        }
        const mapexport::MosaicSampler sampler(refs, double(tileSideMetersFromZoom(zoom)), double(ZL[zoom - 1].pxPerMeter),
                                               defaultTileSidePixelSize);

        // The depths.
        mapexport::DepthGrid depth;
        bool haveDepth = false;
        if (wantDepth) {
            QString err;
            haveDepth = mapexport::buildDepthGrid(snap.bottomTrack, snap.ref, kDepthCellM, fillGaps, meta.draftM,
                                                  kDepthPixels, depth, [&](double f) { return report(f, sDepth); }, &err);
            if (cancel_.load()) {
                done(false, cancelledText, {});
                return;
            }
            if (!haveDepth) {
                notes << (snap.bottomTrack.isEmpty() ? noBottom : err);
            }
            qInfo().noquote() << QStringLiteral("map export: depths %1 (%2 soundings, %3 triangles, grid %4 x %5) at %6 s")
                                 .arg(haveDepth ? QStringLiteral("built") : err).arg(depth.soundings).arg(depth.triangles)
                                 .arg(depth.grid.width).arg(depth.grid.height).arg(clock.elapsed() / 1000.0, 0, 'f', 1);
        }

        if (refs.isEmpty() && !haveDepth) {
            done(false, notSavedFmt.arg(notes.join(QLatin1Char(' '))), {});
            return;
        }

        mapexport::ExportInputs in;
        in.ref = snap.ref;
        in.mosaic = refs.isEmpty() ? nullptr : &sampler;
        in.mosaicMetresPerPixel = double(res);
        for (size_t i = 0; i + 3 < cfg.mosaicColorTable.size() && in.mosaicPalette.size() < 256; i += 4) {
            in.mosaicPalette << qRgba(cfg.mosaicColorTable[i], cfg.mosaicColorTable[i + 1], cfg.mosaicColorTable[i + 2], cfg.mosaicColorTable[i + 3]);
        }
        in.depth = haveDepth ? &depth : nullptr;
        in.isobathStepM = isoStep > 0.0 ? isoStep : haveDepth ? mapexport::autoIsobathStep(depth.minDepth, depth.maxDepth) : 1.0;
        in.depthTheme = cfg.surfaceTheme;
        in.trackNe = snap.track;
        in.meta = meta;
        in.meta.mosaicSource = sourceName(cfg.mosaicSource);
        in.meta.mosaicTheme = cfg.mosaicTheme;
        in.meta.mosaicLow = cfg.mosaicLow;
        in.meta.mosaicHigh = cfg.mosaicHigh;
        in.meta.edgeLimitM = fillGaps;
        in.meta.depthTheme = cfg.surfaceTheme;
        in.writeKmap = kmap;
        in.writeKmz = kmz;
        in.writeGeoTiff = tiff;
        in.folder = outFolder;
        in.baseName = baseName;
        in.mosaicPixelLimit = kMosaicPixels;

        mapexport::ExportResult result;
        mapexport::writeMapProducts(in, [&](double f, const QString&) { return report(f, sWrite); }, result);
        if (result.cancelled || cancel_.load()) {
            done(false, cancelledText, {});
            return;
        }
        if (!result.error.isEmpty()) {
            done(false, notSavedFmt.arg(result.error), {});
            return;
        }
        QStringList names;
        for (const QString& f : std::as_const(result.files)) {
            names << QFileInfo(f).fileName();
            qInfo().noquote() << QStringLiteral("map export: wrote %1 (%2 bytes)").arg(f).arg(QFileInfo(f).size());
        }
        qInfo().noquote() << QStringLiteral("map export: done in %1 s").arg(clock.elapsed() / 1000.0, 0, 'f', 1);
        QString text = savedFmt.arg(QDir::toNativeSeparators(outFolder), names.join(QStringLiteral(", ")));
        if (!notes.isEmpty()) {
            text += QLatin1Char(' ') + notes.join(QLatin1Char(' '));
        }
        done(true, text, result.files);
    });
    thread_->setObjectName(QStringLiteral("MapExportThread"));
    thread_->start(QThread::LowPriority);
}
