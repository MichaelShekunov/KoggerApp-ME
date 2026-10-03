#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

#include <cstdint>
#include <functional>

/**
 * @brief Moves every sonar of a ring line -- and the host's port -- to another UART rate, and saves it.
 *
 * A ring board's RX and TX are one UART (USART1) with one rate register, and each board passes the other's
 * bytes on as they are, so the whole line runs at one rate: it is chosen for the longest wire. Order matters,
 * the path to every board must work at the moment its command goes out (docs/RING_UPDATE.md, "Скорость
 * линии кольца"):
 *  1. each board's ID_DATASET is read and its pings stopped -- the line is quiet, no sync BREAK at a rate
 *     the other board does not run at;
 *  2. ID_UART v0 to the LAST board (it acknowledges at the old rate, then switches), then to the FIRST
 *     board (its acknowledgement is lost: the last board already listens at the new rate), then the host;
 *  3. both boards must answer a GET of ID_UART v0 at the new rate, reporting it;
 *  4. pings back as they were, then ID_FLASH v0 on each board;
 *  5. proof: ID_FLASH v1 reloads the settings from flash -- the firmware takes the rate from there but
 *     applies it only at boot -- and a GET of ID_UART v0 must report the new rate again.
 * Nothing is saved before step 3 holds; a failed run leaves the old rate in flash, so a power cycle brings
 * the boards back at it. A lone board (one entry) goes the same way without the second.
 *
 * Transport-free, like RingUpgrade: the owner feeds every complete KP1 frame read from the line to
 * onFrame(), carries the bytes and rate changes to the line and calls tick() every few milliseconds.
 */
class RingRate : public QObject
{
    Q_OBJECT

public:
    struct Io {
        std::function<void(const QByteArray&)> write;   ///< bytes to the line, in order
        std::function<void(int)> setRate;               ///< host-side line rate; bytes written before it are out
        std::function<qint64()> nowMs;                  ///< monotonic milliseconds
    };

    struct Plan {
        QList<int> boards;     ///< bus addresses in ring order (the smaller address is the first board); one = lone
        int from = 921600;     ///< rate the line runs at now
        int to = 0;            ///< rate to move to; one of rates()
    };

    explicit RingRate(Io io, QObject* parent = nullptr);

    /// Rates the ring firmware takes (its BAUDS table, 9600..2000000).
    static QList<int> rates();

    /// Checks the plan and starts. Returns false, and emits finished(false, ...), when it cannot be carried
    /// out; nothing has been sent then.
    bool start(const Plan& plan);
    bool active() const { return phase_ != Phase::Idle; }

    /// A complete KP1 frame read from the line, header to checksum.
    void onFrame(const QByteArray& frame);
    /// Drives every step. Call every 5..10 ms while active().
    void tick();

signals:
    /// ok: every board runs and keeps `rate`, the host is on it. Not ok: `rate` is where the host was left.
    void finished(bool ok, int rate, const QString& detail);
    void logLine(const QString& line);

private:
    enum class Phase { Idle, Settle, PingsAsk, Drain, SetLast, SetFirst, HostRate, Verify, Restore, Save, Prove, Check };

    void send(uint8_t route, uint8_t mode, uint8_t id, const QByteArray& payload = {});
    void askRates();                        ///< GET ID_UART v0 to every board; answers collected from here on
    QString ratesText() const;
    void next(Phase p, int ms);
    void fail(const QString& detail);
    void finish(bool ok, int rate, const QString& detail);
    void log(const QString& s);
    qint64 now() const { return io_.nowMs(); }

    Io io_;
    Plan plan_;
    Phase phase_ = Phase::Idle;
    qint64 due_ = 0;
    int step_ = 0;
    bool asked_ = false;
    bool switched_ = false;                 ///< a rate change went out: the boards may be off the old rate
    QList<QPair<int, QByteArray>> datasets_;    ///< boards whose pings were stopped, and the setting to restore
    QHash<int, QByteArray> lastDataset_;    ///< ID_DATASET answers since the last ask, by route
    QHash<int, int> reported_;              ///< ID_UART v0 answers since the last ask: route -> rate
};
