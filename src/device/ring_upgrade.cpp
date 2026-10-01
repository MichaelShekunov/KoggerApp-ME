#include "ring_upgrade.h"

#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace {

constexpr uint8_t kIdDataset = 0x10, kIdChartSetup = 0x12, kIdUart = 0x18, kIdVersion = 0x20,
                  kIdBoot = 0x24, kIdUpdate = 0x25;
constexpr uint8_t kSet = 0x82, kSetV1 = 0x8A, kGet = 0x03, kGetV2 = 0x13;   // SETTING asks for an answer, as KoggerApp does
constexpr uint8_t kLoaderRoute = 16;
constexpr int kChunk = 96;
constexpr int kLoaderRate = 115200;

// Timings. The bootloader answers ~0.4 s after the reset and runs the old firmware ~3 s after its
// start when no chunk has come; the first chunk is answered after ~1.2 s, the rest in ~10 ms.
constexpr int kSettleMs = 20;          // a frame written at one rate is out before the rate changes
constexpr int kBootQuietMs = 150;      // after ID_BOOT: the target has surely reset
constexpr int kBackMs = 4000;          // after ID_BOOT: a target left without chunks runs its firmware again
constexpr int kProbeMs = 80;
constexpr int kContactLastMs = 2200;   // from ID_BOOT: the bootloader must be found by then
constexpr int kContactFirstMs = 2400;
constexpr int kFirstAckMs = 5000;      // as KoggerApp waits for the first answer
constexpr int kAckMs = 2000;
constexpr int kResends = 5;
constexpr int kFirmwareMs = 8000;
constexpr int kVersionPollMs = 250;

QByteArray keyBytes()
{
    const uint32_t k = 0xC96B5D4Au;
    QByteArray b(4, 0);
    for (int i = 0; i < 4; ++i)
        b[i] = char((k >> (8 * i)) & 0xFF);
    return b;
}

QByteArray u32le(uint32_t v)
{
    QByteArray b(4, 0);
    for (int i = 0; i < 4; ++i)
        b[i] = char((v >> (8 * i)) & 0xFF);
    return b;
}

} // namespace

RingUpgrade::RingUpgrade(Io io, QObject* parent)
    : QObject(parent), io_(std::move(io))
{
}

QByteArray RingUpgrade::kp1(uint8_t route, uint8_t mode, uint8_t id, const QByteArray& payload)
{
    QByteArray f;
    f.reserve(8 + payload.size());
    f.append(char(0xBB)).append(char(0x55)).append(char(route)).append(char(mode)).append(char(id))
     .append(char(uint8_t(payload.size()))).append(payload);
    uint8_t c1 = 0, c2 = 0;
    for (int i = 2; i < f.size(); ++i) {
        c1 = uint8_t(c1 + uint8_t(f[i]));
        c2 = uint8_t(c2 + c1);
    }
    f.append(char(c1)).append(char(c2));
    return f;
}

QString RingUpgrade::versionFromFileName(const QString& path)
{
    static const QRegularExpression re(QStringLiteral("_v(\\d+)\\.(\\d+)_"));
    const auto m = re.match(QFileInfo(path).fileName());
    return m.hasMatch() ? m.captured(1) + QLatin1Char('.') + m.captured(2) : QString();
}

QList<int> RingUpgrade::allTargets(QList<int> boards)
{
    std::sort(boards.begin(), boards.end());
    std::reverse(boards.begin(), boards.end());
    return boards;
}

