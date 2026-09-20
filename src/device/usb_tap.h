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
 * The sonar's second core streams the complex baseband of the two-pass
 * receiver (IQ) — and, in a later step, raw ADC samples — as KLF frames over
 * its own CDC serial port. The frame layout is the contract published in the
 * firmware repository (Core1USB/firmware/tap_mbox.h): ID_TAP_META (0x78)
 * describes a ping, ID_TAP_IQ (0x79) carries 30 complex float32 samples per
 * frame, ID_TAP_STAT (0x7B) reports the streamer's counters once a second.
 *
 * Kept apart from the device stack on purpose: the port belongs to another
 * core, answers no requests and speaks only these frames, so it is found by
 * VID/PID, opened when the operator selects IQ or RAW in the echogram switch,
 * and every complete ping is appended to a .tapiq file in the log directory.
 * Incomplete pings (torn on the device or lost in transit) are counted and
 * dropped. Runs in the GUI thread: tens of kilobytes per second is nothing.
 *
 * .tapiq layout, little-endian: "TAPIQ1\0\0", u32 version = 1, u32 reserved;
 * then per ping: "PING", u32 length, the 44-byte META payload as received,
 * bb_len pairs of float32 (I, Q).
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

    /** Selects what to capture; Off closes the port and the file. */
    Q_INVOKABLE void setMode(int mode, const QString& logDir);

    int     mode()       const { return mode_; }
    bool    connected()  const { return port_.isOpen(); }
    QString portName()   const { return port_.isOpen() ? port_.portName() : QString(); }
    int     pings()      const { return pings_; }
    int     incomplete() const { return incomplete_; }
    QString filePath()   const { return file_.isOpen() ? file_.fileName() : QString(); }
    QString statusText() const;

signals:
    void stateChanged();
    /** A complete ping: sequence number, sample count, mean |I+jQ|. */
    void pingReceived(int seq, int samples, float meanMagnitude);

private slots:
    void scan();
    void onReadyRead();
    void onError(QSerialPort::SerialPortError error);

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

    void parseFrames();
    void onFrame(uint8_t id, const QByteArray& payload);
    void onMeta(const QByteArray& payload);
    void onIq(const QByteArray& payload);
    void onStat(const QByteArray& payload);
    void finishPing();
    bool openFile();
    void closePort();
    void tickRate(int bytes);

    static const uint16_t kVendorId  = 0x600D;
    static const uint16_t kProductId = 0x1705;

    QSerialPort   port_;
    QTimer        scan_;
    QByteArray    rx_;
    QFile         file_;
    QString       logDir_;
    QElapsedTimer rateClock_;
    qint64        rateBytes_ = 0;
    double        kbps_ = 0.0;
    Ping          cur_;

    int      mode_ = Off;
    int      pings_ = 0;
    int      incomplete_ = 0;
    uint32_t lastStackFree_ = 0;
    uint32_t lastDistMm_ = 0;
    uint32_t devTorn_ = 0;
    uint32_t devSkipped_ = 0;
    uint32_t devFrames_ = 0;
    bool     statSeen_ = false;
};
