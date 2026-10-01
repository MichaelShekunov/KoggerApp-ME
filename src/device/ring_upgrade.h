#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>

/**
 * @brief Firmware update of sonars that share one host line as a ring -- host TX -> first board ->
 * last board -> host RX -- one board at a time, or all of them in one run. Also a lone board with a
 * bus address, which the stock flow cannot update either.
 *
 * The wire procedure is the stock one (docs/firmware-update-protocol.md): ID_BOOT v0 puts the board
 * into its bootloader, ID_UPDATE v0 carries the image in 96-byte chunks numbered from 1, ID_BOOT v1
 * runs it, ID_VERSION v2 tells the version. What the ring adds is in docs/RING_UPDATE.md:
 *  - every frame for a bootloader goes to route 16: the bootloader takes it, a ring board (bus
 *    address 1..15) only relays it; the bootloader answers on route 0, which the last board relays;
 *  - the bootloader starts at 115200 and is moved to the line rate (ID_UART v0) before any chunk.
 *    For the LAST board the first one drops to 115200 for that moment, so the host hears the
 *    bootloader directly; for the FIRST board the host sends the rate change blind and listens
 *    through the last one;
 *  - pings are stopped on every board for the run -- no sync BREAK may reach a bootloader -- and
 *    restored afterwards.
 * No chunk goes out before the bootloader answers at the line rate: a failure up to that point
 * leaves the board to start its old firmware by itself after the bootloader's ~3 s window. After it,
 * the board stays in its bootloader.
 *
 * Transport-free: the owner feeds every complete KP1 frame read from the line to onFrame(), carries
 * the bytes and rate changes it asks for to the line, and calls tick() every few milliseconds. Ring
 * order is by address: the smaller address is the first board.
 */
class RingUpgrade : public QObject
{
    Q_OBJECT

public:
    struct Io {
        std::function<void(const QByteArray&)> write;   ///< bytes to the line, in order
        std::function<void(int)> setRate;               ///< host-side line rate; bytes written before it are out
        std::function<qint64()> nowMs;                  ///< monotonic milliseconds
    };

    struct Plan {
        QList<int> boards;     ///< bus addresses of the boards on the line, 1..15 each; one entry = a lone board
        QList<int> targets;    ///< addresses to update, in that order
        QByteArray image;      ///< the .ufw file as it is
        QString expect;        ///< "1.12": version each board must report afterwards; empty = not checked
        int lineRate = 921600; ///< rate the line and the boards' firmware run at
    };

    /// Status values in the stock convention (DevDriver::upgradeFWStatus): 0..100 progress, these at the end.
    static constexpr int kStatusOk = 101;
    static constexpr int kStatusFail = -1;

    explicit RingUpgrade(Io io, QObject* parent = nullptr);

    /// Checks the plan and starts the run. Returns false, and emits finished(false, ...), when the plan
    /// cannot be carried out; nothing has been sent then.
    bool start(const Plan& plan);
    bool active() const { return phase_ != Phase::Idle; }
    int currentTarget() const { return target_; }

    /// A complete KP1 frame read from the line, header to checksum.
    void onFrame(const QByteArray& frame);
    /// Drives every timeout. Call every 5..10 ms while active().
    void tick();

    /// Targets for "every board on the line": the last board first, so the board that relays the
    /// bootloader's answers for the first one already runs the new firmware when it is needed.
    static QList<int> allTargets(QList<int> boards);
    /// A KP1 frame: 0xBB 0x55 route mode id len payload check1 check2.
    static QByteArray kp1(uint8_t route, uint8_t mode, uint8_t id, const QByteArray& payload = {});
    /// "..._v1.12_..." -> "1.12": the version an image file's name says it carries; empty if it says none.
    static QString versionFromFileName(const QString& path);

signals:
    void targetProgress(int address, int percent);
    void targetFinished(int address, bool ok, const QString& detail);
    void finished(bool ok, const QString& detail);
    void logLine(const QString& line);

private:
    enum class Phase {
        Idle,
        PingsAsk,       ///< reading each board's ID_DATASET, then switching its pings off
        Boot,           ///< ID_BOOT v0 sent to the target
        LastSlow,       ///< last board: the first one told to drop to 115200
        LastPoll,       ///< ... looking for the bootloader at 115200
        LastUp,         ///< ... bootloader and first board moved back to the line rate
        LastVerify,     ///< ... the bootloader must answer at the line rate
        FirstFast,      ///< first / lone board: is the bootloader at the line rate yet?
        FirstSlow,      ///< ... at 115200: (lone board: find it) then the rate change
        Transfer,       ///< ID_UPDATE chunks
        RunFirmware,    ///< ID_BOOT v1 sent
        WaitFirmware,   ///< the target's firmware must answer with the expected version
        Restore         ///< pings back on, one board at a time
    };
    enum class Role { Lone, First, Last };

    struct Window {             ///< answers collected for one probe
        bool open = false;
        qint64 until = 0;
        int route = -1;         ///< the version of interest must come from this route (-1 = any)
        bool version = false, bootloader = false, firmware = false;
        QString fwVersion;
        QByteArray dataset;
    };

    static QString boardsText(const QList<int>& list);
    void openProbe(int route, int ms);          ///< ID_VERSION v0 + v2 + ID_CHART_SETUP to a route
    QString probeResult() const;                ///< "bootloader" | "firmware" | "none"
    void send(uint8_t route, uint8_t mode, uint8_t id, const QByteArray& pl = {});
    void sendRate(uint8_t route, int baud);     ///< ID_UART v0: KoggerApp "Set baudrate"
    void setRate(int baud);
    void at(qint64 delayMs) { due_ = now() + delayMs; }
    bool due() const { return now() >= due_; }
    qint64 now() const { return io_.nowMs(); }
    void log(const QString& s);

    void beginTarget();
    void beginTransfer();
    void sendChunk();
    void targetOk(const QString& detail);
    void fail(const QString& detail, bool chunksSent);
    void beginRestore(bool ok, const QString& detail, qint64 delayMs);
    void finish();

    Io io_;
    Plan plan_;
    Phase phase_ = Phase::Idle;
    Role role_ = Role::Lone;
    int target_ = -1, other_ = -1, nextTarget_ = 0;
    qint64 due_ = 0, bootAt_ = 0;
    int step_ = 0, tries_ = 0, rate_ = 0;
    Window win_;
    QList<QPair<int, QByteArray>> datasets_;    ///< boards whose pings were stopped, and the setting to restore
    // transfer
    int chunk_ = 1, offset_ = 0, resends_ = 0, shownPct_ = -1;
    bool waitingAck_ = false;
    qint64 chunkAt_ = 0, transferAt_ = 0;
    int keepRate_ = 0;                          ///< a lone board answered at its own saved rate: stay there
    // outcome
    bool booted_ = false;                       ///< the current target was sent to its bootloader
    bool runOk_ = true;
    QString runDetail_;
    QStringList done_;
};