bool RingUpgrade::start(const Plan& plan)
{
    auto refuse = [this](const QString& why) {
        log(QStringLiteral("refused: %1").arg(why));
        emit finished(false, why);
        return false;
    };
    if (active())
        return refuse(QStringLiteral("a run is already going"));
    if (!io_.write || !io_.setRate || !io_.nowMs)
        return refuse(QStringLiteral("no line"));
    if (plan.image.isEmpty() || plan.image.size() > 65535 * kChunk)
        return refuse(QStringLiteral("no image, or one over 65535 chunks"));
    if (plan.boards.isEmpty() || plan.boards.size() > 2)
        return refuse(QStringLiteral("one board or a ring of two, not %1").arg(plan.boards.size()));
    for (int b : plan.boards)
        if (b < 1 || b > 15)
            return refuse(QStringLiteral("bus address %1 is outside 1..15").arg(b));
    if (plan.boards.size() == 2 && plan.boards[0] == plan.boards[1])
        return refuse(QStringLiteral("both boards have address %1").arg(plan.boards[0]));
    if (plan.targets.isEmpty())
        return refuse(QStringLiteral("nothing to update"));
    for (int t : plan.targets)
        if (!plan.boards.contains(t))
            return refuse(QStringLiteral("address %1 is not on the line").arg(t));
    if (plan.lineRate <= 0)
        return refuse(QStringLiteral("no line rate"));

    plan_ = plan;
    std::sort(plan_.boards.begin(), plan_.boards.end());
    runOk_ = true;
    runDetail_.clear();
    done_.clear();
    datasets_.clear();
    nextTarget_ = 0;
    target_ = -1;
    booted_ = false;
    keepRate_ = 0;
    rate_ = plan_.lineRate;
    win_ = Window{};
    log(QStringLiteral("start: boards %1, targets %2, %3 bytes, expect %4")
            .arg(boardsText(plan_.boards), boardsText(plan_.targets)).arg(plan_.image.size())
            .arg(plan_.expect.isEmpty() ? QStringLiteral("any version") : plan_.expect));
    if (plan_.boards.size() > 1) {     // a ring: no BREAK into a bootloader, no echograms in the way
        phase_ = Phase::PingsAsk;
        step_ = 0;
    } else {
        phase_ = Phase::Boot;
        step_ = -1;
        at(0);
    }
    return true;
}

QString RingUpgrade::boardsText(const QList<int>& list)
{
    QStringList s;
    for (int b : list)
        s << QString::number(b);
    return s.join(QLatin1Char(','));
}

void RingUpgrade::log(const QString& s)
{
    emit logLine(QStringLiteral("ring-upgrade: ") + s);
}

void RingUpgrade::send(uint8_t route, uint8_t mode, uint8_t id, const QByteArray& pl)
{
    io_.write(kp1(route, mode, id, pl));
}

void RingUpgrade::sendRate(uint8_t route, int baud)
{
    send(route, kSet, kIdUart, keyBytes() + QByteArray(1, char(1)) + u32le(uint32_t(baud)));
}

void RingUpgrade::setRate(int baud)
{
    if (baud == rate_)
        return;
    rate_ = baud;
    io_.setRate(baud);
}

void RingUpgrade::openProbe(int route, int ms)
{
    win_ = Window{};
    win_.open = true;
    win_.until = now() + ms;
    send(uint8_t(route), kGet, kIdVersion);
    send(uint8_t(route), kGetV2, kIdVersion);
    send(uint8_t(route), kGet, kIdChartSetup);
}

QString RingUpgrade::probeResult() const
{
    if (win_.bootloader)
        return QStringLiteral("bootloader");
    if (win_.firmware)
        return QStringLiteral("firmware");
    return win_.version ? QStringLiteral("bootloader") : QStringLiteral("none");
}

