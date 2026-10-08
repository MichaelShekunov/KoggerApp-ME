// Standalone check of src/device/node_probe.* (NodeProbe, canWrapNodeCmd, wrapNodeCmd).
//
//   nodeprobe_check        every offline check; exit code 0 = all passed
//
// The simulated recorder and boards follow what docs/RING_UPDATE.md records of the real ones:
//  * up: the recorder passes a board's own bus address as its route; a board with no address (0) gets
//    the recorder's port number instead (owner, 2026-10-05: the plaque follows the connector);
//  * down: route 0 goes to every sonar port, route N to port N; a port hands the frame, as it is, to
//    every board on it (2026-10-01);
//  * a board runs ID_NODE_CMD only when it names the board's address, a plain command on its own route
//    or, at address 0, on any route (bus_address.h), and relays every frame it gets back up.

#include "node_probe.h"

#include <QCoreApplication>
#include <QString>

#include <cstdio>
#include <vector>

using namespace Parsers;

namespace {

int failures = 0;

void check(bool ok, const QString& what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        ++failures;
}

bool fletcherOk(ProtoBinOut& f)
{
    const uint8_t* b = f.frame();
    const int n = f.frameLen();
    uint8_t c1 = 0, c2 = 0;
    for (int i = 2; i < n - 2; ++i) {
        c1 = uint8_t(c1 + b[i]);
        c2 = uint8_t(c2 + c1);
    }
    return c1 == b[n - 2] && c2 == b[n - 1];
}

struct Board {
    int address = 0;
    int port = 1;
    int delayMs = 20;     ///< answer latency
    bool deaf = false;    ///< relays every frame, runs none
};

int upRoute(const Board& b) { return b.address != 0 ? b.address : b.port; }

struct Delivery {
    qint64 at;
    int route;            ///< as the app sees it
    uint8_t id, ver, type;
    int boardAddress;     ///< in an ID_UART v1 CONTENT, else -1
};

struct Result {
    NodeProbe::Mode mode = NodeProbe::Mode::Unknown;
    int boardAddress = -2;
    bool finished = false;
    int sent = 0;
    qint64 tookMs = 0;
};

const char* modeName(NodeProbe::Mode m)
{
    switch (m) {
    case NodeProbe::Mode::Wrapped: return "wrapped";
    case NodeProbe::Mode::Plain:   return "plain";
    default:                       return "unknown";
    }
}

/// The probe of the device the app sees on `deviceRoute`, its wrapper naming `named`, ticked every
/// `tickMs` (the app's process() runs every 200 ms).
Result run(const std::vector<Board>& boards, int deviceRoute, int named, int tickMs)
{
    qint64 now = 0;
    std::vector<Delivery> pending;
    Result r;

    NodeProbe::Io io;
    io.nowMs = [&now] { return now; };
    io.send = [&](ProtoBinOut out) {
        ++r.sent;
        const uint8_t* f = out.frame();
        const int route = f[2];
        for (const Board& b : boards) {
            if (route != 0 && route != b.port)
                continue;
            pending.push_back({now + 5, upRoute(b), f[4], uint8_t((f[3] >> 3) & 7), uint8_t(f[3] & 3), -1});
            if (b.deaf)
                continue;
            uint8_t mode = 0, id = 0;
            if (f[4] == ID_NODE_CMD) {
                if (f[5] < 3 || f[6] != b.address)
                    continue;
                mode = f[7];
                id = f[8];
            } else {
                if (b.address != 0 && route != b.address)
                    continue;
                mode = f[3];
                id = f[4];
            }
            if (id == ID_UART && (mode & 3) == GETTING && ((mode >> 3) & 7) == 1)
                pending.push_back({now + b.delayMs, upRoute(b), uint8_t(ID_UART), 1, uint8_t(CONTENT), b.address});
        }
    };

    NodeProbe probe(io);
    QObject::connect(&probe, &NodeProbe::finished, [&](NodeProbe::Mode mode, int address, const QString&) {
        r.mode = mode;
        r.boardAddress = address;
        r.finished = true;
        r.tookMs = now;
    });
    probe.start(uint8_t(deviceRoute), uint8_t(named));
    while (probe.active() && now < 30000) {
        now += 5;
        for (size_t i = 0; i < pending.size();) {
            if (pending[i].at > now) {
                ++i;
                continue;
            }
            const Delivery d = pending[i];
            pending.erase(pending.begin() + long(i));
            if (d.route == deviceRoute)
                probe.onFrame(d.id, d.ver, d.type, d.boardAddress);
        }
        if (now % tickMs == 0)
            probe.tick();
    }
    return r;
}

void expect(const QString& name, const std::vector<Board>& boards, int deviceRoute, int named,
            NodeProbe::Mode mode, int boardAddress)
{
    for (int tickMs : {5, 200}) {
        const Result r = run(boards, deviceRoute, named, tickMs);
        check(r.finished && r.mode == mode && r.boardAddress == boardAddress,
              QStringLiteral("%1, tick %2 ms: %3, board %4 in %5 ms")
                  .arg(name).arg(tickMs).arg(modeName(r.mode)).arg(r.boardAddress).arg(r.tookMs));
    }
}

void wrapperBytes()
{
    ProtoBinOut get;
    get.create(GETTING, v1, ID_UART, 1);
    get.end();
    check(canWrapNodeCmd(get), QStringLiteral("GET without payload can be wrapped"));
    ProtoBinOut w = wrapNodeCmd(get, 7);
    const uint8_t* f = w.frame();
    check(f[0] == 0xBB && f[1] == 0x55 && f[2] == 0 && f[4] == ID_NODE_CMD && (f[3] & 3) == GETTING,
          QStringLiteral("wrapper: KP1, route 0, ID_NODE_CMD, the inner command's type"));
    check(f[5] == 3 && f[6] == 7 && f[7] == get.frame()[3] && f[8] == ID_UART,
          QStringLiteral("wrapper payload: [address][mode][id]"));
    check(fletcherOk(w), QStringLiteral("wrapper checksum"));

    ProtoBinOut set;
    set.create(SETTING, v0, ID_UART, 2);
    set.write<U1>(0x11);
    set.write<U1>(0x22);
    set.end();
    ProtoBinOut ws = wrapNodeCmd(set, 2);
    const uint8_t* g = ws.frame();
    check(g[5] == 5 && g[6] == 2 && g[9] == 0x11 && g[10] == 0x22 && (g[3] & 3) == SETTING && fletcherOk(ws),
          QStringLiteral("wrapper carries the inner payload"));

    ProtoBinOut content;
    content.create(CONTENT, v0, ID_UART, 1);
    content.end();
    check(!canWrapNodeCmd(content), QStringLiteral("CONTENT is not wrapped"));

    for (int len : {61, 62}) {
        ProtoBinOut big;
        big.create(SETTING, v0, ID_UART, 1);
        for (int i = 0; i < len; ++i)
            big.write<U1>(uint8_t(i));
        big.end();
        check(canWrapNodeCmd(big) == (len <= 61), QStringLiteral("%1 payload bytes: wrapped %2").arg(len).arg(len <= 61));
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    using M = NodeProbe::Mode;

    wrapperBytes();

    // The owner's case: one board, no address, on either port. The app sees the port as its route.
    expect(QStringLiteral("lone board, address 0, port 1"), {{0, 1}}, 1, 1, M::Plain, 0);
    expect(QStringLiteral("lone board, address 0, port 2"), {{0, 2}}, 2, 2, M::Plain, 0);

    // The 2026-10-01 case: a ring of boards 1 and 2 on one port; each takes only what names it.
    for (int port : {1, 2}) {
        const std::vector<Board> ring{{1, port}, {2, port}};
        expect(QStringLiteral("ring 1+2 on port %1, board 1").arg(port), ring, 1, 1, M::Wrapped, 1);
        expect(QStringLiteral("ring 1+2 on port %1, board 2").arg(port), ring, 2, 2, M::Wrapped, 2);
    }

    // A lone board that kept a ring address: its own route comes up, the wrapper names it.
    expect(QStringLiteral("lone board, address 3, port 1"), {{3, 1}}, 3, 3, M::Wrapped, 3);
    // KOGGER_NODE_ADDR=0 on the owner's board: the wrapper names 0 and the board takes it.
    expect(QStringLiteral("lone board, address 0, port 2, wrapper naming 0"), {{0, 2}}, 2, 0, M::Wrapped, 0);

    // Slow but inside the wait: still right.
    expect(QStringLiteral("lone board, address 0, answering in 500 ms"), {{0, 1, 500}}, 1, 1, M::Plain, 0);

    // Later than the wait but inside the drain: dropped every time, never taken for the wrapped request.
    expect(QStringLiteral("lone board, address 0, answering in 900 ms"), {{0, 1, 900}}, 1, 1, M::Unknown, -1);

    // A board that only relays: its echoes of the requests are not answers.
    {
        const Result r = run({{0, 1, 20, true}}, 1, 1, 200);
        check(r.finished && r.mode == M::Unknown && r.sent == 2 * NodeProbe::kRounds,
              QStringLiteral("relaying board, nothing run: %1 after %2 requests").arg(modeName(r.mode)).arg(r.sent));
    }

    // No restart while a run is going on.
    {
        NodeProbe::Io io;
        qint64 now = 0;
        io.nowMs = [&now] { return now; };
        io.send = [](ProtoBinOut) {};
        NodeProbe p(io);
        check(p.start(1, 1) && !p.start(2, 2), QStringLiteral("a second start during a run is refused"));
    }

    std::printf("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
