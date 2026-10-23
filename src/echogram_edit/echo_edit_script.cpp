#include "echo_edit_script.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSValue>
#include <QMouseEvent>
#include <QJsonDocument>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSaveFile>
#include <QWindow>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>

#include "core.h"
#include "device_manager_wrapper.h"
#include "echogram_editor.h"
#include "scene3d_view.h"
#include "vehicle/mav_vehicle.h"
#include "ui_probe.h"

namespace {

QString hex64(quint64 v)
{
    return QStringLiteral("%1").arg(v, 16, 16, QLatin1Char('0'));
}

// Every item of every window.
void forEachPlotItemTree(const std::function<void(QQuickItem*)>& f)
{
    std::function<void(QQuickItem*)> walk = [&](QQuickItem* item) {
        f(item);
        for (QQuickItem* child : item->childItems()) {
            walk(child);
        }
    };
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
        if (auto* qw = qobject_cast<QQuickWindow*>(w)) {
            walk(qw->contentItem());
        }
    }
}

// Every echogram (qPlot2D), or only those on screen. The panes' echograms are made by QML at run time and hang in
// the item tree, not in the window's object tree, so the walk goes over the items.
void forEachPlot(bool visibleOnly, const std::function<void(QQuickItem*)>& f)
{
    std::function<void(QQuickItem*)> walk = [&](QQuickItem* item) {
        if (item->inherits("qPlot2D") && (!visibleOnly || (item->isVisible() && item->width() > 0))) {
            f(item);
        }
        for (QQuickItem* child : item->childItems()) {
            walk(child);
        }
    };
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
        if (auto* qw = qobject_cast<QQuickWindow*>(w)) {
            walk(qw->contentItem());
        }
    }
}

// The first object of the windows' object trees with a property of that name: the workspace store is a QtObject.
QObject* findObjectWithProperty(const char* name)
{
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
        for (QObject* o : w->findChildren<QObject*>()) {
            if (o->property(name).isValid()) {
                return o;
            }
        }
    }
    return nullptr;
}

// The settings widget row (SettingMirrorRow) of a pinned key, on screen.
QQuickItem* findMirrorRow(const QString& key)
{
    QQuickItem* found = nullptr;
    forEachPlotItemTree([&](QQuickItem* item) {
        if (!found && item->property("availableWidth").isValid() && item->property("isDevice").isValid()
                && item->property("key").toString() == key) {
            found = item;
        }
    });
    return found;
}

// A control's value as a string: a switch's checked, a spin box's or a slider's value, a combo's index, tabs' value.
QString controlValue(QObject* c)
{
    if (!c) {
        return QStringLiteral("-");
    }
    const QString kind = c->property("controlKind").toString();
    if (kind == QLatin1String("switch")) return c->property("checked").toBool() ? QStringLiteral("on") : QStringLiteral("off");
    if (kind == QLatin1String("combo"))  return c->property("currentIndex").toString();
    if (kind == QLatin1String("tabs"))   return c->property("currentValue").toString();
    return c->property("value").toString();
}

QQuickItem* findWithProperty(QQuickItem* item, const char* name)
{
    if (item->property(name).isValid()) {
        return item;
    }
    for (QQuickItem* child : item->childItems()) {
        if (QQuickItem* found = findWithProperty(child, name)) {
            return found;
        }
    }
    return nullptr;
}

// The main window's WorkspaceStore: the store of the WorkspaceView (the item with "secondaryPlotItem").
QObject* findWorkspaceStore(QQuickItem** workspace = nullptr)
{
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
        auto* qw = qobject_cast<QQuickWindow*>(w);
        if (!qw || !qw->isVisible()) {
            continue;
        }
        if (QQuickItem* ws = findWithProperty(qw->contentItem(), "secondaryPlotItem")) {
            if (workspace) {
                *workspace = ws;
            }
            return ws->property("store").value<QObject*>();
        }
    }
    return nullptr;
}

// A QML var property, or what a QML function returned, as JSON.
QJsonValue toJson(const QVariant& v)
{
    if (v.metaType() == QMetaType::fromType<QJSValue>()) {
        return QJsonValue::fromVariant(v.value<QJSValue>().toVariant());
    }
    return QJsonValue::fromVariant(v);
}

// Every widget's panel (a BasePanePopup with popupId "widget:<id>") under an item.
void collectWidgetPanels(QQuickItem* item, QList<QQuickItem*>& out)
{
    if (item->property("popupId").toString().startsWith(QLatin1String("widget:"))) {
        out.append(item);
    }
    for (QQuickItem* child : item->childItems()) {
        collectWidgetPanels(child, out);
    }
}

QJsonObject sceneRect(QQuickItem* item)
{
    const QRectF r = item->mapRectToScene(QRectF(0.0, 0.0, item->width(), item->height()));
    return QJsonObject{ { QStringLiteral("x"), r.x() }, { QStringLiteral("y"), r.y() },
                        { QStringLiteral("w"), r.width() }, { QStringLiteral("h"), r.height() } };
}