void RingUpgrade::onFrame(const QByteArray& f)
{
    if (!active() || f.size() < 8 || uint8_t(f[0]) != 0xBB || uint8_t(f[1]) != 0x55)
        return;
    const uint8_t route = uint8_t(f[2]), mode = uint8_t(f[3]), id = uint8_t(f[4]), n = uint8_t(f[5]);
    if (f.size() < 8 + n)
        return;
    const QByteArray pl = f.mid(6, n);
    const int type = mode & 3, ver = (mode >> 3) & 7;
    const bool ack = mode & 0x80;

    if (win_.open && type == 1 && !ack && (win_.route < 0 || route == win_.route)) {
        if (id == kIdVersion) {
            win_.version = true;
            if (ver == 2 && n >= 9) {
                if (uint8_t(pl[0]) == 1)
                    win_.bootloader = true;
                else if (uint8_t(pl[0]) == 0) {
                    win_.firmware = true;
                    win_.fwVersion = QStringLiteral("%1.%2").arg(uint8_t(pl[8])).arg(uint8_t(pl[7]));
                }
            }
        } else if (id == kIdChartSetup) {
            win_.firmware = true;
        } else if (id == kIdDataset) {
            win_.dataset = pl;
        }
    }

    if (phase_ != Phase::Transfer || !waitingAck_ || id != kIdUpdate || type != 1)
        return;
    const int size = int(plan_.image.size());
    if (ack) {                                   // the bare acknowledgement: [code][echo of our checksum]
        const int code = n >= 1 ? uint8_t(pl[0]) : -1;
        if (code != 1) {
            fail(QStringLiteral("chunk %1 refused, code %2").arg(chunk_).arg(code), true);
            return;
        }
        offset_ = std::min(size, offset_ + kChunk);
        ++chunk_;
    } else {                                     // progress: U2 lastNumMsg, U4 lastOffset, U1 type, U1 rcvNumMsg
        if (n < 7) {
            fail(QStringLiteral("unreadable answer to chunk %1").arg(chunk_), true);
            return;
        }
        const int lastNum = uint8_t(pl[0]) | (uint8_t(pl[1]) << 8);
        const int lastOff = int(uint8_t(pl[2]) | (uint8_t(pl[3]) << 8) | (uint8_t(pl[4]) << 16) | (uint32_t(uint8_t(pl[5])) << 24));
        const int cond = uint8_t(pl[6]);
        if (cond == 0 || cond == 1) {
            offset_ = std::min(size, offset_ + kChunk);
            ++chunk_;
        } else if (cond == 2 && lastOff >= 0 && lastOff <= size) {
            chunk_ = lastNum + 1;
            offset_ = lastOff;
        } else {
            fail(QStringLiteral("the bootloader reported a fault (type %1) at chunk %2").arg(cond).arg(chunk_), true);
            return;
        }
    }
    waitingAck_ = false;
    resends_ = 0;
    const int pct = int(qint64(offset_) * 100 / size);
    if (pct != shownPct_) {
        shownPct_ = pct;
        emit targetProgress(target_, pct);
    }
    if (offset_ >= size) {
        log(QStringLiteral("board %1: %2 chunks in %3 s, running it")
                .arg(target_).arg(chunk_ - 1).arg(double(now() - transferAt_) / 1000.0, 0, 'f', 1));
        send(kLoaderRoute, kSetV1, kIdBoot, keyBytes());
        phase_ = Phase::RunFirmware;
        at(60);
        return;
    }
    sendChunk();
}

void RingUpgrade::sendChunk()
{
    QByteArray pl;
    pl.append(char(chunk_ & 0xFF)).append(char((chunk_ >> 8) & 0xFF));
    pl.append(plan_.image.mid(offset_, kChunk));
    send(kLoaderRoute, kSet, kIdUpdate, pl);
    waitingAck_ = true;
    chunkAt_ = now();
}

void RingUpgrade::beginTarget()
{
    if (nextTarget_ >= plan_.targets.size()) {
        beginRestore(runOk_, done_.join(QStringLiteral("; ")), kSettleMs);
        return;
    }
    target_ = plan_.targets[nextTarget_++];
    if (plan_.boards.size() == 1) {
        role_ = Role::Lone;
        other_ = -1;
    } else if (target_ == plan_.boards.first()) {
        role_ = Role::First;
        other_ = plan_.boards.last();
    } else {
        role_ = Role::Last;
        other_ = plan_.boards.first();
    }
    setRate(plan_.lineRate);
    log(QStringLiteral("board %1 (%2): to its bootloader").arg(target_)
            .arg(role_ == Role::Lone ? QStringLiteral("alone") : role_ == Role::First ? QStringLiteral("first") : QStringLiteral("last")));
    emit targetProgress(target_, 0);
    bootAt_ = now();
    booted_ = true;
    send(uint8_t(target_), kSet, kIdBoot, keyBytes());
    phase_ = Phase::Boot;
    step_ = 0;
    // The board resets ~30 ms after its answer. A command for the other board must not go out before:
    // were the ring wired the other way round, it would still pass the target and reach the wrong board.
    at(role_ == Role::Last ? kBootQuietMs : 30);
}

void RingUpgrade::beginTransfer()
{
    log(QStringLiteral("board %1: bootloader at %2 after %3 ms").arg(target_).arg(plan_.lineRate).arg(now() - bootAt_));
    phase_ = Phase::Transfer;
    chunk_ = 1;
    offset_ = 0;
    resends_ = 0;
    shownPct_ = -1;
    transferAt_ = now();
    sendChunk();
}

