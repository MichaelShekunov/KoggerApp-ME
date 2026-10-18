#include "map_layer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <QtMath>
#include <QDebug>
#include <QSettings>
#include "app_profile.h"
#include "map_export_writer.h"
#include "map_library.h"

namespace {

constexpr int kMaxSideVertices = 400;   // a map's depth mesh: at most ~400 x 400 vertices
constexpr int kPiecePx = 1024;          // mosaic textures
constexpr int kPieceSub = 8;            // each piece a grid of 8 x 8 quads, so it follows the projection
constexpr float kSlopeGain = 30.0f;     // the shader draws isobath lines only where the normal leans

QImage depthPaletteImage(int theme, int levels)
{
    const QVector<QRgb> pal = mapexport::depthPalette(theme, levels);
    QImage img(levels, 1, QImage::Format_RGBA8888);
    for (int i = 0; i < levels; ++i) {
        img.setPixel(i, 0, pal[1 + i]);
    }
    return img;
}

QImage mosaicPaletteImage(const QVector<QRgb>& palette)
{
    QImage img(256, 1, QImage::Format_RGBA8888);
    for (int i = 0; i < 256; ++i) {
        const QRgb c = i < palette.size() ? palette[i] : qRgb(i, i, i);
        img.setPixel(i, 0, qRgba(qRed(c), qGreen(c), qBlue(c), 255));
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

void MapLayer::rebuildGeo()
{
    geoDirty_ = false;
    frame_.reset();
    geoMeshes_.clear();
    geoPieces_.clear();
    textures_.clear();
    if (!library_) {
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
            m.step = float(mapexport::autoIsobathStep(dg.minDepth, dg.maxDepth));
            m.levels = mapexport::depthBands(dg.minDepth, dg.maxDepth, m.step);
            m.depthMin = -dg.maxDepth;
            // the maker's colours (map.json), so the map looks as it did to them and on its preview
            const int theme = p.meta.value(QStringLiteral("products")).toObject().value(QStringLiteral("bathymetry")).toObject()
                                  .value(QStringLiteral("paletteTheme")).toInt(1);
            m.paletteKey = QStringLiteral("depthpal:%1:%2").arg(theme).arg(m.levels);
            if (!textures_.contains(m.paletteKey)) {
                textures_.insert(m.paletteKey, depthPaletteImage(theme, m.levels));
            }
            if (!m.idx.isEmpty()) {
                geoMeshes_.append(m);
            }
        }

        if (p.hasMosaic) {
            const auto& g = p.mosaicGrid;
            const QString palKey = tag + QStringLiteral("mospal");
            textures_.insert(palKey, mosaicPaletteImage(p.mosaicPalette));
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
                    pc.paletteKey = palKey;
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
            m.depthMin = g.depthMin;
            m.step = g.step;
            m.levels = g.levels;
            m.paletteKey = g.paletteKey;
            f->meshes << m;
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
    qInfo().noquote() << QStringLiteral("my maps layer: %1 depth meshes, %2 mosaic pieces, view frame %3, %4 (%5)")
                         .arg(f->meshes.size()).arg(f->pieces.size()).arg(viewRef.refLla.latitude, 0, 'f', 6)
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
}
