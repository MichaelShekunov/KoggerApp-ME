#include "usb_tap.h"

#include <QDateTime>
#include <QDir>
#include <QSerialPortInfo>
#include <QtEndian>

#include <cmath>
#include <cstring>

namespace {

constexpr uint8_t kIdMeta = 0x78;
constexpr uint8_t kIdIq   = 0x79;
constexpr uint8_t kIdRaw  = 0x7A;
constexpr uint8_t kIdStat = 0x7B;

constexpr int kMetaLen     = 44;
constexpr int kIqHdrLen    = 8;
constexpr int kRawHdrLen   = 12;
constexpr int kStatLen     = 29;          // 33 with the raw-drop counter
constexpr int kMaxSamples  = 65535;       // u16 on the wire; a 5000-cell window at 10 mm is about 20 500 samples
constexpr uint32_t kMaxRaw = 1u << 24;    // raw window: 16M samples is ~6 s of ADC, far beyond any range

constexpr uint16_t kFlagIq  = 1;
constexpr uint16_t kFlagRaw = 2;

constexpr uint8_t kFmtU16   = 1;
constexpr uint8_t kFmtU12P  = 2;

uint16_t u16(const QByteArray& b, int at) { return qFromLittleEndian<uint16_t>(reinterpret_cast<const uchar*>(b.constData() + at)); }
uint32_t u32(const QByteArray& b, int at) { return qFromLittleEndian<uint32_t>(reinterpret_cast<const uchar*>(b.constData() + at)); }
float    f32(const QByteArray& b, int at) {
    const uint32_t raw = u32(b, at);
    float v;
    memcpy(&v, &raw, sizeof v);
    return v;
}

} // namespace

UsbTap::UsbTap(QObject* parent) : QObject(parent) {
    scan_.setInterval(1000);
    connect(&scan_, &QTimer::timeout, this, &UsbTap::scan);
    rawClose_.setSingleShot(true);
    iqClose_.setSingleShot(true);
    rawClose_.setInterval(250);
    iqClose_.setInterval(250);
    connect(&rawClose_, &QTimer::timeout, this, &UsbTap::closeRawWindow);
    connect(&iqClose_, &QTimer::timeout, this, &UsbTap::closeIqWindow);
    connect(&port_, &QSerialPort::readyRead, this, &UsbTap::onReadyRead);
    connect(&port_, &QSerialPort::errorOccurred, this, &UsbTap::onError);
}

UsbTap::~UsbTap() {
    closePort();
    if (fileIq_.isOpen()) fileIq_.close();
    if (fileRaw_.isOpen()) fileRaw_.close();
}

QString UsbTap::filePath() const {
    if (fileRaw_.isOpen()) return fileRaw_.fileName();
    if (fileIq_.isOpen()) return fileIq_.fileName();
    return QString();
}

void UsbTap::setMode(int mode, const QString& logDir) {
    if (mode < Off || mode > Raw) mode = Off;
    logDir_ = logDir;
    if (mode == mode_) return;
    mode_ = mode;
    if (fileIq_.isOpen()) fileIq_.close();
    if (fileRaw_.isOpen()) fileRaw_.close();
    if (mode_ == Off) {
        scan_.stop();
        closePort();
    } else {
        // A new capture starts new files; the counters describe that capture.
        pings_ = incomplete_ = 0;
        rawWindows_ = rawIncomplete_ = 0;
        cur_ = Ping();
        raw_ = RawWindow();
        rateBytes_ = 0;
        kbps_ = 0.0;
        rateClock_.start();
        scan();
        scan_.start();
    }
    emit stateChanged();
}

void UsbTap::scan() {
    if (mode_ == Off || port_.isOpen()) return;
    for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
        if (!info.hasVendorIdentifier() || !info.hasProductIdentifier()) continue;
        if (info.vendorIdentifier() != kVendorId || info.productIdentifier() != kProductId) continue;
        port_.setPort(info);
        port_.setBaudRate(QSerialPort::Baud115200);   // CDC ignores it; the streamer waits for DTR
        port_.setReadBufferSize(0);                    // unlimited: RAW bursts at ~4 MB/s
        if (!port_.open(QIODevice::ReadWrite)) {
            port_.clearError();
            continue;
        }
        port_.setDataTerminalReady(true);
        rx_.clear();
        cur_ = Ping();
        raw_ = RawWindow();
        emit stateChanged();
        return;
    }
}

void UsbTap::closePort() {
    if (port_.isOpen()) {
        port_.setDataTerminalReady(false);
        port_.close();
    }
    rx_.clear();
}

