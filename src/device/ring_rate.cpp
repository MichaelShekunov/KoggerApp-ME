#include "ring_rate.h"

#include "ring_upgrade.h"

#include <QStringList>

namespace {

constexpr uint8_t kIdDataset = 0x10, kIdUart = 0x18, kIdFlash = 0x23;
constexpr uint8_t kSetResp = 0x82, kSetV0 = 0x02, kSetV1 = 0x0A, kGet = 0x03;   // mode: type, version in bits 3..5
constexpr uint32_t kKey = 0xC96B5D4Au;
constexpr int kSettleMs = 150;      // answers to requests sent before the start arrive and are dropped
constexpr int kAnswerMs = 300;
constexpr int kDatasetTries = 3;   // a busy slow line may lose an answer of the first board
constexpr int kDrainMs = 400;       // the last chart of each board is out before the rate changes
constexpr int kSwitchMs = 60;       // acknowledgement at the old rate, transmitter drained, rate switched
constexpr int kRestoreMs = 150;
constexpr int kSaveMs = 500;
constexpr int kReloadMs = 300;

QByteArray keyBytes()
{
    QByteArray k(4, 0);
    for (int i = 0; i < 4; ++i)
        k[i] = char((kKey >> (8 * i)) & 0xFF);
    return k;
}

QByteArray u32le(uint32_t v)
{
    QByteArray b(4, 0);
    for (int i = 0; i < 4; ++i)
        b[i] = char((v >> (8 * i)) & 0xFF);
    return b;
}

uint32_t readU32(const QByteArray& b, int o)
{
    return uint32_t(uint8_t(b[o])) | (uint32_t(uint8_t(b[o + 1])) << 8) | (uint32_t(uint8_t(b[o + 2])) << 16)
         | (uint32_t(uint8_t(b[o + 3])) << 24);
}

} // namespace

RingRate::RingRate(Io io, QObject* parent) : QObject(parent), io_(std::move(io)) {}

QList<int> RingRate::rates()
{
    return { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1200000, 2000000 };
}

bool RingRate::start(const Plan& plan)
{
    auto refuse = [this](const QString& why) {
        emit finished(false, plan_.from, why);
        return false;
    };
    if (active())
        return refuse(QStringLiteral("a line rate change is running already"));
    plan_ = plan;
    if (plan.boards.isEmpty() || plan.boards.size() > 2)
        return refuse(QStringLiteral("one or two boards on the line, not %1").arg(plan.boards.size()));
    for (int b : plan.boards)
        if (b < 1 || b > 15)
            return refuse(QStringLiteral("board address %1 is out of 1..15").arg(b));
    if (plan.boards.size() == 2 && plan.boards[0] >= plan.boards[1])
        return refuse(QStringLiteral("boards must be in ring order, the smaller address first"));
    if (!rates().contains(plan.to))
        return refuse(QStringLiteral("%1 is not a rate the ring firmware takes").arg(plan.to));
    if (plan.from <= 0)
        return refuse(QStringLiteral("the line's present rate is not known"));

    step_ = 0;
    asked_ = false;
    switched_ = false;
    datasets_.clear();
    lastDataset_.clear();
    reported_.clear();
    originals_.clear();
    redone_ = false;
    log(QStringLiteral("line rate %1 -> %2, boards %3").arg(plan.from).arg(plan.to).arg(
        plan.boards.size() == 2 ? QStringLiteral("%1 (first), %2 (last)").arg(plan.boards[0]).arg(plan.boards[1])
                                : QStringLiteral("%1 (alone)").arg(plan.boards[0])));
    next(Phase::Settle, kSettleMs);
    return true;
}

void RingRate::onFrame(const QByteArray& f)
{
    if (!active() || f.size() < 8 || uint8_t(f[0]) != 0xBB || uint8_t(f[1]) != 0x55)
        return;
    const int n = uint8_t(f[5]);
    if (f.size() < 8 + n)
        return;
    const int route = uint8_t(f[2]), mode = uint8_t(f[3]), id = uint8_t(f[4]);
    if ((mode & 3) != 1 || ((mode >> 3) & 7) != 0)     // CONTENT v0 only: the boards' answers, not relayed commands
        return;
    const QByteArray pl = f.mid(6, n);
    if (id == kIdDataset)
        lastDataset_[route] = pl;
    else if (id == kIdUart && n >= 4)
        reported_[route] = int(readU32(pl, 0));
}

