#ifndef IDBINNARY_H
#define IDBINNARY_H

#include <array>

#include <QObject>
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QVector>
#include <QStringList>
#include <QTimer>
#include "dataset_defs.h"
#include "proto_binnary.h"
#include "device/wifi_ops.h"
#include "codec/chart_v3.h"


using namespace Parsers;
using Segment = QPair<uint16_t, uint16_t>; // first - begin, second - end

enum BoardVersion : int16_t {
    BoardNone = -1,
    BoardEnhanced = 1,
    BoardBase = 3,
    BoardNBase = 4,
    BoardChirp = 5,
    BoardAssist = 6,
    BoardNEnhanced = 7,
    BoardSideEnhanced = 8,
    BoardRecorderMini = 9,
    BoardDVL = 10,
    BoardBasic2D = 12,
    BoardUSBL = 15,
    BoardUSBLBeacon = 16,
    BoardNanoSSS = 17,
    BoardWiFi = 87,           // Wi-Fi module (ESP32-C3), docs/WIFI_MODULE.md
    BoardBaikalSonar = 100,
    BoardPULSEred_2D = 128,
    BoardPULSEblue_DSS = 129
};

QString boardVersionName(BoardVersion version, uint8_t versionMinor);

struct LastReadInfo {
    LastReadInfo() : version(), checkSum(0), address(0), isReaded(true) {};
    LastReadInfo(Version _version, uint16_t _checkSum, uint8_t _address, bool _isReaded) :
        version(_version), checkSum(_checkSum), address(_address), isReaded(_isReaded) {};

    Version  version;
    uint16_t checkSum;
    uint8_t  address;
    bool     isReaded;
};

using ListLinkedChannels = QList<QPair<ChannelId, uint8_t>>;

struct ChartParameters {
    ChartParameters()
        : boardVersion(BoardNone),
          version(v0)
    {};

    ChartParameters(BoardVersion _boardVersion, Version _version, QList<Segment> _errList)
        : boardVersion(_boardVersion),
          version(_version),
          errList(_errList)
    {};

    BoardVersion boardVersion;
    Version version;
    QList<Segment> errList;
};

class IDBin : public QObject
{
    Q_OBJECT
public:
    explicit IDBin(QObject *parent = nullptr);
    ~IDBin() override;

    Resp  parse(FrameParser &proto);

    virtual ID id() = 0;
    virtual bool isSettable() { return false; }
    virtual bool isSettup() { return false; }
    virtual bool isRequestable() { return true; }

    Type lastType() { return m_lastType; }
    Version lastVersion() { return m_lastVersion; }
    Resp lastResp() { return m_lastResp; }

    virtual void simpleRequest(Version ver);
    virtual void requestAll() { simpleRequest(v0); }
    virtual void startColdStartTimer() {};

    void setAddress(uint8_t addr) { m_address = addr; }
    void setConsoleOut(bool is_console) { isConsoleOut = is_console; }

signals:
    void updateContent(Parsers::Type type, Parsers::Version ver, Parsers::Resp resp, uint8_t address);
    void dataSend(QByteArray data);
    void binFrameOut(Parsers::ProtoBinOut proto_out);
    void notifyDevDriver(bool state);

protected:
    const U4 m_key = 0xC96B5D4A;

    Type m_lastType;
    Version m_lastVersion;
    Resp m_lastResp;
    uint8_t _lastAddress = 0;
    QList<Version> availableVer;
    uint8_t m_address = 0;
    bool isConsoleOut = false;

    virtual Resp  parsePayload(FrameParser &proto) = 0;
    virtual void requestSpecific(ProtoBinOut &proto_out) { Q_UNUSED(proto_out) }

    bool checkKeyConfirm(U4 key) { return (key == m_key); }
    void appendKey(ProtoBinOut &proto_out);

    void hashBinFrameOut(ProtoBinOut &proto);
    void interExecColdStartTimer();

private:
    /*methods*/
    bool checkResponse(FrameParser& proto);
    void onExpiredColdStartTimer();
    void onExpiredSetTimer();
    /*data*/
    static const uint8_t repeatingCount_ = 7;
    static const int timerPeriodMsec_ = 1500;
    QTimer setTimer_;
    QTimer coldStartTimer_;
    LastReadInfo hashLastInfo_;
    uint8_t setTimerCount_;
    uint8_t coldStartTimerCount_;
    bool isColdStart_;
    bool needToCheckSetResp_;
};



class IDBinTimestamp : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinTimestamp() : IDBin() {
    }

    ID id() override { return ID_TIMESTAMP; }
    Resp  parsePayload(FrameParser &proto) override;

    uint32_t timestamp() { return m_timestamp; }
protected:
    uint32_t m_timestamp;
};



class IDBinDist : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinDist() : IDBin() {
    }

    ID id() override { return ID_DIST; }
    Resp  parsePayload(FrameParser &proto) override;

    uint32_t dist_mm() { return m_dist_mm; }
protected:
    uint32_t m_dist_mm;
};



struct RawData {
    struct RawDataHeader {
        struct  __attribute__((packed)) {
            uint16_t dataType : 5; //
            uint16_t dataSize : 6; // +1 bytes
            uint16_t dataTrigger : 2;
            uint16_t channelGroup : 3;
        };
        uint8_t channelCount = 0;
        uint32_t globalOffset = 0;
        uint32_t localOffset = 0;
        float sampleRate = 0;
    } __attribute__((packed));

    RawDataHeader header;
    QByteArray data;

    uint32_t samplesPerChannel() {
        return data.size()/(header.dataSize + 1)/header.channelCount;
    }
};


class IDBinChart : public IDBin
{
    Q_OBJECT
public:
    IDBinChart() :
        IDBin(),
        lossIndex_(0)
    {
        lossHistory_.resize(1000);
        std::fill(lossHistory_.begin(), lossHistory_.end(), 0);
    }

    ID id() override { return ID_CHART; }
    Resp  parsePayload(FrameParser &proto) override;

    uint16_t sampleResol() const { return m_sampleResol; }
    uint16_t absOffset() const { return m_absOffset; }
    int chartSize() const { return m_chartSize; }

    bool readAndClearIsCompleteChart() {
        bool flag_val = m_isCompleteChart;
        m_isCompleteChart = false;
        return flag_val;
    }

    U2 resolution() {return m_sampleResol; }
    U2 offsetRange() {return m_absOffset*m_sampleResol; }

    uint8_t* logData8() { return m_completeChart; }
    uint8_t* logData28() { return m_completeChart2; }

    /**
     * Take an echogram that arrived compressed and publish it as if it had been
     * reassembled from ID_CHART frames.
     *
     * Everything downstream — rendering, the dataset, the bottom track — reads
     * this object, so decoding into it keeps one path instead of two.
     *
     * @param data   decoded samples, one byte per cell
     * @param n      number of samples
     * @param resol  cell size, mm
     * @param absOffset  index of the first cell
     * @param channels   1, or 2 when the samples are both sides end to end: first half port,
     *                   second half starboard. That is how a two-channel sonar compresses a
     *                   ping — interleaving the sides would be coded along the wrong axis — and
     *                   it lands in the same two buffers an ID_CHART v1 fills.
     */
    void acceptDecoded(const uint8_t* data, int n, U2 resol, U2 absOffset, int channels = 1) {
        if (n < 0 || channels < 1 || channels > 2) {
            return;
        }
        int per = n / channels;
        if (per > static_cast<int>(sizeof(m_completeChart))) {
            per = static_cast<int>(sizeof(m_completeChart));
        }
        memcpy(m_completeChart, data, static_cast<size_t>(per));
        if (channels == 2) {
            memcpy(m_completeChart2, data + n / 2, static_cast<size_t>(per));
        }
        m_chartSize = static_cast<uint32_t>(per);
        m_sampleResol = resol;
        m_absOffset = absOffset;
        // A compressed ping is all-or-nothing, so there is never a partial
        // range to report as lost.
        compErrList_.clear();
        m_isCompleteChart = true;
    }

    /** ID_CHART V3 pings published, and their blocks that had to come from the previous ping, since the last call. */
    uint32_t takeV3Pings() { const uint32_t v = v3Pings_; v3Pings_ = 0; return v; }
    uint32_t takeV3Filled() { const uint32_t v = v3Filled_; v3Filled_ = 0; return v; }

    QList<Segment> getErrList() {
        if (!compErrList_.empty()) {
            auto retVal = std::move(compErrList_);
            compErrList_.clear();
            return retVal;
        }
        return {};
    }

    // uint8_t* rawData() { return _rawDataSave; }
    // uint32_t rawDataSize() { return _rawDataSize; }
    // uint8_t rawType() { return type; }

    uint8_t getAverageLosses() const {
        int sum = 0;
        for (uint8_t loss : lossHistory_) {
            sum += loss;
        }
        return (sum * 1.0f) / (lossHistory_.size() * 1.0f) * 100.0f;
    }

protected:
    uint32_t m_seqOffset = 0, m_sampleResol = 0, m_absOffset = 0;
    uint32_t sampleResolLast_ = 0, absOffsetLast_ = 0;
    uint32_t m_chartSizeIncr = 0;
    uint32_t m_chartSize = 0;
    uint8_t m_fillChart[20000];
    uint8_t m_completeChart[20000];
    uint8_t m_completeChart2[20000];

    QList<Segment> tempErrList_;
    QList<Segment> compErrList_;

    bool m_isCompleteChart = false;

    QMap<int, RawData> _rawData;
    QMap<int, RawData> _rawDataComplete;
signals:
    void rawDataRecieved(RawData raw_data);

private:
    QVector<uint8_t> lossHistory_;
    int lossIndex_ = 0;
    ChartV3Assembler v3_;   // ID_CHART V3: echocodec blocks into whole pings
    uint32_t v3Pings_ = 0, v3Filled_ = 0;
};



/**
 * Compressed echogram (ID_CHART_Z), as sent by Baikal-sonar.
 *
 * The device splits one coded ping across frames; each carries the offset of
 * its slice plus the length of the whole stream, so the stream is reassembled
 * without relying on frame order. Once it is whole it is decoded and handed to
 * IDBinChart, which publishes it exactly like an uncompressed ping — nothing
 * downstream needs to know the difference.
 *
 * A lost slice drops the entire ping. That is not laziness: the stream is
 * arithmetic-coded, so a hole destroys the decoder state for everything after
 * it. An uncompressed echogram can be drawn with a gap; this one cannot.
 */
class IDBinChartZ : public IDBin
{
    Q_OBJECT
public:
    /** @param sink where decoded pings are published; must outlive this object. */
    explicit IDBinChartZ(IDBinChart* sink) : IDBin(), sink_(sink) {}

    ID id() override { return ID_CHART_Z; }
    Resp parsePayload(FrameParser &proto) override;

    /** Pings dropped because a slice went missing, since the last call. */
    uint32_t takeDropped() {
        uint32_t v = dropped_;
        dropped_ = 0;
        return v;
    }

    /** Whole pings the decoder refused (a stream it does not know), since the last call. */
    uint32_t takeUndecoded() {
        uint32_t v = undecoded_;
        undecoded_ = 0;
        return v;
    }

private:
    IDBinChart* sink_ = nullptr;
    int channels_ = 1;            ///< 1, or 2 when the stream carries both sides

    QByteArray stream_;          // coded bytes of the ping being reassembled
    QByteArray decoded_;         // one byte per cell, after decoding
    int expected_ = 0;           // coded length the device announced, 0 = idle
    int filled_ = 0;             // contiguous bytes received so far
    bool holed_ = false;         // a slice was missed: the ping is unusable
    uint32_t dropped_ = 0;
    uint32_t undecoded_ = 0;

    U2 resol_ = 0;
    U2 absOffset_ = 0;
    U2 samples_ = 0;
};

class IDBinAttitude : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinAttitude()
        : IDBin(),
        m_yaw(0.0f),
        m_pitch(0.0f),
        m_roll(0.0f),
        m_w0(0.0f),
        m_w1(0.0f),
        m_w2(0.0f),
        m_w3(0.0f)
    {
    }

    ID id() override { return ID_ATTITUDE; }
    Resp  parsePayload(FrameParser &proto) override;

    float yaw(Version src_ver = v0);
    float pitch(Version src_ver = v0);
    float roll(Version src_ver = v0);
    float w0(Version src_ver = v1);
    float w1(Version src_ver = v1);
    float w2(Version src_ver = v1);
    float w3(Version src_ver = v1);
protected:
    float m_yaw, m_pitch, m_roll;
    float m_w0, m_w1, m_w2, m_w3;
};



class IDBinTemp : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinTemp() : IDBin() {
    }

    ID id() override { return ID_TEMP; }
    Resp  parsePayload(FrameParser &proto) override;

    float temp() { return m_temp; }
protected:
    float m_temp;
};

class IDBinEncoder : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinEncoder() : IDBin() {
    }

    struct Encoder {
        float e1 = NAN, e2 = NAN, e3 = NAN;
    };

    ID id() override { return Parsers::ID_ENCODER; }
    Resp  parsePayload(FrameParser &proto) override;

    Encoder get() { return data; }
protected:
    Encoder data;
};

