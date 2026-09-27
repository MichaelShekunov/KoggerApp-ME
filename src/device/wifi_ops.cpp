#include "wifi_ops.h"

#include <QStringList>
#include <cmath>

// ---- WifiPortChange -------------------------------------------------------------------------------

WifiPortChange::WifiPortChange(Hooks hooks, Timing timing, QObject* parent)
    : QObject(parent),
      hooks_(std::move(hooks)),
      timing_(timing)
{
    switchTimer_.setSingleShot(true);
    connect(&switchTimer_, &QTimer::timeout, this, &WifiPortChange::onSwitch);
    checkTimer_.setInterval(timing_.checkMs);
    connect(&checkTimer_, &QTimer::timeout, this, &WifiPortChange::onCheck);
}

bool WifiPortChange::start(int newBaud, int oldBaud)
{
    if (phase_ != Idle || newBaud < 9600 || newBaud > 4000000 || oldBaud <= 0 || newBaud == oldBaud)
        return false;
    newBaud_ = newBaud;
    oldBaud_ = oldBaud;
    tries_ = 0;
    answered_ = false;
    phase_ = Switching;
    emit activeChanged();
    hooks_.setModuleBaud(newBaud_);
    switchTimer_.start(timing_.switchMs);
    return true;
}

void WifiPortChange::moduleAnswered()
{
    if (phase_ == Checking)
        answered_ = true;
}

void WifiPortChange::onSwitch()
{
    hooks_.setLinkBaud(newBaud_);
    phase_ = Checking;
    tries_ = 0;
    answered_ = false;
    checkTimer_.start();
}

void WifiPortChange::onCheck()
{
    // Each check forgets what came before and asks again, so a frame decoded from the old speed never counts.
    if (answered_ && tries_ > 0) {
        finish(true);
        return;
    }
    if (++tries_ > timing_.checks) {
        finish(false);
        return;
    }
    answered_ = false;
    hooks_.askStatus();
}

void WifiPortChange::finish(bool ok)
{
    checkTimer_.stop();
    if (ok)
        hooks_.saveInModule();
    else
        hooks_.setLinkBaud(oldBaud_);
    phase_ = Idle;
    emit activeChanged();
    emit finished(ok, newBaud_, oldBaud_);
}

// ---- WifiRestartWatch -----------------------------------------------------------------------------

bool WifiRestartWatch::onUptime(uint32_t uptimeS, qint64 nowMs)
{
    const bool restarted = seen_ && (uptimeS < lastS_ || qint64(uptimeS) + kStallS < expectedS(nowMs));
    seen_ = true;
    lastS_ = uptimeS;
    lastAtMs_ = nowMs;
    return restarted;
}

// ---- WifiRadioCheck -------------------------------------------------------------------------------

WifiRadioCheck::WifiRadioCheck(Hooks hooks, Timing timing, QObject* parent)
    : QObject(parent),
      hooks_(std::move(hooks)),
      timing_(timing)
{
    askTimer_.setInterval(timing_.askMs);
    connect(&askTimer_, &QTimer::timeout, this, &WifiRadioCheck::onAsk);
}

void WifiRadioCheck::start(double powerDbm, int protocols, int widthMhz, int powerSave)
{
    askPower_ = powerDbm;
    askProto_ = protocols;
    askWidth_ = widthMhz == 40 ? 40 : 20;
    askPs_ = powerSave;
    since_.start();
    askTimer_.start();
    if (!active_) {
        active_ = true;
        emit activeChanged();
    }
}

void WifiRadioCheck::stop()
{
    askTimer_.stop();
    active_ = false;
    emit activeChanged();
}

void WifiRadioCheck::onRadio(double powerNowDbm, int phyNow)
{
    if (!active_ || phyNow == 0xFF || since_.elapsed() < timing_.settleMs)
        return;
    stop();
    const int widthNow = widthOfPhy(phyNow);
    QStringList diffs;
    if (std::fabs(powerNowDbm - askPower_) >= 0.25)
        diffs << tr("%1 dBm instead of %2 (the driver's step)").arg(powerNowDbm).arg(askPower_);
    if (widthNow != askWidth_)
        diffs << tr("%1 MHz instead of %2 (the access point gives no 40 MHz)").arg(widthNow).arg(askWidth_);
    if (diffs.isEmpty()) {
        emit finished(AsAsked, tr("in effect as asked: %1 dBm, %2 MHz").arg(askPower_).arg(askWidth_));
        return;
    }
    hooks_.saveRadio(powerNowDbm, askProto_, widthNow, askPs_);
    emit finished(Corrected, tr("the module uses %1: saved like that").arg(diffs.join(QStringLiteral(", "))));
}

void WifiRadioCheck::onAsk()
{
    if (since_.elapsed() > timing_.deadlineMs) {
        stop();
        emit finished(NoLink, tr("no link to the access point after the radio change: the result could not be checked"));
        return;
    }
    hooks_.askRadio();
}
