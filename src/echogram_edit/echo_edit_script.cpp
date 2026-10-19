#include "echo_edit_script.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QSaveFile>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <limits>

#include "core.h"
#include "echogram_editor.h"
#include "ui_probe.h"

namespace {

QString hex64(quint64 v)
{
    return QStringLiteral("%1").arg(v, 16, 16, QLatin1Char('0'));
}

} // namespace

EchoEditScript::EchoEditScript(Core* core, QObject* parent) :
    QObject(parent),
    core_(core)
{
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &EchoEditScript::step);
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
    // A recording open and read: the steps are about its epochs.
    if (core_->property("openedFilePath").toString().isEmpty() || core_->getIsFileOpening() || core_->getDatasetPtr()->size() <= 0) {
        timer_.start(500);
        return;
    }

    const QJsonObject s = steps_.at(next_).toObject();
    const QString op = s.value(QStringLiteral("op")).toString();

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
    else if (op == QLatin1String("grab")) {
        const QString path = s.value(QStringLiteral("path")).toString();
        const bool ok = uiProbe_ && uiProbe_->grabWindow(path);
        qInfo().noquote() << QStringLiteral("echoedit script: grab %1 %2").arg(path, ok ? QStringLiteral("written") : QStringLiteral("failed"));
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
    QByteArray bottom;   // <path>.bottom.bin: the first channel's bottom, float32 an epoch, NaN where none
    bottom.reserve(n * 4);
    for (int e = 0; e < n; ++e) {
        Epoch* ep = ds->fromIndex(e);
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
    qInfo().noquote() << QStringLiteral("echoedit script: dump %1: %2 epochs, bottom %3, %4 finite, %5 by hand, %6 left out")
                         .arg(path).arg(n).arg(hex64(h)).arg(finite).arg(hand).arg(excluded);
}
