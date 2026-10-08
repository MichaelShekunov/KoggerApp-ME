#include "node_probe.h"

using namespace Parsers;

NodeProbe::NodeProbe(Io io, QObject* parent) : QObject(parent), io_(std::move(io))
{
}

bool NodeProbe::start(uint8_t route, uint8_t address)
{
    if (active())
        return false;
    route_ = route;
    address_ = address;
    round_ = 0;
    phase_ = Phase::Settle;
    due_ = io_.nowMs() + kSettleMs;
    emit logLine(QStringLiteral("node-probe: route %1, the wrapper names %2").arg(route).arg(address));
    return true;
}

void NodeProbe::onFrame(uint8_t id, uint8_t ver, uint8_t type, int boardAddress)
{
    if (phase_ != Phase::Wrapped && phase_ != Phase::Plain)
        return;
    if (id != ID_UART || ver != v1 || type != CONTENT)
        return;
    const bool wrapped = phase_ == Phase::Wrapped;
    finish(wrapped ? Mode::Wrapped : Mode::Plain, boardAddress,
           QStringLiteral("the board (address %1) answers %2").arg(boardAddress)
               .arg(wrapped ? QStringLiteral("wrapped") : QStringLiteral("plain")));
}

void NodeProbe::tick()
{
    if (!active() || io_.nowMs() < due_)
        return;
    const auto drain = [this](Phase p) { phase_ = p; due_ = io_.nowMs() + kDrainMs; };
    switch (phase_) {
    case Phase::Settle:
        return ask(true);
    case Phase::Wrapped:
        return drain(Phase::DrainWrapped);
    case Phase::DrainWrapped:
        return ask(false);
    case Phase::Plain:
        return drain(Phase::DrainPlain);
    case Phase::DrainPlain:
        if (++round_ < kRounds)
            return ask(true);
        return finish(Mode::Unknown, -1, QStringLiteral("no answer, wrapped or plain, in %1 rounds").arg(kRounds));
    case Phase::Idle:
        return;
    }
}

void NodeProbe::ask(bool wrapped)
{
    ProtoBinOut get;
    get.create(GETTING, v1, ID_UART, route_);
    get.end();
    io_.send(wrapped ? wrapNodeCmd(get, address_) : get);
    phase_ = wrapped ? Phase::Wrapped : Phase::Plain;
    due_ = io_.nowMs() + kAnswerMs;
}

void NodeProbe::finish(Mode mode, int boardAddress, const QString& detail)
{
    phase_ = Phase::Idle;
    emit logLine(QStringLiteral("node-probe: ") + detail);
    emit finished(mode, boardAddress, detail);
}

bool canWrapNodeCmd(ProtoBinOut& inner)
{
    const uint8_t* f = inner.frame();
    return inner.frameLen() >= 8 && (f[3] & 0x3) >= 2 && f[5] <= 61;
}

ProtoBinOut wrapNodeCmd(ProtoBinOut& inner, uint8_t address)
{
    const uint8_t* f = inner.frame();
    ProtoBinOut node;
    node.create(Type(f[3] & 0x3), v0, ID_NODE_CMD, 0);
    node.write<U1>(address);
    node.write<U1>(f[3]);
    node.write<U1>(f[4]);
    for (int i = 0; i < f[5]; ++i)
        node.write<U1>(f[6 + i]);
    node.end();
    return node;
}