void UsbTap::onError(QSerialPort::SerialPortError error) {
    if (error == QSerialPort::NoError) return;
    // Unplugged or the second core restarted: drop the port, the scan timer will find it again.
    if (error == QSerialPort::ResourceError || error == QSerialPort::DeviceNotFoundError
        || error == QSerialPort::PermissionError) {
        closePort();
        port_.clearError();
        emit stateChanged();
    }
}

void UsbTap::onReadyRead() {
    const QByteArray chunk = port_.readAll();
    if (chunk.isEmpty()) return;
    rx_ += chunk;
    tickRate(chunk.size());
    parseFrames();
    if (rx_.size() > 65536) rx_ = rx_.right(1024);   // no sync for 64 KB: not our stream
}

void UsbTap::tickRate(int bytes) {
    rateBytes_ += bytes;
    const qint64 ms = rateClock_.elapsed();
    if (ms >= 2000) {
        kbps_ = double(rateBytes_) / 1024.0 * 1000.0 / double(ms);
        rateBytes_ = 0;
        rateClock_.restart();
        emit stateChanged();
    }
}

/** KLF: BB 55 route mode id len payload c1 c2, Fletcher-8 over route..payload. */
void UsbTap::parseFrames() {
    int i = 0;
    const int n = rx_.size();
    const uchar* d = reinterpret_cast<const uchar*>(rx_.constData());
    while (i + 8 <= n) {
        if (d[i] != 0xBB || d[i + 1] != 0x55) { ++i; continue; }
        const int len = d[i + 5];
        if (i + 8 + len > n) break;
        uint8_t c1 = 0, c2 = 0;
        for (int k = i + 2; k < i + 6 + len; ++k) { c1 = uint8_t(c1 + d[k]); c2 = uint8_t(c2 + c1); }
        if (d[i + 6 + len] != c1 || d[i + 7 + len] != c2) { ++i; continue; }
        onFrame(d[i + 4], rx_.mid(i + 6, len));
        i += 8 + len;
    }
    rx_.remove(0, i);
}

void UsbTap::onFrame(uint8_t id, const QByteArray& payload) {
    switch (id) {
    case kIdMeta: onMeta(payload); break;
    case kIdIq:   onIq(payload);   break;
    case kIdRaw:  onRaw(payload);  break;
    case kIdStat: onStat(payload); break;
    default:      break;
    }
}

void UsbTap::onMeta(const QByteArray& p) {
    if (p.size() < kMetaLen) return;
    const uint16_t seq = u16(p, 0);
    const uint16_t flags = u16(p, 42);
    lastStackFree_ = u32(p, 36);
    lastDistMm_ = u32(p, 28);

    // META closes an IQ window: the device streams the chunks while the
    // listening window runs and sends META after the last one. A META without
    // chunks (the port opened mid-window) still describes the ping; it is then
    // closed as incomplete by the timer or by the next window.
    if (flags & kFlagIq) {
        if (!cur_.valid || cur_.seq != seq) {
            if (cur_.valid) finishPing();              // the previous window never got its META
            cur_ = Ping();
            cur_.seq = seq;
            cur_.valid = true;
            resizeIq(u16(p, 22));
        }
        cur_.meta = p.left(kMetaLen);
        cur_.pingsCounter = u32(p, 32);
        cur_.distMm = u32(p, 28);
        cur_.stackFree = u32(p, 36);
        if (cur_.total > 0 && cur_.got >= cur_.total) finishPing();
        else iqClose_.start();
    }

    // Same for a RAW window.
    if ((flags & kFlagRaw) && raw_.valid && raw_.seq == seq) {
        raw_.meta = p.left(kMetaLen);
        if (raw_.got >= int(raw_.total)) finishRaw();
        else rawClose_.start();
    }
}

void UsbTap::onIq(const QByteArray& p) {
    if (p.size() < kIqHdrLen) return;
    const uint16_t seq = u16(p, 0);
    const uint16_t first = u16(p, 2);
    const int count = uchar(p[4]);
    const int fmt = uchar(p[5]);
    const uint16_t total = u16(p, 6);
    if (fmt != 1 || total == 0 || total > kMaxSamples || p.size() < kIqHdrLen + 8 * count) return;
    if (!cur_.valid || cur_.seq != seq) {
        if (cur_.valid) finishPing();                  // the previous window never got its META
        cur_ = Ping();
        cur_.seq = seq;
        cur_.valid = true;
    }
    resizeIq(total);                                    // the sonar refines the length when the window ends
    for (int k = 0; k < count; ++k) {
        const int idx = first + k;
        if (idx >= cur_.total || cur_.have[idx]) continue;
        cur_.i[idx] = f32(p, kIqHdrLen + 8 * k);
        cur_.q[idx] = f32(p, kIqHdrLen + 8 * k + 4);
        cur_.have[idx] = true;
        ++cur_.got;
    }
    if (!cur_.meta.isEmpty() && cur_.got >= cur_.total) finishPing();
}

