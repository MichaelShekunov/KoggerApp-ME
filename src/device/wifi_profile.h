#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVector>
#include <cstdint>
#include <functional>

/**
 * @brief A whole profile of the Wi-Fi module (firmware 0.22, ID_WIFI_PROFILE = 0x5B): every setting the module saves,
 * in one block that the module checks whole and either refuses (nothing changes) or saves and reboots into.
 *
 * Contract: the module project's docs/SBP_WIFI.md 3г (contract 12): the block (format 1, little-endian), the requests,
 * the refusal reasons. The JSON form is that project's own (profiles/<name>.json, described in host/sbpframe.py before
 * PROFILE_FORMAT), so a file made by its tools/profile.py loads here and the other way round. One key is this app's:
 * "secure": true on a station network whose password must be given (an empty password means an open network to the
 * module); the module project's tools ignore it.
 */
struct WifiProfile
{
    static constexpr int kFormat = 1;
    static constexpr int kMaxBytes = 1024;        ///< the module's limit (the longest profile is 977 bytes)
    static constexpr int kMaxNets = 8;
    static constexpr int kChunk = 200;            ///< bytes per piece sent (SETTING v2 takes up to kMaxChunk)
    static constexpr int kMaxChunk = 253;
    static constexpr uint32_t kKey = 0xC96B5D4A;

    /** The module's refusal reasons (CONTENT v3), in the contract's order. */
    enum Reason { ReasonOk = 0, ReasonIncomplete, ReasonCrc, ReasonFormat, ReasonName, ReasonRole, ReasonAddress,
                  ReasonX1Baud, ReasonRadio, ReasonAp, ReasonIp, ReasonLine0, ReasonLine1, ReasonLines, ReasonXlat,
                  ReasonNets, ReasonBusy, kReasons };

    struct Line {
        uint8_t mode = 0;                         ///< 0 off, 1 UDP, 2 TCP
        uint8_t dest = 1;                         ///< 0 fixed, 1 senders, 2 broadcast
        quint32 ip = 0;                           ///< a.b.c.d, a in the top byte
        uint16_t rport = 0, lport = 0;            ///< peer port; own UDP port (0 = the peer port)
        bool operator==(const Line&) const = default;
    };
    struct Net {
        QString ssid;
        QString password;                         ///< empty = an open network
        bool secure = false;                      ///< this app's mark: the password must not be empty
        bool operator==(const Net&) const = default;
    };

    QString name;                                 ///< 1..32 bytes of UTF-8
    uint8_t role = 0;                             ///< 0 station, 1 access point
    uint8_t address = 87;                         ///< own SBP address 1..254
    uint32_t x1Baud = 921600;
    uint8_t powerQ = 80;                          ///< 0.25 dBm
    uint8_t protocols = 0x07;                     ///< 0x01 b, 0x03 b/g, 0x07 b/g/n, 0x08 LR, 0x0F b/g/n+LR
    uint8_t bandwidth = 1;                        ///< 1 = 20 MHz, 2 = 40 MHz
    uint8_t powerSave = 0;                        ///< 0 none, 1 min, 2 max
    uint8_t apChannel = 6, apHidden = 0, apMaxClients = 4;
    uint8_t apAuth = 1;                           ///< 0 open, 1 WPA2, 2 WPA2+WPA3
    QString apSsid, apPassword;
    quint32 ip = 0, mask = 0, poolStart = 0, poolEnd = 0;
    uint8_t dhcp = 1;
    uint16_t leaseMin = 120;
    uint8_t offer = 0;                            ///< bit0 the gateway, bit1 DNS
    Line line0, line1;
    uint32_t x2Baud = 921600;                     ///< line 1's rate
    int8_t txPin = -1, rxPin = -1;                ///< line 1's pins
    uint8_t xlat = 0;                             ///< line 1's input translator: 0 none, 1 GODIO telemetry
    QVector<Net> networks;                        ///< the station's networks, the first tried first

    bool operator==(const WifiProfile&) const = default;

    /** A profile from its JSON form; an empty string when it was read, else what is wrong (the field named). Value
     *  ranges are checked only as far as the block's fields hold them: the module's own rules are its to apply. */
    static QString fromJson(const QJsonObject& o, WifiProfile* out);
    /** The JSON form (names as the module project's files have them). */
    QJsonObject toJson() const;
    /** The block (format 1); empty, with *error set, when a string does not fit its length or the block its limit. */
    QByteArray encode(QString* error = nullptr) const;
    /** The block read back by the module's own order of checks: ReasonOk, or the reason the module gives for what the
     *  layout rules out (ReasonFormat, ReasonName, ReasonNets, ReasonRole, ReasonAddress). */
    static int decode(const QByteArray& blob, WifiProfile* out);
    /** What the module would refuse that the app can see before sending: the access point's password 8..63 bytes
     *  unless it is open; a network's empty (open), 8..63 bytes or 64 hex digits; no network name twice; at most
     *  kMaxNets networks; and this app's own rule: a "secure" network's password not empty. One line per problem
     *  (translated in the context "WifiProfile"). */
    QStringList problems() const;

