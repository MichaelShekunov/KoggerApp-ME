#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>

/**
 * @brief Changing the speed of the Wi-Fi module's port this host is connected through (line 0 before
 * firmware 0.12; X1 or X2 since, as the module reports) as one sequence that does not depend on a settings
 * page being open (the device settings page is unloaded as soon as it is left).
 *
 * The module is told the new speed (it answers at the old one and then switches), this host's link
 * follows after a short pause, the module has to answer a status request at the new speed, and only then
 * is the speed saved in the module. Without an answer the link goes back to the old speed; a speed the
 * module was given but did not save is gone at its next reboot (firmware 0.12 also returns to the old speed
 * by itself after 10 s without a request through that port, and takes the status request as confirmation).
 *
 * The transport is injected through Hooks, so tools/mission_check drives it with made-up events.
 */
class WifiPortChange : public QObject
{
    Q_OBJECT

public:
    struct Hooks {
        std::function<void(int)> setModuleBaud;   ///< ID_UART SETTING to the module
        std::function<void(int)> setLinkBaud;     ///< this host's link to the module
        std::function<void()> askStatus;          ///< a request the module answers (ID_WIFI v0)
        std::function<void()> saveInModule;       ///< ID_FLASH v0
    };
    struct Timing {
        int switchMs = 400;    ///< from the module's switch to the link's
        int checkMs = 700;     ///< between two status requests at the new speed
        int checks = 8;        ///< requests before giving up
    };

    WifiPortChange(Hooks hooks, Timing timing, QObject* parent = nullptr);

    /** False while a change runs, for a speed outside WifiRates::kMin..kMax, or when nothing would change. */
    bool start(int newBaud, int oldBaud);
    /** As start(); moduleKeepsIt: the module saves the new speed at once (firmware 0.15+), so without an answer the
     *  link stays at the new speed, where the module is, instead of going back to the old one. */
    bool start(int newBaud, int oldBaud, bool moduleKeepsIt);
    /** The running (or last) change was one where the module keeps the new speed at once. */
    bool moduleKeepsIt() const { return keeps_; }
    /** A frame from the module that proves it hears this host; counts only after the first check. */
    void moduleAnswered();
    /** The module refused the new speed (its confirmation of ID_UART is an error: above its firmware's limit). The
     *  confirmation comes at the old speed before the module would switch, so the link has not moved: the change
     *  ends at once and nothing else is sent. */
    void moduleRefused();
    /** The last change ended because the module refused the speed. */
    bool refused() const { return refused_; }
    bool active() const { return phase_ != Idle; }
    int target() const { return newBaud_; }

signals:
    void finished(bool ok, int newBaud, int oldBaud);
    void activeChanged();

private:
    enum Phase { Idle, Switching, Checking };
    void onSwitch();
    void onCheck();
    void finish(bool ok);

    Hooks hooks_;
    Timing timing_;
    Phase phase_ = Idle;
    int newBaud_ = 0;
    int oldBaud_ = 0;
    int tries_ = 0;
    bool answered_ = false;
    bool refused_ = false;
    bool keeps_ = false;
    QTimer switchTimer_;
    QTimer checkTimer_;
};

/** One UART line's record as ID_WIFI_NET v3 carries it: the fields a SETTING sends (after the key and line). */
struct WifiLineRecord {
    uint8_t mode = 0, dest = 0;                   ///< mode 0 off / 1 UDP / 2 TCP; dest 0 fixed / 1 senders / 2 broadcast
    quint32 ip = 0;                               ///< a.b.c.d, a in the top byte
    uint16_t rport = 0, lport = 0;                ///< peer port; own UDP port (0 = the peer port)
    uint32_t baud = 0;
    int8_t txPin = -1, rxPin = -1;
};

/**
 * @brief The fields of a line record an operator changed. Everything else is taken from the record read from
 * the module just before sending, so a field this host did not touch keeps what the module has, even when
 * another host changed it meanwhile.
 */
struct WifiLineEdit {
    std::optional<uint8_t> mode, dest;
    std::optional<quint32> ip;
    std::optional<uint16_t> rport, lport;
    std::optional<uint32_t> baud;
    std::optional<int8_t> txPin, rxPin;

    bool empty() const { return !mode && !dest && !ip && !rport && !lport && !baud && !txPin && !rxPin; }
    /** Only a rate: the port-rate path, reported as such. */
    bool rateOnly() const { return baud && !mode && !dest && !ip && !rport && !lport && !txPin && !rxPin; }
    /** `r` with the edited fields replaced. */
    WifiLineRecord appliedTo(WifiLineRecord r) const;
};

/**
 * @brief Changing a UART line's record in the module (ID_WIFI_NET v3 SETTING) as one sequence: the record is read
 * first, sent back with only the edited fields changed, and read back after the module's answer. Used for the
 * network side of a line (mode, destination, peer, ports, pins) and for the rate of a port this host is NOT
 * connected through (firmware 0.12 takes the rate for either line, 0.10/0.11 for line 1 only; the module saves it
 * at once, this host's link is not touched). For the line of the port the request comes through, 0.12 ignores the
 * rate field: that rate goes through WifiPortChange (ID_UART). Runs in the driver like WifiPortChange, so it
 * finishes when the settings page is left.
 */