void UsbTap::resizeIq(uint16_t total) {
    if (total == cur_.total || total == 0 || total > kMaxSamples) return;
    cur_.i.resize(total);
    cur_.q.resize(total);
    cur_.have.resize(total);
    cur_.total = total;
    cur_.got = 0;
    for (bool h : cur_.have) if (h) ++cur_.got;
}

void UsbTap::closeIqWindow() {
    if (cur_.valid) finishPing();
}

void UsbTap::resizeRaw(uint32_t total) {
    // The sonar refines the window length when the window ends; the last frame is right.
    if (total == raw_.total || total == 0 || total > kMaxRaw) return;
    raw_.data.resize(int(total));
    raw_.have.resize(int(total));
    raw_.total = total;
    raw_.got = 0;
    for (bool h : raw_.have) if (h) ++raw_.got;
}

void UsbTap::onRaw(const QByteArray& p) {
    if (p.size() < kRawHdrLen) return;
    const uint16_t seq = u16(p, 0);
    const uint32_t first = u32(p, 2);
    const uint32_t total = u32(p, 6);
    const int n = uchar(p[10]);
    const int fmt = uchar(p[11]);
    if (total == 0 || total > kMaxRaw || n == 0) return;

    QVector<uint16_t> s(n);
    if (fmt == kFmtU16) {
        if (p.size() < kRawHdrLen + 2 * n) return;
        for (int k = 0; k < n; ++k) s[k] = u16(p, kRawHdrLen + 2 * k);
    } else if (fmt == kFmtU12P) {
        if (p.size() < kRawHdrLen + (3 * n + 1) / 2) return;
        const uchar* d = reinterpret_cast<const uchar*>(p.constData()) + kRawHdrLen;
        int k = 0;
        for (int i = 0; i + 1 < n; i += 2, k += 3) {
            s[i]     = uint16_t(d[k] | ((d[k + 1] & 0x0F) << 8));
            s[i + 1] = uint16_t((d[k + 1] >> 4) | (d[k + 2] << 4));
        }
        if (n & 1) s[n - 1] = uint16_t(d[k] | ((d[k + 1] & 0x0F) << 8));
    } else {
        return;
    }

    if (!raw_.valid || raw_.seq != seq) {
        if (raw_.valid) finishRaw();          // the previous window never got its META
        raw_ = RawWindow();
        raw_.seq = seq;
        raw_.total = total;
        raw_.data.fill(0, int(total));
        raw_.have.fill(false, int(total));
        raw_.valid = true;
    } else {
        resizeRaw(total);
    }
    for (int k = 0; k < n; ++k) {
        const uint32_t idx = first + uint32_t(k);
        if (idx >= raw_.total || raw_.have[int(idx)]) continue;
        raw_.data[int(idx)] = s[k];
        raw_.have[int(idx)] = true;
        ++raw_.got;
    }
    ++raw_.frames;
    if (!raw_.meta.isEmpty() && raw_.got >= int(raw_.total)) finishRaw();
}

void UsbTap::closeRawWindow() {
    if (raw_.valid && !raw_.meta.isEmpty()) finishRaw();
}

void UsbTap::onStat(const QByteArray& p) {
    if (p.size() < kStatLen) return;
    devFrames_ = u32(p, 8);
    devTorn_ = u32(p, 12);
    devSkipped_ = u32(p, 16);
    lastStackFree_ = u32(p, 24);
    if (p.size() >= 33) devRawDropped_ = u32(p, 29);
    if (p.size() >= 37) devIqDropped_ = u32(p, 33);
    statSeen_ = true;
    emit stateChanged();
}

