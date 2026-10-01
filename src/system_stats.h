#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cstdint>

/**
 * @brief Parsers of the Linux system files SystemMonitor reads (/proc/stat, /proc/meminfo, an eMMC's life_time, a
 * thermal zone's type, the disk and name of a volume, a DRM client's fdinfo), pure so the offline checks run them on
 * samples.
 */
namespace system_stats {

struct CpuTimes {
    quint64 busy = 0;    // user + nice + system + irq + softirq + steal
    quint64 total = 0;   // busy + idle + iowait
};

/** The per-core lines ("cpu0 ...") of /proc/stat, in order; the summary "cpu" line is left out. */
inline QVector<CpuTimes> parseProcStat(const QString& text)
{
    QVector<CpuTimes> out;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        if (line.size() < 4 || !line.startsWith(QLatin1String("cpu")) || !line.at(3).isDigit())
            continue;
        const QStringList f = line.simplified().split(QLatin1Char(' '));
        if (f.size() < 5)
            continue;
        auto v = [&f](int i) -> quint64 { return i < f.size() ? f.at(i).toULongLong() : 0; };
        CpuTimes t;
        t.busy = v(1) + v(2) + v(3) + v(6) + v(7) + v(8);
        t.total = t.busy + v(4) + v(5);
        out.append(t);
    }
    return out;
}

/** Each core's load 0..1 between two samples; a core missing in either, or with no time passed, reads 0. */
inline QVector<double> loads(const QVector<CpuTimes>& before, const QVector<CpuTimes>& after)
{
    QVector<double> out;
    out.reserve(after.size());
    for (int i = 0; i < after.size(); ++i) {
        if (i >= before.size() || after[i].total <= before[i].total || after[i].busy < before[i].busy) {
            out.append(0.0);
            continue;
        }
        const double dt = double(after[i].total - before[i].total);
        out.append(std::clamp(double(after[i].busy - before[i].busy) / dt, 0.0, 1.0));
    }
    return out;
}

/** MemTotal and MemAvailable of /proc/meminfo, kB; false when either is missing. */
inline bool parseMeminfo(const QString& text, qint64* totalKb, qint64* availableKb)
{
    qint64 total = -1, avail = -1;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        const QStringList f = line.simplified().split(QLatin1Char(' '));
        if (f.size() < 2)
            continue;
        if (f.at(0) == QLatin1String("MemTotal:"))
            total = f.at(1).toLongLong();
        else if (f.at(0) == QLatin1String("MemAvailable:"))
            avail = f.at(1).toLongLong();
    }
    if (total <= 0 || avail < 0)
        return false;
    if (totalKb)
        *totalKb = total;
    if (availableKb)
        *availableKb = avail;
    return true;
}

/** An eMMC's wear from its life_time ("0x01 0x02", types A and B: 0x01 up to 10 % of its life used ... 0x0A up to
 *  100 %, 0x0B beyond): the worse of the two as an upper bound in percent, 110 beyond, -1 unknown. */
inline int emmcWearPercent(const QString& lifeTime)
{
    int worst = -1;
    for (const QString& f : lifeTime.simplified().split(QLatin1Char(' '))) {
        bool ok = false;
        const int v = f.toInt(&ok, 16);
        if (!ok || v < 1 || v > 0x0B)
            continue;
        worst = std::max(worst, v);
    }
    return worst < 0 ? -1 : worst * 10;
}

/** The disk a volume's device is on: /dev/mmcblk0p2 -> mmcblk0, /dev/nvme0n1p1 -> nvme0n1, /dev/sda1 -> sda; empty
 *  for anything else (/dev/mapper/..., not under /dev). */
inline QString diskOf(const QString& device)
{
    if (!device.startsWith(QLatin1String("/dev/")))
        return QString();
    const QString n = device.mid(5);
    auto allDigits = [](const QString& s) {
        return !s.isEmpty() && std::all_of(s.cbegin(), s.cend(), [](QChar c) { return c.isDigit(); });
    };
    if (n.startsWith(QLatin1String("mmcblk")) || n.startsWith(QLatin1String("nvme"))) {
        // a partition: "p" and digits after the disk's own number
        const int p = n.lastIndexOf(QLatin1Char('p'));
        if (p > 0 && n.at(p - 1).isDigit() && allDigits(n.mid(p + 1)))
            return n.left(p);
        return n;
    }
    if (n.startsWith(QLatin1String("sd")) || n.startsWith(QLatin1String("vd")) || n.startsWith(QLatin1String("hd"))) {
        int end = n.size();
        while (end > 0 && n.at(end - 1).isDigit())
            --end;
        return n.left(end);
    }
    return QString();
}