class WifiLineChange : public QObject
{
    Q_OBJECT

public:
    struct Hooks {
        std::function<void(int)> askRecord;                             ///< ID_WIFI_NET v3 GETTING (the line's record)
        std::function<bool(int, const WifiLineEdit&)> sendRecord;       ///< v3 SETTING of the fresh record with the edit; false = not sent
        std::function<void(int)> readBack;                              ///< v3 and v7 GETTING after the answer
    };
    struct Timing {
        int answerMs = 3000;   ///< for the record, then for the answer to the SETTING
    };
    enum Outcome { Done = 0, NoRecord, NotSent, Refused, NoAnswer };

    WifiLineChange(Hooks hooks, Timing timing, QObject* parent = nullptr);

    /** False while a change runs, for a line other than 0/1, an empty edit, or a rate outside WifiRates::kMin..kMax. */
    bool start(int line, const WifiLineEdit& edit);
    /** A v3 record of `line` arrived (read from the module). */
    void onRecord(int line);
    /** The module's answer to the SETTING (Parsers::Resp: 1 = OK). */
    void onAnswer(int resp);
    bool active() const { return phase_ != Idle; }
    /** True between sending the SETTING and its answer: the driver hands that answer here. */
    bool awaitingAnswer() const { return phase_ == Sent; }
    int line() const { return line_; }
    /** The edit of the running (or last) change. */
    const WifiLineEdit& edit() const { return edit_; }
    /** The rate being set; 0 when the change is not a rate. */
    int target() const { return edit_.baud ? int(*edit_.baud) : 0; }

signals:
    /** outcome: Outcome; resp: the module's code for Refused, 0 otherwise. edit() still holds what was asked. */
    void finished(int outcome, int line, int resp);
    void activeChanged();

private:
    enum Phase { Idle, Reading, Sent };
    void onTimeout();
    void finish(int outcome, int resp = 0);

    Hooks hooks_;
    Timing timing_;
    Phase phase_ = Idle;
    int line_ = -1;
    WifiLineEdit edit_;
    QTimer timer_;
};

/**
 * @brief Bytes per second of a growing byte counter sampled now and then (the module's port RX/TX counters).
 *
 * The rate is the counter's growth since the previous sample over the time between them. A sample closer
 * than kMinSpanMs to the previous one keeps the last rate (a second answer to an overlapping request), a
 * counter that went back (the module restarted, or the 32-bit counter wrapped) or a gap longer than
 * kMaxGapMs (polling stopped) starts over: unknown until the next sample.
 */
class WifiByteRate
{
public:
    static constexpr qint64 kMinSpanMs = 1000;
    static constexpr qint64 kMaxGapMs = 10000;

    /** One sample; nowMs from a monotonic clock. @returns the rate in bytes per second, -1 while unknown. */
    double onSample(uint32_t bytes, qint64 nowMs);
    double rate() const { return rate_; }
    void reset() { has_ = false; base_ = 0; baseAt_ = 0; rate_ = -1; }

private:
    bool has_ = false;
    uint32_t base_ = 0;
    qint64 baseAt_ = 0;
    double rate_ = -1;
};

/**
 * @brief Port information of the Wi-Fi module, firmware 0.12 (ID_WIFI_NET v7 CONTENT): for each physical
 * port (0 = X1, 1 = X2) a summary (page 0), the devices heard from it (page 1) and the network side of its
 * line (page 2). Payloads are decoded by minimum length (bytes after the documented ones are ignored); a
 * payload too short for its page, or for an unknown port or page, changes nothing. Little-endian throughout;
 * ages are in 0.1 s with 0xFFFF = never. Contract: the module project's ports contract for 0.12.
 */
class WifiPorts
{
public:
    static constexpr int kPorts = 2;
    static constexpr int kProtocols = 6;            ///< KP1, KP2, UBX, MAVLink 1, MAVLink 2, raw (in this order)
    static constexpr int kSummaryBytes = 116;       ///< page 0, including {port, page}
    static constexpr uint16_t kAgeNever = 0xFFFF;

    enum Flag : uint8_t {
        FlagUartRunning = 1 << 0,
        FlagBridging = 1 << 1,        ///< the line is on
        FlagAskedHere = 1 << 2,       ///< the request that asked for this summary came through this port
        FlagGetsReports = 1 << 3,     ///< the module's unsolicited frames go to this port
        FlagSlip = 1 << 4,            ///< locked to SLIP (IP bridge, X1 only)
        FlagProvisional = 1 << 5,     ///< rate waiting for confirmation
        FlagUsb = 1 << 6              ///< USB transport (X1 in the USB build)
    };
    /** What is connected, judged by the module over the last 10 s. */
    enum Connected : uint8_t { ConnNothing = 0, ConnUnreadable = 1, ConnSbpHost = 2, ConnSbpDevices = 3,
                               ConnSbpHostDevices = 4, ConnMavlink = 5, ConnUblox = 6, ConnSlipHost = 7, ConnOther = 8 };
    enum DeviceKind : uint8_t { KindSbp = 1, KindMavlink = 2 };
    enum DeviceFlag : uint8_t { DevVersionKnown = 1 << 0, DevHeartbeat = 1 << 1 };

