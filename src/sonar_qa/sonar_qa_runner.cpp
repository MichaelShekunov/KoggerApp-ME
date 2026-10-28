#include "sonar_qa_runner.h"

#include "sonar_qa_demo.h"
#include "sonar_qa_device.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QUrl>

#include <cmath>

using namespace sonarqa;

namespace {

constexpr int kWarmPings = 3;          // same-size pings after the settings before the sweep starts
constexpr int kLivePoints = 400;       // a side of the live plot

bool isNanoSss(int board)
{
    return board == 17 || board == 129;   // BoardNanoSSS, BoardPULSEblue_DSS: the l412_adc firmware
}

QVariantList toList(const QVector<float>& v)
{
    QVariantList l;
    l.reserve(v.size());
    for (float x : v) {
        l.append(double(x));
    }
    return l;
}

} // namespace

// KOGGER_SONARQA_ROOT: runs of a check of the window go elsewhere than the stand's results.
SonarQaRunner::SonarQaRunner(QObject* parent)
    : SonarQaRunner(qEnvironmentVariableIsSet("KOGGER_SONARQA_ROOT") ? qEnvironmentVariable("KOGGER_SONARQA_ROOT")
                                                                     : Store::defaultRoot(),
                    parent)
{
}

SonarQaRunner::SonarQaRunner(const QString& root, QObject* parent) : QObject(parent), store_(root)
{
    state_ = store_.loadState();
    pingTimer_.setSingleShot(true);
    connect(&pingTimer_, &QTimer::timeout, this, [this]() {
        abort(tr("No echogram from the sonar for %1 s").arg(pingTimer_.interval() / 1000));
    });
}

SonarQaRunner::~SonarQaRunner()
{
    if (busy() && adapter_) {
        adapter_->restore(snapshot_);
    }
    raw_.close();
    if (!rawPath_.isEmpty()) {
        QFile::remove(rawPath_);
    }
}

void SonarQaRunner::setDevice(QObject* device)
{
    if (busy() || device == deviceObject_) {
        return;
    }
    deviceObject_ = device;
    SonarQaDevice* a = device ? makeSonarQaDevice(device, this) : nullptr;
    if (adapter_) {
        disconnect(adapter_, nullptr, this, nullptr);   // a sonar no longer chosen must not feed the run
        if (ownsAdapter_) {
            adapter_->deleteLater();
        }
    }
    adapter_ = a;
    ownsAdapter_ = a != nullptr;
    attach(a);
    refreshDevice();
}

void SonarQaRunner::attach(SonarQaDevice* a)
{
    if (!a) {
        return;
    }
    connect(a, &SonarQaDevice::ping, this, &SonarQaRunner::onPing);
    connect(a, &SonarQaDevice::supply, this, [this](float v) {
        live_[QStringLiteral("supply")] = v;
        if (busy()) {
            supply_.append(v);
        }
    });
    connect(a, &SonarQaDevice::temperature, this, [this](float c) {
        live_[QStringLiteral("temperature")] = c;
        if (busy()) {
            temperature_.append(c);
        }
    });
    connect(a, &SonarQaDevice::lost, this, [this]() { abort(tr("The sonar went away")); });
}

void SonarQaRunner::setWindow(QObject* w)
{
    if (window_ != w) {
        window_ = w;
        emit windowChanged();
    }
}

void SonarQaRunner::setAdapter(SonarQaDevice* adapter)
{
    if (busy()) {
        return;
    }
    if (adapter_) {
        disconnect(adapter_, nullptr, this, nullptr);   // a sonar no longer chosen must not feed the run
        if (ownsAdapter_) {
            adapter_->deleteLater();
        }
    }
    deviceObject_ = adapter;
    adapter_ = adapter;
    ownsAdapter_ = false;
    attach(adapter);
    refreshDevice();
}

void SonarQaRunner::refreshDevice()
{
    QVariantMap m;
    m[QStringLiteral("connected")] = adapter_ != nullptr;
    if (adapter_) {
        const SonarQaDevice::Identity id = adapter_->identity();
        m[QStringLiteral("serial")] = double(id.serial);
        m[QStringLiteral("name")] = id.name;
        m[QStringLiteral("firmware")] = id.firmware;
        m[QStringLiteral("board")] = id.board;
        m[QStringLiteral("supported")] = isNanoSss(id.board);
        m[QStringLiteral("isReference")] = state_.referenceSerial != 0 && id.serial == state_.referenceSerial;
    }
    if (m != deviceInfo_) {
        deviceInfo_ = m;
        emit deviceChanged();
    }
}

