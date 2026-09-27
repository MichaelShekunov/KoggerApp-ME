#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVector>
#include <functional>

/**
 * @brief The boat's autopilot (ArduPilot Rover) as seen through MAVLink: telemetry, a waypoint plan,
 * the mission transfer protocol and the handful of commands a head unit needs to run a mission.
 *
 * Transport-agnostic: feed() takes MAVLink bytes (whole frames or any chunks) from whatever link
 * carries them, and every outgoing frame leaves through sendBytes(). In the app DeviceManager hands
 * over the frames the common parser recognises in the boat link's mixed SBP/MAVLink/UBX stream and
 * writes the outgoing bytes back into the same link; tools/mission_check wires the same class to a
 * TCP socket on ArduPilot SITL, which is how the protocol code is tested without a boat.
 *
 * Talks as a ground station: system 255 (ArduPilot's default SYSID_MYGCS), component 190. While
 * controlEnabled is on and an autopilot is heard, a 1 Hz GCS heartbeat goes out; that is what arms
 * the autopilot's GCS failsafe (FS_GCS_ENABLE, FS_GCS_TIMEOUT + FS_TIMEOUT). Behaviour of that
 * failsafe on Rover 4.4, measured on SITL: HeadUnit docs/BOAT_MISSION.md.
 *
 * The plan is a list of points; uploadPlan() turns it into a mission (item 0 = home placeholder,
 * optional speed change, one NAV_WAYPOINT per point, optional return-to-launch), uploads it, reads
 * it back and compares, so planOnVehicle is only ever true for a mission the vehicle really holds.
 *
 * Navigation readiness (navState, navText) comes from ArduPilot's EKF_STATUS_REPORT, judged the way
 * Rover 4.4 judges it once armed: AUTO needs a horizontal position (absolute or relative) outside
 * constant-position mode, and the EKF failsafe fires when the velocity variance, or both the position
 * and compass variances, reach FS_EKF_THRESH (read from the autopilot, 0.8 until then). The report is
 * polled with REQUEST_MESSAGE when the autopilot does not stream it. startMission() refuses while the
 * navigation is not ready, and when a start that armed the boat fails afterwards (AUTO refused, no
 * answer) it disarms again instead of leaving the boat armed in HOLD.
 */