    struct Protocol { uint32_t fromPort = 0, toPort = 0; uint16_t ageFrom = kAgeNever; };
    struct Summary {
        bool valid = false;
        uint8_t flags = 0;
        uint32_t baud = 0, baudSaved = 0;           ///< current rate (0 = UART off) and the saved one
        uint8_t connected = ConnNothing;
        uint32_t rxBytes = 0, txBytes = 0;          ///< RX = from the port, TX = to the port
        uint16_t rxAge = kAgeNever, txAge = kAgeNever;
        Protocol protocols[kProtocols];
        uint32_t sbpRequestsFrom = 0, sbpContentFrom = 0, sbpRequestsTo = 0, sbpContentTo = 0;
        uint32_t moduleRequests = 0, moduleFrames = 0, rxOverflows = 0, txDrops = 0;
        double rxBps = -1, txBps = -1;              ///< from successive summaries (WifiByteRate); -1 unknown
    };
    /** Page 1 entry. SBP: addr, board (0 unknown), firmware major.minor, serial. MAVLink: sysid in addr,
     *  compid in board, MAV_TYPE in major, MAV_AUTOPILOT in minor, frames seen in serial. */
    struct Device {
        uint8_t kind = 0, addr = 0, board = 0, major = 0, minor = 0, flags = 0;
        uint16_t age = kAgeNever;
        uint32_t serial = 0;
    };
    struct DeviceList { bool valid = false; QVector<Device> list; };
    struct Peer { quint32 ip = 0; uint16_t port = 0; uint16_t age = kAgeNever; };   ///< ip as a.b.c.d, a in the top byte
    struct Network { bool valid = false; uint8_t mode = 0, state = 0; QVector<Peer> peers; };

    /** One CONTENT v7 payload; nowMs from a monotonic clock (for the rates). @returns its page, -1 if refused. */
    int apply(const uint8_t* data, int size, qint64 nowMs);
    int lastPort() const { return lastPort_; }
    int lastPage() const { return lastPage_; }
    const Summary& summary(int port) const { return summary_[clampPort(port)]; }
    const DeviceList& devices(int port) const { return devices_[clampPort(port)]; }
    const Network& network(int port) const { return network_[clampPort(port)]; }
    /** The port whose latest summary says the request came through it; -1 when none (the network) or unknown. */
    int askedPort() const;
    void reset();

private:
    static int clampPort(int port) { return port < 0 ? 0 : (port >= kPorts ? kPorts - 1 : port); }

    Summary summary_[kPorts];
    DeviceList devices_[kPorts];
    Network network_[kPorts];
    WifiByteRate rx_[kPorts];
    WifiByteRate tx_[kPorts];
    int lastPort_ = -1;
    int lastPage_ = -1;
};

/**
 * @brief The Wi-Fi module's link rate report (ID_WIFI_SURVEY v1, firmware 0.18; the module's docs/SBP_WIFI.md 3b).
 *
 * Per other end of the link (a station's access point, an access point's clients): the PHY rates its data frames came
 * at in a short listening window, the most frames first. The module does not decode LR (kbit/s 0, the code a raw field
 * of the received frame): no number is shown for it. Listening cuts the link's throughput (ESP-IDF), so the app asks
 * rarely, and the two ends apart.
 */
struct WifiLinkRate
{
    static constexpr int kWindowMs = 200;       ///< the module's default window
    static constexpr int kEveryMs = 3000;       ///< not more often: listening cuts the throughput
    static constexpr int kPeerDelayMs = 400;    ///< the other end is asked this much later: the windows apart
    enum Kind { KindOther = 0, Kind11b = 1, Kind11g = 2, Kind11n = 3, KindLr = 4 };
    struct Rate {
        int kind = KindOther, code = 0, flags = 0;   // flags (11n): bit0 short guard interval, bit1 40 MHz
        uint32_t kbps = 0;                           // 0: not known (LR always)
        int frames = 0;
    };
    struct Page {
        int index = 0, total = 0;                    // total 0: no other end (a station not joined, no clients)
        QString mac;
        int windowMs = 0, frames = 0;
        int rssi = -128;                             // the mean of its frames; -128 none
        int mode = 0xFF;                             // negotiated (as phy of ID_WIFI v7); 0xFF not known, an access point's
        QVector<Rate> rates;                         // the most frames first
        int retries = -1;                            // of `frames`, the ones that came again (firmware 0.20); -1 not sent
    };

    /** 1 from firmware 0.18, 0 before, -1 while the version is not known. */
    static int supportFor(int fwMajor, int fwMinor)
    {
        if (fwMajor <= 0 && fwMinor <= 0)
            return -1;
        return (fwMajor > 0 || fwMinor >= 18) ? 1 : 0;
    }

    /** One CONTENT v1 frame's payload: U1 index, U1 total, U1[6] MAC, U2 window, U2 frames, S1 RSSI, U1 mode, U1 n,
     *  U1 0, then n x {U1 kind, U1 code, U1 flags, U4 kbit/s, U2 frames}, little-endian; {0, 0} when no other end.
     *  Firmware 0.19 appends n x U4 rate bounds (not read here), 0.20 after them U2 frames that came with the Retry
     *  bit (16 + 13 n + 2 bytes). */
    static bool parse(const QByteArray& b, Page& out)
    {
        out = Page();
        if (b.size() < 2)
            return false;
        const auto u8 = [&b](int i) { return int(uint8_t(b[i])); };
        out.index = u8(0);
        out.total = u8(1);
        if (out.total == 0)
            return true;
        if (b.size() < 16 || out.index >= out.total)
            return false;
        QStringList mac;
        for (int i = 0; i < 6; ++i)
            mac << QStringLiteral("%1").arg(u8(2 + i), 2, 16, QLatin1Char('0'));
        out.mac = mac.join(QLatin1Char(':'));
        out.windowMs = u8(8) | (u8(9) << 8);
        out.frames = u8(10) | (u8(11) << 8);
        out.rssi = int(int8_t(b[12]));
        out.mode = u8(13);
        const int n = u8(14);
        if (n > 4 || b.size() < 16 + 9 * n)
            return false;
        for (int k = 0; k < n; ++k) {
            const int o = 16 + 9 * k;
            Rate r;
            r.kind = u8(o);
            r.code = u8(o + 1);
            r.flags = u8(o + 2);
            r.kbps = uint32_t(u8(o + 3)) | (uint32_t(u8(o + 4)) << 8) | (uint32_t(u8(o + 5)) << 16)
                   | (uint32_t(u8(o + 6)) << 24);
            r.frames = u8(o + 7) | (u8(o + 8) << 8);
            out.rates.append(r);
        }
        if (b.size() >= 16 + 13 * n + 2)
            out.retries = u8(16 + 13 * n) | (u8(17 + 13 * n) << 8);
        return true;
    }

