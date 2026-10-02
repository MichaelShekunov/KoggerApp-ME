#include "stall_monitor.h"
#include "system_stats.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QQuickWindow>
#include <QTimer>
#include <QThread>
#include <algorithm>
#include <chrono>

// Qt counts Android as a Linux, and most of what this file reads is there too:
// /proc/self/task, /proc/loadavg, the thread states. The unwinder is not -
// bionic declares backtrace() only from API 33 and the app targets 28 - so the
// backtrace half is a desktop and head-unit facility, and the rest still runs.
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#define KOGGER_STALL_BACKTRACE
#endif

#ifdef Q_OS_LINUX
#include <QHash>
#include <QStringList>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef KOGGER_STALL_BACKTRACE
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

qint64 nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

constexpr int kBeatMs = 200;
constexpr int kStallMs = 2000;
constexpr int kRepeatMs = 5000;
constexpr int kSummaryMs = 60000;
constexpr int kLookMs = 250;

#ifdef KOGGER_STALL_BACKTRACE
int g_btFd = -1;
std::atomic<int> g_btCount{0};

// Signal handler: only write(), backtrace() (loaded at start) and backtrace_symbols_fd(), all without malloc.
void writeNumber(int fd, long v)
{
    char buf[24];
    int i = int(sizeof buf);
    unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
    do {
        buf[--i] = char('0' + u % 10);
        u /= 10;
    } while (u && i > 1);
    if (v < 0)
        buf[--i] = '-';
    (void)!::write(fd, buf + i, sizeof buf - size_t(i));
}

void backtraceHandler(int)
{
    void* frames[64];
    const int n = backtrace(frames, 64);
    const long tid = syscall(SYS_gettid);
    const int fds[2] = { g_btFd, STDERR_FILENO };
    for (int fd : fds) {
        if (fd < 0)
            continue;
        static const char head[] = "--- backtrace of thread ";
        (void)!::write(fd, head, sizeof head - 1);
        writeNumber(fd, tid);
        (void)!::write(fd, " ---\n", 5);
        backtrace_symbols_fd(frames, n, fd);
    }
    g_btCount.fetch_add(1);
}
#endif

#ifdef Q_OS_LINUX
QString readSmall(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromLatin1(f.read(4096)).trimmed();
}

struct ThreadLook {
    long tid = 0;
    QString comm;
    QChar state;
    QString wchan;
    qint64 ticks = 0;
};

QList<ThreadLook> lookAtThreads()
{
    QList<ThreadLook> out;
    const QStringList ids = QDir(QStringLiteral("/proc/self/task")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& id : ids) {
        const QString base = QStringLiteral("/proc/self/task/") + id;
        const QString stat = readSmall(base + QStringLiteral("/stat"));
        const int open = stat.indexOf(QLatin1Char('('));
        const int close = stat.lastIndexOf(QLatin1Char(')'));
        if (open < 0 || close < open)
            continue;
        ThreadLook t;
        t.tid = id.toLong();
        t.comm = stat.mid(open + 1, close - open - 1);
        const QStringList rest = stat.mid(close + 2).split(QLatin1Char(' '));
        if (rest.size() > 12) {
            t.state = rest.at(0).isEmpty() ? QLatin1Char('?') : rest.at(0).at(0);
            t.ticks = rest.at(11).toLongLong() + rest.at(12).toLongLong();   // utime + stime (fields 14, 15)
        }
        t.wchan = readSmall(base + QStringLiteral("/wchan"));
        if (t.wchan.isEmpty())
            t.wchan = QStringLiteral("0");
        out << t;
    }
    return out;
}

long findThread(const QString& comm)
{
    for (const ThreadLook& t : lookAtThreads())
        if (t.comm == comm)
            return t.tid;
    return 0;
}

// The process's GPU memory (panthor fdinfo, resident, per DRM client), MB; -1 when there is none.
double gpuMemoryMb()
{
    QHash<qint64, qint64> byClient;
    const QStringList fds = QDir(QStringLiteral("/proc/self/fdinfo")).entryList(QDir::Files);
    for (const QString& fd : fds) {
        const system_stats::DrmClient c = system_stats::parseDrmFdinfo(readSmall(QStringLiteral("/proc/self/fdinfo/") + fd));
        if (c.driver == QLatin1String("panthor") && c.id >= 0 && c.residentKiB >= 0)
            byClient.insert(c.id, c.residentKiB);
    }
    if (byClient.isEmpty())
        return -1;
    qint64 kib = 0;
    for (qint64 v : byClient)
        kib += v;
    return kib / 1024.0;
}

double rssMb()
{
    for (const QString& line : readSmall(QStringLiteral("/proc/self/status")).split(QLatin1Char('\n')))
        if (line.startsWith(QStringLiteral("VmRSS:")))
            return line.section(QLatin1Char(':'), 1).trimmed().section(QLatin1Char(' '), 0, 0).toLongLong() / 1024.0;
    return -1;
}

