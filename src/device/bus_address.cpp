#include "bus_address.h"

#include "ring_upgrade.h"

#include <QStringList>

namespace {

constexpr uint8_t kIdUart = 0x18, kIdFlash = 0x23;
constexpr uint8_t kSetV0 = 0x02, kSetV1 = 0x0A, kGetV1 = 0x0B;  // mode: type 2/3, version in bits 3..5
constexpr uint32_t kKey = 0xC96B5D4Au;
// The pauses of chain_addr.py, which numbered the bench ring (firmware tree, docs/CHAIN.md).
constexpr int kAnswerMs = 400, kSetMs = 200, kSaveMs = 500, kRestoreMs = 300;

QByteArray key()
{
    QByteArray k(4, 0);
    for (int i = 0; i < 4; ++i)
        k[i] = char((kKey >> (8 * i)) & 0xFF);
    return k;
}

QByteArray addressPayload(int address)
{
    QByteArray p = key();
    p.append(char(1));          // UART 1, the host's
    p.append(char(address));
    return p;
}

QString listText(const QList<int>& list)
{
    QStringList s;
    for (int a : list)
        s << QString::number(a);
    return s.isEmpty() ? QStringLiteral("nothing") : s.join(QStringLiteral(", "));
}

} // namespace

BusAddress::BusAddress(Io io, QObject* parent) : QObject(parent), io_(std::move(io)) {}

bool BusAddress::start(int address)
{
    if (active()) {
        emit finished(false, -1, QStringLiteral("an address change is running already"));
        return false;
    }
    if (address < 0 || address > 15) {
        emit finished(false, -1, QStringLiteral("address %1 is out of 0..15: route 16 carries the bootloader's frames").arg(address));
        return false;
    }
    target_ = address;
    temp_ = address ? 0 : 15;
    log(QStringLiteral("setting bus address %1").arg(target_));
    phase_ = Phase::Count;
    ask(0);
    due_ = io_.nowMs() + kAnswerMs;
    return true;
}

void BusAddress::onFrame(const QByteArray& f)
{
    if (!active() || f.size() < 8 || uint8_t(f[0]) != 0xBB || uint8_t(f[1]) != 0x55)
        return;
    const uint8_t mode = uint8_t(f[3]), id = uint8_t(f[4]), n = uint8_t(f[5]);
    // CONTENT v1 [key][uart 1][address]. A board relays the host's own commands too; those are not answers.
    if (id != kIdUart || (mode & 3) != 1 || ((mode >> 3) & 7) != 1 || n < 6 || f.size() < 8 + n)
        return;
    answers_.append(uint8_t(f[6 + 5]));
}

void BusAddress::tick()
{
    if (!active() || io_.nowMs() < due_)
        return;
    const auto next = [this](Phase p, int ms) { phase_ = p; due_ = io_.nowMs() + ms; };
    switch (phase_) {
    case Phase::Count:
        if (answers_.isEmpty())
            return finish(false, -1, QStringLiteral("no board answered: is it a sonar with firmware 1.12 or newer, alone on the line?"));
        if (answers_.size() > 1)
            return finish(false, answers_.first(), QStringLiteral("%1 answers to one request (%2): more than one board on the line. "
                                                                  "Leave one board and repeat").arg(answers_.size()).arg(listText(answers_)));
        log(QStringLiteral("one board on the line, at address %1").arg(answers_.first()));
        send(0, kSetV1, kIdUart, addressPayload(target_));
        return next(Phase::Set, kSetMs);
    case Phase::Set:
        ask(uint8_t(target_));
        return next(Phase::ReadBack, kAnswerMs);
    case Phase::ReadBack:
        if (answers_.isEmpty() || answers_.count(target_) != answers_.size())
            return finish(false, answers_.isEmpty() ? -1 : answers_.last(),
                          QStringLiteral("the board did not take address %1: on route %1 it reports %2").arg(target_).arg(listText(answers_)));
        send(uint8_t(target_), kSetV0, kIdFlash, key());                         // save the settings
        return next(Phase::Save, kSaveMs);
    case Phase::Save:
        send(uint8_t(target_), kSetV1, kIdUart, addressPayload(temp_));         // proof: move the RAM address away
        return next(Phase::MoveAway, kSetMs);
    case Phase::MoveAway:
        send(uint8_t(temp_), kSetV1, kIdFlash, key());                           // ... restore the settings from flash
        return next(Phase::Restore, kRestoreMs);
    case Phase::Restore:
        ask(uint8_t(target_));                                                   // ... and the address must be back
        return next(Phase::Prove, kAnswerMs);
    case Phase::Prove:
        if (!answers_.isEmpty() && answers_.count(target_) == answers_.size())
            return finish(true, target_, QStringLiteral("address %1 is set and saved: it came back from flash").arg(target_));
        return finish(false, answers_.isEmpty() ? -1 : answers_.last(),
                      QStringLiteral("address %1 was not saved: after a restore from flash the board reports %2").arg(target_).arg(listText(answers_)));
    case Phase::Idle:
        return;
    }
}

void BusAddress::send(uint8_t route, uint8_t mode, uint8_t id, const QByteArray& payload)
{
    io_.write(RingUpgrade::kp1(route, mode, id, payload));
}

void BusAddress::ask(uint8_t route)
{
    answers_.clear();
    send(route, kGetV1, kIdUart, QByteArray());
}

void BusAddress::finish(bool ok, int address, const QString& detail)
{
    phase_ = Phase::Idle;
    log(QStringLiteral("%1: %2").arg(ok ? QStringLiteral("done") : QStringLiteral("failed"), detail));
    emit finished(ok, address, detail);
}

void BusAddress::log(const QString& s)
{
    emit logLine(QStringLiteral("bus-address: ") + s);
}