    /** "72.2 Mbit/s · 11n MCS7 SGI", "54.0 Mbit/s · 11g", "LR, speed not calibrated". */
    static QString text(const Rate& r)
    {
        const QString mbit = r.kbps ? QString::number(r.kbps / 1000.0, 'f', 1) + QStringLiteral(" Mbit/s")
                                    : QStringLiteral("? Mbit/s");
        switch (r.kind) {
        case KindLr:
            return QStringLiteral("LR, speed not calibrated");
        case Kind11n: {
            QString t = mbit + QStringLiteral(" · 11n MCS%1").arg(r.code);
            if (r.flags & 0x01)
                t += QStringLiteral(" SGI");
            if (r.flags & 0x02)
                t += QStringLiteral(" 40 MHz");
            return t;
        }
        case Kind11b:
            return mbit + QStringLiteral(" · 11b");
        case Kind11g:
            return mbit + QStringLiteral(" · 11g");
        }
        return QStringLiteral("other (code %1)").arg(r.code);
    }
};

/**
 * @brief The other end's frames that came again (the Retry bit: its earlier try or the acknowledgement was lost), summed
 * over the last link rate reports per other end (firmware 0.20). A 200 ms window holds tens of frames, so one report's
 * share is noise: up to kKeep reports not older than kMaxAgeMs are added up (the module session's advice, 2026-10-23).
 * A lower bound: a frame none of whose tries arrived is not seen.
 */
class WifiRetryWindow
{
public:
    static constexpr int kKeep = 10;
    static constexpr qint64 kMaxAgeMs = 30000;
    struct Sum { int retries = 0, frames = 0, reports = 0; };

    /** One report of one other end; a report without frames, or with more retries than frames, adds nothing. */
    void add(const QString& mac, int retries, int frames, qint64 nowMs)
    {
        if (frames <= 0 || retries < 0 || retries > frames)
            return;
        QVector<Entry>& e = byMac_[mac];
        e.append({ nowMs, retries, frames });
        while (e.size() > kKeep || (!e.isEmpty() && nowMs - e.first().atMs > kMaxAgeMs))
            e.removeFirst();
    }
    Sum sum(const QString& mac, qint64 nowMs) const
    {
        Sum s;
        const auto it = byMac_.constFind(mac);
        if (it == byMac_.constEnd())
            return s;
        for (const Entry& e : it.value()) {
            if (nowMs - e.atMs > kMaxAgeMs)
                continue;
            s.retries += e.retries;
            s.frames += e.frames;
            ++s.reports;
        }
        return s;
    }
    void clear() { byMac_.clear(); }

private:
    struct Entry { qint64 atMs; int retries, frames; };
    QHash<QString, QVector<Entry>> byMac_;
};

/**
 * @brief The Wi-Fi link's recent events, for the module's page.
 *
 * A station's state changes with why its link ended (the module's class and the ESP-IDF code), an access point's
 * clients coming and going, the module's restarts. Only what the module's frames say: nothing is guessed between
 * two of them. Newest last, at most kKeep, times on the app's monotonic clock (the head unit's wall clock is not
 * set).
 */
class WifiLinkLog
{
public:
    static constexpr int kKeep = 12;
    enum Kind { KindState = 0, KindClients = 1, KindRestart = 2 };
    struct Event {
        int kind = KindState;
        qint64 atMs = 0;
        int state = -1, prevState = -1;       // 0 idle, 1 searching, 2 connecting, 3 connected, 4 access point; -1 none
        int reason = 0, code = 0;             // why the last link ended: the module's class, the ESP-IDF code
        int clients = -1, prevClients = -1;   // an access point's; -1 not one
        QString ssid;
        int channel = 0, rssi = 0;            // an access point's rssi: its weakest client
        QString text;                         // a restart: why, when the module said
    };

    /** A status frame: an event when the state changed or, an access point staying one, when its clients did. */
    bool onStatus(int state, int reason, int code, int clients, const QString& ssid, int channel, int rssi, qint64 nowMs)
    {
        const bool ap = state == 4;
        const bool stateChanged = state != lastState_;
        if (!stateChanged && !(ap && clients != lastClients_))
            return false;
        Event e;
        e.kind = stateChanged ? KindState : KindClients;
        e.atMs = nowMs;
        e.state = state;
        e.prevState = lastState_;
        e.reason = reason;
        e.code = code;
        e.clients = ap ? clients : -1;
        e.prevClients = lastState_ == 4 ? lastClients_ : -1;
        e.ssid = ssid;
        e.channel = channel;
        e.rssi = rssi;
        push(e);
        lastState_ = state;
        lastClients_ = ap ? clients : -1;
        return true;
    }

    /** The module restarted (WifiRestartWatch); why, when it said. */
    void onRestart(const QString& why, qint64 nowMs)
    {
        Event e;
        e.kind = KindRestart;
        e.atMs = nowMs;
        e.text = why;
        push(e);
    }

