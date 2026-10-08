#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>

#include "proto_binnary.h"

/**
 * @brief Finds out how a ring sonar behind a recorder takes commands (docs/RING_UPDATE.md).
 *
 * Upward the recorder passes a ring board's own bus address as its route, but a board with no address
 * (0) gets the recorder's port number instead, and the app cannot tell which one it got. A ring board
 * runs a command wrapped in ID_NODE_CMD only when the wrapper names its address, so a wrapper naming a
 * port number reaches nobody; a board with no address takes a plain command on any route.
 *
 * The probe asks GET ID_UART v1 -- the board's bus address, read only -- wrapped and naming `address`,
 * then plain on `route`; whichever is answered is the mode. A ring board relays the host's own frames
 * back, so only a CONTENT counts. Both requests ask the same thing, so after each wait comes a drain in
 * which answers are dropped: a late answer cannot be taken for the other request's unless it is later
 * than kAnswerMs + kDrainMs. kRounds rounds, then it gives up.
 *
 * Transport-free, like BusAddress: the owner carries every frame of send() to the line, feeds every
 * complete frame of this device to onFrame() and calls tick() every few hundred milliseconds or less.
 */
class NodeProbe : public QObject
{
    Q_OBJECT

public:
    enum class Mode { Unknown, Wrapped, Plain };

    struct Io {
        std::function<void(Parsers::ProtoBinOut)> send;   ///< a frame to the line, as it is
        std::function<qint64()> nowMs;                    ///< monotonic milliseconds
    };

    static constexpr int kSettleMs = 200;   ///< answers to what was asked before the probe drain first
    static constexpr int kAnswerMs = 600;   ///< the wait for each request
    static constexpr int kDrainMs  = 400;   ///< after each wait, answers dropped
    static constexpr int kRounds   = 3;

    explicit NodeProbe(Io io, QObject* parent = nullptr);

    /// Starts on `route`, the device's route as the app sees it, naming `address` in the wrapper.
    /// False when a run is going on.
    bool start(uint8_t route, uint8_t address);
    bool active() const { return phase_ != Phase::Idle; }

    /// A complete frame of this device. `boardAddress`: the address an ID_UART v1 CONTENT reports, else -1.
    void onFrame(uint8_t id, uint8_t ver, uint8_t type, int boardAddress);
    void tick();

signals:
    /// Unknown: neither request was answered in kRounds rounds. `boardAddress`: as reported, -1 = nothing.
    void finished(NodeProbe::Mode mode, int boardAddress, const QString& detail);
    void logLine(const QString& line);

private:
    enum class Phase { Idle, Settle, Wrapped, DrainWrapped, Plain, DrainPlain };

    void ask(bool wrapped);
    void finish(Mode mode, int boardAddress, const QString& detail);

    Io io_;
    Phase phase_ = Phase::Idle;
    uint8_t route_ = 0, address_ = 0;
    int round_ = 0;
    qint64 due_ = 0;
};

/// The command fits an ID_NODE_CMD wrapper: SETTING or GETTING with at most 61 bytes of payload -- the
/// ring firmware runs a wrapped command within its 64.
bool canWrapNodeCmd(Parsers::ProtoBinOut& inner);
/// ID_NODE_CMD 0x7E on route 0 with the payload [address][mode][id][data]: the inner command, for the
/// board named. The recorder hands route 0 to its sonar ports.
Parsers::ProtoBinOut wrapNodeCmd(Parsers::ProtoBinOut& inner, uint8_t address);
