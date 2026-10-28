#include "sonar_qa_store.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>

#include <cmath>

namespace sonarqa {

namespace {

const char* const kJournalHeader = "time;kind;serial;firmware;verdict;warnings;echo_band_mean_db;resonance_khz;"
                                   "noise_db;ringdown_samples;supply_v;temperature_c;file";

void putLe(QByteArray& b, quint32 v, int bytes)
{
    for (int i = 0; i < bytes; ++i) {
        b.append(char((v >> (8 * i)) & 0xFF));
    }
}

// The worst figure of a check id over the sides, for the journal.
double worst(const Comparison& cmp, const QString& id)
{
    double w = 0;
    bool any = false;
    for (const Check& c : cmp.checks) {
        if (c.id == id && (!any || std::fabs(c.value) > std::fabs(w))) {
            w = c.value;
            any = true;
        }
    }
    return w;
}

} // namespace

QJsonObject State::toJson() const
{
    return { { "referenceSerial", double(referenceSerial) }, { "baselineFile", baselineFile }, { "baselineTime", baselineTime },
             { "checkFile", checkFile }, { "checkTime", checkTime }, { "checkPassed", checkPassed },
             { "unitsSinceCheck", unitsSinceCheck }, { "unitsPerCheck", unitsPerCheck },
             { "params", params.toJson() }, { "tolerances", tolerances.toJson() } };
}

State State::fromJson(const QJsonObject& o)
{
    State s;
    s.referenceSerial = quint32(o.value("referenceSerial").toDouble());
    s.baselineFile = o.value("baselineFile").toString();
    s.baselineTime = o.value("baselineTime").toString();
    s.checkFile = o.value("checkFile").toString();
    s.checkTime = o.value("checkTime").toString();
    s.checkPassed = o.value("checkPassed").toBool();
    s.unitsSinceCheck = o.value("unitsSinceCheck").toInt();
    s.unitsPerCheck = o.value("unitsPerCheck").toInt(10);
    if (o.contains("params")) {
        s.params = Params::fromJson(o.value("params").toObject());
    }
    if (o.contains("tolerances")) {
        s.tolerances = Tolerances::fromJson(o.value("tolerances").toObject());
    }
    return s;
}

Store::Store(const QString& root) : root_(root) {}

QString Store::defaultRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .filePath(QStringLiteral("KoggerApp/sonar_qa"));
}

State Store::loadState() const
{
    QFile f(QDir(root_).filePath(QStringLiteral("state.json")));
    if (!f.open(QIODevice::ReadOnly)) {
        return State();
    }
    return State::fromJson(QJsonDocument::fromJson(f.readAll()).object());
}

bool Store::saveState(const State& s) const
{
    QDir().mkpath(root_);
    QSaveFile f(QDir(root_).filePath(QStringLiteral("state.json")));
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(QJsonDocument(s.toJson()).toJson(QJsonDocument::Indented));
    return f.commit();
}

QString Store::newRunPath(const Run& run, const QString& verdict) const
{
    const bool reference = run.kind != QLatin1String("unit");
    const QString dir = QDir(root_).filePath(QStringLiteral("%1/%2").arg(reference ? "reference" : "units")
                                                 .arg(run.serial));
    QDir().mkpath(dir);
    const QString stamp = QDateTime::fromString(run.time, Qt::ISODate).toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString base = QDir(dir).filePath(QStringLiteral("%1_%2").arg(stamp, reference ? run.kind : verdict));
    for (int n = 2; QFileInfo::exists(base + QStringLiteral(".json")); ++n) {
        base = QDir(dir).filePath(QStringLiteral("%1_%2_%3").arg(stamp, reference ? run.kind : verdict).arg(n));
    }
    return base;
}

bool Store::saveRun(const QString& basePath, const Run& run, const QJsonObject& comparison,
                    const QString& referenceFile, QString* error) const
{
    QJsonObject o = run.toJson();
    if (!comparison.isEmpty()) {
        o.insert("comparison", comparison);
    }
    if (!referenceFile.isEmpty()) {
        o.insert("comparedWith", QDir(root_).relativeFilePath(referenceFile));
    }
    QSaveFile f(basePath + QStringLiteral(".json"));
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = f.errorString();
        }
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) {
            *error = f.errorString();
        }
        return false;
    }
    return true;
}

bool Store::loadRun(const QString& jsonPath, Run* run, QString* error) const
{
    QFile f(QDir(root_).filePath(jsonPath));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = f.errorString();
        }
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError) {
        if (error) {
            *error = pe.errorString();
        }
        return false;
    }
    *run = Run::fromJson(doc.object());
    return true;
}

void Store::appendJournal(const Run& run, const QString& verdict, const Comparison& cmp, const QString& file) const
{
    QDir().mkpath(root_);
    QFile f(QDir(root_).filePath(QStringLiteral("journal.csv")));
    const bool fresh = !f.exists();
    if (!f.open(QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream ts(&f);
    if (fresh) {
        ts << kJournalHeader << '\n';
    }
    auto num = [](double v) { return QString::number(v, 'f', 2); };
    ts << run.time << ';' << run.kind << ';' << run.serial << ';' << run.firmware << ';' << verdict << ';'
       << cmp.warnings << ';' << num(worst(cmp, "echo_band_mean")) << ';' << num(worst(cmp, "resonance")) << ';'
       << num(worst(cmp, "noise")) << ';' << num(worst(cmp, "ringdown")) << ';'
       << (run.supply.valid() ? num(run.supply.median) : QString()) << ';'
       << (run.temperature.valid() ? num(run.temperature.median) : QString()) << ';'
       << QDir(root_).relativeFilePath(file) << '\n';
}

QVariantList Store::journal(int last) const
{
    QVariantList out;
    QFile f(QDir(root_).filePath(QStringLiteral("journal.csv")));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return out;
    }
    const QStringList header = QString::fromLatin1(kJournalHeader).split(';');
    QStringList lines = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
    if (!lines.isEmpty()) {
        lines.removeFirst();
    }
    for (int i = lines.size() - 1; i >= 0 && out.size() < last; --i) {
        const QStringList cells = lines[i].split(';');
        QVariantMap m;
        for (int c = 0; c < header.size() && c < cells.size(); ++c) {
            m[header[c]] = cells[c];
        }
        out.append(m);
    }
    return out;
}

bool RawWriter::open(const QString& path, const QJsonObject& header)
{
    file_.setFileName(path);
    if (!file_.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray b("KSQA");
    putLe(b, 1, 4);
    putLe(b, quint32(json.size()), 4);
    b.append(json);
    file_.write(b);
    return true;
}

void RawWriter::write(int freqKhz, int pulses, int ping, int side, const QVector<uint8_t>& samples)
{
    if (!file_.isOpen()) {
        return;
    }
    QByteArray b;
    putLe(b, quint32(freqKhz), 2);
    putLe(b, quint32(pulses), 1);
    putLe(b, quint32(ping), 1);
    putLe(b, quint32(side), 1);
    putLe(b, quint32(samples.size()), 2);
    b.append(reinterpret_cast<const char*>(samples.constData()), samples.size());
    file_.write(b);
}

void RawWriter::close()
{
    if (file_.isOpen()) {
        file_.close();
    }
}

} // namespace sonarqa