double SonarQaRunner::progress() const
{
    const int total = run_.params.totalPings();
    return busy() && total > 0 ? qMin(1.0, double(pingsDone_) / total) : 0.0;
}

int SonarQaRunner::etaSec() const
{
    if (!busy()) {
        return 0;
    }
    const int left = qMax(0, run_.params.totalPings() - pingsDone_);
    if (pingsDone_ > 20 && clock_.isValid()) {
        return int(double(clock_.elapsed()) / pingsDone_ * left / 1000.0);
    }
    return left * run_.params.periodMs / 1000;
}

QVariantMap SonarQaRunner::reference() const
{
    QVariantMap m;
    m[QStringLiteral("serial")] = double(state_.referenceSerial);
    m[QStringLiteral("baselineFile")] = state_.baselineFile;
    m[QStringLiteral("baselineTime")] = state_.baselineTime;
    m[QStringLiteral("checkFile")] = state_.checkFile;
    m[QStringLiteral("checkTime")] = state_.checkTime;
    m[QStringLiteral("checkPassed")] = state_.checkPassed;
    m[QStringLiteral("unitsSinceCheck")] = state_.unitsSinceCheck;
    m[QStringLiteral("unitsPerCheck")] = state_.unitsPerCheck;
    m[QStringLiteral("unitsLeft")] = qMax(0, state_.unitsPerCheck - state_.unitsSinceCheck);
    return m;
}

QVariantMap SonarQaRunner::params() const
{
    QVariantMap m = state_.params.toJson().toVariantMap();
    m[QStringLiteral("unitsPerCheck")] = state_.unitsPerCheck;
    return m;
}

void SonarQaRunner::setParams(const QVariantMap& p)
{
    if (busy()) {
        return;
    }
    state_.params = Params::fromJson(QJsonObject::fromVariantMap(p));
    state_.unitsPerCheck = qMax(1, p.value(QStringLiteral("unitsPerCheck"), state_.unitsPerCheck).toInt());
    store_.saveState(state_);
    emit stateChanged();
}

QVariantMap SonarQaRunner::tolerances() const
{
    return state_.tolerances.toJson().toVariantMap();
}

void SonarQaRunner::setTolerances(const QVariantMap& t)
{
    if (busy()) {
        return;
    }
    state_.tolerances = Tolerances::fromJson(QJsonObject::fromVariantMap(t));
    store_.saveState(state_);
    emit stateChanged();
}

void SonarQaRunner::resetParams()
{
    if (busy()) {
        return;
    }
    state_.params = Params();
    state_.tolerances = Tolerances();
    state_.unitsPerCheck = 10;
    store_.saveState(state_);
    emit stateChanged();
}

bool SonarQaRunner::demoAvailable() const
{
    return qEnvironmentVariableIsSet("KOGGER_SONARQA_DEMO");
}

void SonarQaRunner::useDemo(int serial, const QString& fault)
{
    if (busy() || !demoAvailable()) {
        return;
    }
    auto* demo = new SonarQaDemoDevice(qEnvironmentVariable("KOGGER_SONARQA_DEMO") != QLatin1String("fast"), this);
    demo->m.serial = quint32(serial);
    if (serial != 1001) {   // no two sonars alike: a little weaker, the resonance a little off
        demo->m.gainDb = -0.8;
        demo->m.f0 += 5;
    }
    if (fault == QLatin1String("weak")) {
        demo->m.gainDb = -8;
    } else if (fault == QLatin1String("resonance")) {
        demo->m.f0 += 60;
    } else if (fault == QLatin1String("ringing")) {
        demo->m.ringScale = 2.5;
    }
    setAdapter(demo);
    ownsAdapter_ = true;
    deviceObject_ = demo;
    emit deviceChanged();
}

void SonarQaRunner::openFolder() const
{
    QDir().mkpath(store_.root());
    QDesktopServices::openUrl(QUrl::fromLocalFile(store_.root()));
}