bool UsbTap::openFile(QFile& file, const char* suffix, const QByteArray& header) {
    if (file.isOpen()) return true;
    QDir dir(logDir_.isEmpty() ? QDir::currentPath() : logDir_);
    if (!dir.exists() && !dir.mkpath(".")) return false;
    const QString name = QStringLiteral("tap_%1.%2")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")))
                             .arg(QLatin1String(suffix));
    file.setFileName(dir.filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QByteArray hdr = header;
    uint32_t words[2] = { qToLittleEndian<uint32_t>(1), 0 };
    hdr.append(reinterpret_cast<const char*>(words), sizeof words);
    file.write(hdr);
    return true;
}

void UsbTap::finishPing() {
    if (!cur_.valid) return;
    iqClose_.stop();
    if (cur_.meta.isEmpty() || cur_.total == 0 || cur_.got < cur_.total) {
        ++incomplete_;                                  // torn on the device, lost in transit, or no META
        cur_ = Ping();
        emit stateChanged();
        return;
    }
    double acc = 0.0;
    for (int k = 0; k < cur_.total; ++k) acc += std::sqrt(double(cur_.i[k]) * cur_.i[k] + double(cur_.q[k]) * cur_.q[k]);
    const float mean = cur_.total ? float(acc / cur_.total) : 0.0f;

    if (openFile(fileIq_, "tapiq", QByteArray("TAPIQ1\0\0", 8))) {
        QByteArray body = cur_.meta;
        body.reserve(kMetaLen + 8 * cur_.total);
        for (int k = 0; k < cur_.total; ++k) {
            float v[2] = { cur_.i[k], cur_.q[k] };
            body.append(reinterpret_cast<const char*>(v), sizeof v);   // host is little-endian, like the wire
        }
        const uint32_t len = qToLittleEndian<uint32_t>(uint32_t(body.size()));
        fileIq_.write("PING", 4);
        fileIq_.write(reinterpret_cast<const char*>(&len), 4);
        fileIq_.write(body);
        if ((pings_ % 8) == 0) fileIq_.flush();
    }
    ++pings_;
    emit pingReceived(cur_.seq, cur_.total, mean);
    cur_ = Ping();
    emit stateChanged();
}

void UsbTap::finishRaw() {
    if (!raw_.valid) return;
    rawClose_.stop();
    const int total = int(raw_.total);
    QVector<QPair<uint32_t, uint32_t>> gaps;
    for (int k = 0; k < total;) {
        if (raw_.have[k]) { ++k; continue; }
        int j = k;
        while (j < total && !raw_.have[j]) ++j;
        gaps.append(qMakePair(uint32_t(k), uint32_t(j - k)));
        k = j;
    }
    if (openFile(fileRaw_, "tapraw", QByteArray("TAPRAW1\0", 8))) {
        QByteArray body = raw_.meta.isEmpty() ? QByteArray(kMetaLen, '\0') : raw_.meta;
        body.reserve(kMetaLen + 12 + 8 * gaps.size() + 2 * total);
        uint32_t head[3] = { qToLittleEndian<uint32_t>(raw_.total), qToLittleEndian<uint32_t>(uint32_t(raw_.got)),
                             qToLittleEndian<uint32_t>(uint32_t(gaps.size())) };
        body.append(reinterpret_cast<const char*>(head), sizeof head);
        for (const auto& g : gaps) {
            uint32_t pair[2] = { qToLittleEndian<uint32_t>(g.first), qToLittleEndian<uint32_t>(g.second) };
            body.append(reinterpret_cast<const char*>(pair), sizeof pair);
        }
        body.append(reinterpret_cast<const char*>(raw_.data.constData()), 2 * total);   // little-endian host
        const uint32_t len = qToLittleEndian<uint32_t>(uint32_t(body.size()));
        fileRaw_.write("PING", 4);
        fileRaw_.write(reinterpret_cast<const char*>(&len), 4);
        fileRaw_.write(body);
        fileRaw_.flush();
    }
    ++rawWindows_;
    if (raw_.got < total) ++rawIncomplete_;
    emit rawWindowReceived(raw_.seq, raw_.got, total);
    raw_ = RawWindow();
    emit stateChanged();
}

QString UsbTap::statusText() const {
    if (mode_ == Off) return QString();
    if (!port_.isOpen()) {
        return tr("USB tap port (VID 600D) not found: is the second core running?");
    }
    QString s;
    if (mode_ == Raw) {
        s = tr("%1: %2 RAW windows, %3 with gaps, %4 KB/s")
                .arg(port_.portName()).arg(rawWindows_).arg(rawIncomplete_).arg(kbps_, 0, 'f', 1);
        if (statSeen_) s += tr("; device dropped %1 samples").arg(devRawDropped_);
    } else {
        s = tr("%1: %2 pings, %3 KB/s, %4 incomplete")
                .arg(port_.portName()).arg(pings_).arg(kbps_, 0, 'f', 1).arg(incomplete_);
        if (statSeen_) s += tr("; device: torn %1, skipped %2, dropped %3 samples").arg(devTorn_).arg(devSkipped_).arg(devIqDropped_);
    }
    if (lastStackFree_) s += tr("; core 0 stack free %1 B").arg(lastStackFree_);
    const QString path = filePath();
    if (!path.isEmpty()) s += QStringLiteral("\n") + QDir::toNativeSeparators(path);
    return s;
}