/**
 * Network settings of the Baikal-sonar (ID_NET).
 *
 * The sonar's second core owns a CDC-NCM adapter; these are its addresses.
 * The sonar stores them in its flash and publishes them to that core, which
 * applies them while running, so a change takes effect without a reboot.
 *
 * Only the three addresses and the DHCP switch travel here. There is no port
 * to set: the device streams to whatever port the host asked from, and the
 * discovery port is fixed in the firmware on purpose, because it is the way
 * back when an address turns out to be wrong. The same frame sent to that
 * port over UDP, even broadcast, is answered broadcast by the device and can
 * set the addresses temporarily; that path lives outside this class, which
 * speaks over the ordinary device link.
 *
 * Layout of the 20-byte payload is the firmware contract (tap_mbox.h):
 * u8 version, u8 flags, u8 mac[6], then device address, mask and host
 * address, four octets each in reading order.
 */
class IDBinNet : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinNet() : IDBin() {}

    ID id() override { return ID_NET; }
    Resp parsePayload(FrameParser &proto) override;

    enum Flags : uint8_t {
        FlagDhcp = 1 << 0,      // the device hands the host an address
        FlagLive = 1 << 7,      // running on settings that were never saved
    };

    /** "192.168.7.1", empty until the device has answered. */
    QString ip() const { return m_valid ? addrText(m_ip) : QString(); }
    QString mask() const { return m_valid ? addrText(m_mask) : QString(); }
    QString host() const { return m_valid ? addrText(m_host) : QString(); }
    QString mac() const { return m_mac; }
    bool dhcp() const { return (m_flags & FlagDhcp) != 0; }
    bool live() const { return (m_flags & FlagLive) != 0; }
    bool valid() const { return m_valid; }

    /**
     * Sends the three addresses to the device.
     * @returns false if any of them is not four octets; nothing is sent then.
     * The device checks them again and refuses a pair that cannot work, so a
     * typo cannot leave it unreachable.
     */
    bool setNet(const QString& ip, const QString& mask, const QString& host, bool dhcp);

protected:
    static QString addrText(quint32 a) {
        return QStringLiteral("%1.%2.%3.%4").arg((a >> 24) & 0xFF).arg((a >> 16) & 0xFF)
                                            .arg((a >> 8) & 0xFF).arg(a & 0xFF);
    }
    static bool addrValue(const QString& s, quint32* out);

    quint32 m_ip = 0, m_mask = 0, m_host = 0;
    uint8_t m_flags = 0;
    QString m_mac;
    bool m_valid = false;
};

class IDBinDataset : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinDataset() : IDBin() {
    }

    ID id() override { return ID_DATASET; }
    Resp  parsePayload(FrameParser &proto) override;
    void startColdStartTimer() override;
    struct Channel
    {
        uint8_t id = 0;
        uint32_t period = 0;
        uint32_t mask = 0;
    };

    enum ChannelMask : uint8_t {
        MASK_DIST_V0 = 1,
        MASK_CHART_V0 = 2,
        MASK_ATTITUDE_V0 = 4,
        MASK_ATTITUDE_V1 = 8,
        MASK_TEMP_V0 = 16,
        MASK_TIMESTAMP_V0 = 32,
        MASK_DIST_SDDBT = 64,
        MASK_DIST_SDDBT_P2 = 128,
    };

    void setChannel(uint8_t ch_id, uint32_t period, uint32_t mask);
    uint32_t mask(U1 ch_id);
    void setMask(U1 ch_id, uint32_t mask);


    void setFlag(U1 ch_id, uint32_t flag) {
        if(ch_id != 0) {
            setMask(ch_id, mask(ch_id) | ((U4)flag));

            for(int i = 1; i < 3; i++) {
                if(ch_id != i) {
                    resetFlag(i, flag);
                }
            }
        } else {
            for(int i = 1; i < 3; i++) {
                resetFlag(i, flag);
            }
        }
    }

    void resetFlag(U1 ch_id, uint32_t flag) { setMask(ch_id, mask(ch_id) & (~((U4)flag))); }
    void moveFlag(U1 ch_id, uint32_t flag) {
        if(ch_id != 0) {
            uint32_t msk = mask(0)&flag;
            setFlag(ch_id, msk);
        }
    }
    bool flag(U1 ch_id, uint32_t flag) { return (mask(ch_id) & ((U4)flag)) != 0 ;}

    void setDist_v0(U1 ch_id) {
        setFlag(ch_id, MASK_DIST_V0);
        moveFlag(ch_id,    MASK_DIST_V0 | MASK_CHART_V0 | MASK_DIST_SDDBT | MASK_DIST_SDDBT_P2);
        setFlag(0, MASK_DIST_SDDBT | MASK_DIST_SDDBT_P2);
    }
    void resetDist_v0(U1 ch_id) { resetFlag(ch_id, MASK_DIST_V0); }
    bool getDist_v0(U1 ch_id) { return flag(ch_id, MASK_DIST_V0); }

    void setChart_v0(U1 ch_id) {
        setFlag(ch_id, MASK_CHART_V0);
        moveFlag(ch_id,  MASK_DIST_V0 | MASK_CHART_V0 | MASK_DIST_SDDBT | MASK_DIST_SDDBT_P2);
    }
    void resetChart_v0(U1 ch_id) { resetFlag(ch_id, MASK_CHART_V0); }
    bool getChart_v0(U1 ch_id) { return flag(ch_id, MASK_CHART_V0); }

    /**
     * Compressed echogram, sent as ID_CHART_Z by Baikal-sonar.
     *
     * Bit 21, not one of the low eight: the published protocol owns the low
     * byte (attitude, temperature, timestamp, SDDBT), and a private mode
     * sitting there would be switched on by an unrelated checkbox.
     *
     * Same channel semantics as the uncompressed chart, so the two are set the
     * same way and can be told apart only by which one the device sends.
     */
    static constexpr uint32_t MASK_CHART_Z_V0 = 1u << 21;

    void setChartZ_v0(U1 ch_id) { setFlag(ch_id, MASK_CHART_Z_V0); }
    void resetChartZ_v0(U1 ch_id) { resetFlag(ch_id, MASK_CHART_Z_V0); }
    bool getChartZ_v0(U1 ch_id) { return flag(ch_id, MASK_CHART_Z_V0); }

    /**
     * USB tap of the Baikal-sonar. IQ (bit 22) publishes the complex baseband
     * of the two-pass receiver after every ping, RAW (bit 23) the raw ADC
     * samples; both leave this link and arrive over the second core's own
     * CDC port (see UsbTap). Like the compressed echogram they live above the
     * low byte the published protocol owns.
     */
    static constexpr uint32_t MASK_TAP_IQ_V0  = 1u << 22;
    static constexpr uint32_t MASK_TAP_RAW_V0 = 1u << 23;

    void setTapIq_v0(U1 ch_id) { setFlag(ch_id, MASK_TAP_IQ_V0); }
    void resetTapIq_v0(U1 ch_id) { resetFlag(ch_id, MASK_TAP_IQ_V0); }
    bool getTapIq_v0(U1 ch_id) { return flag(ch_id, MASK_TAP_IQ_V0); }

    void setTapRaw_v0(U1 ch_id) { setFlag(ch_id, MASK_TAP_RAW_V0); }
    void resetTapRaw_v0(U1 ch_id) { resetFlag(ch_id, MASK_TAP_RAW_V0); }
    bool getTapRaw_v0(U1 ch_id) { return flag(ch_id, MASK_TAP_RAW_V0); }

    /**
     * Supply voltage, sent as ID_VOLTAGE v0 every ping by the nanoSSS firmware of this fork
     * (l412_adc 11.8 and later): channel 0 is the supply in millivolts. The same bit sends that
     * firmware's diagnostic channels with it, 33 frames a ping, about 7 KB/s. Like the compressed
     * echogram and the taps it lives above the low byte the published protocol owns; what other
     * firmware does with it is not known.
     */
    static constexpr uint32_t MASK_VOLTAGE_V0 = 1u << 24;

    void setVoltage_v0(U1 ch_id) { setFlag(ch_id, MASK_VOLTAGE_V0); }
    void resetVoltage_v0(U1 ch_id) { resetFlag(ch_id, MASK_VOLTAGE_V0); }
    bool getVoltage_v0(U1 ch_id) { return flag(ch_id, MASK_VOLTAGE_V0); }

    void setAttitude_v0(U1 ch_id) { setFlag(ch_id, MASK_ATTITUDE_V0); }
    void resetAttitude_v0(U1 ch_id) { resetFlag(ch_id, MASK_ATTITUDE_V0); }
    bool getAttitude_v0(U1 ch_id) { return flag(ch_id, MASK_ATTITUDE_V0); }

    void setAttitude_v1(U1 ch_id) { setFlag(ch_id, MASK_ATTITUDE_V1); }
    void resetAttitude_v1(U1 ch_id) { resetFlag(ch_id, MASK_ATTITUDE_V1); }
    bool getAttitude_v1(U1 ch_id) { return flag(ch_id, MASK_ATTITUDE_V1); }

    void setTemp_v0(U1 ch_id) {
        setFlag(ch_id, MASK_TEMP_V0);
        moveFlag(ch_id,  MASK_TEMP_V0);
    }
    void resetTemp_v0(U1 ch_id) { resetFlag(ch_id, MASK_TEMP_V0); }
    bool getTemp_v0(U1 ch_id) { return flag(ch_id, MASK_TEMP_V0); }

    void setEuler(U1 ch_id) { setFlag(ch_id, MASK_ATTITUDE_V0); }
    void resetEuler(U1 ch_id) { resetFlag(ch_id, MASK_ATTITUDE_V0); }
    bool getEuler(U1 ch_id) { return flag(ch_id, MASK_ATTITUDE_V0); }

    void setTimestamp_v0(U1 ch_id) { setFlag(ch_id, MASK_TIMESTAMP_V0); }
    void resetTimestamp_v0(U1 ch_id) { resetFlag(ch_id, MASK_TIMESTAMP_V0); }
    bool getTimestamp_v0(U1 ch_id) { return flag(ch_id, MASK_TIMESTAMP_V0); }

    void setSDDBT(U1 ch_id) {
        setFlag(ch_id, MASK_DIST_SDDBT);
        moveFlag(ch_id,  MASK_DIST_V0 | MASK_CHART_V0 | MASK_DIST_SDDBT | MASK_DIST_SDDBT_P2);
        setFlag(0, MASK_DIST_V0 | MASK_DIST_SDDBT_P2);
    }
    void resetSDDBT(U1 ch_id) { resetFlag(ch_id, MASK_DIST_SDDBT); }
    bool getSDDBT(U1 ch_id) { return flag(ch_id, MASK_DIST_SDDBT); }

    void setSDDBT_P2(U1 ch_id) {
        setFlag(ch_id, MASK_DIST_SDDBT_P2);
        moveFlag(ch_id,  MASK_DIST_V0 | MASK_CHART_V0 | MASK_DIST_SDDBT | MASK_DIST_SDDBT_P2);
        setFlag(0, MASK_DIST_V0 | MASK_DIST_SDDBT);

    }
    void resetSDDBT_P2(U1 ch_id) { resetFlag(ch_id, MASK_DIST_SDDBT_P2); }
    bool getSDDBT_P2(U1 ch_id) { return flag(ch_id, MASK_DIST_SDDBT_P2); }

    void resetAll() {
        setMask(1, 0);
        setMask(2, 0);
    }

    void commit() {
        sendChannel(1, period(1), mask(1));
        sendChannel(2, period(2), mask(2));
    }

    uint32_t period(U1 ch_id);
    void setPeriod(U1 ch_id, uint32_t period);

    QVector<Channel> getChannels() const;
    Channel getChannel(U1 channelId) const;

protected:
    Channel m_channel[3];

    void sendChannel(U1 ch_id, uint32_t period, uint32_t mask);
    void requestSpecific(ProtoBinOut &proto_out) override { proto_out.write<U1>(0); }
};



class IDBinDistSetup : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinDistSetup() : IDBin() {
    }

    ID id() override { return ID_DIST_SETUP; }
    Resp  parsePayload(FrameParser &proto) override;
    void startColdStartTimer() override;

    void setRange(uint32_t start_offset, uint32_t max_dist);

    int max() { return m_maxDist; }
    void setMax(uint32_t max_dist) { setRange(deadZone(), max_dist); }

    int deadZone() { return m_startOffset; }
    void setDeadZone(uint32_t dead_zone) { setRange(dead_zone, max()); }

    void setConfidence(int confidence);
    int confidence() { return m_confidence; }

    void requestAll() override {
        simpleRequest(v1);
        simpleRequest(v2);
    }
protected:
    uint32_t m_startOffset = 200;
    uint32_t m_maxDist = 50000;
    U1 m_confidence;

};



class IDBinServoControl : public IDBin
{
    Q_OBJECT
public:
    static constexpr int AngleScale = 100;

    // GenManual: Recorder firmware with the servo PWM output on X7 (feat/servo-pwm-x7). While it is set,
    // center_deg carries the angle to hold instead of the scan's center, every SETTING moves the servo there,
    // and the sonar's triggers do not move it. A firmware without it answers RespErrorPayload.
    enum GeneralBits : U1 { GenEnable = 0x0001, GenManual = 0x0002 };
    enum OptionsBits : U1 { OptReverse = 0x0001 };

    explicit IDBinServoControl() : IDBin() {}