QString SonarQaRunner::canStart(const QString& kind)
{
    if (busy()) {
        return tr("A run is going on");
    }
    if (!adapter_) {
        return tr("No sonar selected");
    }
    refreshDevice();
    const quint32 serial = quint32(deviceInfo_.value(QStringLiteral("serial")).toDouble());
    const int board = deviceInfo_.value(QStringLiteral("board")).toInt();
    if (!isNanoSss(board)) {
        return board == 0 ? tr("The sonar has not answered yet") : tr("Not a nanoSSS (board %1)").arg(board);
    }
    if (serial == 0) {
        return tr("The sonar has not reported its serial number");
    }
    if (kind == QLatin1String("baseline")) {
        return QString();
    }
    if (state_.referenceSerial == 0 || state_.baselineFile.isEmpty()) {
        return tr("No reference yet: make the baseline on the reference sonar first");
    }
    if (kind == QLatin1String("check")) {
        return serial == state_.referenceSerial ? QString()
                                                : tr("This is not the reference sonar (that is serial %1)")
                                                      .arg(state_.referenceSerial);
    }
    if (kind != QLatin1String("unit")) {
        return tr("Unknown run");
    }
    if (serial == state_.referenceSerial) {
        return tr("This is the reference sonar: run its check instead");
    }
    if (!state_.checkPassed) {
        return tr("The reference's last check moved away from its baseline: look at the stand, check the reference "
                  "again or accept the check");
    }
    if (state_.unitsSinceCheck >= state_.unitsPerCheck) {
        return tr("%1 sonars tested since the reference's check: check the reference again")
            .arg(state_.unitsSinceCheck);
    }
    Run ref;
    QString err;
    if (!store_.loadRun(state_.checkFile, &ref, &err)) {
        return tr("The reference's check cannot be read: %1").arg(err);
    }
    const QString fw = deviceInfo_.value(QStringLiteral("firmware")).toString();
    if (fw != ref.firmware) {
        return tr("Firmware %1, the reference has %2: load the same firmware").arg(fw, ref.firmware);
    }
    return QString();
}

bool SonarQaRunner::start(const QString& kind)
{
    const QString why = canStart(kind);
    if (!why.isEmpty()) {
        setStatus(why);
        return false;
    }
    const SonarQaDevice::Identity id = adapter_->identity();
    snapshot_ = adapter_->snapshot();

    run_ = Run();
    run_.kind = kind;
    run_.time = QDateTime::currentDateTime().toString(Qt::ISODate);
    run_.serial = id.serial;
    run_.deviceName = id.name;
    run_.firmware = id.firmware;
    run_.board = id.board;
    reference_ = Run();
    referenceFile_.clear();

    QJsonObject settings;
    if (kind == QLatin1String("baseline")) {
        // the reference's own settings now become the stand's; the sweep is the one set in the window
        run_.params = state_.params;
        settings = { { "resolution", snapshot_.value("resolution").toInt() },
                     { "offset", snapshot_.value("offset").toInt() }, { "boost", snapshot_.value("boost").toInt() } };
    } else {
        referenceFile_ = kind == QLatin1String("check") ? state_.baselineFile : state_.checkFile;
        QString err;
        if (!store_.loadRun(referenceFile_, &reference_, &err)) {
            setStatus(tr("The reference run cannot be read: %1").arg(err));
            return false;
        }
        run_.params = reference_.params;   // the same sweep and settings as the reference, whatever the window says
        settings = reference_.settings;
    }
    settings.insert("samples", run_.params.samples);
    settings.insert("periodMs", run_.params.periodMs);
    run_.settings = settings;

    const int sides = 2;
    off_ = QVector<QVector<PingMetrics>>(sides);
    on_ = QVector<QVector<PingMetrics>>(sides);
    supply_.clear();
    temperature_.clear();
    step_ = txState_ = discard_ = collected_ = warm_ = pingsDone_ = pingIndex_ = 0;
    warmSize_ = -1;

    rawPath_ = QDir(store_.root()).filePath(QStringLiteral("running.ksqa"));
    QDir().mkpath(store_.root());
    raw_.open(rawPath_, QJsonObject{ { "kind", kind }, { "time", run_.time }, { "serial", double(run_.serial) },
                                      { "params", run_.params.toJson() }, { "settings", settings } });

    adapter_->applyRun(run_.params.samples, run_.params.periodMs, settings.value("resolution").toInt(),
                       settings.value("offset").toInt(), settings.value("boost").toInt());
    phase_ = Phase::Warmup;
    clock_.invalidate();
    pingTimer_.start(qMax(3000, 20 * run_.params.periodMs));
    setStatus(tr("Setting the sonar up"));
    emit busyChanged();
    emit progressChanged();
    return true;
}

void SonarQaRunner::stop()
{
    if (busy()) {
        abort(tr("Stopped"));
    }
}

void SonarQaRunner::acceptCheck()
{
    if (busy() || state_.checkFile.isEmpty()) {
        return;
    }
    state_.checkPassed = true;
    store_.saveState(state_);
    emit stateChanged();
}

