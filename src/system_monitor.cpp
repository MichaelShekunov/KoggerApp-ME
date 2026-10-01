#include "system_monitor.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QStorageInfo>

#include <algorithm>
#include <climits>
#include <cmath>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace {

#if defined(Q_OS_LINUX) || defined(Q_OS_ANDROID)
QString readSmall(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromLatin1(f.read(65536));
}

long long readNumber(const QString& path, long long fallback = -1)
{
    bool ok = false;
    const long long v = readSmall(path).trimmed().toLongLong(&ok);
    return ok ? v : fallback;
}
#endif

#if defined(Q_OS_WIN)
// ntdll's SystemProcessorPerformanceInformation (8) and powrprof's ProcessorInformation (11), resolved at run time.
struct ProcessorTimes {
    LARGE_INTEGER idle;
    LARGE_INTEGER kernel;   // with the idle time in it
    LARGE_INTEGER user;
    LARGE_INTEGER dpc;
    LARGE_INTEGER interrupt;
    ULONG interruptCount;
};
struct ProcessorPower {
    ULONG number;
    ULONG maxMhz;
    ULONG currentMhz;
    ULONG mhzLimit;
    ULONG maxIdleState;
    ULONG currentIdleState;
};
using NtQuerySystemInformationFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
using CallNtPowerInformationFn = LONG(WINAPI*)(int, PVOID, ULONG, PVOID, ULONG);

int processorCount()
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return int(si.dwNumberOfProcessors);
}
#endif

QString gb(double v) { return QString::number(v, 'f', 1); }

} // namespace

SystemMonitor::SystemMonitor(QObject* parent)
    : QObject(parent)
{
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &SystemMonitor::refresh);
    // One look a few seconds after the start (two readings, for the loads), so the log line is there with no widget
    // showing the values.
    QTimer::singleShot(3000, this, [this] {
        subscribe();
        QTimer::singleShot(1500, this, [this] { unsubscribe(); });
    });
}

void SystemMonitor::subscribe()
{
    if (subscribers_++ == 0) {
        ticks_ = 0;
        refresh();
        timer_.start();
    }
}

void SystemMonitor::unsubscribe()
{
    if (subscribers_ > 0 && --subscribers_ == 0)
        timer_.stop();
}

void SystemMonitor::refresh()
{
    readCpu();
    readMemory();
    readThermal();
    if (ticks_ % 10 == 0)
        readVolumes();
    readUptime();
    ++ticks_;
    available_ = !coreLoad_.isEmpty() || memTotal_ > 0.0;
    if (available_ && !logged_ && ticks_ >= 2) {   // the second tick: loads need two samples
        logged_ = true;
        QStringList cores;   // the maxima as runs: "1800 x4, 2400 x4"
        for (int i = 0; i < coreMax_.size();) {
            int j = i;
            while (j < coreMax_.size() && coreMax_[j] == coreMax_[i])
                ++j;
            cores << QStringLiteral("%1 x%2").arg(coreMax_[i]).arg(j - i);
            i = j;
        }
        QStringList vols;
        for (const QVariant& v : volumes_) {
            const QVariantMap m = v.toMap();
            const QString kind = m.value(QStringLiteral("kind")).toString();
            const int wear = m.value(QStringLiteral("wearPct")).toInt();
            QString disk = kind;
            if (wear >= 0)
                disk += (disk.isEmpty() ? QString() : QStringLiteral(", ")) + QStringLiteral("wear <=%1 %").arg(wear);
            vols << QStringLiteral("%1%2 %3/%4 GB")
                        .arg(m.value(QStringLiteral("name")).toString(),
                             disk.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(disk),
                             gb(m.value(QStringLiteral("usedGB")).toDouble()), gb(m.value(QStringLiteral("totalGB")).toDouble()));
        }
        qInfo().noquote() << QStringLiteral("system monitor: %1 cores (max MHz %2), throttled %3, CPU %4 C, GPU %5 MHz %6 C, "
                                            "memory %7/%8 GB, volumes %9, uptime %10 s")
                                 .arg(coreLoad_.size()).arg(cores.join(QStringLiteral(", ")))
                                 .arg(throttled_ ? QStringLiteral("yes") : QStringLiteral("no"))
                                 .arg(std::isfinite(cpuTemp_) ? QString::number(cpuTemp_, 'f', 1) : QStringLiteral("-"))
                                 .arg(gpuFreq_).arg(std::isfinite(gpuTemp_) ? QString::number(gpuTemp_, 'f', 1) : QStringLiteral("-"))
                                 .arg(gb(memUsed_), gb(memTotal_), vols.join(QStringLiteral(", ")))
                                 .arg(uptime_);
    }
    emit changed();
}