GraphicsScene3dView* findScene3dView(QQuickItem* item)
{
    if (auto* view = qobject_cast<GraphicsScene3dView*>(item); view && view->isVisible() && view->width() > 0.0 && view->height() > 0.0) {
        return view;
    }
    for (QQuickItem* child : item->childItems()) {
        if (GraphicsScene3dView* found = findScene3dView(child)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

struct EchoEditScript::RelayoutFrames
{
    QElapsedTimer clock;
    // 0 idle, 1 a change made, 2 the frame that takes it synced, 3 that frame on screen
    std::atomic<int> state{ 0 };
    std::atomic<qint64> syncStartNs{ 0 };
    std::atomic<qint64> syncNs{ 0 };
    std::atomic<qint64> shownNs{ 0 };
    // Every frame since the change, the frame that takes it and any after (an animation's): how many, their syncs
    // added up, when the last was on screen.
    std::atomic<qint64> anySyncStartNs{ 0 };
    std::atomic<int> frames{ 0 };
    std::atomic<qint64> syncSumNs{ 0 };
    std::atomic<qint64> lastShownNs{ 0 };
};

struct EchoEditScript::DragFrames
{
    QElapsedTimer clock;
    std::atomic<int> frames{ 0 };
    std::atomic<qint64> lastNs{ 0 };
    std::atomic<qint64> maxGapNs{ 0 };
    std::atomic<int> over50ms{ 0 };   // gaps a hand sees as a jerk
};

EchoEditScript::EchoEditScript(Core* core, QObject* parent) :
    QObject(parent),
    core_(core)
{
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &EchoEditScript::step);
    dragTimer_.setTimerType(Qt::PreciseTimer);
    connect(&dragTimer_, &QTimer::timeout, this, &EchoEditScript::dragMove);

    // Every end of a mission transfer and every mission read from the autopilot, for "waitMissionTransfer" and the log.
    DeviceManagerWrapper* dmw = core_ ? core_->getDeviceManagerWrapperPtr() : nullptr;
    mission::MissionPlanController* plan = core_ ? core_->getMissionPlanControllerPtr() : nullptr;
    if (dmw) {
        auto ended = [this](const char* kind, bool ok, int result, int type) {
            const QString line = QStringLiteral("mission transfer: %1 %2 result %3 type %4")
                                 .arg(QLatin1String(kind), ok ? QStringLiteral("ok") : QStringLiteral("failed")).arg(result).arg(type);
            missionResults_.append(line);
            qInfo().noquote() << QStringLiteral("echoedit script: ") + line;
        };
        connect(dmw, &DeviceManagerWrapper::missionUploadFinished, this, [ended](bool ok, int r, int t) { ended("upload", ok, r, t); });
        connect(dmw, &DeviceManagerWrapper::missionDownloadFinished, this, [ended](bool ok, int r, int t) { ended("download", ok, r, t); });
    }
    if (plan) {
        connect(plan, &mission::MissionPlanController::vehicleMissionReceived, this,
                [](bool empty, bool matches, int points, int fence, int rally, int skipped) {
            qInfo().noquote() << QStringLiteral("echoedit script: mission received: empty %1, matches the plan %2, route points %3, "
                                                "fence %4, rally %5, skipped %6")
                                 .arg(int(empty)).arg(int(matches)).arg(points).arg(fence).arg(rally).arg(skipped);
        });
    }
}

void EchoEditScript::armFromEnvironment()
{
    const QString path = qEnvironmentVariable("KOGGER_ECHOEDIT_SCRIPT");
    if (path.isEmpty() || !core_) {
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning().noquote() << QStringLiteral("echoedit script: %1 not read: %2").arg(path, f.errorString());
        return;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError) {
        qWarning().noquote() << QStringLiteral("echoedit script: %1 is not JSON: %2").arg(path, pe.errorString());
        return;
    }
    steps_ = doc.object().value(QStringLiteral("steps")).toArray();
    live_ = doc.object().value(QStringLiteral("live")).toBool(false);

    Dataset* ds = core_->getDatasetPtr();
    connect(ds, &Dataset::bottomTrackUpdated, this, [this]() { quiet_.restart(); });
    quiet_.start();
    qInfo().noquote() << QStringLiteral("echoedit script: armed, %1 steps from %2").arg(steps_.size()).arg(path);
    timer_.start(500);
}

void EchoEditScript::step()
{
    if (next_ >= steps_.size()) {
        return;
    }
    // A recording open and read: the steps are about its epochs (a live script runs on the devices instead).
    if (!live_ && (core_->property("openedFilePath").toString().isEmpty() || core_->getIsFileOpening()
                   || core_->getDatasetPtr()->size() <= 0)) {
        timer_.start(500);
        return;
    }

    const QJsonObject s = steps_.at(next_).toObject();
    const QString op = s.value(QStringLiteral("op")).toString();

    if (op == QLatin1String("waitUpgrade")) {
        // Until the update the "upgrade" step started ends: its device's upgradeFWStatus 101 (flashed) or -1
        // (failed); the progress in steps of 10 %.
        if (!settleClock_.isValid()) {
            settleClock_.start();
            upgradeLogged_ = -10;
        }
        const int st = upgradeDev_ ? upgradeDev_->property("upgradeFWStatus").toInt() : -1;
        if (st >= 0 && st <= 100 && st / 10 > upgradeLogged_ / 10) {
            upgradeLogged_ = st;
            qInfo().noquote() << QStringLiteral("echoedit script: waitUpgrade, %1 %, %2 ms").arg(st).arg(settleClock_.elapsed());
        }
        const bool timedOut = settleClock_.elapsed() >= s.value(QStringLiteral("timeoutMs")).toInt(180000);
        if (!upgradeDev_ || st == 101 || st == -1 || timedOut) {
            qInfo().noquote() << QStringLiteral("echoedit script: waitUpgrade, %1 ms: %2").arg(settleClock_.elapsed())
                                 .arg(!upgradeDev_ ? QStringLiteral("no update running")
                                      : st == 101 ? QStringLiteral("flashed")
                                      : st == -1 ? QStringLiteral("failed")
                                      : QStringLiteral("timed out at %1 %").arg(st));
            settleClock_.invalidate();
            ++next_;
            timer_.start(0);
            return;
        }
        timer_.start(250);
        return;
    }

    if (op == QLatin1String("waitServo")) {
        if (!settleClock_.isValid()) {
            settleClock_.start();
        }
        const bool known = firstServoDevice() != nullptr;
        const bool timedOut = settleClock_.elapsed() >= s.value(QStringLiteral("timeoutMs")).toInt(20000);
        if (known || timedOut) {
            qInfo().noquote() << QStringLiteral("echoedit script: waitServo, %1 ms%2")
                                 .arg(settleClock_.elapsed()).arg(known ? QString() : QStringLiteral(", timed out"));
            settleClock_.invalidate();
            ++next_;
            timer_.start(0);
            return;
        }
        timer_.start(200);
        return;
    }

    if (op == QLatin1String("servoDrag")) {
        QObject* dev = firstServoDevice();
        const double from = s.value(QStringLiteral("from")).toDouble();
        const double to = s.value(QStringLiteral("to")).toDouble();
        const int ms = s.value(QStringLiteral("ms")).toInt(1000);
        if (!settleClock_.isValid()) {
            settleClock_.start();
            servoDragMoves_ = 0;
        }
        const qint64 t = settleClock_.elapsed();
        if (dev && t < ms) {
            QMetaObject::invokeMethod(dev, "servoManualMove", Q_ARG(double, from + (to - from) * double(t) / ms));
            ++servoDragMoves_;
            timer_.start(qMax(1, s.value(QStringLiteral("everyMs")).toInt(5)));
            return;
        }
        if (dev) {
            QMetaObject::invokeMethod(dev, "servoManualSet", Q_ARG(double, to));
        }
        qInfo().noquote() << QStringLiteral("echoedit script: servoDrag %1..%2 in %3 ms: %4")
                             .arg(from).arg(to).arg(t)
                             .arg(dev ? QStringLiteral("%1 moves, then set %2").arg(servoDragMoves_).arg(to)
                                      : QStringLiteral("no servo device"));
        settleClock_.invalidate();
        ++next_;
        timer_.start(0);
        return;
    }

    if (op == QLatin1String("waitPresets")) {
        if (!settleClock_.isValid()) {
            settleClock_.start();
        }
        bool known = false;
        for (QObject* d : core_->getDeviceManagerWrapperPtr()->getDevList()) {
            known = known || d->property("presetsKnown").toBool();
        }
        const bool timedOut = settleClock_.elapsed() >= s.value(QStringLiteral("timeoutMs")).toInt(20000);
        if (known || timedOut) {
            qInfo().noquote() << QStringLiteral("echoedit script: waitPresets, %1 ms%2")
                                 .arg(settleClock_.elapsed()).arg(known ? QString() : QStringLiteral(", timed out"));
            settleClock_.invalidate();
            ++next_;
            timer_.start(0);
            return;
        }
        timer_.start(200);
        return;
    }

    if (op == QLatin1String("waitAutopilot") || op == QLatin1String("waitMissionTransfer")) {
        if (!settleClock_.isValid()) {
            settleClock_.start();
        }
        DeviceManagerWrapper* dmw = core_->getDeviceManagerWrapperPtr();
        const bool done = op == QLatin1String("waitAutopilot")
                              ? dmw->autopilotOnline()
                              : (missionResults_.size() > missionResultsBefore_ && !dmw->missionTransferActive());
        const bool timedOut = settleClock_.elapsed() >= s.value(QStringLiteral("timeoutMs")).toInt(op == QLatin1String("waitAutopilot") ? 30000 : 60000);
        if (done || timedOut) {
            qInfo().noquote() << QStringLiteral("echoedit script: %1, %2 ms%3").arg(op).arg(settleClock_.elapsed())
                                 .arg(done ? QString() : QStringLiteral(", timed out"));
            settleClock_.invalidate();
            ++next_;
            timer_.start(0);
            return;
        }
        timer_.start(200);
        return;
    }

    if (op == QLatin1String("settle")) {
        if (!settleClock_.isValid()) {
            settleClock_.start();
        }
        const int quietMs = s.value(QStringLiteral("quietMs")).toInt(3000);
        const int timeoutMs = s.value(QStringLiteral("timeoutMs")).toInt(180000);
        const bool trackThere = core_->getDatasetPtr()->getLastBottomTrackEpoch() > 0;
        const bool timedOut = settleClock_.elapsed() >= timeoutMs;
        if ((trackThere && quiet_.elapsed() >= quietMs) || timedOut) {
            qInfo().noquote() << QStringLiteral("echoedit script: settle, %1 ms%2")
                                 .arg(settleClock_.elapsed()).arg(timedOut ? QStringLiteral(", timed out") : QString());
            settleClock_.invalidate();
            ++next_;
            timer_.start(0);
            return;
        }
        timer_.start(200);
        return;
    }
    if (op == QLatin1String("wait")) {
        if (!settleClock_.isValid()) {
            settleClock_.start();
        }
        if (settleClock_.elapsed() < s.value(QStringLiteral("ms")).toInt(1000)) {
            timer_.start(100);
            return;
        }
        settleClock_.invalidate();
        qInfo().noquote() << QStringLiteral("echoedit script: wait done");
        ++next_;
        timer_.start(0);
        return;
    }
    if (op == QLatin1String("drag")) {
        if (!settleClock_.isValid()) {
            if (!startGesture(s)) {
                ++next_;
                timer_.start(0);
                return;
            }
            settleClock_.start();
        }
        const int wait = gestureStep(s);
        if (wait >= 0) {
            timer_.start(wait);
            return;
        }
        settleClock_.invalidate();
        ++next_;
        timer_.start(300);
        return;
    }
    if (op == QLatin1String("relayout")) {
        if (!settleClock_.isValid()) {
            if (!startRelayout(s)) {
                ++next_;
                timer_.start(0);
                return;
            }
            settleClock_.start();
        }
        if (relayoutStep()) {
            timer_.start(relayoutPending_ ? 20 : relayoutIntervalMs_);
            return;
        }
        settleClock_.invalidate();
        finishRelayout();
        ++next_;
        timer_.start(0);
        return;
    }
    if (op == QLatin1String("drag3d")) {
        if (!settleClock_.isValid()) {
            if (!startDrag3d(s)) {
                ++next_;
                timer_.start(0);
                return;
            }
            settleClock_.start();
        }
        if (settleClock_.elapsed() < qint64(s.value(QStringLiteral("seconds")).toDouble(20.0) * 1000.0)) {
            timer_.start(100);
            return;
        }
        settleClock_.invalidate();
        finishDrag3d();
        ++next_;
        timer_.start(0);
        return;
    }

    runStep(s);
    ++next_;
    if (op != QLatin1String("quit")) {
        timer_.start(50);
    }
}

void EchoEditScript::runStep(const QJsonObject& s)
{
    Dataset* ds = core_->getDatasetPtr();
    EchogramEditor* ed = core_->getEchogramEditorPtr();
    const QString op = s.value(QStringLiteral("op")).toString();
    // An epoch as a number, or as a share of the recording ("fromFrac": 0.4): a script for any file.
    const int n = ds->size();
    auto epochOf = [&](const char* key, int fallback) {
        const QString k = QString::fromLatin1(key);
        if (s.contains(k + QStringLiteral("Frac"))) {
            return std::clamp(int(std::lround(s.value(k + QStringLiteral("Frac")).toDouble() * n)), 0, n);
        }
        return s.value(k).toInt(fallback);
    };
    const int from = epochOf("from", 0);
    const int to = s.contains(QStringLiteral("width")) ? from + s.value(QStringLiteral("width")).toInt(0) : epochOf("to", 0);
    float depth = float(s.value(QStringLiteral("depth")).toDouble(NAN));
    if (s.contains(QStringLiteral("depthRel"))) {
        // A share of the median bottom over the stretch: one depth for every ping of it, as a hand
        // drawing a line would give.
        const QVector<DatasetChannel> channels = ds->channelsList();
        QVector<float> d;
        for (int e = std::max(0, from); e < std::min(to, n) && !channels.isEmpty(); ++e) {
            if (Epoch* ep = ds->fromIndex(e); ep) {
                const double v = ep->distProccesing(channels.first().channelId_);
                if (std::isfinite(v)) {
                    d.append(float(v));
                }
            }
        }
        std::sort(d.begin(), d.end());
        depth = d.isEmpty() ? NAN : d.at(d.size() / 2) * float(s.value(QStringLiteral("depthRel")).toDouble());
    }

    if (dockStep(op, s) || missionStep(op, s)) {
        return;
    }
    if (op == QLatin1String("draw") || op == QLatin1String("erase") || op == QLatin1String("raise") || op == QLatin1String("lower")) {
        if (op != QLatin1String("erase") && !std::isfinite(depth)) {
            qWarning().noquote() << QStringLiteral("echoedit script: %1 %2..%3 has no depth, skipped").arg(op).arg(from).arg(to);
            return;
        }
        stroke(op, from, to, depth);
        qInfo().noquote() << QStringLiteral("echoedit script: %1 at %2 m").arg(op).arg(depth);
    }
    else if (op == QLatin1String("mark") || op == QLatin1String("unmark")) {
        ed->beginStroke(op == QLatin1String("mark") ? echoedit::Command::Mark : echoedit::Command::Unmark);
        ed->markRange(from, to, op == QLatin1String("mark"));
        ed->endStroke();
    }
    else if (op == QLatin1String("smooth") || op == QLatin1String("fillGaps")) {
        // A stroke of the brush over the stretch, as Plot2D gives the pings it sweeps.
        const bool smooth = op == QLatin1String("smooth");
        const int passes = smooth ? std::max(1, s.value(QStringLiteral("passes")).toInt(1)) : 1;
        ed->beginStroke(smooth ? echoedit::Command::Smooth : echoedit::Command::FillGaps);
        for (int k = 0; k < passes; ++k) {
            if (smooth) {
                ed->smoothRange(from, to);
            }
            else {
                ed->fillGapsRange(from, to);
            }
        }
        ed->endStroke();
    }
    else if (op == QLatin1String("mosaicOut")) {
        const QVector<DatasetChannel> channels = ds->channelsList();
        const int side = s.value(QStringLiteral("side")).toInt(0);
        if (side < 0 || side >= channels.size()) {
            qWarning().noquote() << QStringLiteral("echoedit script: mosaicOut has no side %1, skipped").arg(side);
            return;
        }
        ed->beginStroke(echoedit::Command::MosaicOut);
        ed->setMosaicOut(from, to, channels.at(side).channelId_.address, channels.at(side).subChannelId_, true);
        ed->endStroke();
    }
    else if (op == QLatin1String("despike") || op == QLatin1String("turns")) {
        const int pings = op == QLatin1String("despike") ? ed->removeSpikes() : ed->turnsOutOfMosaic();
        results_.insert(op, pings);
        qInfo().noquote() << QStringLiteral("echoedit script: %1 changed %2 pings").arg(op).arg(pings);
    }
    else if (op == QLatin1String("spike")) {
        // Not an edit: what a bad ping of the bottom track looks like, for "despike" to find.
        const double factor = s.value(QStringLiteral("factor")).toDouble(1.5);
        int moved = 0;
        for (int e = std::max(0, from); e < std::min(to, n); ++e) {
            echoedit::PingState st = ds->pingEditState(e);
            bool any = false;
            for (echoedit::ChannelBottom& b : st.channels) {
                if (std::isfinite(b.distance)) {
                    b.distance = float(b.distance * factor);
                    b.source = uint8_t(Epoch::DistProcessing::DistanceSource::DistanceSourceProcessing);
                    any = true;
                }
            }
            if (any) {
                ds->setPingEditState(e, st);
                ++moved;
            }
        }
        emit ds->dataUpdate();
        results_.insert(QStringLiteral("spiked"), results_.value(QStringLiteral("spiked")).toInt(0) + moved);
        qInfo().noquote() << QStringLiteral("echoedit script: spike, %1 pings x%2").arg(moved).arg(factor);
    }
    else if (op == QLatin1String("trimStart") || op == QLatin1String("trimEnd")) {
        ed->beginStroke(op == QLatin1String("trimStart") ? echoedit::Command::TrimStart : echoedit::Command::TrimEnd);
        ed->moveTrim(epochOf("epoch", 0));
        ed->endStroke();
    }
    else if (op == QLatin1String("clearTrim")) {
        ed->clearTrim();
    }
    else if (op == QLatin1String("resetAll")) {
        ed->resetAll();
    }
    else if (op == QLatin1String("undo")) {
        ed->undo();
    }
    else if (op == QLatin1String("redo")) {
        ed->redo();
    }
    else if (op == QLatin1String("undoAll")) {
        while (ed->canUndo()) {
            ed->undo();
        }
    }
    else if (op == QLatin1String("redoAll")) {
        while (ed->canRedo()) {
            ed->redo();
        }
    }
    else if (op == QLatin1String("undoTo")) {
        const int steps = std::max(0, s.value(QStringLiteral("steps")).toInt(0));
        while (ed->undoCount() > steps) {
            ed->undo();
        }
    }
    else if (op == QLatin1String("flush")) {
        ed->flush();
    }
    else if (op == QLatin1String("magnetProbe")) {
        probe(s.value(QStringLiteral("name")).toString(QStringLiteral("probe")), from, to,
              float(s.value(QStringLiteral("offset")).toDouble(0.3)), float(s.value(QStringLiteral("radius")).toDouble(0.5)));
    }
    else if (op == QLatin1String("exportCsv")) {
        const QString dir = s.value(QStringLiteral("dir")).toString();
        const QVector<DatasetChannel> channels = ds->channelsList();
        QDir().mkpath(dir);
        const bool ok = !channels.isEmpty() && core_->exportPlotAsCVS(dir, channels.first().channelId_, 0);
        QString file;
        for (const QString& name : QDir(dir).entryList({ QStringLiteral("*_export_csv.csv") }, QDir::Files, QDir::Time)) {
            file = QDir(dir).filePath(name);   // the newest
            break;
        }
        int rows = -1;
        QFile f(file);
        if (ok && f.open(QIODevice::ReadOnly)) {
            rows = 0;
            while (!f.atEnd()) {
                f.readLine();
                ++rows;
            }
            rows -= 1;   // the header
        }
        export_ = QJsonObject{ { QStringLiteral("ok"), ok }, { QStringLiteral("file"), file }, { QStringLiteral("rows"), rows } };
        qInfo().noquote() << QStringLiteral("echoedit script: exportCsv %1 rows to %2").arg(rows).arg(file);
    }
    else if (op == QLatin1String("dump")) {
        dump(s.value(QStringLiteral("path")).toString());
    }
    else if (op == QLatin1String("columns")) {
        // Echo columns around the bottom track's bottom, for working out the magnet offline: "count"
        // pings spread over [from, to), each channel's samples within "halfM" of the bottom.
        const QVector<DatasetChannel> channels = ds->channelsList();
        const int count = std::max(1, s.value(QStringLiteral("count")).toInt(200));
        const float halfM = float(s.value(QStringLiteral("halfM")).toDouble(1.5));
        QJsonArray cols;
        for (int k = 0; k < count && !channels.isEmpty(); ++k) {
            const int e = from + int((qint64(to - from) * k) / count);
            Epoch* ep = ds->fromIndex(e);
            const Epoch::DistProcessing* bp = ep ? ep->bottomProcessingOf(channels.first().channelId_, channels.first().subChannelId_) : nullptr;
            if (!bp || !std::isfinite(bp->distance) || ds->isEpochExcluded(e)) {
                continue;
            }
            QJsonObject col{ { QStringLiteral("epoch"), e }, { QStringLiteral("bottom"), bp->distance } };
            QJsonArray chans;
            for (const DatasetChannel& ch : channels) {
                const Epoch::Echogram* g = ep->chart(ch.channelId_, ch.subChannelId_);
                if (!g || !(g->resolution > 0.0f)) {
                    continue;
                }
                const int i0 = std::max(0, int((bp->distance - halfM - g->offset) / g->resolution));
                const int i1 = std::min(int(g->amplitude.size()), int((bp->distance + halfM - g->offset) / g->resolution));
                QJsonArray a;
                for (int i = i0; i < i1; ++i) {
                    a.append(int(g->amplitude.at(i)));
                }
                chans.append(QJsonObject{ { QStringLiteral("address"), int(ch.channelId_.address) },
                                          { QStringLiteral("resolution"), g->resolution }, { QStringLiteral("offset"), g->offset },
                                          { QStringLiteral("first"), i0 }, { QStringLiteral("samples"), a } });
            }
            col.insert(QStringLiteral("channels"), chans);
            cols.append(col);
        }
        QSaveFile out(s.value(QStringLiteral("path")).toString());
        if (out.open(QIODevice::WriteOnly)) {
            out.write(QJsonDocument(cols).toJson(QJsonDocument::Compact));
            out.commit();
        }
        qInfo().noquote() << QStringLiteral("echoedit script: columns, %1 written").arg(cols.size());
    }
    else if (op == QLatin1String("view")) {
        // The echograms at a place of the recording, the palette open or not, a tool chosen: what a
        // "grab" then shows.
        if (s.contains(QStringLiteral("epoch")) || s.contains(QStringLiteral("epochFrac"))) {
            core_->setTimelinePosition(n > 1 ? double(epochOf("epoch", 0)) / double(n - 1) : 1.0);
        }
        if (s.contains(QStringLiteral("tool"))) {
            core_->setBottomTrackEditTool(s.value(QStringLiteral("tool")).toInt(0));
        }
        if (s.contains(QStringLiteral("palette"))) {
            // The workspace store of the main window (a QML object): the palette's switch is its property.
            QObject* store = nullptr;
            for (QWindow* w : QGuiApplication::topLevelWindows()) {
                for (QObject* o : w->findChildren<QObject*>()) {
                    if (o->property("bottomTrackEditorOpen").isValid()) {
                        store = o;
                        break;
                    }
                }
                if (store) {
                    break;
                }
            }
            if (store) {
                store->setProperty("bottomTrackEditorOpen", s.value(QStringLiteral("palette")).toBool());
            }
            else {
                qWarning().noquote() << QStringLiteral("echoedit script: no object with bottomTrackEditorOpen to open the palette");
            }
        }
    }
    else if (op == QLatin1String("hscale")) {
        // Every echogram's horizontal scale, as a restore sets it (no scale hint).
        const double scale = s.value(QStringLiteral("scale")).toDouble(1.0);
        int plots = 0;
        int visible = 0;
        forEachPlot(false, [&](QQuickItem* plot) {
            if (plot->setProperty("horizontalScale", scale)) {
                ++plots;
                visible += plot->isVisible() && plot->width() > 0 ? 1 : 0;
            }
        });
        qInfo().noquote() << QStringLiteral("echoedit script: hscale %1 on %2 echogram(s), %3 visible; %4 pings")
                                 .arg(scale).arg(plots).arg(visible).arg(n);
    }
    else if (op == QLatin1String("hzoom") || op == QLatin1String("vzoom")) {
        // A zoom as the user makes it on the echograms on screen, which then show their scale hint: hzoom steps
        // the horizontal scale as the settings panel does, vzoom turns the range as Ctrl + wheel does.
        const bool across = op == QLatin1String("hzoom");
        const int amount = across ? s.value(QStringLiteral("steps")).toInt(1) : s.value(QStringLiteral("delta")).toInt(120);
        int plots = 0;
        forEachPlot(true, [&](QQuickItem* plot) {
            if (across) {
                QMetaObject::invokeMethod(plot, "stepHorizontalScaleBy", Q_ARG(int, amount));
            }
            else {
                QMetaObject::invokeMethod(plot, "verZoomEvent", Q_ARG(int, amount));
            }
            ++plots;
        });
        qInfo().noquote() << QStringLiteral("echoedit script: %1 %2 on %3 echogram(s)").arg(op).arg(amount).arg(plots);
    }
    else if (op == QLatin1String("hint")) {
        // What the scale hint of each echogram on screen says, and the numbers it is made from.
        forEachPlot(true, [&](QQuickItem* plot) {
            QQuickItem* hint = findWithProperty(plot, "acrossText");
            if (!hint) {
                return;
            }
            QVariantMap span;
            float from = 0.0f;
            float to = 0.0f;
            QMetaObject::invokeMethod(plot, "visibleSpan", Q_RETURN_ARG(QVariantMap, span));
            QMetaObject::invokeMethod(plot, "cursorFrom", Q_RETURN_ARG(float, from));
            QMetaObject::invokeMethod(plot, "cursorTo", Q_RETURN_ARG(float, to));
            qInfo().noquote() << QStringLiteral("echoedit script: hint across %1 opacity %2 [%3] [%4]; scale %5 pings %6"
                                                " columns %7 from %8 to %9")
                                     .arg(hint->property("across").toBool() ? 1 : 0)
                                     .arg(hint->opacity(), 0, 'f', 2)
                                     .arg(hint->property("acrossText").toString(), hint->property("rangeText").toString())
                                     .arg(plot->property("horizontalScale").toDouble())
                                     .arg(span.value(QStringLiteral("pings")).toInt())
                                     .arg(span.value(QStringLiteral("columns")).toInt())
                                     .arg(double(from), 0, 'g', 9).arg(double(to), 0, 'g', 9);
        });
    }
    else if (op == QLatin1String("pin") || op == QLatin1String("pinuse") || op == QLatin1String("pinstate")) {
        // Pinned settings (PinRegistry, SettingsWidgetPopup). pin: a setting pinned as pin mode's pin does it;
        // pinuse: a settings widget's mirror control used as a hand would (a switch clicked, a spin box stepped
        // up); pinstate: the setting's row controls on the settings page and their mirrors, logged side by side.
        const QString key = s.value(QStringLiteral("key")).toString();
        QObject* store = findObjectWithProperty("settingsPinTarget");
        QQmlEngine* eng = store ? qmlEngine(store) : nullptr;
        QObject* reg = eng ? eng->singletonInstance<QObject*>("kqml_types", "PinRegistry") : nullptr;
        if (!store || !reg) {
            qWarning().noquote() << QStringLiteral("echoedit script: %1: no store or no PinRegistry").arg(op);
        }
        else if (op == QLatin1String("pin") && s.contains(QStringLiteral("mode"))) {
            // Pin mode itself: the settings pane open at a group ("group"), pins shown; or closed.
            const bool on = s.value(QStringLiteral("mode")).toBool();
            if (on) {
                const QString group = s.value(QStringLiteral("group")).toString();
                if (!group.isEmpty()) {
                    QMetaObject::invokeMethod(store, "openAppSettingsAtGroup", Q_ARG(QVariant, group));
                }
                reg->setProperty("pinMode", true);
            }
            else {
                store->setProperty("settingsPanelOpen", false);
            }
            qInfo().noquote() << QStringLiteral("echoedit script: pin mode %1").arg(on ? QStringLiteral("on") : QStringLiteral("off"));
        }
        else if (op == QLatin1String("pin")) {
            QMetaObject::invokeMethod(store, "togglePinnedSetting", Q_ARG(QVariant, key),
                                      Q_ARG(QVariant, s.value(QStringLiteral("label")).toString()));
            QVariant map = reg->property("pinnedMap");
            qInfo().noquote() << QStringLiteral("echoedit script: pin %1 -> widget %2").arg(key, map.toMap().value(key).toString());
        }
        else {
            auto controls = [&](QObject* block) {
                QVariant v;
                QMetaObject::invokeMethod(reg, "controlsOf", Q_RETURN_ARG(QVariant, v), Q_ARG(QVariant, QVariant::fromValue(block)));
                QList<QObject*> out;
                const QVariantList list = v.toList();
                for (const QVariant& e : list) {
                    out.append(e.value<QObject*>());
                }
                return out;
            };
            QVariant tagV;
            QMetaObject::invokeMethod(reg, "tagOf", Q_RETURN_ARG(QVariant, tagV), Q_ARG(QVariant, key));
            QObject* tag = tagV.value<QObject*>();
            QObject* block = tag ? tag->property("block").value<QObject*>() : nullptr;
            const QList<QObject*> rowCtl = block ? controls(block) : QList<QObject*>();
            QQuickItem* mrow = findMirrorRow(key);
            const QList<QObject*> mirCtl = mrow ? controls(mrow) : QList<QObject*>();
            if (op == QLatin1String("pinuse")) {
                const int index = s.value(QStringLiteral("index")).toInt(0);
                QObject* c = (index >= 0 && index < mirCtl.size()) ? mirCtl.at(index) : nullptr;
                const QString kind = c ? c->property("controlKind").toString() : QString();
                if (kind == QLatin1String("switch")) {
                    QMetaObject::invokeMethod(c, "click");
                }
                else if (kind == QLatin1String("spin")) {
                    QMetaObject::invokeMethod(c, "increment");
                }
                qInfo().noquote() << QStringLiteral("echoedit script: pinuse %1 #%2 %3").arg(key).arg(index)
                                         .arg(kind.isEmpty() ? QStringLiteral("no mirror control") : kind);
            }
            QStringList pairs;
            const int n = std::max(rowCtl.size(), mirCtl.size());
            for (int i = 0; i < n; ++i) {
                QObject* r = i < rowCtl.size() ? rowCtl.at(i) : nullptr;
                QObject* m = i < mirCtl.size() ? mirCtl.at(i) : nullptr;
                pairs << QStringLiteral("%1 row %2 mirror %3")
                             .arg((r ? r : m) ? (r ? r : m)->property("controlKind").toString() : QStringLiteral("?"),
                                  controlValue(r), controlValue(m));
            }
            qInfo().noquote() << QStringLiteral("echoedit script: pinstate %1: row %2, mirror %3; %4")
                                     .arg(key, block ? QStringLiteral("found") : QStringLiteral("none"),
                                          mrow ? QStringLiteral("found") : QStringLiteral("none"), pairs.join(QStringLiteral("; ")));
        }
    }
    else if (op == QLatin1String("plotState")) {
        // Each echogram's panel state (Plot2D.qml's echoViewState): "set" changes it as the panel's controls do;
        // then a line an echogram with what the controls and the plot itself say.
        const QJsonObject set = s.value(QStringLiteral("set")).toObject();
        forEachPlot(false, [&](QQuickItem* plot) {
            QQuickItem* st = findWithProperty(plot, "bottomTrackLine");
            if (!st) {
                return;
            }
            for (auto it = set.begin(); it != set.end(); ++it) {
                st->setProperty(it.key().toUtf8().constData(), it.value().toVariant());
            }
            int btTheme = -1;
            int rfTheme = -1;
            bool btVisible = false;
            QMetaObject::invokeMethod(plot, "getBottomTrackThemeId", Q_RETURN_ARG(int, btTheme));
            QMetaObject::invokeMethod(plot, "getRangefinderThemeId", Q_RETURN_ARG(int, rfTheme));
            QMetaObject::invokeMethod(plot, "getBottomTrackVisible", Q_RETURN_ARG(bool, btVisible));
            qInfo().noquote() << QStringLiteral("echoedit script: plotState plot %1%2: bottomTrackLine %3 bottomTrackValue %4 "
                                                "bottomTrackTheme %5 rangefinderLine %6; plot: bottom track visible %7 theme %8, "
                                                "rangefinder theme %9")
                                 .arg(plot->property("indx").toInt()).arg(plot->isVisible() ? QString() : QStringLiteral(" (hidden)"))
                                 .arg(st->property("bottomTrackLine").toBool()).arg(st->property("bottomTrackValue").toBool())
                                 .arg(st->property("bottomTrackTheme").toInt()).arg(st->property("rangefinderLine").toBool())
                                 .arg(btVisible).arg(btTheme).arg(rfTheme);
        });
    }
    else if (op == QLatin1String("presets")) {
        for (QObject* d : core_->getDeviceManagerWrapperPtr()->getDevList()) {
            QStringList slotLines;
            for (const QVariant& v : d->property("presetSlots").toList()) {
                const QVariantMap m = v.toMap();
                const int slot = m.value(QStringLiteral("slot")).toInt();
                slotLines.append(m.value(QStringLiteral("used")).toBool()
                    ? QStringLiteral("%1: %2 kHz %3 pulses \"").arg(slot).arg(m.value(QStringLiteral("freqKhz")).toInt())
                          .arg(m.value(QStringLiteral("pulses")).toInt()) + m.value(QStringLiteral("name")).toString() + QStringLiteral("\"")
                    : QStringLiteral("%1: empty").arg(slot));
            }
            qInfo().noquote() << QStringLiteral("echoedit script: presets of %1: known %2; ")
                                 .arg(d->property("devName").toString()).arg(d->property("presetsKnown").toBool())
                                 + slotLines.join(QStringLiteral(", "));
        }
    }
    else if (op == QLatin1String("upgrade")) {
        // A firmware file into a sonar as its device page's UPGRADE does (Core::upgradeFW; behind a recorder the
        // update through it): the first sonar, or the one whose ID_VERSION board is "board" at the route "addr".
        const int board = s.value(QStringLiteral("board")).toInt(-1);
        const int addr = s.value(QStringLiteral("addr")).toInt(-1);
        QObject* dev = nullptr;
        for (QObject* d : core_->getDeviceManagerWrapperPtr()->getDevList()) {
            if (!dev && d->property("isSonar").toBool() && (board < 0 || d->property("devType").toInt() == board)
                && (addr < 0 || d->property("busAddress").toInt() == addr)) {
                dev = d;
            }
        }
        const QString path = s.value(QStringLiteral("path")).toString();
        // Who it is, read before the update starts: the update resets what the sonar reported
        const QString who = !dev ? QString()
            : QStringLiteral("%1 (board %2, route %3, firmware %4, behind a recorder %5)")
                  .arg(dev->property("devName").toString()).arg(dev->property("devType").toInt())
                  .arg(dev->property("busAddress").toInt()).arg(dev->property("fwVersion").toString())
                  .arg(dev->property("isBehindRecorder").toBool());
        // Not while a recorder records: through one it answers the bootloader's route 0 itself (HANDOFF, "A sonar
        // behind a recorder"), and what its firmware does with that while writing is not known.
        QString recording;
        for (QObject* d : core_->getDeviceManagerWrapperPtr()->getDevList()) {
            if (d->property("recorderStatusValid").toBool() && d->property("recorderRecordingState").toInt() == 2) {
                recording = d->property("devName").toString();
            }
        }
        const bool started = dev && recording.isEmpty() && core_->upgradeFW(path, dev);
        upgradeDev_ = started ? dev : nullptr;
        qInfo().noquote() << QStringLiteral("echoedit script: upgrade %1: %2").arg(path,
            !dev ? QStringLiteral("no such sonar")
                 : who + (started ? QStringLiteral(": started")
                          : !recording.isEmpty() ? QStringLiteral(": not started, %1 records").arg(recording)
                          : QStringLiteral(": the file did not open")));
    }
    else if (op == QLatin1String("clickItem")) {
        // A tap on the "index"-th item (window order) whose objectName is "name": its clicked() as its MouseArea
        // emits it -- a device badge (devBadgeButton) opens its pill.
        const QString name = s.value(QStringLiteral("name")).toString();
        const int index = s.value(QStringLiteral("index")).toInt(0);
        QList<QQuickItem*> found;
        for (QWindow* w : QGuiApplication::topLevelWindows()) {
            if (auto* qw = qobject_cast<QQuickWindow*>(w)) {
                std::function<void(QQuickItem*)> walk = [&](QQuickItem* it) {
                    if (it->objectName() == name) {
                        found.append(it);
                    }
                    for (QQuickItem* c : it->childItems()) {
                        walk(c);
                    }
                };
                walk(qw->contentItem());
            }
        }
        const bool clicked = index >= 0 && index < found.size()
                             && QMetaObject::invokeMethod(found.at(index), "clicked");
        qInfo().noquote() << QStringLiteral("echoedit script: clickItem %1 #%2 of %3: %4").arg(name).arg(index)
                             .arg(found.size()).arg(clicked ? QStringLiteral("clicked") : QStringLiteral("not clicked"));
    }
    else if (op == QLatin1String("recorderState")) {
        // Every recorder's status as the app has it (ID_RECORDER_STATUS; recording state 2 = recording).
        for (QObject* d : core_->getDeviceManagerWrapperPtr()->getDevList()) {
            if (d->property("recorderStatusValid").toBool()) {
                qInfo().noquote() << QStringLiteral("echoedit script: recorderState %1: state %2 mode %3 condition %4 flags 0x%5")
                                     .arg(d->property("devName").toString()).arg(d->property("recorderRecordingState").toInt())
                                     .arg(d->property("recorderRecordingMode").toInt())
                                     .arg(d->property("recorderDeviceCondition").toInt())
                                     .arg(d->property("recorderStatusFlags").toInt(), 0, 16);
            }
        }
    }
    else if (op == QLatin1String("devState")) {
        // A sonar's settings as the app has them (TRANSC, CHART_SETUP, DIST_SETUP, DATASET, DSP, sound speed, UART):
        // the first sonar, or the one of "board" at "addr" -- to compare before and after an update.
        const int board = s.value(QStringLiteral("board")).toInt(-1);
        const int addr = s.value(QStringLiteral("addr")).toInt(-1);
        QObject* dev = nullptr;
        for (QObject* d : core_->getDeviceManagerWrapperPtr()->getDevList()) {
            if (!dev && d->property("isSonar").toBool() && (board < 0 || d->property("devType").toInt() == board)
                && (addr < 0 || d->property("busAddress").toInt() == addr)) {
                dev = d;
            }
        }
        const auto v = [dev](const char* name) { return dev->property(name).toInt(); };
        qInfo().noquote() << (!dev ? QStringLiteral("echoedit script: devState: no such sonar")
            : QStringLiteral("echoedit script: devState %1 (board %2, route %3): trans %4 kHz %5 pulses boost %6; chart %7 x %8 mm "
                             "offset %9; ").arg(dev->property("devName").toString()).arg(v("devType")).arg(v("busAddress"))
                  .arg(v("transFreq")).arg(v("transPulse")).arg(v("transBoost")).arg(v("chartSamples")).arg(v("chartResolution"))
                  .arg(v("chartOffset"))
              + QStringLiteral("dist max %1 dead %2 confidence %3; dataset chart %4 chartZ %5 dist %6 temp %7 voltage %8 "
                               "euler %9; ").arg(v("distMax")).arg(v("distDeadZone")).arg(v("distConfidence"))
                  .arg(v("datasetChart")).arg(v("datasetChartZ")).arg(v("datasetDist")).arg(v("datasetTemp"))
                  .arg(v("datasetVoltage")).arg(v("datasetEuler"))
              + QStringLiteral("period %1/%2 ms; dsp smooth %3; sound %4; baud %5").arg(v("ch1Period")).arg(v("ch2Period"))
                  .arg(v("dspHorSmooth")).arg(v("soundSpeed")).arg(v("baudrate")));
    }
    else if (op == QLatin1String("wifiPage") || op == QLatin1String("wifiRadio")) {
        // The first Wi-Fi module, or the one at "addr" (its bus address: the head unit's 87, the boat's 88 behind it).
        // wifiPage: its device page opened, as a tap on it in the device list does. wifiRadio: its radio switched off
        // ({"off": seconds}) or on ({"on": true}) through DevDriver as the page does; with neither, its radio state as
        // the page reads it (docs/WIFI_MODULE.md 2o).
        const int addr = s.value(QStringLiteral("addr")).toInt(-1);
        QObject* dev = nullptr;
        int index = -1, i = 0;
        for (QObject* d : core_->getDeviceManagerWrapperPtr()->getDevList()) {
            if (!dev && d->property("isWifi").toBool() && (addr < 0 || d->property("busAddress").toInt() == addr)) {
                dev = d;
                index = i;
            }
            ++i;
        }
        if (!dev) {
            qInfo().noquote() << QStringLiteral("echoedit script: %1: no Wi-Fi module").arg(op);
        }
        else if (op == QLatin1String("wifiPage")) {
            QObject* store = findObjectWithProperty("settingsPinTarget");
            if (store) {
                QMetaObject::invokeMethod(store, "openDeviceSettingsForIndex", Q_ARG(QVariant, index));
            }
            qInfo().noquote() << QStringLiteral("echoedit script: wifiPage: device %1 %2").arg(index)
                                 .arg(store ? QStringLiteral("opened") : QStringLiteral("no store"));
        }
        else {
            bool sent = false;
            if (s.contains(QStringLiteral("off"))) {
                const int seconds = s.value(QStringLiteral("off")).toInt();
                QMetaObject::invokeMethod(dev, "wifiRadioSwitchOff", Q_RETURN_ARG(bool, sent), Q_ARG(int, seconds));
                qInfo().noquote() << QStringLiteral("echoedit script: wifiRadio off %1: %2").arg(seconds)
                                     .arg(sent ? QStringLiteral("sent") : QStringLiteral("not sent"));
            }
            else if (s.value(QStringLiteral("on")).toBool()) {
                QMetaObject::invokeMethod(dev, "wifiRadioSwitchOn", Q_RETURN_ARG(bool, sent));
                qInfo().noquote() << QStringLiteral("echoedit script: wifiRadio on: %1")
                                     .arg(sent ? QStringLiteral("sent") : QStringLiteral("not sent"));
            }
            else {
                int left = 0;
                QMetaObject::invokeMethod(dev, "wifiRadioOffLeft", Q_RETURN_ARG(int, left));
                qInfo().noquote() << QStringLiteral("echoedit script: wifiRadio state: support %1 off %2 timed %3 untilOn %4 "
                                                    "left %5 wired %6 error \"%7\" addr %8 state %9")
                                     .arg(dev->property("wifiRfSupport").toInt()).arg(dev->property("wifiRadioOff").toBool())
                                     .arg(dev->property("wifiRadioOffTimed").toBool())
                                     .arg(dev->property("wifiRadioOffUntilOn").toBool())
                                     .arg(left).arg(dev->property("wifiRadioWired").toBool())
                                     .arg(dev->property("wifiRfError").toString()).arg(dev->property("busAddress").toInt())
                                     .arg(dev->property("wifiState").toInt());
            }
        }
    }
    else if (op == QLatin1String("servo")) {
        for (QObject* d : core_->getDeviceManagerWrapperPtr()->getDevList()) {
            if (!d->property("isServoSupport").toBool()) {
                continue;
            }
            qInfo().noquote() << QStringLiteral("echoedit script: servo of %1: supported %2, enabled %3, manual %4 at %5, "
                                                "manual support %6, center %7, angle range %8")
                                 .arg(d->property("devName").toString()).arg(d->property("isServoSupport").toBool())
                                 .arg(d->property("servoEnabled").toBool()).arg(d->property("servoManual").toBool())
                                 .arg(d->property("servoManualDeg").toDouble(), 0, 'f', 2)
                                 .arg(d->property("servoManualSupport").toInt())
                                 .arg(d->property("servoCenterDeg").toDouble(), 0, 'f', 2)
                                 .arg(d->property("servoAngleRangeDeg").toDouble(), 0, 'f', 2);
        }
    }
    else if (op == QLatin1String("servoManual") || op == QLatin1String("servoSet")) {
        QObject* dev = firstServoDevice();
        bool sent = false;
        if (dev && op == QLatin1String("servoManual")) {
            sent = dev->setProperty("servoManual", s.value(QStringLiteral("on")).toBool());
        } else if (dev) {
            sent = QMetaObject::invokeMethod(dev, "servoManualSet", Q_ARG(double, s.value(QStringLiteral("deg")).toDouble()));
        }
        qInfo().noquote() << QStringLiteral("echoedit script: %1 %2: %3").arg(op)
                             .arg(op == QLatin1String("servoManual") ? QString::number(s.value(QStringLiteral("on")).toBool())
                                                                     : QString::number(s.value(QStringLiteral("deg")).toDouble()))
                             .arg(sent ? QStringLiteral("sent") : QStringLiteral("no servo device"));
    }
    else if (op == QLatin1String("presetRecall") || op == QLatin1String("presetSave") || op == QLatin1String("presetDelete")) {
        QObject* dev = nullptr;
        for (QObject* d : core_->getDeviceManagerWrapperPtr()->getDevList()) {
            if (!dev && d->property("presetsKnown").toBool()) {
                dev = d;
            }
        }
        const int slot = s.value(QStringLiteral("slot")).toInt(0);
        bool sent = false;
        if (dev && op == QLatin1String("presetRecall")) {
            sent = QMetaObject::invokeMethod(dev, "presetRecall", Q_ARG(int, slot));
        } else if (dev && op == QLatin1String("presetDelete")) {
            sent = QMetaObject::invokeMethod(dev, "presetDelete", Q_ARG(int, slot));
        } else if (dev) {
            sent = QMetaObject::invokeMethod(dev, "presetSave", Q_ARG(int, slot), Q_ARG(QString, s.value(QStringLiteral("name")).toString()));
        }
        qInfo().noquote() << QStringLiteral("echoedit script: %1 slot %2: %3").arg(op).arg(slot)
                             .arg(sent ? QStringLiteral("sent") : QStringLiteral("no device keeps presets"));
    }
    else if (op == QLatin1String("probe")) {
        const QString tag = s.value(QStringLiteral("tag")).toString();
        const bool ok = uiProbe_ && uiProbe_->dumpAll(tag, s.value(QStringLiteral("root")).toString());
        qInfo().noquote() << QStringLiteral("echoedit script: probe %1 %2").arg(tag, ok ? QStringLiteral("written") : QStringLiteral("failed"));
    }
    else if (op == QLatin1String("grab")) {
        const QString path = s.value(QStringLiteral("path")).toString();
        const bool ok = uiProbe_ && uiProbe_->grabWindow(path);
        qInfo().noquote() << QStringLiteral("echoedit script: grab %1 %2").arg(path, ok ? QStringLiteral("written") : QStringLiteral("failed"));
    }
    else if (op == QLatin1String("tr")) {
        const QByteArray context = s.value(QStringLiteral("context")).toString().toUtf8();
        const QByteArray source = s.value(QStringLiteral("source")).toString().toUtf8();
        QObject* store = findObjectWithProperty("settingsPinTarget");
        QQmlEngine* eng = store ? qmlEngine(store) : nullptr;
        QObject* lang = eng ? eng->rootContext()->contextProperty(QStringLiteral("langController")).value<QObject*>() : nullptr;
        if (lang && s.contains(QStringLiteral("apply"))) {
            QMetaObject::invokeMethod(lang, "applyCode", Q_ARG(QString, s.value(QStringLiteral("apply")).toString()));
        }
        qInfo().noquote() << QStringLiteral("echoedit script: tr %1 \"%2\" -> \"%3\"")
                             .arg(lang ? lang->property("currentCode").toString() : QStringLiteral("?"),
                                  QString::fromUtf8(source),
                                  QCoreApplication::translate(context.constData(), source.constData()));
    }
    else if (op == QLatin1String("viewCenter")) {
        GraphicsScene3dView* view = nullptr;
        for (QWindow* w : QGuiApplication::topLevelWindows()) {
            auto* qw = qobject_cast<QQuickWindow*>(w);
            if (qw && qw->isVisible() && (view = findScene3dView(qw->contentItem()))) {
                break;
            }
        }
        if (!view) {
            qWarning().noquote() << QStringLiteral("echoedit script: viewCenter: no 3D view on screen");
        }
        else {
            const QVariantMap g = view->screenToGeo(view->width() / 2.0, view->height() / 2.0);
            qInfo().noquote() << QStringLiteral("echoedit script: viewCenter lat %1 lon %2 valid %3")
                                 .arg(g.value(QStringLiteral("lat")).toDouble(), 0, 'f', 7)
                                 .arg(g.value(QStringLiteral("lon")).toDouble(), 0, 'f', 7)
                                 .arg(g.value(QStringLiteral("valid")).toBool() ? 1 : 0);
        }
    }
    else if (op == QLatin1String("linkTrack")) {
        double lat = 0.0, lon = 0.0;
        if (!boat_ || !boat_->firstPlanPoint(&lat, &lon)) {
            qWarning().noquote() << QStringLiteral("echoedit script: linkTrack: no boat plan to place it by");
        }
        else {
            boat_->fillLinkTrackForTest(s.value(QStringLiteral("points")).toInt(LinkTrack::kMaxSamples),
                                        s.value(QStringLiteral("run")).toInt(6), lat, lon);
            const QList<int> q = boat_->linkTrackQualities();
            int runs = q.isEmpty() ? 0 : 1;
            for (int i = 1; i < q.size(); ++i)
                runs += q[i] != q[i - 1] ? 1 : 0;
            qInfo().noquote() << QStringLiteral("echoedit script: linkTrack %1 samples, %2 runs of one quality, shown %3")
                                 .arg(q.size()).arg(runs).arg(boat_->linkTrackShown() ? 1 : 0);
        }
    }
    else if (op == QLatin1String("quit")) {
        ed->flush();
        qInfo().noquote() << QStringLiteral("echoedit script: quit");
        QTimer::singleShot(300, qApp, &QCoreApplication::quit);
    }
    else {
        qWarning().noquote() << QStringLiteral("echoedit script: unknown step \"%1\"").arg(op);
        return;
    }
    qInfo().noquote() << QStringLiteral("echoedit script: %1 done (%2..%3), undo %4, redo %5")
                         .arg(op).arg(from).arg(to).arg(ed->canUndo()).arg(ed->canRedo());
}

namespace {

// The item a press at p (in the window) goes to first: the topmost visible, enabled one there that takes mouse
// buttons, children before their parent, the higher z first.
QQuickItem* pressTarget(QQuickItem* item, const QPointF& p)
{
    if (!item->isVisible() || !item->isEnabled()) {
        return nullptr;
    }
    QList<QQuickItem*> kids = item->childItems();
    std::stable_sort(kids.begin(), kids.end(), [](QQuickItem* a, QQuickItem* b) { return a->z() < b->z(); });
    for (auto it = kids.crbegin(); it != kids.crend(); ++it) {
        if (QQuickItem* hit = pressTarget(*it, p)) {
            return hit;
        }
    }
    return (item->acceptedMouseButtons() != Qt::NoButton && item->contains(item->mapFromScene(p))) ? item : nullptr;
}

// A point of a "drag" step in the window: [x, y]; a share of the window ("...Frac"); a widget's panel ("...Widget":
// its id, "...Offset": [dx, dy] from the panel's top-left, its middle without); a dock's tab ("...Tab": side); an
// edge ("toEdge": side, "toAlong": share along it, 0.5 without: a few pixels in from it). For "to" also "by": [dx, dy]
// from the start. Null when the step names none it can find.
std::optional<QPointF> gesturePoint(const QJsonObject& s, const QString& key, QQuickWindow* w, const QPointF& from)
{
    const double W = w->contentItem()->width();
    const double H = w->contentItem()->height();
    const QJsonArray xy = s.value(key).toArray();
    if (xy.size() == 2) {
        return QPointF(xy.at(0).toDouble(), xy.at(1).toDouble());
    }
    const QJsonArray frac = s.value(key + QStringLiteral("Frac")).toArray();
    if (frac.size() == 2) {
        return QPointF(frac.at(0).toDouble() * W, frac.at(1).toDouble() * H);
    }
    if (s.contains(key + QStringLiteral("Widget"))) {
        QList<QQuickItem*> panels;
        collectWidgetPanels(w->contentItem(), panels);
        const QString id = s.value(key + QStringLiteral("Widget")).toString();
        for (QQuickItem* p : panels) {
            if (p->property("popupId").toString() != QStringLiteral("widget:") + id) {
                continue;
            }
            for (QQuickItem* c : p->childItems()) {
                if (c->objectName() == QLatin1String("panePanel")) {
                    const QRectF r = c->mapRectToScene(QRectF(0.0, 0.0, c->width(), c->height()));
                    const QJsonArray off = s.value(key + QStringLiteral("Offset")).toArray();
                    if (off.size() == 2) {
                        return r.topLeft() + QPointF(off.at(0).toDouble(), off.at(1).toDouble());
                    }
                    // Its middle, or where it is not under another panel.
                    const auto mine = [&](const QPointF& q) {
                        QQuickItem* t = pressTarget(w->contentItem(), q);
                        return t && (t == c || c->isAncestorOf(t));
                    };
                    if (mine(r.center())) {
                        return r.center();
                    }
                    for (int iy = 1; iy < 8; ++iy) {
                        for (int ix = 1; ix < 8; ++ix) {
                            const QPointF q(r.x() + r.width() * ix / 8.0, r.y() + r.height() * iy / 8.0);
                            if (mine(q)) {
                                return q;
                            }
                        }
                    }
                    qWarning().noquote() << QStringLiteral("echoedit script: drag: the panel of %1 is under others").arg(id);
                    return std::nullopt;
                }
            }
        }
        return std::nullopt;
    }
    if (s.contains(key + QStringLiteral("Tab"))) {
        const QString name = QStringLiteral("dockTab_") + s.value(key + QStringLiteral("Tab")).toString();
        std::function<QQuickItem*(QQuickItem*)> find = [&](QQuickItem* item) -> QQuickItem* {
            if (item->objectName() == name) {
                return item;
            }
            for (QQuickItem* c : item->childItems()) {
                if (QQuickItem* f = find(c)) {
                    return f;
                }
            }
            return nullptr;
        };
        if (QQuickItem* tab = find(w->contentItem())) {
            return tab->mapRectToScene(QRectF(0.0, 0.0, tab->width(), tab->height())).center();
        }
        return std::nullopt;
    }
    if (key == QLatin1String("to") && s.contains(QStringLiteral("toEdge"))) {
        const QString side = s.value(QStringLiteral("toEdge")).toString();
        const double along = s.value(QStringLiteral("toAlong")).toDouble(0.5);
        const double in = 6.0;
        if (side == QLatin1String("left"))   return QPointF(in, along * H);
        if (side == QLatin1String("right"))  return QPointF(W - in, along * H);
        if (side == QLatin1String("top"))    return QPointF(along * W, in);
        if (side == QLatin1String("bottom")) return QPointF(along * W, H - in);
        return std::nullopt;
    }
    if (key == QLatin1String("to") && s.contains(QStringLiteral("by"))) {
        const QJsonArray by = s.value(QStringLiteral("by")).toArray();
        return from + QPointF(by.at(0).toDouble(), by.at(1).toDouble());
    }
    return std::nullopt;
}

void sendMouse(QQuickWindow* w, QEvent::Type type, const QPointF& p, Qt::MouseButton button, Qt::MouseButtons buttons, qint64 ms)
{
    QMouseEvent ev(type, p, p, w->mapToGlobal(p), button, buttons, Qt::NoModifier);
    ev.setTimestamp(quint64(ms));
    QCoreApplication::sendEvent(w, &ev);
}

} // namespace

bool EchoEditScript::startGesture(const QJsonObject& s)
{
    gestureWin_.clear();
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
        auto* qw = qobject_cast<QQuickWindow*>(w);
        if (qw && qw->isVisible() && findWithProperty(qw->contentItem(), "secondaryPlotItem")) {
            gestureWin_ = qw;
            break;
        }
    }
    const auto from = gestureWin_ ? gesturePoint(s, QStringLiteral("from"), gestureWin_, QPointF()) : std::nullopt;
    const auto to = from ? gesturePoint(s, QStringLiteral("to"), gestureWin_, *from) : std::nullopt;
    if (!from || !to) {
        qWarning().noquote() << QStringLiteral("echoedit script: drag: no main window, or its start or end not found");
        return false;
    }
    gestureFrom_ = *from;
    gestureTo_ = *to;
    gestureSteps_ = std::max(1, s.value(QStringLiteral("steps")).toInt(20));
    gestureStep_ = 0;
    gestureHoldMs_ = std::max(0, s.value(QStringLiteral("holdMs")).toInt(0));
    gestureMs_ = std::max(0, s.value(QStringLiteral("ms")).toInt(400));
    gestureClock_.start();
    sendMouse(gestureWin_, QEvent::MouseButtonPress, gestureFrom_, Qt::LeftButton, Qt::LeftButton, gestureClock_.elapsed());
    return true;
}

int EchoEditScript::gestureStep(const QJsonObject& s)
{
    if (!gestureWin_) {
        return -1;
    }
    const qint64 now = gestureClock_.elapsed();
    if (gestureStep_ == 0 && now < gestureHoldMs_) {
        return int(gestureHoldMs_ - now);
    }
    if (gestureStep_ < gestureSteps_) {
        ++gestureStep_;
        const QPointF p = gestureFrom_ + (gestureTo_ - gestureFrom_) * (double(gestureStep_) / gestureSteps_);
        sendMouse(gestureWin_, QEvent::MouseMove, p, Qt::NoButton, Qt::LeftButton, now);
        return gestureMs_ / gestureSteps_;
    }
    // Held still at the end a moment, a picture of what shows then (the ghosts), and let go.
    if (gestureStep_ == gestureSteps_) {
        ++gestureStep_;
        const QString path = s.value(QStringLiteral("grab")).toString();
        if (!path.isEmpty() && uiProbe_) {
            uiProbe_->grabWindow(path);
        }
        return 120;
    }
    sendMouse(gestureWin_, QEvent::MouseButtonRelease, gestureTo_, Qt::LeftButton, Qt::NoButton, now);
    qInfo().noquote() << QStringLiteral("echoedit script: drag from %1,%2 to %3,%4, held %5 ms: done")
                         .arg(gestureFrom_.x(), 0, 'f', 0).arg(gestureFrom_.y(), 0, 'f', 0)
                         .arg(gestureTo_.x(), 0, 'f', 0).arg(gestureTo_.y(), 0, 'f', 0).arg(gestureHoldMs_);
    gestureWin_.clear();
    return -1;
}

QObject* EchoEditScript::firstServoDevice() const
{
    for (QObject* d : core_->getDeviceManagerWrapperPtr()->getDevList()) {
        if (d->property("isServoSupport").toBool()) {
            return d;
        }
    }
    return nullptr;
}

bool EchoEditScript::missionStep(const QString& op, const QJsonObject& s)
{
    static const QStringList ops = { QStringLiteral("missionPlan"), QStringLiteral("missionUpload"), QStringLiteral("missionDownload"),
                                     QStringLiteral("missionState"), QStringLiteral("missionEditor"), QStringLiteral("autopilotMode"),
                                     QStringLiteral("autopilotArm") };
    if (!ops.contains(op)) {
        return false;
    }
    mission::MissionPlanController* plan = core_->getMissionPlanControllerPtr();
    mission::MissionRunTracker* run = core_->getMissionRunTrackerPtr();
    DeviceManagerWrapper* dmw = core_->getDeviceManagerWrapperPtr();
    auto pairs = [](const QJsonValue& v) {
        QVariantList out;
        for (const QJsonValue& p : v.toArray()) {
            out.append(QVariant(p.toArray().toVariantList()));
        }
        return out;
    };
    if (op == QLatin1String("missionPlan")) {
        plan->newPlan();
        const QJsonArray home = s.value(QStringLiteral("home")).toArray();
        if (home.size() == 2) {
            plan->setHome(home.at(0).toDouble(), home.at(1).toDouble());   // the start point, as the editor sets it
        }
        int added = 0;
        for (const QVariant& p : pairs(s.value(QStringLiteral("points")))) {
            const QVariantList ll = p.toList();
            added += plan->addWaypoint(ll.value(0).toDouble(), ll.value(1).toDouble()).isEmpty() ? 0 : 1;
        }
        for (const QVariant& p : pairs(s.value(QStringLiteral("rally")))) {
            const QVariantList ll = p.toList();
            added += plan->addRally(ll.value(0).toDouble(), ll.value(1).toDouble()).isEmpty() ? 0 : 1;
        }
        if (s.contains(QStringLiteral("fence"))) {
            added += plan->addFence(pairs(s.value(QStringLiteral("fence"))), true).isEmpty() ? 0 : 1;
        }
        if (s.contains(QStringLiteral("speed"))) {
            plan->setCruiseSpeed(s.value(QStringLiteral("speed")).toDouble());
        }
        if (s.contains(QStringLiteral("end"))) {
            plan->setEndAction(s.value(QStringLiteral("end")).toInt());
        }
        qInfo().noquote() << QStringLiteral("echoedit script: missionPlan, %1 added: items %2, rally %3, fence %4%5")
                             .arg(added).arg(plan->itemCount()).arg(plan->rallyCount()).arg(plan->fenceCount())
                             .arg(plan->lastError().isEmpty() ? QString() : QStringLiteral(", last error: ") + plan->lastError());
    }
    else if (op == QLatin1String("missionUpload")) {
        missionResultsBefore_ = int(missionResults_.size());
        const bool sent = plan->uploadToVehicle();
        qInfo().noquote() << QStringLiteral("echoedit script: missionUpload %1%2").arg(sent ? QStringLiteral("started") : QStringLiteral("refused"))
                             .arg(sent ? QString() : QStringLiteral(": ") + plan->lastError());
    }
    else if (op == QLatin1String("missionDownload")) {
        missionResultsBefore_ = int(missionResults_.size());
        dmw->downloadMission();
        qInfo().noquote() << QStringLiteral("echoedit script: missionDownload started");
    }
    else if (op == QLatin1String("missionState")) {
        QJsonObject o;
        o[QStringLiteral("online")] = dmw->autopilotOnline();
        o[QStringLiteral("mode")] = dmw->pilotModeState();
        o[QStringLiteral("modeName")] = dmw->currentAutopilotModeName();
        o[QStringLiteral("armed")] = dmw->pilotArmState();
        o[QStringLiteral("missionSeq")] = dmw->missionCurrentSeq();
        o[QStringLiteral("missionTotal")] = dmw->missionTotal();
        o[QStringLiteral("transferActive")] = dmw->missionTransferActive();
        o[QStringLiteral("planItems")] = plan->itemCount();
        o[QStringLiteral("planRally")] = plan->rallyCount();
        o[QStringLiteral("planFence")] = plan->fenceCount();
        o[QStringLiteral("planError")] = plan->lastError();
        o[QStringLiteral("runKnown")] = run->known();
        o[QStringLiteral("runItems")] = run->itemCount();
        o[QStringLiteral("runMatchesPlan")] = run->matchesPlan();
        o[QStringLiteral("runNav")] = run->navCount();
        o[QStringLiteral("boatRoute")] = boat_ ? boat_->vehicleRouteSize() : -1;   // the route the ghost follows
        o[QStringLiteral("transfers")] = QJsonArray::fromStringList(missionResults_);
        qInfo().noquote() << QStringLiteral("echoedit script: missionState ")
                             + QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
    else if (op == QLatin1String("missionEditor")) {
        core_->setMissionEditorActive(s.value(QStringLiteral("on")).toBool(true));
        qInfo().noquote() << QStringLiteral("echoedit script: missionEditor %1").arg(core_->getMissionEditorActive() ? 1 : 0);
    }
    else if (op == QLatin1String("autopilotMode")) {
        dmw->autopilotSetMode(s.value(QStringLiteral("mode")).toInt(-1));
        qInfo().noquote() << QStringLiteral("echoedit script: autopilotMode %1 sent").arg(s.value(QStringLiteral("mode")).toInt(-1));
    }
    else {
        dmw->autopilotArm(s.value(QStringLiteral("on")).toBool(true));
        qInfo().noquote() << QStringLiteral("echoedit script: autopilotArm %1 sent").arg(s.value(QStringLiteral("on")).toBool(true) ? 1 : 0);
    }
    return true;
}

bool EchoEditScript::dockStep(const QString& op, const QJsonObject& s)
{
    static const QStringList ops = { QStringLiteral("dock"), QStringLiteral("undock"), QStringLiteral("dockScale"),
                                     QStringLiteral("dockCollapse"), QStringLiteral("docks"), QStringLiteral("layout"),
                                     QStringLiteral("front"), QStringLiteral("editMode") };
    if (!ops.contains(op)) {
        return false;
    }
    QQuickItem* workspace = nullptr;
    QObject* store = findWorkspaceStore(&workspace);
    if (!store) {
        qWarning().noquote() << QStringLiteral("echoedit script: %1: no workspace on screen").arg(op);
        return true;
    }
    // A widget by its id, or by its place in the store's list.
    const auto widgetId = [&]() {
        const QJsonValue w = s.value(QStringLiteral("widget"));
        if (w.isString()) {
            return w.toString();
        }
        const QJsonArray list = toJson(store->property("widgets")).toArray();
        const int i = w.toInt(-1);
        return (i >= 0 && i < list.size()) ? list.at(i).toObject().value(QStringLiteral("id")).toString() : QString();
    };
    const QString side = s.value(QStringLiteral("side")).toString();
    QVariant r;
    if (op == QLatin1String("dock")) {
        const QString id = widgetId();
        const int at = s.value(QStringLiteral("at")).toInt(-1);
        QMetaObject::invokeMethod(store, "dockWidget", Q_RETURN_ARG(QVariant, r), Q_ARG(QVariant, id), Q_ARG(QVariant, side),
                                  Q_ARG(QVariant, at));
        qInfo().noquote() << QStringLiteral("echoedit script: dock %1 to %2 at %3: %4").arg(id, side).arg(at)
                             .arg(r.toBool() ? QStringLiteral("docked") : QStringLiteral("refused"));
    }
    else if (op == QLatin1String("undock")) {
        const QString id = widgetId();
        QMetaObject::invokeMethod(store, "undockWidget", Q_RETURN_ARG(QVariant, r), Q_ARG(QVariant, id));
        qInfo().noquote() << QStringLiteral("echoedit script: undock %1: %2").arg(id, r.toBool() ? QStringLiteral("undocked")
                                                                                             : QStringLiteral("was not docked"));
    }
    else if (op == QLatin1String("dockScale")) {
        const double scale = s.value(QStringLiteral("scale")).toDouble(1.0);
        QMetaObject::invokeMethod(store, "setDockScale", Q_RETURN_ARG(QVariant, r), Q_ARG(QVariant, side), Q_ARG(QVariant, scale),
                                  Q_ARG(QVariant, false));
        qInfo().noquote() << QStringLiteral("echoedit script: dockScale %1 %2: %3").arg(side).arg(scale)
                             .arg(r.toBool() ? QStringLiteral("set") : QStringLiteral("refused"));
    }
    else if (op == QLatin1String("dockCollapse")) {
        if (s.contains(QStringLiteral("collapsed"))) {
            QMetaObject::invokeMethod(store, "setDockCollapsed", Q_ARG(QVariant, side),
                                      Q_ARG(QVariant, s.value(QStringLiteral("collapsed")).toBool()));
        } else {
            QMetaObject::invokeMethod(store, "toggleDockCollapsed", Q_ARG(QVariant, side));
        }
        qInfo().noquote() << QStringLiteral("echoedit script: dockCollapse %1 done").arg(side);
    }
    else if (op == QLatin1String("front")) {
        const QString id = widgetId();
        QMetaObject::invokeMethod(store, "widgetBringToFront", Q_ARG(QVariant, id));
        qInfo().noquote() << QStringLiteral("echoedit script: front %1 done").arg(id);
    }
    else if (op == QLatin1String("editMode")) {
        const bool on = s.value(QStringLiteral("on")).toBool(true);
        store->setProperty("editableMode", on);
        qInfo().noquote() << QStringLiteral("echoedit script: editMode %1").arg(store->property("editableMode").toBool());
    }
    else if (op == QLatin1String("layout")) {
        const int index = s.value(QStringLiteral("index")).toInt(0);
        QMetaObject::invokeMethod(store, "applyLayout", Q_ARG(QVariant, index));
        qInfo().noquote() << QStringLiteral("echoedit script: layout %1 applied, active %2").arg(index)
                             .arg(store->property("activeLayoutIndex").toInt());
    }
    else {
        // What the store says, and what is on screen: the workspace, every widget's panel.
        QJsonObject out;
        out.insert(QStringLiteral("docks"), toJson(store->property("docks")));
        const QJsonObject layout = toJson(store->property("dockLayout")).toObject();
        out.insert(QStringLiteral("insets"), layout.value(QStringLiteral("insets")));
        out.insert(QStringLiteral("squeezed"), layout.value(QStringLiteral("squeezed")));
        out.insert(QStringLiteral("slots"), layout.value(QStringLiteral("widgets")));
        out.insert(QStringLiteral("activeLayout"), store->property("activeLayoutIndex").toInt());
        out.insert(QStringLiteral("natural"), toJson(store->property("widgetNaturalSizes")));
        out.insert(QStringLiteral("lines"), toJson(store->property("dockLines")));
        out.insert(QStringLiteral("preview"), !toJson(store->property("dockPreview")).isNull());
        out.insert(QStringLiteral("window"), QJsonObject{ { QStringLiteral("w"), store->property("windowWidth").toDouble() },
                                                          { QStringLiteral("h"), store->property("windowHeight").toDouble() } });
        out.insert(QStringLiteral("workspace"), sceneRect(workspace));
        QJsonArray steps;
        for (const QString& sd : { QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("top"), QStringLiteral("bottom") }) {
            QVariant st;
            QMetaObject::invokeMethod(store, "dockScaleSteps", Q_RETURN_ARG(QVariant, st), Q_ARG(QVariant, sd));
            steps.append(QJsonObject{ { QStringLiteral("side"), sd }, { QStringLiteral("steps"), toJson(st) } });
        }
        out.insert(QStringLiteral("steps"), steps);
        QList<QQuickItem*> panels;
        collectWidgetPanels(workspace->window()->contentItem(), panels);
        QJsonArray list;
        for (QQuickItem* p : panels) {
            QQuickItem* panel = nullptr;
            for (QQuickItem* c : p->childItems()) {
                if (c->objectName() == QLatin1String("panePanel")) {
                    panel = c;
                }
            }
            QJsonObject o{ { QStringLiteral("id"), p->property("popupId").toString().mid(7) },
                           { QStringLiteral("docked"), p->property("edgeDocked").toBool() },
                           { QStringLiteral("visible"), p->isVisible() && (!panel || panel->isVisible()) } };
            if (panel) {
                o.insert(QStringLiteral("panel"), sceneRect(panel));
            }
            list.append(o);
        }
        out.insert(QStringLiteral("panels"), list);
        qInfo().noquote() << QStringLiteral("echoedit script: docks ") + QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact));
    }
    return true;
}

