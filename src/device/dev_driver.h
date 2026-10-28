#pragma once

#include <QSet>
#include <atomic>
#include <QObject>
#include <QHash>
#include <QMap>
#include <QVector>
#include <QTimer>
#include <QDateTime>
#include <QUuid>
#include <QVariantMap>
#include <QVariantList>
#include <QStringList>
#include "proto_binnary.h"
#include "id_binnary.h"
#include "wifi_ops.h"
#include "device_liveness.h"
#include "node_probe.h"


using namespace Parsers;

class DevDriver : public QObject
{
    Q_OBJECT
public:
    explicit DevDriver(QObject *parent = nullptr);
    ~DevDriver() override;
    typedef enum : quint8 {
        DatasetOff = 0,
        DatasetCh1 = 1,
        DatasetCh2 = 2,
        DatasetRequest = 255
    } DatasetChannel;

    enum UpgradeStatus : qint8 {
        failUpgrade = -1,
        successUpgrade = 101
    };

    ChannelId getChannelId() const;

    int distMax();
    void setDistMax(int dist);

    int distDeadZone();
    void setDistDeadZone(int dead_zone);

    int distConfidence();
    void setConfidence(int confidence);

    int chartSamples();
    void setChartSamples(int samples);

    int chartResolution();
    void setChartResolution(int resol);

    int chartOffset();
    void setChartOffset(int offset);

    /// Sides in the last complete echogram frame: 2 for ID_CHART / ID_CHART_Z v1 (both sides
    /// interleaved), 1 for v0, 0 before the first frame. A two-sided device counts both sides in
    /// ID_CHART_SETUP, so one side gets chartSamples() / 2 of them.
    int chartSides() const { return chartSides_; }

    int dspSmoothFactor();
    void setDspSmoothFactor(int dsp_smooth);

    int datasetTimestamp();
    void setDatasetTimestamp(int ch_param);
    int datasetDist();
    void setDatasetDist(int ch_param);
    int datasetChart();
    void setDatasetChart(int ch_param);
    // Compressed echogram: a separate axis from datasetChart, because that one
    // is a channel mask, not a format.
    int datasetChartZ();
    void setDatasetChartZ(int ch_param);
    // USB tap (IQ / RAW): only asks the device to publish; the data comes through UsbTap.
    int datasetTapIq();
    void setDatasetTapIq(int ch_param);
    int datasetTapRaw();
    void setDatasetTapRaw(int ch_param);
    int datasetTemp();
    void setDatasetTemp(int ch_param);
    // Supply voltage (IDBinDataset::MASK_VOLTAGE_V0); the readings come as supplyVoltageComplete.
    int datasetVoltage();
    void setDatasetVoltage(int ch_param);
    int datasetEuler();
    void setDatasetEuler(int ch_param);
    int datasetSDDBT();
    void setDatasetSDDBT(int ch_param);
    int datasetSDDBT_P2();
    void setDatasetSDDBT_P2(int ch_param);

    int ch1Period();
    void setCh1Period(int period);

    int ch2Period();
    void setCh2Period(int period);

    void sendUpdateFW(QByteArray update_data);
    bool isUpdatingFw() { return m_state.in_update; }
    int upgradeFWStatus() {return m_upgrade_status; }

    /// A sonar answering on a bus address 1..15: one board of a ring on a shared line (or a lone board
    /// with an address). The stock flow cannot reach its bootloader; its update goes through
    /// RingUpgrade (docs/RING_UPDATE.md), which sendUpdateFW() hands it to. Not a sonar behind a
    /// recorder: its "address" is the recorder's port number, its frames come as KP2, and the ring
    /// procedure cannot drive the recorder's port.
    bool isRingBoard() { return boardVersion() == BoardBasic2D && lastAddress_ >= 1 && lastAddress_ <= 15 && !viaRecorder_; }
    /// A sonar of a ring (bus address 1..15, firmware 1.14+) behind a recorder. The recorder hands every
    /// frame to its sonar port as a broadcast, so this sonar's commands go wrapped in ID_NODE_CMD naming
    /// it (docs/RING_UPDATE.md). Its firmware cannot be updated through the recorder: the bootloader
    /// needs 115200 on a port the recorder keeps at its own rate.
    bool isRingBehindRecorder() {
        const int fw = idVersion->fwVersion(), fwMinor = idVersion->fwVersionMinor();
        return boardVersion() == BoardBasic2D && viaRecorder_ && lastAddress_ >= 1 && lastAddress_ <= 15
               && (fw > 1 || (fw == 1 && fwMinor >= 14));
    }
    /// Any device behind a recorder (its frames come as KP2), ring or not, except the recorder itself and a
    /// Wi-Fi module. Its firmware cannot be updated through the recorder, for the reason above.
    bool isBehindRecorder() { return viaRecorder_ && !isRecorder() && !isWifi(); }
    /// A device behind a recorder whose commands go plain on the route the app sees -- a board with no bus
    /// address, which the recorder stamps with its port. It is updated through the recorder: moved to 115200
    /// first, then the stock update with the bootloader on route 0 (docs/RING_UPDATE.md).
    bool isRecorderUpgradeSupported() { return isBehindRecorder() && !(isRingBehindRecorder() && commandsGoWrapped()); }
    /// Version the image is expected to report ("1.12"), taken from the file name; empty = not checked.
    void setUpgradeExpect(const QString& version) { upgradeExpect_ = version; }
    /// Every ring board on this device's line, the last one first.
    void sendUpdateFWAll(QByteArray update_data);
    /// Progress and outcome of a ring update, in the stock convention: 0..100, 101 done, -1 failed.
    void setRingUpgradeStatus(int status, bool done);
    /// A sonar whose bus address the app sets (BusAddress, docs/RING_UPDATE.md): a Basic2D board running
    /// the ring firmware (1.12+), not behind a recorder -- there the route is the recorder's port number.
    bool isBusAddressSettable() {
        const int fw = idVersion->fwVersion(), fwMinor = idVersion->fwVersionMinor();
        return boardVersion() == BoardBasic2D && !viaRecorder_ && (fw > 1 || (fw == 1 && fwMinor >= 12));
    }
    /// Sets this sonar's bus address and saves it in its flash; DeviceManager runs it and
    /// busAddressDone() tells the outcome. The run first checks that the board is alone on its line.
    Q_INVOKABLE void assignBusAddress(int address);
    /// The outcome of assignBusAddress(), from DeviceManager. On success this device answers on `address`.
    void setBusAddressResult(bool ok, int address, const QString& detail);
    bool busAddressBusy() const { return busAddressBusy_; }
    /// The whole line's UART rate, asked by a ring board (RingRate, docs/RING_UPDATE.md): both boards and the
    /// host's port move together and keep it -- a board's RX and TX are one UART and each board passes the
    /// other's bytes on, so the line runs at one rate. ringRateDone() tells the outcome.
    Q_INVOKABLE void setRingLineRate(int rate);
    /// The outcome of setRingLineRate(), from DeviceManager; `rate` is where the host's port was left.
    void setRingRateResult(bool ok, int rate, const QString& detail);
    bool ringRateBusy() const { return ringRateBusy_; }
    /// The per-device baud control is hidden: a Wi-Fi module has its own port settings, and a ring board's
    /// rate is the whole line's (setRingLineRate) -- changed on one board alone it breaks the ring.
    bool baudrateHidden() { return isWifi() || isRingBoard(); }

    void sendFactoryFW(QByteArray update_data);

    int transFreq();
    void setTransFreq(int freq);

    int transPulse();
    void setTransPulse(int pulse);

    int transBoost();
    void setTransBoost(int boost);

    /// Presets in the sonar's own flash (ID_PRESET, nanoSSS firmware 11.21+): a map a slot -- slot, used,
    /// factory, name, freqKhz, pulses, boost, dsp, cells, cellMm, offset, periodMs -- empty until the sonar has
    /// answered on this connection.
    QVariantList presetSlots();
    /// The sonar has answered about its presets on this connection: it knows ID_PRESET.
    bool presetsKnown() const { return presetsAnswered_; }
    /// Apply a slot; the sonar then reports its settings as after a change by hand.
    Q_INVOKABLE void presetRecall(int slot);
    /// Save the sonar's current settings into a slot under `name` (24 bytes of UTF-8 at most).
    Q_INVOKABLE void presetSave(int slot, const QString& name);
    /// Empty a slot (SETTING v2 with the key). No button calls it in this version: the live check of the buttons on a
    /// sonar (tools/preset_check/preset_sonar.ps1) undoes its save with it.
    Q_INVOKABLE void presetDelete(int slot);

