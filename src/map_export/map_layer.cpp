#include "map_layer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <QtMath>
#include <QDebug>
#include <QElapsedTimer>
#include <QSettings>
#include "app_profile.h"
#include "draw_utils.h"
#include "isobaths_defs.h"
#include "map_export_writer.h"
#include "map_library.h"
#if defined(Q_OS_ANDROID) || defined(LINUX_ES)
#include <GLES3/gl3.h>   // GL_R8, GL_RED, GL_RGBA8: OpenGL ES 3, as scene3d_renderer.h takes them
#endif

namespace {

constexpr int kMaxSideVertices = 400;   // a map's depth mesh: at most ~400 x 400 vertices
constexpr int kPiecePx = 1024;          // mosaic textures
constexpr int kPieceSub = 8;            // each piece a grid of 8 x 8 quads, so it follows the projection
constexpr float kSlopeGain = 30.0f;     // the shader draws isobath lines only where the normal leans
constexpr int kMaxLevels = 2048;        // the palette is a levels x 1 texture: OpenGL ES 3 takes 2048 wide everywhere
constexpr float kLabelZShift = 0.03f;   // SurfaceView::kLabelZShift

// The isobath tool's theme over `levels` bands, deepest first, as the surface's own palette is made
// (SurfaceProcessor::generateExpandedPalette, then updateTexture's rounding down to bytes).
QImage toolPaletteImage(int theme, int levels)
{
    const QVector<QVector3D>& base = IsobathUtils::colorPalette(theme);
    QImage img(levels, 1, QImage::Format_RGBA8888);
    const int n = base.size();
    for (int i = 0; i < levels; ++i) {
        QVector3D c(1.0f, 1.0f, 1.0f);
        if (n > 0 && levels <= 1) {
            c = base.first();
        }
        else if (n > 0) {
            const float t = static_cast<float>(i) / static_cast<float>(levels - 1);
            const float ft = t * (n - 1);
            const int i0 = static_cast<int>(ft);
            const int i1 = std::min(i0 + 1, n - 1);
            const float l = ft - static_cast<float>(i0);
            c = (1.f - l) * base[i0] + l * base[i1];
        }
        img.setPixel(i, 0, qRgba(static_cast<uint8_t>(qBound(0.f, c.x() * 255.f, 255.f)),
                                 static_cast<uint8_t>(qBound(0.f, c.y() * 255.f, 255.f)),
                                 static_cast<uint8_t>(qBound(0.f, c.z() * 255.f, 255.f)), 255));
    }
    return img;
}


} // namespace

MapLayer::MapLayer(QObject* parent) :
    QObject(parent)
{
    QSettings s(QStringLiteral("KOGGER"), koggerAppName());
    visible_ = s.value(QStringLiteral("mapLayer/visible"), true).toBool();
    depthVisible_ = s.value(QStringLiteral("mapLayer/depth"), true).toBool();
    mosaicVisible_ = s.value(QStringLiteral("mapLayer/mosaic"), false).toBool();
}

void MapLayer::saveSettings() const
{
    QSettings s(QStringLiteral("KOGGER"), koggerAppName());
    s.setValue(QStringLiteral("mapLayer/visible"), visible_);
    s.setValue(QStringLiteral("mapLayer/depth"), depthVisible_);
    s.setValue(QStringLiteral("mapLayer/mosaic"), mosaicVisible_);
}

void MapLayer::setVisible(bool on)
{
    if (visible_ != on) {
        visible_ = on;
        frame_.reset();
        saveSettings();
        emit settingsChanged();
        emit changed();
    }
}

void MapLayer::setLibrary(MapLibrary* library)
{
    if (library_) {
        disconnect(library_, nullptr, this, nullptr);
    }
    library_ = library;
    if (library_) {
        // Made here, on the GUI thread, not in the renderer's synchronize: a large mosaic takes a while.
        connect(library_, &MapLibrary::mapsChanged, this, [this]() {
            rebuildGeo();
            emit changed();
        });
    }
    rebuildGeo();
    emit changed();
}

void MapLayer::setDepthVisible(bool on)
{
    if (depthVisible_ != on) {
        depthVisible_ = on;
        frame_.reset();
        saveSettings();
        emit settingsChanged();
        emit changed();
    }
}