void EchoEditScript::stroke(const QString& op, int from, int to, float depth)
{
    // As a stroke of the tool over these pings at this depth: Plot2D::setMousePosition's edits.
    Dataset* ds = core_->getDatasetPtr();
    EchogramEditor* ed = core_->getEchogramEditorPtr();
    using K = echoedit::Command;
    const K::Kind kind = op == QLatin1String("draw") ? K::Draw : op == QLatin1String("erase") ? K::Erase
                       : op == QLatin1String("raise") ? K::Raise : K::Lower;
    const QVector<DatasetChannel> channels = ds->channelsList();
    from = std::max(0, from);
    to = std::min(to, ds->size());
    if (channels.isEmpty() || from >= to) {
        return;
    }
    ed->beginStroke(kind);
    for (int e = from; e < to; ++e) {
        Epoch* ep = ds->fromIndex(e);
        if (!ep) {
            continue;
        }
        ed->touch(e);
        for (const DatasetChannel& ch : channels) {
            switch (kind) {
            case K::Draw:  ep->setDistProcessing(ch.channelId_, depth); break;
            case K::Erase: ep->clearDistProcessing(ch.channelId_);      break;
            case K::Raise: ep->setMaxDistProc(ch.channelId_, depth);    break;
            default:       ep->setMinDistProc(ch.channelId_, depth);    break;
            }
        }
    }
    if (kind == K::Raise || kind == K::Lower) {
        ed->runBottomTrack(from, to);
    }
    emit ds->dataUpdate();
    emit ds->bottomTrackUpdated(channels.first().channelId_, from, to, true, false);
    ed->endStroke();
}