    /// A GODIO ring board's sync counters (ID_RING_STAT, firmware 1.16+; docs/RING_UPDATE.md): a ring board
    /// answering on a bus address, not behind a recorder.
    bool ringStatSupported() {
        const int fw = idVersion->fwVersion(), fwMinor = idVersion->fwVersionMinor();
        return isRingBoard() && (fw > 1 || (fw == 1 && fwMinor >= 16));
    }
    /// The counters as last read: known, address, role (0 leader or alone, 1 follower), upstream (a board before
    /// this one), guardMs, breakIntervalMs (0 not measured), relayed, breaks, torn, timeouts, skipped; error: why
    /// the last request got no state, empty when it did.
    QVariantMap ringStat();
    /// A request is out and nothing has come back yet (2 s at most).
    bool ringStatBusy() const { return ringStatTimer_.isActive(); }
    /// Asks the board once. Only on request: on a ring with no slack an answer can cost a following board a ping.
    Q_INVOKABLE void ringStatRead();

    /// A Recorder's connectors (ID_BUS / ID_PIN, Recorder firmware with the port table; the recorder fork's
    /// docs/Port-Config-Spec.md). Asked once per connection of a Recorder only, one frame at a time.
    /// 1: described, -1: the firmware has none (RespErrorID or silence), 0: not known yet.
    int portsSupport() const { return portsSupport_; }
    /// Every connector: id, label, supply (0 none, 1 VBAT, 2 +5.2 V), function, functionSaved, functionCaps, uart,
    /// uartSaved, uartCaps, txPin, rxPin, baud, pins (each: id, label, mcuPin, roleCaps, role, roleSaved, timer,
    /// param, value). Empty until the table is read whole.
    QVariantList portBuses();
    /// The contacts each logical PWM output OUT1..OUT3 drives (role Pwm, param = its index), labels joined: for the
    /// servo panel's routing rows. Empty strings while the table is not known.
    QStringList portPwmOutPins();
    /// ID_VOLTAGE channels 0 and 1 as last read, millivolts; -1 until one comes. On a Recorder 0 is VBAT and 1 the
    /// +5.2 V rail; on a nanoSSS 1 is a diagnostic channel (receivedVoltage names them).
    int voltage0Mv() const { return voltage0Mv_; }
    int voltage1Mv() const { return voltage1Mv_; }
    /// While on, the pins' roles and values are asked every 200 ms in one frame (ID_PIN v1): a page showing them.
    Q_INVOKABLE void setPortsLive(bool on);

    /// The Recorder's UART connectors: each one's function and each function's rate (ID_BUS V0 SETTING, V2..V4; the
    /// recorder fork's Port-Config-Spec.md). After the table, each function's settings are read once.
    /// 1: the firmware answered them; -1: it has none (refused or silent); 0: not known yet.
    int portSettable() const;
    /// By function (index 1 sonar A .. 4 navigation; 0 unused): support (1, -1, 0), valid, baud (the setting, 0 auto).
    QVariantList portFunctions() const;
    /// The rates the firmware takes: 0 (auto), then 9600 .. 5000000.
    QVariantList portBauds() const;
    /// Where the last change stands, and the connector it was made on (-1 none yet).
    enum PortState { PortIdle = 0, PortSending, PortSaved, PortRefusedLink, PortRefusedValue, PortNoAnswer, PortNotSaved };
    int portState() const { return portState_; }
    int portStateBus() const { return portOpBus_; }
    /// The app's link to this device is a serial port. The firmware changes its UART connectors only from its USB
    /// port and refuses through a UART, which is how a UDP or TCP link (the Wi-Fi bridge) reaches it. A serial
    /// adapter on a UART connector is a serial link too: the refusal tells it apart (PortRefusedLink).
    bool linkSerial() const { return linkSerial_; }
    void setLinkSerial(bool on);
    /// The function onto the connector (a swap with the connector that has it): SETTING v0, on RespOk ID_FLASH, then
    /// the four UART connectors read again. One change at a time.
    Q_INVOKABLE void setPortFunction(int bus, int function);
    /// The function's rate (0 auto): SETTING v2..v4, on RespOk ID_FLASH, then its settings and its connector read
    /// again. A rate not in portBauds() is not sent.
    Q_INVOKABLE void setPortBaud(int function, int baud);

    /// Zeroes the board's counters (SETTING v0 with the key); the board sends its state after.
    Q_INVOKABLE void ringStatReset();

    /// Another device of this driver's line has been heard (DeviceManager, on each of its frames). For address 0's
    /// driver that has never had a frame -- the line's search -- this holds its asks (DeviceLiveness::searchHeld).
    void holdSearch() { otherDeviceHeardMs_ = QDateTime::currentMSecsSinceEpoch(); }

    /// Manual mode of a Recorder's servo (ID_SERVO_CONTROL, bit 1 of general; docs/SERVO_MANUAL.md): the servo
    /// holds an angle set here and the scan stops. Each change is read back from the device a moment later.
    bool servoManual() const;
    void setServoManual(bool on);
    /// The angle held in manual mode, degrees.
    double servoManualDeg() const;
    /// 1: the device has shown manual mode; -1: it refused it (a firmware without it); 0: not known yet.
    /// Per connection.
    int servoManualSupport() const { return servoManualSupport_; }
    /// A drag in manual mode: sent at once, then at most every kServoManualMs; a value that comes in between waits
    /// for the next slot, and only the last such one is sent. Not read back -- servoManualSet() ends a drag.
    Q_INVOKABLE void servoManualMove(double deg);
    /// The angle to hold, sent now: the end of a drag or a typed value.
    Q_INVOKABLE void servoManualSet(double deg);
    static constexpr int kServoManualMs = 25;   // 40 frames a second at most, the servo's PWM runs at 50 Hz

    int soundSpeed();
    void setSoundSpeed(int speed);

    // Network settings of the device's own USB link (Baikal-sonar, ID_NET).
    // Read-only properties plus one call that sends all three at once: an
    // address, a mask and a host address only make sense together, and the
    // device refuses a set that cannot work.
    QString netIp();
    QString netMask();
    QString netHost();
    QString netMac();
    bool netDhcp();
    bool netLive();
    bool netValid();
    Q_INVOKABLE bool applyNet(const QString& ip, const QString& mask, const QString& host, bool dhcp);
    Q_INVOKABLE void requestNet();

    // Wi-Fi module (board 87), ID_WIFI: mirrors of the module's last frames and the commands the
    // Wi-Fi island sends. Field meaning: docs/WIFI_MODULE.md. Readers are cheap; the lists are
    // rebuilt on every call, which is fine for a dozen networks.
    bool wifiStatusValid();
    int wifiState();
    bool wifiAutoConnect();
    bool wifiCurrentSaved();
    QString wifiSsid();
    int wifiRssi();
    int wifiChannel();
    int wifiSecurity();
    int wifiReason();
    int wifiDisconnectCode();
    QString wifiBssid();
    QString wifiIp();
    QString wifiMask();
    QString wifiGateway();
    QString wifiDns();
    int wifiRxBps();
    int wifiTxBps();
    bool wifiReportValid();
    double wifiRxTotal();
    double wifiTxTotal();
    int wifiUptime();
    QString wifiResetText();
    /** Requests the module could not queue since its start (firmware 0.14+); -1 when it does not report them. */
    double wifiLostRequests();
    /** "[address]" in front of this module's Wi-Fi log lines: the head unit's module and one reached through the
     *  relay (the boat's, on our firmware) log side by side. */
    QString wifiTag() const;
    bool wifiScanning();
    QVariantList wifiNetworks();
    /** Channels 1..13 rated by the last scan (WifiChannels): channel, networks, strongest, level (dBm), best, ap
     *  (the access point may use it). Empty before the first scan result. */
    QVariantList wifiChannels();
    /** The least busy channel for the access point by the last scan, 0 before it. */
    int wifiBestChannel();
    /** The module's channel survey (ID_WIFI_SURVEY, firmware 0.16): 1 it has one, 0 not, -1 the version not known. */
    int wifiSurveySupport();
    bool wifiSurveying();
    /** The last survey per channel (WifiSurvey): channel, busy (per mille of Wi-Fi airtime), frames, transmitters,
     *  strongest (dBm, -128 nothing), noise, noiseValid, own, ap, best. Empty before the first survey. */
    QVariantList wifiSurveyChannels();
    /** The channel for the access point by the mean of the last surveys (WifiSurveyWindow, WifiSurvey::best), 0
     *  before the first. */
    int wifiSurveyBest();
    /** Surveys the mean is taken over now (WifiSurveyWindow: up to 5 within a minute, since our channel last changed). */
    int wifiSurveySamples();
    /** Why the last survey did not run or did not end; empty when it did. */
    QString wifiSurveyError() const { return wifiSurveyError_; }
    /** Starts a survey: the module leaves its channel for about 2 s (the boat link pauses meanwhile). */
    Q_INVOKABLE void wifiSurvey();
    /** The link rate report (ID_WIFI_SURVEY v1, firmware 0.18): 1 supported, 0 not, -1 not known yet. */
    int wifiLinkRateSupport();
    bool wifiLinkRateRunning();
    /** Per other end: mac, windowMs, frames, rssi (-128 none), mode, text (the most frames' rate; empty without
     *  frames), rates [{kind, code, sgi, bw40, kbps, frames, text}], retries (of this report's frames the ones that came
     *  again, -1 before firmware 0.20), and over the last reports (WifiRetryWindow) retrySum of retryFrames in
     *  retryReports. */
    QVariantList wifiLinkRates();
    QString wifiLinkRateError() const { return wifiLinkRateError_; }
    /** Asks for a report (a 200 ms window); nothing while one runs or before firmware 0.18. */
    Q_INVOKABLE void wifiLinkRate();
    /** The link's recent events (WifiLinkLog), oldest first: kind (0 state, 1 clients, 2 restart), atMs (wifiClockMs),
     *  state, prevState, reason, code, clients, prevClients, ssid, channel, rssi, text. */
    QVariantList wifiLinkEvents();
    /** The app's monotonic clock the link events are stamped with, ms. */
    Q_INVOKABLE double wifiClockMs() const { return wifiClock_.isValid() ? double(wifiClock_.elapsed()) : 0.0; }
    QStringList wifiSavedNetworks();