class MavVehicle : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool detected READ detected NOTIFY linkChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY linkChanged)
    Q_PROPERTY(int heartbeatAgeS READ heartbeatAgeS NOTIFY linkChanged)
    Q_PROPERTY(bool controlEnabled READ controlEnabled WRITE setControlEnabled NOTIFY controlEnabledChanged)
    Q_PROPERTY(QString autopilotText READ autopilotText NOTIFY identityChanged)
    Q_PROPERTY(QString firmwareText READ firmwareText NOTIFY identityChanged)

    Q_PROPERTY(bool armed READ armed NOTIFY stateChanged)
    Q_PROPERTY(int mode READ mode NOTIFY stateChanged)
    Q_PROPERTY(QString modeName READ modeName NOTIFY stateChanged)

    Q_PROPERTY(bool positionValid READ positionValid NOTIFY positionChanged)
    Q_PROPERTY(double latitude READ latitude NOTIFY positionChanged)
    Q_PROPERTY(double longitude READ longitude NOTIFY positionChanged)
    Q_PROPERTY(double headingDeg READ headingDeg NOTIFY positionChanged)
    Q_PROPERTY(double groundSpeed READ groundSpeed NOTIFY positionChanged)

    Q_PROPERTY(int gpsFix READ gpsFix NOTIFY gpsChanged)
    Q_PROPERTY(int gpsSats READ gpsSats NOTIFY gpsChanged)
    Q_PROPERTY(double gpsHdop READ gpsHdop NOTIFY gpsChanged)

    Q_PROPERTY(int navState READ navState NOTIFY navChanged)
    Q_PROPERTY(QString navText READ navText NOTIFY navChanged)
    Q_PROPERTY(QString healthText READ healthText NOTIFY healthChanged)
    Q_PROPERTY(int gpsHeadingState READ gpsHeadingState NOTIFY gpsChanged)
    Q_PROPERTY(QString gpsHeadingText READ gpsHeadingText NOTIFY gpsChanged)
    Q_PROPERTY(int headingSource READ headingSource NOTIFY settingsChanged)

    Q_PROPERTY(double batteryVoltage READ batteryVoltage NOTIFY batteryChanged)
    Q_PROPERTY(int batteryRemaining READ batteryRemaining NOTIFY batteryChanged)

    Q_PROPERTY(bool homeValid READ homeValid NOTIFY homeChanged)
    Q_PROPERTY(double homeLatitude READ homeLatitude NOTIFY homeChanged)
    Q_PROPERTY(double homeLongitude READ homeLongitude NOTIFY homeChanged)

    Q_PROPERTY(int missionCurrent READ missionCurrent NOTIFY missionStateChanged)
    Q_PROPERTY(int missionReached READ missionReached NOTIFY missionStateChanged)
    Q_PROPERTY(int missionCount READ missionCount NOTIFY missionStateChanged)
    Q_PROPERTY(double wpDistance READ wpDistance NOTIFY missionStateChanged)
    Q_PROPERTY(int currentPlanIndex READ currentPlanIndex NOTIFY missionStateChanged)

    Q_PROPERTY(QVariantList plan READ plan NOTIFY planChanged)
    Q_PROPERTY(int planCount READ planCount NOTIFY planChanged)
    Q_PROPERTY(double planSpeed READ planSpeed WRITE setPlanSpeed NOTIFY planChanged)
    Q_PROPERTY(bool planReturnHome READ planReturnHome WRITE setPlanReturnHome NOTIFY planChanged)
    Q_PROPERTY(double planLengthM READ planLengthM NOTIFY planChanged)

    Q_PROPERTY(int transferState READ transferState NOTIFY transferChanged)
    Q_PROPERTY(int transferProgress READ transferProgress NOTIFY transferChanged)
    Q_PROPERTY(QString transferText READ transferText NOTIFY transferChanged)
    Q_PROPERTY(bool planOnVehicle READ planOnVehicle NOTIFY transferChanged)

    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString busyText READ busyText NOTIFY busyChanged)
    Q_PROPERTY(QStringList messages READ messages NOTIFY messagesChanged)

