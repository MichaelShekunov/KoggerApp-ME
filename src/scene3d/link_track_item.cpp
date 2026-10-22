#include "link_track_item.h"

#include "link_track_geometry.h"
#include "scene3d_view.h"
#include "vehicle/mav_vehicle.h"

#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>

#include <cstring>

static_assert(sizeof(link_track_geometry::Vertex) == sizeof(QSGGeometry::ColoredPoint2D),
              "link_track_geometry::Vertex is QSGGeometry::ColoredPoint2D's layout");

LinkTrackItem::LinkTrackItem(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

QObject* LinkTrackItem::view() const
{
    return view_.data();
}

void LinkTrackItem::setView(QObject* view)
{
    auto* v = qobject_cast<GraphicsScene3dView*>(view);
    if (v == view_) {
        return;
    }
    disconnect(cameraConn_);
    view_ = v;
    if (view_) {
        cameraConn_ = connect(view_, &GraphicsScene3dView::cameraChanged, this, [this]() { update(); });
    }
    emit viewChanged();
    update();
}

QObject* LinkTrackItem::vehicle() const
{
    return vehicle_.data();
}

void LinkTrackItem::setVehicle(QObject* vehicle)
{
    auto* v = qobject_cast<MavVehicle*>(vehicle);
    if (v == vehicle_) {
        return;
    }
    for (const auto& c : vehicleConns_) {
        disconnect(c);
    }
    vehicleConns_.clear();
    vehicle_ = v;
    if (vehicle_) {
        vehicleConns_ << connect(vehicle_, &MavVehicle::linkTrackChanged, this, &LinkTrackItem::readTrack)
                      << connect(vehicle_, &MavVehicle::linkTrackShownChanged, this, &LinkTrackItem::readTrack);
    }
    emit vehicleChanged();
    readTrack();
}

void LinkTrackItem::setColors(const QVariantList& colors)
{
    colorsIn_ = colors;
    colors_.clear();
    for (const QVariant& c : colors) {
        colors_ << c.value<QColor>();
    }
    emit colorsChanged();
    update();
}

void LinkTrackItem::setLineWidth(qreal w)
{
    if (qFuzzyCompare(w, lineWidth_)) {
        return;
    }
    lineWidth_ = w;
    emit lineWidthChanged();
    update();
}

void LinkTrackItem::readTrack()
{
    const int was = samples();
    if (vehicle_ && vehicle_->linkTrackShown()) {
        lats_ = vehicle_->linkTrackLats();
        lons_ = vehicle_->linkTrackLons();
        quality_ = vehicle_->linkTrackQualities();
    }
    else {
        lats_.clear();
        lons_.clear();
        quality_.clear();
    }
    if (samples() != was) {
        emit samplesChanged();
    }
    update();
}

void LinkTrackItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    update();
}

QSGNode* LinkTrackItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    // Here the GUI thread waits for the sync: the view's camera is read as it is now.
    QList<link_track_geometry::Vertex> v;
    if (view_ && lats_.size() >= 2) {
        v = link_track_geometry::build(view_->geoToScreenList(lats_, lons_), quality_, colors_, float(lineWidth_),
                                       LinkTrack::Lost, boundingRect());
    }
    auto* node = static_cast<QSGGeometryNode*>(old);
    if (v.isEmpty()) {
        delete node;
        return nullptr;
    }
    if (!node) {
        node = new QSGGeometryNode;
        auto* g = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        g->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(g);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGVertexColorMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
    }
    QSGGeometry* g = node->geometry();
    g->allocate(int(v.size()));
    std::memcpy(g->vertexData(), v.constData(), size_t(v.size()) * sizeof(link_track_geometry::Vertex));
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}