    const QVector<Event>& events() const { return events_; }

private:
    void push(const Event& e)
    {
        events_.append(e);
        while (events_.size() > kKeep)
            events_.removeFirst();
    }

    QVector<Event> events_;
    int lastState_ = -1;
    int lastClients_ = -1;
};

/**
 * @brief Tells a restart of the Wi-Fi module from its uptime reports.
 *
 * While the module runs its uptime grows with the time between two reports. A report behind the previous
 * one (any drop), or behind what the previous one plus the time since predicts by more than a stall of this
 * app explains, is a restart: a boot loop of a few seconds and a restart between rare reports both show.
 */
class WifiRestartWatch
{
public:
    /** One report; nowMs from a monotonic clock. True when it shows a restart since the previous report. */
    bool onUptime(uint32_t uptimeS, qint64 nowMs);
    /** The previous report's uptime plus the time since, whole seconds; 0 before the first report. */
    qint64 expectedS(qint64 nowMs) const { return seen_ ? qint64(lastS_) + (nowMs - lastAtMs_) / 1000 : 0; }
    uint32_t lastUptimeS() const { return lastS_; }
    bool seen() const { return seen_; }
    /** The module's reset reason (esp_reset_reason_t, the last byte of ID_WIFI v1 from 0.11) in words. */
    static QString reasonText(int reason);

private:
    static constexpr qint64 kStallS = 5;   ///< reports this much late on this side are not a restart
    bool seen_ = false;
    uint32_t lastS_ = 0;
    qint64 lastAtMs_ = 0;
};

/**
 * @brief Signal level of a Wi-Fi module for its icon (dot + three arcs), from the RSSI of its ID_WIFI v1 report: a
 * station reports its access point, an access point its weakest client (0 = no clients). The thresholds live here only.
 */
struct WifiSignal
{
    /** -1 no link (not connected, or an access point without clients): everything dim; 0 below -80 dBm: only the dot;
     *  1 from -80, 2 from -70, 3 from -60 dBm lit arcs. `linked`: connected station or running access point. */
    static int level(bool linked, int rssiDbm)
    {
        if (!linked || rssiDbm >= 0)
            return -1;
        if (rssiDbm >= -60)
            return 3;
        if (rssiDbm >= -70)
            return 2;
        if (rssiDbm >= -80)
            return 1;
        return 0;
    }

    /** Link report period asked of the module on this app's own port, so the badge's number follows the antenna
     *  (field test 2026-09-29); the module takes 100..60000 ms (ID_WIFI v1 SETTING). */
    static constexpr int kFastReportMs = 200;
};

/**
 * @brief The Wi-Fi module's survey of the channels (ID_WIFI_SURVEY 0x59, firmware 0.16; contract: the module project's
 * docs/SBP_WIFI.md 3b): per channel the share of the dwell the air carried Wi-Fi frames ("Wi-Fi airtime": a lower
 * bound, no interference that is not Wi-Fi, no gaps, no ACKs), frames, transmitters, the strongest RSSI and the noise
 * floor (not to be believed while flag bit2 is set; on the ESP32-C3 it always is).
 */
struct WifiSurvey
{
    static constexpr int kDefaultDwellMs = 120;   // the module's own default (0 in the request means the same)
    // What the app asks for (owner 2026-09-29: stay longer): 120 ms gave 16.7, 9.7, 0.0 % on one channel in three
    // surveys in a row; 11 channels x (300 + 30 ms) is about 3.6 s a survey, the boat link pausing meanwhile.
    static constexpr int kDwellMs = 300;
    static constexpr int kTiePermille = 5;        // airtime within this of the least counts as a tie
    // Firmware 0.16 only (no own-network field): our own channel carries our own link as well, so another channel
    // wins over it only when it is lower by more than this (an Assumption). From 0.17 the module measures it.
    static constexpr int kOwnLinkPermille = 20;

    struct Channel {
        bool valid = false;
        int channel = 0, dwellMs = 0, busyPermille = 0, frames = 0, transmitters = 0;
        int strongest = -128;                     // -128: nothing heard
        int noiseDbm = 0;
        int flags = 0;                            // bit0 our own channel, bit1 the access point may use it, bit2 noise unreliable
        int ownPermille = 0;                      // of the airtime, our own network's (firmware 0.17)
        bool ownValid = false;                    // the module sent it
        bool own() const { return flags & 0x01; }
        bool apUsable() const { return flags & 0x02; }
        bool noiseValid() const { return !(flags & 0x04); }
    };

    /** 1 from firmware 0.16, 0 before, -1 while the version is not known. */
    static int supportFor(int fwMajor, int fwMinor)
    {
        if (fwMajor <= 0 && fwMinor <= 0)
            return -1;
        return (fwMajor > 0 || fwMinor >= 16) ? 1 : 0;
    }

    /** The airtime a channel is compared with, the other networks' only: less our own network's on every channel
     *  (a neighbour hears our link too; moving, we take it with us), from 0.17; before, our channel less the guess. */
    static int compared(const Channel& c)
    {
        if (c.ownValid)
            return std::max(0, c.busyPermille - c.ownPermille);
        return c.own() ? std::max(0, c.busyPermille - kOwnLinkPermille) : c.busyPermille;
    }

