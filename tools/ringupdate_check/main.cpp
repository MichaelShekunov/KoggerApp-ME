// ringupdate_check - drives RingUpgrade (src/device/ring_upgrade.*) and reports PASS/FAIL per check.
//
//   ringupdate_check                 offline: the class against a simulated ring -- two boards, their
//                                    bootloaders, UART rates and relays -- normal runs and failures
//   ringupdate_check --live COM13 --file F.ufw [--targets 2,1] [--expect 1.12] [--rate 921600]
//                                    the real boards: discovery on the line, then the run
//
// Exit code 0 = every offline check passed / the live run succeeded. docs/RING_UPDATE.md tells what
// the simulated boards are modelled on and how the live run was verified.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSerialPort>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "ring_upgrade.h"
#include "ring_rate.h"

namespace {

int g_failed = 0;
int g_checks = 0;

bool check(const QString& name, bool ok, const QString& detail = QString())
{
    ++g_checks;
    if (!ok)
        ++g_failed;
    std::printf("%s  %s%s\n", ok ? "PASS" : "FAIL", qPrintable(name),
                detail.isEmpty() ? "" : qPrintable(QStringLiteral("  -- ") + detail));
    std::fflush(stdout);
    return ok;
}

constexpr uint8_t kIdDataset = 0x10, kIdChartSetup = 0x12, kIdUart = 0x18, kIdVersion = 0x20,
                  kIdBoot = 0x24, kIdUpdate = 0x25, kIdFlash = 0x23;
constexpr uint32_t kKey = 0xC96B5D4Au;
constexpr int kLine = 921600, kSlow = 115200;

struct Kp1 {
    uint8_t route = 0, mode = 0, id = 0;
    QByteArray pl;
    int type() const { return mode & 3; }
    int ver() const { return (mode >> 3) & 7; }
    bool resp() const { return mode & 0x80; }
};

std::optional<Kp1> parseKp1(const QByteArray& f)
{
    if (f.size() < 8 || uint8_t(f[0]) != 0xBB || uint8_t(f[1]) != 0x55)
        return std::nullopt;
    const int n = uint8_t(f[5]);
    if (f.size() != 8 + n)
        return std::nullopt;
    return Kp1{uint8_t(f[2]), uint8_t(f[3]), uint8_t(f[4]), f.mid(6, n)};
}

uint32_t u32(const QByteArray& b, int o)
{
    return uint32_t(uint8_t(b[o])) | (uint32_t(uint8_t(b[o + 1])) << 8) | (uint32_t(uint8_t(b[o + 2])) << 16)
         | (uint32_t(uint8_t(b[o + 3])) << 24);
}

QByteArray dataset(uint32_t periodMs, uint32_t mask)
{
    QByteArray d(1, char(1));
    for (uint32_t v : {periodMs, mask})
        for (int i = 0; i < 4; ++i)
            d.append(char((v >> (8 * i)) & 0xFF));
    return d;
}

bool pinging(const QByteArray& ds) { return ds.size() >= 9 && u32(ds, 1) != 0 && u32(ds, 5) != 0; }

// ---------------------------------------------------------------- simulation

/// Discrete events on a microsecond clock.
struct Sim {
    qint64 now = 0;
    std::multimap<qint64, std::function<void()>> q;
    void at(qint64 t, std::function<void()> f) { q.emplace(t, std::move(f)); }
    void after(qint64 dt, std::function<void()> f) { at(now + dt, std::move(f)); }
    bool runUntil(const std::function<bool()>& stop, qint64 limit)
    {
        while (!q.empty() && !stop()) {
            auto it = q.begin();
            if (it->first > limit)
                break;
            now = it->first;
            auto f = std::move(it->second);
            q.erase(it);
            f();
        }
        return stop();
    }
};

/// One direction of a UART line. Bytes are serialized; a frame arrives whole at the end of its
/// transmission and is understood only if the receiver runs at the sender's rate at that moment.
struct Wire {
    Sim* sim = nullptr;
    std::function<int()> rxRate;
    std::function<void(const QByteArray&)> rx;
    std::function<bool(const QByteArray&)> dropIf;   // test hook: lose this frame on the way
    qint64 busyUntil = 0;
    int garbled = 0;
    void send(const QByteArray& f, int rate)
    {
        const qint64 start = std::max(sim->now, busyUntil);
        busyUntil = start + qint64(f.size()) * 10 * 1000000 / rate;
        sim->at(busyUntil, [this, f, rate] {
            if (dropIf && dropIf(f))
                return;
            if (rxRate() == rate)
                rx(f);
            else
                ++garbled;
        });
    }
};

/// A ring board: the chain firmware's frame rules and the bootloader's behaviour as measured
/// (docs/RING_UPDATE.md). Firmware: relays what is not addressed to it -- the board that has seen data
/// from a board before it relays data only; answers ID_VERSION v0+v2, ID_CHART_SETUP, ID_DATASET;
/// ID_UART v0 changes its rate; ID_BOOT v0 enters the bootloader. Bootloader: up 0.37 s after the
/// reset at 115200, takes routes 0 and 16, answers ID_VERSION v0 only, ID_UART v0 moves it, ID_UPDATE
/// chunks in order (first answered after 1.2 s), ID_BOOT v1 runs the image; the old firmware again
/// 3 s after its start when no chunk came.
struct Board {
    Sim* sim = nullptr;
    int addr = 0;
    int rate = kLine;
    bool down = false;
    QString version = QStringLiteral("1.11");
    QByteArray ds, savedDs;              // runtime and saved ID_DATASET
    Wire* tx = nullptr;
    // bootloader
    bool inLoader = false, loaderUp = false, windowOff = false, loaderDead = false;
    int loaderRate = kSlow, lastNum = 0;
    QByteArray got;
    // what a good image turns into, and test hooks
    QByteArray goodImage;
    QString goodVersion = QStringLiteral("1.12");
    bool ignoreBoot = false, loaderIgnoresRate = false;
    int dieAtChunk = -1;
    qint64 resetUs = 30000;              // answer to ID_BOOT -> reset
    int boots = 0, chunksStored = 0;
    // the UART rate as the ring firmware keeps it: running, reported (g_baud, reloaded from flash by ID_FLASH v1
    // but applied only at boot) and saved
    int savedRate = kLine, baudReported = kLine;
    bool ignoreRate = false, saveBroken = false;
    bool dropRelayWhilePinging = false;  // a slow line: while it pings, the board loses what it should relay
    int datasetAnswersToLose = 0;        // its next ID_DATASET answers are lost on the way

