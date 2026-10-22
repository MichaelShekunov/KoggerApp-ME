#include "map_library.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <QDebug>
#include <QtMath>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include "app_profile.h"

namespace {

QString settingsApp()
{
    return koggerAppName();
}

QString readableTitle(const mapexport::MapPackage& p, const QString& fileName)
{
    const QString t = p.title().trimmed();
    return t.isEmpty() ? QFileInfo(fileName).completeBaseName() : t;
}

} // namespace

MapLibrary::MapLibrary(QObject* parent) :
    QObject(parent)
{
    connect(this, &MapLibrary::mapsChanged, this, &MapLibrary::updateBoatDepth);   // a map shown or hidden under the boat
}

QString MapLibrary::folder() const
{
    // Asked for late: Core (and this) are made before main() names the application, and AppDataLocation
    // without the name is a folder every program shares.
    if (folder_.isEmpty()) {
        folder_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/maps");
    }
    return folder_;
}

void MapLibrary::setFolder(const QString& folder)
{
    folder_ = folder;
}

void MapLibrary::reload()
{
    entries_.clear();
    QDir().mkpath(folder());
    QSettings s(QStringLiteral("KOGGER"), settingsApp());
    const QStringList hidden = s.value(QStringLiteral("mapLibrary/hidden")).toStringList();
    QDir dir(folder());
    const QFileInfoList files = dir.entryInfoList({ QStringLiteral("*.") + mapexport::kMapSuffix }, QDir::Files, QDir::Time | QDir::Reversed);
    for (const QFileInfo& fi : files) {
        auto pkg = std::make_shared<mapexport::MapPackage>();
        QString err;
        if (!mapexport::readMapPackage(fi.absoluteFilePath(), *pkg, &err)) {
            qWarning().noquote() << QStringLiteral("map library: %1 skipped: %2").arg(fi.fileName(), err);
            continue;
        }
        Entry e;
        e.id = fi.fileName();
        e.path = fi.absoluteFilePath();
        e.visible = !hidden.contains(e.id);
        e.addedAt = fi.lastModified().toMSecsSinceEpoch();
        e.pkg = pkg;
        entries_.append(e);
    }
    qInfo().noquote() << QStringLiteral("map library: %1 maps in %2").arg(entries_.size()).arg(folder());
    updatePreviews();
    emit mapsChanged();
}

bool MapLibrary::importFile(const QString& pathOrUrl)
{
    const QUrl url(pathOrUrl);
    const QString src = url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
    // Copied first, in pieces, then read from the copy: a content:// file from a messenger is read
    // once, front to back, and the copy stays when the messenger clears its cache. Not by its name:
    // a file another app hands over is often nameless or application/octet-stream.
    QDir().mkpath(folder());
    const QString tmp = QDir(folder()).filePath(QStringLiteral(".incoming.part"));
    QByteArray sha1;
    QString why;
    switch (mapexport::copyMapFile(src, tmp, &sha1, &why)) {
    case mapexport::MapCopy::Copied:
        break;
    case mapexport::MapCopy::CannotRead:
        setMessage(false, tr("Not opened: %1").arg(why));
        return false;
    case mapexport::MapCopy::NotAMap:
        setMessage(false, tr("Not opened: this is not a KOGGER map"));
        return false;
    case mapexport::MapCopy::CannotWrite:
        setMessage(false, tr("Not opened: cannot keep the file (%1)").arg(why));
        return false;
    }
    const QString hash = QString::fromLatin1(sha1.left(10));
    for (const Entry& e : std::as_const(entries_)) {   // the same file again is not added twice
        if (e.id.endsWith(QLatin1Char('_') + hash + QLatin1Char('.') + mapexport::kMapSuffix)) {
            QFile::remove(tmp);
            setVisible(e.id, true);
            showOnMap(e.id);
            setMessage(true, tr("Already in My maps: %1").arg(readableTitle(*e.pkg, e.id)));
            return true;
        }
    }
    auto pkg = std::make_shared<mapexport::MapPackage>();
    QString err;
    if (!mapexport::readMapPackage(tmp, *pkg, &err)) {
        QFile::remove(tmp);
        setMessage(false, tr("Not opened: %1").arg(err));
        return false;
    }
    QString stem = readableTitle(*pkg, src);
    static const QString bad = QStringLiteral("<>:\"/\\|?*");
    for (QChar& ch : stem) {
        if (bad.contains(ch) || ch.unicode() < 32) {
            ch = QLatin1Char('_');
        }
    }
    const QString id = stem.left(60) + QLatin1Char('_') + hash + QLatin1Char('.') + mapexport::kMapSuffix;
    const QString dst = QDir(folder()).filePath(id);
    QFile::remove(dst);
    if (!QFile::rename(tmp, dst)) {
        QFile::remove(tmp);
        setMessage(false, tr("Not opened: cannot keep the file"));
        return false;
    }
    Entry e;
    e.id = id;
    e.path = dst;
    e.visible = true;
    e.addedAt = QDateTime::currentMSecsSinceEpoch();
    e.pkg = pkg;
    entries_.append(e);
    saveHidden();
    updatePreviews();
    emit mapsChanged();
    setMessage(true, tr("Opened: %1").arg(readableTitle(*pkg, id)));
    showOnMap(id);
    return true;
}