    // Firmware 0.20: the radio switched off to swap an antenna (ID_WIFI_RF 0x5A; the rules: WifiRf, device/wifi_ops.h).
    /** 1 from firmware 0.20, 0 before, -1 the version not known. */
    int wifiRfSupport();
    /** The radio is off on request: the module's last ID_WIFI_RF state; before one, flag bit3 of its status (ID_WIFI
     *  v0). A status that contradicts a state over a second old wins (the radio came back, or another host switched
     *  it off). */
    bool wifiRadioOff();
    /** Off with the module's timer running: it switches the radio on by itself, wifiRadioOffLeft() s from now. */
    bool wifiRadioOffTimed();
    /** Off with no timer, by the module's answer: until switched on or a reboot. Neither this nor timed while the
     *  off is known only from the status (the module is asked how long). */
    bool wifiRadioOffUntilOn();
    /** Whole seconds until the module's timer switches the radio on, counted on from its last answer; 0 when it ran
     *  out or there is none. A plain call (no NOTIFY): the page ticks it. */
    Q_INVOKABLE int wifiRadioOffLeft();
    /** This app reaches the module over a wire (X1/X2 by its latest port summary): "on" reaches it while the radio is
     *  off, and the timer may be left out. False over the network and while not known. */
    bool wifiRadioWired();
    /** Why the last switch-off or switch-on was refused; empty when it was not. */
    QString wifiRfError() const { return wifiRfError_; }
    /** Switches the radio off for `seconds`: 0 = until switched on or a reboot (over a wire only), over the network
     *  1..600 (WifiRf::offAllowed). False when nothing was sent: no connection, firmware before 0.20, or seconds the
     *  module would refuse. */
    Q_INVOKABLE bool wifiRadioSwitchOff(int seconds);
    /** Switches the radio on now; false when nothing was sent. */
    Q_INVOKABLE bool wifiRadioSwitchOn();
    /** Asks the module for its radio state (ID_WIFI_RF GETTING v0). */
    Q_INVOKABLE void wifiRadioRefresh();

    // Relay of the boat's SBP stream through the module (ID_WIFI v5 / v6, firmware 0.6+).
    int wifiRelaySupport();
    bool wifiRelayValid();
    int wifiRelayMode();
    int wifiRelayAddr();
    QString wifiRelayIp();
    int wifiRelayPort();
    int wifiRelayLocalPort();
    bool wifiRelayStatsValid();
    int wifiRelayState();
    QString wifiRelayPeerIp();
    int wifiRelayPeerPort();
    double wifiRelayUpFrames();
    double wifiRelayUpPackets();
    double wifiRelayUpDrops();
    double wifiRelayDownPackets();
    double wifiRelayDownFrames();
    double wifiRelayDownDrops();
    double wifiRelayLocalFrames();

    // Firmware 0.10: access-point role, radio (ID_WIFI v7) and the network setup (ID_WIFI_NET 0x58).
    // Structured values go to QML as maps and lists; field names are listed in docs/WIFI_MODULE.md.
    int wifiClients();
    int wifiRadioSupport();
    QVariantMap wifiRadio();
    int wifiNetSupport();
    QVariantMap wifiNetRole();
    QVariantMap wifiNetAp();
    QVariantMap wifiNetIp();
    QVariantList wifiNetLines();
    QVariantList wifiNetLineStats();
    QVariantList wifiNetClients();
    int wifiNetOwnAddr();
    // Firmware 0.12: the two physical ports X1 (0) and X2 (1), ID_WIFI_NET v7. Field names: docs/WIFI_MODULE.md §2e.
    /** -1 not yet known, 0 not in this firmware (before 0.12), 1 the module reports its ports. */
    int wifiNetPortSupport();
    /** Two maps (X1, X2): the port summary (page 0) with byte rates from successive summaries. */
    QVariantList wifiNetPorts();
    /** Two lists (X1, X2) of the devices heard from each port (page 1), most recent first. */
    QVariantList wifiNetPortDevices();
    /** Two maps (X1, X2): mode, state and peers of each port's line (page 2). */
    QVariantList wifiNetPortPeers();
    /** The port this app's requests come through by the module's latest summaries; -1 the network or unknown. */
    int wifiNetOwnPort();
    /** The name of a Kogger board id (ID_VERSION v0 byte 1) as the device list shows it; empty when unknown. */
    static QString knownBoardName(int board);