void RingUpgrade::targetOk(const QString& detail)
{
    log(QStringLiteral("board %1: %2").arg(target_).arg(detail));
    done_ << QStringLiteral("%1: %2").arg(target_).arg(detail);
    emit targetFinished(target_, true, detail);
    // It came back with its saved settings. Pinging, the first board would send its sync BREAK into
    // the bootloader of the next target behind it: quiet until the run ends (Restore turns it back).
    if (!datasets_.isEmpty())
        send(uint8_t(target_), kSet, kIdDataset, QByteArray(1, char(1)) + u32le(0) + u32le(0));
    beginTarget();
}

void RingUpgrade::fail(const QString& detail, bool chunksSent)
{
    const QString d = chunksSent
        ? detail + QStringLiteral(" -- the board stays in its bootloader")
        : detail + QStringLiteral(" -- nothing was written, the board starts its old firmware by itself");
    log(QStringLiteral("board %1: %2").arg(target_).arg(d));
    waitingAck_ = false;
    win_.open = false;
    emit targetFinished(target_, false, d);
    // Nothing written: the target is still in its bootloader for a moment, and a first board there
    // blocks the way to the other one. Restore once its old firmware is surely back.
    const qint64 wait = (booted_ && !chunksSent) ? std::max<qint64>(kSettleMs, bootAt_ + kBackMs - now()) : kSettleMs;
    beginRestore(false, QStringLiteral("%1: %2").arg(target_).arg(d), wait);
}

void RingUpgrade::beginRestore(bool ok, const QString& detail, qint64 delayMs)
{
    runOk_ = runOk_ && ok;
    runDetail_ = detail;
    setRate(keepRate_ ? keepRate_ : plan_.lineRate);
    phase_ = Phase::Restore;
    step_ = 0;
    at(delayMs);
}

void RingUpgrade::finish()
{
    log(QStringLiteral("%1: %2").arg(runOk_ ? QStringLiteral("done") : QStringLiteral("failed"), runDetail_));
    phase_ = Phase::Idle;
    target_ = -1;
    emit finished(runOk_, runDetail_);
}