void MapLibrary::setVisible(const QString& id, bool visible)
{
    for (Entry& e : entries_) {
        if (e.id == id && e.visible != visible) {
            e.visible = visible;
            saveHidden();
            emit mapsChanged();
            return;
        }
    }
}

void MapLibrary::remove(const QString& id)
{
    for (int i = 0; i < entries_.size(); ++i) {
        if (entries_[i].id == id) {
            QFile::remove(entries_[i].path);
            qInfo().noquote() << QStringLiteral("map library: removed %1").arg(id);   // a map gone by hand, not lost
            entries_.removeAt(i);
            saveHidden();
            emit mapsChanged();
            return;
        }
    }
}

void MapLibrary::showOnMap(const QString& id)
{
    for (const Entry& e : std::as_const(entries_)) {
        if (e.id == id) {
            double w, s, ea, n;
            e.pkg->bounds(w, s, ea, n);
            emit flyToRequested(w, s, ea, n);
            return;
        }
    }
}

bool MapLibrary::showNewestOnMap()
{
    for (int i = entries_.size() - 1; i >= 0; --i) {   // in the order they were added: the newest last
        if (entries_[i].visible) {
            qInfo().noquote() << QStringLiteral("map library: the view to %1, the newest map shown").arg(entries_[i].id);
            showOnMap(entries_[i].id);
            return true;
        }
    }
    return false;
}

void MapLibrary::setBoatPosition(double latDeg, double lonDeg)
{
    boatLat_ = latDeg;
    boatLon_ = lonDeg;
    updateBoatDepth();
}

void MapLibrary::updateBoatDepth()
{
    double depth = std::numeric_limits<double>::quiet_NaN();
    QString title;
    if (std::isfinite(boatLat_) && std::isfinite(boatLon_)) {
        const QVariantMap at = depthAt(boatLat_, boatLon_);
        depth = at.value(QStringLiteral("depth")).toDouble();
        title = at.value(QStringLiteral("title")).toString();
    }
    const bool valid = std::isfinite(depth);
    if (valid == boatDepthValid_ && (!valid || std::fabs(depth - boatDepth_) < 0.005) && title == boatDepthMap_) {
        return;
    }
    boatDepthValid_ = valid;
    boatDepth_ = valid ? depth : 0.0;
    boatDepthMap_ = title;
    emit boatDepthChanged();
}