    /** Re-reads status and the saved list (GETTING v0 + v4). */
    Q_INVOKABLE void wifiRefresh();
    /** Starts a scan; results arrive over the next seconds and wifiNetworksChanged() follows each. */
    Q_INVOKABLE void wifiScan();
    /** @returns false when the strings do not fit the frame; nothing is sent then. */
    Q_INVOKABLE bool wifiConnect(const QString& ssid, const QString& password, bool save);
    /** Leaves the network and switches auto-connect off (the module has no plain "leave"). */
    Q_INVOKABLE void wifiDisconnect();
    Q_INVOKABLE void wifiSetAutoConnect(bool on);
    Q_INVOKABLE void wifiForget(const QString& ssid);
    Q_INVOKABLE void wifiForgetAll();
    /** Re-reads the relay configuration and its statistics (GETTING v5 + v6). */
    Q_INVOKABLE void wifiRelayRefresh();
    /** GETTING v6 only: the counters, cheap enough to poll while the island is on screen. */
    Q_INVOKABLE void wifiRelayStatsRefresh();
    /**
     * SETTING v5. The module confirms from its old address and then answers at `addr`, so the
     * device this was sent to stops being the module: the module reappears as a new device.
     * @returns false when the address text is not a.b.c.d; nothing is sent then.
     */
    Q_INVOKABLE bool wifiSetRelay(int mode, int addr, const QString& ip, int rport, int lport);
    /** ID_WIFI v7. power in dBm (2..21); protocols 0x01/0x03/0x07/0x08/0x0F; bandwidth 20 or 40; powerSave 0..2. */
    /** False without a connection. LR only (0x08) is sent in either role: 0.12 takes it for an access point, 0.11
     *  refuses it there with ERR_PAYLOAD (wifiCommandResult). Without 802.11n the width goes as 20 MHz. Also
     *  starts the check of what is in effect (WifiRadioCheck). */
    Q_INVOKABLE bool wifiSetRadio(double powerDbm, int protocols, int bandwidthMhz, int powerSave);
    /** 40 MHz is possible with these protocols (802.11n in them); b, b/g and LR only run at 20 MHz. wifiSetRadio
     *  sends 20 MHz for them whatever it is given (WifiRadioRules). */
    Q_INVOKABLE bool wifiRadio40Allowed(int protocols) const;
    /** LR only for an access point by the module's firmware: 1 from 0.12, 0 before (refused), -1 not known yet. */
    int wifiLrApSupport();
    /** Top port rate the module's firmware takes: 5000000 from 0.13, 4000000 before (WifiRates::maxFor). */
    int wifiMaxBaud();
    /** Firmware 0.15+: a rate set for this app's port is saved at once (WifiNetRules::rateSavedAtOnce). */
    bool wifiRateSavedAtOnce();
    /** Lit arcs of the module's signal icon (WifiSignal): -1 no link, 0 only the dot, 1..3 arcs; from the v1 report. */
    int wifiSignalLevel();
    /**
     * Sets the rate of the module's port `line` (0 = X1, 1 = X2), 9600..5000000 (the module's own limit: wifiMaxBaud). The port this app is connected
     * through (the module's latest summary says so; before firmware 0.12 a serial link is on line 0) moves
     * together with this app's link, the module has to answer at the new rate, and only then is it saved
     * (WifiPortChange). The other port gets its line record back with only the rate changed (ID_WIFI_NET v3,
     * WifiLineChange; before 0.12 line 1 only). False when nothing was started.
     */
    Q_INVOKABLE bool wifiSetLineBaud(int line, int baud);
    /**
     * Firmware 0.12: changes the network side of line `line` (0 = X1, 1 = X2) through its record (ID_WIFI_NET v3):
     * read first, sent back with only the keys of `edits` changed, read back with v3 and v7 (WifiLineChange). Keys:
     * mode 0..2, dest 0..2, ip "a.b.c.d", rport and lport 0..65535, txPin and rxPin -1..127 (line 1 only). No rate
     * here (wifiSetLineBaud). The outcome arrives as wifiLineChangeFinished(). False when nothing was started.
     */
    Q_INVOKABLE bool wifiSetLineConfig(int line, const QVariantMap& edits);
    bool wifiPortBusy() const { return (wifiPort_ && wifiPort_->active()) || (wifiLine_ && wifiLine_->active()); }
    int wifiPortTarget() const {
        return (wifiPort_ && wifiPort_->active()) ? wifiPort_->target() : ((wifiLine_ && wifiLine_->active()) ? wifiLine_->target() : 0);
    }
    /** The port whose rate is being changed; -1 while none is. */
    int wifiPortLine() const {
        return (wifiPort_ && wifiPort_->active()) ? wifiPortLine_ : ((wifiLine_ && wifiLine_->active()) ? wifiLine_->line() : -1);
    }
    bool wifiRadioChecking() const { return wifiRadioCheck_ && wifiRadioCheck_->active(); }
    /** Re-reads role, access point, address, lines and own address (ID_WIFI_NET v0, v1, v2, v3, v6). */
    Q_INVOKABLE void wifiNetRefresh();
    Q_INVOKABLE void wifiNetRefreshStats();
    Q_INVOKABLE void wifiNetRefreshClients();
    /** Firmware 0.12: both port summaries (v7 page 0) and each port's devices and network side (pages 1, 2). */
    Q_INVOKABLE void wifiNetRefreshPorts();
    /** role 0 station, 1 access point. The module answers, then restarts when reboot is set. */
    Q_INVOKABLE void wifiNetSetRole(int role, bool reboot, bool forgetLines);
    /** auth 0 open, 1 WPA2, 2 WPA2/WPA3. An empty password with security keeps the current one. */
    Q_INVOKABLE bool wifiNetSetAp(int channel, bool hidden, int maxClients, int auth, const QString& ssid,
                                  const QString& password);
    Q_INVOKABLE bool wifiNetSetIp(const QString& ip, const QString& mask, bool dhcp, const QString& poolStart,
                                  const QString& poolEnd, int leaseMin, bool offerGateway, bool offerDns);
    /** mode 0 off, 1 UDP, 2 TCP; dest 0 fixed, 1 senders, 2 broadcast. Line 0's pins are fixed; before 0.12 line 0
     *  ignores baud as well (from 0.12 the rate applies to both lines). */
    Q_INVOKABLE bool wifiNetSetLine(int line, int mode, int dest, const QString& ip, int rport, int lport, int baud,
                                    int txPin, int rxPin);
    /** False without a connection or outside 1..255 (0 is where devices sit; refused by 0.11), 1..254 from 0.12. */
    Q_INVOKABLE bool wifiNetSetOwnAddr(int addr);

    float yaw();
    float pitch();
    float roll();

    void setBusAddress(int addr);
    int getBusAddress();

    void setDevAddress(int addr);
    int getDevAddress();

    void setBaudrate(int baudrate);
    int getBaudrate();

    void setDevDefAddress(int addr);
    int getDevDefAddress();

    QString devName() { return m_devName; }
    int devType() const { return static_cast<int>(idVersion->boardVersion()); }
    int devTypeMinor() const { return static_cast<int>(idVersion->boardVersionMinor()); }
    uint32_t devSerialNumber();
    QString devPN();

    QString fwVersion() { return m_fwVer; }
    QString bootVersion() { return m_bootVer; }
    int bootMode() const { return idVersion->bootMode(); }
    QString devUID() const { return QString::fromLatin1(idVersion->uid().toHex()).toUpper(); }

    // Recorder status (ID_RECORDER_STATUS 0x26 / RecorderStatusV0). See docs
    // Recorder-Host-Integration-Guide.md for field meaning.
    bool recorderStatusValid()           { return idRecorderStatus && idRecorderStatus->isValid(); }
    int  recorderDeviceCondition()       { return idRecorderStatus ? idRecorderStatus->deviceCondition() : 0; }
    int  recorderRecordingMode()         { return idRecorderStatus ? idRecorderStatus->recordingMode() : 0; }
    int  recorderRecordingState()        { return idRecorderStatus ? idRecorderStatus->recordingState() : 0; }
    int  recorderStatusFlags()           { return idRecorderStatus ? idRecorderStatus->statusFlags() : 0; }
    int  recorderWarningFlags()          { return idRecorderStatus ? idRecorderStatus->warningFlags() : 0; }
    int  recorderDegradedFlags()         { return idRecorderStatus ? idRecorderStatus->degradedFlags() : 0; }
    int  recorderCriticalFlags()         { return idRecorderStatus ? idRecorderStatus->criticalFlags() : 0; }
    int  recorderCurrentLogId()          { return idRecorderStatus ? idRecorderStatus->currentLogId() : 0; }
    int  recorderRecordedSize64k()       { return idRecorderStatus ? idRecorderStatus->recordedSize64k() : 0; }
    int  recorderFreeSpace1m()           { return idRecorderStatus ? idRecorderStatus->freeSpace1m() : 0; }
    int  recorderDurationSeconds()       { return idRecorderStatus ? idRecorderStatus->recordingDurationSeconds() : 0; }
    int  recorderSecondsSinceLastWrite() { return idRecorderStatus ? idRecorderStatus->secondsSinceLastWrite() : 0; }

    static inline std::atomic<bool> usblConsoled_{ false };

    bool navSensorStatusValid() { return idNavSensorStatus && idNavSensorStatus->isValid(); }
    bool navSensorStatusStale() { return navSensorStatusStale_; }
    int  navSensorSeq()         { return idNavSensorStatus ? idNavSensorStatus->seq() : 0; }
    int  imuState()             { return idNavSensorStatus ? idNavSensorStatus->imuState() : 0; }
    int  imuWhoAmI()            { return idNavSensorStatus ? idNavSensorStatus->imuWhoAmI() : 0; }
    int  imuFaultFlags()        { return idNavSensorStatus ? idNavSensorStatus->imuFaultFlags() : 0; }
    int  imuSamplesPerS()       { return idNavSensorStatus ? idNavSensorStatus->imuSamplesPerS() : 0; }
    int  imuEkfFedPerS()        { return idNavSensorStatus ? idNavSensorStatus->imuEkfFedPerS() : 0; }
    int  imuBusErrCnt()         { return idNavSensorStatus ? idNavSensorStatus->imuBusErrCnt() : 0; }
    int  imuInvalidCnt()        { return idNavSensorStatus ? idNavSensorStatus->imuInvalidCnt() : 0; }
    int  imuStuckRunMax()       { return idNavSensorStatus ? idNavSensorStatus->imuStuckRunMax() : 0; }
    int  imuAccNormMg()         { return idNavSensorStatus ? idNavSensorStatus->imuAccNormMg() : 0; }
    int  imuGyrNormCdps()       { return idNavSensorStatus ? idNavSensorStatus->imuGyrNormCdps() : 0; }
    int  imuTempCc()            { return idNavSensorStatus ? idNavSensorStatus->imuTempCc() : 0; }
    int  imuClipMask()          { return idNavSensorStatus ? idNavSensorStatus->imuClipMask() : 0; }
    int  imuReinitCnt()         { return idNavSensorStatus ? idNavSensorStatus->imuReinitCnt() : 0; }
    int  imuBadSeconds()        { return idNavSensorStatus ? idNavSensorStatus->imuBadSeconds() : 0; }

