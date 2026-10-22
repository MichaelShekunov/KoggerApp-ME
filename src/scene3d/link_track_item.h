#pragma once

#include <QColor>
#include <QList>
#include <QPointer>
#include <QQuickItem>
#include <QVariantList>

class GraphicsScene3dView;
class MavVehicle;

/**
 * @brief The boat's track by link quality over the 3D view (BoatMissionLayer.qml): each stretch in the colour of the
 * link on it, a stretch over a loss dashed (link_track_geometry::build).
 *
 * One scene-graph node. The samples are read when the track changes (MavVehicle::linkTrackChanged, ...ShownChanged);
 * at each camera change the item is only marked for update, and placed again in C++ as it is drawn. It replaced a
 * Repeater of Shapes made anew in JavaScript at every camera move: with a long session's track (3000 samples, 550
 * stretches) a move of a drag took 46 ms on a desktop, several times that on the head unit (2026-10-22).
 */
class LinkTrackItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QObject* view READ view WRITE setView NOTIFY viewChanged)
    Q_PROPERTY(QObject* vehicle READ vehicle WRITE setVehicle NOTIFY vehicleChanged)
    /** A colour for each quality (LinkTrack::Quality): AppPalette.linkQualityColors. */
    Q_PROPERTY(QVariantList colors READ colors WRITE setColors NOTIFY colorsChanged)
    Q_PROPERTY(qreal lineWidth READ lineWidth WRITE setLineWidth NOTIFY lineWidthChanged)
    /** The samples drawn: 0 while the track is hidden. */
    Q_PROPERTY(int samples READ samples NOTIFY samplesChanged)

public:
    explicit LinkTrackItem(QQuickItem* parent = nullptr);

    QObject* view() const;
    void setView(QObject* view);
    QObject* vehicle() const;
    void setVehicle(QObject* vehicle);
    QVariantList colors() const { return colorsIn_; }
    void setColors(const QVariantList& colors);
    qreal lineWidth() const { return lineWidth_; }
    void setLineWidth(qreal w);
    int samples() const { return int(lats_.size()); }

signals:
    void viewChanged();
    void vehicleChanged();
    void colorsChanged();
    void lineWidthChanged();
    void samplesChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void readTrack();

    QPointer<GraphicsScene3dView> view_;
    QPointer<MavVehicle> vehicle_;
    QMetaObject::Connection cameraConn_;
    QList<QMetaObject::Connection> vehicleConns_;
    QVariantList colorsIn_;
    QList<QColor> colors_;
    qreal lineWidth_ = 4.0;
    QList<double> lats_, lons_;
    QList<int> quality_;
};