void EchoEditScript::probe(const QString& name, int from, int to, float offset, float radius)
{
    // The pings the bottom track found (not set by hand, not left out): the magnet started `offset`
    // from that bottom, as a hand a little off would. How far from the bottom track it lands.
    Dataset* ds = core_->getDatasetPtr();
    const QVector<DatasetChannel> channels = ds->channelsList();
    if (channels.isEmpty()) {
        return;
    }
    const DatasetChannel ch1 = channels.first();
    const DatasetChannel ch2 = channels.size() >= 2 ? channels.at(1) : DatasetChannel();
    from = std::max(0, from);
    to = std::min(to, ds->size());
    QVector<float> err;
    int considered = 0;
    int stayed = 0;
    for (int e = from; e < to; ++e) {
        if (ds->isEpochExcluded(e)) {
            continue;
        }
        Epoch* ep = ds->fromIndex(e);
        const Epoch::DistProcessing* bp = ep ? ep->bottomProcessingOf(ch1.channelId_, ch1.subChannelId_) : nullptr;
        if (!bp || !std::isfinite(bp->distance) || bp->source == Epoch::DistProcessing::DistanceSource::DistanceSourceDirectHand) {
            continue;
        }
        ++considered;
        const float start = bp->distance + offset;
        const float got = EchogramEditor::magnetDistance(ds, e, ch1.channelId_, ch1.subChannelId_,
                                                         ch2.channelId_, ch2.subChannelId_, start, radius);
        if (got == start) {
            ++stayed;
            continue;
        }
        err.append(std::fabs(got - bp->distance));
    }
    std::sort(err.begin(), err.end());
    auto pct = [&](double q) { return err.isEmpty() ? double(NAN) : double(err.at(std::min<int>(err.size() - 1, int(q * err.size())))); };
    int within10 = 0;
    for (float x : std::as_const(err)) {
        within10 += x <= 0.10f;
    }
    probe_.insert(name, QJsonObject{
        { QStringLiteral("from"), from }, { QStringLiteral("to"), to },
        { QStringLiteral("offset"), offset }, { QStringLiteral("radius"), radius },
        { QStringLiteral("considered"), considered }, { QStringLiteral("moved"), err.size() },
        { QStringLiteral("stayed"), stayed },
        { QStringLiteral("medianAbsM"), pct(0.5) }, { QStringLiteral("p90AbsM"), pct(0.9) },
        { QStringLiteral("within10cm"), considered > 0 ? double(within10) / considered : 0.0 }
    });
    qInfo().noquote() << QStringLiteral("echoedit script: magnet %1 from %2 m off: %3 pings, %4 moved, median %5 m, 90% %6 m, within 10 cm %7")
                         .arg(name).arg(offset).arg(considered).arg(err.size()).arg(pct(0.5)).arg(pct(0.9))
                         .arg(considered > 0 ? double(within10) / considered : 0.0);
}