    ID id() override { return ID_SERVO_CONTROL; }
    Resp parsePayload(FrameParser &proto) override;
    void startColdStartTimer() override;

    void setAll(U2 general, U2 options, U2 pwm_min_us, U2 pwm_max_us,
                S2 angle_range_deg, S2 step_deg, S2 range_deg, S2 center_deg);

    bool enabled() const { return (m_general & GenEnable) != 0; }
    void setEnabled(bool on) {
        U2 g = on ? (m_general | GenEnable) : (m_general & ~GenEnable);
        setAll(g, m_options, m_pwmMinUs, m_pwmMaxUs, m_angleRangeDeg, m_stepDeg, m_rangeDeg, m_centerDeg);
    }

    bool reverse() const { return (m_options & OptReverse) != 0; }
    void setReverse(bool on) {
        U2 o = on ? (m_options | OptReverse) : (m_options & ~OptReverse);
        setAll(m_general, o, m_pwmMinUs, m_pwmMaxUs, m_angleRangeDeg, m_stepDeg, m_rangeDeg, m_centerDeg);
    }

    U2 pwmMinUs() const { return m_pwmMinUs; }
    void setPwmMinUs(U2 v) {
        setAll(m_general, m_options, v, m_pwmMaxUs, m_angleRangeDeg, m_stepDeg, m_rangeDeg, m_centerDeg);
    }

    U2 pwmMaxUs() const { return m_pwmMaxUs; }
    void setPwmMaxUs(U2 v) {
        setAll(m_general, m_options, m_pwmMinUs, v, m_angleRangeDeg, m_stepDeg, m_rangeDeg, m_centerDeg);
    }

    S2 angleRangeDeg() const { return m_angleRangeDeg; }
    void setAngleRangeDeg(S2 v) {
        setAll(m_general, m_options, m_pwmMinUs, m_pwmMaxUs, v, m_stepDeg, m_rangeDeg, m_centerDeg);
    }

    S2 stepDeg() const { return m_stepDeg; }
    void setStepDeg(S2 v) {
        setAll(m_general, m_options, m_pwmMinUs, m_pwmMaxUs, m_angleRangeDeg, v, m_rangeDeg, m_centerDeg);
    }

    S2 rangeDeg() const { return m_rangeDeg; }
    void setRangeDeg(S2 v) {
        setAll(m_general, m_options, m_pwmMinUs, m_pwmMaxUs, m_angleRangeDeg, m_stepDeg, v, m_centerDeg);
    }

    /// center_deg as on the wire: the scan's center, or in manual mode the angle held.
    S2 centerDeg() const { return m_centerDeg; }
    /// The scan's center. In manual mode the device's center_deg is the angle held, so the scan's center is kept
    /// here and sent back when manual mode ends; setting it then only changes what will be sent.
    S2 scanCenterDeg() const { return manual() ? m_scanCenterDeg : m_centerDeg; }
    void setCenterDeg(S2 v) {
        if (manual()) {
            m_scanCenterDeg = v;
            return;
        }
        setAll(m_general, m_options, m_pwmMinUs, m_pwmMaxUs, m_angleRangeDeg, m_stepDeg, m_rangeDeg, v);
    }

    bool manual() const { return (m_general & GenManual) != 0; }
    /// Into manual mode holding the scan's center (within the servo's range), or back to the scan with its
    /// center. Not tracked (see setManualDeg): the caller reads the state back.
    void setManual(bool on);
    /// The angle held in manual mode, x AngleScale.
    S2 manualDeg() const { return m_manualDeg; }
    /// Moves the servo in manual mode; cut to +-angle_range/2 as the firmware does. Not tracked by the
    /// acknowledgement check: a drag sends tens of these a second, and the answer to one would not match the
    /// checksum of the frame sent after it -- the check would then report the servo control lost. Nothing is
    /// sent outside manual mode.
    void setManualDeg(S2 deg);
    /// Leaves manual mode here without sending anything: the device refused it and never entered it.
    void dropManual();

    /// PWM frequency of the servo outputs, Hz (ID_SERVO_CONTROL v1, Recorder firmware from 2026-10-28). 0 until
    /// the device answers v1: a firmware without it never does, and its outputs run at 50 Hz.
    U2 pwmHz() const { return m_pwmHz; }
    static constexpr int PwmHzMin = 50;
    static constexpr int PwmHzMax = 1000;
    /// The PWM period at `hz` as the firmware counts it: a 1 MHz tick, 1e6 / hz us, integer division. A pulse
    /// must be shorter: the firmware refuses a frequency the current pwm_max_us does not fit, and a pwm_max_us
    /// the current frequency does not fit.
    static int periodUs(int hz) { return hz > 0 ? 1000000 / hz : 0; }
    /// Sends the frequency, then reads it back. Not tracked: the read-back shows whether the device took it.
    void setPwmHz(U2 hz);

    /// What steps the scan (ID_SERVO_CONTROL v2 [kind][function][address], Recorder firmware from 2026-10-28). Kind
    /// 1: a ping of a device of sonar function 1 (Sonar A) or 2 (Sonar B), the one with that host address, or any
    /// of them for address 0. trigKind() is 0 until the device answers v2: a firmware without it steps on every
    /// Sonar A device.
    enum TriggerKind : U1 { TrigSonarPing = 1 };
    U1 trigKind() const { return m_trigKind; }
    U1 trigFunction() const { return m_trigFunction; }
    U1 trigAddress() const { return m_trigAddress; }
    /// Makes the pings of `function`'s device `address` (0: any) the step source, then reads it back. Not tracked.
    void setTrigger(U1 function, U1 address);
    /// The sonar function a device with this host address sits behind, as the firmware maps addresses: Sonar B
    /// (2) for 2 (its device without an address of its own) and 9..15, Sonar A (1) otherwise.
    static int functionOfAddress(int address) { return (address == 2 || (address >= 9 && address <= 15)) ? 2 : 1; }

    void requestAll() override {
        simpleRequest(v0);
        simpleRequest(v1);
        simpleRequest(v2);
    }

protected:
    void send(bool tracked);
    S2 clampManual(S2 deg) const;

    S2 m_scanCenterDeg = 0;   // the scan's center while center_deg carries the manual angle
    S2 m_manualDeg     = 0;

    U2 m_general       = 0;
    U2 m_options       = 0;
    U2 m_pwmMinUs      = 500;
    U2 m_pwmMaxUs      = 2500;
    S2 m_angleRangeDeg = 18000;
    S2 m_stepDeg       = 450;
    S2 m_rangeDeg      = 18000;
    S2 m_centerDeg     = 0;
    U2 m_pwmHz         = 0;
    U1 m_trigKind      = 0;
    U1 m_trigFunction  = 1;
    U1 m_trigAddress   = 0;
};



class IDBinPwmRoute : public IDBin
{
    Q_OBJECT
public:
    static constexpr int PwmOutCount = 3;
    enum Target : U1 { TargetOff = 0, TargetServoScan = 1 };

    explicit IDBinPwmRoute() : IDBin() {}

    ID id() override { return ID_PWM_ROUTE; }
    Resp parsePayload(FrameParser &proto) override;
    void startColdStartTimer() override;

    void setRoute(U1 out1, U1 out2, U1 out3);

    U1 target(int idx) const {
        return (idx >= 0 && idx < PwmOutCount) ? m_target[idx] : static_cast<U1>(TargetOff);
    }
    void setTarget(int idx, U1 t) {
        if (idx < 0 || idx >= PwmOutCount) return;
        U1 t0 = m_target[0], t1 = m_target[1], t2 = m_target[2];
        if (idx == 0) t0 = t;
        else if (idx == 1) t1 = t;
        else t2 = t;
        setRoute(t0, t1, t2);
    }

    void requestAll() override { simpleRequest(v0); }

protected:
    U1 m_target[PwmOutCount] = { TargetServoScan, TargetOff, TargetOff };
};



// Calibration stand. Control-only: the device answers a command and publishes nothing else, so
// this class sends and never mirrors device state — unlike every setup command around it.
//
// Frames go out through binFrameOut, not hashBinFrameOut. The hash path re-sends until the
// device acknowledges, which is right for a setting and wrong here: a resent Start restarts a
// physical scan.
class IDBinStandScan : public IDBin
{
    Q_OBJECT
public:
    enum Command : U1 {
        CmdNop    = 0,
        CmdStart  = 1,
        CmdStop   = 2,
        CmdPause  = 3,
        CmdResume = 4,
        CmdHome   = 5
    };

    enum ScanOrder : U1 {
        AzimuthToElevation = 0,
        ElevationToAzimuth = 1
    };

    enum OptionsBits : U1 {
        OptReverseInner    = 0x01,
        OptContinuousInner = 0x02
    };

    struct Scan {
        U1 order      = AzimuthToElevation;
        U1 options    = 0;
        U2 fires      = 1;
        U2 cycles     = 1;
        U2 settleMs   = 0;
        U2 postFireMs = 0;
        S4 innerStart = 0;
        S4 innerEnd   = 0;
        S4 innerStep  = 0;
        S4 outerStart = 0;
        S4 outerEnd   = 0;
        S4 outerStep  = 0;
    };

    explicit IDBinStandScan() : IDBin() {}

    ID id() override { return ID_STAND_SCAN; }
    Resp parsePayload(FrameParser &proto) override;

    // The capability probe. The command does nothing on a device that has a stand and is
    // answered with an unknown-id response by one that does not — which is the only signal
    // available, since there is no readback to request.
    void probe();

    void start(const Scan &scan);
    void stop()   { command(CmdStop); }
    void pause()  { command(CmdPause); }
    void resume() { command(CmdResume); }
    void home()   { command(CmdHome); }

protected:
    void command(Command cmd);
    void writeScan(Command cmd, const Scan &scan);
};



class IDBinDevSync : public IDBin
{
    Q_OBJECT
public:
    enum SyncSource : U1 { SyncOff = 0, SyncTimer = 1 };

    explicit IDBinDevSync() : IDBin() {}

    ID id() override { return ID_DEV_SYNC; }
    Resp parsePayload(FrameParser &proto) override;
    void startColdStartTimer() override;
    void requestAll() override { simpleRequest(v0); }

    int  portCount() const { return m_portSource.size(); }
    U1   portSource(int idx) const {
        return (idx >= 0 && idx < m_portSource.size())
            ? static_cast<U1>(m_portSource.at(idx))
            : static_cast<U1>(SyncOff);
    }
    U2   periodMs() const { return m_periodMs; }
    bool synced()  const { return m_synced; }

    void setPortSource(int idx, U1 src) {
        if (idx < 0 || idx >= m_portSource.size()) return;
        if (static_cast<U1>(m_portSource.at(idx)) == src) return;
        m_portSource[idx] = static_cast<char>(src);
        m_pending = true;
    }
    void setPeriodMs(U2 ms) {
        if (m_periodMs == ms) return;
        m_periodMs = ms;
        m_pending = true;
    }

    void flushPending();
    void commitFromDisplayed() {
        m_committedPortSource = m_portSource;
        m_committedPeriodMs   = m_periodMs;
        m_pending = false;
    }
    void revertToCommitted() {
        m_portSource = m_committedPortSource;
        m_periodMs   = m_committedPeriodMs;
        m_pending = false;
    }
    void reset() {
        m_synced  = false;
        m_pending = false;
        m_periodMs = 20;
        m_portSource.clear();
        m_committedPeriodMs = 20;
        m_committedPortSource.clear();
    }

protected:
    QByteArray m_portSource;
    U2         m_periodMs = 20;
    bool       m_synced   = false;

    QByteArray m_committedPortSource;
    U2         m_committedPeriodMs = 20;

    bool       m_pending  = false;
};



class IDBinChartSetup : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinChartSetup() : IDBin() {
    }

    ID id() override { return ID_CHART_SETUP; }
    Resp  parsePayload(FrameParser &proto) override;
    void startColdStartTimer() override;

    void setV0(U2 count, U2 resolution, U2 offset);

    U2 count() { return m_sanpleCount; }
    void setCount(U2 count) {
        setV0(count, resolution(), offset());
    }

    U2 resolution() { return m_sanpleResolution; }
    void setResolution(U2 resol) {
        setV0(count(), resol, offset());
    }

    U2 offset() { return m_sanpleOffset; }
    void setOffset(U2 offset) {
        setV0(count(), resolution(), offset);
    }

protected:
    U2 m_sanpleCount = 100;
    U2 m_sanpleResolution = 10;
    U2 m_sanpleOffset = 0;
};


class IDBinDSPSetup : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinDSPSetup() : IDBin() {
    }

    ID id() override { return ID_DSP; }
    Resp  parsePayload(FrameParser &proto) override;
    void startColdStartTimer() override;

    void setV0(U1 hor_smooth_factor);

    U1 horSmoothFactor() { return m_horSmoothFactor; }
    void setHorSmoothFactor(U1 hor_smooth_factor) {
        setV0(hor_smooth_factor);
    }

protected:
    U1 m_horSmoothFactor = 0;
};


class IDBinTransc : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinTransc() : IDBin() {
    }

    ID id() override { return ID_TRANSC; }
    Resp  parsePayload(FrameParser &proto) override;
    void startColdStartTimer() override;

    void setTransc(U2 freq, U1 pulse, U1 boost);

    U2 freq() { return m_freq; }
    void setFreq(U2 freq) { setTransc(freq, pulse(), boost()); }

    U1 pulse() { return m_pulse; }
    void setPulse(U1 pulse) { setTransc(freq(), pulse, boost()); }

    U1 boost() { return m_boost; }
    void setBoost(U1 boost) { setTransc(freq(), pulse(), boost); }