qint64 memAvailableMb()
{
    for (const QString& line : readSmall(QStringLiteral("/proc/meminfo")).split(QLatin1Char('\n')))
        if (line.startsWith(QStringLiteral("MemAvailable:")))
            return line.section(QLatin1Char(':'), 1).trimmed().section(QLatin1Char(' '), 0, 0).toLongLong() / 1024;
    return -1;
}

QHash<long, qint64> g_lastTicks;   // monitor thread only
qint64 g_lastTicksAt = 0;
#endif

} // namespace

StallMonitor& StallMonitor::instance()
{
    static StallMonitor monitor;
    return monitor;
}

StallMonitor::~StallMonitor()
{
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

void StallMonitor::start(QQuickWindow* window, const QString& logDir)
{
    if (started_)
        return;
    started_ = true;
    const qint64 now = nowMs();
    lastBeat_ = now;
    lastFrame_ = now;
    lastSummary_ = now;

#ifdef Q_OS_LINUX
    guiTid_ = syscall(SYS_gettid);
#endif
#ifdef KOGGER_STALL_BACKTRACE
    btPath_ = QDir(logDir).filePath(QStringLiteral("stall.log"));
    g_btFd = ::open(btPath_.toLocal8Bit().constData(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    void* warm[4];
    backtrace(warm, 4);                        // loads the unwinder now, not inside the signal handler
    struct sigaction sa = {};
    sa.sa_handler = backtraceHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, nullptr);
#else
    Q_UNUSED(logDir)
#endif

    auto* timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    timer->setInterval(kBeatMs);
    connect(timer, &QTimer::timeout, this, &StallMonitor::beat);
    timer->start();

    if (window) {
        // frameSwapped comes from the render thread with the threaded render loop: count there, no queuing
        connect(window, &QQuickWindow::frameSwapped, window, [this]() {
            const qint64 t = nowMs();
            const qint64 gap = t - lastFrame_.exchange(t);
            ++frames_;
            qint64 seen = maxFrameGap_.load();
            while (gap > seen && !maxFrameGap_.compare_exchange_weak(seen, gap)) {}
        }, Qt::DirectConnection);
    }

    // A test stall on request: KOGGER_STALL_TEST=<s>, or a file "stall_test" holding <s> in the log directory (taken
    // once: the file is removed), so a device can be checked without touching its service.
    int testS = qEnvironmentVariableIntValue("KOGGER_STALL_TEST");
    QFile testFile(QDir(logDir).filePath(QStringLiteral("stall_test")));
    if (testS <= 0 && testFile.open(QIODevice::ReadOnly)) {
        testS = QString::fromLatin1(testFile.read(16)).trimmed().toInt();
        testFile.close();
        testFile.remove();
    }
    if (testS > 0) {
        QTimer::singleShot(30000, this, [testS]() {
            qWarning().noquote() << QStringLiteral("stall test: blocking the GUI thread for %1 s (KOGGER_STALL_TEST)").arg(testS);
            QThread::sleep(static_cast<unsigned long>(testS));
        });
    }

    thread_ = std::thread(&StallMonitor::run, this);
    qInfo().noquote() << QStringLiteral("stall monitor: on (heartbeat %1 ms, stall at %2 s, summary every %3 s%4)")
                             .arg(kBeatMs).arg(kStallMs / 1000).arg(kSummaryMs / 1000)
                             .arg(btPath_.isEmpty() ? QString() : QStringLiteral(", backtraces to ") + btPath_);
}

void StallMonitor::beat()
{
    const qint64 t = nowMs();
    const qint64 lag = t - lastBeat_.exchange(t) - kBeatMs;
    qint64 seen = maxLag_.load();
    while (lag > seen && !maxLag_.compare_exchange_weak(seen, lag)) {}
}

void StallMonitor::run()
{
    while (!stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kLookMs));
        const qint64 now = nowMs();
        const qint64 gap = now - lastBeat_.load();
        if (!inStall_ && gap >= kStallMs) {
            inStall_ = true;
            stallStart_ = now - gap;
            nextRepeat_ = now + kRepeatMs;
            backtraces_ = 0;
            report(true, gap, now);
            takeBacktraces(now);
        } else if (inStall_ && gap < kStallMs) {
            inStall_ = false;
            qWarning().noquote() << QStringLiteral("stall: the GUI thread runs again after %1 s")
                                        .arg((now - stallStart_) / 1000.0, 0, 'f', 1);
            lastSummary_ = now;                  // the next summary covers a full period after the stall
            framesAtSummary_ = frames_.load();
            maxLag_ = 0;
            maxFrameGap_ = 0;
        } else if (inStall_ && now >= nextRepeat_) {
            nextRepeat_ = now + kRepeatMs;
            report(false, gap, now);
            const qint64 length = now - stallStart_;
            if ((length >= 10000 && backtraces_ < 2) || (length >= 30000 && backtraces_ < 3))
                takeBacktraces(now);
        }
        if (!inStall_ && now - lastSummary_ >= kSummaryMs) {
            summary(now);
            lastSummary_ = now;
        }
    }
}

