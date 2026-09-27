#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVector>
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

    /** False while a change runs, for a speed outside 9600..4000000, or when nothing would change. */
    bool start(int newBaud, int oldBaud);
    /** A frame from the module that proves it hears this host; counts only after the first check. */
    void moduleAnswered();
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

    /** False while a change runs, for a line other than 0/1, an empty edit, or a rate outside 9600..4000000. */
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
};

/**
 * @brief Radio-setting rules of the module (ID_WIFI v7) that it enforces with ERR_PAYLOAD, so the app never sends a
 * combination the module refuses. Protocol bits: 0x01 b, 0x02 g, 0x04 n, 0x08 LR.
 */
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