protected:
    U2 m_freq = 700;
    U1 m_pulse = 10;
    U1 m_boost = 0;
};



class IDBinSoundSpeed : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinSoundSpeed() : IDBin() {
    }

    ID id() override { return ID_SND_SPEED; }
    Resp  parsePayload(FrameParser &proto) override;
    void startColdStartTimer() override;

    void setSoundSpeed(U4 snd_spd);
    int getSoundSpeed() { return m_soundSpeed; }
protected:
    U4 m_soundSpeed = 1500000;
};



class IDBinUART : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinUART() : IDBin() {
    }

    ID id() override { return ID_UART; }
    Resp  parsePayload(FrameParser &proto) override;
    void startColdStartTimer() override;

    struct UART
    {
        U1 id = 0;
        U4 baudrate = 115200;
        U1 dev_address = 0;
    };

    void setBaudrate(U4 baudrate);
    int getBaudrate(int id = 1) { return m_uart[id].baudrate; }

    void setDevAddress(U1 addr);
    U1 devAddress() { return m_uart[1].dev_address; }

    void setDevDefAddress(U1 addr);
    U1 devDefAddress() { return devDef_address; }

    QVector<UART> getUart() const;

    void requestAll() override {
        simpleRequest(v0);
        simpleRequest(v1);
        simpleRequest(v2);
    }
protected:
    UART m_uart[5];
    U1 devDef_address = 0;

    void requestSpecific(ProtoBinOut &proto_out) override {
        appendKey(proto_out);
        if(proto_out.ver() == 0 || proto_out.ver() == 1) {
            proto_out.write<U1>(1);
        }
    }
};

/**
 * Wi-Fi module (board 87), command ID_WIFI. The module is a Wi-Fi client for the host: it scans,
 * joins the boat's access point and reports the link. Contract: docs/WIFI_MODULE.md.
 *
 * GETTING v0 -> CONTENT v0 status (also pushed by the module on every state change); GETTING v2
 * starts a scan answered by one CONTENT v2 frame per network; GETTING v4 lists the saved networks,
 * one frame each. CONTENT v1 is the link report the module sends once a second. SETTING v1 sets
 * that period, v3 connects, v4 carries an action. Every SETTING is confirmed with the usual
 * {code, checksum} frame, which the IDBin base verifies like for any other device.
 */
class IDBinWifi : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinWifi() : IDBin() {}

    ID id() override { return ID_WIFI; }
    Resp parsePayload(FrameParser &proto) override;
    void requestAll() override {
        simpleRequest(v0);
        simpleRequest(v4);
        simpleRequest(v5);
        simpleRequest(v7);
    }

    enum State : uint8_t { StateIdle = 0, StateSearching = 1, StateConnecting = 2, StateConnected = 3,
                           StateAp = 4 /* firmware 0.10: the module is an access point */ };
    enum StatusFlag : uint8_t { FlagAutoConnect = 1 << 0, FlagScanning = 1 << 1, FlagCurrentSaved = 1 << 2,
                                FlagRadioOff = 1 << 3 /* firmware 0.20: switched off by ID_WIFI_RF */ };
    enum Reason : uint8_t { ReasonNone = 0, ReasonNotFound = 1, ReasonWrongPassword = 2,
                            ReasonAssociation = 3, ReasonLinkLost = 4, ReasonOther = 5 };
    enum Security : uint8_t { SecOpen = 0, SecWep = 1, SecWpa = 2, SecWpa2 = 3, SecWpaWpa2 = 4,
                              SecWpa3 = 5, SecWpa2Wpa3 = 6, SecEnterprise = 7, SecOwe = 8, SecUnknown = 255 };
    enum NetworkFlag : uint8_t { NetSaved = 1 << 0, NetCurrent = 1 << 1 };
    enum Action : uint8_t { ActDisconnect = 0, ActAutoConnect = 1, ActForget = 2, ActForgetAll = 3 };
    enum RelayMode : uint8_t { RelayOff = 0, RelayUdp = 1, RelayTcp = 2 };
    enum RelayState : uint8_t { RelayStateOff = 0, RelayNoWifi = 1, RelayNoPeer = 2, RelayReady = 3 };

    struct Status {
        bool valid = false;
        uint8_t state = StateIdle;
        uint8_t flags = 0;
        int8_t rssi = 0;
        uint8_t channel = 0;
        uint8_t security = SecUnknown;
        uint8_t disconnectCode = 0;   // ESP-IDF reason code of the last disconnect
        uint8_t reason = ReasonNone;
        uint8_t clients = 0;          // access-point clients (firmware 0.10, AP role)
        QString bssid;
        QString ip, mask, gateway, dns;
        uint32_t rxBps = 0, txBps = 0;
        QString ssid;
    };
    struct Report {
        bool valid = false;
        uint8_t state = StateIdle;
        int8_t rssi = 0;
        uint32_t rxBps = 0, txBps = 0, rxTotal = 0, txTotal = 0, uptimeS = 0;
        int resetReason = -1;         // esp_reset_reason_t of the last reset (0.11+, 23-byte v1); -1 before
        bool lostValid = false;       // the module reports lostRequests (0.14+, 27-byte v1)
        uint32_t lostRequests = 0;    // requests it could not queue since its start; not reset by reading; 0 is normal
    };
    /**
     * Relay of the SBP stream between this port and the boat (firmware 0.6+): frames not addressed
     * to the module go out over UDP/TCP, frames from the boat come in unchanged, and the module
     * itself answers at `addr`. GETTING/SETTING v5; the module stores it and applies it at once.
     */
    struct Relay {
        bool valid = false;
        uint8_t mode = RelayOff;
        uint8_t addr = 87;
        QString ip;                   // "a.b.c.d"; 0.0.0.0 with UDP = answer whoever sent last
        uint16_t rport = 0;           // the boat's port
        uint16_t lport = 0;           // own UDP port, 0 = same as rport
    };
    /** GETTING v6, 47 bytes. Counters run since the relay was switched on. */
    struct RelayStats {
        bool valid = false;
        uint8_t state = RelayStateOff;
        QString peerIp;
        uint16_t peerPort = 0;
        uint32_t upFrames = 0, upBytes = 0, upPackets = 0, upDrops = 0;
        uint32_t downPackets = 0, downBytes = 0, downFrames = 0, downDrops = 0;
        uint32_t localFrames = 0, tcpConnects = 0;
    };

    /**
     * Radio (firmware 0.8+), ID_WIFI v7: the four settings and what is in effect. Power is in 0.25 dBm
     * steps (8..80 since 0.11, which stores 81..84 as 80); protocols 0x01 b, 0x03 b/g, 0x07 b/g/n, 0x08 LR, 0x0F b/g/n+LR; bandwidth 1 = 20 MHz,
     * 2 = 40 MHz; power save 0 off, 1 min, 2 max. phyNow: 0 LR, 1 11b, 2 11g, 4 HT20, 5 HT40, 0xFF none.
     */
    struct Radio {
        bool valid = false;
        uint8_t powerQ = 84, protocols = 0x07, bandwidth = 1, powerSave = 0;
        uint8_t powerNowQ = 0, phyNow = 0xFF, protocolsNow = 0, bandwidthNow = 0;
    };

    struct Network {
        bool valid = false;           // false for an index whose frame never arrived
        int8_t rssi = 0;
        uint8_t channel = 0;
        uint8_t security = SecUnknown;
        uint8_t flags = 0;
        QString bssid;
        QString ssid;
    };

    const Status& status() const { return status_; }
    const Report& report() const { return report_; }
    /** Networks of the last scan, strongest first, sized by the module's total. */
    const QVector<Network>& networks() const { return networks_; }
    /** Saved SSIDs; an entry is empty when its frame was lost. */
    const QStringList& savedNetworks() const { return saved_; }
    /** Saved networks the module announced: -1 before its list came. */
    int savedTotal() const { return savedTotal_; }
    bool scanning() const { return scanPending_; }
    /** Networks the last scan reported: -1 no scan result yet, 0 it heard none. */
    int scanTotal() const { return scanTotal_; }
    const Relay& relay() const { return relay_; }
    const RelayStats& relayStats() const { return relayStats_; }
    /** -1 not yet known, 0 the firmware refused v5/v6 (no relay), 1 the module answered. */
    int relaySupport() const { return relaySupport_; }
    void markRelayUnsupported() { relaySupport_ = 0; }
    void requestRelay() { simpleRequest(v5); }
    void requestRelayStats() { simpleRequest(v6); }
    /** SETTING v5. @returns false when the address text is not four octets; nothing is sent then. */
    bool setRelay(uint8_t mode, uint8_t addr, const QString& ip, uint16_t rport, uint16_t lport);
    const Radio& radio() const { return radio_; }
    /** -1 not yet known, 0 the firmware refused v7 (before 0.8), 1 the module answered. */
    int radioSupport() const { return radioSupport_; }
    void markRadioUnsupported() { radioSupport_ = 0; }
    void requestRadio() { simpleRequest(v7); }
    /** SETTING v7. Protocol and width changes make the module reconnect. */
    void setRadio(uint8_t powerQ, uint8_t protocols, uint8_t bandwidth, uint8_t powerSave);
    bool hasData() const { return status_.valid || report_.valid || !networks_.isEmpty() || !saved_.isEmpty(); }

    /** GETTING v2. The answer arrives as one frame per network; scanning() drops when all came. */
    void scan();
    void cancelScan() { scanPending_ = false; }
    /** SETTING v3. @returns false when the SSID is empty or a string is too long; nothing is sent then. */
    bool connectTo(const QString& ssid, const QString& password, bool save);
    /** SETTING v4: Action code, SSID only for ActForget. */
    void action(uint8_t code, const QString& ssid = QString());
    /** SETTING v1: link-report period, 0 stops it, 100..60000 ms otherwise. */
    void setReportPeriod(uint16_t ms);

    /** For the driver's polling loop: ends a scan whose frames stopped coming. @returns true once. */
    bool expireScan(int64_t nowMs);
    void reset();

protected:
    static QString readAddr(FrameParser& proto);
    static QString readMac(FrameParser& proto);
    static QString readSsid(FrameParser& proto);

    Status status_;
    Report report_;
    QVector<Network> networks_;
    QStringList saved_;
    Relay relay_;
    RelayStats relayStats_;
    int relaySupport_ = -1;
    Radio radio_;
    int radioSupport_ = -1;
    int scanTotal_ = -1;
    int scanFilled_ = 0;
    bool scanFresh_ = false;          // a scan was asked for: its first result replaces the last scan's
    int savedTotal_ = -1;
    bool scanPending_ = false;
    int64_t scanStartedMs_ = 0;
    static constexpr int64_t scanTimeoutMs = 8000;
};

/**
 * Wi-Fi module network setup (firmware 0.10+), command ID_WIFI_NET 0x58: the role (station or access
 * point), the access point's network, its address and DHCP, the two UART lines and where each maps
 * to on the network, their counters, the access point's clients and the module's own SBP address
 * while a line relays. Every SETTING starts with the key 0xC96B5D4A, like ID_UART, because these
 * settings can cut the module off; each is confirmed, then the module sends the values in force.
 * Firmware 0.12 adds v7 (GETTING only): per physical port X1/X2 a summary, the devices heard from it and the
 * network side of its line, decoded by WifiPorts (src/device/wifi_ops.h); 0.11 refuses v7 with ERR_VERSION.
 * Contract: the module project's docs/SBP_WIFI.md §3a and docs/NETWORK.md; docs/WIFI_MODULE.md §2e.
 */
/**
 * @brief ID_WIFI_SURVEY (0x59, Wi-Fi module firmware 0.16): how much of the time each channel carries Wi-Fi frames.
 * SETTING v0 {key, U2 dwell ms, U2 channel mask} starts a survey (the radio leaves its channel for about 1.7 s); the
 * module answers OK at once and sends one CONTENT v0 per channel when it ends; GETTING v0 sends the last survey again
 * (ERR_RUNTIME while one runs). Contract: the module project's docs/SBP_WIFI.md 3b.
 */