void StallMonitor::report(bool first, qint64 gapMs, qint64 now)
{
    const qint64 frameAgo = now - lastFrame_.load();
    qWarning().noquote() << QStringLiteral("stall: %1the GUI thread has not run for %2 s; last frame %3 s ago")
                                .arg(first ? QString() : QStringLiteral("still: "))
                                .arg(gapMs / 1000.0, 0, 'f', 1).arg(frameAgo / 1000.0, 0, 'f', 1);
#ifdef Q_OS_LINUX
    const QList<ThreadLook> list = lookAtThreads();
    const long hz = sysconf(_SC_CLK_TCK);
    const qint64 span = g_lastTicksAt > 0 ? now - g_lastTicksAt : 0;
    QStringList parts;
    int inKernel = 0;
    for (const ThreadLook& t : list) {
        const qint64 was = g_lastTicks.value(t.tid, t.ticks);
        const double cpu = span > 0 && hz > 0 ? 100.0 * (t.ticks - was) * 1000.0 / hz / span : 0.0;
        if (t.state == QLatin1Char('D'))
            ++inKernel;
        const bool key = t.tid == guiTid_ || t.comm == QStringLiteral("QSGRenderThread");
        if (first || key || cpu >= 5.0 || t.state == QLatin1Char('D') || t.state == QLatin1Char('R'))
            parts << QStringLiteral("%1[%2]%3 %4 %5 %6%").arg(t.comm).arg(t.tid).arg(t.tid == guiTid_ ? QStringLiteral("(GUI)") : QString())
                         .arg(t.state).arg(t.wchan).arg(cpu, 0, 'f', 0);
        g_lastTicks.insert(t.tid, t.ticks);
    }
    g_lastTicksAt = now;
    qWarning().noquote() << QStringLiteral("stall threads: ") + parts.join(QStringLiteral(" | "));
    const QString load = readSmall(QStringLiteral("/proc/loadavg")).section(QLatin1Char(' '), 0, 2);
    qWarning().noquote() << QStringLiteral("stall system: load %1, MemAvailable %2 MB, RSS %3 MB, GPU memory %4 MB, threads in D state %5")
                                .arg(load).arg(memAvailableMb()).arg(rssMb(), 0, 'f', 0).arg(gpuMemoryMb(), 0, 'f', 1).arg(inKernel);
#endif
}

void StallMonitor::takeBacktraces(qint64 now)
{
    ++backtraces_;
#ifdef KOGGER_STALL_BACKTRACE
    if (g_btFd < 0)
        return;
    const QByteArray head = QStringLiteral("=== stall, the GUI thread silent for %1 s: backtraces (pid %2) ===\n")
                                .arg((now - stallStart_) / 1000.0, 0, 'f', 1).arg(getpid()).toLatin1();
    (void)!::write(g_btFd, head.constData(), size_t(head.size()));
    const long render = findThread(QStringLiteral("QSGRenderThread"));
    QStringList done;
    const long targets[2] = { guiTid_, render };
    const char* names[2] = { "GUI", "render" };
    for (int i = 0; i < 2; ++i) {
        if (targets[i] <= 0) {
            done << QStringLiteral("%1: no such thread").arg(QLatin1String(names[i]));
            continue;
        }
        const int before = g_btCount.load();
        syscall(SYS_tgkill, getpid(), targets[i], SIGUSR2);
        const qint64 until = nowMs() + 500;
        while (g_btCount.load() == before && nowMs() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        done << QStringLiteral("%1 %2").arg(QLatin1String(names[i]))
                    .arg(g_btCount.load() != before ? QStringLiteral("written")
                                                    : QStringLiteral("not written in 0.5 s (in the kernel: see its state)"));
    }
    qWarning().noquote() << QStringLiteral("stall backtraces (%1): %2").arg(btPath_, done.join(QStringLiteral(", ")));
#else
    Q_UNUSED(now)
#endif
}

void StallMonitor::summary(qint64 now)
{
    const qint64 frames = frames_.load();
    const double spanS = (now - lastSummary_) / 1000.0;
    const qint64 count = frames - framesAtSummary_;
    framesAtSummary_ = frames;
    QString extra;
#ifdef Q_OS_LINUX
    extra = QStringLiteral(", RSS %1 MB, GPU memory %2 MB, threads %3").arg(rssMb(), 0, 'f', 0)
                .arg(gpuMemoryMb(), 0, 'f', 1).arg(lookAtThreads().size());
#endif
    qInfo().noquote() << QStringLiteral("health: event loop worst lag %1 ms, %2 frames in %3 s (%4/s), longest gap between frames %5 ms%6")
                             .arg(std::max<qint64>(0, maxLag_.exchange(0))).arg(count).arg(spanS, 0, 'f', 0)
                             .arg(spanS > 0 ? count / spanS : 0.0, 0, 'f', 1).arg(maxFrameGap_.exchange(0)).arg(extra);
}