public:
    enum TransferState { TransferIdle = 0, TransferUploading, TransferDownloading, TransferDone, TransferFailed };
    Q_ENUM(TransferState)

    /** navState: no fresh EKF report / the boat could navigate in AUTO once armed / it could not. */
    enum NavState { NavUnknown = 0, NavReady, NavNotReady };
    Q_ENUM(NavState)

    /** gpsHeadingState: from the yaw field of GPS_RAW_INT / GPS2_RAW (ArduPilot: 0 not configured,
     *  65535 configured but unavailable, else centidegrees), the best of the two receivers. */
    enum GpsHeadingState { GpsHeadingUnknown = 0, GpsHeadingNotConfigured, GpsHeadingUnavailable, GpsHeadingOk };
    Q_ENUM(GpsHeadingState)

    /** EK3_SRC1_YAW values of ArduPilot 4.4 (AP_NavEKF_Source). */
    enum HeadingSource { HeadingNone = 0, HeadingCompass = 1, HeadingGps = 2, HeadingGpsCompassFallback = 3,
                         HeadingExtNav = 6, HeadingGsf = 8 };
    Q_ENUM(HeadingSource)

    /** ArduPilot Rover custom modes (HEARTBEAT.custom_mode, MAV_CMD_DO_SET_MODE param 2). */
    enum RoverMode { ModeManual = 0, ModeAcro = 1, ModeSteering = 3, ModeHold = 4, ModeLoiter = 5,
                     ModeFollow = 6, ModeSimple = 7, ModeAuto = 10, ModeRtl = 11, ModeSmartRtl = 12,
                     ModeGuided = 15, ModeInitialising = 16 };

    static constexpr int kGcsSystemId = 255;
    static constexpr int kGcsComponentId = 190;

    /** @param persist false for tools: the plan and switches are not read from or written to QSettings. */
    explicit MavVehicle(QObject* parent = nullptr, bool persist = true);
    ~MavVehicle() override;

    bool detected() const { return vehicleSysId_ >= 0; }
    bool connected() const;
    int heartbeatAgeS() const;
    bool controlEnabled() const { return controlEnabled_; }
    void setControlEnabled(bool on);
    QString autopilotText() const { return autopilotText_; }
    QString firmwareText() const { return firmwareText_; }

    bool armed() const { return armed_; }
    int mode() const { return mode_; }
    QString modeName() const { return modeNameOf(mode_); }
    static QString modeNameOf(int mode);

    bool positionValid() const { return positionValid_; }
    double latitude() const { return lat_; }
    double longitude() const { return lon_; }
    double headingDeg() const { return headingDeg_; }
    double groundSpeed() const { return groundSpeed_; }

    int gpsFix() const { return gpsFix_; }
    int gpsSats() const { return gpsSats_; }
    double gpsHdop() const { return gpsHdop_; }

    int navState() const { return navState_; }
    /** "ready" or the reason it is not, ready to show. */
    QString navText() const { return navText_; }
    /** For tools: FS_EKF_THRESH has been read from the autopilot (until then ArduPilot's default 0.8). */
    bool ekfThresholdKnown() const { return ekfThreshKnown_; }
    /** Sensors the autopilot has and uses but calls unhealthy (SYS_STATUS), empty when all are fine. */
    QString healthText() const { return healthText_; }
    int gpsHeadingState() const { return gpsHeadingState_; }
    QString gpsHeadingText() const { return gpsHeadingText_; }
    /** EK3_SRC1_YAW as last read from the autopilot, -1 before it is read. */
    int headingSource() const;
    Q_INVOKABLE static QString headingSourceName(int source);
    /** For tools: the navigation settings (AHRS_EKF_TYPE, EK3_*, COMPASS_*, ARMING_CHECK, FS_EKF_*) went to the log. */
    bool settingsLogged() const { return settingsLogged_; }

    double batteryVoltage() const { return batteryVoltage_; }
    int batteryRemaining() const { return batteryRemaining_; }

    bool homeValid() const { return homeValid_; }
    double homeLatitude() const { return homeLat_; }
    double homeLongitude() const { return homeLon_; }

    int missionCurrent() const { return missionCurrent_; }
    int missionReached() const { return missionReached_; }
    int missionCount() const { return missionCount_; }
    double wpDistance() const { return wpDistance_; }
    /** Index into plan of the point the vehicle heads for (-1 when not on our mission's waypoints). */
    int currentPlanIndex() const;

    QVariantList plan() const;
    int planCount() const { return plan_.size(); }
    double planSpeed() const { return planSpeed_; }
    void setPlanSpeed(double metresPerSecond);
    bool planReturnHome() const { return planReturnHome_; }
    void setPlanReturnHome(bool on);
    double planLengthM() const;

    int transferState() const { return transferState_; }
    int transferProgress() const { return transferProgress_; }
    QString transferText() const { return transferText_; }
    bool planOnVehicle() const { return planOnVehicle_; }

    bool busy() const { return !steps_.isEmpty(); }
    QString busyText() const { return steps_.isEmpty() ? QString() : steps_.first().what; }
    QStringList messages() const { return messages_; }

    Q_INVOKABLE void addPoint(double lat, double lon);
    Q_INVOKABLE void insertPoint(int index, double lat, double lon);
    Q_INVOKABLE void movePoint(int index, double lat, double lon);
    Q_INVOKABLE void removePoint(int index);
    Q_INVOKABLE void clearPlan();

    /** Uploads the plan as a mission, then reads it back and compares (planOnVehicle). */
    Q_INVOKABLE void uploadPlan();
    /** Reads the vehicle's mission back and compares it with the plan. */
    Q_INVOKABLE void readBack();
    /** Reads the vehicle's mission and makes it the plan: waypoints, speed, return home. Other items are
     *  reported and left out. */
    Q_INVOKABLE void loadPlanFromVehicle();

    /** Empty when startMission() may run, otherwise the reason, ready to show. */
    Q_INVOKABLE QString startBlocker() const;
    /** From the first point: set mission item 1 current, arm if needed, switch to AUTO. A start that armed
     *  the boat and then fails disarms it again. */
    Q_INVOKABLE void startMission();
    Q_INVOKABLE void hold();
    Q_INVOKABLE void returnHome();
    Q_INVOKABLE void arm();
    Q_INVOKABLE void disarm();
    /** Writes EK3_SRC1_YAW (HeadingGpsCompassFallback or HeadingCompass) and waits for the autopilot to
     *  report the new value; refused while armed. EKF3 picks up the change without a reboot. */
    Q_INVOKABLE void setHeadingSource(int source);

    /** Great-circle distance in metres. */
    Q_INVOKABLE static double distanceM(double lat1, double lon1, double lat2, double lon2);