class IDBinWifiSurvey : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinWifiSurvey() : IDBin() {}

    ID id() override { return ID_WIFI_SURVEY; }
    Resp parsePayload(FrameParser &proto) override;
    void requestAll() override {}               // asked only by the operator

    struct Channel {
        bool valid = false;
        uint8_t channel = 0;
        uint16_t dwellMs = 0, busyPermille = 0, frames = 0, transmitters = 0;
        int8_t strongest = -128, noiseDbm = 0;
        uint8_t flags = 0;
        uint16_t ownPermille = 0;               // of the airtime, the module's own network's (firmware 0.17)
        bool ownValid = false;                  // the frame had it (16 bytes; 14 before 0.17)
    };

    /** SETTING v0; dwellMs 0 = the module's default. The last survey's channels stay until the new one's first frame. */
    void start(uint16_t dwellMs, uint16_t mask);
    void requestLast() { simpleRequest(v0); }
    void cancel() { running_ = false; }
    void reset();
    const QVector<Channel>& channels() const { return channels_; }
    /** Channels the last survey reported: -1 none yet, 0 the module had no survey. */
    int total() const { return total_; }
    bool running() const { return running_; }
    int64_t startedMs() const { return startedMs_; }
    uint16_t dwellMs() const { return dwellMs_; }
    /** Surveys completed so far (every channel of one arrived): the driver takes each into its mean once. */
    int completedCount() const { return completed_; }

    // The link rate report (GETTING v1, firmware 0.18): one raw CONTENT v1 page per other end, parsed by the driver
    // (WifiLinkRate::parse, device/wifi_ops.h).
    /** Listen windowMs (50..1000; 0 = the module's 200) and report the other ends' rates. */
    void requestLinkRate(uint16_t windowMs);
    void cancelLinkRate() { rateRunning_ = false; }
    const QVector<QByteArray>& ratePages() const { return ratePages_; }
    /** Other ends in the last report: -1 none yet, 0 none (a station not joined, an access point without clients). */
    int ratePeers() const { return ratePeers_; }
    bool rateRunning() const { return rateRunning_; }
    int64_t rateStartedMs() const { return rateStartedMs_; }
    uint16_t rateWindowMs() const { return rateWindowMs_; }
    /** Reports completed so far (every page of one arrived). */
    int rateCompletedCount() const { return rateCompleted_; }

protected:
    Resp parseLinkRate(FrameParser &proto);
    QVector<QByteArray> ratePages_;
    int ratePeers_ = -1;
    int rateFilled_ = 0;
    int rateCompleted_ = 0;
    bool rateRunning_ = false;
    bool rateFresh_ = false;
    int64_t rateStartedMs_ = 0;
    uint16_t rateWindowMs_ = 0;
    QVector<Channel> channels_;
    int total_ = -1;
    int filled_ = 0;
    int completed_ = 0;
    bool running_ = false;
    bool fresh_ = false;
    int64_t startedMs_ = 0;
    uint16_t dwellMs_ = 0;
};

class IDBinWifiNet : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinWifiNet() : IDBin() { portClock_.start(); }

    ID id() override { return ID_WIFI_NET; }
    Resp parsePayload(FrameParser &proto) override;
    void requestAll() override {
        simpleRequest(v0);
        simpleRequest(v1);
        simpleRequest(v2);
        simpleRequest(v3);            // no payload: both lines
        simpleRequest(v6);
    }

    enum Role : uint8_t { RoleStation = 0, RoleAccessPoint = 1 };
    enum ApAuth : uint8_t { ApOpen = 0, ApWpa2 = 1, ApWpa2Wpa3 = 2 };
    enum LineMode : uint8_t { LineOff = 0, LineUdp = 1, LineTcp = 2 };
    enum LineDest : uint8_t { DestFixed = 0, DestSenders = 1, DestBroadcast = 2 };
    enum LineState : uint8_t { LineStateOff = 0, LineNoWifi = 1, LineNoPeer = 2, LineReady = 3 };
    static constexpr int kLines = 2;

    struct RoleInfo { bool valid = false; uint8_t saved = RoleStation, running = RoleStation; };
    struct Ap {
        bool valid = false;
        uint8_t channel = 6, hidden = 0, maxClients = 4, auth = ApWpa2;
        QString ssid;
    };
    struct IpCfg {
        bool valid = false;
        quint32 ip = 0, mask = 0, poolStart = 0, poolEnd = 0;
        bool dhcp = true;
        uint16_t leaseMin = 120;
        uint8_t offer = 0;            // bit0 gateway, bit1 DNS
    };
    struct Line {
        bool valid = false;
        uint8_t mode = LineOff, dest = DestSenders;
        quint32 ip = 0;
        uint16_t rport = 0, lport = 0;
        uint32_t baud = 0;
        int8_t txPin = -1, rxPin = -1;
        uint8_t uart = 0;             // 0 off, 1 running on these pins, 2 saved pins start after a reboot
        // Firmware 0.21 (a 21-byte record): line 1's input translator, saved / running this boot / the build's
        // translators (bit0 GODIO telemetry). -1 before 0.21.
        int xlatSaved = -1, xlatRunning = -1, xlatBuild = -1;
    };
    struct LineStats {
        bool valid = false;
        uint8_t state = LineStateOff;
        quint32 peerIp = 0;
        uint16_t peerPort = 0;
        uint8_t senders = 0;
        // frames up, bytes up, packets up, drops up, packets down, bytes down, frames down, drops down,
        // frames for the module, TCP connects, UART receive overflows, UART transmit drops
        uint32_t counters[12] = {};
    };
    struct Client { QString mac; quint32 ip = 0; int8_t rssi = 0; };

    /** -1 not yet known, 0 no answer / refused (firmware before 0.10), 1 the module answered. */
    int support() const { return support_; }
    void markUnsupported() { support_ = 0; portSupport_ = 0; }
    /** Port information (v7): -1 not yet known, 0 refused or no answer (firmware before 0.12), 1 answered. */
    int portSupport() const { return portSupport_; }
    void markPortsUnsupported() { portSupport_ = 0; }
    const WifiPorts& ports() const { return ports_; }
    /** GETTING v7 without payload: the summary (page 0) of both ports, one CONTENT each. */
    void requestPorts();
    /** GETTING v7 {port, page}: port 0 = X1, 1 = X2; page 0 summary, 1 devices, 2 network side. */
    void requestPortPage(uint8_t port, uint8_t page);
    /** The line of the last v3 record parsed; -1 before one. */
    int lastLine() const { return lastLine_; }
    const RoleInfo& role() const { return role_; }
    const Ap& ap() const { return ap_; }
    const IpCfg& ipCfg() const { return ipCfg_; }
    const Line& line(int i) const { return lines_[qBound(0, i, kLines - 1)]; }
    const LineStats& lineStats(int i) const { return stats_[qBound(0, i, kLines - 1)]; }
    const QVector<Client>& clients() const { return clients_; }
    int ownAddress() const { return ownAddress_; }

    void requestStats() { simpleRequest(v4); }
    void requestClients() { clientsTotal_ = -1; simpleRequest(v5); }

    /** SETTING v0. The role takes effect at boot; with reboot the module restarts right after the answer. */
    void setRole(uint8_t role, bool reboot, bool forgetLines);
    /** SETTING v1. An empty password with security on keeps the current one. @returns false if invalid. */
    bool setAp(uint8_t channel, bool hidden, uint8_t maxClients, uint8_t auth, const QString& ssid, const QString& password);
    /** SETTING v2. @returns false when an address is not a.b.c.d; the module checks the rest. */
    bool setIpCfg(const QString& ip, const QString& mask, bool dhcp, const QString& poolStart, const QString& poolEnd,
                  uint16_t leaseMin, bool offerGateway, bool offerDns);
    /** SETTING v3 for one line. Before 0.12 line 0 ignores baud (ID_UART sets its rate); line 0's pins are fixed. */
    bool setLine(uint8_t line, uint8_t mode, uint8_t dest, const QString& ip, uint16_t rport, uint16_t lport,
                 uint32_t baud, int8_t txPin, int8_t rxPin);
    /** SETTING v6: the module's SBP address while a line relays; confirmed from the old address. */
    void setOwnAddress(uint8_t addr);

    static QString addrText(quint32 a);
    static bool addrValue(const QString& s, quint32* out);
    void reset();

protected:
    static quint32 readAddr(FrameParser& proto);
    static void writeAddr(ProtoBinOut& out, quint32 a);

    int support_ = -1;
    RoleInfo role_;
    Ap ap_;
    IpCfg ipCfg_;
    Line lines_[kLines];
    LineStats stats_[kLines];
    QVector<Client> clients_;
    int clientsTotal_ = -1;
    int ownAddress_ = -1;
    int lastLine_ = -1;
    int portSupport_ = -1;
    WifiPorts ports_;
    QElapsedTimer portClock_;         // monotonic time of the port summaries, for their byte rates
};

/**
 * @brief ID_WIFI_RF (0x5A, Wi-Fi module firmware 0.20): the radio switched off for a while, to swap an antenna live.
 * SETTING v0 {key, U1 on, U2 seconds}: on 0 = off for the seconds (0 = until "on" or a reboot; over the network only
 * 1..600, else ERR_PAYLOAD), 1 = on now. The module confirms, then sends CONTENT v0 {U1 on, U2 seconds left} (after a
 * refusal too); GETTING v0 asks for it. The rules are WifiRf (device/wifi_ops.h). Contract: the module project's
 * docs/SBP_WIFI.md, ID_WIFI_RF.
 */
class IDBinWifiRf : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinWifiRf() : IDBin() {}

    ID id() override { return ID_WIFI_RF; }
    Resp parsePayload(FrameParser &proto) override;
    // Asked by the page and when the module's status says its radio is off; also what a SETTING without its
    // confirmation is followed by (IDBin's set timer).
    void requestAll() override { simpleRequest(v0); }

    struct State {
        bool valid = false;
        bool on = true;
        uint16_t secondsLeft = 0;     // off: what is left of the module's timer when it answered, 0 = no timer
    };
    /** SETTING v0 {key, 0, seconds}: off for `seconds`, 0 = until "on" or a reboot. */
    void switchOff(uint16_t seconds);
    /** SETTING v0 {key, 1, 0}: on now. */
    void switchOn();
    const State& state() const { return state_; }
    /** The module's last answer is overruled by a newer status (ID_WIFI v0 flag bit3): the radio came back. */
    void markOn() { state_.valid = true; state_.on = true; state_.secondsLeft = 0; }
    void reset() { state_ = State(); }

protected:
    void send(uint8_t on, uint16_t seconds);
    State state_;
};

/**
 * @brief ID_WIFI_PROFILE (0x5B, Wi-Fi module firmware 0.22): the module's whole profile. SETTING v1 {key, U2 length,
 * U4 CRC-32} begins an upload, v2 {U2 offset, bytes} carries a piece, v3 {key} applies (each confirmed); GETTING v0 /
 * CONTENT v0 {U1 flags, U4 fingerprint, U2 received, U2 announced, U1 len, name} is the profile the module carries,
 * GETTING v3 / CONTENT v3 {U1 reason} why its last apply was refused (0 none; pushed after every apply). The block, the
 * sequence and the rules are WifiProfile* (device/wifi_profile.h). Contract: the module project's docs/SBP_WIFI.md 3g.
 * Frames go out through binFrameOut: the upload paces and repeats them itself.
 */
class IDBinWifiProfile : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinWifiProfile() : IDBin() {}

    ID id() override { return ID_WIFI_PROFILE; }
    Resp parsePayload(FrameParser &proto) override;
    void requestAll() override { simpleRequest(v0); }

    struct Status {
        bool valid = false;
        uint8_t flags = 0;                        // bit0 loaded with a profile, bit1 a setting changed since
        uint32_t fingerprint = 0;
        uint16_t received = 0, announced = 0;     // an upload in progress (0 of 0: none)
        QString name;
    };
    const Status& status() const { return status_; }
    /** The reason of the last CONTENT v3; -1 before one. */
    int lastReason() const { return lastReason_; }
    /** SETTING of version `ver` (1 begin, 2 piece, 3 apply) with the payload as WifiProfile builds it. */
    void sendSetting(int ver, const QByteArray& payload);
    /** GETTING of version `ver` (0 the status, 3 the last refusal's reason). */
    void ask(int ver) { simpleRequest(Version(ver)); }
    void reset() { status_ = Status(); lastReason_ = -1; }

protected:
    Status status_;
    int lastReason_ = -1;
};

class IDBinVersion : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinVersion() : IDBin() {
    }

    enum BootMode : int8_t {
        BootModeUnknown = -1,
        BootModeFirmware = 0,
        BootModeBootloader = 1
    };

    ID id() override { return ID_VERSION; }
    Resp  parsePayload(FrameParser &proto) override;
    uint8_t productName() { return 0; }

    BoardVersion boardVersion() { return m_boardVersion; }
    uint8_t boardVersionMinor() { return m_boardVersionMinor; }
    int bootMode() const { return _bootMode; }

    int fwVersion() { return _fwVersion; }
    int fwVersionMinor() { return _fwVersionMinor; }

    int bootVersion() { return _bootVersion; }
    int bootVersionMinor() { return _bootVersionMinor; }

    uint32_t serialNumber() { return m_serialNumber; }

    void reset() {
        m_boardVersion = BoardNone;
        m_boardVersionMinor = 0;
        m_serialNumber = 0;
        _bootMode = BootModeUnknown;
    }

    QByteArray uid() { return _uid; }

    void requestAll() override {
        simpleRequest(v0);
        simpleRequest(v1);
        simpleRequest(v2);
    }
protected:
    BoardVersion m_boardVersion = BoardNone;
    uint8_t m_boardVersionMinor = 0;
    uint32_t m_serialNumber = 0;
    QString m_pn;
    QByteArray _uid;
    int _fwVersion = 0;
    int _fwVersionMinor = 0;
    int _bootVersion = 0;
    int _bootVersionMinor = 0;
    int _bootMode = BootModeUnknown;
};

class IDBinMark : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinMark() : IDBin() {
    }

    ID id() override { return ID_MARK; }
    Resp  parsePayload(FrameParser &proto) override;

    bool mark() { return (m_mark & 0x1) == 0x1; }
    void setMark();