QVariantMap MapLibrary::depthAt(double latDeg, double lonDeg) const
{
    for (int i = entries_.size() - 1; i >= 0; --i) {
        const Entry& e = entries_[i];
        if (!e.visible || !e.pkg->hasDepth) {
            continue;
        }
        const double d = mapexport::depthAt(e.pkg->depth, latDeg, lonDeg);
        if (std::isfinite(d)) {
            return { { QStringLiteral("depth"), d }, { QStringLiteral("title"), readableTitle(*e.pkg, e.id) } };
        }
    }
    return { { QStringLiteral("depth"), std::numeric_limits<double>::quiet_NaN() }, { QStringLiteral("title"), QString() } };
}

QVariantList MapLibrary::maps() const
{
    QVariantList out;
    for (int i = entries_.size() - 1; i >= 0; --i) {   // newest first in the list
        const Entry& e = entries_[i];
        const auto& p = *e.pkg;
        double w, s, ea, n;
        p.bounds(w, s, ea, n);
        const double midLat = 0.5 * (s + n);
        const double widthM = (ea - w) * 111320.0 * std::cos(qDegreesToRadians(midLat));
        const double heightM = (n - s) * 110540.0;
        out << QVariantMap{
            { QStringLiteral("id"), e.id },
            { QStringLiteral("title"), readableTitle(p, e.id) },
            { QStringLiteral("author"), p.author() },
            { QStringLiteral("license"), p.licenseName() },
            { QStringLiteral("licenseId"), p.licenseId() },
            { QStringLiteral("copyright"), p.copyright() },
            { QStringLiteral("disclaimer"), p.disclaimer() },
            { QStringLiteral("visible"), e.visible },
            { QStringLiteral("hasDepth"), p.hasDepth },
            { QStringLiteral("hasMosaic"), p.hasMosaic },
            { QStringLiteral("minDepth"), p.hasDepth ? double(p.depth.minDepth) : 0.0 },
            { QStringLiteral("maxDepth"), p.hasDepth ? double(p.depth.maxDepth) : 0.0 },
            { QStringLiteral("widthM"), widthM },
            { QStringLiteral("heightM"), heightM },
            { QStringLiteral("preview"), QStringLiteral("image://kmap/") + QString::fromLatin1(QUrl::toPercentEncoding(e.id)) },
        };
    }
    return out;
}

QList<MapLibrary::Entry> MapLibrary::visibleEntries() const
{
    QList<Entry> out;
    for (const Entry& e : entries_) {
        if (e.visible) {
            out << e;
        }
    }
    return out;
}

void MapLibrary::setMessage(bool ok, const QString& text)
{
    lastOk_ = ok;
    lastMessage_ = text;
    qInfo().noquote() << QStringLiteral("map library: %1").arg(text);
    emit lastMessageChanged();
}

void MapLibrary::saveHidden() const
{
    QStringList hidden;
    for (const Entry& e : entries_) {
        if (!e.visible) {
            hidden << e.id;
        }
    }
    QSettings s(QStringLiteral("KOGGER"), settingsApp());
    s.setValue(QStringLiteral("mapLibrary/hidden"), hidden);
}

MapPreviewProvider::MapPreviewProvider(MapLibrary* library) :
    QQuickImageProvider(QQuickImageProvider::Image),
    library_(library)
{
}

QImage MapPreviewProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize)
{
    QImage img = library_ ? library_->previewOf(QUrl::fromPercentEncoding(id.toUtf8())) : QImage();
    if (img.isNull()) {
        img = QImage(1, 1, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
    }
    if (requestedSize.isValid()) {
        img = img.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (size) {
        *size = img.size();
    }
    return img;
}

QImage MapLibrary::previewOf(const QString& id) const
{
    QMutexLocker lock(&previewsMu_);
    return previews_.value(id);
}

void MapLibrary::updatePreviews()
{
    QHash<QString, QImage> p;
    for (const Entry& e : std::as_const(entries_)) {
        p.insert(e.id, e.pkg->preview);
    }
    QMutexLocker lock(&previewsMu_);
    previews_ = p;
}