    enum UsblApplyState : uint8_t {
        UsblApplyNone      = 0,
        UsblApplyQueued    = 1,
        UsblApplyPending   = 2,
        UsblApplyConfirmed = 3,
        UsblApplyRejected  = 4,
        UsblApplyNoAnswer  = 5,
    };

    bool usblRespKnown() const { return usblMem_.hasTransponder || usblMem_.hasFilter || usblMem_.hasMonitor; }
    bool usblRespTransponderKnown() const { return usblMem_.hasTransponder; }
    bool usblRespEnabled() const { return usblMem_.hasTransponder && usblMem_.transponderTimeoutUs != 0; }
    bool usblRespFilterKnown() const { return usblMem_.hasFilter; }
    QVariantList usblRespAcceptedAddresses() const;
    bool usblRespMonitorKnown() const { return usblMem_.hasMonitor; }
    int  usblRespSuppressResponseUs() const { return (int)usblMem_.suppressSelfResponseUs; }
    int  usblRespSuppressRequestUs() const { return (int)usblMem_.suppressSelfRequestUs; }
    bool usblRespReceiveInIdle() const { return usblMem_.receiveResponseInIdle; }
    int  usblRespApplyState() const;

    BoardVersion boardVersion() {
        return idVersion->boardVersion();
    }

    bool isBoardInited() { return boardVersion() != BoardNone; }

    bool isSonar() {
        BoardVersion ver = boardVersion();
        return ver == BoardBase || ver == BoardNBase || ver == BoardEnhanced || ver == BoardChirp || ver == BoardNEnhanced || ver == BoardSideEnhanced || ver == BoardBasic2D || ver == BoardNanoSSS || ver == BoardPULSEred_2D || ver == BoardPULSEblue_DSS || ver == BoardBaikalSonar;
    }

    bool isRecorder() {
        BoardVersion ver = boardVersion();
        return ver == BoardRecorderMini;
    }

    bool isDoppler() {
        BoardVersion ver = boardVersion();
        return ver == BoardDVL;
    }

    bool isUSBLBeacon() {
        BoardVersion ver = boardVersion();
        return ver == BoardUSBLBeacon;
    }

    bool isUSBL() {
        BoardVersion ver = boardVersion();
        return ver == BoardUSBL;
    }

    bool isWifi() { return boardVersion() == BoardWiFi; }

    bool isChartSupport() { return m_state.duplex && isSonar(); }
    bool isDistSupport() { return m_state.duplex && isSonar(); }
    bool isDSPSupport() { return m_state.duplex && isSonar(); }
    bool isTransducerSupport() { return m_state.duplex && isSonar(); }
    bool isDatasetSupport() { return m_state.duplex && isSonar(); }
    bool isSoundSpeedSupport() { return m_state.duplex && isSonar(); }
    // Only the Baikal-sonar has a network of its own; asking anything else
    // would get an "unknown id" and an empty group in the interface.
    bool isNetSupport() { return m_state.duplex && boardVersion() == BoardBaikalSonar; }
    bool isAddressSupport() { return m_state.duplex; }
    bool isUpgradeSupport() { return m_state.duplex; }

    bool getDatasetState() { return datasetState_; };
    bool getDistSetupState() { return distSetupState_; };
    bool getChartSetupState() { return chartSetupState_; };
    bool getDspSetupState() { return dspSetupState_; };
    bool getTranscState() { return transcState_; };
    bool getSoundSpeedState() { return soundSpeedState_; };
    bool getUartState() { return uartState_; };
    bool getServoControlState() { return servoControlState_; };
    bool getStandState() { return standSupported_; };
    bool getPwmRouteState() { return pwmRouteState_; };
    bool getDevSyncState() { return devSyncState_; };
    int getAverageChartLosses() const { return averageChartLosses_; };
    QUuid getLinkUuid() const;
    void setFirmware(const QByteArray& data);

    /** Nothing from this device for DeviceLiveness::kStaleMs on a live connection: it is shown as without data.
     *  The link's own state cannot say it, another device on the link (the head unit's Wi-Fi module) may keep it
     *  busy. */
    bool dataStale() const { return dataStale_; }

    // Link status mirror — pushed by DeviceManager.
    bool linkConnected()    const { return linkConnected_; }
    bool linkReceivesData() const { return linkReceivesData_; }
    bool linkNotAvailable() const { return linkNotAvailable_; }
    void setLinkStatus(bool connected, bool receivesData, bool notAvailable);

signals:
    void averageChartLossesChanged();
    void binFrameOut(Parsers::ProtoBinOut proto_out);

    // link
    void startUpgradingFirmware();
    void upgradingFirmwareDone();
    // deviceManager
    void startUpgradingFirmwareDM(QUuid linkUuid, uint8_t address, QByteArray firmware);
    /// The host port's rate must not be searched or changed (an update through a recorder).
    void linkRatePinRequested(bool pinned);
    void upgradingFirmwareDoneDM();
    void ringUpgradeRequested(QUuid linkUuid, int address, QByteArray firmware, QString expect, bool everyBoard);
    void busAddressRequested(QUuid linkUuid, int fromAddress, int toAddress);
    void busAddressDone(bool ok, int address, QString detail);
    void busAddressBusyChanged();
    void ringRateRequested(QUuid linkUuid, int fromAddress, int rate);
    void ringRateDone(bool ok, int rate, QString detail);
    void ringRateBusyChanged();

    //
    void sendChartSetup(const ChannelId& channelId, uint16_t resol, uint16_t count, uint16_t offset);
    void sendTranscSetup(const ChannelId& channelId, uint16_t freq, uint8_t pulse, uint8_t boost);
    void sendSoundSpeed(const ChannelId& channelId, uint32_t soundSpeed);

    void chartComplete(const ChannelId& channelId, const ChartParameters& chartsParams, const QVector<QVector<uint8_t>>& data, float resolution, float offset);
    void rawDataRecieved(const ChannelId& channelId, RawData raw_data);

    void iqComplete(QByteArray data, uint8_t type);
    /// `kind` is an AttitudeArbiter::Kind: the device's own IMU (ID_ATTITUDE) or a navigation board (ID_NAV).
    /// `source` tells this device from others of the kind: the object in the high bits, the bus address in
    /// the low byte (for the log).
    void attitudeComplete(float yaw, float pitch, float roll, int kind, quint64 source);
    void tempComplete(float val);
    void voltageComplete(quint8 channel, qint32 value);
    void supplyVoltageComplete(float volts);   // ID_VOLTAGE channel 0, the supply
    /// A slot's frame of the presets in the sonar came (presetSlots, presetsKnown).
    void presetsChanged();
    void ringStatChanged();
    void portsChanged();
    void portValuesChanged();
    void portSettingsChanged();
    void voltagesChanged();
    /// The sonar did not do a recall (version 0), a save (1) or a delete (2): code 2 the flash write did not
    /// verify, 3 a wrong or empty slot, 7 no key (the contract of ID_PRESET). Done is not reported.
    void presetResult(int version, int code);

    void distComplete(const ChannelId& channelId, int dist);
    void encoderComplete(float e1, float e2, float e3);

    void usblSolutionComplete(IDBinUsblSolution::UsblSolution data);
    void usblDiagComplete(IDBinUsblDiag::UsblSignalDiag data);
    void acousticNavSolutionComplete(IDBinUsblSolution::AcousticNavSolution data);
    void baseToBeaconComplete(IDBinUsblSolution::BaseToBeacon data);
    void beaconActivationComplete(uint8_t id);
    void modemSolutionComplete(IDBinModemSolution::ModemSolutionHeader header, QByteArray payload);
    // Argument-free companion so DevQProperty can use it as a Q_PROPERTY NOTIFY
    // (same split as recorderStatusChanged).
    void modemPayloadChanged();