void EchoEditScript::dump(const QString& path)
{
    Dataset* ds = core_->getDatasetPtr();
    EchogramEditor* ed = core_->getEchogramEditorPtr();
    const QVector<DatasetChannel> channels = ds->channelsList();
    const int n = ds->size();

    // The first channel's bottom of every epoch to the millimetre (FNV-1a), the products' input.
    quint64 h = 1469598103934665603ULL;
    auto mix = [&h](qint32 v) {
        const auto* b = reinterpret_cast<const unsigned char*>(&v);
        for (int i = 0; i < 4; ++i) {
            h ^= b[i];
            h *= 1099511628211ULL;
        }
    };
    int finite = 0, hand = 0, excluded = 0, usable = 0;
    // The heading the epochs carry: how many have one, and how often it turns by more than 20 deg from one
    // epoch to the next -- a boat does not, two disagreeing sources mixed together do (attitude_arbiter.h).
    int yawFinite = 0, yawSteps20 = 0;
    float lastYaw = NAN;
    QByteArray bottom;   // <path>.bottom.bin: the first channel's bottom, float32 an epoch, NaN where none
    bottom.reserve(n * 4);
    for (int e = 0; e < n; ++e) {
        Epoch* ep = ds->fromIndex(e);
        if (ep && ep->isAttAvail() && std::isfinite(ep->yaw())) {
            const float y = ep->yaw();
            if (std::isfinite(lastYaw) && std::fabs(std::remainder(y - lastYaw, 360.0f)) > 20.0f) {
                ++yawSteps20;
            }
            lastYaw = y;
            ++yawFinite;
        }
        const Epoch::DistProcessing* bp = (ep && !channels.isEmpty())
            ? ep->bottomProcessingOf(channels.first().channelId_, channels.first().subChannelId_) : nullptr;
        const float d = bp ? bp->distance : NAN;
        bottom.append(reinterpret_cast<const char*>(&d), 4);
        mix(std::isfinite(d) ? qint32(std::lround(double(d) * 1000.0)) : std::numeric_limits<qint32>::min());
        finite += std::isfinite(d);
        hand += bp && bp->source == Epoch::DistProcessing::DistanceSource::DistanceSourceDirectHand;
        const bool out = ds->isEpochExcluded(e);
        excluded += out;
        usable += !out && std::isfinite(d);
    }

    const QJsonDocument edits = QJsonDocument::fromJson(echoedit::toJson(ed->collect()));
    QJsonArray chans;   // which channel is which side ("mosaicOut" names a side by its address)
    for (const DatasetChannel& ch : channels) {
        chans.append(QJsonObject{ { QStringLiteral("address"), int(ch.channelId_.address) },
                                  { QStringLiteral("sub"), int(ch.subChannelId_) } });
    }
    QJsonObject o{
        { QStringLiteral("epochs"), n },
        { QStringLiteral("windowSize"), ds->getBottomTrackParam().windowSize },
        { QStringLiteral("bottomHash"), hex64(h) },
        { QStringLiteral("bottomFinite"), finite },
        { QStringLiteral("hand"), hand },
        { QStringLiteral("excluded"), excluded },
        { QStringLiteral("usable"), usable },
        { QStringLiteral("yawFinite"), yawFinite },
        { QStringLiteral("yawSteps20"), yawSteps20 },
        { QStringLiteral("canUndo"), ed->canUndo() },
        { QStringLiteral("canRedo"), ed->canRedo() },
        { QStringLiteral("undoCount"), ed->undoCount() },
        { QStringLiteral("redoCount"), ed->redoCount() },
        { QStringLiteral("hasEdits"), ed->hasEdits() },
        { QStringLiteral("saving"), ed->saving() },
        { QStringLiteral("editsFile"), ed->editsFile() },
        { QStringLiteral("editsFileExists"), QFile::exists(ed->editsFile()) },
        { QStringLiteral("edits"), edits.object() },
        { QStringLiteral("probe"), probe_ },
        { QStringLiteral("export"), export_ },
        { QStringLiteral("results"), results_ },
        { QStringLiteral("channels"), chans }
    };
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(o).toJson(QJsonDocument::Indented)) < 0 || !out.commit()) {
        qWarning().noquote() << QStringLiteral("echoedit script: dump to %1 failed: %2").arg(path, out.errorString());
        return;
    }
    QSaveFile bin(path + QStringLiteral(".bottom.bin"));
    if (!bin.open(QIODevice::WriteOnly) || bin.write(bottom) < 0 || !bin.commit()) {
        qWarning().noquote() << QStringLiteral("echoedit script: %1.bottom.bin not written: %2").arg(path, bin.errorString());
    }
    // <file>.track.bin: what the turns are found from (EchogramEditor::turnsOutOfMosaic), 24 bytes an
    // epoch: north, east (float64, m), heading (float32, degrees), range (float32, m); NaN where none.
    QByteArray track;
    track.reserve(n * 24);
    for (int e = 0; e < n; ++e) {
        Epoch* ep = ds->fromIndex(e);
        double ne[2] = { NAN, NAN };
        float hr[2] = { NAN, NAN };
        if (ep) {
            const NED ned = ep->getSonarPosition().ned;
            if (ned.isCoordinatesValid()) {
                ne[0] = ned.n;
                ne[1] = ned.e;
                hr[0] = ep->tryRetValidYaw();
                hr[1] = ep->getMaxRange();
            }
        }
        track.append(reinterpret_cast<const char*>(ne), 16);
        track.append(reinterpret_cast<const char*>(hr), 8);
    }
    QSaveFile trk(path + QStringLiteral(".track.bin"));
    if (!trk.open(QIODevice::WriteOnly) || trk.write(track) < 0 || !trk.commit()) {
        qWarning().noquote() << QStringLiteral("echoedit script: %1.track.bin not written: %2").arg(path, trk.errorString());
    }
    qInfo().noquote() << QStringLiteral("echoedit script: dump %1: %2 epochs, bottom %3, %4 finite, %5 by hand, %6 left out; "
                                        "heading in %7 epochs, %8 turns over 20 deg")
                         .arg(path).arg(n).arg(hex64(h)).arg(finite).arg(hand).arg(excluded).arg(yawFinite).arg(yawSteps20);
}