void MapLayer::setMosaicVisible(bool on)
{
    if (mosaicVisible_ != on) {
        mosaicVisible_ = on;
        frame_.reset();
        saveSettings();
        emit settingsChanged();
        emit changed();
    }
}

void MapLayer::setIsobathStyle(int themeId, float stepM)
{
    if (themeId == themeId_ && stepM == toolStep_) {
        return;
    }
    themeId_ = themeId;
    toolStep_ = stepM;
    rebuildStyle();
    emit changed();
}

void MapLayer::setMosaicStyle(int themeId, float lowLevel, float highLevel)
{
    if (themeId == mosaicTheme_ && lowLevel == mosaicLow_ && highLevel == mosaicHigh_) {
        return;
    }
    mosaicTheme_ = themeId;
    mosaicLow_ = lowLevel;
    mosaicHigh_ = highLevel;
    rebuildMosaicStyle();
    emit changed();
}

void MapLayer::rebuildMosaicStyle()
{
    frame_.reset();
    if (!mosaicPaletteKey_.isEmpty()) {
        textures_.remove(mosaicPaletteKey_);
    }
    // The table the live mosaic is drawn with (DataProcessor::setMosaicTheme / setMosaicLevels ->
    // emitMosaicColorTable), made the same way from the same theme and levels.
    mosaic::PlotColorTable table;
    table.setTheme(mosaicTheme_);
    table.setLevels(mosaicLow_, mosaicHigh_);
    const std::vector<uint8_t> rgba = table.getRgbaColors();
    QImage img(256, 1, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    std::memcpy(img.bits(), rgba.data(), std::min(rgba.size(), size_t(256 * 4)));
    mosaicPaletteKey_ = QStringLiteral("mospal:%1:%2:%3").arg(mosaicTheme_).arg(mosaicLow_).arg(mosaicHigh_);
    textures_.insert(mosaicPaletteKey_, img);
    for (GeoPiece& pc : geoPieces_) {
        pc.paletteKey = mosaicPaletteKey_;
    }
    qInfo().noquote() << QStringLiteral("my maps mosaic: theme %1, levels %2..%3, %4 pieces")
                         .arg(mosaicTheme_).arg(mosaicLow_).arg(mosaicHigh_).arg(geoPieces_.size());
}

void MapLayer::rebuildGeo()
{
    geoDirty_ = false;
    frame_.reset();
    geoMeshes_.clear();
    geoPieces_.clear();
    textures_.clear();
    stylePaletteKey_.clear();
    mosaicPaletteKey_.clear();
    if (!library_) {
        rebuildStyle();
        rebuildMosaicStyle();
        return;
    }
    const auto entries = library_->visibleEntries();

    for (const auto& e : entries) {
        const mapexport::MapPackage& p = *e.pkg;
        const QString tag = e.id + QLatin1Char(':');   // texture keys stay with the map, whatever else is opened or hidden

        if (p.hasDepth) {
            const mapexport::DepthGrid& dg = p.depth;
            const auto& g = dg.grid;
            const int k = std::max(1, int(std::ceil(double(std::max(g.width, g.height)) / kMaxSideVertices)));
            const int cols = (g.width + k - 1) / k, rows = (g.height + k - 1) / k;
            GeoMesh m;
            m.lat.resize(rows * cols);
            m.lon.resize(rows * cols);
            m.depth.resize(rows * cols);
            for (int i = 0; i < rows; ++i) {
                const int r = std::min(g.height - 1, i * k);
                for (int j = 0; j < cols; ++j) {
                    const int c = std::min(g.width - 1, j * k);
                    const int v = i * cols + j;
                    m.lat[v] = g.latAt(r + 0.5);
                    m.lon[v] = g.lonAt(c + 0.5);
                    m.depth[v] = dg.depth[size_t(r) * size_t(g.width) + size_t(c)];
                }
            }
            // Slopes in scene axes (x north, y east, z = -depth), from the neighbours that have a depth.
            const double midLat = 0.5 * (g.north + g.south());
            const double mNorth = k * g.dLat * 110540.0;
            const double mEast = k * g.dLon * 111320.0 * std::cos(qDegreesToRadians(midLat));
            m.nrm.resize(rows * cols);
            auto d = [&](int i, int j) { return (i < 0 || j < 0 || i >= rows || j >= cols) ? NAN : m.depth[i * cols + j]; };
            for (int i = 0; i < rows; ++i) {
                for (int j = 0; j < cols; ++j) {
                    const float c0 = d(i, j);
                    auto diff = [&](float a, float b, double span) -> double {
                        if (std::isfinite(a) && std::isfinite(b)) return (a - b) / span;
                        return 0.0;
                    };
                    // row i grows southwards, column j eastwards
                    const float n1 = d(i - 1, j), s1 = d(i + 1, j), e1 = d(i, j + 1), w1 = d(i, j - 1);
                    double dDepthDn = std::isfinite(n1) && std::isfinite(s1) ? (n1 - s1) / (2.0 * mNorth)
                                    : std::isfinite(n1) ? diff(n1, c0, mNorth) : diff(c0, s1, mNorth);
                    double dDepthDe = std::isfinite(e1) && std::isfinite(w1) ? (e1 - w1) / (2.0 * mEast)
                                    : std::isfinite(e1) ? diff(e1, c0, mEast) : diff(c0, w1, mEast);
                    // z = -depth: dz/dn = -dDepth/dn. The normal of z = f(n, e) is (-dz/dn, -dz/de, 1).
                    m.nrm[i * cols + j] = QVector3D(float(dDepthDn) * kSlopeGain, float(dDepthDe) * kSlopeGain, 1.0f).normalized();
                }
            }
            for (int i = 0; i + 1 < rows; ++i) {
                for (int j = 0; j + 1 < cols; ++j) {
                    const GLuint a = GLuint(i * cols + j), b = a + 1, c = GLuint((i + 1) * cols + j), dd = c + 1;
                    const bool fa = std::isfinite(m.depth[a]), fb = std::isfinite(m.depth[b]);
                    const bool fc = std::isfinite(m.depth[c]), fd = std::isfinite(m.depth[dd]);
                    if (fa && fb && fc) { m.idx << a << c << b; }
                    if (fb && fc && fd) { m.idx << b << c << dd; }
                    if (fa && fb && fd && !fc) { m.idx << a << dd << b; }
                    if (fa && fc && fd && !fb) { m.idx << a << c << dd; }
                }
            }
            m.minDepth = dg.minDepth;
            m.maxDepth = dg.maxDepth;
            m.lat0 = midLat;
            m.lon0 = 0.5 * (g.west + g.east());
            m.mLat = 110540.0;
            m.mLon = 111320.0 * std::cos(qDegreesToRadians(midLat));
            if (!m.idx.isEmpty()) {
                geoMeshes_.append(m);
            }
        }

        if (p.hasMosaic) {
            const auto& g = p.mosaicGrid;
            for (int y0 = 0; y0 < g.height; y0 += kPiecePx) {
                for (int x0 = 0; x0 < g.width; x0 += kPiecePx) {
                    const int w = std::min(kPiecePx, g.width - x0), h = std::min(kPiecePx, g.height - y0);
                    QImage img(w, h, QImage::Format_Grayscale8);
                    bool any = false;
                    for (int r = 0; r < h; ++r) {
                        const uint8_t* src = p.mosaic.data() + size_t(y0 + r) * size_t(g.width) + size_t(x0);
                        std::memcpy(img.scanLine(r), src, size_t(w));
                        any = any || std::any_of(src, src + w, [](uint8_t v) { return v != 0; });
                    }
                    if (!any) {
                        continue;
                    }
                    GeoPiece pc;
                    pc.textureKey = tag + QStringLiteral("mos:%1:%2").arg(y0).arg(x0);
                    textures_.insert(pc.textureKey, img);
                    for (int i = 0; i <= kPieceSub; ++i) {
                        for (int j = 0; j <= kPieceSub; ++j) {
                            const double u = double(j) / kPieceSub, v = double(i) / kPieceSub;
                            pc.lat << g.latAt(y0 + v * h);
                            pc.lon << g.lonAt(x0 + u * w);
                            pc.uv << QVector2D(float(u), float(v));
                        }
                    }
                    for (int i = 0; i < kPieceSub; ++i) {
                        for (int j = 0; j < kPieceSub; ++j) {
                            const GLuint a = GLuint(i * (kPieceSub + 1) + j), b = a + 1, c = a + GLuint(kPieceSub + 1), dd = c + 1;
                            pc.idx << a << c << b << b << c << dd;
                        }
                    }
                    geoPieces_.append(pc);
                }
            }
        }
    }
    rebuildStyle();
    rebuildMosaicStyle();   // the pieces' palette key
}

void MapLayer::rebuildStyle()
{
    frame_.reset();
    geoLabels_.clear();
    if (!stylePaletteKey_.isEmpty()) {
        textures_.remove(stylePaletteKey_);
        stylePaletteKey_.clear();
    }
    if (geoMeshes_.isEmpty()) {
        return;
    }
    QElapsedTimer clock;
    clock.start();

    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (const GeoMesh& g : std::as_const(geoMeshes_)) {
        lo = std::min(lo, g.minDepth);
        hi = std::max(hi, g.maxDepth);
    }
    double step = toolStep_ > 0.0f ? double(toolStep_) : mapexport::autoIsobathStep(lo, hi);
    // Lines at whole multiples of the step: band k from z = -bottom + k * step (isobaths_colored.fsh).
    double top = 0.0, bottom = 0.0;
    int levels = 1;
    for (;;) {
        top = std::floor(lo / step) * step;
        bottom = std::ceil(hi / step) * step;
        levels = int(std::lround((bottom - top) / step)) + 1;
        if (levels <= kMaxLevels) {
            break;
        }
        step *= 2.0;
    }
    styleStep_ = float(step);
    styleDepthMin_ = float(-bottom);
    styleLevels_ = levels;
    stylePaletteKey_ = QStringLiteral("depthpal:%1:%2").arg(themeId_).arg(levels);
    textures_.insert(stylePaletteKey_, toolPaletteImage(themeId_, levels));

    // The labels in each map's own frame in metres, where SurfaceView makes its own; spread over the
    // levels of detail once here, each map's alone, and only moved into the view's frame per frame.
    int candidates = 0;
    for (const GeoMesh& g : std::as_const(geoMeshes_)) {
        QVector<QVector3D> loc(g.lat.size());
        for (int v = 0; v < g.lat.size(); ++v) {
            loc[v] = QVector3D(float((g.lat[v] - g.lat0) * g.mLat), float((g.lon[v] - g.lon0) * g.mLon),
                               std::isfinite(g.depth[v]) ? -g.depth[v] : 0.0f);
        }
        QVector<isolabels::Label> cand;
        for (int t = 0; t + 2 < g.idx.size(); t += 3) {
            isolabels::addFromTriangle(loc[g.idx[t]], loc[g.idx[t + 1]], loc[g.idx[t + 2]],
                                       styleDepthMin_, styleStep_, styleLevels_, kLabelZShift, cand);
        }
        candidates += cand.size();
        QVector<isolabels::Label> kept;
        isolabels::assignLevels(cand, kept, styleStep_ * 2.0f);   // SurfaceView's base spacing
        for (const isolabels::Label& l : std::as_const(kept)) {
            geoLabels_.append(GeoLabel{ g.lat0 + l.pos.x() / g.mLat, g.lon0 + l.pos.y() / g.mLon, l });
        }
    }
    qInfo().noquote() << QStringLiteral("my maps isobaths: theme %1, step %2 m (tool %3), depths %4..%5 m, %6 bands, %7 labels of %8, %9 ms")
                         .arg(themeId_).arg(styleStep_).arg(toolStep_).arg(top).arg(bottom).arg(levels)
                         .arg(geoLabels_.size()).arg(candidates).arg(clock.elapsed());
}

std::shared_ptr<const MapLayerFrame> MapLayer::frameFor(const LLARef& viewRef, bool perspective)
{
    if (geoDirty_) {
        rebuildGeo();
    }
    if (frame_ && frameLat_ == viewRef.refLla.latitude && frameLon_ == viewRef.refLla.longitude && framePerspective_ == perspective) {
        return frame_;
    }
    auto f = std::make_shared<MapLayerFrame>();
    auto project = [&](double lat, double lon, float z) {
        const LLA lla(lat, lon, 0.0);
        const NED ned(&lla, &viewRef, perspective);
        return QVector3D(float(ned.n), float(ned.e), z);
    };
    if (visible_ && depthVisible_) {
        for (const GeoMesh& g : std::as_const(geoMeshes_)) {
            MapLayerFrame::Mesh m;
            m.pos.resize(g.lat.size());
            for (int v = 0; v < g.lat.size(); ++v) {
                m.pos[v] = project(g.lat[v], g.lon[v], std::isfinite(g.depth[v]) ? -g.depth[v] : 0.0f);
            }
            m.nrm = g.nrm;
            m.idx = g.idx;
            m.depthMin = styleDepthMin_;
            m.step = styleStep_;
            m.levels = styleLevels_;
            m.paletteKey = stylePaletteKey_;
            f->meshes << m;
        }
        if (!mosaicVisible_) {
            f->labels.reserve(geoLabels_.size());
            for (const GeoLabel& gl : std::as_const(geoLabels_)) {
                isolabels::Label l = gl.label;
                l.pos = project(gl.lat, gl.lon, gl.label.pos.z());
                f->labels << l;
            }
            f->labelSpacing = styleStep_ * 2.0f;
            f->labelStep = styleStep_;
        }
    }
    if (visible_ && mosaicVisible_) {
        for (const GeoPiece& g : std::as_const(geoPieces_)) {
            MapLayerFrame::Piece p;
            p.pos.resize(g.lat.size());
            for (int v = 0; v < g.lat.size(); ++v) {
                p.pos[v] = project(g.lat[v], g.lon[v], 0.0f);
            }
            p.uv = g.uv;
            p.idx = g.idx;
            p.textureKey = g.textureKey;
            p.paletteKey = g.paletteKey;
            f->pieces << p;
        }
    }
    f->textures = textures_;
    qInfo().noquote() << QStringLiteral("my maps layer: %1 depth meshes, %2 mosaic pieces, %3 labels, view frame %4, %5 (%6)")
                         .arg(f->meshes.size()).arg(f->pieces.size()).arg(f->labels.size()).arg(viewRef.refLla.latitude, 0, 'f', 6)
                         .arg(viewRef.refLla.longitude, 0, 'f', 6).arg(perspective ? QStringLiteral("perspective") : QStringLiteral("top-down"));
    frame_ = f;
    frameLat_ = viewRef.refLla.latitude;
    frameLon_ = viewRef.refLla.longitude;
    framePerspective_ = perspective;
    return frame_;
}

void MapLayerGl::syncTextures(QOpenGLFunctions* ctx)
{
    if (uploadedFor_ == frame_) {
        return;
    }
    uploadedFor_ = frame_;
    const QHash<QString, QImage> empty;
    const auto& want = frame_ ? frame_->textures : empty;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (!want.contains(it.key())) {
            ctx->glDeleteTextures(1, &it.value());
            it = textures_.erase(it);
        }
        else {
            ++it;
        }
    }
    GLint prevUnpack = 4;
    ctx->glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevUnpack);
    ctx->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (auto it = want.cbegin(); it != want.cend(); ++it) {
        if (textures_.contains(it.key())) {
            continue;
        }
        const QImage& img = it.value();
        GLuint id = 0;
        ctx->glGenTextures(1, &id);
        ctx->glBindTexture(GL_TEXTURE_2D, id);
        ctx->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        ctx->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (img.format() == QImage::Format_Grayscale8) {   // mosaic indices, as SurfaceView's tiles
            ctx->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            ctx->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            QByteArray packed(img.width() * img.height(), Qt::Uninitialized);
            for (int r = 0; r < img.height(); ++r) {
                std::memcpy(packed.data() + r * img.width(), img.constScanLine(r), size_t(img.width()));
            }
            ctx->glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, img.width(), img.height(), 0, GL_RED, GL_UNSIGNED_BYTE, packed.constData());
            ctx->glGenerateMipmap(GL_TEXTURE_2D);
        }
        else {   // colours: the depth bands (blended as the surface's palette is) or the mosaic table
            const bool depthPal = it.key().startsWith(QStringLiteral("depthpal:"));
            ctx->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, depthPal ? GL_LINEAR : GL_NEAREST);
            ctx->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, depthPal ? GL_LINEAR : GL_NEAREST);
            const QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
            ctx->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rgba.width(), rgba.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
        }
        textures_.insert(it.key(), id);
    }
    ctx->glPixelStorei(GL_UNPACK_ALIGNMENT, prevUnpack);
}

