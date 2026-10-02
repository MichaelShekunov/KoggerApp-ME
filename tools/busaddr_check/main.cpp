// busaddr_check - drives BusAddress (src/device/bus_address.*) and reports PASS/FAIL per check.
//
//   busaddr_check                        offline: the class against simulated boards on one line -- a
//                                        lone board, two in a ring -- normal runs and failures
//   busaddr_check --live COM13 --set 1   a real board alone on the line: the run and its outcome
//
// Exit code 0 = every offline check passed / the live run succeeded. The simulated board follows the
// ring firmware's frame rules (firmware tree: sonar_cwchirp.c, frame_done and handle_frame);
// docs/RING_UPDATE.md tells how the live run was verified.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSerialPort>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "bus_address.h"
#include "ring_upgrade.h"

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

constexpr uint8_t kIdUart = 0x18, kIdFlash = 0x23, kIdChartZ = 0x7D;
constexpr uint32_t kKey = 0xC96B5D4Au;

struct Kp1 {
    uint8_t route = 0, mode = 0, id = 0;
    QByteArray pl;
    int type() const { return mode & 3; }
    int ver() const { return (mode >> 3) & 7; }
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

/// Discrete events on a microsecond clock.
struct Sim {
    qint64 now = 0;
    std::multimap<qint64, std::function<void()>> q;
    void after(qint64 dt, std::function<void()> f) { q.emplace(now + dt, std::move(f)); }
    void runUntil(const std::function<bool()>& stop, qint64 limit)
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
    }
};

/// A board with the ring firmware, as far as addresses go. At address 0 it handles every frame of up
/// to 64 B and relays nothing. With an address it handles commands for it or for 0, and relays what is
/// not its own -- only data once a board before it has shown up (its data came in). ID_UART v1 is
/// answered whatever the frame's type, with CONTENT v1 [key][1][address] from the route after the
/// change; ID_FLASH v0 saves the settings, v1 restores them, the address with them.
struct Board {
    Sim* sim = nullptr;
    int addr = 0, flash = 0;
    bool down = false;
    std::function<void(const QByteArray&)> tx;
    // test hooks
    bool noV1 = false;            // firmware before 1.12: no bus address
    bool saveBroken = false;      // ID_FLASH v0 writes nothing
    bool restoreIgnored = false;  // ID_FLASH v1 does nothing
    bool pinging = false;         // an echogram frame every 80 ms
    int sets = 0, saves = 0;

    void out(uint8_t mode, uint8_t id, const QByteArray& pl) { tx(RingUpgrade::kp1(uint8_t(addr), mode, id, pl)); }
    void changed() { down = false; }                         // chain_cfg(): the place in the ring is found anew

    void receive(const QByteArray& f)
    {
        const auto k = parseKp1(f);
        if (!k)
            return;
        if (addr == 0) {
            if (k->pl.size() <= 64)
                handle(*k);
            return;
        }
        if (k->type() == 1 && k->route != addr)
            down = true;
        const bool mine = k->route == addr || k->route == 0, cmd = k->type() == 2 || k->type() == 3;
        const bool fwd = k->route != addr && (!down || k->type() == 1);   // decided before the frame is handled
        if (mine && cmd && k->pl.size() <= 64)
            handle(*k);
        if (fwd && addr != 0)
            tx(f);
    }

    void handle(const Kp1& k)
    {
        if (k.id == kIdUart && k.ver() == 1) {
            if (noV1)
                return;
            if (k.type() == 2 && k.pl.size() >= 6 && u32(k.pl, 0) == kKey) {
                addr = uint8_t(k.pl[5]);
                ++sets;
                changed();
            }
            QByteArray r;
            for (int i = 0; i < 4; ++i)
                r.append(char((kKey >> (8 * i)) & 0xFF));
            r.append(char(1));
            r.append(char(addr));
            out(0x09, kIdUart, r);                                // CONTENT v1
        } else if (k.id == kIdFlash && k.type() == 2 && k.pl.size() >= 4 && u32(k.pl, 0) == kKey) {
            if (k.ver() == 0 && !saveBroken) {
                flash = addr;
                ++saves;
            }
            if (k.ver() == 1 && !restoreIgnored) {
                addr = flash;
                changed();
            }
        }
    }

    void schedulePing()
    {
        sim->after(80000, [this] {
            if (pinging)
                out(0x01, kIdChartZ, QByteArray(40, char(7)));
            schedulePing();
        });
    }
};

