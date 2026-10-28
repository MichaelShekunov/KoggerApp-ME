#pragma once

#include "sonar_qa_analysis.h"

#include <QFile>
#include <QString>
#include <QVariantList>

/* Where the acceptance runs are kept (docs/SONAR_QA.md):
     <root>/reference/<serial>/<yyyyMMdd-HHmmss>_<baseline|check>.json   the reference sonar's runs
     <root>/units/<serial>/<yyyyMMdd-HHmmss>_<pass|fail>.json            each sonar tested, by serial number
     next to each .json a .ksqa with every ping measured (RawWriter)
     <root>/journal.csv                                                  one line a run
     <root>/state.json                                                   the reference, its last check, the count
   The default root is Documents/KoggerApp/sonar_qa. */
namespace sonarqa {

/// The reference and the count of sonars tested against its last check.
struct State {
    QString referenceSerial;       ///< the reference's number as filed (typed from its sticker, or as reported)
    quint32 referenceReported = 0; ///< the number the reference itself reports
    QString baselineFile;          ///< the reference's first run: the stand's long-term yardstick
    QString baselineTime;
    QString checkFile;             ///< its latest check: what sonars are compared with
    QString checkTime;
    bool checkPassed = false;      ///< the latest check agreed with the baseline (or was accepted anyway)
    int unitsSinceCheck = 0;
    int unitsPerCheck = 10;
    Params params;
    Tolerances tolerances;
    QJsonObject toJson() const;
    static State fromJson(const QJsonObject& o);
};

class Store
{
public:
    explicit Store(const QString& root = defaultRoot());
    static QString defaultRoot();
    QString root() const { return root_; }

    State loadState() const;
    bool saveState(const State& s) const;

    /// The path a run's files take (without the extension), its folder made.
    QString newRunPath(const Run& run, const QString& verdict) const;
    /// Writes the run, its comparison and the reference it was compared with as one JSON document.
    bool saveRun(const QString& basePath, const Run& run, const QJsonObject& comparison, const QString& referenceFile,
                 QString* error) const;
    bool loadRun(const QString& jsonPath, Run* run, QString* error) const;
    /// Loads a run and measures it again from the raw pings beside it (a run of an older format): steps, and the
    /// reflections at its own places. Identity, settings, supply and temperature stay as saved.
    bool rebuildRun(const QString& jsonPath, Run* run, QString* error) const;
    /// A serial number as a folder name: letters, digits, '-' and '_' kept, anything else '_'.
    static QString folderName(const QString& serial);

    /// Adds a line to journal.csv (made with a header the first time).
    void appendJournal(const Run& run, const QString& verdict, const Comparison& cmp, const QString& file) const;
    /// The last lines of the journal, newest first, each a map of its columns.
    QVariantList journal(int last) const;

private:
    QString root_;
};

/// Every measured ping of a run: "KSQA" u32 version, u32 header length, the run's params as JSON, then records of
/// u16 frequency kHz, u8 pulses, u8 ping index, u8 side, u16 sample count and the samples. Little-endian.
struct RawPing {
    int freqKhz = 0, pulses = 0, ping = 0, side = 0;
    QVector<uint8_t> samples;
};
bool readRaw(const QString& path, QJsonObject* header, QVector<RawPing>* pings, QString* error);

/// The steps of a run from its raw pings (as RawWriter wrote them), the reflections read at ref's places, or at its
/// own when ref is null.
void buildSteps(Run& run, const QVector<RawPing>& pings, const Run* ref);

class RawWriter
{
public:
    bool open(const QString& path, const QJsonObject& header);
    void write(int freqKhz, int pulses, int ping, int side, const QVector<uint8_t>& samples);
    void close();
    bool isOpen() const { return file_.isOpen(); }

private:
    QFile file_;
};

} // namespace sonarqa