void RingUpgrade::tick()
{
    if (!active())
        return;
    const bool closed = win_.open && now() >= win_.until;
    if (closed)
        win_.open = false;

    switch (phase_) {
    case Phase::Idle:
        return;

    case Phase::PingsAsk: {
        const int board = plan_.boards[step_];
        if (!closed && !win_.open) {             // ask this board for its dataset
            win_ = Window{};
            win_.open = true;
            win_.route = board;
            win_.until = now() + 300;
            send(uint8_t(board), kGet, kIdDataset);
            return;
        }
        if (!closed)
            return;
        if (win_.dataset.isEmpty()) {
            target_ = board;
            fail(QStringLiteral("no answer to ID_DATASET"), false);
            return;
        }
        datasets_.append({board, win_.dataset});
        send(uint8_t(board), kSet, kIdDataset, QByteArray(1, char(1)) + u32le(0) + u32le(0));
        if (++step_ >= plan_.boards.size()) {
            phase_ = Phase::Boot;
            step_ = -1;
            at(400);                             // the last chart of each board drains
        }
        return;
    }

    case Phase::Boot:
        if (!due())
            return;
        if (step_ == -1) {
            beginTarget();
            return;
        }
        if (role_ == Role::Last) {               // the first board relays to the bootloader at 115200 for a moment
            sendRate(uint8_t(other_), kLoaderRate);
            phase_ = Phase::LastSlow;
            at(kSettleMs);
        } else {
            phase_ = Phase::FirstFast;
        }
        return;

    case Phase::LastSlow:
        if (!due())
            return;
        setRate(kLoaderRate);
        openProbe(kLoaderRoute, kProbeMs);
        phase_ = Phase::LastPoll;
        return;

    case Phase::LastPoll:
        if (!closed)
            return;
        if (probeResult() == QLatin1String("bootloader")) {
            sendRate(kLoaderRoute, plan_.lineRate);
            phase_ = Phase::LastUp;
            step_ = 0;
            at(kSettleMs);
        } else if (now() - bootAt_ > kContactLastMs) {
            sendRate(uint8_t(other_), plan_.lineRate);   // give the first board its rate back first
            phase_ = Phase::LastUp;
            step_ = 99;
            at(kSettleMs);
        } else {
            openProbe(kLoaderRoute, kProbeMs);
        }
        return;

    case Phase::LastUp:
        if (!due())
            return;
        if (step_ == 0) {
            sendRate(uint8_t(other_), plan_.lineRate);
            step_ = 1;
            at(kSettleMs);
        } else if (step_ == 1) {
            setRate(plan_.lineRate);
            tries_ = 0;
            openProbe(kLoaderRoute, 100);
            phase_ = Phase::LastVerify;
        } else {
            setRate(plan_.lineRate);
            fail(QStringLiteral("its bootloader did not answer at %1").arg(kLoaderRate), false);
        }
        return;

    case Phase::LastVerify:
        if (!closed)
            return;
        if (probeResult() == QLatin1String("bootloader"))
            beginTransfer();
        else if (++tries_ < 3)
            openProbe(kLoaderRoute, 100);
        else
            fail(QStringLiteral("its bootloader did not answer at %1").arg(plan_.lineRate), false);
        return;

    case Phase::FirstFast:
        if (win_.open)
            return;
        if (closed) {
            if (probeResult() == QLatin1String("bootloader")) {
                beginTransfer();
                return;
            }
            setRate(kLoaderRate);
            phase_ = Phase::FirstSlow;
            if (role_ == Role::Lone) {           // alone: the host hears the bootloader at 115200 too
                step_ = 0;
                openProbe(kLoaderRoute, kProbeMs);
            } else {                             // first: its answer at 115200 cannot pass the last board
                sendRate(kLoaderRoute, plan_.lineRate);
                step_ = 2;
                at(15);
            }
            return;
        }
        if (now() - bootAt_ > kContactFirstMs) {
            setRate(plan_.lineRate);
            fail(QStringLiteral("its bootloader did not answer"), false);
            return;
        }
        setRate(plan_.lineRate);
        openProbe(kLoaderRoute, kProbeMs);
        return;

    case Phase::FirstSlow:
        if (step_ == 0) {
            if (!closed)
                return;
            if (probeResult() == QLatin1String("bootloader")) {
                sendRate(kLoaderRoute, plan_.lineRate);
                step_ = 2;
                at(15);
            } else {
                phase_ = Phase::FirstFast;
            }
            return;
        }
        if (due())
            phase_ = Phase::FirstFast;
        return;

    case Phase::Transfer:
        if (waitingAck_ && now() - chunkAt_ > (chunk_ == 1 ? kFirstAckMs : kAckMs)) {
            if (++resends_ > kResends) {
                fail(QStringLiteral("no answer to chunk %1 after %2 resends").arg(chunk_).arg(kResends), chunk_ > 1);
                return;
            }
            log(QStringLiteral("board %1: chunk %2 again (%3)").arg(target_).arg(chunk_).arg(resends_));
            sendChunk();
        }
        return;

    case Phase::RunFirmware:
        if (!due())
            return;
        phase_ = Phase::WaitFirmware;
        step_ = 0;
        due_ = now() + kFirmwareMs;              // overall deadline for the firmware to answer
        return;

    case Phase::WaitFirmware: {
        if (closed) {
            if (!win_.fwVersion.isEmpty()) {
                const QString got = win_.fwVersion;
                if (!plan_.expect.isEmpty() && got != plan_.expect)
                    fail(QStringLiteral("runs, but reports version %1 instead of %2").arg(got, plan_.expect), true);
                else {
                    if (role_ == Role::Lone && rate_ != plan_.lineRate)
                        keepRate_ = rate_;
                    targetOk(QStringLiteral("version %1, at %2").arg(got).arg(rate_));
                }
                return;
            }
            ++step_;
        }
        if (win_.open)
            return;
        if (due()) {
            fail(QStringLiteral("its firmware did not answer within %1 s").arg(kFirmwareMs / 1000), true);
            return;
        }
        // a lone board may come back at another saved rate; a ring always runs at the line rate
        setRate(role_ == Role::Lone && (step_ % 2) ? kLoaderRate : plan_.lineRate);
        win_ = Window{};
        win_.open = true;
        win_.route = target_;
        win_.until = now() + kVersionPollMs;
        send(uint8_t(target_), kGet, kIdVersion);
        return;
    }

    case Phase::Restore:
        if (!due())
            return;
        if (step_ < datasets_.size()) {
            send(uint8_t(datasets_[step_].first), kSet, kIdDataset, datasets_[step_].second);
            ++step_;
            at(50);
            return;
        }
        finish();
        return;
    }
}