bool EchoEditScript::startDrag3d(const QJsonObject& s)
{
    GraphicsScene3dView* view = nullptr;
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
        auto* qw = qobject_cast<QQuickWindow*>(w);
        if (qw && qw->isVisible() && (view = findScene3dView(qw->contentItem()))) {
            break;
        }
    }
    if (!view || !view->window()) {
        qWarning().noquote() << QStringLiteral("echoedit script: drag3d: no 3D view on screen");
        return false;
    }
    const int hz = std::clamp(s.value(QStringLiteral("hz")).toInt(100), 1, 1000);
    dragIntervalMs_ = std::max(1, 1000 / hz);
    dragTurnS_ = std::max(0.1, s.value(QStringLiteral("turnSeconds")).toDouble(2.0));
    dragCx_ = view->width() * 0.5;
    dragCy_ = view->height() * 0.5;
    dragRadius_ = std::min(s.value(QStringLiteral("radius")).toDouble(120.0), 0.4 * std::min(view->width(), view->height()));
    dragX_ = dragCx_ + dragRadius_;
    dragY_ = dragCy_;
    dragView_ = view;
    dragMoveNs_.clear();
    dragMoveNs_.reserve(size_t(hz) * 64);
    dragLastTickNs_ = 0;
    dragLateMaxNs_ = 0;

    // frameSwapped comes from the render thread with the threaded render loop: counted there.
    auto frames = std::make_shared<DragFrames>();
    frames->clock.start();
    dragFrames_ = frames;
    dragFrameConn_ = connect(view->window(), &QQuickWindow::frameSwapped, this, [frames]() {
        const qint64 now = frames->clock.nsecsElapsed();
        const qint64 gap = now - frames->lastNs.exchange(now);
        if (gap > 50 * 1000000LL) {
            ++frames->over50ms;
        }
        qint64 worst = frames->maxGapNs.load();
        while (gap > worst && !frames->maxGapNs.compare_exchange_weak(worst, gap)) {
        }
        ++frames->frames;
    }, Qt::DirectConnection);

    qInfo().noquote() << QStringLiteral("echoedit script: drag3d start: the view %1x%2, about (%3, %4), radius %5 px, %6 moves a second, a turn in %7 s")
                         .arg(view->width()).arg(view->height()).arg(dragCx_).arg(dragCy_).arg(dragRadius_).arg(hz).arg(dragTurnS_);
    dragClock_.start();
    view->mousePressTrigger(Qt::LeftButton, dragX_, dragY_, Qt::Key_unknown);
    dragTimer_.start(dragIntervalMs_);
    return true;
}

