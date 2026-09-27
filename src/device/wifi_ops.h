#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <functional>

/**
 * @brief Changing the Wi-Fi module's port speed (its line 0 to this host) as one sequence that does not
 * depend on a settings page being open (the device settings page is unloaded as soon as it is left).
 *
 * The module is told the new speed (it answers at the old one and then switches), this host's link
 * follows after a short pause, the module has to answer a status request at the new speed, and only then
 * is the speed saved in the module. Without an answer the link goes back to the old speed; a speed the
 * module was given but did not save is gone at its next reboot.
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