    void positionComplete(double lat, double lon, uint32_t date, uint32_t time);
    void gnssVelocityComplete(double hSpeed, double course);
    void simpleNavV2Complete(uint8_t gnssFixType,
                             uint8_t numSats,
                             uint32_t unixTime,
                             int16_t unixOffsetMs,
                             double latitude,
                             double longitude,
                             double groundCourseDeg,
                             double groundVelocityMps,
                             float yawDeg,
                             float pitchDeg,
                             float rollDeg);
    void boatStatusComplete(uint8_t batteryBoatPercent, uint8_t batteryBridgePercent, uint8_t signalQualityBoatPercent, uint8_t signalQualityBridgePercent);
    void depthComplete(float depth);
    void chartSetupChanged();
    void chartSidesChanged();
    void dspSetupChanged();
    void distSetupChanged();
    void datasetChanged();
    void transChanged();
    void soundChanged();
    void netChanged();
    void wifiStatusChanged();
    void wifiReportChanged();
    void wifiNetworksChanged();
    void wifiSurveyChanged();
    void wifiRfChanged();
    void wifiLinkEventsChanged();
    void wifiLinkRateChanged();
    void wifiSavedChanged();
    void wifiRelayChanged();
    void wifiRelayStatsChanged();
    void wifiRadioChanged();
    void wifiPortChanged();
    void wifiRadioCheckChanged();
    void wifiNetChanged();
    void wifiNetStatsChanged();
    void wifiNetClientsChanged();
    void wifiNetPortsChanged();           // v7 page 0 (and the port support)
    void wifiNetPortDevicesChanged();     // v7 page 1
    void wifiNetPortPeersChanged();       // v7 page 2
    // End of a line-record change (WifiLineChange::Outcome; resp = the module's code on a refusal); rate: it was a
    // port rate, otherwise the line's network settings.
    void wifiLineChangeFinished(int line, int outcome, int resp, bool rate);
    // Answer to an ID_WIFI_NET SETTING (or a refusal): version and response code (Parsers::Resp).
    void wifiNetCommandResult(int version, int resp);
    // The module's answer to a Wi-Fi SETTING (or its refusal of a request): version of the frame
    // and the response code (Parsers::Resp). Only failures matter to the page.
    void wifiCommandResult(int version, int resp);
    void UARTChanged();
    void servoControlChanged();
    void standChanged();
    void pwmRouteChanged();
    void servoCurrentAngleChanged();
    void devSyncChanged();
    void devSyncErrorOccurred(QString reason);
    void linkStatusChanged();
    void dataStaleChanged();
    void upgradeProgressChanged(int progress_status);
    void upgradeChanged();
    void deviceVersionChanged();
    void deviceIDChanged(QByteArray uid);
    void onReboot();
    void recorderStatusChanged();
    void navSensorStatusChanged();
    void usblResponseConfigChanged();
    // One per ID_USBL_CONTROL frame: sent, then acknowledged. code is the RESP code (1 = OK),
    // or 0 when no RESP arrived within the ACK timeout. checksum is CK1 | CK2 << 8 of the frame.
    void usblControlFrameSent(int ver, int checksum);
    void usblControlAck(int ver, int checksum, int code);

    void dopplerVeloComplete();
    void dopplerBeamComplete(IDBinDVL::BeamSolution *beams, uint16_t cnt);
    void dvlSolutionComplete(IDBinDVL::DVLSolution dvlSolution);

public slots:
    void protoComplete(Parsers::FrameParser& proto);
    void startConnection(bool duplex);
    void stopConnection();
    void restartState();

    void requestDist();
    void requestChart();

    void requestStreamList();
    void requestStream(int stream_id);
    void requestStreamRange(int stream_id, quint32 start, quint32 end);
    void requestStreamRanges(int stream_id, QVector<quint32> ranges);
    void requestRecorderStatus();

    void setConsoleOut(bool is_console);

    void flashSettings();
    void resetSettings();
    void reboot();
    void process();

    void dvlChangeMode(bool ismode1, bool ismode2, bool ismode3, bool ismode4, float range_mode4);

    void importSettingsFromXML(const QString& file_path);
    void exportSettingsToXML(const QString& file_path);

    void setDatasetState(bool state);
    void setDistSetupState(bool state);
    void setChartSetupState(bool state);
    void setDspSetupState(bool state);
    void setTranscState(bool state);
    void setSoundSpeedState(bool state);
    void setUartState(bool state);
    void setServoControlState(bool state);

    // The stand's whole command surface. Start carries the configuration because the device
    // takes it no other way; the rest are control-only and read nothing from the map.
    void standStart(const QVariantMap& config);
    void standStop();
    void standPause();
    void standResume();
    void standHome();

    void setPwmRouteState(bool state);
    void setDevSyncState(bool state);
    void setDevSyncPeriodMs(int ms);
    void setDevSyncPortSource(int idx, int src);
    void setLinkUuid(QUuid linkUuid);
    void askBeaconPosition() {
        IDBinUsblSolution::USBLRequestBeacon ask;
        askBeaconPosition(ask);
    }
    void askBeaconPosition(IDBinUsblSolution::USBLRequestBeacon ask);
    void enableBeaconOnce(float timeout);

    void acousticPingRequest(uint8_t address, uint32_t timeout_us = 0xFFFFFFFF);
    void acousticPingRequestEx(uint8_t address, uint32_t timeout_us, uint8_t cmdId, uint32_t replyDistanceMm, const QByteArray& payload = {});
    // QML form: the payload is hex text, payloadBits < 0 sends every byte of it. Times as double
    // because 0xFFFFFFFF does not fit a QML int.
    void acousticPingRequestHex(int address, double timeoutUs, int cmdId, double replyDistanceMm,
                                const QString& hexPayload, int payloadBits);
    bool usblControlReady() const { return m_state.connect; }
    void acousticResponceFilter(uint8_t address);
    void acousticResponceFilterSlots(const QVector<int>& addresses);
    void acousticResponceTimeout(uint32_t timeout_us = 0xFFFFFFFF);

    void setUsblTransponderEnable(bool enabled);
    void setUsblMonitorConfig(uint32_t suppressSelfResponseUs, uint32_t suppressSelfRequestUs, bool receiveResponseInIdle);
    // The only per-slot write there is. v6 USBLCmdConfig carries a receiver_function AND a
    // sender_function, so one frame configures both directions of a command slot.
    //
    // There is no "disable" or "stay silent": current firmware dropped those Function
    // values along with USBLCmdSlotConfig. All-default arguments are the closest thing —
    // the slot still answers, it just handles no payload. Per-device silence is
    // setUsblTransponderEnable(false); per-address filtering is acousticResponceFilterSlots().
    void setUsblCmdConfig(int cmdId, int event,
                          int receiverFunction, int receiveBitLength,
                          int senderFunction, const QString& sendHexPayload,
                          int eventAction = 0,
                          int cmdIdAction = 0, int cmdIdReplacement = 0,
                          int addressAction = 0, int addressReplacement = 0,
                          int sendBitLength = -1);
    QString modemLastPayload() const;
    QVariantMap modemPayloads() const { return modemByAddr_; }

    void doRequestAll();

protected:
    friend class DeviceManager;

    typedef void (DevDriver::* ParseCallback)(Type type, Version ver, Resp resp);

    //FrameParser* m_proto;

    IDBinTimestamp* idTimestamp = nullptr;
    IDBinDist* idDist = nullptr;
    IDBinChart* idChart = nullptr;
    // Decodes compressed pings into idChart, so both paths end in one place.
    IDBinChartZ* idChartZ = nullptr;
    IDBinAttitude* idAtt = nullptr;
    IDBinTemp* idTemp = nullptr;
    IDBinVoltage* idVoltage = nullptr;
    IDBinPreset* idPreset = nullptr;
    IDBinRingStat* idRingStat = nullptr;
    IDBinBus* idBus = nullptr;
    IDBinPin* idPin = nullptr;
    QSet<quint8> loggedVoltageChannels_;   /* report each channel once, not on every ping */
    qint32 lastSupplyLogged_ = 0;
    IDBinEncoder* idEncoder = nullptr;

    IDBinDataset* idDataset = nullptr;
    IDBinDistSetup* idDistSetup = nullptr;
    IDBinChartSetup* idChartSetup = nullptr;
    IDBinDSPSetup* idDSPSetup = nullptr;
    IDBinTransc* idTransc = nullptr;
    IDBinSoundSpeed* idSoundSpeed = nullptr;
    IDBinNet* idNet = nullptr;
    IDBinUART* idUART = nullptr;
    IDBinWifi* idWifi = nullptr;
    IDBinWifiNet* idWifiNet = nullptr;

    IDBinVersion* idVersion = nullptr;
    IDBinMark* idMark = nullptr;
    IDBinFlash* idFlash = nullptr;
    IDBinWifiSurvey* idWifiSurvey = nullptr;
    IDBinWifiRf* idWifiRf = nullptr;
    IDBinBoot* idBoot = nullptr;
    IDBinUpdate* idUpdate = nullptr;

