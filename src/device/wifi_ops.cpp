#include "wifi_ops.h"

#include <QCoreApplication>
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
    if (phase_ != Idle || newBaud < WifiRates::kMin || newBaud > WifiRates::kMax || oldBaud <= 0 || newBaud == oldBaud)
        return false;
    newBaud_ = newBaud;
    oldBaud_ = oldBaud;
    tries_ = 0;
    answered_ = false;
    refused_ = false;
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

void WifiPortChange::moduleRefused()
{
    if (phase_ != Switching)
        return;
    switchTimer_.stop();
    refused_ = true;
    finish(false);
}

void WifiPortChange::finish(bool ok)
{
    checkTimer_.stop();
    if (ok)
        hooks_.saveInModule();
    else if (!refused_)   // refused before the switch: the link never moved
        hooks_.setLinkBaud(oldBaud_);
    phase_ = Idle;
    emit activeChanged();
    emit finished(ok, newBaud_, oldBaud_);
}

// ---- WifiLineEdit / WifiLineChange ---------------------------------------------------------------

WifiLineRecord WifiLineEdit::appliedTo(WifiLineRecord r) const
{
    if (mode) r.mode = *mode;
    if (dest) r.dest = *dest;
    if (ip) r.ip = *ip;
    if (rport) r.rport = *rport;
    if (lport) r.lport = *lport;
    if (baud) r.baud = *baud;
    if (txPin) r.txPin = *txPin;
    if (rxPin) r.rxPin = *rxPin;
    return r;
}

WifiLineChange::WifiLineChange(Hooks hooks, Timing timing, QObject* parent)
    : QObject(parent),
      hooks_(std::move(hooks)),
      timing_(timing)
{
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &WifiLineChange::onTimeout);
}

bool WifiLineChange::start(int line, const WifiLineEdit& edit)
{
    if (phase_ != Idle || line < 0 || line > 1 || edit.empty() || (edit.baud && (*edit.baud < WifiRates::kMin || *edit.baud > WifiRates::kMax)))
        return false;
    line_ = line;
    edit_ = edit;
    phase_ = Reading;
    emit activeChanged();
    hooks_.askRecord(line_);
    timer_.start(timing_.answerMs);
    return true;
}

void WifiLineChange::onRecord(int line)
{
    if (phase_ != Reading || line != line_)
        return;
    timer_.stop();
    if (!hooks_.sendRecord(line_, edit_)) {
        finish(NotSent);
        return;
    }
    phase_ = Sent;
    timer_.start(timing_.answerMs);
}

void WifiLineChange::onAnswer(int resp)
{
    if (phase_ != Sent)
        return;
    timer_.stop();
    hooks_.readBack(line_);
    finish(resp == 1 ? Done : Refused, resp == 1 ? 0 : resp);
}

void WifiLineChange::onTimeout()
{
    if (phase_ == Reading) {
        finish(NoRecord);
    } else if (phase_ == Sent) {
        hooks_.readBack(line_);   // the change may have happened all the same: show what the module says
        finish(NoAnswer);
    }
}

void WifiLineChange::finish(int outcome, int resp)
{
    timer_.stop();
    phase_ = Idle;
    emit activeChanged();
    emit finished(outcome, line_, resp);
}

// ---- WifiByteRate ---------------------------------------------------------------------------------

double WifiByteRate::onSample(uint32_t bytes, qint64 nowMs)
{
    if (!has_ || bytes < base_ || nowMs - baseAt_ > kMaxGapMs || nowMs < baseAt_) {
        has_ = true;
        base_ = bytes;
        baseAt_ = nowMs;
        rate_ = -1;
        return rate_;
    }
    const qint64 span = nowMs - baseAt_;
    if (span < kMinSpanMs)
        return rate_;
    rate_ = double(bytes - base_) * 1000.0 / double(span);
    base_ = bytes;
    baseAt_ = nowMs;
    return rate_;
}

// ---- WifiPorts ------------------------------------------------------------------------------------

namespace {
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
} // namespace