protected:
    U1 m_mark = 0;
};

class IDBinFlash : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinFlash() : IDBin() {
    }

    ID id() override { return ID_FLASH; }
    Resp  parsePayload(FrameParser &proto) override;

    void flashing();
    void restore();
    void erase();

protected:
};



class IDBinBoot : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinBoot() : IDBin() {
    }

    ID id() override { return ID_BOOT; }
    Resp  parsePayload(FrameParser &proto) override;

    void reboot();
    void runFW();
protected:

};



class IDBinUpdate : public IDBin
{
    Q_OBJECT
public:
    struct __attribute__((packed)) ID_UPGRADE_V0
    {
       uint16_t lastNumMsg = 0;
       uint32_t lastOffset = 0;
       uint8_t type = 0;
       uint8_t rcvNumMsg = 0;
    };

    explicit IDBinUpdate() : IDBin() {
    }

    ID id() override { return ID_UPDATE; }
    Resp  parsePayload(FrameParser &proto) override;

    void setUpdate(QByteArray fw);
    QByteArray getUpdate() const;

    int availSend() {return _fw.length() - _currentFwOffset; }
    int progress() {
        if(_fw.length() != 0) {
            return static_cast<int>((100ll * static_cast<long long>(_currentFwOffset)) /
                                    static_cast<long long>(_fw.length()));
        }

        return 0;
    }

    ID_UPGRADE_V0 getDeviceProgress() { return _progress; }

    uint16_t currentNumPacket() {
        return _currentNumPacket;
    }

    int currentFwOffset() {
        return _currentFwOffset;
    }

    void setUpgradeNewPoint(uint16_t num_packet, int offset) {
        _currentNumPacket = num_packet + 1;
        _currentFwOffset = offset;
    }

public slots:
    bool putUpdate(bool is_auto_offset = true);

protected:
    uint16_t _currentNumPacket = 0;
    int _currentFwOffset = 0;
    QByteArray _fw;
    const uint16_t _packetSize = 32*3;

    ID_UPGRADE_V0 _progress;
};

class IDBinVoltage : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinVoltage() : IDBin() {
    }

    ID id() override { return ID_VOLTAGE; }
    Resp  parsePayload(FrameParser &proto) override;

    float voltage(int v_id) { return _v[v_id]; }
    /* The payload carries the channel, and a handler has no other way to learn which one this
       frame was about -- _v alone cannot say what just changed. */
    uint8_t lastId() const { return _last_id; }
    int32_t lastRaw() const { return _last_raw; }
protected:
    float _v[256];   // indexed by a uint8_t channel id, so 0..255 -- 255 entries overran by one
    uint8_t _last_id = 0;
    int32_t _last_raw = 0;
};

/* Presets in the sonar's own flash (ID_PRESET, nanoSSS firmware 11.21+). Contract: the l412_adc project's
   docs/PRESETS_CONCEPT_2026-10-21.md, section 5.
     GET v0                         -> one CONTENT v0 frame a slot, all eight, an empty one flagged unused
     CONTENT v0, 41 bytes           [slot u8][flags u8: bit 0 used, bit 1 factory][freq kHz u16][pulses u8]
                                    [boost u8][dsp u8][cells u16][cell mm u16][offset u16][period ms u32]
                                    [name 24 B, UTF-8, padded with zeros]
     SETTING v0 [slot]              recall: the sonar applies the slot and reports ID_TRANSC, ID_CHART_SETUP,
                                    ID_DSP and ID_DATASET as after a change by hand; an empty slot is refused
     SETTING v1 [slot][key][name]   save the sonar's current settings into the slot; the key is needed for
                                    the factory slots 0..1; the slot's frame comes back
     SETTING v2 [slot][key]         delete the slot; the key always
   The answer to a SETTING is a RESP code: 1 done, 2 the flash write did not verify, 3 a wrong or empty slot,
   7 no key. */
class IDBinPreset : public IDBin
{
    Q_OBJECT
public:
    static constexpr int kSlots = 8;
    static constexpr int kNameBytes = 24;
    static constexpr int kFrameBytes = 41;

    struct Slot {
        bool known = false;     // its frame has come
        bool used = false;
        bool factory = false;
        uint16_t freqKhz = 0;
        uint8_t pulses = 0;
        uint8_t boost = 0;
        uint8_t dsp = 0;
        uint16_t cells = 0;
        uint16_t cellMm = 0;
        uint16_t offset = 0;
        uint32_t periodMs = 0;
        QString name;
    };

    explicit IDBinPreset() : IDBin() {}

    ID id() override { return ID_PRESET; }
    Resp parsePayload(FrameParser &proto) override;

    const Slot& slot(int i) const { return slots_[i]; }
    /// The slot of the last frame parsed, -1 before any.
    int lastSlot() const { return lastSlot_; }

    void recall(int slot);
    /// `name` goes as UTF-8, cut to 24 bytes on a character boundary.
    void save(int slot, const QString& name);
    void remove(int slot);

    /// The name as the sonar keeps it: UTF-8, at most 24 bytes, a character never cut in two.
    static QByteArray nameBytes(const QString& name);

private:
    Slot slots_[kSlots];
    int lastSlot_ = -1;
};

/* A GODIO ring board's sync counters (ID_RING_STAT, firmware 1.16+). Contract: L412_GODIO
   docs/RING_HOLES_2026-10-23.md, section 8.
     GETTING v0, no payload       -> CONTENT v0, 32 bytes: [address u8][role u8: 0 leader or alone, 1 follower]
                                    [a board before this one u8][spare u8][guard window us u32]
                                    [last BREAK interval us u32, 0 not measured][relayed u32][BREAKs u32]
                                    [torn by a BREAK u32][leads on a timeout u32][pings skipped u32]
     SETTING v0 [key]             zeroes the five counters; RESP 1, then the state frame
   Asked only on request (no cold-start ask): on a ring with no slack the answer can cost a following board a ping. */
class IDBinRingStat : public IDBin
{
    Q_OBJECT
public:
    struct State {
        bool known = false;
        U1 address = 0;
        U1 role = 0;          // 0 leader or alone, 1 follower
        U1 upstream = 0;      // a board before this one (seen from its traffic)
        U4 guardUs = 0;
        U4 breakIntervalUs = 0;
        U4 relayed = 0;
        U4 breaks = 0;
        U4 torn = 0;
        U4 timeouts = 0;
        U4 skipped = 0;
    };
    static constexpr int kFrameBytes = 32;

    explicit IDBinRingStat() : IDBin() {}

    ID id() override { return ID_RING_STAT; }
    Resp parsePayload(FrameParser &proto) override;
    void requestAll() override {}   // nothing on its own: read() when asked

    void read() { simpleRequest(v0); }
    void reset();
    const State& state() const { return m_state; }

private:
    State m_state;
};

/* A Recorder's connectors and their contacts (ID_BUS 0x17, ID_PIN 0x16; Recorder firmware feat/port-config, phase 1
   read-only). Contract: the recorder fork's docs/Port-Config-Spec.md.
     ID_BUS GETTING v0 [bus]   -> CONTENT v0: [bus][bus count][supply: 0 none, 1 VBAT, 2 +5.2 V][function caps]
                                  [function][function saved][UART caps][UART][UART saved][TX pin][RX pin][baud u32]
                                  [pin count][pin ids...][label length][label]
     ID_PIN GETTING v0 [pin]   -> CONTENT v0: [pin][bus][role caps u32][role][role saved][timer][param][value s32]
                                  [label length][label][MCU pin length][MCU pin]
     ID_PIN GETTING v1         -> CONTENT v1: [count] then count x [pin][role][value s32], every pin in id order
     The UART connectors (function caps 0x1E: buses 0..3, SONAR1, SONAR2, TELEM, GNSS), firmware with the settings:
     ID_BUS SETTING v0 [bus][function][its own UART] -> the function onto that connector, the one there onto the
                                  connector set free (a swap; each of the four functions on exactly one connector)
     A function's settings, addressed by the function and moving with it; GETTING [function] -> the same fields:
       v2 sonar A/B: [1|2][baud u32][protocol][log][to host]   v3 telemetry: [3][baud u32][to host]
       v4 navigation: [4][baud u32][protocol][rate Hz]         baud 0 = auto (the port searches)
     A SETTING answers RespOk; RespErrorPayload: a bad value; RespErrorRuntime: it changes the UART it came
     through (only USB changes everything). Kept over a restart only after ID_FLASH (DevDriver sends it).
     The baud a v2..v4 answer carries is the setting; the rate in use is the bus's baud in v0.
   Asked only of a Recorder, on request (DevDriver); nothing on a cold start. */
class IDBinBus : public IDBin
{
    Q_OBJECT
public:
    static constexpr U1 FunctionTelemetry = 3;

    struct Bus {
        U1 id = 0;
        U1 supply = 0;
        U1 functionCaps = 0;
        U1 function = 0;
        U1 functionSaved = 0;
        U1 uartCaps = 0;
        U1 uart = 0;
        U1 uartSaved = 0;
        U1 txPin = 0xFF;
        U1 rxPin = 0xFF;
        U4 baud = 0;
        QVector<U1> pins;
        QString label;
    };

    explicit IDBinBus() : IDBin() {}

    ID id() override { return ID_BUS; }
    Resp parsePayload(FrameParser &proto) override;
    void requestAll() override {}

    void request(U1 bus);
    int count() const { return m_count; }
    int lastBus() const { return m_last; }
    const QMap<int, Bus>& buses() const { return m_buses; }
    void reset() { m_buses.clear(); m_count = 0; m_last = -1; m_func = {}; }

    /// A UART function's settings (1 sonar A, 2 sonar B, 3 telemetry, 4 navigation) as the Recorder last reported
    /// them (v2..v4); not valid until an answer. The defaults are what a SETTING sends for fields never read.
    struct FunctionSettings {
        bool valid = false;
        U4 baud = 0;          // 0 = auto
        U1 protocol = 0;
        U1 log = 1;           // sonar: written to the log
        U1 toHost = 1;        // sonar: passed on to the host; telemetry: 0
        U1 rateHz = 0;        // navigation
    };
    /// The version a function's settings use: v2, v3, v4; v0 for anything else (no settings).
    static Version functionVersion(int function);
    const FunctionSettings& functionSettings(int function) const;
    void requestFunction(U1 function);
    /// SETTING of the function's version with this baud and its other fields as last read.
    void setFunctionBaud(U1 function, U4 baud);
    /// SETTING v0: the function onto the bus (a swap), with the bus's own UART.
    void setBusFunction(U1 bus, U1 function, U1 uart);
    /// The rates the firmware takes: 0 (auto), then 9600 .. 5000000.
    static const QVector<U4>& functionBauds();

private:
    QMap<int, Bus> m_buses;
    int m_count = 0;
    int m_last = -1;
    std::array<FunctionSettings, 5> m_func = {};   // by function, 0 unused
};

class IDBinPin : public IDBin
{
    Q_OBJECT
public:
    enum Role : U1 { RoleOff = 0, RoleUart = 1, RoleI2c = 2, RolePwm = 3, RoleDigitalIn = 4, RoleDigitalOut = 5,
                     RoleAnalogIn = 6 };
    struct Pin {
        U1 id = 0;
        U1 bus = 0;
        U4 roleCaps = 0;
        U1 role = 0;
        U1 roleSaved = 0;
        U1 timer = 0;
        U1 param = 0;
        S4 value = 0;
        QString label;
        QString mcuPin;
    };

    explicit IDBinPin() : IDBin() {}

    ID id() override { return ID_PIN; }
    Resp parsePayload(FrameParser &proto) override;
    void requestAll() override {}

    void request(U1 pin);       // v0: one pin's description
    void requestValues();       // v1: every pin's role and value
    int lastPin() const { return m_last; }
    const QMap<int, Pin>& pins() const { return m_pins; }
    void reset() { m_pins.clear(); m_last = -1; }

private:
    QMap<int, Pin> m_pins;
    int m_last = -1;            // the pin of the last v0 frame; -2 after a v1 frame
};


class IDBinNav : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinNav() : IDBin() {
    }

    ID id() override { return ID_NAV; }
    Resp  parsePayload(FrameParser &proto) override;

    struct SimpleNav {
        static constexpr ID getId() { return ID_NAV; }
        static constexpr Version getVer() { return v1; }

        float yaw = NAN;
        float pitch = NAN;
        float roll = NAN;
        double latitude = NAN;
        double longitude = NAN;
        float depth  = NAN;
    } __attribute__((packed));

    struct SimpleNavV2 {
        static constexpr ID getId() { return ID_NAV; }
        static constexpr Version getVer() { return v2; }

        uint8_t gnss_fix_type = 0;
        uint8_t numSats = 0;
        uint32_t unix_time = 0;
        int16_t unix_offset_ms = 0;
        int32_t latitude = 0;               // deg * 1e7
        int32_t longitude = 0;              // deg * 1e7
        int16_t ground_course_deg = 0;      // deg * 1e2
        int16_t ground_velocity_mps = 0;    // meters/s * 1e3
        int16_t yaw_deg = 0;                // deg * 1e2
        int16_t pitch_deg = 0;              // deg * 1e2
        int16_t roll_deg = 0;               // deg * 1e2
    } __attribute__((packed));

    double latitude() { return _nav.latitude; }
    double longitude() { return _nav.longitude; }

    float yaw() { return _nav.yaw; }
    float pitch() { return _nav.pitch; }
    float roll() { return _nav.roll; }

    float depth() { return _nav.depth; }

    uint8_t gnssFixTypeV2() const { return _navV2.gnss_fix_type; }
    uint8_t numSatsV2() const { return _navV2.numSats; }
    uint32_t unixTimeV2() const { return _navV2.unix_time; }
    int16_t unixOffsetMsV2() const { return _navV2.unix_offset_ms; }
    double latitudeV2() const { return static_cast<double>(_navV2.latitude) * 1e-7; }
    double longitudeV2() const { return static_cast<double>(_navV2.longitude) * 1e-7; }
    double groundCourseDegV2() const { return static_cast<double>(_navV2.ground_course_deg) * 0.01; }
    double groundVelocityMpsV2() const { return static_cast<double>(_navV2.ground_velocity_mps) * 0.001; }
    float yawV2() const { return static_cast<float>(static_cast<double>(_navV2.yaw_deg) * 0.01); }
    float pitchV2() const { return static_cast<float>(static_cast<double>(_navV2.pitch_deg) * 0.01); }
    float rollV2() const { return static_cast<float>(static_cast<double>(_navV2.roll_deg) * 0.01); }