void MapLayerGl::render(QOpenGLFunctions* ctx, const QMatrix4x4& mvp,
                        const QMap<QString, std::shared_ptr<QOpenGLShaderProgram>>& programs)
{
    syncTextures(ctx);
    if (!frame_ || (frame_->meshes.isEmpty() && frame_->pieces.isEmpty())) {
        return;
    }

    if (auto prog = programs.value(QStringLiteral("isobaths"), nullptr); prog && !frame_->meshes.isEmpty() && prog->bind()) {
        prog->setUniformValue("matrix", mvp);
        prog->setUniformValue("shadowsEnabled", false);
        prog->setUniformValue("linePass", false);
        prog->setUniformValue("lineColor", QVector3D(1.0f, 1.0f, 1.0f));
        prog->setUniformValue("lineWidth", 1.0f);
        prog->setUniformValue("lightDir", QVector3D(0.0f, 0.0f, 1.0f));
        prog->setUniformValue("shadowAmbient", 1.0f);
        prog->setUniformValue("shadowIntensity", 0.0f);
        prog->setUniformValue("highlightIntensity", 0.0f);
        prog->setUniformValue("paletteSampler", 0);
        const int posLoc = prog->attributeLocation("position");
        const int nrmLoc = prog->attributeLocation("normal");
        if (posLoc >= 0 && nrmLoc >= 0) {
            prog->enableAttributeArray(posLoc);
            prog->enableAttributeArray(nrmLoc);
            ctx->glActiveTexture(GL_TEXTURE0);
            for (const auto& m : frame_->meshes) {
                ctx->glBindTexture(GL_TEXTURE_2D, textures_.value(m.paletteKey, 0));
                prog->setUniformValue("depthMin", m.depthMin);
                prog->setUniformValue("levelStep", m.step);
                prog->setUniformValue("levelCount", m.levels);
                prog->setAttributeArray(posLoc, m.pos.constData());
                prog->setAttributeArray(nrmLoc, m.nrm.constData());
                ctx->glDrawElements(GL_TRIANGLES, GLsizei(m.idx.size()), GL_UNSIGNED_INT, m.idx.constData());
            }
            prog->disableAttributeArray(nrmLoc);
            prog->disableAttributeArray(posLoc);
        }
        prog->release();
    }

    if (auto prog = programs.value(QStringLiteral("mosaic"), nullptr); prog && !frame_->pieces.isEmpty() && prog->bind()) {
        prog->setUniformValue("mvp", mvp);
        prog->setUniformValue("shadowsEnabled", false);
        prog->setUniformValue("indexedTexture", 0);
        prog->setUniformValue("colorTable", 1);
        const int posLoc = prog->attributeLocation("position");
        const int uvLoc = prog->attributeLocation("texCoord");
        const int nrmLoc = prog->attributeLocation("normal");
        if (posLoc >= 0 && uvLoc >= 0) {
            prog->enableAttributeArray(posLoc);
            prog->enableAttributeArray(uvLoc);
            if (nrmLoc >= 0) {
                prog->disableAttributeArray(nrmLoc);
                prog->setAttributeValue(nrmLoc, QVector3D(0.0f, 0.0f, 1.0f));
            }
            for (const auto& p : frame_->pieces) {
                ctx->glActiveTexture(GL_TEXTURE0);
                ctx->glBindTexture(GL_TEXTURE_2D, textures_.value(p.textureKey, 0));
                ctx->glActiveTexture(GL_TEXTURE1);
                ctx->glBindTexture(GL_TEXTURE_2D, textures_.value(p.paletteKey, 0));
                prog->setAttributeArray(posLoc, p.pos.constData());
                prog->setAttributeArray(uvLoc, p.uv.constData());
                ctx->glDrawElements(GL_TRIANGLES, GLsizei(p.idx.size()), GL_UNSIGNED_INT, p.idx.constData());
            }
            ctx->glActiveTexture(GL_TEXTURE0);
            prog->disableAttributeArray(uvLoc);
            prog->disableAttributeArray(posLoc);
        }
        prog->release();
    }

    if (!frame_->labels.isEmpty()) {
        isolabels::render(ctx, mvp, programs, frame_->labels, frame_->labelSpacing, NAN, frame_->labelStep);
    }
}