void EchoEditScript::dragMove()
{
    auto* view = qobject_cast<GraphicsScene3dView*>(dragView_.data());
    if (!view) {
        dragTimer_.stop();
        return;
    }
    const qint64 now = dragClock_.nsecsElapsed();
    if (dragLastTickNs_ > 0) {
        dragLateMaxNs_ = std::max(dragLateMaxNs_, now - dragLastTickNs_ - qint64(dragIntervalMs_) * 1000000);
    }
    dragLastTickNs_ = now;
    constexpr double kPi = 3.14159265358979323846;
    const double a = 2.0 * kPi * (double(now) / 1e9) / dragTurnS_;
    dragX_ = dragCx_ + dragRadius_ * std::cos(a);
    dragY_ = dragCy_ + dragRadius_ * std::sin(a);
    QElapsedTimer took;
    took.start();
    view->mouseMoveTrigger(Qt::LeftButton, dragX_, dragY_, Qt::Key_unknown);
    dragMoveNs_.push_back(took.nsecsElapsed());
}

void EchoEditScript::finishDrag3d()
{
    dragTimer_.stop();
    disconnect(dragFrameConn_);
    const double seconds = double(dragClock_.nsecsElapsed()) / 1e9;
    if (auto* view = qobject_cast<GraphicsScene3dView*>(dragView_.data())) {
        view->mouseReleaseTrigger(Qt::LeftButton, dragX_, dragY_, Qt::Key_unknown);
    }
    std::vector<qint64> v = dragMoveNs_;
    std::sort(v.begin(), v.end());
    const auto ms = [](qint64 ns) { return QString::number(double(ns) / 1e6, 'f', 2); };
    const auto at = [&v](double q) { return v.empty() ? qint64(0) : v[std::min(v.size() - 1, size_t(q * double(v.size())))]; };
    const qint64 all = std::accumulate(v.begin(), v.end(), qint64(0));
    const int frames = dragFrames_ ? dragFrames_->frames.load() : 0;
    const qint64 gap = dragFrames_ ? dragFrames_->maxGapNs.load() : 0;
    const int jerks = dragFrames_ ? dragFrames_->over50ms.load() : 0;
    qInfo().noquote() << QStringLiteral("echoedit script: drag3d %1 s: %2 moves (%3/s), %4 frames (%5/s), longest gap between frames %6 ms, "
                                        "%12 gaps over 50 ms; a move on the GUI thread %7 ms median, %8 ms p95, %9 ms max, %10 ms in all; "
                                        "the moves' timer late by %11 ms at most")
                         .arg(seconds, 0, 'f', 1).arg(v.size()).arg(double(v.size()) / seconds, 0, 'f', 1)
                         .arg(frames).arg(double(frames) / seconds, 0, 'f', 1).arg(ms(gap))
                         .arg(ms(at(0.5))).arg(ms(at(0.95))).arg(ms(v.empty() ? 0 : v.back())).arg(ms(all)).arg(ms(dragLateMaxNs_))
                         .arg(jerks);
    dragView_.clear();
    dragFrames_.reset();
}