    int curRate() const { return inLoader ? loaderRate : rate; }
    void out(uint8_t route, uint8_t mode, uint8_t id, const QByteArray& pl) { tx->send(RingUpgrade::kp1(route, mode, id, pl), curRate()); }

    void receive(const QByteArray& f)
    {
        const auto k = parseKp1(f);
        if (!k)
            return;
        if (inLoader) {
            loader(*k);
            return;
        }
        const bool mine = k->route == addr || k->route == 0;
        const bool cmd = k->type() == 2 || k->type() == 3;
        if (k->type() == 1 && k->route != addr)
            down = true;
        const bool fwd = k->route != addr && (!down || k->type() == 1);
        if (mine && cmd && k->pl.size() <= 64)
            firmware(*k);
        if (fwd && !inLoader) {
            if (dropRelayWhilePinging && k->type() == 1 && pinging(ds))
                return;
            tx->send(f, rate);
        }
    }

    void ack(const Kp1& k) { if (k.resp()) out(uint8_t(addr), uint8_t(0x81 | (k.mode & 0x38)), k.id, QByteArray(1, char(1))); }

    void firmware(const Kp1& k)
    {
        if (k.id == kIdVersion && k.type() == 3) {
            QByteArray v0(34, 0), v2(9, 0);
            v0[1] = char(12);
            v2[2] = char(12);
            const QStringList p = version.split(QLatin1Char('.'));
            v2[8] = char(p.value(0).toInt());
            v2[7] = char(p.value(1).toInt());
            out(uint8_t(addr), 0x01, kIdVersion, v0);
            out(uint8_t(addr), 0x11, kIdVersion, v2);
        } else if (k.id == kIdChartSetup && k.type() == 3) {
            out(uint8_t(addr), 0x01, kIdChartSetup, QByteArray(6, char(1)));
        } else if (k.id == kIdDataset) {
            if (k.type() == 3) {
                if (datasetAnswersToLose > 0)
                    --datasetAnswersToLose;
                else
                    out(uint8_t(addr), 0x01, kIdDataset, ds);
            }
            else {
                ds = k.pl;
                ack(k);
            }
        } else if (k.id == kIdUart && k.ver() == 0 && k.type() == 2 && k.pl.size() >= 9 && u32(k.pl, 0) == kKey) {
            ack(k);                                       // at the old rate, then the switch
            if (!ignoreRate)
                rate = baudReported = int(u32(k.pl, 5));
        } else if (k.id == kIdUart && k.ver() == 0 && k.type() == 3) {
            QByteArray b(4, 0);
            for (int i = 0; i < 4; ++i)
                b[i] = char((uint32_t(baudReported) >> (8 * i)) & 0xFF);
            out(uint8_t(addr), 0x01, kIdUart, b);
        } else if (k.id == kIdFlash && k.type() == 2 && k.pl.size() >= 4 && u32(k.pl, 0) == kKey) {
            if (k.ver() == 0 && !saveBroken) {
                savedRate = baudReported;
                savedDs = ds;
            } else if (k.ver() == 1) {
                baudReported = savedRate;
                ds = savedDs;
            }
        } else if (k.id == kIdBoot && k.ver() == 0 && k.type() == 2 && k.pl.size() >= 4 && u32(k.pl, 0) == kKey) {
            ack(k);
            if (ignoreBoot)
                return;
            ++boots;
            sim->after(resetUs, [this] { enterLoader(); });
        }
    }

    void enterLoader()
    {
        inLoader = true;
        loaderUp = false;
        windowOff = false;
        loaderRate = kSlow;
        lastNum = 0;
        got.clear();
        sim->after(370000, [this] {
            loaderUp = true;
            sim->after(3000000, [this] {
                if (inLoader && !windowOff && !loaderDead)
                    runFirmware(version);                 // the window lapsed: the old firmware
            });
        });
    }