void SonarQaRunner::onPing(const QVector<QVector<uint8_t>>& sides, float resolutionM)
{
    if (!busy() || sides.isEmpty()) {
        return;
    }
    pingTimer_.start(qMax(2000, 20 * run_.params.periodMs));
    const int size = sides.first().size();

    if (phase_ == Phase::Warmup) {
        // the new sample count is in force once a few pings in a row carry the same one
        warm_ = (size == warmSize_) ? warm_ + 1 : 1;
        warmSize_ = size;
        if (warm_ >= kWarmPings && size > 0) {
            run_.settings.insert("samplesPerSide", size);
            run_.settings.insert("sides", sides.size());
            run_.resolutionM = resolutionM;
            if (reference_.settings.contains("samplesPerSide")
                && reference_.settings.value("samplesPerSide").toInt() != size) {
                abort(tr("The sonar gives %1 samples a side, the reference %2")
                          .arg(size).arg(reference_.settings.value("samplesPerSide").toInt()));
                return;
            }
            clock_.start();
            beginStep();
        }
        return;
    }

    ++pingsDone_;
    if (phase_ == Phase::Discard) {
        if (--discard_ <= 0) {
            phase_ = Phase::Collect;
            collected_ = 0;
        }
    } else if (phase_ == Phase::Collect) {
        const bool tx = txState_ == 1;
        const int freq = run_.params.freqAt(step_);
        const int pulses = tx ? run_.params.txPulses : 0;
        for (int s = 0; s < sides.size() && s < off_.size(); ++s) {
            const PingMetrics m = analysePing(sides[s].constData(), sides[s].size(), tx, run_.params);
            (tx ? on_[s] : off_[s]).append(m);
            raw_.write(freq, pulses, collected_, s, sides[s]);
        }
        if (++collected_ >= run_.params.pingsPerStep) {
            if (!tx) {
                setTransmitterState(1);
            } else {
                finishStep();
            }
        }
    }

    if (pingsDone_ % 4 == 0 || phase_ == Phase::Idle) {
        QVariantList liveSides;
        for (const auto& side : sides) {
            QVariantList pts;
            const int stepN = qMax(1, int(std::ceil(double(side.size()) / kLivePoints)));
            for (int i = 0; i < side.size(); i += stepN) {
                int m = 0;
                for (int j = i; j < i + stepN && j < side.size(); ++j) {
                    m = qMax(m, int(side[j]));
                }
                pts.append(m);
            }
            liveSides.append(QVariant(pts));
        }
        live_[QStringLiteral("sides")] = liveSides;
        live_[QStringLiteral("freqKhz")] = run_.params.freqAt(step_);
        live_[QStringLiteral("pulses")] = txState_ ? run_.params.txPulses : 0;
        emit liveChanged();
        emit progressChanged();
    }
}

void SonarQaRunner::beginStep()
{
    for (auto& v : off_) {
        v.clear();
    }
    for (auto& v : on_) {
        v.clear();
    }
    setTransmitterState(0);
}

void SonarQaRunner::setTransmitterState(int txState)
{
    txState_ = txState;
    const int freq = run_.params.freqAt(step_);
    adapter_->setTransmitter(freq, txState ? run_.params.txPulses : 0);
    discard_ = run_.params.discardPings;
    collected_ = 0;
    phase_ = discard_ > 0 ? Phase::Discard : Phase::Collect;
    setStatus(txState ? tr("%1 kHz, %2 pulses").arg(freq).arg(run_.params.txPulses)
                      : tr("%1 kHz, transmitter off").arg(freq));
}

void SonarQaRunner::finishStep()
{
    Step st;
    st.freqKhz = run_.params.freqAt(step_);
    for (int s = 0; s < off_.size(); ++s) {
        st.sides.append(aggregate(off_[s], on_[s], run_.params.maxEchoes));
    }
    run_.steps.append(st);
    if (++step_ >= run_.params.stepCount()) {
        finish();
    } else {
        beginStep();
    }
}

