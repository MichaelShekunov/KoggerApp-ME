#include "map_export_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QJsonDocument>
#include <QMap>
#include <QPainter>
#include <QRegularExpression>
#include <QTemporaryDir>
#include "isobaths_defs.h"
#include "map_export_formats.h"
#include "map_package.h"

namespace mapexport {

namespace {

constexpr int kChunk = 1024;         // KMZ overlay pieces, pixels
constexpr int kOverviewMax = 2048;   // the mosaic overview's longer side
constexpr float kNoDepth = -9999.0f;

QString num(double v, int prec = 7)
{
    return QString::number(v, 'f', prec);
}

QString xml(const QString& s)
{
    return s.toHtmlEscaped();
}

QString latLonBox(double n, double s, double e, double w)
{
    return QStringLiteral("<LatLonBox><north>%1</north><south>%2</south><east>%3</east><west>%4</west></LatLonBox>")
        .arg(num(n), num(s), num(e), num(w));
}

// Renders the grid's pixels [x0, x0 + w) x [y0, y0 + h) of the mosaic into dst (w * h bytes).
void renderMosaic(const GeoGrid& g, const LLARef& ref, const MosaicSampler& ms, int x0, int y0, int w, int h,
                  uint8_t* dst, int dstStride)
{
    GeoGrid part = g;
    part.west = g.lonAt(x0);
    part.north = g.latAt(y0);
    part.width = w;
    part.height = h;
    Projector proj(ref, part);
    for (int r = 0; r < h; ++r) {
        proj.setRow(r);
        uint8_t* row = dst + size_t(r) * size_t(dstStride);
        for (int c = 0; c < w; ++c) {
            double n, e;
            proj.toNed(c, n, e);
            row[c] = ms.sample(n, e);
        }
    }
}

bool anyNonZero(const std::vector<uint8_t>& v)
{
    return std::any_of(v.begin(), v.end(), [](uint8_t b) { return b != 0; });
}

QString sanitizeFileName(QString s)
{
    static const QString bad = QStringLiteral("<>:\"/\\|?*");
    for (QChar& ch : s) {
        if (bad.contains(ch) || ch.unicode() < 32) {
            ch = QLatin1Char('_');
        }
    }
    s = s.trimmed();
    return s.isEmpty() ? QStringLiteral("map") : s.left(80);
}

// The picture in the list of maps: the depth colours, or the mosaic when there are no depths, the
// longer side 512 px at most.
QImage previewImage(const ExportInputs& in, const GeoGrid& mg, const QVector<QRgb>& mPal, const QVector<QRgb>& dPal, int bands)
{
    constexpr int kSide = 512;
    if (in.depth) {
        const DepthGrid& dg = *in.depth;
        const double f = std::max(1.0, double(std::max(dg.grid.width, dg.grid.height)) / kSide);
        const int w = std::max(1, int(dg.grid.width / f)), h = std::max(1, int(dg.grid.height / f));
        QImage img(w, h, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        for (int y = 0; y < h; ++y) {
            const int r = std::min(dg.grid.height - 1, int((y + 0.5) * f));
            for (int x = 0; x < w; ++x) {
                const int c = std::min(dg.grid.width - 1, int((x + 0.5) * f));
                const float d = dg.depth[size_t(r) * size_t(dg.grid.width) + size_t(c)];
                if (std::isfinite(d)) {
                    img.setPixel(x, y, dPal[1 + depthBand(d, dg.maxDepth, in.isobathStepM, bands)]);
                }
            }
        }
        return img;
    }
    GeoGrid og = mg;
    const int f = std::max(1, int(std::ceil(double(std::max(mg.width, mg.height)) / kSide)));
    og.dLat *= f;
    og.dLon *= f;
    og.width = std::max(1, mg.width / f);
    og.height = std::max(1, mg.height / f);
    std::vector<uint8_t> px(size_t(og.width) * size_t(og.height));
    renderMosaic(og, in.ref, *in.mosaic, 0, 0, og.width, og.height, px.data(), og.width);
    QImage img(og.width, og.height, QImage::Format_Indexed8);
    img.setColorTable(mPal);
    for (int y = 0; y < og.height; ++y) {
        std::memcpy(img.scanLine(y), px.data() + size_t(y) * size_t(og.width), size_t(og.width));
    }
    return img;
}

// Legend: the depth bands (when there are depths) and, always, the notice and the licence line.
QImage legendImage(const ExportInputs& in, const QVector<QRgb>& pal, int bands)
{
    const int w = 300;
    const bool depth = in.depth && bands > 0;
    const int barH = depth ? std::min(360, std::max(120, bands * 12)) : 0;
    const int h = (depth ? 34 + barH + 16 : 0) + 74;
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(qRgba(255, 255, 255, 230));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    QFont f = p.font();
    f.setPixelSize(13);
    p.setFont(f);
    p.setPen(Qt::black);
    int y = 8;
    if (depth) {
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(10, y, w - 20, 18), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Depth, m"));
        f.setBold(false);
        p.setFont(f);
        y += 26;
        const double minD = in.depth->minDepth, maxD = in.depth->maxDepth;
        // Shallow at the top, deep at the bottom.
        for (int i = 0; i < barH; ++i) {
            const double d = minD + (maxD - minD) * (double(i) + 0.5) / double(barH);
            const int b = depthBand(d, maxD, in.isobathStepM, bands);
            p.setPen(QColor::fromRgb(pal[b + 1]));
            p.drawLine(10, y + i, 40, y + i);
        }
        p.setPen(Qt::black);
        p.drawRect(10, y, 30, barH);
        const int labels = std::min(10, bands + 1);
        for (int k = 0; k <= labels; ++k) {
            const double d = minD + (maxD - minD) * double(k) / double(labels);
            const int ly = y + int(std::lround(double(barH) * k / labels));
            p.drawLine(40, ly, 46, ly);
            p.drawText(QRect(50, ly - 9, 120, 18), Qt::AlignLeft | Qt::AlignVCenter, QString::number(d, 'f', d < 10.0 ? 1 : 0));
        }
        y += barH + 16;
    }
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(170, 0, 0));
    p.drawText(QRect(10, y, w - 20, 18), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("NOT FOR NAVIGATION"));
    y += 20;
    if (!in.meta.disclaimerLocal.isEmpty()) {
        // The first sentence ("НЕ ДЛЯ НАВИГАЦИИ"); Chinese ends it with an ideographic full stop.
        const QString first = in.meta.disclaimerLocal.split(QRegularExpression(QStringLiteral("[.。]"))).value(0).trimmed();
        p.drawText(QRect(10, y, w - 20, 18), Qt::AlignLeft | Qt::AlignVCenter, first.toUpper());
    }
    y += 20;
    f.setBold(false);
    f.setPixelSize(11);
    p.setFont(f);
    p.setPen(Qt::black);
    p.drawText(QRect(10, y, w - 20, 30), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, copyrightLine(in.meta));
    p.end();
    return img;
}

// The .kmap's passport: as the export's, but with no track and the survey's time to the month
// ("2026-10-07T08:00:00Z" -> "2026-10").
MapMetadata kmapMetadata(const MapMetadata& m)
{
    MapMetadata k = m;
    k.hideTrackAndTime = true;
    k.trackIncluded = false;
    k.surveyFrom = k.surveyFrom.left(7);
    k.surveyTo = k.surveyTo.left(7);
    return k;
}

} // namespace

double autoIsobathStep(double minDepth, double maxDepth)
{
    const double range = std::max(0.0, maxDepth - minDepth);
    for (double s : { 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0 }) {
        if (range / s <= 25.0) {
            return s;
        }
    }
    return 100.0;
}

int depthBands(double minDepth, double maxDepth, double step)
{
    if (!(step > 0.0) || !(maxDepth >= minDepth)) {
        return 1;
    }
    // SurfaceProcessor::rebuildColorIntervals: ((maxZ - minZ) / step) + 1, at most what 8 bits hold.
    return std::clamp(int((maxDepth - minDepth) / step + 1.0), 1, 254);
}

int depthBand(double depth, double maxDepth, double step, int bands)
{
    // isobaths_colored.fsh: band = floor((z - minZ) / step), z = -depth, minZ = -maxDepth.
    if (!(step > 0.0)) {
        return 0;
    }
    return std::clamp(int(std::floor((maxDepth - depth) / step)), 0, bands - 1);
}

QVector<QRgb> depthPalette(int theme, int bands)
{
    QVector<QRgb> pal(256, qRgba(0, 0, 0, 0));
    const auto& base = IsobathUtils::colorPalette(theme);
    const int n = base.size();
    for (int i = 0; i < bands && i < 255; ++i) {
        QVector3D c = n > 0 ? base.first() : QVector3D(1.0f, 1.0f, 1.0f);
        if (bands > 1 && n > 1) {
            const float t = float(i) / float(bands - 1);
            const float ft = t * float(n - 1);
            const int i0 = int(ft);
            const int i1 = std::min(i0 + 1, n - 1);
            const float l = ft - float(i0);
            c = (1.0f - l) * base[i0] + l * base[i1];
        }
        pal[i + 1] = qRgba(int(std::clamp(c.x() * 255.0f, 0.0f, 255.0f)), int(std::clamp(c.y() * 255.0f, 0.0f, 255.0f)),
                           int(std::clamp(c.z() * 255.0f, 0.0f, 255.0f)), 255);
    }
    return pal;
}

bool writeMapProducts(ExportInputs in, const std::function<bool(double, const QString&)>& progress, ExportResult& out)
{
    out = ExportResult();
    auto stage = [&](const QString& name) {
        return [&progress, name](double f) { return !progress || progress(f, name); };
    };
    auto cancelled = [&]() {
        out.cancelled = true;
        for (const QString& f : std::as_const(out.files)) {
            QFile::remove(f);
        }
        out.files.clear();
        return false;
    };
    auto failed = [&](const QString& why) {
        out.error = why;
        for (const QString& f : std::as_const(out.files)) {
            QFile::remove(f);
        }
        out.files.clear();
        return false;
    };

    if (!in.ref.isInit) {
        return failed(QStringLiteral("no reference position: the survey has no GNSS fixes"));
    }
    if (!in.mosaic && !in.depth) {
        return failed(QStringLiteral("nothing to export"));
    }
    QDir().mkpath(in.folder);
    const QString base = QDir(in.folder).filePath(sanitizeFileName(in.baseName));

    // The mosaic's grid.
    GeoGrid mg;
    if (in.mosaic) {
        double minN, maxN, minE, maxE;
        if (!in.mosaic->bounds(minN, maxN, minE, maxE)) {
            in.mosaic = nullptr;
        }
        else {
            mg = gridForNedBox(in.ref, minN, maxN, minE, maxE, in.mosaicMetresPerPixel, in.mosaicPixelLimit);
            if (!mg.isValid()) {
                return failed(QStringLiteral("the mosaic is too large for this detail; choose a coarser one"));
            }
        }
    }
    if (!in.mosaic && !in.depth) {
        return failed(QStringLiteral("the mosaic is empty and there are no depths"));
    }

    // Bounds and the metadata every file carries.
    {
        double w = 1e9, s = 1e9, e = -1e9, n = -1e9;
        auto take = [&](const GeoGrid& g) {
            w = std::min(w, g.west); e = std::max(e, g.east());
            s = std::min(s, g.south()); n = std::max(n, g.north);
        };
        if (in.mosaic) {
            take(mg);
        }
        if (in.depth) {
            take(in.depth->grid);
        }
        in.meta.west = w; in.meta.south = s; in.meta.east = e; in.meta.north = n;
    }
    in.meta.created = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    in.meta.hasMosaic = in.mosaic != nullptr;
    in.meta.mosaicResolutionM = in.mosaicMetresPerPixel;
    in.meta.hasDepth = in.depth != nullptr;
    in.meta.trackIncluded = !in.trackNe.isEmpty();
    if (in.depth) {
        in.meta.depthCellM = in.depth->grid.dLat * double(CONSTANTS_RADIUS_OF_EARTH) * double(M_DEG_TO_RAD);
        in.meta.soundings = in.depth->soundings;
        in.meta.minDepthM = in.depth->minDepth;
        in.meta.maxDepthM = in.depth->maxDepth;
        in.meta.isobathStepM = in.isobathStepM;
    }
    in.meta.files.clear();
    if (in.writeKmap) {
        in.meta.files << QFileInfo(base + QLatin1Char('.') + kMapSuffix).fileName();
    }
    if (in.writeKmz) {
        in.meta.files << QFileInfo(base + QStringLiteral(".kmz")).fileName();
    }
    if (in.writeGeoTiff && in.mosaic) {
        in.meta.files << QFileInfo(base + QStringLiteral("_mosaic.tif")).fileName();
    }
    if (in.writeGeoTiff && in.depth) {
        in.meta.files << QFileInfo(base + QStringLiteral("_depth.tif")).fileName();
    }
    const QJsonObject metaObj = toJson(in.meta);
    const QByteArray metaJson = QJsonDocument(metaObj).toJson(QJsonDocument::Indented);

    QVector<QRgb> mPal = in.mosaicPalette;
    mPal.resize(256);
    mPal[0] = qRgba(0, 0, 0, 0);
    const int bands = in.depth ? depthBands(in.depth->minDepth, in.depth->maxDepth, in.isobathStepM) : 0;
    const QVector<QRgb> dPal = depthPalette(in.depthTheme, bands);

    // The KOGGER map is the file people pass around: never the boat's track and the survey's time to the
    // month only, whatever the export's privacy switch says (owner, 2026-10-18: a shared map carries the
    // isobaths and / or the mosaic and nothing of the track). The switch is for KMZ and GeoTIFF.
    const MapMetadata kmapMeta = kmapMetadata(in.meta);
    const bool kmapOwnTiffs = in.writeKmap && in.writeGeoTiff   // the GIS files keep the times then
                              && (kmapMeta.surveyFrom != in.meta.surveyFrom || kmapMeta.surveyTo != in.meta.surveyTo);

    // GeoTIFFs: for GIS when asked for, and inside the .kmap -- the same files when their metadata agree.
    QTemporaryDir scratch;
    const bool tiffsForKmapOnly = in.writeKmap && !in.writeGeoTiff;
    if ((tiffsForKmapOnly || kmapOwnTiffs) && !scratch.isValid()) {
        return failed(QStringLiteral("no temporary folder: %1").arg(scratch.errorString()));
    }
    auto writeTiffs = [&](const MapMetadata& meta, const QString& mosaicTif, const QString& depthTif, bool listed) -> bool {
        if (in.mosaic) {
            GeoTiffSpec spec;
            spec.kind = GeoTiffSpec::Kind::Palette8;
            spec.grid = mg;
            spec.palette = mPal;
            spec.noData = QStringLiteral("0");
            spec.description = meta.title + QStringLiteral(" -- side-scan mosaic. ") + disclaimerEn(meta.draftM);
            spec.copyright = copyrightLine(meta);
            spec.software = meta.software;
            spec.metadata = toGdalItems(meta);
            spec.bandDescription = QStringLiteral("Side-scan echo intensity index (0 = no data)");
            QString err;
            const MosaicSampler& ms = *in.mosaic;
            const LLARef ref = in.ref;
            if (listed) {
                out.files << mosaicTif;
            }
            if (!writeGeoTiff(mosaicTif, spec, [&](int x0, int y0, int w, int h, void* dst) {
                    renderMosaic(mg, ref, ms, x0, y0, w, h, static_cast<uint8_t*>(dst), 256);
                }, stage(QStringLiteral("mosaic GeoTIFF")), &err)) {
                return err.isEmpty() ? cancelled() : failed(err);
            }
        }
        if (in.depth) {
            const DepthGrid& dg = *in.depth;
            GeoTiffSpec spec;
            spec.kind = GeoTiffSpec::Kind::Float32;
            spec.grid = dg.grid;
            spec.noData = QStringLiteral("-9999");
            spec.description = meta.title + QStringLiteral(" -- depth, m, positive down. ") + disclaimerEn(meta.draftM);
            spec.copyright = copyrightLine(meta);
            spec.software = meta.software;
            spec.metadata = toGdalItems(meta);
            spec.bandDescription = QStringLiteral("Depth below the %1, m").arg(verticalReference(meta).toLower());
            spec.bandUnit = QStringLiteral("m");
            QString err;
            if (listed) {
                out.files << depthTif;
            }
            if (!writeGeoTiff(depthTif, spec, [&](int x0, int y0, int w, int h, void* dst) {
                    auto* d = static_cast<float*>(dst);
                    for (int r = 0; r < h; ++r) {
                        for (int c = 0; c < w; ++c) {
                            const float v = dg.depth[size_t(y0 + r) * size_t(dg.grid.width) + size_t(x0 + c)];
                            d[r * 256 + c] = std::isfinite(v) ? v : kNoDepth;
                        }
                    }
                }, stage(QStringLiteral("depth GeoTIFF")), &err)) {
                return err.isEmpty() ? cancelled() : failed(err);
            }
        }
        return true;
    };
    QString kmapMosaicTif = base + QStringLiteral("_mosaic.tif");
    QString kmapDepthTif = base + QStringLiteral("_depth.tif");
    if (in.writeGeoTiff && !writeTiffs(in.meta, kmapMosaicTif, kmapDepthTif, true)) {
        return false;
    }
    if (tiffsForKmapOnly || kmapOwnTiffs) {
        kmapMosaicTif = scratch.path() + QStringLiteral("/m_mosaic.tif");
        kmapDepthTif = scratch.path() + QStringLiteral("/d_depth.tif");
        if (!writeTiffs(kmapMeta, kmapMosaicTif, kmapDepthTif, false)) {
            return false;
        }
    }

    // The KOGGER map (.kmap): what another KoggerApp opens under its boat (map_package.h).
    if (in.writeKmap) {
        const QString path = base + QLatin1Char('.') + kMapSuffix;
        QJsonObject pkg = toJson(kmapMeta);
        pkg[QStringLiteral("format")] = kMapFormat;
        QJsonObject survey = pkg.value(QStringLiteral("survey")).toObject();
        survey.remove(QStringLiteral("trackIncluded"));       // always none: not worth a word
        survey.remove(QStringLiteral("trackAndTimeHidden"));
        if (survey.isEmpty()) {
            pkg.remove(QStringLiteral("survey"));
        }
        else {
            pkg[QStringLiteral("survey")] = survey;
        }
        QByteArray preview;
        {
            QBuffer buf(&preview);
            buf.open(QIODevice::WriteOnly);
            previewImage(in, mg, mPal, dPal, bands).save(&buf, "PNG");
        }
        ZipWriter zip;
        QString err;
        out.files << path;
        if (!zip.open(path, &err)
            || !zip.add(QStringLiteral("map.json"), QJsonDocument(pkg).toJson(QJsonDocument::Indented), false, &err)
            || !zip.add(QStringLiteral("README.txt"), toReadme(kmapMeta).toUtf8(), false, &err)
            || !zip.add(QStringLiteral("preview.png"), preview, false, &err)
            || (in.depth && !zip.addFile(QStringLiteral("depth.tif"), kmapDepthTif, &err))
            || (in.mosaic && !zip.addFile(QStringLiteral("mosaic.tif"), kmapMosaicTif, &err))
            || !zip.close(&err)) {
            return failed(err);
        }
    }

    if (!in.writeKmz) {
        return true;
    }

    // KMZ. doc.kml goes first (a viewer takes the archive's first .kml for the document), so the
    // pieces are planned first -- a mosaic piece only where it has data -- and drawn again when they
    // are written, one at a time: no picture is held longer than its own write.
    const QString kmzPath = base + QStringLiteral(".kmz");
    auto kmzStage = stage(QStringLiteral("KMZ"));
    struct Piece {
        enum Kind { MosaicOverview, MosaicChunk, DepthChunk } kind;
        QString name;
        GeoGrid grid;   // the piece's own grid (a part of the product's, or the coarse overview)
    };
    QVector<Piece> pieces;
    auto partOf = [](const GeoGrid& g, int x0, int y0, int w, int h) {
        GeoGrid p = g;
        p.west = g.lonAt(x0);
        p.north = g.latAt(y0);
        p.width = w;
        p.height = h;
        return p;
    };
    auto renderPiece = [&](const Piece& pc, std::vector<uint8_t>& px) {
        const GeoGrid& g = pc.grid;
        px.assign(size_t(g.width) * size_t(g.height), 0);
        if (pc.kind != Piece::DepthChunk) {
            renderMosaic(g, in.ref, *in.mosaic, 0, 0, g.width, g.height, px.data(), g.width);
            return;
        }
        const DepthGrid& dg = *in.depth;
        const int x0 = int(std::lround((g.west - dg.grid.west) / dg.grid.dLon));
        const int y0 = int(std::lround((dg.grid.north - g.north) / dg.grid.dLat));
        for (int r = 0; r < g.height; ++r) {
            for (int c = 0; c < g.width; ++c) {
                const float d = dg.depth[size_t(y0 + r) * size_t(dg.grid.width) + size_t(x0 + c)];
                if (std::isfinite(d)) {
                    px[size_t(r) * size_t(g.width) + size_t(c)] = uint8_t(1 + depthBand(d, dg.maxDepth, in.isobathStepM, bands));
                }
            }
        }
    };

    QString mosaicKml;
    std::vector<uint8_t> px;
    if (in.mosaic) {
        const int f = std::max(1, int(std::ceil(double(std::max(mg.width, mg.height)) / kOverviewMax)));
        const int chunksX = (mg.width + kChunk - 1) / kChunk, chunksY = (mg.height + kChunk - 1) / kChunk;
        if (f > 1) {   // a coarse picture of the whole, under the pieces, for the view from afar
            GeoGrid og = mg;
            og.dLat *= f;
            og.dLon *= f;
            og.width = (mg.width + f - 1) / f;
            og.height = (mg.height + f - 1) / f;
            pieces.append({ Piece::MosaicOverview, QStringLiteral("files/mosaic_overview.png"), og });
            mosaicKml += QStringLiteral("<GroundOverlay><name>overview</name><drawOrder>1</drawOrder><Icon><href>%1</href></Icon>%2</GroundOverlay>\n")
                .arg(pieces.last().name, latLonBox(og.north, og.south(), og.east(), og.west));
        }
        for (int cy = 0; cy < chunksY; ++cy) {
            for (int cx = 0; cx < chunksX; ++cx) {
                const int x0 = cx * kChunk, y0 = cy * kChunk;
                const Piece pc{ Piece::MosaicChunk, QStringLiteral("files/mosaic_%1_%2.png").arg(cy).arg(cx),
                                partOf(mg, x0, y0, std::min(kChunk, mg.width - x0), std::min(kChunk, mg.height - y0)) };
                renderPiece(pc, px);
                if (!kmzStage(0.4 * double(cy * chunksX + cx + 1) / double(chunksX * chunksY))) {
                    return cancelled();
                }
                if (!anyNonZero(px)) {
                    continue;
                }
                pieces.append(pc);
                const GeoGrid& g = pc.grid;
                const QString box = QStringLiteral("<LatLonAltBox><north>%1</north><south>%2</south><east>%3</east><west>%4</west></LatLonAltBox>")
                    .arg(num(g.north), num(g.south()), num(g.east()), num(g.west));
                mosaicKml += QStringLiteral("<GroundOverlay><name>%1</name><Region>%2<Lod><minLodPixels>%3</minLodPixels></Lod></Region>"
                                            "<drawOrder>2</drawOrder><Icon><href>%1</href></Icon>%4</GroundOverlay>\n")
                    .arg(pc.name, box, QString::number(f > 1 ? 128 : 0), latLonBox(g.north, g.south(), g.east(), g.west));
            }
        }
    }

    QString depthKml, isoKml;
    if (in.depth) {
        const DepthGrid& dg = *in.depth;
        constexpr int kDepthChunk = 2048;
        for (int y0 = 0; y0 < dg.grid.height; y0 += kDepthChunk) {
            for (int x0 = 0; x0 < dg.grid.width; x0 += kDepthChunk) {
                const Piece pc{ Piece::DepthChunk, QStringLiteral("files/depth_%1_%2.png").arg(y0 / kDepthChunk).arg(x0 / kDepthChunk),
                                partOf(dg.grid, x0, y0, std::min(kDepthChunk, dg.grid.width - x0), std::min(kDepthChunk, dg.grid.height - y0)) };
                renderPiece(pc, px);
                if (!anyNonZero(px)) {
                    continue;
                }
                pieces.append(pc);
                const GeoGrid& g = pc.grid;
                // Over the mosaic the colours would hide it: shown when there is no mosaic, a tick away otherwise.
                depthKml += QStringLiteral("<GroundOverlay><name>depth</name><visibility>%1</visibility><drawOrder>3</drawOrder>"
                                           "<color>d8ffffff</color><Icon><href>%2</href></Icon>%3</GroundOverlay>\n")
                    .arg(QString::number(in.mosaic ? 0 : 1), pc.name, latLonBox(g.north, g.south(), g.east(), g.west));
            }
        }
        // Isobaths, one placemark per depth.
        const QVector<Isobath> isos = traceIsobaths(dg, in.isobathStepM);
        QMap<double, QStringList> byDepth;
        for (const Isobath& iso : isos) {
            QString coords;
            coords.reserve(iso.lonLat.size() * 26);
            for (const QPointF& p : iso.lonLat) {
                coords += num(p.x()) + QLatin1Char(',') + num(p.y()) + QStringLiteral(",0 ");
            }
            byDepth[iso.depth] << QStringLiteral("<LineString><tessellate>1</tessellate><coordinates>%1</coordinates></LineString>").arg(coords.trimmed());
        }
        for (auto it = byDepth.cbegin(); it != byDepth.cend(); ++it) {
            isoKml += QStringLiteral("<Placemark><name>%1 m</name><styleUrl>#isobath</styleUrl><MultiGeometry>%2</MultiGeometry></Placemark>\n")
                .arg(QString::number(it.key(), 'g', 6), it.value().join(QString()));
        }
    }

    QString trackKml;
    if (!in.trackNe.isEmpty()) {
        QString coords;
        for (const QPointF& p : std::as_const(in.trackNe)) {
            double lat, lon;
            nedToLla(in.ref, p.x(), p.y(), lat, lon);
            coords += num(lon) + QLatin1Char(',') + num(lat) + QStringLiteral(",0 ");
        }
        trackKml = QStringLiteral("<Placemark><name>Track</name><styleUrl>#track</styleUrl><LineString><tessellate>1</tessellate><coordinates>%1</coordinates></LineString></Placemark>\n")
            .arg(coords.trimmed());
    }

    QString kml;
    kml += QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<kml xmlns=\"http://www.opengis.net/kml/2.2\" xmlns:atom=\"http://www.w3.org/2005/Atom\">\n<Document>\n");
    kml += QStringLiteral("<name>%1</name>\n").arg(xml(in.meta.title));
    if (!in.meta.authorName.isEmpty()) {
        kml += QStringLiteral("<atom:author><atom:name>%1</atom:name></atom:author>\n").arg(xml(in.meta.authorName));
    }
    kml += QStringLiteral("<description><![CDATA[%1]]></description>\n").arg(toKmlDescription(in.meta));
    {   // Opened, the viewer flies straight over the map, north up, the whole of it in view.
        const double midLat = 0.5 * (in.meta.north + in.meta.south), midLon = 0.5 * (in.meta.east + in.meta.west);
        const double mPerDeg = double(CONSTANTS_RADIUS_OF_EARTH) * double(M_DEG_TO_RAD);
        const double spanM = std::max((in.meta.north - in.meta.south) * mPerDeg,
                                      (in.meta.east - in.meta.west) * mPerDeg * std::cos(midLat * double(M_DEG_TO_RAD)));
        kml += QStringLiteral("<LookAt><longitude>%1</longitude><latitude>%2</latitude><altitude>0</altitude><heading>0</heading>"
                              "<tilt>0</tilt><range>%3</range><altitudeMode>clampToGround</altitudeMode></LookAt>\n")
            .arg(num(midLon), num(midLat), num(std::max(200.0, 1.6 * spanM), 0));
    }
    kml += QStringLiteral("<ExtendedData>");
    for (const auto& kv : toGdalItems(in.meta)) {
        kml += QStringLiteral("<Data name=\"%1\"><value>%2</value></Data>").arg(xml(kv.first.toLower()), xml(kv.second));
    }
    kml += QStringLiteral("</ExtendedData>\n");
    kml += QStringLiteral("<Style id=\"isobath\"><LineStyle><color>ffffffff</color><width>1.2</width></LineStyle></Style>\n"
                          "<Style id=\"track\"><LineStyle><color>ff00a5ff</color><width>2</width></LineStyle></Style>\n");
    kml += QStringLiteral("<ScreenOverlay><name>Legend and notice</name><Icon><href>files/legend.png</href></Icon>"
                          "<overlayXY x=\"0\" y=\"0\" xunits=\"fraction\" yunits=\"fraction\"/>"
                          "<screenXY x=\"0.01\" y=\"0.04\" xunits=\"fraction\" yunits=\"fraction\"/>"
                          "<size x=\"0\" y=\"0\" xunits=\"pixels\" yunits=\"pixels\"/></ScreenOverlay>\n");
    if (!mosaicKml.isEmpty()) {
        kml += QStringLiteral("<Folder><name>Side-scan mosaic</name>\n") + mosaicKml + QStringLiteral("</Folder>\n");
    }
    if (!depthKml.isEmpty()) {
        kml += QStringLiteral("<Folder><name>Depth colours</name><visibility>") + (in.mosaic ? QStringLiteral("0") : QStringLiteral("1"))
            + QStringLiteral("</visibility>\n") + depthKml + QStringLiteral("</Folder>\n");
    }
    if (!isoKml.isEmpty()) {
        kml += QStringLiteral("<Folder><name>Isobaths, m</name>\n") + isoKml + QStringLiteral("</Folder>\n");
    }
    if (!trackKml.isEmpty()) {
        kml += QStringLiteral("<Folder><name>Track</name>\n") + trackKml + QStringLiteral("</Folder>\n");
    }
    kml += QStringLiteral("</Document>\n</kml>\n");

