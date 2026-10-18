#include "mosaic_precomputer.h"

#include "data_processor.h"
#include "data_processor_defs.h"
#include "surface_tile.h"

#include <iterator>

namespace {
constexpr int kPrecomputeMeshHighWM = 512;
constexpr int kPrecomputeMeshLowWM = 256;
constexpr int kPrecomputePaintThreads = 2;   // beside the view's worker and its own pool
} // namespace

MosaicPrecomputer::MosaicPrecomputer(DataProcessor* dp, QObject* parent) :
    QObject(parent),
    dp_(dp),
    mesh_(defaultTileSidePixelSize, defaultTileHeightMatrixRatio, defaultTileResolution),
    mosaic_(dp, nullptr)
{
    mesh_.setLRUWatermarks(kPrecomputeMeshHighWM, kPrecomputeMeshLowWM);
    mosaic_.setSurfaceMeshPtr(&mesh_);
    mosaic_.setPaintThreads(kPrecomputePaintThreads);
    mosaic_.setBackground(&tiles_, &latest_, 0);
}

void MosaicPrecomputer::setDatasetPtr(Dataset* ds)
{
    mosaic_.setDatasetPtr(ds);
    hasDataset_ = ds != nullptr;
}

void MosaicPrecomputer::setChannels(const ChannelId& ch1, uint8_t sub1, const ChannelId& ch2, uint8_t sub2)
{
    mosaic_.setChannels(ch1, sub1, ch2, sub2);
}

void MosaicPrecomputer::setLAngleOffset(float val)
{
    mosaic_.setLAngleOffset(val);
}

void MosaicPrecomputer::setRAngleOffset(float val)
{
    mosaic_.setRAngleOffset(val);
}

void MosaicPrecomputer::setSource(int source)
{
    mosaic_.setSource(static_cast<MosaicProcessor::Source>(source));
}

void MosaicPrecomputer::setBeam(std::shared_ptr<const mosaic_beam::Model> model, const mosaic_beam::PaintRules& rules)
{
    mosaic_.setBeamModel(std::move(model));
    mosaic_.setPaintRules(rules);
}

void MosaicPrecomputer::setPaintThreads(int n)
{
    mosaic_.setPaintThreads(n > 0 ? n : kPrecomputePaintThreads);
}

void MosaicPrecomputer::run(int zoom, const QSet<TileKey>& tiles, const QVector<int>& epochs, quint64 generation)
{
    if (latest_.load() == generation && hasDataset_ && zoom >= 1 && zoom <= int(std::size(ZL)) && !epochs.isEmpty()) {
        const float res = 1.0f / ZL[zoom - 1].pxPerMeter;
        mosaic_.clear();
        mesh_.reinit(defaultTileSidePixelSize, defaultTileHeightMatrixRatio, res);
        mosaic_.setTileResolution(res);
        tiles_ = tiles;
        mosaic_.setBackground(&tiles_, &latest_, generation);
        // In ascending slices, the mesh started afresh once it covers too much: it grows to the box of
        // what it has painted, a cell (~250 B) for every tile in it, and a chunk walks all of them --
        // over a whole fine level, the survey's extent (a 5 km box at level 2: ~10^6 cells). What a
        // chunk painted is in the cache by then (posted with each chunk); a tile touched again comes
        // back from there, as after an eviction, and a slice's first chunk takes the three epochs
        // before it. Not too often: each restart fetches its tiles again (16 K cells made level 2 of
        // a 1.1 GB survey 23 % slower, 2026-10-18).
        constexpr qsizetype kSlice = 1024;
        constexpr qint64 kMaxCells = 128 * 1024;
        int restarts = 0;
        for (qsizetype at = 0; at < epochs.size() && latest_.load() == generation; at += kSlice) {
            if (qint64(mesh_.getNumWidthTiles()) * mesh_.getNumHeightTiles() > kMaxCells) {
                mosaic_.clear();
                mesh_.reinit(defaultTileSidePixelSize, defaultTileHeightMatrixRatio, res);
                ++restarts;
            }
            mosaic_.updateDataWrapper(epochs.mid(at, kSlice), false);
        }
        if (restarts > 0) {
            qInfo().noquote() << QStringLiteral("mosaic precompute: zoom %1, the mesh started afresh %2 times (over %3 cells)")
                                 .arg(zoom).arg(restarts).arg(kMaxCells);
        }
        clear();   // the tiles are the cache's now; the next run starts empty anyway
    }
    QMetaObject::invokeMethod(dp_, "onPrecomputeFinished", Qt::QueuedConnection, Q_ARG(quint64, generation));
}

void MosaicPrecomputer::clear()
{
    mosaic_.clear();
    mesh_.clear();
    tiles_.clear();
}
