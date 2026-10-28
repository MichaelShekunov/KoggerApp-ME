#pragma once

#include "sonar_qa_analysis.h"
#include "sonar_qa_store.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class SonarQaDevice;

/* The acceptance run of a nanoSSS on the water stand (docs/SONAR_QA.md), for the developer-mode window
   SonarQaWindow.qml. Three kinds of run, the same sweep each:
     baseline  the reference sonar's first run: its settings and sweep become the stand's yardstick
     check     the reference again: compared with the baseline (has the stand moved?); allows unitsPerCheck sonars
     unit      a sonar under test: compared with the reference's latest check, saved by its serial number
   Every step of the sweep is measured with the transmitter off (noise) and on (ringing and reflections); the sonar's
   own settings are put back when the run ends or stops. */
class SonarQaRunner : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* device READ device WRITE setDevice NOTIFY deviceChanged)
    Q_PROPERTY(QVariantMap deviceInfo READ deviceInfo NOTIFY deviceChanged)
    Q_PROPERTY(QString serialEntry READ serialEntry WRITE setSerialEntry NOTIFY serialEntryChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString status READ status NOTIFY progressChanged)
    Q_PROPERTY(int etaSec READ etaSec NOTIFY progressChanged)
    Q_PROPERTY(QVariantMap reference READ reference NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap params READ params WRITE setParams NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap tolerances READ tolerances WRITE setTolerances NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap result READ result NOTIFY resultChanged)
    Q_PROPERTY(QVariantList journal READ journal NOTIFY resultChanged)
    Q_PROPERTY(QVariantMap live READ live NOTIFY liveChanged)
    Q_PROPERTY(QString folder READ folder CONSTANT)
    Q_PROPERTY(QObject* window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(bool demoAvailable READ demoAvailable CONSTANT)

public:
    explicit SonarQaRunner(QObject* parent = nullptr);
    /// Runs and state under another folder than Documents/KoggerApp/sonar_qa (the offline check).
    explicit SonarQaRunner(const QString& root, QObject* parent = nullptr);
    ~SonarQaRunner() override;

    QObject* device() const { return deviceObject_; }
    /// A DevDriver from the device list; the runner talks to it through SonarQaDevDriver.
    void setDevice(QObject* device);
    /// Another sonar than a DevDriver (a simulated one); the runner does not own it.
    void setAdapter(SonarQaDevice* adapter);
    /// serial (as reported), name, firmware, board, supported (a nanoSSS), isReference; connected false without a
    /// sonar.
    QVariantMap deviceInfo() const { return deviceInfo_; }
    /// The number from the sonar's sticker, typed in the window; empty: the one the sonar reports. l412_adc reports
    /// the same number for every board, so with it only the typed number tells sonars apart. Cleared after a test.
    QString serialEntry() const { return serialEntry_; }
    void setSerialEntry(const QString& s);

    bool busy() const { return phase_ != Phase::Idle; }
    QString mode() const { return run_.kind; }
    double progress() const;
    QString status() const { return status_; }
    int etaSec() const;
    /// set, serial, reported, baselineTime, checkTime, checkPassed, unitsSinceCheck, unitsPerCheck, unitsLeft.
    QVariantMap reference() const;
    QVariantMap params() const;
    void setParams(const QVariantMap& p);
    QVariantMap tolerances() const;
    void setTolerances(const QVariantMap& t);
    /// The last finished run: kind, verdict, serial, time, file, checks, curves, supply, temperature.
    QVariantMap result() const { return result_; }
    QVariantList journal() const { return store_.journal(30); }
    /// The latest ping, per side, thinned for a small plot: sides, freqKhz, pulses, supply, temperature.
    QVariantMap live() const { return live_; }
    QString folder() const { return store_.root(); }
    /// The open window of the application's one runner (the QML singleton SonarQa): a second "open" raises it.
    QObject* window() const { return window_; }
    void setWindow(QObject* w);

    /// Empty when a run of this kind ("baseline", "check", "unit") can start now, else why not.
    Q_INVOKABLE QString canStart(const QString& kind);
    Q_INVOKABLE bool start(const QString& kind);
    Q_INVOKABLE void stop();
    /// Lets sonars be tested against a reference check that drifted from the baseline.
    Q_INVOKABLE void acceptCheck();
    /// Reads the sonar's identity again (it is known only once the sonar has answered).
    Q_INVOKABLE void refreshDevice();
    /// The owner's sweep (2026-10-28) and the starting tolerances.
    Q_INVOKABLE void resetParams();
    Q_INVOKABLE void openFolder() const;
    /// KOGGER_SONARQA_DEMO set ("fast": no waiting between pings): the simulated sonar can be chosen instead of a
    /// real one, to check the window without a stand.
    bool demoAvailable() const;
    /// The simulated sonar as the device: serial 1001 (the reference) or another, good or with one fault.
    Q_INVOKABLE void useDemo(int serial, const QString& fault);

signals:
    void deviceChanged();
    void serialEntryChanged();
    void busyChanged();
    void progressChanged();
    void stateChanged();
    void resultChanged();
    void liveChanged();
    void windowChanged();
    /// A run ended: finished true with a result, false when it stopped or failed (status says why).
    void runEnded(bool finished);

private:
    enum class Phase { Idle, Warmup, Discard, Collect };

    void attach(SonarQaDevice* a);
    void onPing(const QVector<QVector<uint8_t>>& sides, float resolutionM);
    void beginStep();
    void setTransmitterState(int txState);
    void finishStep();
    void finish();
    void abort(const QString& why);
    void endRun();
    void setStatus(const QString& s);
    QVariantMap curvesOf(const sonarqa::Run& unit, const sonarqa::Run* ref) const;
    /// The number a run of this kind is filed under: typed, the reference's for its check, or as reported.
    QString serialFor(const QString& kind) const;

    sonarqa::Store store_;
    sonarqa::State state_;
    QPointer<QObject> deviceObject_;
    QPointer<QObject> window_;
    QPointer<SonarQaDevice> adapter_;
    bool ownsAdapter_ = false;
    QVariantMap deviceInfo_;
    quint32 reported_ = 0;
    QString serialEntry_;

    Phase phase_ = Phase::Idle;
    sonarqa::Run run_;
    sonarqa::Run reference_;
    QString referenceFile_;
    QVariantMap snapshot_;
    int step_ = 0, txState_ = 0, discard_ = 0, collected_ = 0, warm_ = 0, warmSize_ = -1;
    int pingsDone_ = 0, pingIndex_ = 0;
    QVector<QVector<sonarqa::PingMetrics>> off_, on_;
    QVector<QVector<QVector<uint8_t>>> offRaw_, onRaw_;   ///< this step's pings, transmitter off and on, per side
    QVector<QVector<QVector<uint8_t>>> profiles_;         ///< the median echogram of each step done, per side
    QVector<float> supply_, temperature_;
    sonarqa::RawWriter raw_;
    QString rawPath_;
    QTimer pingTimer_;
    QElapsedTimer clock_;
    QString status_;
    QVariantMap result_, live_;
};