public slots:
    /** MAVLink bytes from the link; whole frames or arbitrary chunks. */
    void feed(const QByteArray& bytes);

signals:
    void sendBytes(const QByteArray& bytes);
    /** Outcome of a command or a command sequence (arm, start, hold, ...). */
    void commandResult(const QString& what, bool ok, const QString& detail);
    /** A STATUSTEXT at WARNING or worse, for a notification. */
    void vehicleWarning(const QString& text);

    void linkChanged();
    void controlEnabledChanged();
    void identityChanged();
    void stateChanged();
    void positionChanged();
    void gpsChanged();
    void navChanged();
    void healthChanged();
    void settingsChanged();
    void batteryChanged();
    void homeChanged();
    void missionStateChanged();
    void planChanged();
    void transferChanged();
    void busyChanged();
    void messagesChanged();

private:
    struct Point { double lat; double lon; };
    struct Item {
        uint16_t command = 16;
        uint8_t frame = 3;
        float p1 = 0, p2 = 0, p3 = 0, p4 = 0;
        int32_t x = 0, y = 0;
        float z = 0;
    };
    /** One step of a command sequence: send, repeat until done() or a refusal, give up after tries. */
    struct Step {
        QString what;
        std::function<void()> send;
        std::function<bool()> done;
        uint16_t ackCommand = 0;     // COMMAND_LONG id whose COMMAND_ACK decides, 0 for none
        int tries = 4;
        int timeoutMs = 1500;
    };

    void handleMessage(const void* msg);   // const mavlink_message_t*
    void onSecondTick();
    void onTransferTimeout();
    void onStepTimeout();

    void sendMessage(const void* msg);     // const mavlink_message_t*
    void sendHeartbeat();
    void sendCommandLong(uint16_t command, float p1 = 0, float p2 = 0, float p3 = 0, float p4 = 0,
                         float p5 = 0, float p6 = 0, float p7 = 0);
    void requestMessage(uint32_t messageId);
    void requestParam(const char* id);
    void sendParamSet(const char* id, float value, int type);
    bool importVehicleItems();
    void requestStreams();
    void updateNav();
    void updateGpsHeading();
    void logGpsHeading();
    void logHealth();
    void logSettings();

    QVector<Item> buildItems() const;
    static bool itemsMatch(const QVector<Item>& planned, const QVector<Item>& onVehicle);
    void updatePlanOnVehicle();
    void sendMissionItem(int seq);
    void requestDownloadItem(int seq);
    void startDownload(bool afterUpload);
    void finishTransfer(bool ok, const QString& text);
    void setTransfer(int state, int progress, const QString& text);

    void runSteps(const QList<Step>& steps);
    void startStep();
    void finishSteps(bool ok, const QString& detail);
    void interruptSequence();
    Step stepSetMode(int mode, const QString& what);
    Step stepArm(bool on);

    void addMessage(const QString& text);
    void loadSettings();
    void savePlan();

    bool persist_;
    int vehicleSysId_ = -1;
    int vehicleCompId_ = -1;
    QElapsedTimer lastHeartbeat_;
    bool wasConnected_ = false;
    QElapsedTimer linkLostAt_;           // running while a logged loss has not been followed by a heartbeat
    bool controlEnabled_ = true;
    QString autopilotText_;
    QString firmwareText_;
    QTimer secondTimer_;
    int secondsSinceDetect_ = 0;
    bool streamsRequested_ = false;
    bool positionSeen_ = false;

    bool armed_ = false;
    int mode_ = -1;
    bool positionValid_ = false;
    double lat_ = 0, lon_ = 0, headingDeg_ = 0, groundSpeed_ = 0;
    int gpsFix_ = 0, gpsSats_ = -1;
    double gpsHdop_ = -1;

    enum NavReason { NavReasonNoReport = 0, NavReasonReady, NavReasonNoPosition, NavReasonVariance };
    int ekfFlags_ = -1;              // EKF_STATUS_REPORT.flags, -1 before the first report
    float ekfVelVar_ = 0, ekfPosVar_ = 0, ekfMagVar_ = 0;
    QElapsedTimer ekfSeen_;
    double ekfThresh_ = 0.8;         // FS_EKF_THRESH
    bool ekfThreshKnown_ = false;
    int navState_ = NavUnknown;
    QString navText_;
    int navLoggedReason_ = -1;
    int ekfPollsUnanswered_ = 0;     // REQUEST_MESSAGE polls since the last EKF report
    int navLoggedState_ = -1;
    QElapsedTimer navLoggedAt_;
    QString healthText_;
    bool healthSeen_ = false;
    // Health and GPS-heading changes go to the log once they have held for a moment, with how many flips it
    // took, so a flapping RC link or RTK heading does not push everything else out of the message list.
    QTimer healthLogTimer_;
    QString healthLogged_;
    bool healthLoggedOnce_ = false;
    int healthChanges_ = 0;
    QTimer gpsHeadingLogTimer_;
    int gpsHeadingChanges_ = 0;
    QMap<QString, double> settings_;
    bool settingsLogged_ = false;
    int settingsEpoch_ = 0;          // secondsSinceDetect_ the read windows count from (re-armed on reconnect)
    int settingsCursor_ = 0;
    QMap<QString, int> settingTypes_;    // MAV_PARAM_TYPE from PARAM_VALUE, for PARAM_SET
    bool importAfterDownload_ = false;
    int gpsYaw_[2] = { -1, -1 };     // raw yaw field per receiver, -1 before its first message
    int gps2Fix_ = -1, gps2Sats_ = -1;
    int gpsHeadingState_ = GpsHeadingUnknown;
    QString gpsHeadingText_;
    int gpsHeadingLogged_ = -1;
    double batteryVoltage_ = -1;
    int batteryRemaining_ = -1;
    bool homeValid_ = false;
    double homeLat_ = 0, homeLon_ = 0;
    int missionCurrent_ = -1, missionReached_ = -1, missionCount_ = -1;
    bool missionCurrentFresh_ = false;
    double wpDistance_ = -1;

    QVector<Point> plan_;
    double planSpeed_ = 0;
    bool planReturnHome_ = true;

    int transferState_ = TransferIdle;
    int transferProgress_ = 0;
    QString transferText_;
    bool planOnVehicle_ = false;
    QVector<Item> upItems_;
    QVector<Item> vehicleItems_;     // last complete read-back
    bool vehicleItemsValid_ = false;
    QVector<Item> downItems_;
    int downCount_ = -1;
    int downNext_ = 0;
    bool verifyAfterUpload_ = false;
    bool uploadRequestSeen_ = false;
    int lastRequestedSeq_ = -1;
    int transferRetries_ = 0;
    QTimer transferTimer_;

    QList<Step> steps_;
    QString sequenceWhat_;     // name of the running sequence, for commandResult
    std::function<void()> onSequenceFail_;   // runs once if the running sequence fails (not when interrupted)
    int stepTry_ = 0;
    QTimer stepTimer_;

    QStringList messages_;
};
