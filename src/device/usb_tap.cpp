#include "usb_tap.h"

#include <QDateTime>
#include <QDir>
#include <QSerialPortInfo>
#include <QtEndian>

#include <cmath>

namespace {

constexpr uint8_t kIdMeta = 0x78;
constexpr uint8_t kIdIq   = 0x79;
constexpr uint8_t kIdRaw  = 0x7A;
constexpr uint8_t kIdStat = 0x7B;

constexpr int kMetaLen   = 44;
constexpr int kIqHdrLen  = 8;
constexpr int kStatLen   = 29;
constexpr int kMaxSamples = 4096;   // the sonar's baseband buffer; anything bigger is noise

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
    connect(&port_, &QSerialPort::readyRead, this, &UsbTap::onReadyRead);
    connect(&port_, &QSerialPort::errorOccurred, this, &UsbTap::onError);
}

UsbTap::~UsbTap() {
    closePort();
    if (file_.isOpen()) file_.close();
}

void UsbTap::setMode(int mode, const QString& logDir) {
    if (mode < Off || mode > Raw) mode = Off;
    logDir_ = logDir;
    if (mode == mode_) return;
    mode_ = mode;
    if (mode_ == Off) {
        scan_.stop();
        closePort();
        if (file_.isOpen()) file_.close();
    } else {
        // A new capture starts a new file; the counters describe that capture.
        if (file_.isOpen()) file_.close();
        pings_ = 0;
        incomplete_ = 0;
        cur_ = Ping();
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
        if (!port_.open(QIODevice::ReadWrite)) {
            port_.clearError();
            continue;
        }
        port_.setDataTerminalReady(true);
        rx_.clear();
        cur_ = Ping();
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
    case kIdStat: onStat(payload); break;
    case kIdRaw:  break;   // firmware step 2: not emitted yet
    default:      break;
    }
}

void UsbTap::onMeta(const QByteArray& p) {
    if (p.size() < kMetaLen) return;
    if (cur_.valid && cur_.got < cur_.total) {
        ++incomplete_;   // the device tore the previous ping or frames were lost
    }
    cur_ = Ping();
    cur_.meta = p.left(kMetaLen);
    cur_.seq = u16(p, 0);
    cur_.total = u16(p, 22);
    cur_.pingsCounter = u32(p, 32);
    cur_.distMm = u32(p, 28);
    cur_.stackFree = u32(p, 36);
    if (cur_.total == 0 || cur_.total > kMaxSamples) { cur_ = Ping(); return; }
    cur_.i.fill(0.0f, cur_.total);
    cur_.q.fill(0.0f, cur_.total);
    cur_.have.fill(false, cur_.total);
    cur_.valid = true;
    lastStackFree_ = cur_.stackFree;
    lastDistMm_ = cur_.distMm;
}

void UsbTap::onIq(const QByteArray& p) {
    if (!cur_.valid || p.size() < kIqHdrLen) return;
    const uint16_t seq = u16(p, 0);
    const uint16_t first = u16(p, 2);
    const int count = uchar(p[4]);
    const int fmt = uchar(p[5]);
    if (seq != cur_.seq || fmt != 1 || p.size() < kIqHdrLen + 8 * count) return;
    for (int k = 0; k < count; ++k) {
        const int idx = first + k;
        if (idx >= cur_.total || cur_.have[idx]) continue;
        cur_.i[idx] = f32(p, kIqHdrLen + 8 * k);
        cur_.q[idx] = f32(p, kIqHdrLen + 8 * k + 4);
        cur_.have[idx] = true;
        ++cur_.got;
    }
    if (cur_.got == cur_.total) finishPing();
}

void UsbTap::onStat(const QByteArray& p) {
    if (p.size() < kStatLen) return;
    devFrames_ = u32(p, 8);
    devTorn_ = u32(p, 12);
    devSkipped_ = u32(p, 16);
    lastStackFree_ = u32(p, 24);
    statSeen_ = true;
    emit stateChanged();
}

bool UsbTap::openFile() {
    if (file_.isOpen()) return true;
    QDir dir(logDir_.isEmpty() ? QDir::currentPath() : logDir_);
    if (!dir.exists() && !dir.mkpath(".")) return false;
    const QString name = QStringLiteral("tap_%1.tapiq")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    file_.setFileName(dir.filePath(name));
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QByteArray hdr("TAPIQ1\0\0", 8);
    uint32_t words[2] = { qToLittleEndian<uint32_t>(1), 0 };
    hdr.append(reinterpret_cast<const char*>(words), sizeof words);
    file_.write(hdr);
    return true;
}

void UsbTap::finishPing() {
    if (!cur_.valid) return;
    double acc = 0.0;
    for (int k = 0; k < cur_.total; ++k) acc += std::sqrt(double(cur_.i[k]) * cur_.i[k] + double(cur_.q[k]) * cur_.q[k]);
    const float mean = cur_.total ? float(acc / cur_.total) : 0.0f;

    if (openFile()) {
        QByteArray body = cur_.meta;
        body.reserve(kMetaLen + 8 * cur_.total);
        for (int k = 0; k < cur_.total; ++k) {
            float v[2] = { cur_.i[k], cur_.q[k] };
            body.append(reinterpret_cast<const char*>(v), sizeof v);   // host is little-endian, like the wire
        }
        const uint32_t len = qToLittleEndian<uint32_t>(uint32_t(body.size()));
        file_.write("PING", 4);
        file_.write(reinterpret_cast<const char*>(&len), 4);
        file_.write(body);
        if ((pings_ % 8) == 0) file_.flush();
    }
    ++pings_;
    emit pingReceived(cur_.seq, cur_.total, mean);
    cur_ = Ping();
    emit stateChanged();
}

QString UsbTap::statusText() const {
    if (mode_ == Off) return QString();
    if (!port_.isOpen()) {
        return tr("USB tap port (VID 600D) not found: is the second core running?");
    }
    QString s = tr("%1: %2 pings, %3 KB/s, %4 incomplete")
                    .arg(port_.portName()).arg(pings_).arg(kbps_, 0, 'f', 1).arg(incomplete_);
    if (statSeen_) {
        s += tr("; device: torn %1, skipped %2").arg(devTorn_).arg(devSkipped_);
    }
    if (lastStackFree_) {
        s += tr("; core 0 stack free %1 B").arg(lastStackFree_);
    }
    if (mode_ == Raw) {
        s += tr("; RAW frames are not emitted by this firmware yet");
    }
    if (file_.isOpen()) {
        s += QStringLiteral("\n") + QDir::toNativeSeparators(file_.fileName());
    }
    return s;
}