/// The host and its boards wired host -> boards[0] -> boards[1] -> host.
struct Line {
    Sim sim;
    std::vector<std::unique_ptr<Board>> boards;
    std::unique_ptr<BusAddress> engine;
    bool done = false, ok = false;
    int address = -2, sent = 0;
    QString detail;

    explicit Line(const std::vector<int>& addrsInWiringOrder)
    {
        for (int a : addrsInWiringOrder) {
            auto b = std::make_unique<Board>();
            b->sim = &sim;
            b->addr = b->flash = a;
            boards.push_back(std::move(b));
        }
        constexpr qint64 kHop = 200;                              // microseconds a frame takes to the next one
        for (size_t i = 0; i + 1 < boards.size(); ++i) {
            Board* next = boards[i + 1].get();
            boards[i]->tx = [this, next](const QByteArray& f) { sim.after(kHop, [next, f] { next->receive(f); }); };
        }
        if (!boards.empty())
            boards.back()->tx = [this](const QByteArray& f) { sim.after(kHop, [this, f] { if (engine) engine->onFrame(f); }); };

        BusAddress::Io io;
        io.write = [this](const QByteArray& f) {
            ++sent;
            if (!boards.empty()) {
                Board* first = boards[0].get();
                sim.after(kHop, [first, f] { first->receive(f); });
            }
        };
        io.nowMs = [this] { return sim.now / 1000; };
        engine = std::make_unique<BusAddress>(io);
        QObject::connect(engine.get(), &BusAddress::finished, [this](bool o, int a, const QString& d) {
            done = true;
            ok = o;
            address = a;
            detail = d;
        });
        if (qEnvironmentVariableIsSet("BUSADDR_CHECK_LOG"))
            QObject::connect(engine.get(), &BusAddress::logLine, [this](const QString& s) {
                std::printf("      [%6.3f] %s\n", double(sim.now) / 1e6, qPrintable(s));
            });
    }

    void run(int target)
    {
        for (auto& b : boards)
            if (b->pinging)
                b->schedulePing();
        if (!engine->start(target))
            return;                                               // finished(false) has come already
        std::function<void()> tick = [this, &tick] {
            engine->tick();
            if (!done)
                sim.after(5000, tick);
        };
        sim.after(5000, tick);
        sim.runUntil([this] { return done; }, sim.now + 20LL * 1000000);
        sim.runUntil([] { return false; }, sim.now + 300000);    // late frames settle
    }

    QString state() const
    {
        QStringList s;
        for (const auto& b : boards)
            s << QStringLiteral("%1/%2").arg(b->addr).arg(b->flash);
        return QStringLiteral("boards (RAM/flash): ") + s.join(QStringLiteral(", ")) + QStringLiteral("; ") + detail;
    }
};

void offline()
{
    {
        Line l({0});
        l.run(1);
        check("lone board at 0 -> 1: set and saved", l.done && l.ok && l.address == 1 && l.boards[0]->addr == 1 && l.boards[0]->flash == 1, l.state());
    }
    {
        Line l({2});
        l.run(1);
        check("lone board at 2 -> 1", l.ok && l.boards[0]->addr == 1 && l.boards[0]->flash == 1, l.state());
    }
    {
        Line l({1});
        l.run(0);
        check("lone board at 1 -> 0 (the proof moves it to 15)", l.ok && l.address == 0 && l.boards[0]->addr == 0 && l.boards[0]->flash == 0, l.state());
    }
    {
        Line l({1});
        l.boards[0]->pinging = true;
        l.run(2);
        check("lone board at 1, pinging, -> 2", l.ok && l.boards[0]->addr == 2 && l.boards[0]->flash == 2, l.state());
    }
    {
        Line l({1, 2});
        l.run(1);
        check("ring 1+2: refused, two answers, nothing changed",
              l.done && !l.ok && l.detail.contains(QStringLiteral("more than one board")) && l.boards[0]->sets == 0 && l.boards[1]->sets == 0
                  && l.boards[0]->flash == 1 && l.boards[1]->flash == 2, l.state());
    }
    {
        Line l({2, 0});
        l.run(1);
        check("ring, numbered board first (the 2026-10-01 mix-up): refused, nothing changed",
              l.done && !l.ok && l.boards[0]->sets == 0 && l.boards[1]->sets == 0 && l.boards[0]->addr == 2 && l.boards[1]->addr == 0, l.state());
    }
    {
        Line l({0, 2});
        l.run(1);
        check("ring, first board at 0 and the second numbered: the first gets 1, the second untouched",
              l.ok && l.boards[0]->addr == 1 && l.boards[0]->flash == 1 && l.boards[1]->sets == 0 && l.boards[1]->addr == 2 && l.boards[1]->flash == 2, l.state());
    }
    {
        Line l({0, 0});
        l.run(1);
        check("ring of two boards at 0: fails, nothing saved",
              l.done && !l.ok && l.boards[0]->saves == 0 && l.boards[1]->saves == 0 && l.boards[0]->flash == 0 && l.boards[1]->flash == 0, l.state());
    }
    {
        Line l({0});
        l.boards[0]->noV1 = true;
        l.run(1);
        check("firmware without bus address: fails, nothing changed",
              l.done && !l.ok && l.detail.contains(QStringLiteral("no board answered")) && l.boards[0]->sets == 0, l.state());
    }
    {
        Line l({});
        l.run(1);
        check("nothing on the line: fails", l.done && !l.ok && l.detail.contains(QStringLiteral("no board answered")), l.state());
    }
    {
        Line l({0});
        l.boards[0]->saveBroken = true;
        l.run(1);
        check("the save does not reach flash: caught", l.done && !l.ok && l.detail.contains(QStringLiteral("not saved")), l.state());
    }
    {
        Line l({0});
        l.boards[0]->saveBroken = true;
        l.boards[0]->restoreIgnored = true;
        l.run(1);
        check("the save does not reach flash and the restore does nothing: still caught (the address is moved away first)",
              l.done && !l.ok && l.detail.contains(QStringLiteral("not saved")), l.state());
    }
    {
        Line l({0});
        l.run(16);
        check("address 16 refused before anything is sent", l.done && !l.ok && l.sent == 0 && l.boards[0]->addr == 0, l.state());
    }
}