/** A disk's kind for the widget, from its name, its MMC card type (/sys/block/X/device/type) and the removable
 *  flag: "eMMC", "SD", "NVMe", "USB" or empty. */
inline QString diskKind(const QString& disk, const QString& mmcType, bool removable)
{
    if (disk.startsWith(QLatin1String("mmcblk"))) {
        const QString t = mmcType.trimmed();
        return t == QLatin1String("MMC") ? QStringLiteral("eMMC") : t == QLatin1String("SD") ? QStringLiteral("SD") : QString();
    }
    if (disk.startsWith(QLatin1String("nvme")))
        return QStringLiteral("NVMe");
    if (disk.startsWith(QLatin1String("sd")) && removable)
        return QStringLiteral("USB");
    return QString();
}

/** A volume's name for the widget: a mount point as it is ("/", "/data"), a mounted stick or card by its last part
 *  (/media/orangepi/KINGSTON -> KINGSTON). */
inline QString volumeName(const QString& rootPath)
{
    for (const char* pre : { "/media/", "/run/media/", "/mnt/" }) {
        if (rootPath.startsWith(QLatin1String(pre)))
            return rootPath.section(QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty);
    }
    return rootPath;
}

/** A mount the widget leaves out: the boot and EFI partitions. */
inline bool hiddenMount(const QString& rootPath)
{
    return rootPath == QLatin1String("/boot") || rootPath.startsWith(QLatin1String("/boot/"))
           || rootPath == QLatin1String("/efi");
}

/** One DRM client's lines in an fdinfo (/proc/<pid>/fdinfo/<fd> of a /dev/dri node): its id, the driver, resident
 *  memory in KiB, each engine's busy time in ns ("drm-engine-<e>: N ns"; panthor reports it only with its job
 *  profiling on) and how many engines of a class there are ("drm-engine-capacity-<e>: N"). id -1 when the text is
 *  not a DRM client's. */
struct DrmClient {
    qint64 id = -1;
    QString driver;
    qint64 residentKiB = -1;
    QHash<QString, quint64> engineNs;
    QHash<QString, int> capacity;
};

inline DrmClient parseDrmFdinfo(const QString& text)
{
    DrmClient c;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        const QString key = line.left(colon).trimmed();
        const QStringList v = line.mid(colon + 1).simplified().split(QLatin1Char(' '));
        if (key == QLatin1String("drm-client-id"))
            c.id = v.value(0).toLongLong();
        else if (key == QLatin1String("drm-driver"))
            c.driver = v.value(0);
        else if (key == QLatin1String("drm-resident-memory"))
            c.residentKiB = v.value(0).toLongLong();
        else if (key.startsWith(QLatin1String("drm-engine-capacity-")))
            c.capacity.insert(key.mid(20), v.value(0).toInt());
        else if (key.startsWith(QLatin1String("drm-engine-")) && v.value(1) == QLatin1String("ns"))
            c.engineNs.insert(key.mid(11), v.value(0).toULongLong());
    }
    return c;
}

/** The GPU's busy share between two sums of its engines' busy time: the busiest engine class's time over the wall
 *  time (and over its capacity), 0..1; -1 when no engine is in both samples or no time passed. */
inline double drmBusy(const QHash<QString, quint64>& before, const QHash<QString, quint64>& after,
                      const QHash<QString, int>& capacity, qint64 wallNs)
{
    if (wallNs <= 0)
        return -1.0;
    double busiest = -1.0;
    for (auto it = after.cbegin(); it != after.cend(); ++it) {
        const auto was = before.constFind(it.key());
        if (was == before.cend() || it.value() < was.value())
            continue;
        const int engines = std::max(1, capacity.value(it.key(), 1));
        busiest = std::max(busiest, std::clamp(double(it.value() - was.value()) / (double(wallNs) * engines), 0.0, 1.0));
    }
    return busiest;
}

/** What a thermal zone measures, from its type ("bigcore0-thermal", "gpu-thermal", "x86_pkg_temp", ...). */
enum ThermalKind { ThermalOther = 0, ThermalCpu, ThermalGpu, ThermalNpu };
inline int thermalKind(const QString& type)
{
    const QString t = type.toLower();
    if (t.contains(QLatin1String("gpu")))
        return ThermalGpu;
    if (t.contains(QLatin1String("npu")))
        return ThermalNpu;
    if (t.contains(QLatin1String("core")) || t.contains(QLatin1String("cpu")) || t.contains(QLatin1String("package"))
        || t.contains(QLatin1String("soc")) || t.contains(QLatin1String("pkg")))
        return ThermalCpu;
    return ThermalOther;
}

} // namespace system_stats