bool EchoEditScript::startRelayout(const QJsonObject& s)
{
    QQuickItem* workspace = nullptr;
    QQuickWindow* window = nullptr;
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
        auto* qw = qobject_cast<QQuickWindow*>(w);
        if (qw && qw->isVisible() && (workspace = findWithProperty(qw->contentItem(), "secondaryPlotItem"))) {
            window = qw;
            break;
        }
    }
    const QJsonObject dock = s.value(QStringLiteral("dock")).toObject();
    const QString collapse = s.value(QStringLiteral("collapse")).toString();   // a dock collapsed and opened, as its tab does
    const bool byStore = !dock.isEmpty() || !collapse.isEmpty();
    const QString side = !collapse.isEmpty() ? collapse
                       : dock.isEmpty() ? s.value(QStringLiteral("side")).toString(QStringLiteral("right"))
                                        : dock.value(QStringLiteral("side")).toString(QStringLiteral("right"));
    if (!workspace || !window || (!byStore && side != QLatin1String("right") && side != QLatin1String("left")
                                  && side != QLatin1String("bottom"))) {
        qWarning().noquote() << QStringLiteral("echoedit script: relayout: no workspace on screen, or side \"%1\" not right, left or bottom").arg(side);
        return false;
    }
    relayoutItem_ = workspace;
    relayoutStore_ = byStore ? workspace->property("store").value<QObject*>() : nullptr;
    relayoutDockId_ = dock.value(QStringLiteral("widget")).toString();
    if (relayoutStore_ && dock.value(QStringLiteral("widget")).isDouble()) {   // its place in the list of widgets
        const QJsonArray list = toJson(relayoutStore_->property("widgets")).toArray();
        const int i = dock.value(QStringLiteral("widget")).toInt(-1);
        relayoutDockId_ = (i >= 0 && i < list.size()) ? list.at(i).toObject().value(QStringLiteral("id")).toString() : QString();
    }
    relayoutDockSide_ = side;
    relayoutCollapse_ = !collapse.isEmpty();
    if (byStore && (!relayoutStore_ || (!relayoutCollapse_ && relayoutDockId_.isEmpty()))) {
        qWarning().noquote() << QStringLiteral("echoedit script: relayout: no store, or no widget to dock");
        return false;
    }
    relayoutProperty_ = relayoutCollapse_ ? QStringLiteral("the %1 dock collapsed and opened").arg(side)
                      : dock.isEmpty() ? QStringLiteral("anchors.%1Margin").arg(side)
                                       : QStringLiteral("the dock of %1 at the %2").arg(relayoutDockId_, side);
    relayoutOrig_ = byStore ? 0.0 : QQmlProperty::read(workspace, relayoutProperty_).toDouble();
    relayoutMargin_ = relayoutOrig_ + s.value(QStringLiteral("margin")).toDouble(300.0);
    relayoutLeft_ = 2 * std::max(1, s.value(QStringLiteral("times")).toInt(10));   // there and back
    relayoutIntervalMs_ = std::max(50, s.value(QStringLiteral("intervalMs")).toInt(400));
    relayoutPending_ = false;
    relayoutChangeNs_.clear();
    relayoutSyncNs_.clear();
    relayoutShownNs_.clear();
    relayoutFrameCount_.clear();
    relayoutSyncSumNs_.clear();
    relayoutLastNs_.clear();

    // On the render thread: the first sync after a change is the frame that takes it (the GUI waits through it); every
    // frame until the next change is counted, the panes' animation's if there is one.
    auto frames = std::make_shared<RelayoutFrames>();
    frames->clock.start();
    relayoutFrames_ = frames;
    relayoutConns_ << connect(window, &QQuickWindow::beforeSynchronizing, this, [frames]() {
        const qint64 now = frames->clock.nsecsElapsed();
        frames->anySyncStartNs = now;
        if (frames->state.load() == 1) {
            frames->syncStartNs = now;
        }
    }, Qt::DirectConnection);
    relayoutConns_ << connect(window, &QQuickWindow::afterSynchronizing, this, [frames]() {
        int one = 1;
        const qint64 now = frames->clock.nsecsElapsed();
        if (frames->state.load() >= 1 && frames->anySyncStartNs.load() > 0) {
            frames->syncSumNs += now - frames->anySyncStartNs.load();
        }
        if (frames->state.load() == 1 && frames->syncStartNs.load() > 0) {
            frames->syncNs = now - frames->syncStartNs.load();
            frames->state.compare_exchange_strong(one, 2);
        }
    }, Qt::DirectConnection);
    relayoutConns_ << connect(window, &QQuickWindow::frameSwapped, this, [frames]() {
        int two = 2;
        const qint64 now = frames->clock.nsecsElapsed();
        if (frames->state.load() >= 2) {
            ++frames->frames;
            frames->lastShownNs = now;
        }
        if (frames->state.load() == 2) {
            frames->shownNs = now;
            frames->state.compare_exchange_strong(two, 3);
        }
    }, Qt::DirectConnection);
    if (!byStore) {
        qWarning().noquote() << QStringLiteral("echoedit script: relayout: %1 written: its binding (the docks, the settings panel) "
                                               "is gone for the rest of this run").arg(relayoutProperty_);
    }
    qInfo().noquote() << QStringLiteral("echoedit script: relayout start: %1%2, %3 changes %4 ms apart")
                         .arg(relayoutProperty_)
                         .arg(relayoutStore_ ? QString() : QStringLiteral(" from %1 to %2 px").arg(relayoutOrig_).arg(relayoutMargin_))
                         .arg(relayoutLeft_).arg(relayoutIntervalMs_);
    return true;
}

bool EchoEditScript::relayoutStep()
{
    RelayoutFrames* f = relayoutFrames_.get();
    if (!f) {
        return false;
    }
    const qint64 now = f->clock.nsecsElapsed();
    if (relayoutPending_) {
        if (f->state.load() < 3 && now - relayoutT0_ < 3000000000LL) {
            return true;   // its frame still to come
        }
        if (now - relayoutT0_ < qint64(relayoutIntervalMs_) * 1000000) {
            return true;   // and the frames after it
        }
        const bool shown = f->state.load() == 3;
        relayoutSyncNs_.push_back(shown ? f->syncNs.load() : -1);   // -1: no frame in 3 s
        relayoutShownNs_.push_back(shown ? f->shownNs.load() - relayoutT0_ : -1);
        relayoutFrameCount_.push_back(f->frames.load());
        relayoutSyncSumNs_.push_back(f->syncSumNs.load());
        relayoutLastNs_.push_back(f->frames.load() > 0 ? f->lastShownNs.load() - relayoutT0_ : -1);
        relayoutPending_ = false;
    }
    if (relayoutLeft_ <= 0 || !relayoutItem_) {
        return false;
    }
    const bool there = relayoutLeft_ % 2 == 0;
    f->syncStartNs = 0;
    f->frames = 0;
    f->syncSumNs = 0;
    f->lastShownNs = 0;
    f->state = 1;
    relayoutT0_ = f->clock.nsecsElapsed();
    if (relayoutStore_ && relayoutCollapse_) {
        QMetaObject::invokeMethod(relayoutStore_.data(), "setDockCollapsed", Q_ARG(QVariant, relayoutDockSide_), Q_ARG(QVariant, there));
    }
    else if (relayoutStore_) {
        QVariant r;
        if (there) {
            QMetaObject::invokeMethod(relayoutStore_.data(), "dockWidget", Q_RETURN_ARG(QVariant, r), Q_ARG(QVariant, relayoutDockId_),
                                      Q_ARG(QVariant, relayoutDockSide_), Q_ARG(QVariant, -1));
        } else {
            QMetaObject::invokeMethod(relayoutStore_.data(), "undockWidget", Q_RETURN_ARG(QVariant, r), Q_ARG(QVariant, relayoutDockId_));
        }
        if (!r.toBool()) {
            qWarning().noquote() << QStringLiteral("echoedit script: relayout: %1 %2 refused").arg(there ? QStringLiteral("dock") : QStringLiteral("undock"), relayoutDockId_);
        }
    } else {
        QQmlProperty::write(relayoutItem_.data(), relayoutProperty_, there ? relayoutMargin_ : relayoutOrig_);
    }
    relayoutChangeNs_.push_back(f->clock.nsecsElapsed() - relayoutT0_);
    --relayoutLeft_;
    relayoutPending_ = true;
    return true;
}

void EchoEditScript::finishRelayout()
{
    for (const auto& c : relayoutConns_) {
        disconnect(c);
    }
    relayoutConns_.clear();
    const auto stats = [](const std::vector<qint64>& v, double unit, int digits) {
        std::vector<qint64> ok;
        for (qint64 x : v) {
            if (x >= 0) ok.push_back(x);
        }
        std::sort(ok.begin(), ok.end());
        const auto u = [&](qint64 x) { return QString::number(double(x) / unit, 'f', digits); };
        return ok.empty() ? QStringLiteral("-") : QStringLiteral("%1 median / %2 max").arg(u(ok[ok.size() / 2]), u(ok.back()));
    };
    int missing = 0;
    for (qint64 x : relayoutShownNs_) missing += x < 0 ? 1 : 0;
    std::vector<qint64> counts(relayoutFrameCount_.begin(), relayoutFrameCount_.end());
    qInfo().noquote() << QStringLiteral("echoedit script: relayout %1 changes of %2%3: in the GUI thread %4 ms, the sync of its frame %5 ms, on screen after %6 ms; %7 without a frame in 3 s")
                         .arg(relayoutChangeNs_.size()).arg(relayoutProperty_)
                         .arg(relayoutStore_ ? QString() : QStringLiteral(" by %1 px").arg(relayoutMargin_ - relayoutOrig_))
                         .arg(stats(relayoutChangeNs_, 1e6, 1), stats(relayoutSyncNs_, 1e6, 1), stats(relayoutShownNs_, 1e6, 1)).arg(missing);
    qInfo().noquote() << QStringLiteral("echoedit script: relayout frames in the %1 ms after a change: %2, their syncs together %3 ms, the last on screen after %4 ms")
                         .arg(relayoutIntervalMs_).arg(stats(counts, 1.0, 0), stats(relayoutSyncSumNs_, 1e6, 1), stats(relayoutLastNs_, 1e6, 1));
    relayoutFrames_.reset();
    relayoutItem_.clear();
    relayoutStore_.clear();
}