void SystemMonitor::readCpu()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_ANDROID)
    const QVector<system_stats::CpuTimes> times = system_stats::parseProcStat(readSmall(QStringLiteral("/proc/stat")));
    const QVector<double> loads = system_stats::loads(lastTimes_, times);
    lastTimes_ = times;
    coreLoad_ = QList<double>(loads.cbegin(), loads.cend());
    coreFreq_.clear();
    coreMax_.clear();
    throttled_ = false;
    for (int i = 0; i < times.size(); ++i) {
        const QString base = QStringLiteral("/sys/devices/system/cpu/cpu%1/cpufreq/").arg(i);
        const long long cur = readNumber(base + QStringLiteral("scaling_cur_freq"), 0);
        const long long max = readNumber(base + QStringLiteral("cpuinfo_max_freq"), 0);
        const long long cap = readNumber(base + QStringLiteral("scaling_max_freq"), max);
        coreFreq_.append(int(cur / 1000));
        coreMax_.append(int(max / 1000));
        if (max > 0 && cap > 0 && cap < max)
            throttled_ = true;
    }
#elif defined(Q_OS_WIN)
    static const auto query = reinterpret_cast<NtQuerySystemInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    static HMODULE powrprof = LoadLibraryW(L"powrprof.dll");
    static const auto power = powrprof ? reinterpret_cast<CallNtPowerInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(powrprof, "CallNtPowerInformation"))) : nullptr;
    const int n = processorCount();
    QVector<system_stats::CpuTimes> times;
    if (query && n > 0) {
        QVector<ProcessorTimes> raw(n);
        ULONG got = 0;
        if (query(8, raw.data(), ULONG(sizeof(ProcessorTimes) * n), &got) == 0) {
            for (int i = 0; i < int(got / sizeof(ProcessorTimes)); ++i) {
                system_stats::CpuTimes t;
                t.total = quint64(raw[i].kernel.QuadPart + raw[i].user.QuadPart);
                t.busy = t.total - quint64(raw[i].idle.QuadPart);
                times.append(t);
            }
        }
    }
    const QVector<double> loads = system_stats::loads(lastTimes_, times);
    lastTimes_ = times;
    coreLoad_ = QList<double>(loads.cbegin(), loads.cend());
    coreFreq_.clear();
    coreMax_.clear();
    throttled_ = false;
    if (power && n > 0) {
        QVector<ProcessorPower> pw(n);
        if (power(11, nullptr, 0, pw.data(), ULONG(sizeof(ProcessorPower) * n)) == 0) {
            for (const ProcessorPower& p : pw) {
                coreFreq_.append(int(p.currentMhz));
                coreMax_.append(int(p.maxMhz));
                if (p.mhzLimit > 0 && p.mhzLimit < p.maxMhz)
                    throttled_ = true;
            }
        }
    }
#endif
    double sum = 0.0;
    for (double l : coreLoad_)
        sum += l;
    cpuLoad_ = coreLoad_.isEmpty() ? -1.0 : sum / coreLoad_.size();
    cpuFreq_ = -1;
    for (int f : coreFreq_)
        cpuFreq_ = std::max(cpuFreq_, f);
}

void SystemMonitor::readMemory()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_ANDROID)
    qint64 total = 0, avail = 0;
    if (system_stats::parseMeminfo(readSmall(QStringLiteral("/proc/meminfo")), &total, &avail)) {
        memTotal_ = double(total) / (1024.0 * 1024.0);
        memUsed_ = double(total - avail) / (1024.0 * 1024.0);
    }
#elif defined(Q_OS_WIN)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        memTotal_ = double(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
        memUsed_ = double(ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
    }
#endif
}