    IDBinNav* idNav = nullptr;
    IDBinBoatStatus* idBoatStatus = nullptr;
    IDBinRecorderStatus* idRecorderStatus = nullptr;
    IDBinNavSensorStatus* idNavSensorStatus = nullptr;
    IDBinDVL* idDVL = nullptr;
    IDBinDVLMode* idDVLMode = nullptr;

    IDBinUsblSolution* idUSBL = nullptr;
    IDBinUsblDiag* idUsblDiag = nullptr;
    IDBinUsblControl* idUSBLControl = nullptr;
    IDBinModemSolution* idModemSolution = nullptr;

    IDBinServoControl* idServoControl = nullptr;
    IDBinStandScan* idStandScan = nullptr;
    IDBinPwmRoute* idPwmRoute = nullptr;
    IDBinDevSync* idDevSync = nullptr;

//    QHash<ID, IDBin*> hashIDParsing;
//    QHash<ID, ParseCallback> hashIDCallback;
//    QHash<ID, IDBin*> hashIDSetup;

    typedef struct ID_Instance {
        ID_Instance() {
            instance = nullptr;
            callback = nullptr;
            isSetup = false;
        }

        ID_Instance(IDBin* inst, ParseCallback call, bool is_setup = false) {
            instance = inst;
            callback = call;
            isSetup = is_setup;
        }
        IDBin* instance = nullptr;
        ParseCallback callback = nullptr;
        bool isSetup = false;
    } ID_Instance;

    QHash<ID, ID_Instance> _hashID;

    typedef enum : quint8 {
        ConfNone = 0,
        ConfRequest,
        ConfRx
    } ConfStatus;

    typedef enum : quint8 {
        UptimeNone,
        UptimeRequest,
        UptimeFix
    } UptimeStatus;

    struct {
        bool duplex = false;

        bool connect = false;
        bool heartbeat = false;
        bool mark = false;
        bool in_boot = false;
        bool reboot = false;
        bool in_update = false;

        ConfStatus conf = ConfNone;
        UptimeStatus uptime = UptimeNone;

        int64_t lastConnectTime = 0;

        void resetState() {
            connect = false;
            heartbeat = false;
            mark = false;
            // in_boot = false;
            reboot = false;
            in_update = false;

            conf = ConfNone;
            uptime = UptimeNone;
        }
    } m_state;

    uint8_t lastAddress_ = 0;
    /// This device as an attitude source (attitudeComplete): the object, then the address in the low byte.
    quint64 attitudeSource() const { return (quint64(reinterpret_cast<quintptr>(this)) << 8) | lastAddress_; }

    QTimer m_processTimer;

    bool m_bootloaderLagacyMode = true;
    bool rebootFlag_ = false;
    int m_upgrade_status = 0;
    QString upgradeExpect_;
    bool busAddressBusy_ = false;
    bool ringRateBusy_ = false;
    bool viaRecorder_ = false;   ///< frames came as KP2: the device sits behind a recorder
    void onIdFrameOut(Parsers::ProtoBinOut out);   ///< a command leaving; wrapped for a ring sonar behind a recorder
    NodeProbe* nodeProbe_ = nullptr;               ///< how a ring sonar behind a recorder takes commands
    NodeProbe::Mode nodeMode_ = NodeProbe::Mode::Unknown;
    bool nodeProbeDone_ = false;
    void startNodeProbeIfDue();
    bool nodeProbeRunning() const;
    bool commandsGoWrapped() const;
    uint8_t nodeAddress() const;                   ///< the address the wrapper names

    enum class RecorderUpgrade { Idle, SlowLine };
    RecorderUpgrade recorderUpgrade_ = RecorderUpgrade::Idle;   ///< SlowLine: waiting for the sonar at 115200
    QByteArray recorderUpgradeImage_;
    int64_t recorderUpgradeAskAt_ = 0, recorderUpgradeUntil_ = 0;
    bool recorderUpgradeAsked_ = false;            ///< the settle time is over: a frame of the sonar now counts
    bool recorderRoute0_ = false;                  ///< the bootloader is talked to on route 0
    static constexpr int kBootloaderRate = 115200;
    static constexpr int64_t kRecorderSettleMs = 500;    ///< the sonar moves, the recorder starts following
    static constexpr int64_t kRecorderAskMs = 700;
    static constexpr int64_t kRecorderFollowMs = 8000;   ///< the recorder's time to follow the line
    static constexpr int kRecorderUnpinMs = 10000;
    void recorderLog(const QString& line);
    void beginRecorderUpgrade(const QByteArray& image);
    void stepRecorderUpgrade(int64_t now);
    void onRecorderUpgradeHeard();
    void failRecorderUpgrade(const QString& reason);
    void startStockUpgrade(const QByteArray& update_data);
    void setIdRoute(uint8_t route);
    void endRecorderRoute();
    int64_t _lastUpgradeAnswerTime = 0;
    int64_t _timeoutUpgradeAnswerTime = 0;
    int64_t upgradeStartedTime_ = 0;
    int64_t rebootAtTime_ = 0;
    int upgradeResendCount_ = 0;
    int64_t lastProcessAtMs_ = 0;

    static constexpr int64_t staleVersionGuardMsec = 400;
    // process() ticks every 200 ms; a longer gap while flashing means the thread was busy elsewhere.
    static constexpr int64_t processStallLogMsec = 600;
    static constexpr int64_t bootHandshakeTimeoutMsec = 8000;
    static constexpr int64_t packetAnswerTimeoutMsec = 2000;
    static constexpr int upgradeResendLimit = 5;
    bool m_isConsole = false;

    int m_busAddress = 0;
    int m_devAddress = 0;
    int m_devDefAddress = 0;

    QString m_devName = "...";
    QString m_fwVer = "";
    QString m_bootVer = "";

    void regID(IDBin* id_bin, ParseCallback method, bool is_setup = false);
    void requestSetup();

    void fwUpgradeProcess();
    bool checkUpgradeTimeouts(int64_t curr_time);
    void abortUpgrade(const QString& reason);
    bool isStaleVersionAfterReboot() const;