// ---------------------------------------------------------------- live

int live(const QString& port, int target)
{
    QSerialPort sp;
    sp.setPortName(port);
    sp.setBaudRate(921600);
    if (!sp.open(QIODevice::ReadWrite)) {
        std::printf("cannot open %s: %s\n", qPrintable(port), qPrintable(sp.errorString()));
        return 2;
    }
    QElapsedTimer clock;
    clock.start();
    BusAddress::Io io;
    io.write = [&sp](const QByteArray& b) { sp.write(b); sp.flush(); };
    io.nowMs = [&clock] { return clock.elapsed(); };
    BusAddress engine(io);
    int rc = 1;
    QObject::connect(&engine, &BusAddress::logLine, [&clock](const QString& s) {
        std::printf("[%6.3f] %s\n", double(clock.elapsed()) / 1000.0, qPrintable(s));
        std::fflush(stdout);
    });
    QObject::connect(&engine, &BusAddress::finished, [&rc](bool ok, int, const QString&) {
        rc = ok ? 0 : 1;
        QTimer::singleShot(100, qApp, &QCoreApplication::quit);
    });
    QByteArray buf;
    QObject::connect(&sp, &QSerialPort::readyRead, [&] {
        buf += sp.readAll();
        int i = 0;
        while (i + 8 <= buf.size()) {
            if (uint8_t(buf[i]) != 0xBB || uint8_t(buf[i + 1]) != 0x55) {
                ++i;
                continue;
            }
            const int end = i + 6 + uint8_t(buf[i + 5]);
            if (end + 2 > buf.size())
                break;
            uint8_t c1 = 0, c2 = 0;
            for (int j = i + 2; j < end; ++j) {
                c1 = uint8_t(c1 + uint8_t(buf[j]));
                c2 = uint8_t(c2 + c1);
            }
            if (c1 == uint8_t(buf[end]) && c2 == uint8_t(buf[end + 1])) {
                engine.onFrame(buf.mid(i, end + 2 - i));
                i = end + 2;
            } else {
                ++i;
            }
        }
        buf.remove(0, i);
    });
    QTimer t;
    t.setInterval(5);
    QObject::connect(&t, &QTimer::timeout, &engine, &BusAddress::tick);
    t.start();
    if (!engine.start(target))
        return 1;
    QCoreApplication::exec();
    return rc;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    const int li = args.indexOf(QStringLiteral("--live"));
    if (li >= 0) {
        const int si = args.indexOf(QStringLiteral("--set"));
        if (li + 1 >= args.size() || si < 0 || si + 1 >= args.size()) {
            std::printf("usage: busaddr_check --live COMx --set N\n");
            return 2;
        }
        return live(args[li + 1], args[si + 1].toInt());
    }
    offline();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