void SonarQaRunner::finish()
{
    pingTimer_.stop();
    adapter_->restore(snapshot_);
    raw_.close();
    run_.supply = Series::of(supply_);
    run_.temperature = Series::of(temperature_);

    Comparison cmp;
    QString verdict;
    if (run_.kind == QLatin1String("baseline")) {
        verdict = QStringLiteral("baseline");
    } else {
        cmp = compare(run_, reference_, state_.tolerances);
        verdict = run_.kind == QLatin1String("check") ? (cmp.pass ? QStringLiteral("check_ok")
                                                                  : QStringLiteral("check_drift"))
                                                      : (cmp.pass ? QStringLiteral("pass") : QStringLiteral("fail"));
    }

    const QString base = store_.newRunPath(run_, cmp.pass ? QStringLiteral("pass") : QStringLiteral("fail"));
    QString err;
    const bool saved = store_.saveRun(base, run_, run_.kind == QLatin1String("baseline") ? QJsonObject() : cmp.toJson(),
                                      run_.kind == QLatin1String("baseline") ? QString()
                                                                            : QDir(store_.root()).filePath(referenceFile_),
                                      &err);
    QFile::remove(base + QStringLiteral(".ksqa"));
    QFile::rename(rawPath_, base + QStringLiteral(".ksqa"));
    rawPath_.clear();
    const QString rel = QDir(store_.root()).relativeFilePath(base + QStringLiteral(".json"));

    if (run_.kind == QLatin1String("baseline")) {
        state_.referenceSerial = run_.serial;
        state_.baselineFile = rel;
        state_.baselineTime = run_.time;
        state_.checkFile = rel;   // the baseline is also the first check
        state_.checkTime = run_.time;
        state_.checkPassed = true;
        state_.unitsSinceCheck = 0;
    } else if (run_.kind == QLatin1String("check")) {
        state_.checkFile = rel;
        state_.checkTime = run_.time;
        state_.checkPassed = cmp.pass;
        state_.unitsSinceCheck = 0;
    } else {
        ++state_.unitsSinceCheck;
    }
    store_.saveState(state_);
    store_.appendJournal(run_, verdict, cmp, base + QStringLiteral(".json"));

    QVariantList checks;
    for (const Check& c : cmp.checks) {
        checks.append(QVariantMap{ { "id", c.id }, { "side", c.side }, { "value", c.value }, { "limit", c.limit },
                                   { "unit", c.unit }, { "level", c.level }, { "detail", c.detail } });
    }
    result_ = QVariantMap{
        { "kind", run_.kind }, { "verdict", verdict }, { "serial", double(run_.serial) }, { "time", run_.time },
        { "firmware", run_.firmware }, { "file", rel }, { "saved", saved }, { "error", err }, { "checks", checks },
        { "warnings", cmp.warnings },
        { "supply", run_.supply.valid() ? QVariant(run_.supply.toJson().toVariantMap()) : QVariant() },
        { "temperature", run_.temperature.valid() ? QVariant(run_.temperature.toJson().toVariantMap()) : QVariant() },
        { "curves", curvesOf(run_, run_.kind == QLatin1String("baseline") ? nullptr : &reference_) } };
    setStatus(saved ? tr("Saved: %1").arg(rel) : tr("Not saved: %1").arg(err));
    endRun();
    emit stateChanged();
    emit resultChanged();
    emit runEnded(true);
}

void SonarQaRunner::abort(const QString& why)
{
    if (!busy()) {
        return;
    }
    pingTimer_.stop();
    if (adapter_) {
        adapter_->restore(snapshot_);
    }
    raw_.close();
    QFile::remove(rawPath_);
    rawPath_.clear();
    setStatus(why);
    endRun();
    emit runEnded(false);
}

void SonarQaRunner::endRun()
{
    phase_ = Phase::Idle;
    emit busyChanged();
    emit progressChanged();
    refreshDevice();
}

void SonarQaRunner::setStatus(const QString& s)
{
    if (s != status_) {
        status_ = s;
        emit progressChanged();
    }
}

QVariantMap SonarQaRunner::curvesOf(const Run& unit, const Run* ref) const
{
    QVariantList freqs, sides;
    for (const Step& st : unit.steps) {
        freqs.append(st.freqKhz);
    }
    const int nSides = unit.steps.isEmpty() ? 0 : unit.steps.first().sides.size();
    for (int s = 0; s < nSides; ++s) {
        QVector<float> ue, un, ur, re, rn, rr;
        for (int i = 0; i < unit.steps.size(); ++i) {
            const SideStep& u = unit.steps[i].sides[s];
            ue.append(u.echo1Amp());
            un.append(u.noiseP50);
            ur.append(u.ringdown);
            if (ref && i < ref->steps.size() && s < ref->steps[i].sides.size()) {
                const SideStep& r = ref->steps[i].sides[s];
                re.append(r.echo1Amp());
                rn.append(r.noiseP50);
                rr.append(r.ringdown);
            }
        }
        sides.append(QVariantMap{ { "echo", toList(ue) }, { "noise", toList(un) }, { "ringdown", toList(ur) },
                                  { "refEcho", toList(re) }, { "refNoise", toList(rn) }, { "refRingdown", toList(rr) } });
    }
    return QVariantMap{ { "freqs", freqs }, { "sides", sides } };
}
