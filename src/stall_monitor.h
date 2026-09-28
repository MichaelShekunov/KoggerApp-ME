#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <thread>

class QQuickWindow;

/**
 * @brief Watches the GUI thread and the render thread and writes what they are doing when the UI stops.
 *
 * Made for the head unit's full hangs at the 2026-09-28 trials, where the GUI thread went silent while other threads
 * of the app still ran. The GUI thread stamps a heartbeat every 200 ms and the render thread every swapped frame; a
 * monitor thread looks every 250 ms. When the heartbeat is 2 s old it logs a stall: every thread of the process
 * (state, kernel wait channel, CPU since the last look) and the system (load, free memory, the process's RSS and GPU
 * memory), and the GUI and render threads write their backtraces to stall.log next to the app log and to stderr
 * (the journal). While the stall lasts it logs again every 5 s, takes backtraces again at 10 s and 30 s, and logs the
 * end with its length. Every 60 s without a stall it logs a summary: the event loop's worst lag, the frames and the
 * longest gap between them, RSS and GPU memory. The /proc and backtrace parts are Linux only.
 *
 * KOGGER_STALL_TEST=<seconds>, or a file "stall_test" with the seconds in the log directory (removed when taken),
 * blocks the GUI thread once for that long, 30 s after start, to check all this on a device.
 */
class StallMonitor : public QObject
{
    Q_OBJECT

public:
    static StallMonitor& instance();
    ~StallMonitor() override;

    /** Starts watching (from the GUI thread); window may be null (then no frame statistics). logDir: stall.log. */
    void start(QQuickWindow* window, const QString& logDir);

private:
    StallMonitor() = default;
    void beat();
    void run();
    void report(bool first, qint64 gapMs, qint64 nowMs);
    void takeBacktraces(qint64 nowMs);
    void summary(qint64 nowMs);

    std::atomic<qint64> lastBeat_{0};
    std::atomic<qint64> maxLag_{0};
    std::atomic<qint64> lastFrame_{0};
    std::atomic<qint64> frames_{0};
    std::atomic<qint64> maxFrameGap_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
    bool started_ = false;
    long guiTid_ = 0;
    QString btPath_;
    // monitor thread only
    bool inStall_ = false;
    qint64 stallStart_ = 0;
    qint64 nextRepeat_ = 0;
    int backtraces_ = 0;
    qint64 lastSummary_ = 0;
    qint64 framesAtSummary_ = 0;
};
