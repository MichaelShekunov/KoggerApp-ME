#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>

/**
 * @brief Sets the bus address of the one sonar on a line and proves it was saved: the sequence of
 * tools/bench/chain_addr.py in the firmware tree, which numbered the boards of the bench ring.
 *  1. A broadcast GET of ID_UART v1 must be answered exactly once. With two boards on the line both
 *     may take the address. The count starts after a short pause: an answer to a request someone else
 *     sent just before (the app polls its devices) must not be taken for a second board.
 *  2. SET ID_UART v1 [key][1][address] on route 0; a GET on the new route must read the address back.
 *  3. ID_FLASH v0 on the new route saves the settings, the address with them.
 *  4. Proof: the address in RAM is moved away (to 0, or to 15 when 0 is being set), ID_FLASH v1
 *     restores the settings from flash, and a GET on the new route must read the address again. An
 *     unsaved address cannot come back.
 * The ring firmware (1.12+) answers a SET or GET of ID_UART v1 with CONTENT v1 [key][1][address] from
 * its route after the change; at address 0 it takes a command on any route. docs/RING_UPDATE.md.
 *
 * Transport-free, like RingUpgrade: the owner feeds every complete KP1 frame read from the line to
 * onFrame(), carries the bytes to the line and calls tick() every few milliseconds.
 */
class BusAddress : public QObject
{
    Q_OBJECT

public:
    struct Io {
        std::function<void(const QByteArray&)> write;   ///< bytes to the line, in order
        std::function<qint64()> nowMs;                  ///< monotonic milliseconds
    };

    explicit BusAddress(Io io, QObject* parent = nullptr);

    /// Starts setting `address` (0..15). Returns false, and emits finished(false, ...), when it is out
    /// of range or a run is going on; nothing has been sent then.
    bool start(int address);
    bool active() const { return phase_ != Phase::Idle; }

    /// A complete KP1 frame read from the line, header to checksum.
    void onFrame(const QByteArray& frame);
    /// Drives every step. Call every 5..10 ms while active().
    void tick();

signals:
    /// ok: the board answers on `address` and the address came back from flash. Not ok: `address` is
    /// what the board reported last (-1 = nothing), and detail says what went wrong.
    void finished(bool ok, int address, const QString& detail);
    void logLine(const QString& line);

private:
    enum class Phase { Idle, Settle, Count, Set, ReadBack, Save, MoveAway, Restore, Prove };

    void send(uint8_t route, uint8_t mode, uint8_t id, const QByteArray& payload);
    void ask(uint8_t route);            ///< GET ID_UART v1; the answers are collected from here on
    void finish(bool ok, int address, const QString& detail);
    void log(const QString& s);

    Io io_;
    Phase phase_ = Phase::Idle;
    int target_ = 0, temp_ = 0;
    qint64 due_ = 0;
    QList<int> answers_;                ///< addresses in the ID_UART v1 answers since the last ask()
};
