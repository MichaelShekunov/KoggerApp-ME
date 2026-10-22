#include "echo_edit_script.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
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
            timer_.start(250);
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
    else if (op == QLatin1String("presetRecall") || op == QLatin1String("presetSave")) {
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
        } else if (dev) {
            sent = QMetaObject::invokeMethod(dev, "presetSave", Q_ARG(int, slot), Q_ARG(QString, s.value(QStringLiteral("name")).toString()));
        }
        qInfo().noquote() << QStringLiteral("echoedit script: %1 slot %2: %3").arg(op).arg(slot)
                             .arg(sent ? QStringLiteral("sent") : QStringLiteral("no device keeps presets"));
    }
    else if (op == QLatin1String("grab")) {
        const QString path = s.value(QStringLiteral("path")).toString();
        const bool ok = uiProbe_ && uiProbe_->grabWindow(path);
        qInfo().noquote() << QStringLiteral("echoedit script: grab %1 %2").arg(path, ok ? QStringLiteral("written") : QStringLiteral("failed"));
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
    const QString side = s.value(QStringLiteral("side")).toString(QStringLiteral("right"));
    if (!workspace || !window || (side != QLatin1String("right") && side != QLatin1String("left") && side != QLatin1String("bottom"))) {
        qWarning().noquote() << QStringLiteral("echoedit script: relayout: no workspace on screen, or side \"%1\" not right, left or bottom").arg(side);
        return false;
    }
    relayoutItem_ = workspace;
    relayoutProperty_ = QStringLiteral("anchors.%1Margin").arg(side);
    relayoutOrig_ = QQmlProperty::read(workspace, relayoutProperty_).toDouble();
    relayoutMargin_ = relayoutOrig_ + s.value(QStringLiteral("margin")).toDouble(300.0);
    relayoutLeft_ = 2 * std::max(1, s.value(QStringLiteral("times")).toInt(10));   // to the margin and back
    relayoutPending_ = false;
    relayoutChangeNs_.clear();
    relayoutSyncNs_.clear();
    relayoutShownNs_.clear();

    // On the render thread: the first sync after a change is the frame that takes it (the GUI waits through it).
    auto frames = std::make_shared<RelayoutFrames>();
    frames->clock.start();
    relayoutFrames_ = frames;
    relayoutConns_ << connect(window, &QQuickWindow::beforeSynchronizing, this, [frames]() {
        if (frames->state.load() == 1) {
            frames->syncStartNs = frames->clock.nsecsElapsed();
        }
    }, Qt::DirectConnection);
    relayoutConns_ << connect(window, &QQuickWindow::afterSynchronizing, this, [frames]() {
        int one = 1;
        const qint64 now = frames->clock.nsecsElapsed();
        if (frames->state.load() == 1 && frames->syncStartNs.load() > 0) {
            frames->syncNs = now - frames->syncStartNs.load();
            frames->state.compare_exchange_strong(one, 2);
        }
    }, Qt::DirectConnection);
    relayoutConns_ << connect(window, &QQuickWindow::frameSwapped, this, [frames]() {
        int two = 2;
        if (frames->state.load() == 2) {
            frames->shownNs = frames->clock.nsecsElapsed();
            frames->state.compare_exchange_strong(two, 3);
        }
    }, Qt::DirectConnection);
    qInfo().noquote() << QStringLiteral("echoedit script: relayout start: %1 from %2 to %3 px, %4 changes")
                         .arg(relayoutProperty_).arg(relayoutOrig_).arg(relayoutMargin_).arg(relayoutLeft_);
    return true;
}

bool EchoEditScript::relayoutStep()
{
    RelayoutFrames* f = relayoutFrames_.get();
    if (!f) {
        return false;
    }
    if (relayoutPending_) {
        if (f->state.load() == 3) {
            relayoutSyncNs_.push_back(f->syncNs.load());
            relayoutShownNs_.push_back(f->shownNs.load() - relayoutT0_);
            relayoutPending_ = false;
        }
        else if (f->clock.nsecsElapsed() - relayoutT0_ < 3000000000LL) {
            return true;   // its frame still to come
        }
        else {
            relayoutSyncNs_.push_back(-1);   // no frame in 3 s
            relayoutShownNs_.push_back(-1);
            relayoutPending_ = false;
        }
    }
    if (relayoutLeft_ <= 0 || !relayoutItem_) {
        return false;
    }
    const double value = relayoutLeft_ % 2 == 0 ? relayoutMargin_ : relayoutOrig_;
    f->syncStartNs = 0;
    f->state = 1;
    relayoutT0_ = f->clock.nsecsElapsed();
    QQmlProperty::write(relayoutItem_.data(), relayoutProperty_, value);
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
    const auto stats = [](const std::vector<qint64>& v) {
        std::vector<qint64> ok;
        for (qint64 x : v) {
            if (x >= 0) ok.push_back(x);
        }
        std::sort(ok.begin(), ok.end());
        const auto ms = [](qint64 ns) { return QString::number(double(ns) / 1e6, 'f', 1); };
        return ok.empty() ? QStringLiteral("-") : QStringLiteral("%1 median / %2 max").arg(ms(ok[ok.size() / 2]), ms(ok.back()));
    };
    int missing = 0;
    for (qint64 x : relayoutShownNs_) missing += x < 0 ? 1 : 0;
    qInfo().noquote() << QStringLiteral("echoedit script: relayout %1 changes of %2 by %3 px: in the GUI thread %4 ms, the sync of its frame %5 ms, on screen after %6 ms; %7 without a frame in 3 s")
                         .arg(relayoutChangeNs_.size()).arg(relayoutProperty_).arg(relayoutMargin_ - relayoutOrig_)
                         .arg(stats(relayoutChangeNs_), stats(relayoutSyncNs_), stats(relayoutShownNs_)).arg(missing);
    relayoutFrames_.reset();
    relayoutItem_.clear();
}
