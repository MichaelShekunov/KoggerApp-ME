#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QSerialPort>
#include <QString>
#include <QTimer>
#include <QVector>

#include <cstdint>

/**
 * Receiver of the Baikal-sonar USB tap.
 *
 * The sonar's second core owns a CDC serial port and streams diagnostic data
 * over it as KLF frames: the complex baseband of the streaming receiver (IQ)
 * or the raw ADC samples (RAW, three ADCs interleaved), both sent chunk by
 * chunk while the listening window runs, followed by the META of the ping. The frame layout is the contract published in the
 * firmware repository (Core1USB/firmware/tap_mbox.h): ID_TAP_META (0x78)
 * describes a ping, ID_TAP_IQ (0x79) carries 30 complex float32 samples,
 * ID_TAP_RAW (0x7A) carries up to 156 twelve-bit samples packed two in three
 * bytes, ID_TAP_STAT (0x7B) reports the streamer's counters once a second.
 *
 * Kept apart from the device stack on purpose: the port belongs to another
 * core, answers no requests and speaks only these frames, so it is found by
 * VID/PID, opened when the operator selects IQ or RAW in the echogram switch,
 * and every ping is appended to a .tapiq or .tapraw file in the log
 * directory. Incomplete IQ pings are dropped and counted; RAW windows are
 * kept as received, gaps and all, because the device itself may drop samples
 * when the channel cannot keep up, and it reports how many. Runs in the GUI
 * thread; a few hundred kilobytes per second is nothing.
 *
 * .tapiq: "TAPIQ1\0\0", u32 version = 1, u32 reserved; then per ping "PING",
 * u32 length, the 44-byte META payload as received, bb_len × float32 (I, Q).
 * .tapraw: "TAPRAW1\0", u32 version = 1, u32 reserved; then per window
 * "PING", u32 length, META (zeros if it never came), u32 total, u32 got,
 * u32 ngaps, ngaps × (u32 first, u32 count), total × uint16 samples
 * (s0 s1 s2 ..., missing ones zero).
 */
class UsbTap : public QObject {
    Q_OBJECT
    Q_PROPERTY(int     mode        READ mode        NOTIFY stateChanged)
    Q_PROPERTY(bool    connected   READ connected   NOTIFY stateChanged)
    Q_PROPERTY(QString portName    READ portName    NOTIFY stateChanged)
    Q_PROPERTY(int     pings       READ pings       NOTIFY stateChanged)
    Q_PROPERTY(int     incomplete  READ incomplete  NOTIFY stateChanged)
    Q_PROPERTY(QString filePath    READ filePath    NOTIFY stateChanged)
    Q_PROPERTY(QString statusText  READ statusText  NOTIFY stateChanged)

public:
    enum Mode { Off = 0, IQ = 1, Raw = 2 };

    explicit UsbTap(QObject* parent = nullptr);
    ~UsbTap() override;

    /** Selects what to capture; Off closes the port and the files. */
    Q_INVOKABLE void setMode(int mode, const QString& logDir);

    int     mode()       const { return mode_; }
    bool    connected()  const { return port_.isOpen(); }
    QString portName()   const { return port_.isOpen() ? port_.portName() : QString(); }
    int     pings()      const { return pings_ + rawWindows_; }
    int     incomplete() const { return incomplete_ + rawIncomplete_; }
    QString filePath()   const;
    QString statusText() const;

signals:
    void stateChanged();
    /** A complete IQ ping: sequence number, sample count, mean |I+jQ|. */
    void pingReceived(int seq, int samples, float meanMagnitude);
    /** A RAW window closed: sequence number, samples received, samples announced. */
    void rawWindowReceived(int seq, int got, int total);

private slots:
    void scan();
    void onReadyRead();
    void onError(QSerialPort::SerialPortError error);
    void closeRawWindow();
    void closeIqWindow();

private:
    struct Ping {
        QByteArray     meta;      // 44 bytes as received
        uint16_t       seq = 0;
        uint16_t       total = 0; // complex samples announced
        uint32_t       pingsCounter = 0;
        uint32_t       distMm = 0;
        uint32_t       stackFree = 0;
        QVector<float> i, q;
        QVector<bool>  have;
        int            got = 0;
        bool           valid = false;
    };
    struct RawWindow {
        uint16_t          seq = 0;
        uint32_t          total = 0;    // interleaved samples announced (the last frame is authoritative)
        QVector<uint16_t> data;
        QVector<bool>     have;
        int               got = 0;
        int               frames = 0;
        QByteArray        meta;         // empty until ID_TAP_META arrives
        bool              valid = false;
    };

    void parseFrames();
    void onFrame(uint8_t id, const QByteArray& payload);
    void onMeta(const QByteArray& payload);
    void onIq(const QByteArray& payload);
    void onRaw(const QByteArray& payload);
    void onStat(const QByteArray& payload);
    void finishPing();
    void resizeIq(uint16_t total);
    void finishRaw();
    void resizeRaw(uint32_t total);
    bool openFile(QFile& file, const char* suffix, const QByteArray& header);
    void closePort();
    void tickRate(int bytes);

    static const uint16_t kVendorId  = 0x600D;
    static const uint16_t kProductId = 0x1705;

    QSerialPort   port_;
    QTimer        scan_;
    QTimer        rawClose_;
    QTimer        iqClose_;
    QByteArray    rx_;
    QFile         fileIq_;
    QFile         fileRaw_;
    QString       logDir_;
    QElapsedTimer rateClock_;
    qint64        rateBytes_ = 0;
    double        kbps_ = 0.0;
    Ping          cur_;
    RawWindow     raw_;

    int      mode_ = Off;
    int      pings_ = 0;
    int      incomplete_ = 0;
    int      rawWindows_ = 0;
    int      rawIncomplete_ = 0;
    uint32_t lastStackFree_ = 0;
    uint32_t lastDistMm_ = 0;
    uint32_t devTorn_ = 0;
    uint32_t devSkipped_ = 0;
    uint32_t devFrames_ = 0;
    uint32_t devRawDropped_ = 0;
    uint32_t devIqDropped_ = 0;
    bool     statSeen_ = false;
};
