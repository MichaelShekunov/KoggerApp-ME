#pragma once

// "My maps" (docs/map-export.md): the KOGGER maps (.kmap) this user has opened -- their own and the
// ones others sent. Each opened file is copied into the app's data folder, so it stays when the
// download or the messenger's cache goes. Exposed to QML as `mapLibrary`; the scene draws the visible
// ones under the boat (MapLayer) and asks it for the depth at a point.

#include <limits>
#include <memory>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QQuickImageProvider>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include "map_package.h"

class MapLibrary : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList maps READ maps NOTIFY mapsChanged)
    Q_PROPERTY(QString lastMessage READ lastMessage NOTIFY lastMessageChanged)
    Q_PROPERTY(bool lastOk READ lastOk NOTIFY lastMessageChanged)
    // The depth from the visible maps under the boat (Dataset's last raw position).
    Q_PROPERTY(bool boatDepthValid READ boatDepthValid NOTIFY boatDepthChanged)
    Q_PROPERTY(double boatDepth READ boatDepth NOTIFY boatDepthChanged)
    Q_PROPERTY(QString boatDepthMap READ boatDepthMap NOTIFY boatDepthChanged)

public:
    explicit MapLibrary(QObject* parent = nullptr);

    /// The folder the maps are kept in (AppDataLocation/maps unless set, for the tools).
    void setFolder(const QString& folder);
    QString folder() const;
    /// Reads every .kmap in the folder (on start).
    void reload();

    /// Opens a map file (a path, a file:// or content:// URL): checks it, copies it into the folder
    /// and shows it. False, with lastMessage saying why, when it is not a KOGGER map.
    Q_INVOKABLE bool importFile(const QString& pathOrUrl);
    Q_INVOKABLE void setVisible(const QString& id, bool visible);
    Q_INVOKABLE void remove(const QString& id);
    /// Asks the view to show the whole map (flyToRequested).
    Q_INVOKABLE void showOnMap(const QString& id);
    /// The depth from the visible maps at a point: {depth (m, NaN if none), title}; the newest map
    /// that has a depth there wins.
    Q_INVOKABLE QVariantMap depthAt(double latDeg, double lonDeg) const;

    QVariantList maps() const;
    QString lastMessage() const { return lastMessage_; }
    bool lastOk() const { return lastOk_; }
    void setBoatPosition(double latDeg, double lonDeg);
    bool boatDepthValid() const { return boatDepthValid_; }
    double boatDepth() const { return boatDepth_; }
    QString boatDepthMap() const { return boatDepthMap_; }

    struct Entry {
        QString id;          // the file's name in the folder
        QString path;
        bool visible = true;
        qint64 addedAt = 0;  // ms since the epoch, for "newest wins"
        std::shared_ptr<const mapexport::MapPackage> pkg;
    };
    /// The visible maps, oldest first (so the newest is drawn on top).
    QList<Entry> visibleEntries() const;
    /// A map's preview; safe from the image provider's thread.
    QImage previewOf(const QString& id) const;

signals:
    void mapsChanged();
    void lastMessageChanged();
    void boatDepthChanged();
    void flyToRequested(double west, double south, double east, double north);

private:
    void setMessage(bool ok, const QString& text);
    void saveHidden() const;
    void updatePreviews();
    void updateBoatDepth();
    double boatLat_ = std::numeric_limits<double>::quiet_NaN();
    double boatLon_ = std::numeric_limits<double>::quiet_NaN();
    bool boatDepthValid_ = false;
    double boatDepth_ = 0.0;
    QString boatDepthMap_;
    mutable QString folder_;   // folder(): set on first use
    QList<Entry> entries_;   // in the order they were added
    mutable QMutex previewsMu_;
    QHash<QString, QImage> previews_;   // id -> preview, read by the image provider's thread
    QString lastMessage_;
    bool lastOk_ = true;
};

/// "image://kmap/<id>": a map's preview for the list (registered on the QML engine by Core).
class MapPreviewProvider : public QQuickImageProvider
{
public:
    explicit MapPreviewProvider(MapLibrary* library);
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
private:
    QPointer<MapLibrary> library_;
};
