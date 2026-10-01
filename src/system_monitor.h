#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVector>

#include "system_stats.h"

#include <limits>

/**
 * @brief The computer the app runs on, for the "System" widget (owner 2026-10-01): each core's frequency and load,
 * the CPU and GPU temperatures, memory, the volumes with their disks' kind and an eMMC's wear, the uptime.
 *
 * Read once a second while someone shows it (subscribe / unsubscribe), the volumes every tenth time, and for a moment
 * 3 s after it is made, so the log has its line (once) with no widget showing it. Linux: cpufreq,
 * /proc/stat, /proc/meminfo, the thermal zones, devfreq (GPU), QStorageInfo, /sys/block (kind, life_time),
 * /proc/uptime.
 * The GPU: devfreq and this process's DRM fdinfo (drm-engine-*). Windows: memory, each core's load and frequency
 * (ntdll, powrprof, loaded at run time), the volumes, the uptime; no temperatures, no GPU. Unknown values: -1, NaN or empty.
 */
class SystemMonitor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QList<int> coreFreqMHz READ coreFreqMHz NOTIFY changed)
    Q_PROPERTY(QList<int> coreMaxMHz READ coreMaxMHz NOTIFY changed)
    Q_PROPERTY(QList<double> coreLoad READ coreLoad NOTIFY changed)
    Q_PROPERTY(double cpuLoad READ cpuLoad NOTIFY changed)
    Q_PROPERTY(int cpuFreqMHz READ cpuFreqMHz NOTIFY changed)
    Q_PROPERTY(double cpuTempC READ cpuTempC NOTIFY changed)
    Q_PROPERTY(double gpuTempC READ gpuTempC NOTIFY changed)
    Q_PROPERTY(int gpuFreqMHz READ gpuFreqMHz NOTIFY changed)
    Q_PROPERTY(int gpuMaxMHz READ gpuMaxMHz NOTIFY changed)
    Q_PROPERTY(double gpuLoad READ gpuLoad NOTIFY changed)
    Q_PROPERTY(bool throttled READ throttled NOTIFY changed)
    Q_PROPERTY(double memTotalGB READ memTotalGB NOTIFY changed)
    Q_PROPERTY(double memUsedGB READ memUsedGB NOTIFY changed)
    Q_PROPERTY(QVariantList volumes READ volumes NOTIFY changed)
    Q_PROPERTY(qint64 uptimeS READ uptimeS NOTIFY changed)

public:
    explicit SystemMonitor(QObject* parent = nullptr);

    /** Someone shows the values: the reading runs while anyone does. */
    Q_INVOKABLE void subscribe();
    Q_INVOKABLE void unsubscribe();

    bool available() const { return available_; }
    /** Each core now, MHz (0 unknown). */
    QList<int> coreFreqMHz() const { return coreFreq_; }
    /** Each core's maximum, MHz: the scale of its bar; cores of one cluster share it. */
    QList<int> coreMaxMHz() const { return coreMax_; }
    /** Each core's load over the last second, 0..1. */
    QList<double> coreLoad() const { return coreLoad_; }
    /** All cores, 0..1; -1 unknown. */
    double cpuLoad() const { return cpuLoad_; }
    /** The fastest core now, MHz; -1 unknown. */
    int cpuFreqMHz() const { return cpuFreq_; }
    /** The hottest CPU thermal zone, degrees C; NaN unknown. */
    double cpuTempC() const { return cpuTemp_; }
    double gpuTempC() const { return gpuTemp_; }
    /** The GPU now and the top of its frequency table, MHz; -1 unknown. */
    int gpuFreqMHz() const { return gpuFreq_; }
    int gpuMaxMHz() const { return gpuMax_; }
    /** This app's share of the GPU over the last second, 0..1 (on the head unit's kiosk, the GPU's only user); -1
     *  when the driver does not report its engines' time (panthor does only with its job profiling on). */
    double gpuLoad() const { return gpuLoad_; }
    /** A cpufreq policy held below its maximum (thermal or power capping). */
    bool throttled() const { return throttled_; }
    double memTotalGB() const { return memTotal_; }
    double memUsedGB() const { return memUsed_; }
    /** [{name, kind, wearPct, totalGB, usedGB}]: the system's volume first, then other local ones, three at most.
     *  kind ("eMMC", "SD", "NVMe", "USB", empty) and wearPct (an eMMC's, an upper bound in percent, 110 beyond its
     *  rated life, -1 unknown) come on the first volume of each disk only. */
    QVariantList volumes() const { return volumes_; }
    qint64 uptimeS() const { return uptime_; }

signals:
    void changed();

private:
    void refresh();
    void readCpu();
    void readMemory();
    void readThermal();
    void readGpu();
    void readVolumes();
    void readUptime();

    static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

    QTimer timer_;
    int subscribers_ = 0;
    int ticks_ = 0;
    bool logged_ = false;

    QVector<system_stats::CpuTimes> lastTimes_;
    bool available_ = false;
    QList<int> coreFreq_, coreMax_;
    QList<double> coreLoad_;
    double cpuLoad_ = -1.0;
    int cpuFreq_ = -1;
    double cpuTemp_ = kNaN, gpuTemp_ = kNaN;
    int gpuFreq_ = -1, gpuMax_ = -1;
    double gpuLoad_ = -1.0;
    QElapsedTimer clock_;
    QHash<QString, quint64> lastEngines_;
    qint64 lastGpuAt_ = -1;
    bool throttled_ = false;
    double memTotal_ = 0.0, memUsed_ = 0.0;
    QVariantList volumes_;
    qint64 uptime_ = -1;
};