    void runFirmware(const QString& v)
    {
        inLoader = false;
        version = v;
        rate = baudReported = savedRate;                  // the saved rate
        down = false;
        ds = savedDs;                                     // saved settings, pings included
    }

    void loader(const Kp1& k)
    {
        if (!loaderUp || loaderDead || (k.route & 0x0F) != 0)
            return;
        if (k.id == kIdVersion && k.type() == 3 && k.ver() == 0) {
            QByteArray v0(34, 0);
            v0[1] = char(12);
            out(0, 0x01, kIdVersion, v0);
        } else if (k.id == kIdUart && k.type() == 2 && k.ver() == 0 && k.pl.size() == 9 && u32(k.pl, 0) == kKey
                   && uint8_t(k.pl[4]) == 1) {
            if (!loaderIgnoresRate)
                loaderRate = int(u32(k.pl, 5));
        } else if (k.id == kIdUpdate && k.type() == 2 && k.ver() == 0 && k.pl.size() >= 2) {
            const int n = uint8_t(k.pl[0]) | (uint8_t(k.pl[1]) << 8);
            if (dieAtChunk > 0 && n >= dieAtChunk) {
                loaderDead = true;
                return;
            }
            const QByteArray f = RingUpgrade::kp1(k.route, k.mode, k.id, k.pl);
            QByteArray a(1, char(1));
            a.append(f.right(2));
            if (n == lastNum + 1) {
                windowOff = true;
                lastNum = n;
                got.append(k.pl.mid(2));
                ++chunksStored;
            } else {
                a[0] = char(3);
            }
            sim->after(n == 1 ? 1200000 : 8000, [this, a] { out(0, 0x81, kIdUpdate, a); });
        } else if (k.id == kIdBoot && k.type() == 2 && k.ver() == 1 && k.pl.size() >= 4 && u32(k.pl, 0) == kKey) {
            const QString v = got == goodImage ? goodVersion : QStringLiteral("0.0");
            sim->after(100000, [this, v] { runFirmware(v); });
        }
    }
};

/// The host, its line and one or two boards wired host -> boards[0] -> boards[1] -> host.
struct Ring {
    Sim sim;
    std::vector<std::unique_ptr<Board>> boards;
    std::vector<std::unique_ptr<Wire>> wires;
    int hostRate = kLine;
    std::unique_ptr<RingUpgrade> up;
    bool done = false, ok = false;
    QString detail;
    QStringList finished;
    int maxPct = 0, breaksIntoLoader = 0;
    QList<int> targetsOk, targetsFailed;

    Ring(const QList<int>& addrsInWiringOrder, const QByteArray& image)
    {
        for (int a : addrsInWiringOrder) {
            auto b = std::make_unique<Board>();
            b->sim = &sim;
            b->addr = a;
            b->goodImage = image;
            b->ds = b->savedDs = dataset(80, 1u << 21);  // pinging, and saved so
            boards.push_back(std::move(b));
        }
        auto wire = [this](std::function<int()> rate, std::function<void(const QByteArray&)> rx) {
            auto w = std::make_unique<Wire>();
            w->sim = &sim;
            w->rxRate = std::move(rate);
            w->rx = std::move(rx);
            wires.push_back(std::move(w));
            return wires.back().get();
        };
        hostTx = wire([this] { return boards[0]->curRate(); }, [this](const QByteArray& f) { boards[0]->receive(f); });
        for (size_t i = 0; i + 1 < boards.size(); ++i)
            boards[i]->tx = wire([this, i] { return boards[i + 1]->curRate(); },
                                 [this, i](const QByteArray& f) { boards[i + 1]->receive(f); });
        boards.back()->tx = wire([this] { return hostRate; }, [this](const QByteArray& f) {
            if (up) up->onFrame(f);
            if (rr) rr->onFrame(f);
        });

        RingUpgrade::Io io;
        io.write = [this](const QByteArray& b) { hostTx->send(b, hostRate); };
        io.setRate = [this](int r) { hostRate = r; };
        io.nowMs = [this] { return sim.now / 1000; };
        up = std::make_unique<RingUpgrade>(io);
        QObject::connect(up.get(), &RingUpgrade::finished, [this](bool o, const QString& d) { done = true; ok = o; detail = d; });
        QObject::connect(up.get(), &RingUpgrade::targetProgress, [this](int, int p) { maxPct = std::max(maxPct, p); });
        QObject::connect(up.get(), &RingUpgrade::targetFinished, [this](int a, bool o, const QString&) {
            (o ? targetsOk : targetsFailed).append(a);
        });
        if (qEnvironmentVariableIsSet("RINGUPDATE_CHECK_LOG"))
            QObject::connect(up.get(), &RingUpgrade::logLine, [this](const QString& s) {
                std::printf("      [%7.3f] %s\n", double(sim.now) / 1e6, qPrintable(s));
            });
    }

    Board& byAddr(int a) { for (auto& b : boards) if (b->addr == a) return *b; return *boards[0]; }