protected:
    SimpleNav _nav;
    SimpleNavV2 _navV2;
};

class IDBinBoatStatus : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinBoatStatus() : IDBin() {
    }

    ID id() override { return ID_BOAT_STATUS; }
    Resp parsePayload(FrameParser& proto) override;

    struct BoatStatus {
        static constexpr ID getId() { return ID_BOAT_STATUS; }
        static constexpr Version getVer() { return v0; }

        uint8_t batteryBoat_percent = 0;
        uint8_t batteryBridge_percent = 0;
        uint8_t signal_quality_boat_percent = 0;
        uint8_t signal_quality_bridge_percent = 0;
    } __attribute__((packed));

    uint8_t batteryBoatPercent() const { return data_.batteryBoat_percent; }
    uint8_t batteryBridgePercent() const { return data_.batteryBridge_percent; }
    uint8_t signalQualityBoatPercent() const { return data_.signal_quality_boat_percent; }
    uint8_t signalQualityBridgePercent() const { return data_.signal_quality_bridge_percent; }

protected:
    BoatStatus data_{};
};

class IDBinRecorderStatus : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinRecorderStatus() : IDBin() {
    }

    ID id() override { return ID_RECORDER_STATUS; }
    Resp parsePayload(FrameParser& proto) override;

    struct RecorderStatus {
        static constexpr ID getId() { return ID_RECORDER_STATUS; }
        static constexpr Version getVer() { return v0; }

        union {
            struct {
                uint8_t device_condition : 3;
                uint8_t recording_mode   : 2;
                uint8_t recording_state  : 3;
            };
            uint8_t packed_status;
        };
        uint16_t status_flags;
        uint16_t warning_flags;
        uint16_t degraded_flags;
        uint16_t critical_flags;
        uint16_t uptime_10s;
        uint16_t current_log_id;
        uint16_t recorded_size_64k;
        uint16_t free_space_1m;
        uint16_t recording_duration_seconds;
        uint16_t seconds_since_last_write;
    } __attribute__((packed));

    bool     isValid() const { return valid_; }
    uint8_t  deviceCondition() const { return data_.device_condition; }
    uint8_t  recordingMode() const { return data_.recording_mode; }
    uint8_t  recordingState() const { return data_.recording_state; }
    uint16_t statusFlags() const { return data_.status_flags; }
    uint16_t warningFlags() const { return data_.warning_flags; }
    uint16_t degradedFlags() const { return data_.degraded_flags; }
    uint16_t criticalFlags() const { return data_.critical_flags; }
    uint16_t uptime10s() const { return data_.uptime_10s; }
    uint16_t currentLogId() const { return data_.current_log_id; }
    uint16_t recordedSize64k() const { return data_.recorded_size_64k; }
    uint16_t freeSpace1m() const { return data_.free_space_1m; }
    uint16_t recordingDurationSeconds() const { return data_.recording_duration_seconds; }
    uint16_t secondsSinceLastWrite() const { return data_.seconds_since_last_write; }

protected:
    RecorderStatus data_{};
    bool valid_ = false;
};

class IDBinNavSensorStatus : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinNavSensorStatus() : IDBin() {
    }

    ID id() override { return ID_NAV_SENSOR_STATUS; }
    Resp parsePayload(FrameParser& proto) override;

    enum State : uint8_t {
        StateOk          = 0,
        StateNotFound    = 1,
        StateBusError    = 2,
        StateNoData      = 3,
        StateStuck       = 4,
        StateImplausible = 5,
        StateRecovering  = 6,
    };

    enum Fault : uint16_t {
        FaultIdMismatch = 1u << 0,
        FaultBus        = 1u << 1,
        FaultConfigLost = 1u << 2,
        FaultLowRate    = 1u << 3,
        FaultInvalid    = 1u << 4,
        FaultStuck      = 1u << 5,
        FaultNorm       = 1u << 6,
        FaultTemp       = 1u << 7,
        FaultClip       = 1u << 8,
        FaultInjected   = 1u << 15,
    };

    struct NavSensorStatus {
        static constexpr ID getId() { return ID_NAV_SENSOR_STATUS; }
        static constexpr Version getVer() { return v0; }

        uint16_t seq;
        uint8_t  imu_state;
        uint8_t  imu_whoami;
        uint16_t imu_fault_flags;
        uint16_t imu_samples_per_s;
        uint16_t imu_ekf_fed_per_s;
        uint16_t imu_bus_err_cnt;
        uint16_t imu_invalid_cnt;
        uint16_t imu_stuck_run_max;
        uint16_t imu_acc_norm_mg;
        uint16_t imu_gyr_norm_cdps;
        int16_t  imu_temp_cc;
        uint8_t  imu_clip_mask;
        uint8_t  imu_reinit_cnt;
        uint8_t  imu_bad_seconds;
        uint8_t  mag_reserved[13];
    } __attribute__((packed));

    bool     isValid() const { return valid_; }
    void     invalidate() { valid_ = false; }
    uint16_t seq() const { return data_.seq; }
    uint8_t  imuState() const { return data_.imu_state; }
    uint8_t  imuWhoAmI() const { return data_.imu_whoami; }
    uint16_t imuFaultFlags() const { return data_.imu_fault_flags; }
    uint16_t imuSamplesPerS() const { return data_.imu_samples_per_s; }
    uint16_t imuEkfFedPerS() const { return data_.imu_ekf_fed_per_s; }
    uint16_t imuBusErrCnt() const { return data_.imu_bus_err_cnt; }
    uint16_t imuInvalidCnt() const { return data_.imu_invalid_cnt; }
    uint16_t imuStuckRunMax() const { return data_.imu_stuck_run_max; }
    uint16_t imuAccNormMg() const { return data_.imu_acc_norm_mg; }
    uint16_t imuGyrNormCdps() const { return data_.imu_gyr_norm_cdps; }
    int16_t  imuTempCc() const { return data_.imu_temp_cc; }
    uint8_t  imuClipMask() const { return data_.imu_clip_mask; }
    uint8_t  imuReinitCnt() const { return data_.imu_reinit_cnt; }
    uint8_t  imuBadSeconds() const { return data_.imu_bad_seconds; }

protected:
    NavSensorStatus data_{};
    bool valid_ = false;
};

class IDBinDVL : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinDVL() : IDBin() {
    }

    ID id() override { return ID_DVL_VEL; }
    Resp  parsePayload(FrameParser &proto) override;

    struct __attribute__((packed)) BeamSolution
    {
        uint8_t num;
        uint8_t flags;
        float velocity = NAN;
        float uncertainty = NAN;
        float dt = NAN;
        float distance = NAN;
        uint8_t amplitude;
        uint8_t mode;
        uint8_t coherence[4];
        int8_t difference[4];
    };

    typedef struct  __attribute__((packed)) {
        union {
            struct {
                uint32_t xValid : 1;
                uint32_t yValid : 1;
                uint32_t zValid : 1;
                uint32_t z1Valid : 1;
                uint32_t z2Valid : 1;
                uint32_t isBottomTrack : 1;
                uint32_t beam1Valid : 1;
                uint32_t beam2Valid : 1;
                uint32_t beam3Valid : 1;
                uint32_t beam4Valid : 1;
                uint32_t beam5Valid : 1;
                uint32_t beam6Valid : 1;
                uint32_t beamAvailNum : 3;
                uint32_t beamSolutionNum : 3;
            };
            uint32_t flags;
        };

        struct {
            uint32_t timestamp_ms;
            float deltaT;
            float latency;
        } solutionTime  __attribute__((packed));

        struct {
            float x;
            float y;
            float z;
            float z1;
            float z2;
        } velocity  __attribute__((packed));

        struct {
            float x;
            float y;
            float z;
            float z1;
            float z2;
        } uncertainty  __attribute__((packed));

        struct {
            float z;
            float z1;
            float z2;
        } distance  __attribute__((packed));
    } DVLSolution;

    float velX() { return vel_x; }
    float velY() { return vel_y; }
    float velZ() { return vel_z; }
    float dist() { return _dist; }

    BeamSolution* beams() { return _beams; }
    uint16_t beamsCount() { return _beamCount; }

    DVLSolution dvlSolution() {
        return _dvlSolution;
    }

protected:
    float vel_x, vel_y, vel_z;
    float _dist;

    BeamSolution _beams[4] = {};
    DVLSolution _dvlSolution;
    uint16_t _beamCount = 0;
    float test_bias = 0;
};

class IDBinDVLMode : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinDVLMode() : IDBin() {
    }

    ID id() override { return ID_DVL_MODE; }
    Resp  parsePayload(FrameParser &proto) override;

    struct __attribute__((packed)) DVLModeSetup
    {
        uint8_t id = 0;
        uint8_t selection = 1; // 0 - not select, 1 - always
        int8_t gain = 0;
        int8_t curve = 0;
        uint16_t reserved = 0;
        float start = 0, stop = 0; // 0 - ignore
    };

    void setModes(bool ismode1, bool ismode2, bool ismode3, bool ismode4, float range_mode4);

protected:

};



class IDBinUsblSolution : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinUsblSolution() : IDBin() {
    }

    ID id() override { return ID_USBL_SOLUTION; }
    Resp  parsePayload(FrameParser &proto) override;

    // ID_USBL_SOLUTION multiplexes unrelated payloads onto the same version:
    // v1 carries either AcousticNavSolution or BeaconActivationResponce, told
    // apart by payload length. Version alone cannot identify the content.
    enum class PayloadKind : uint8_t {
        None = 0,
        Solution,
        AcousticNav,
        BaseToBeacon,
        BeaconActivation
    };

    struct UsblSolution {
        enum Role : uint8_t { RoleNone = 0, RoleRequest = 1, RoleResponse = 2 };

        uint8_t id = 0xFF;
        uint8_t role = 0;
        uint8_t cmd_id = 0xFF;
        uint8_t reserved = 0;

        int64_t timestamp_us = 0;
        uint32_t ping_counter = 0;
        int64_t carrier_counter = 0;

        float distance_m = NAN;
        float distance_unc = 0;

        float azimuth_deg = NAN;
        float azimuth_unc = 0;

        float elevation_deg = NAN;
        float elevation_unc = 0;

        // NAN like its neighbours, and for the same reason: v1/v2 are PROJECTED onto this struct
        // and projectToSolution assigns no SNR, because neither AcousticNavSolution nor
        // BaseToBeacon carries one. A 0 default made that absence indistinguishable from a
        // measured 0 dB, and every consumer -- the node rows, the widget fields, the CSV export --
        // reported a confident zero for a number the payload never contained. Only v0 reads this
        // off the wire, where a genuine 0 stays a genuine 0.
        float snr = NAN;

        float beacon_x_m = NAN;
        float beacon_y_m = NAN;
        double beacon_latitude = NAN;
        double beacon_longitude = NAN;
        float beacon_depth = NAN;

        float usbl_yaw = NAN;
        float usbl_pitch = NAN;
        float usbl_roll = NAN;

        double usbl_latitude = NAN;
        double usbl_longitude = NAN;
        uint32_t last_iTOW = 0;

        float beacon_n_m = NAN;
        float beacon_e_m = NAN;
        // Tail-appended: absent from older firmware payloads, left NAN by the
        // short-payload path in FrameParser::read<T>().
        float sync_val = NAN;
        float sync_snr = NAN;
        float sync_first_val = NAN;
        float sync_first_snr = NAN;
        float sync_rms = NAN;
        float sync_first_main_max_msec = NAN;
        float reserved_array[2] = {NAN, NAN};
    } __attribute__((packed));

    struct AcousticNavSolution {
        static constexpr ID getId() { return ID_USBL_SOLUTION; }
        static constexpr Version getVer() { return v1; }

        uint8_t address = 0xFF;
        uint8_t cmd_id = 0xFF;

        uint32_t reserved = 0;

        int64_t timestamp_us = 0;
        int64_t carrier_us = 0;
        int64_t carrier_counter = 0;

        double lat = NAN;
        double lon = NAN;
        float depth = NAN;

        float acousticAzimuth = NAN;
        float geoAzimuth = NAN;
        float heading = NAN;

        float distance = NAN;

        double baseLat = NAN;
        double baseLon = NAN;
        float baseDepth = NAN;
    } __attribute__((packed));

    struct BaseToBeacon {
        static constexpr ID getId() { return ID_USBL_SOLUTION; }
        static constexpr Version getVer() { return v2; }

        uint8_t address = 0xFF;
        uint8_t cmd_id = 0xFF;

        uint32_t reserved = 0;

        int64_t timestamp_us = 0;
        int64_t carrier_us = 0;
        int64_t carrier_counter = 0;

        float acousticAzimuth = NAN;
        float geoAzimuth = NAN;
        float beaconDistance = NAN;
        float beaconN = NAN;
        float beaconE = NAN;
        float beaconD = NAN;
        double beaconLat = NAN;
        double beaconLon = NAN;

        float antennaYaw = NAN;
        float antennaDepth = NAN;
        double antennaLat = NAN;
        double antennaLon = NAN;
    } __attribute__((packed));

    struct USBLRequestBeacon {
        uint8_t id = 0; // 0 is promisc mode
        uint8_t reserved = 0;
        uint16_t watermark = 0;
        double latitude_deg = NAN;
        double longitude_deg = NAN;
        float external_heading_deg = NAN;
        float force_beacon_depth_m = NAN;
        float external_pitch = NAN;
        float external_roll = NAN;
    }  __attribute__((packed));

    struct BeaconActivationResponce {
        uint8_t id = 0; // 0 is promisc mode
        uint8_t reserved = 0;
        uint16_t reserved1 = 0;
    }  __attribute__((packed));

    struct BeaconActivate {
        float timeout_s = 2;
    }  __attribute__((packed));

    UsblSolution usblSolution() {
        return _usblSolution;
    }

    AcousticNavSolution acousticNavSolution() const { return _acousticNavSolution; }
    BaseToBeacon baseToBeacon() const { return _baseToBeacon; }
    PayloadKind lastPayloadKind() const { return _lastPayloadKind; }

    void askBeacon(USBLRequestBeacon ask);
    void enableBeaconOnce(float timeout);