    /** The channel for the access point: the least airtime (compared()) among the channels it may use; within
     *  kTiePermille of it our own channel first (no move for nothing), then 1, 6 or 11, then the lower channel.
     *  0 when there is none. */
    static int best(const QVector<Channel>& chans)
    {
        int min = 1 << 30;
        for (const Channel& c : chans)
            if (c.valid && c.apUsable())
                min = std::min(min, compared(c));
        int pick = 0, pickRank = 99;
        for (const Channel& c : chans) {
            if (!c.valid || !c.apUsable() || compared(c) > min + kTiePermille)
                continue;
            const int rank = c.own() ? 0 : (c.channel == 1 || c.channel == 6 || c.channel == 11) ? 1 : 2;
            if (pick == 0 || rank < pickRank || (rank == pickRank && c.channel < pick)) {
                pick = c.channel;
                pickRank = rank;
            }
        }
        return pick;
    }

    /** How long a survey may take before the app asks for it once more: the channels at (dwell + the 30 ms return to
     *  the module's own channel), the module's own 4 s grace, and 2 s for the frames. */
    static int timeoutMs(int channels, int dwellMs) { return channels * (dwellMs + 30) + 6000; }
};

/**
 * @brief The last surveys, so that the pick does not jump with bursty traffic (owner 2026-09-29: pick by the mean of
 * the last measurements). Up to kKeep surveys not older than kMaxAgeMs; a change of the module's own channel starts it
 * over (our own link moved with it, the old figures no longer describe the air).
 */
class WifiSurveyWindow
{
public:
    static constexpr int kKeep = 5;
    static constexpr qint64 kMaxAgeMs = 60000;

    void add(const QVector<WifiSurvey::Channel>& survey, qint64 nowMs)
    {
        const int own = ownOf(survey);
        if (!entries_.isEmpty() && entries_.last().own != own)
            entries_.clear();
        dropOld(nowMs);
        entries_.append({ nowMs, survey, own });
        while (entries_.size() > kKeep)
            entries_.removeFirst();
    }
    void clear() { entries_.clear(); }
    /** Surveys the mean is taken over now. */
    int count(qint64 nowMs) const
    {
        int n = 0;
        for (const Entry& e : entries_)
            if (nowMs - e.atMs <= kMaxAgeMs)
                ++n;
        return n;
    }
    /** The channels of the last survey, each with its airtime (and frames, transmitters) the mean over the window and
     *  its strongest RSSI the strongest of them; empty when the window is. */
    QVector<WifiSurvey::Channel> mean(qint64 nowMs) const
    {
        QVector<WifiSurvey::Channel> out;
        if (entries_.isEmpty())
            return out;
        for (const WifiSurvey::Channel& last : entries_.last().channels) {
            if (!last.valid)
                continue;
            WifiSurvey::Channel m = last;
            int n = 0, nOwn = 0;
            long busy = 0, frames = 0, tx = 0, ownAir = 0;
            for (const Entry& e : entries_) {
                if (nowMs - e.atMs > kMaxAgeMs)
                    continue;
                for (const WifiSurvey::Channel& c : e.channels) {
                    if (!c.valid || c.channel != last.channel)
                        continue;
                    ++n;
                    busy += c.busyPermille;
                    frames += c.frames;
                    tx += c.transmitters;
                    m.strongest = std::max(m.strongest, c.strongest);
                    if (c.ownValid) {
                        ++nOwn;
                        ownAir += c.ownPermille;
                    }
                }
            }
            if (n > 0) {
                m.busyPermille = int((busy + n / 2) / n);
                m.frames = int((frames + n / 2) / n);
                m.transmitters = int((tx + n / 2) / n);
            }
            m.ownValid = nOwn > 0;
            m.ownPermille = nOwn > 0 ? int((ownAir + nOwn / 2) / nOwn) : 0;
            out.append(m);
        }
        return out;
    }

private:
    struct Entry { qint64 atMs; QVector<WifiSurvey::Channel> channels; int own; };
    static int ownOf(const QVector<WifiSurvey::Channel>& s)
    {
        for (const WifiSurvey::Channel& c : s)
            if (c.valid && c.own())
                return c.channel;
        return 0;
    }
    void dropOld(qint64 nowMs)
    {
        while (!entries_.isEmpty() && nowMs - entries_.first().atMs > kMaxAgeMs)
            entries_.removeFirst();
    }
    QVector<Entry> entries_;
};

/**
 * @brief The Wi-Fi module's radio switched off to swap an antenna live (ID_WIFI_RF 0x5A, firmware 0.20; contract: the
 * module project's docs/SBP_WIFI.md, ID_WIFI_RF). The module stops its radio 200 ms after it answers and starts it
 * again when its timer runs out, on "on", or at a reboot; nothing is saved. A request that reaches it over the network
 * must carry a timer of 1..kNetworkMaxS s (it would be cut off through the very radio it stops); over a wire (X1, X2)
 * the timer is optional, 0 = until "on" or a reboot.
 */
struct WifiRf
{
    static constexpr int kNetworkMaxS = 600;
    static constexpr int kWiredMaxS = 65535;
    /** After the radio is back, the link between two modules returns in about this: a station searches every 15 s
     *  (the contract's Inference). */
    static constexpr int kRelinkS = 20;

    /** 1 from firmware 0.20, 0 before, -1 while the version is not known. */
    static int supportFor(int fwMajor, int fwMinor)
    {
        if (fwMajor <= 0 && fwMinor <= 0)
            return -1;
        return (fwMajor > 0 || fwMinor >= 20) ? 1 : 0;
    }

    /** Whether the module takes "off for `seconds`" (0 = no timer) from a request over a wire (`wired`) or not. */
    static bool offAllowed(int seconds, bool wired)
    {
        if (seconds < 0 || seconds > (wired ? kWiredMaxS : kNetworkMaxS))
            return false;
        return wired || seconds > 0;
    }