    bool run(const RingUpgrade::Plan& plan)
    {
        std::function<void()> tick = [this, &tick] {
            up->tick();
            if (boards.size() > 1 && !boards[0]->inLoader && pinging(boards[0]->ds) && boards[1]->inLoader)
                ++breaksIntoLoader;                     // the first board pings into a bootloader behind it
            if (!done)
                sim.after(5000, tick);
        };
        if (!up->start(plan)) {
            done = true;
            return false;
        }
        sim.after(5000, tick);
        sim.runUntil([this] { return done; }, sim.now + 120LL * 1000000);
        // let late events (a lapsing window, a reboot) settle before the boards are looked at
        sim.runUntil([] { return false; }, sim.now + 5LL * 1000000);
        return done;
    }

    Wire* hostTx = nullptr;

    // RingRate on the same line
    std::unique_ptr<RingRate> rr;
    bool rrDone = false, rrOk = false;
    int rrRate = 0;
    QString rrDetail;
    bool runRate(const RingRate::Plan& plan)
    {
        RingRate::Io io;
        io.write = [this](const QByteArray& b) { hostTx->send(b, hostRate); };
        io.setRate = [this](int r) { hostRate = r; };
        io.nowMs = [this] { return sim.now / 1000; };
        rr = std::make_unique<RingRate>(io);
        rrDone = false;
        QObject::connect(rr.get(), &RingRate::finished, [this](bool o, int r, const QString& d) {
            rrDone = true; rrOk = o; rrRate = r; rrDetail = d;
        });
        if (qEnvironmentVariableIsSet("RINGUPDATE_CHECK_LOG"))
            QObject::connect(rr.get(), &RingRate::logLine, [this](const QString& s) {
                std::printf("      [%7.3f] %s\n", double(sim.now) / 1e6, qPrintable(s));
            });
        if (!rr->start(plan))
            return false;
        std::function<void()> tick = [this, &tick] {
            rr->tick();
            if (!rrDone)
                sim.after(5000, tick);
        };
        sim.after(5000, tick);
        sim.runUntil([this] { return rrDone; }, sim.now + 30LL * 1000000);
        sim.runUntil([] { return false; }, sim.now + 500000);
        return rrDone;
    }
};

QByteArray makeImage(int size)
{
    QByteArray img(size, 0);
    uint32_t x = 0x12345678u;
    for (int i = 0; i < size; ++i) {
        x = x * 1664525u + 1013904223u;
        img[i] = char(x >> 24);
    }
    return img;
}

RingUpgrade::Plan plan(const QList<int>& boards, const QList<int>& targets, const QByteArray& img)
{
    RingUpgrade::Plan p;
    p.boards = boards;
    p.targets = targets;
    p.image = img;
    p.expect = QStringLiteral("1.12");
    p.lineRate = kLine;
    return p;
}

void offline()
{
    const QByteArray img = makeImage(44940);

    {   // 1. the last board alone
        Ring r({1, 2}, img);
        r.run(plan({1, 2}, {2}, img));
        check(QStringLiteral("ring, last board: run succeeds"), r.ok, r.detail);
        check(QStringLiteral("ring, last board: it runs 1.12, the first still 1.11"),
              r.byAddr(2).version == QLatin1String("1.12") && r.byAddr(1).version == QLatin1String("1.11"),
              r.byAddr(2).version + QLatin1Char('/') + r.byAddr(1).version);
        check(QStringLiteral("ring, last board: both boards and the host back at 921600"),
              r.byAddr(1).rate == kLine && r.byAddr(2).rate == kLine && r.hostRate == kLine);
        check(QStringLiteral("ring, last board: pings restored, none into a bootloader"),
              r.byAddr(1).ds == dataset(80, 1u << 21) && r.byAddr(2).ds == dataset(80, 1u << 21) && r.breaksIntoLoader == 0,
              QStringLiteral("into loader %1").arg(r.breaksIntoLoader));
        check(QStringLiteral("ring, last board: progress reached 100"), r.maxPct == 100, QString::number(r.maxPct));
    }
    {   // 2. the first board alone
        Ring r({1, 2}, img);
        r.run(plan({1, 2}, {1}, img));
        check(QStringLiteral("ring, first board: run succeeds"), r.ok, r.detail);
        check(QStringLiteral("ring, first board: it runs 1.12, the last still 1.11"),
              r.byAddr(1).version == QLatin1String("1.12") && r.byAddr(2).version == QLatin1String("1.11"));
        check(QStringLiteral("ring, first board: rates and pings back, none into a bootloader"),
              r.byAddr(1).rate == kLine && r.byAddr(2).rate == kLine && r.hostRate == kLine
              && pinging(r.byAddr(1).ds) && pinging(r.byAddr(2).ds) && r.breaksIntoLoader == 0);
    }
    {   // 3. both, the order "every board"
        Ring r({1, 2}, img);
        const QList<int> all = RingUpgrade::allTargets({1, 2});
        check(QStringLiteral("every board: the last board first"), all == QList<int>({2, 1}));
        r.run(plan({1, 2}, all, img));
        check(QStringLiteral("every board: run succeeds"), r.ok, r.detail);
        check(QStringLiteral("every board: both run 1.12"),
              r.byAddr(1).version == QLatin1String("1.12") && r.byAddr(2).version == QLatin1String("1.12"));
        check(QStringLiteral("every board: no BREAK into a bootloader, pings back"),
              r.breaksIntoLoader == 0 && pinging(r.byAddr(1).ds) && pinging(r.byAddr(2).ds),
              QStringLiteral("into loader %1").arg(r.breaksIntoLoader));
    }
    {   // 4. both in the other order: the first board comes back pinging (saved) before the last is updated
        Ring r({1, 2}, img);
        r.run(plan({1, 2}, {1, 2}, img));
        check(QStringLiteral("first then last: run succeeds, no BREAK into the last board's bootloader"),
              r.ok && r.breaksIntoLoader == 0, QStringLiteral("%1, into loader %2").arg(r.detail).arg(r.breaksIntoLoader));
    }
    {   // 5. a lone board with a bus address
        Ring r({3}, img);
        r.run(plan({3}, {3}, img));
        check(QStringLiteral("lone board at address 3: run succeeds, runs 1.12"), r.ok && r.byAddr(3).version == QLatin1String("1.12"), r.detail);
    }
    {   // 6. the target does not go to its bootloader
        Ring r({1, 2}, img);
        r.byAddr(2).ignoreBoot = true;
        r.run(plan({1, 2}, {2}, img));
        check(QStringLiteral("no bootloader: run fails, nothing written"),
              !r.ok && r.byAddr(2).chunksStored == 0 && r.detail.contains(QLatin1String("nothing was written")), r.detail);
        check(QStringLiteral("no bootloader: rates and pings back"),
              r.byAddr(1).rate == kLine && r.byAddr(2).rate == kLine && r.hostRate == kLine
              && pinging(r.byAddr(1).ds) && pinging(r.byAddr(2).ds));
    }
    {   // 7. the last board's bootloader ignores the rate change
        Ring r({1, 2}, img);
        r.byAddr(2).loaderIgnoresRate = true;
        r.run(plan({1, 2}, {2}, img));
        check(QStringLiteral("last, bootloader stays at 115200: fails before any chunk"),
              !r.ok && r.byAddr(2).chunksStored == 0, r.detail);
        check(QStringLiteral("last, bootloader stays at 115200: old firmware runs again, first board at 921600"),
              !r.byAddr(2).inLoader && r.byAddr(2).version == QLatin1String("1.11") && r.byAddr(1).rate == kLine
              && r.hostRate == kLine);
    }
    {   // 8. the first board's bootloader ignores the rate change
        Ring r({1, 2}, img);
        r.byAddr(1).loaderIgnoresRate = true;
        r.run(plan({1, 2}, {1}, img));
        check(QStringLiteral("first, bootloader stays at 115200: fails before any chunk, old firmware again"),
              !r.ok && r.byAddr(1).chunksStored == 0 && !r.byAddr(1).inLoader && r.byAddr(1).version == QLatin1String("1.11"),
              r.detail);
    }
    {   // 9. the bootloader stops answering in the middle
        Ring r({1, 2}, img);
        r.byAddr(2).dieAtChunk = 200;
        r.run(plan({1, 2}, {2}, img));
        check(QStringLiteral("dies at chunk 200: fails, says the board stays in its bootloader"),
              !r.ok && r.detail.contains(QLatin1String("stays in its bootloader")), r.detail);
        check(QStringLiteral("dies at chunk 200: the other board's pings and rate back"),
              pinging(r.byAddr(1).ds) && r.byAddr(1).rate == kLine && r.hostRate == kLine);
    }
    {   // 10. one chunk lost on the way: sent again, the image arrives whole
        Ring r({1, 2}, img);
        int seen = 0;
        r.wires[0]->dropIf = [&seen](const QByteArray& f) {
            return uint8_t(f[4]) == kIdUpdate && uint8_t(f[6]) == 50 && uint8_t(f[7]) == 0 && seen++ == 0;
        };
        r.run(plan({1, 2}, {2}, img));
        check(QStringLiteral("chunk 50 lost once: run succeeds, image whole"),
              r.ok && r.byAddr(2).got == img && r.byAddr(2).version == QLatin1String("1.12"), r.detail);
    }
    {   // 11. the ring is wired the other way round: address 2 first. A slow reset: a command for the
        //     other board sent before it would pass the target and reach board 1
        Ring r({2, 1}, img);
        r.byAddr(2).resetUs = 100000;
        r.run(plan({1, 2}, {2}, img));
        check(QStringLiteral("wired 2 -> 1, update 2: fails before any chunk"),
              !r.ok && r.byAddr(2).chunksStored == 0 && r.byAddr(1).chunksStored == 0, r.detail);
        check(QStringLiteral("wired 2 -> 1: both boards run their old firmware at 921600, pings back"),
              !r.byAddr(2).inLoader && !r.byAddr(1).inLoader && r.byAddr(1).rate == kLine && r.byAddr(2).rate == kLine
              && pinging(r.byAddr(1).ds) && pinging(r.byAddr(2).ds) && r.hostRate == kLine,
              QStringLiteral("board 1: loader %1 rate %2 pings %3; board 2: loader %4 rate %5 pings %6; host %7")
                  .arg(r.byAddr(1).inLoader).arg(r.byAddr(1).rate).arg(pinging(r.byAddr(1).ds))
                  .arg(r.byAddr(2).inLoader).arg(r.byAddr(2).rate).arg(pinging(r.byAddr(2).ds)).arg(r.hostRate));
    }
    {   // 12. a different image: the version check catches it
        Ring r({1, 2}, img);
        RingUpgrade::Plan p = plan({1, 2}, {2}, img);
        p.expect = QStringLiteral("1.13");
        r.run(p);
        check(QStringLiteral("expects 1.13, gets 1.12: fails on the version"),
              !r.ok && r.detail.contains(QLatin1String("instead of 1.13")), r.detail);
    }
    {   // 13. plans that cannot be carried out
        Ring r({1, 2}, img);
        bool any = false;
        for (const auto& p : {plan({1, 2}, {3}, img), plan({0, 2}, {2}, img), plan({1, 2, 3}, {2}, img),
                              plan({1, 1}, {1}, img), plan({1, 2}, {}, img), plan({1, 2}, {2}, QByteArray())}) {
            r.done = false;
            any |= r.up->start(p);
        }
        check(QStringLiteral("bad plans are refused before anything is sent"), !any && r.sim.q.empty());
    }

    // ---- RingRate: the whole line to another UART rate
    auto ratePlan = [](const QList<int>& boards, int to) { RingRate::Plan p; p.boards = boards; p.from = kLine; p.to = to; return p; };
    for (int to : {460800, 115200}) {
        Ring r({1, 2}, img);
        r.runRate(ratePlan({1, 2}, to));
        const QString tag = QStringLiteral("line rate %1, ring: ").arg(to);
        check(tag + QStringLiteral("run succeeds"), r.rrOk && r.rrRate == to, r.rrDetail);
        check(tag + QStringLiteral("both boards and the host run it"),
              r.byAddr(1).rate == to && r.byAddr(2).rate == to && r.hostRate == to,
              QStringLiteral("%1/%2/%3").arg(r.byAddr(1).rate).arg(r.byAddr(2).rate).arg(r.hostRate));
        check(tag + QStringLiteral("both keep it in flash"), r.byAddr(1).savedRate == to && r.byAddr(2).savedRate == to);
        check(tag + QStringLiteral("pings back and saved as they were"),
              r.byAddr(1).ds == dataset(80, 1u << 21) && r.byAddr(2).ds == dataset(80, 1u << 21)
              && r.byAddr(1).savedDs == dataset(80, 1u << 21) && r.byAddr(2).savedDs == dataset(80, 1u << 21));
    }
    {   // 2026-10-03: at 115200 with 50 ms pings the last board lost the first board's answer
        Ring r({1, 2}, img);
        r.byAddr(2).dropRelayWhilePinging = true;
        r.runRate(ratePlan({1, 2}, 230400));
        check(QStringLiteral("line rate, a pinging last board drops what it relays: pings off on it first, run succeeds"),
              r.rrOk && r.byAddr(1).rate == 230400 && r.byAddr(2).rate == 230400 && r.byAddr(1).savedRate == 230400, r.rrDetail);
    }
    {
        Ring r({1, 2}, img);
        r.byAddr(1).datasetAnswersToLose = 2;
        r.runRate(ratePlan({1, 2}, 460800));
        check(QStringLiteral("line rate, two ID_DATASET answers lost: asked again, run succeeds"), r.rrOk, r.rrDetail);
    }
    {
        Ring r({1, 2}, img);
        r.byAddr(1).datasetAnswersToLose = 3;
        r.runRate(ratePlan({1, 2}, 460800));
        check(QStringLiteral("line rate, three ID_DATASET answers lost: fails, nothing changed, pings back"),
              r.rrDone && !r.rrOk && r.byAddr(1).rate == kLine && r.byAddr(2).rate == kLine && pinging(r.byAddr(2).ds)
              && r.hostRate == kLine, r.rrDetail);
    }
    {
        Ring r({2}, img);
        r.runRate(ratePlan({2}, 230400));
        check(QStringLiteral("line rate 230400, a lone board: run, rate, flash"),
              r.rrOk && r.byAddr(2).rate == 230400 && r.byAddr(2).savedRate == 230400 && r.hostRate == 230400, r.rrDetail);
    }
    {
        Ring r({2, 1}, img);                              // wired the other way round
        r.runRate(ratePlan({1, 2}, 460800));
        check(QStringLiteral("line rate, ring wired the other way: fails, nothing saved, host back"),
              r.rrDone && !r.rrOk && r.byAddr(1).savedRate == kLine && r.byAddr(2).savedRate == kLine && r.hostRate == kLine,
              r.rrDetail);
    }
    {
        Ring r({1, 2}, img);
        r.byAddr(1).ignoreRate = true;
        r.runRate(ratePlan({1, 2}, 460800));
        check(QStringLiteral("line rate, the first board does not switch: fails, nothing saved, host back"),
              r.rrDone && !r.rrOk && r.byAddr(1).savedRate == kLine && r.byAddr(2).savedRate == kLine && r.hostRate == kLine,
              r.rrDetail);
    }
    {
        Ring r({1, 2}, img);
        r.byAddr(2).saveBroken = true;
        r.runRate(ratePlan({1, 2}, 460800));
        check(QStringLiteral("line rate, the last board's flash write lost: caught by the proof"),
              r.rrDone && !r.rrOk && r.rrDetail.contains(QLatin1String("did not keep")), r.rrDetail);
    }
    {
        Ring r({1, 2}, img);
        const bool started = r.runRate(ratePlan({1, 2}, 500000)) || !r.sim.q.empty();
        check(QStringLiteral("line rate 500000 (not in the firmware's table) refused before anything is sent"), !started && r.rrDone && !r.rrOk);
    }
    {   // the firmware update on a ring that runs at 460800 and keeps it
        Ring r({1, 2}, img);
        r.runRate(ratePlan({1, 2}, 460800));
        RingUpgrade::Plan p = plan({1, 2}, RingUpgrade::allTargets({1, 2}), img);
        p.lineRate = 460800;
        r.run(p);
        check(QStringLiteral("update on a ring at 460800 (bootloader taking 460800 in the model): both 1.12, back at 460800"),
              r.ok && r.byAddr(1).version == QLatin1String("1.12") && r.byAddr(2).version == QLatin1String("1.12")
              && r.byAddr(1).rate == 460800 && r.byAddr(2).rate == 460800 && r.hostRate == 460800, r.detail);
    }
}

// ---------------------------------------------------------------- live

// --live-rate COMx --to N [--from 921600]: the boards answering at --from move to N with RingRate
int liveRate(const QStringList& args)
{
    auto arg = [&args](const QString& k) { const int i = args.indexOf(k); return i >= 0 && i + 1 < args.size() ? args[i + 1] : QString(); };
    const QString port = arg(QStringLiteral("--live-rate"));
    const int from = arg(QStringLiteral("--from")).isEmpty() ? kLine : arg(QStringLiteral("--from")).toInt();
    const int to = arg(QStringLiteral("--to")).toInt();
    QSerialPort sp(port);
    sp.setBaudRate(from);
    if (port.isEmpty() || to <= 0 || !sp.open(QIODevice::ReadWrite)) {
        std::printf("need --live-rate COMx --to N [--from N]: %s\n", qPrintable(sp.errorString()));
        return 3;
    }
    QByteArray buf;
    std::function<void(const QByteArray&)> sink;
    auto pump = [&] {
        buf.append(sp.readAll());
        int i = 0;
        while (i + 8 <= buf.size()) {
            if (uint8_t(buf[i]) != 0xBB || uint8_t(buf[i + 1]) != 0x55) { ++i; continue; }
            const int n = uint8_t(buf[i + 5]);
            if (i + 8 + n > buf.size())
                break;
            uint8_t c1 = 0, c2 = 0;
            for (int j = i + 2; j < i + 6 + n; ++j) { c1 = uint8_t(c1 + uint8_t(buf[j])); c2 = uint8_t(c2 + c1); }
            if (c1 == uint8_t(buf[i + 6 + n]) && c2 == uint8_t(buf[i + 7 + n])) {
                if (sink) sink(buf.mid(i, 8 + n));
                i += 8 + n;
            } else {
                ++i;
            }
        }
        buf.remove(0, i);
    };
    QList<int> boards;
    sink = [&boards](const QByteArray& fr) {
        const auto k = parseKp1(fr);
        if (k && k->id == kIdVersion && k->type() == 1 && k->ver() == 2 && k->pl.size() >= 9 && uint8_t(k->pl[0]) == 0
            && uint8_t(k->pl[2]) == 12 && k->route >= 1 && k->route <= 15 && !boards.contains(k->route))
            boards.append(k->route);
    };
    sp.write(RingUpgrade::kp1(0, 0x03, kIdVersion));
    sp.flush();
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 600) {
        sp.waitForReadyRead(20);
        pump();
    }
    std::sort(boards.begin(), boards.end());
    QStringList bl;
    for (int b : boards) bl << QString::number(b);
    std::printf("ring boards on %s at %d: %s; to %d\n", qPrintable(port), from, qPrintable(bl.join(QLatin1Char(','))), to);
    QElapsedTimer clock;
    clock.start();
    RingRate::Io io;
    io.write = [&sp](const QByteArray& b) { sp.write(b); sp.flush(); };
    io.setRate = [&sp](int r) { sp.waitForBytesWritten(50); sp.setBaudRate(r); };
    io.nowMs = [&clock] { return clock.elapsed(); };
    RingRate rr(io);
    int result = 1;
    bool finished = false;
    QObject::connect(&rr, &RingRate::logLine, [&clock](const QString& s) {
        std::printf("[%7.3f] %s\n", double(clock.elapsed()) / 1000.0, qPrintable(s));
        std::fflush(stdout);
    });
    QObject::connect(&rr, &RingRate::finished, [&](bool ok, int r, const QString& d) {
        std::printf("%s at %d: %s\n", ok ? "DONE" : "FAILED", r, qPrintable(d));
        result = ok ? 0 : 1;
        finished = true;
    });
    sink = [&rr](const QByteArray& fr) { rr.onFrame(fr); };
    RingRate::Plan p;
    p.boards = boards;
    p.from = from;
    p.to = to;
    if (!rr.start(p))
        return 3;
    while (!finished) {
        sp.waitForReadyRead(5);
        pump();
        rr.tick();
    }
    return result;
}