protected:
    // v1/v2 payloads are projected onto _usblSolution so the 2D/3D views and the
    // epoch pool keep consuming a single solution type.
    void projectToSolution(const AcousticNavSolution& src);
    void projectToSolution(const BaseToBeacon& src);

    UsblSolution _usblSolution;
    AcousticNavSolution _acousticNavSolution;
    BaseToBeacon _baseToBeacon;
    BeaconActivationResponce _beaconResponcel;
    PayloadKind _lastPayloadKind = PayloadKind::None;
};
static_assert(sizeof(IDBinUsblSolution::UsblSolution) == 152);

class IDBinUsblDiag : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinUsblDiag() : IDBin() {
    }

    ID id() override { return ID_USBL_DIAG; }
    Resp parsePayload(FrameParser& proto) override;

    struct UsblChannelDiag {
        enum Flag : uint8_t {
            ChUsedForDirection = 1u << 0,
        };
        static constexpr int16_t PhaseNone = -32768;
        static constexpr int8_t  OffsetNone = -128;

        float   peak_amp = NAN;
        float   mf_noise_rms = NAN;
        float   in_rms = NAN;
        int16_t phase_cdeg = PhaseNone;
        int16_t phase_resid_cdeg = PhaseNone;
        int8_t  peak_offset = OffsetNone;
        uint8_t flags = 0;
    } __attribute__((packed));

    struct UsblSignalDiag {
        static constexpr ID getId() { return ID_USBL_DIAG; }
        static constexpr Version getVer() { return v0; }
        static constexpr int ChannelCapacity = 5;

        enum DirFlag : uint8_t {
            DirChannelsValid = 1u << 0,
            DirValid         = 1u << 1,
            DirSolverFailed  = 1u << 2,
        };

        enum Geometry : uint8_t {
            GeometryUnknown = 0,
            GeometryTetra4  = 1,
            GeometryCrossA  = 2,
            GeometryCrossB  = 3,
        };

        uint8_t  id = 0xFF;
        uint8_t  role = 0;
        uint8_t  cmd_id = 0xFF;
        uint8_t  ch_count = 0;
        int64_t  timestamp_us = 0;
        uint32_t ping_counter = 0;
        int64_t  carrier_counter = 0;

        float    distance_m = NAN;
        float    azimuth_deg = NAN;
        float    elevation_deg = NAN;
        float    sync_val = NAN;
        float    sync_snr = NAN;
        float    sync_first_val = NAN;
        float    sync_first_snr = NAN;
        float    sync_rms = NAN;
        float    sync_first_main_max_msec = NAN;

        float    dir_az_array_deg = NAN;
        float    dir_el_array_deg = NAN;
        uint16_t dir_flags = 0;
        uint8_t  geometry_id = GeometryUnknown;
        int8_t   peak_frac_offset = UsblChannelDiag::OffsetNone;

        float    fit_misfit = NAN;
        float    amp_nonuniformity = NAN;
        float    phase_nonplanarity = NAN;
        float    dir_width_az_deg = NAN;
        float    dir_width_el_deg = NAN;
        float    alt_margin_db = NAN;
        float    mirror_margin_db = NAN;
        float    array_coherence = NAN;

        UsblChannelDiag ch[ChannelCapacity];

        int channelCount() const { return ch_count < ChannelCapacity ? ch_count : ChannelCapacity; }
    } __attribute__((packed));

    bool isValid() const { return valid_; }
    void invalidate() { valid_ = false; }
    const UsblSignalDiag& diag() const { return data_; }

protected:
    UsblSignalDiag data_{};
    bool valid_ = false;
};
static_assert(sizeof(IDBinUsblDiag::UsblChannelDiag) == 18);
static_assert(sizeof(IDBinUsblDiag::UsblSignalDiag) == 194);

class IDBinUsblControl : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinUsblControl() : IDBin() {
    }

    ID id() override { return ID_USBL_CONTROL; }
    Resp parsePayload(FrameParser& proto) override;

    // Fire SIGNAL_SYNC_MASTER and SIGNAL_ADDRESS_MASTER (if enabled by "address") in a sequence
    // If (timeout_us > 0) then it will fire the signal sequence by trigger event,
    // else immediately (not precise time) by this CMD
    struct USBLPingRequest {
        static constexpr ID getId() { return ID_USBL_CONTROL; }
        static constexpr Version getVer() { return v1; }
        // 0: immediately by this CMD
        // 0xFFFFFFFF: trigger always enabled
        // otherwise: enable tx trigger for a certain time after setting the value

        uint32_t trigger_timeout_us = 0;
        // 1-8 are addresses, 0: promisc address, 0xFF: disabled address slot
        uint8_t address = 0;
        uint8_t cmd_id = 0;
        // 0: Pinger (no waiting for a response), >0: Interrogator (ranging)
        uint32_t reply_distance_mm = 20000;

        enum Function : uint8_t {
            FunctionDefault = 0,
            FunctionBitArray = 1,
            FunctionLLGeoAzimuth = 2
        };

        Function function = FunctionDefault;
        // non-zero only if (function == FunctionBitArray)
        // Payload bytes count is ((payload_bit_length + 7) / 8)
        uint16_t payload_bit_length = 0;
    } __attribute__((packed));

    // Regular addresses
    struct USBLPingAddresses {
        static constexpr ID getId() { return ID_USBL_CONTROL; }
        static constexpr Version getVer() { return v2; }

        uint8_t cur_position = 1; // 1-8, 0 is reserved
        uint8_t max_position = 1; // 1-8, 0 is reserved
        // 1-8 are addresses, 0: promisc address, 0xFF: disabled address slot
        uint8_t address[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    } __attribute__((packed));

    // Firmware calls this USBLTransponderEnable.
    struct USBLResponseTimeout {
        static constexpr ID getId() { return ID_USBL_CONTROL; }
        static constexpr Version getVer() { return v3; }
        // 0: silent mode, response disabled
        // 0xFFFFFFFF: response always enabled
        // otherwise: enable response for a certain time after setting the value
        uint32_t timeout_us = 0;
    } __attribute__((packed));

    struct USBLMonitorConfig {
        static constexpr ID getId() { return ID_USBL_CONTROL; }
        static constexpr Version getVer() { return v7; }

        uint32_t suppressSelfResponse_us = 0;
        uint32_t suppressSelfRequest_us = 0;
        bool receiveResponseInIdle = false;
    } __attribute__((packed));

    // Filter for incoming request addresses. Firmware calls this USBLRequestAddressFilter.
    struct USBLResponseAddressFilter {
        static constexpr ID getId() { return ID_USBL_CONTROL; }
        static constexpr Version getVer() { return v4; }
        // 0-7 are addresses, 0xFF: disabled address slot
        uint8_t address[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    } __attribute__((packed));

    // The one per-command-slot configuration. Firmware dispatches ID_USBL_CONTROL by
    // VERSION ALONE (a flat isContextVer chain, no length discrimination anywhere), so a
    // second struct at v6 is unreachable by construction — an earlier USBLCmdSlotConfig
    // here was silently parsed as this struct and rejected with RespErrorPayload.
    // One version, one struct.
    struct USBLCmdConfig  {
        static constexpr ID getId() { return ID_USBL_CONTROL; }
        static constexpr Version getVer() { return v6; }

        uint8_t cmd_id = 0;

        enum EventFilter : uint8_t {
            EventOnReceiveRequest = 1,
            EventOnReceiveResponse = 2
        } eventFilter = EventOnReceiveRequest;

        enum SendBackCmdIdAction : uint8_t {
            SendBackCmdIdIncoming = 0,
            SendBackCmdIdReplacement = 1
        } cmdIdAction = SendBackCmdIdIncoming;

        uint8_t cmd_id_replacement = 0;

        enum SendBackAddressAction : uint8_t {
            SendBackAddressIncoming = 0,
            SendBackAddressReplacement = 1,
        } addressAction = SendBackAddressIncoming;

        uint8_t address_replacement = 0;

        enum SendBackEventAction : uint8_t {
            SendBackEventSwaping = 0,
            SendBackEventSame = 1,
        } eventAction = SendBackEventSwaping;

        uint8_t reserved1 = 0;
        uint32_t reserved2 = 0;

        enum Function : uint8_t {
            FunctionDefault = 0,
            FunctionBitArray = 1,
            FunctionLLGeoAzimuth = 2
        };

        Function receiver_function = FunctionDefault;
        uint16_t receive_bit_length = 0;
        Function sender_function = FunctionDefault;
        uint16_t sending_bit_length = 0;
    } __attribute__((packed));

    // Every setter returns the checksum of the frame it sent, which the device echoes in its RESP.
    uint16_t pingRequest(uint32_t timeout_us, uint8_t address, uint8_t cmd_id = 0);
    // payload_bits < 0 means every byte of payload; otherwise at most that many bits of it.
    uint16_t pingRequest(uint32_t timeout_us, uint8_t address, uint8_t cmd_id, uint32_t reply_distance_mm, const QByteArray& payload = {}, int payload_bits = -1);
    uint16_t setTransponderEnable(uint32_t timeout_us);
    uint16_t setResponseTimeout(uint32_t timeout_us);
    uint16_t setMonitorConfig(const USBLMonitorConfig& cfg);
    uint16_t setResponseAddressFilter(const std::array<uint8_t, 8>& addresses);
    uint16_t setResponseAddressFilter(uint8_t address);
    // sending_bit_length is derived from the payload actually written, so a truncated
    // payload can never declare more bits than went on the wire. sending_bits < 0 means all of it.
    uint16_t setCmdConfig(const USBLCmdConfig& cfg, const QByteArray& sendingPayload = {}, int sending_bits = -1);

    // A relay (a logger port) hands the frame to the board with route 0, and the board echoes
    // the checksum of what it received: this is that checksum for the last frame sent.
    uint16_t lastRelayedChecksum() const { return lastRelayedChecksum_; }

protected:
    uint16_t send(ProtoBinOut& frame);
    uint16_t lastRelayedChecksum_ = 0;
};

// Receive-only: modem payloads carried back by an acoustic request/response.
// Transmission is configured through IDBinUsblControl's command slots.
class IDBinModemSolution : public IDBin
{
    Q_OBJECT
public:
    explicit IDBinModemSolution() : IDBin() {
    }

    ID id() override { return ID_MODEM_SOLUTION; }
    Resp  parsePayload(FrameParser &proto) override;

    struct ModemSolutionHeader {
        static constexpr ID getId() { return ID_MODEM_SOLUTION; }
        static constexpr Version getVer() { return v0; }

        int64_t timestamp_us = 0;
        int64_t carrier_us = 0;
        int64_t carrier_counter = 0;

        uint64_t reserved1 = 0;

        enum EventFilter : uint8_t {
            EventOnRequest = 1,
            EventOnResponse = 2,
        } event = EventOnRequest;

        uint8_t address_from = 0;
        uint8_t address_to = 0;
        uint8_t cmd_id_from = 0;

        uint16_t bit_length = 0;
        // payload bytes follow
    } __attribute__((packed));

    ModemSolutionHeader header() const { return _header; }
    QByteArray payload() const { return _payload; }

protected:
    ModemSolutionHeader _header;
    QByteArray _payload;
};


#endif // IDBINNARY_H