void RingRate::tick()
{
    if (!active() || now() < due_)
        return;
    const QList<int>& boards = plan_.boards;
    switch (phase_) {
    case Phase::Settle:
        step_ = 0;
        tries_ = 0;
        askDataset();
        return next(Phase::PingsAsk, kAnswerMs);

    case Phase::PingsAsk: {
        const int board = pingBoard();
        if (!lastDataset_.contains(board)) {
            if (++tries_ < kDatasetTries) {
                askDataset();
                return next(Phase::PingsAsk, kAnswerMs);
            }
            return fail(QStringLiteral("board %1 did not answer ID_DATASET at %2 (%3 asks)").arg(board).arg(plan_.from).arg(kDatasetTries));
        }
        datasets_.append({board, lastDataset_.value(board)});
        originals_[board] = lastDataset_.value(board);
        send(uint8_t(board), kSetResp, kIdDataset, QByteArray(1, char(1)) + u32le(0) + u32le(0));   // pings off
        if (++step_ < boards.size()) {
            tries_ = 0;
            askDataset();
            return next(Phase::PingsAsk, kAnswerMs);
        }
        return next(Phase::Drain, kDrainMs);
    }

    case Phase::Drain:
        // The last board first: the first one still relays the command to it at the old rate.
        send(uint8_t(boards.last()), kSetResp, kIdUart, keyBytes() + QByteArray(1, char(1)) + u32le(uint32_t(plan_.to)));
        switched_ = true;
        return next(Phase::SetLast, kSwitchMs);

    case Phase::SetLast:
        if (boards.size() == 2) {
            // Its acknowledgement goes to the last board, which listens at the new rate already: it is lost.
            send(uint8_t(boards.first()), kSetResp, kIdUart, keyBytes() + QByteArray(1, char(1)) + u32le(uint32_t(plan_.to)));
            return next(Phase::SetFirst, kSwitchMs);
        }
        [[fallthrough]];
    case Phase::SetFirst:
        io_.setRate(plan_.to);
        return next(Phase::HostRate, kSwitchMs);

    case Phase::HostRate:
        askRates();
        return next(Phase::Verify, kAnswerMs + 100);

    case Phase::Verify: {
        for (int b : boards)
            if (reported_.value(b, 0) != plan_.to)
                return fail(QStringLiteral("at %1 the line does not answer as it should (%2)").arg(plan_.to).arg(ratesText()));
        log(QStringLiteral("every board answers at %1").arg(plan_.to));
        datasets_.clear();
        step_ = 0;
        redone_ = false;
        return restoreBoard();
    }

    // One board at a time, the first one first: while its pings come back and it saves and reloads, the last
    // board is still quiet and relays its answers -- a pinging last board loses them on a slow line.
    case Phase::Restore:
        send(uint8_t(boards[step_]), kSetV0, kIdFlash, keyBytes());              // save the settings
        return next(Phase::Save, kSaveMs);

    case Phase::Save:
        send(uint8_t(boards[step_]), kSetV1, kIdFlash, keyBytes());              // reload them: the rate comes from flash
        tries_ = 0;
        return next(Phase::Reload, kReloadMs);

    case Phase::Reload:
        askBoard(boards[step_]);
        return next(Phase::Check, kAnswerMs + 100);

    case Phase::Check: {
        const int b = boards[step_];
        if (!reported_.contains(b) || !lastDataset_.contains(b)) {
            if (++tries_ < kDatasetTries) {
                askBoard(b);
                return next(Phase::Check, kAnswerMs + 100);
            }
            return finish(false, plan_.to, QStringLiteral("the line runs at %1, but board %2 does not answer after its settings "
                                                          "were saved").arg(plan_.to).arg(b));
        }
        if (reported_.value(b) != plan_.to)
            return finish(false, plan_.to,
                          QStringLiteral("the line runs at %1, but board %2 did not keep it: from flash it reports %3. "
                                         "After a power cycle it comes back at that rate").arg(plan_.to).arg(b).arg(reported_.value(b)));
        if (lastDataset_.value(b) != originals_.value(b)) {
            if (!redone_) {                                                      // its pings setting was lost: once more
                redone_ = true;
                return restoreBoard();
            }
            return finish(false, plan_.to, QStringLiteral("the line runs at %1, but board %2 did not keep its pings: set Period "
                                                          "and Echogram on its page and save them").arg(plan_.to).arg(b));
        }
        if (++step_ < boards.size()) {
            redone_ = false;
            return restoreBoard();
        }
        return finish(true, plan_.to, QStringLiteral("the line runs at %1 and every board keeps it").arg(plan_.to));
    }

    case Phase::Idle:
        return;
    }
}

void RingRate::send(uint8_t route, uint8_t mode, uint8_t id, const QByteArray& payload)
{
    io_.write(RingUpgrade::kp1(route, mode, id, payload));
}

void RingRate::askDataset()
{
    lastDataset_.clear();
    send(uint8_t(pingBoard()), kGet, kIdDataset);
}

void RingRate::restoreBoard()
{
    const int b = plan_.boards[step_];
    send(uint8_t(b), kSetResp, kIdDataset, originals_.value(b));               // pings as they were
    next(Phase::Restore, kRestoreMs);
}

void RingRate::askBoard(int board)
{
    reported_.remove(board);
    lastDataset_.remove(board);
    send(uint8_t(board), kGet, kIdUart);
    send(uint8_t(board), kGet, kIdDataset);
}

void RingRate::askRates()
{
    reported_.clear();
    for (int b : plan_.boards)
        send(uint8_t(b), kGet, kIdUart);
}

QString RingRate::ratesText() const
{
    QStringList s;
    for (int b : plan_.boards)
        s << QStringLiteral("board %1: %2").arg(b).arg(reported_.contains(b) ? QString::number(reported_.value(b))
                                                                               : QStringLiteral("no answer"));
    return s.join(QStringLiteral(", "));
}

void RingRate::next(Phase p, int ms)
{
    phase_ = p;
    due_ = now() + ms;
}

void RingRate::fail(const QString& detail)
{
    if (!switched_) {
        for (const auto& d : datasets_)                                          // only the pings were touched
            send(uint8_t(d.first), kSetResp, kIdDataset, d.second);
        datasets_.clear();
        return finish(false, plan_.from, detail + QStringLiteral("; nothing was changed"));
    }
    io_.setRate(plan_.from);
    finish(false, plan_.from, detail + QStringLiteral(". Nothing was saved: power both sonars off and on, they come back at %1")
                                           .arg(plan_.from));
}

void RingRate::finish(bool ok, int rate, const QString& detail)
{
    phase_ = Phase::Idle;
    log(QStringLiteral("%1: %2").arg(ok ? QStringLiteral("done") : QStringLiteral("failed"), detail));
    emit finished(ok, rate, detail);
}

void RingRate::log(const QString& s)
{
    emit logLine(QStringLiteral("ring-rate: ") + s);
}