int live(const QStringList& args)
{
    auto arg = [&args](const QString& k) { const int i = args.indexOf(k); return i >= 0 && i + 1 < args.size() ? args[i + 1] : QString(); };
    const QString port = arg(QStringLiteral("--live")), file = arg(QStringLiteral("--file"));
    const int rate = arg(QStringLiteral("--rate")).isEmpty() ? kLine : arg(QStringLiteral("--rate")).toInt();
    QFile f(file);
    if (port.isEmpty() || !f.open(QIODevice::ReadOnly)) {
        std::printf("need --live COMx --file F.ufw\n");
        return 3;
    }
    const QByteArray image = f.readAll();
    QString expect = arg(QStringLiteral("--expect"));
    if (expect.isEmpty()) {
        const auto m = QRegularExpression(QStringLiteral("_v(\\d+)\\.(\\d+)_")).match(QFileInfo(file).fileName());
        if (m.hasMatch())
            expect = m.captured(1) + QLatin1Char('.') + m.captured(2);
    }

    QSerialPort sp(port);
    sp.setBaudRate(rate);
    if (!sp.open(QIODevice::ReadWrite)) {
        std::printf("cannot open %s: %s\n", qPrintable(port), qPrintable(sp.errorString()));
        return 3;
    }
    QByteArray buf;
    std::function<void(const QByteArray&)> sink;
    auto pump = [&] {
        buf.append(sp.readAll());
        int i = 0;
        while (i + 8 <= buf.size()) {
            if (uint8_t(buf[i]) != 0xBB || uint8_t(buf[i + 1]) != 0x55) { ++i; continue; }
            const int n = uint8_t(buf[i + 5]);
            if (i + 8 + n > buf.size())
                break;
            uint8_t c1 = 0, c2 = 0;
            for (int j = i + 2; j < i + 6 + n; ++j) { c1 = uint8_t(c1 + uint8_t(buf[j])); c2 = uint8_t(c2 + c1); }
            if (c1 == uint8_t(buf[i + 6 + n]) && c2 == uint8_t(buf[i + 7 + n])) {
                if (sink) sink(buf.mid(i, 8 + n));
                i += 8 + n;
            } else {
                ++i;
            }
        }
        buf.remove(0, i);
    };
    QObject::connect(&sp, &QSerialPort::readyRead, pump);

    // discovery: the boards answering a broadcast ID_VERSION with a v2 from their firmware
    QList<int> boards;
    sink = [&boards](const QByteArray& fr) {
        const auto k = parseKp1(fr);
        if (k && k->id == kIdVersion && k->type() == 1 && k->ver() == 2 && k->pl.size() >= 9 && uint8_t(k->pl[0]) == 0
            && uint8_t(k->pl[2]) == 12 && k->route >= 1 && k->route <= 15 && !boards.contains(k->route))
            boards.append(k->route);
    };
    sp.write(RingUpgrade::kp1(0, 0x03, kIdVersion));
    sp.flush();
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 600) {
        sp.waitForReadyRead(20);
        pump();
    }
    std::sort(boards.begin(), boards.end());
    QList<int> targets;
    for (const QString& s : arg(QStringLiteral("--targets")).split(QLatin1Char(','), Qt::SkipEmptyParts))
        targets.append(s.toInt());
    if (targets.isEmpty())
        targets = RingUpgrade::allTargets(boards);
    auto joined = [](const QList<int>& l) { QStringList s; for (int v : l) s << QString::number(v); return s.join(QLatin1Char(',')); };
    std::printf("boards on %s: %s; targets %s; %lld bytes; expect %s\n", qPrintable(port),
                qPrintable(joined(boards)), qPrintable(joined(targets)),
                qint64(image.size()), qPrintable(expect.isEmpty() ? QStringLiteral("any") : expect));

    QElapsedTimer clock;
    clock.start();
    RingUpgrade::Io io;
    io.write = [&sp](const QByteArray& b) { sp.write(b); sp.flush(); };
    io.setRate = [&sp](int r) { sp.waitForBytesWritten(50); sp.setBaudRate(r); };
    io.nowMs = [&clock] { return clock.elapsed(); };
    RingUpgrade up(io);
    int result = 1;
    bool finished = false;
    QObject::connect(&up, &RingUpgrade::logLine, [&clock](const QString& s) {
        std::printf("[%7.3f] %s\n", double(clock.elapsed()) / 1000.0, qPrintable(s));
        std::fflush(stdout);
    });
    int shown = -1;
    QObject::connect(&up, &RingUpgrade::targetProgress, [&shown](int a, int p) {
        if (p / 20 != shown) { shown = p / 20; std::printf("          board %d: %3d%%\n", a, p); std::fflush(stdout); }
        if (p == 0) shown = 0;
    });
    QObject::connect(&up, &RingUpgrade::finished, [&](bool ok, const QString& d) {
        std::printf("%s: %s\n", ok ? "DONE" : "FAILED", qPrintable(d));
        result = ok ? 0 : 1;
        finished = true;
    });
    sink = [&up](const QByteArray& fr) { up.onFrame(fr); };
    RingUpgrade::Plan p;
    p.boards = boards;
    p.targets = targets;
    p.image = image;
    p.expect = expect;
    p.lineRate = rate;
    if (!up.start(p))
        return 3;
    while (!finished) {
        sp.waitForReadyRead(5);
        pump();
        up.tick();
    }
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--live")))
        return live(args);
    if (args.contains(QStringLiteral("--live-rate")))
        return liveRate(args);
    offline();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