    // Tolerant hex text → bytes: accepts separators and an odd digit count, which is what a
    // hand-typed payload field produces. Deliberately outside the slots section — it is a
    // static helper, not something to invoke from a connection.
    static QByteArray parseHexPayload(const QString& text);

protected slots:
    void receivedTimestamp  (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedDist       (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedChart      (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedRaw        (RawData raw_data);
    void receivedAtt        (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedTemp       (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedEncoder    (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedVoltage    (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedPreset     (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedRingStat   (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedBus        (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedPin        (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);

    void receivedDataset    (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedDistSetup  (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedChartSetup (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedDSPSetup   (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedTransc     (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedSoundSpeed (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedNet        (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedUART       (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedWifi       (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedWifiNet    (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedWifiSurvey (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedWifiRf     (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    /** Takes flag bit3 of a status (ID_WIFI v0) against the module's last ID_WIFI_RF state (wifiRadioOff()). */
    void wifiRfOnStatus(bool flagOff);
    void logWifiSurvey();
    void logWifiNet(int ver);
    void logWifiPort(int port, int page);
    /** The rate of this app's own link to the module when it is a serial link; 0 otherwise or unknown. */
    int wifiLinkBaud() const;
    /** The module port (line) this app is connected through: firmware 0.12 says so (v7 flag b2); before it a
     *  serial link is on line 0, as it always was. -1 when not known (the network). */
    int wifiOwnLine() const;
    /** Asks the module on this app's own port (v7 flag b2, so 0.12+) for a link report every
     *  WifiSignal::kFastReportMs, once per connection and module start. */
    void wifiKeepFastReports();

    void receivedVersion    (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedMark       (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedFlash      (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedBoot       (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedUpdate     (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);

    void receivedNav        (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedBoatStatus (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedRecorderStatus(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedNavSensorStatus(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedDVL        (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedDVLMode    (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);

    void receivedUSBL       (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedUsblDiag   (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedUSBLControl(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedModemSolution(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);

    void receivedServoControl(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedStandScan   (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedPwmRoute    (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);

    void receivedDevSync     (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void onDevSyncDebounceFired();

private:
    bool datasetState_;
    bool distSetupState_;
    bool chartSetupState_;
    bool dspSetupState_;
    bool transcState_;
    bool soundSpeedState_;
    bool uartState_;
    bool servoControlState_ = false;
    int servoManualSupport_ = 0;
    QTimer servoManualTimer_;          // paces servoManualMove()
    QTimer servoReadbackTimer_;        // reads the servo control back after a manual change
    double servoManualPending_ = 0.0;  // the drag's latest angle, waiting for servoManualTimer_
    bool servoManualDirty_ = false;
    int servoManualSent_ = 0;          // manual mode's frames sent on this connection (the read-back's log line)
    int servoAnswered_ = 0;            // ID_SERVO_CONTROL settings the device answered with RespOk
    void flushServoManual();
    void stopServoManualDrag();
    // The stand is discovered by probing, not declared: the command family is control-only, so
    // there is no readback to sync and no board version that separates a stand build from a
    // servo one. Both flags reset with the connection — a device may come back reflashed.
    bool standSupported_ = false;
    bool standProbeSent_ = false;
    // The Wi-Fi module is asked once per connection, then it pushes; see process().
    bool wifiProbeSent_ = false;
    // Presets (ID_PRESET) are asked of a nanoSSS once per connection and again every 3 s, five times at most,
    // until a slot's frame comes: one lost request must not hide them for the session. A sonar that answers
    // ERR_ID does not know them and is not asked again.
    bool presetsAnswered_ = false;
    QTimer ringStatTimer_;             // no answer to a ring counters request within it: said so
    bool portsProbeSent_ = false;      // ID_BUS 0 asked on this connection (a Recorder only)
    int portsSupport_ = 0;             // 1 the Recorder described its ports, -1 it has none, 0 not known
    QTimer portsAnswerTimer_;          // the read of the table: no answer within it ends it
    QTimer portsLiveTimer_;            // ID_PIN v1 every 200 ms while a page shows the ports
    int voltage0Mv_ = -1;              // ID_VOLTAGE channel 0 as last read, -1 none yet
    int voltage1Mv_ = -1;              // channel 1
    bool portsLiveWanted_ = false;     // a page shows the ports (setPortsLive): the poll starts once the table is read
    void askNextPort();
    void resetPorts();
    // The UART connectors' settings: one exchange out at a time, portOp_ says which; portOpTimer_ ends it.
    enum PortOp { PortOpNone = 0, PortOpReadFunctions, PortOpSetFunction, PortOpSetBaud, PortOpSave, PortOpReadBuses,
                  PortOpReadBack };
    int portOp_ = PortOpNone;
    int portOpFunction_ = 0;           // the function the exchange is about (a read, a rate, a function moved)
    int portOpBus_ = -1;               // the connector the change was made on
    bool portOpMoves_ = false;         // the change moves a function (else it sets a rate)
    uint32_t portWanted_ = 0;          // the rate the SETTING asked for
    int portResult_ = PortSaved;       // what the read-back found
    QVector<int> portQueue_;           // functions or connectors still to read in this step
    std::array<int, 5> portSupport_ = {};   // by function: 1 answered, -1 refused or silent, 0 not asked
    int portState_ = PortIdle;
    bool linkSerial_ = false;
    int portLiveTick_ = 0;             // the live poll also reads one UART connector a second (auto rates move)
    QTimer portOpTimer_;
    QVector<int> uartBuses() const;
    int busOfFunction(int function) const;
    void readNextFunction();
    void readNextBus();
    bool portAnswer(Parsers::Version ver, Parsers::Resp resp);
    void portSetAnswered(Parsers::Resp resp);
    void portDone(int state);
    QString ringStatError_;
    bool presetsUnsupported_ = false;
    int presetAsks_ = 0;
    int64_t presetAskedAt_ = 0;
    void resetPresets();
    QString lastWifiNetLog_[8];       // last logged line per ID_WIFI_NET version (7 = radio)
    int64_t wifiNetAskedAt_ = 0;      // when ID_WIFI_NET was first asked; silence for 4 s = firmware before 0.10
    int64_t wifiPortsAskedAt_ = 0;    // the last ID_WIFI_NET v7 request while the ports are not known yet
    int wifiPortsAsks_ = 0;           // v7 requests after the first, while the ports are not known
    bool wifiFastReport_ = false;     // the fast link report was asked for (once per connection and module start)
    QString wifiSurveyError_;         // why the last survey did not run or end
    WifiSurveyWindow wifiSurveyWindow_;   // the last surveys, for the mean the pick goes by
    int wifiSurveyTaken_ = 0;         // IDBinWifiSurvey::completedCount() already taken into the window
    bool wifiSurveyAskedAgain_ = false;   // the late survey was asked for once more (GETTING v0)
    QString wifiRfError_;             // why the last switch-off or switch-on was refused
    qint64 wifiRfAtMs_ = 0;           // wifiClock_ time of the module's last ID_WIFI_RF state
    QString wifiFwLogged_;            // the module firmware version last written to the log
    int wifiLastState_ = -1;
    WifiRestartWatch wifiRestart_;     // module uptime reports, to see a restart
    WifiLinkLog wifiLinkLog_;          // the link's recent events, for the page
    QString wifiLinkRateError_;        // why the last link rate report did not come
    int wifiLinkRateTaken_ = 0;        // IDBinWifiSurvey::rateCompletedCount() already logged
    QString wifiLinkRateLogged_;       // the main rates last written to the log
    WifiRetryWindow wifiRetries_;      // the other ends' frames that came again, over the last reports (0.20)
    WifiLossWatch wifiLoss_;           // requests the module could not queue (0.14), to log when they grow
    QElapsedTimer wifiClock_;          // the monotonic time those reports are compared with
    WifiPortChange* wifiPort_ = nullptr;
    int wifiPortLine_ = -1;            // the line wifiPort_ is changing
    WifiLineChange* wifiLine_ = nullptr;
    QString lastWifiPortLog_[WifiPorts::kPorts][3];   // last logged line per port and v7 page
    WifiRadioCheck* wifiRadioCheck_ = nullptr;
    bool pwmRouteState_ = false;
    bool devSyncState_ = false;
    QTimer m_devSyncDebounceTimer;
    int errorFreezeCnt_;
    int averageChartLosses_;
    int chartSides_ = 0;
    QElapsedTimer chartLogClock_;      // logChartHealth: once a minute while chart frames come
    int chartPings_ = 0;               // whole pings since its last line
    void logChartHealth();
    QString lastChartSetupLog_;        // the device's chart setup as last logged
    void logChartSetupSent();
    QUuid linkUuid_;
    bool linkConnected_    = false;
    bool linkReceivesData_ = false;
    bool linkNotAvailable_ = false;
    int64_t lastRecorderStatusReq_ = 0;
    static constexpr int64_t navSensorStatusStaleMsec = 5000;
    int64_t lastNavSensorStatusMs_ = 0;
    bool navSensorStatusStale_ = false;

    struct UsblSlotArgs {
        int cmdId = 0;
        int event = 0;
        int receiverFunction = 0;
        int receiveBitLength = 0;
        int senderFunction = 0;
        QString sendHexPayload;
        int eventAction = 0;
        int cmdIdAction = 0;
        int cmdIdReplacement = 0;
        int addressAction = 0;
        int addressReplacement = 0;
        int sendBitLength = -1;
    };
    struct UsblControlMemory {
        bool hasTransponder = false;
        uint32_t transponderTimeoutUs = 0;
        bool hasFilter = false;
        std::array<uint8_t, 8> filter{};
        bool hasMonitor = false;
        uint32_t suppressSelfResponseUs = 0;
        uint32_t suppressSelfRequestUs = 0;
        bool receiveResponseInIdle = false;
        QMap<int, UsblSlotArgs> cmdSlots;
    };
    static constexpr int64_t usblAckTimeoutMsec = 2000;
    UsblControlMemory usblMem_;
    QVariantMap modemByAddr_;
    std::array<uint8_t, 8> usblAck_{};
    std::array<int64_t, 8> usblAckSentMs_{};
    struct UsblPendingFrame {
        int ver = 0;
        int64_t sentMs = 0;
        int relayed = -1;
    };
    QHash<int, UsblPendingFrame> usblPending_;
    QHash<int, int64_t> usblAnswered_;
    int usblAckEcho_ = -1;
    void trackUsblFrame(int ver, uint16_t checksum);
    void logUsblControl(int ver, const QString& what);

    void sendUsblTransponder();
    void sendUsblFilter();
    void sendUsblMonitor();
    void sendUsblSlot(const UsblSlotArgs& a);
    void markUsblSent(int ver);
    void resendUsblControl();
    bool streamListRequested_ = false;
    // Liveness (dataStale, the rule in DeviceLiveness): a quiet device is asked for its mark (ID_MARK GET).
    bool dataStale_ = false;
    int64_t lastQuietProbe_ = 0;
    DeviceLiveness::Unanswered unanswered_;   // asked less while it is without the mark (process())
    int64_t otherDeviceHeardMs_ = 0;          // another device of this line, last heard (holdSearch), 0: never
    bool searchHeldLogged_ = false;
    void updateDataStale(int64_t now);
};