void SystemMonitor::readThermal()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_ANDROID)
    double cpu = kNaN, gpu = kNaN;
    const QDir zones(QStringLiteral("/sys/class/thermal"));
    for (const QString& z : zones.entryList({ QStringLiteral("thermal_zone*") }, QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = zones.filePath(z) + QLatin1Char('/');
        const long long milli = readNumber(base + QStringLiteral("temp"), LLONG_MIN);
        if (milli == LLONG_MIN)
            continue;
        const double c = double(milli) / 1000.0;
        switch (system_stats::thermalKind(readSmall(base + QStringLiteral("type")).trimmed())) {
        case system_stats::ThermalCpu:
            cpu = std::isfinite(cpu) ? std::max(cpu, c) : c;
            break;
        case system_stats::ThermalGpu:
            gpu = std::isfinite(gpu) ? std::max(gpu, c) : c;
            break;
        default:
            break;
        }
    }
    cpuTemp_ = cpu;
    gpuTemp_ = gpu;
    gpuFreq_ = -1;
    const QDir devfreq(QStringLiteral("/sys/class/devfreq"));
    for (const QString& d : devfreq.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!d.contains(QLatin1String("gpu"), Qt::CaseInsensitive))
            continue;
        const long long hz = readNumber(devfreq.filePath(d) + QStringLiteral("/cur_freq"), -1);
        if (hz > 0)
            gpuFreq_ = int(hz / 1000000);
        break;
    }
#endif
}

void SystemMonitor::readVolumes()
{
    QVariantList out;
    QStringList devices, disks;
    const QStorageInfo root = QStorageInfo::root();
    QList<QStorageInfo> all = QStorageInfo::mountedVolumes();
    std::stable_sort(all.begin(), all.end(), [&root](const QStorageInfo& a, const QStorageInfo& b) {
        return a.rootPath() == root.rootPath() && b.rootPath() != root.rootPath();
    });
    for (const QStorageInfo& v : all) {
        if (!v.isValid() || !v.isReady() || v.bytesTotal() < 256LL * 1024 * 1024)
            continue;
        const QString dev = QString::fromLocal8Bit(v.device());
        const QString fs = QString::fromLatin1(v.fileSystemType());
#if !defined(Q_OS_WIN)
        if (!dev.startsWith(QLatin1String("/dev/")) || fs == QLatin1String("squashfs") || system_stats::hiddenMount(v.rootPath()))
            continue;
#endif
        if (devices.contains(dev))
            continue;
        devices << dev;
#if defined(Q_OS_WIN)
        const QString name = v.rootPath().left(2);
#else
        const QString name = system_stats::volumeName(v.rootPath());
#endif
        // the disk's kind and wear, on its first volume (/dev/mmcblk0p1 -> /sys/block/mmcblk0)
        QString kind;
        int wear = -1;
#if defined(Q_OS_LINUX) || defined(Q_OS_ANDROID)
        const QString disk = system_stats::diskOf(dev);
        if (!disk.isEmpty() && !disks.contains(disk)) {
            disks << disk;
            const QString block = QStringLiteral("/sys/block/%1/").arg(disk);
            kind = system_stats::diskKind(disk, readSmall(block + QStringLiteral("device/type")),
                                          readNumber(block + QStringLiteral("removable"), 0) == 1);
            if (disk.startsWith(QLatin1String("mmcblk")))
                wear = system_stats::emmcWearPercent(readSmall(block + QStringLiteral("device/life_time")));
        }
#endif
        const double total = double(v.bytesTotal()) / (1024.0 * 1024.0 * 1024.0);
        const double used = double(v.bytesTotal() - v.bytesAvailable()) / (1024.0 * 1024.0 * 1024.0);
        out.append(QVariantMap{ { QStringLiteral("name"), name }, { QStringLiteral("kind"), kind },
                                { QStringLiteral("wearPct"), wear }, { QStringLiteral("totalGB"), total },
                                { QStringLiteral("usedGB"), used } });
        if (out.size() >= 3)
            break;
    }
    volumes_ = out;
}

void SystemMonitor::readUptime()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_ANDROID)
    bool ok = false;
    const double s = readSmall(QStringLiteral("/proc/uptime")).section(QLatin1Char(' '), 0, 0).toDouble(&ok);
    uptime_ = ok ? qint64(s) : -1;
#elif defined(Q_OS_WIN)
    uptime_ = qint64(GetTickCount64() / 1000);
#endif
}
