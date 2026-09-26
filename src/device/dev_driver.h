#pragma once

#include <QObject>
#include <QHash>
#include <QVector>
#include <QTimer>
#include <QUuid>
#include <QVariantMap>
#include <QVariantList>
#include <QStringList>
#include "proto_binnary.h"
#include "id_binnary.h"


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

#ifdef SEPARATE_READING
    QTimer* getProcessTimer();
    QList<QTimer*> getChildTimers();
#endif

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

    void sendFactoryFW(QByteArray update_data);

    int transFreq();
    void setTransFreq(int freq);

    int transPulse();
    void setTransPulse(int pulse);

    int transBoost();
    void setTransBoost(int boost);

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
    bool wifiScanning();
    QVariantList wifiNetworks();
    QStringList wifiSavedNetworks();

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
    Q_INVOKABLE void wifiSetRadio(double powerDbm, int protocols, int bandwidthMhz, int powerSave);
    /** Re-reads role, access point, address, lines and own address (ID_WIFI_NET v0, v1, v2, v3, v6). */
    Q_INVOKABLE void wifiNetRefresh();
    Q_INVOKABLE void wifiNetRefreshStats();
    Q_INVOKABLE void wifiNetRefreshClients();
    /** role 0 station, 1 access point. The module answers, then restarts when reboot is set. */
    Q_INVOKABLE void wifiNetSetRole(int role, bool reboot, bool forgetLines);
    /** auth 0 open, 1 WPA2, 2 WPA2/WPA3. An empty password with security keeps the current one. */
    Q_INVOKABLE bool wifiNetSetAp(int channel, bool hidden, int maxClients, int auth, const QString& ssid,
                                  const QString& password);
    Q_INVOKABLE bool wifiNetSetIp(const QString& ip, const QString& mask, bool dhcp, const QString& poolStart,
                                  const QString& poolEnd, int leaseMin, bool offerGateway, bool offerDns);
    /** mode 0 off, 1 UDP, 2 TCP; dest 0 fixed, 1 senders, 2 broadcast. Line 0 ignores baud and pins. */
    Q_INVOKABLE bool wifiNetSetLine(int line, int mode, int dest, const QString& ip, int rport, int lport, int baud,
                                    int txPin, int rxPin);
    Q_INVOKABLE void wifiNetSetOwnAddr(int addr);

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
    void upgradingFirmwareDoneDM();

    //
    void sendChartSetup(const ChannelId& channelId, uint16_t resol, uint16_t count, uint16_t offset);
    void sendTranscSetup(const ChannelId& channelId, uint16_t freq, uint8_t pulse, uint8_t boost);
    void sendSoundSpeed(const ChannelId& channelId, uint32_t soundSpeed);

    void chartComplete(const ChannelId& channelId, const ChartParameters& chartsParams, const QVector<QVector<uint8_t>>& data, float resolution, float offset);
    void rawDataRecieved(const ChannelId& channelId, RawData raw_data);

    void iqComplete(QByteArray data, uint8_t type);
    void attitudeComplete(float yaw, float pitch, float roll);
    void tempComplete(float val);
    void distComplete(const ChannelId& channelId, int dist);
    void encoderComplete(float e1, float e2, float e3);

    void usblSolutionComplete(IDBinUsblSolution::UsblSolution data);
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
    void dspSetupChanged();
    void distSetupChanged();
    void datasetChanged();
    void transChanged();
    void soundChanged();
    void netChanged();
    void wifiStatusChanged();
    void wifiReportChanged();
    void wifiNetworksChanged();
    void wifiSavedChanged();
    void wifiRelayChanged();
    void wifiRelayStatsChanged();
    void wifiRadioChanged();
    void wifiNetChanged();
    void wifiNetStatsChanged();
    void wifiNetClientsChanged();
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
    void upgradeProgressChanged(int progress_status);
    void upgradeChanged();
    void deviceVersionChanged();
    void deviceIDChanged(QByteArray uid);
    void onReboot();
    void recorderStatusChanged();

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
                          int addressAction = 0, int addressReplacement = 0);
    QString modemLastPayload() const;

#ifdef SEPARATE_READING
    Q_INVOKABLE void initProcessTimerConnects();
    Q_INVOKABLE void initChildsTimersConnects();
#endif

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
    IDBinBoot* idBoot = nullptr;
    IDBinUpdate* idUpdate = nullptr;

    IDBinNav* idNav = nullptr;
    IDBinBoatStatus* idBoatStatus = nullptr;
    IDBinRecorderStatus* idRecorderStatus = nullptr;
    IDBinDVL* idDVL = nullptr;
    IDBinDVLMode* idDVLMode = nullptr;

    IDBinUsblSolution* idUSBL = nullptr;
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

    QTimer m_processTimer;

    bool m_bootloaderLagacyMode = true;
    bool rebootFlag_ = false;
    int m_upgrade_status = 0;
    int64_t _lastUpgradeAnswerTime = 0;
    int64_t _timeoutUpgradeAnswerTime = 0;
    int64_t upgradeStartedTime_ = 0;
    int64_t rebootAtTime_ = 0;
    int upgradeResendCount_ = 0;

    static constexpr int64_t staleVersionGuardMsec = 400;
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
    void logWifiNet(int ver);

    void receivedVersion    (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedMark       (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedFlash      (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedBoot       (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedUpdate     (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);

    void receivedNav        (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedBoatStatus (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedRecorderStatus(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedDVL        (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
    void receivedDVLMode    (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);

    void receivedUSBL       (Parsers::Type type, Parsers::Version ver, Parsers::Resp resp);
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
    // The stand is discovered by probing, not declared: the command family is control-only, so
    // there is no readback to sync and no board version that separates a stand build from a
    // servo one. Both flags reset with the connection — a device may come back reflashed.
    bool standSupported_ = false;
    bool standProbeSent_ = false;
    // The Wi-Fi module is asked once per connection, then it pushes; see process().
    bool wifiProbeSent_ = false;
    QString lastWifiNetLog_[8];       // last logged line per ID_WIFI_NET version (7 = radio)
    int64_t wifiNetAskedAt_ = 0;      // when ID_WIFI_NET was first asked; silence for 4 s = firmware before 0.10
    int wifiLastState_ = -1;
    bool pwmRouteState_ = false;
    bool devSyncState_ = false;
    QTimer m_devSyncDebounceTimer;
    int errorFreezeCnt_;
    int averageChartLosses_;
    QUuid linkUuid_;
    bool linkConnected_    = false;
    bool linkReceivesData_ = false;
    bool linkNotAvailable_ = false;
    int64_t lastRecorderStatusReq_ = 0;
    bool streamListRequested_ = false;
};