    /** Whole seconds left of the module's timer, counted on from its report of `reportedS` at `atMs`, rounded up; 0
     *  once it ran out or when there was none. */
    static int leftS(int reportedS, qint64 atMs, qint64 nowMs)
    {
        if (reportedS <= 0)
            return 0;
        const qint64 left = qint64(reportedS) * 1000 - (nowMs - atMs);
        return left <= 0 ? 0 : int((left + 999) / 1000);
    }

    /** A status contradicting the module's own answer of less than this ago left the module before the answer (the
     *  radio stops 200 ms after it): it is not taken. */
    static constexpr qint64 kSettleMs = 1000;
    enum StatusStep {
        StatusKeep,     ///< the status agrees, or is too close to the answer
        StatusBackOn,   ///< the radio came back (the boat module's push of it was lost while the link was down)
        StatusOffAsk,   ///< off, by another host or unseen: forget the answer, ask how long
        StatusAsk       ///< off known only from the status: ask how long
    };
    /** What flag bit3 of a status (ID_WIFI v0) does to the module's last ID_WIFI_RF answer (`valid`, `on`, given
     *  `sinceMs` ago). */
    static StatusStep onStatus(bool valid, bool on, bool flagOff, qint64 sinceMs)
    {
        if (valid && on == flagOff && sinceMs > kSettleMs)
            return flagOff ? StatusOffAsk : StatusBackOn;
        if (!valid && flagOff)
            return StatusAsk;
        return StatusKeep;
    }
};

/**
 * @brief How busy each 2.4 GHz channel is by a scan (ID_WIFI v2), and the least busy one for the access point.
 *
 * Every network heard adds its power (from its RSSI) to the channel rated, weighted by how much of that channel's band
 * it covers: a 20 MHz channel is flat over +-9 MHz (the 802.11 OFDM mask) and channels are 5 MHz apart, so a network
 * 1, 2 or 3 channels off covers 14/18, 9/18 or 4/18 of it, one 4 off only its skirt (-20 dBr and below), 5 and more
 * nothing. Our own network (the one the module is on) is not counted. A scan hears access points by their beacons,
 * not the traffic on a channel. The module's access point takes channels 1..11.
 */
struct WifiChannels
{
    static constexpr int kFirst = 1;
    static constexpr int kLast = 13;              // what a scan may report
    static constexpr int kLastAp = 11;            // what the module's access point takes
    static constexpr double kFloorDbm = -100.0;   // nothing heard

    struct Net { int channel = 0; int rssi = 0; bool own = false; };
    struct Channel { int channel = 0; int networks = 0; int strongest = 0; double levelDbm = kFloorDbm; };

    static double weight(int delta)
    {
        switch (delta < 0 ? -delta : delta) {
        case 0: return 1.0;
        case 1: return 14.0 / 18.0;
        case 2: return 9.0 / 18.0;
        case 3: return 4.0 / 18.0;
        case 4: return 0.01;
        default: return 0.0;
        }
    }

    /** Channels kFirst..kLast: the networks on each (not our own), the strongest of them, the level summed. */
    static QVector<Channel> rate(const QVector<Net>& nets)
    {
        QVector<Channel> out;
        for (int c = kFirst; c <= kLast; ++c) {
            Channel ch;
            ch.channel = c;
            double mw = 0.0;
            for (const Net& n : nets) {
                if (n.own || n.channel < kFirst || n.channel > kLast || n.rssi >= 0)
                    continue;
                if (n.channel == c) {
                    if (ch.networks == 0 || n.rssi > ch.strongest)
                        ch.strongest = n.rssi;
                    ++ch.networks;
                }
                mw += weight(c - n.channel) * std::pow(10.0, n.rssi / 10.0);
            }
            ch.levelDbm = mw > 0.0 ? std::max(kFloorDbm, 10.0 * std::log10(mw)) : kFloorDbm;
            out.append(ch);
        }
        return out;
    }

    /** The channel for the access point: the lowest level among 1..kLastAp; within 1 dB of it 1, 6 or 11 first
     *  (they do not overlap each other), then the lower channel. 0 when nothing was rated. */
    static int best(const QVector<Channel>& rated)
    {
        double min = 1e9;
        for (const Channel& ch : rated)
            if (ch.channel >= kFirst && ch.channel <= kLastAp)
                min = std::min(min, ch.levelDbm);
        int pick = 0;
        bool pickPreferred = false;
        for (const Channel& ch : rated) {
            if (ch.channel < kFirst || ch.channel > kLastAp || ch.levelDbm > min + 1.0)
                continue;
            const bool preferred = ch.channel == 1 || ch.channel == 6 || ch.channel == 11;
            if (pick == 0 || (preferred && !pickPreferred)) {
                pick = ch.channel;
                pickPreferred = preferred;
            }
        }
        return pick;
    }
};

/**
 * @brief Radio-setting rules of the module (ID_WIFI v7) that it enforces with ERR_PAYLOAD, so the app never sends a
 * combination the module refuses. Protocol bits: 0x01 b, 0x02 g, 0x04 n, 0x08 LR.
 */
/** Port information (ID_WIFI_NET v7) by the module's firmware (ID_VERSION v2): 1 from 0.12, 0 before, -1 while the
 *  version is not known (not read yet: 0.0). The firmware decides, not a timeout: a module that has just started
 *  may leave the first v7 request unanswered. */