int WifiPorts::apply(const uint8_t* data, int size, qint64 nowMs)
{
    if (!data || size < 2 || data[0] >= kPorts)
        return -1;
    const int port = data[0];
    const int page = data[1];
    if (page == 0) {
        if (size < kSummaryBytes)
            return -1;
        Summary s;
        s.valid = true;
        s.flags = data[2];
        s.baud = le32(data + 3);
        s.baudSaved = le32(data + 7);
        s.connected = data[11];
        s.rxBytes = le32(data + 12);
        s.txBytes = le32(data + 16);
        s.rxAge = le16(data + 20);
        s.txAge = le16(data + 22);
        for (int k = 0; k < kProtocols; ++k) {
            const uint8_t* p = data + 24 + 10 * k;
            s.protocols[k].fromPort = le32(p);
            s.protocols[k].toPort = le32(p + 4);
            s.protocols[k].ageFrom = le16(p + 8);
        }
        s.sbpRequestsFrom = le32(data + 84);
        s.sbpContentFrom = le32(data + 88);
        s.sbpRequestsTo = le32(data + 92);
        s.sbpContentTo = le32(data + 96);
        s.moduleRequests = le32(data + 100);
        s.moduleFrames = le32(data + 104);
        s.rxOverflows = le32(data + 108);
        s.txDrops = le32(data + 112);
        s.rxBps = rx_[port].onSample(s.rxBytes, nowMs);
        s.txBps = tx_[port].onSample(s.txBytes, nowMs);
        summary_[port] = s;
    } else if (page == 1) {
        if (size < 3)
            return -1;
        const int n = data[2];
        if (size < 3 + 12 * n)
            return -1;
        DeviceList d;
        d.valid = true;
        for (int i = 0; i < n; ++i) {
            const uint8_t* e = data + 3 + 12 * i;
            Device dev;
            dev.kind = e[0];
            dev.addr = e[1];
            dev.board = e[2];
            dev.major = e[3];
            dev.minor = e[4];
            dev.flags = e[5];
            dev.age = le16(e + 6);
            dev.serial = le32(e + 8);
            d.list.append(dev);
        }
        devices_[port] = d;
    } else if (page == 2) {
        if (size < 5)
            return -1;
        const int n = data[4];
        if (size < 5 + 8 * n)
            return -1;
        Network net;
        net.valid = true;
        net.mode = data[2];
        net.state = data[3];
        for (int i = 0; i < n; ++i) {
            const uint8_t* e = data + 5 + 8 * i;
            Peer peer;
            peer.ip = (quint32(e[0]) << 24) | (quint32(e[1]) << 16) | (quint32(e[2]) << 8) | quint32(e[3]);
            peer.port = le16(e + 4);
            peer.age = le16(e + 6);
            net.peers.append(peer);
        }
        network_[port] = net;
    } else {
        return -1;
    }
    lastPort_ = port;
    lastPage_ = page;
    return page;
}

int WifiPorts::askedPort() const
{
    for (int i = 0; i < kPorts; ++i)
        if (summary_[i].valid && (summary_[i].flags & FlagAskedHere))
            return i;
    return -1;
}

void WifiPorts::reset()
{
    for (int i = 0; i < kPorts; ++i) {
        summary_[i] = Summary();
        devices_[i] = DeviceList();
        network_[i] = Network();
        rx_[i].reset();
        tx_[i].reset();
    }
    lastPort_ = -1;
    lastPage_ = -1;
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

QString WifiRestartWatch::reasonText(int reason)
{
    // esp_reset_reason_t, as the module's contract lists them (ESP32_WiFi docs/SBP_WIFI.md, ID_WIFI v1)
    switch (reason) {
    case 0: return QCoreApplication::translate("WifiRestartWatch", "unknown (0)");
    case 1: return QCoreApplication::translate("WifiRestartWatch", "power-on (1)");
    case 3: return QCoreApplication::translate("WifiRestartWatch", "software: update, role change or reboot (3)");
    case 4: return QCoreApplication::translate("WifiRestartWatch", "panic (4)");
    case 5: return QCoreApplication::translate("WifiRestartWatch", "interrupt watchdog (5)");
    case 6: return QCoreApplication::translate("WifiRestartWatch", "task watchdog (6)");
    case 7: return QCoreApplication::translate("WifiRestartWatch", "other watchdog (7)");
    case 9: return QCoreApplication::translate("WifiRestartWatch", "brownout (9)");
    case 14: return QCoreApplication::translate("WifiRestartWatch", "power glitch (14)");
    case 15: return QCoreApplication::translate("WifiRestartWatch", "CPU lockup (15)");
    }
    return QCoreApplication::translate("WifiRestartWatch", "code %1").arg(reason);
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

void WifiRadioCheck::start(double powerDbm, int protocols, int widthMhz, int powerSave, bool accessPoint)
{
    ap_ = accessPoint;
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

void WifiRadioCheck::cancel()
{
    if (active_)
        stop();
}

void WifiRadioCheck::onRadio(double powerNowDbm, int phyNow, int widthSetMhz)
{
    // A station waits for its link; an access point has no negotiated PHY and uses what the driver was given.
    if (!active_ || since_.elapsed() < timing_.settleMs || (!ap_ && phyNow == 0xFF))
        return;
    stop();
    const int widthNow = ap_ ? (widthSetMhz == 40 ? 40 : 20) : widthOfPhy(phyNow);
    QStringList diffs;
    if (std::fabs(powerNowDbm - askPower_) >= 0.25)
        diffs << tr("%1 dBm instead of %2 (the driver's step)").arg(powerNowDbm).arg(askPower_);
    if (widthNow != askWidth_)
        diffs << (ap_ ? tr("%1 MHz instead of %2").arg(widthNow).arg(askWidth_)
                      : tr("%1 MHz instead of %2 (the access point gives no 40 MHz)").arg(widthNow).arg(askWidth_));
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
        emit finished(NoLink, ap_ ? tr("no radio report from the module after the change: the result could not be checked")
                                  : tr("no link to the access point after the radio change: the result could not be checked"));
        return;
    }
    hooks_.askRadio();
}