    QByteArray legend;
    {
        QBuffer buf(&legend);
        buf.open(QIODevice::WriteOnly);
        legendImage(in, dPal, bands).save(&buf, "PNG");
    }

    ZipWriter zip;
    QString err;
    if (!zip.open(kmzPath, &err)) {
        return failed(err);
    }
    out.files << kmzPath;
    if (!zip.add(QStringLiteral("doc.kml"), kml.toUtf8(), true, &err)
        || !zip.add(QStringLiteral("README.txt"), toReadme(in.meta).toUtf8(), true, &err)
        || !zip.add(QStringLiteral("metadata.json"), metaJson, true, &err)
        || !zip.add(QStringLiteral("files/legend.png"), legend, false, &err)) {
        zip.close(nullptr);
        return failed(err);
    }
    for (int i = 0; i < pieces.size(); ++i) {
        const Piece& pc = pieces[i];
        renderPiece(pc, px);
        const QByteArray png = indexedPng(px.data(), pc.grid.width, pc.grid.height, pc.kind == Piece::DepthChunk ? dPal : mPal);
        if (!zip.add(pc.name, png, false, &err)) {
            zip.close(nullptr);
            return failed(err);
        }
        if (!kmzStage(0.4 + 0.6 * double(i + 1) / double(pieces.size()))) {
            zip.close(nullptr);
            return cancelled();
        }
    }
    if (!zip.close(&err)) {
        return failed(err);
    }
    kmzStage(1.0);
    return true;
}

} // namespace mapexport