struct WifiNetRules
{
    static int portsFor(int fwMajor, int fwMinor)
    {
        if (fwMajor <= 0 && fwMinor <= 0)
            return -1;
        return (fwMajor > 0 || fwMinor >= 12) ? 1 : 0;
    }
    /** From 0.15 a rate set through the same port (ID_UART) is saved at once: no return after 10 s, only holding the
     *  module's BOOT button 5 s puts both ports back to 921600. False while the version is not known. */
    static bool rateSavedAtOnce(int fwMajor, int fwMinor) { return fwMajor > 0 || fwMinor >= 15; }
};

/** Port rates of the Wi-Fi module (ESP32-C3 UART, both ports alike). */
struct WifiRates
{
    static constexpr int kMin = 9600;
    static constexpr int kMax = 5000000;   // what any firmware takes; the module itself refuses above its own limit
    /** Top rate of the module's firmware (ID_VERSION v2): 5000000 from 0.13, 4000000 before or while unknown. */
    static int maxFor(int fwMajor, int fwMinor) { return (fwMajor > 0 || fwMinor >= 13) ? kMax : 4000000; }
};

struct WifiRadioRules
{
    /** 40 MHz needs 802.11n: b (0x01), b/g (0x03) and LR only (0x08) run at 20 MHz. */
    static bool allows40(int protocols) { return (protocols & 0x04) != 0; }
    /** The width to send with these protocols: the asked one where 40 MHz is possible, 20 MHz otherwise. */
    static int widthFor(int protocols, int widthMhz) { return (widthMhz == 40 && allows40(protocols)) ? 40 : 20; }
    /** LR only for an access point by the module's firmware (ID_VERSION v2): 1 from 0.12, 0 before (0.11 refuses it
     *  with ERR_PAYLOAD), -1 while the version is not known (not read yet: 0.0). */
    static int lrOnlyApSupport(int fwMajor, int fwMinor)
    {
        if (fwMajor <= 0 && fwMinor <= 0)
            return -1;
        return (fwMajor > 0 || fwMinor >= 12) ? 1 : 0;
    }
};

/**
 * @brief The module's count of requests it could not queue (ID_WIFI v1, firmware 0.14): counted since the module's
 * start, never reset by reading, wrapping at 2^32; 0 is normal. Tells when a log line is due and how many requests it
 * covers: at most one line per interval, with the sum of what was lost meanwhile. The first report is the baseline
 * (its count is history, written with "module up"); after a restart the module counts from 0 again.
 */
class WifiLossWatch
{
public:
    explicit WifiLossWatch(qint64 intervalMs = 10000) : intervalMs_(intervalMs) {}
    /** One report; nowMs from a monotonic clock; restarted: the module restarted since the previous report.
     *  Returns the requests lost since the last line when a line is due, 0 otherwise. */
    uint32_t onReport(uint32_t total, qint64 nowMs, bool restarted);

private:
    qint64 intervalMs_;
    bool seen_ = false;
    uint32_t last_ = 0;       ///< the previous report's total
    uint32_t pending_ = 0;    ///< lost since the last line
    qint64 lineAtMs_ = -1;    ///< when the last line was due
};

/**
 * @brief After a radio setting: waits for the link to the access point, compares what is in effect with
 * what was asked, and when the module could not do it (a power between the driver's steps, 40 MHz from an
 * access point that gives 20) saves what is in effect, so the module's settings and the link agree.
 *
 * The module (before 0.11) does not push its radio state after a reconnect, so it is asked for until the
 * link is back. The width in effect comes from the negotiated PHY (HT40 = 40 MHz, anything else 20 MHz):
 * the module's own "width in use" is the driver's setting and read 40 on an HT20 link. An access point has
 * no negotiated PHY (0xFF): there the width in effect is what the driver was given, and nothing waits for
 * a link.
 */
class WifiRadioCheck : public QObject
{
    Q_OBJECT

public:
    struct Hooks {
        std::function<void()> askRadio;                        ///< ID_WIFI v7 GETTING
        std::function<void(double, int, int, int)> saveRadio;  ///< power dBm, protocols, width MHz, power save
    };
    struct Timing {
        int settleMs = 1500;      ///< reports before this are from before the change
        int askMs = 2000;         ///< between two radio requests while waiting
        int deadlineMs = 30000;   ///< no link by then: give up
    };
    enum Outcome { AsAsked = 0, Corrected, NoLink };

    WifiRadioCheck(Hooks hooks, Timing timing, QObject* parent = nullptr);

    /** What was just asked of the module (power already on the driver's quarter-dB grid); accessPoint when
     *  the module runs as one. */
    void start(double powerDbm, int protocols, int widthMhz, int powerSave, bool accessPoint = false);
    /** Every radio report: the power in use, the negotiated PHY (0xFF = no link) and the width the driver
     *  was given (v7 byte 7; used in the access-point role). */
    void onRadio(double powerNowDbm, int phyNow, int widthSetMhz = 0);
    /** The module refused the setting: nothing to check. */
    void cancel();
    bool active() const { return active_; }

    /** The width the link really uses, from the negotiated PHY; 0 without a link. */
    static int widthOfPhy(int phy) { return phy == 0xFF ? 0 : (phy == 5 ? 40 : 20); }

signals:
    void finished(int outcome, const QString& text);
    void activeChanged();

private:
    void onAsk();
    void stop();

    Hooks hooks_;
    Timing timing_;
    bool active_ = false;
    bool ap_ = false;
    double askPower_ = 0;
    int askProto_ = 0;
    int askWidth_ = 20;
    int askPs_ = 0;
    QElapsedTimer since_;
    QTimer askTimer_;
};