    /** CRC-32 (ISO-HDLC, as zlib.crc32) of the whole block: the upload's check. */
    static quint32 crc32(const QByteArray& data);
    /** SETTING v1 {KEY, U2 length, U4 CRC-32}: a new upload. */
    static QByteArray beginPayload(const QByteArray& blob);
    /** SETTING v2 {U2 offset, bytes}: the pieces in order, each sent after the previous one's OK. */
    static QVector<QByteArray> chunkPayloads(const QByteArray& blob, int size = kChunk);
    /** SETTING v3 {KEY}: check whole, save, reboot. */
    static QByteArray applyPayload();
    /** The contract's name of a reason ("ok", "incomplete", ... "busy"); "reason N" past them. */
    static QString reasonName(int reason);

    static QString addrText(quint32 a);
    static bool addrValue(const QString& s, quint32* out);
};

/** What the module says of its profile (CONTENT v0 {U1 flags, U4 fingerprint, U2 received, U2 announced, U1 len, name}). */
struct WifiProfileStatus
{
    bool valid = false;
    bool loaded = false;                          ///< bit0: the module was loaded with a profile (the name is its)
    bool modified = false;                        ///< bit1: a setting changed since
    quint32 fingerprint = 0;
    uint16_t received = 0, announced = 0;         ///< an upload in progress; 0 of 0 = none
    QString name;

    /** By minimum length (10 + the name); false for a shorter payload. */
    static bool parse(const uint8_t* data, int size, WifiProfileStatus* out);
};

/** Rules of the profile that the app keeps (the module checks the rest). */
struct WifiProfileRules
{
    /** 1 from firmware 0.22, 0 before, -1 while the version is not known (0.0). */
    static int supportFor(int fwMajor, int fwMinor)
    {
        if (fwMajor <= 0 && fwMinor <= 0)
            return -1;
        return (fwMajor > 0 || fwMinor >= 22) ? 1 : 0;
    }
    /** The fastest rate a USB serial adapter can follow, by its USB ids; 0 when not known. Silicon Labs CP2105
     *  (10C4:EA70) takes 2 Mbaud: a module set faster on that wire is lost to it until its BOOT button resets the
     *  rates (the contract, 3г "После загрузки"). */
    static int adapterMaxBaud(int vendorId, int productId)
    {
        return (vendorId == 0x10C4 && productId == 0xEA70) ? 2000000 : 0;
    }
    /** The rate of the module's port `ownPort` (0 = X1, 1 = X2, -1 = the network) once it runs the profile; 0 for
     *  the network. */
    static int portBaudAfter(const WifiProfile& p, int ownPort)
    {
        return ownPort == 0 ? int(p.x1Baud) : ownPort == 1 ? int(p.x2Baud) : 0;
    }
};

/**
 * @brief Loading a profile into the module as one sequence (the contract's SETTING v1, the pieces v2, v3), which runs
 * in the driver so that it ends whether or not the settings page is open.
 *
 * Each piece goes after the OK of the one before; a piece without an answer is sent again (the module takes a repeat
 * of the same bytes with OK). The apply is never sent twice blindly: an OK lost on the way means the module saves and
 * reboots, and a second apply would come back "incomplete". Without its answer the module's status is asked instead:
 * the profile's name with no change flag = saved; the upload still held whole = the apply was lost, sent once more.
 * The transport is injected through Hooks, so the offline check drives it with made-up answers.
 */
class WifiProfileUpload : public QObject
{
    Q_OBJECT

public:
    struct Hooks {
        std::function<void(int, const QByteArray&)> send;   ///< SETTING of that version with that payload
        std::function<void(int)> ask;                       ///< GETTING of that version (0 status, 3 the last reason)
    };
    struct Timing {
        int answerMs = 1500;    ///< for each answer to a SETTING
        int tries = 3;          ///< sends of a begin or a piece before giving up
        int reasonMs = 1500;    ///< for CONTENT v3 after a refusal, and for the status after an unanswered apply
    };
    enum Outcome {
        Saved = 0,      ///< the module took the profile: it reboots into it
        Refused,        ///< ERR_PAYLOAD at the apply: reason() says why; nothing changed
        Busy,           ///< ERR_RUNTIME with reason 16: a firmware update runs or has not confirmed itself
        WriteFailed,    ///< ERR_RUNTIME without a reason: the flash refused halfway; no reboot (the contract's Inference)
        Rejected,       ///< the begin or a piece refused (resp() = the module's code)
        NoAnswer        ///< no answer (an apply without one: not known whether it was saved)
    };
    enum Phase { Idle, Begin, Pieces, Apply, Reason, Status };

    WifiProfileUpload(Hooks hooks, Timing timing, QObject* parent = nullptr);

    /** False while one runs, or for an empty block or one over WifiProfile::kMaxBytes. */
    bool start(const QByteArray& blob);
    /** The module's confirmation of a SETTING of version `ver` (Parsers::Resp: 1 OK, 3 ERR_PAYLOAD, 8 ERR_RUNTIME). */
    void onAnswer(int ver, int resp);
    /** CONTENT v3 {reason}. */
    void onReason(int reason);
    /** CONTENT v0: taken only while an unanswered apply is being looked into. */
    void onStatus(const WifiProfileStatus& st);
    void cancel();

    bool active() const { return phase_ != Idle; }
    int phase() const { return phase_; }
    /** Bytes the module confirmed, of total(). */
    int sent() const { return sent_; }
    int total() const { return int(blob_.size()); }
    /** Of the last outcome: the module's code (Rejected, Refused, Busy, WriteFailed) and reason (Refused; -1 none). */
    int resp() const { return resp_; }
    int reason() const { return reason_; }

signals:
    void finished(int outcome);
    void progressChanged();

private:
    void sendPiece();
    void onTimeout();
    void finish(int outcome);

    Hooks hooks_;
    Timing timing_;
    Phase phase_ = Idle;
    QByteArray blob_;
    QString name_;
    QVector<QByteArray> pieces_;
    int index_ = 0;
    int tries_ = 0;
    int extraOks_ = 0;          ///< OKs still due for copies of a piece sent again: not the next piece's
    int sent_ = 0;
    int resp_ = 0;
    int reason_ = -1;
    bool runtime_ = false;      ///< the apply's refusal was ERR_RUNTIME
    bool askedReason_ = false;
    bool appliedAgain_ = false;
    int statusAsks_ = 0;
    QTimer timer_;
};

/** One line of what loading a profile changes: key (stable, for the log and the checks), what, from, to; primary = one
 *  of the changes the page lists first (role, the access point's or the station's networks, address, the port rates).
 *  Not "main": Qt on Windows defines main as qMain. */
struct WifiProfileChange
{
    QString key, what, from, to;
    bool primary = false;
};

/**
 * @brief The module's settings as the app last read them, against a profile: what a load would change. Only what the
 * app knows is compared (`known` flags); the passwords never are (the module does not give them back).
 */
struct WifiProfileDiff
{
    enum Known : uint32_t {
        KnownRole = 1 << 0, KnownAddress = 1 << 1, KnownX1 = 1 << 2, KnownX2 = 1 << 3, KnownRadio = 1 << 4,
        KnownAp = 1 << 5, KnownIp = 1 << 6, KnownLine0 = 1 << 7, KnownLine1 = 1 << 8, KnownXlat = 1 << 9,
        KnownNets = 1 << 10
    };
    static QVector<WifiProfileChange> changes(const WifiProfile& now, uint32_t known, const WifiProfile& next);

    // The texts of the lines (translated in the context "WifiProfile").
    static QString roleText(int role);
    static QString apText(const WifiProfile& p);
    static QString netsText(const WifiProfile& p);
    static QString lineText(const WifiProfile::Line& l);
    static QString line1Text(const WifiProfile& p);
    static QString radioText(const WifiProfile& p);
    static QString ipText(const WifiProfile& p);
    static QString baudText(uint32_t baud);
};

/**
 * @brief Where the profiles come from: three built into the app (the module project's presets without their passwords:
 * "Бошка", "Лодка Kogger", "GODIO boat") and the user's own files, JSON in the app's data folder (wifi_profiles/).
 * Passwords live only in the user's files: none is built in.
 */
struct WifiProfileStore
{
    struct Entry {
        QString id;                               ///< "builtin:<key>" or "file:<file name>"
        bool builtin = false;
        QString file;                             ///< the full path of a user's file
        WifiProfile profile;
        QString error;                            ///< a file that could not be read: why (the profile is empty)
    };
    /** The folder of the user's profiles; `override` (non-empty) replaces it for a check. */
    static QString folder();
    static void setFolderForCheck(const QString& path);
    static QVector<Entry> list();
    static bool find(const QString& id, Entry* out);
    /** Writes the profile as a new file named after it; an empty string when written, else why not. */
    static QString save(const WifiProfile& p, QString* idOut = nullptr);
    /** Reads a JSON file and copies it into the folder; an empty string when done, else why not. */
    static QString import(const QString& path, QString* idOut = nullptr);
    /** Removes a user's file; built-in profiles cannot be. */
    static bool remove(const QString& id);
    /** The built-in profiles' JSON (no passwords), for the checks. */
    static QVector<QPair<QString, QByteArray>> builtinJson();
};
