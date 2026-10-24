// Offline check of the echogram codecs the app decodes (docs/ECHO_CODECS.md), against the test vectors of the
// codec's repository (KoggerEchoCodec test/app_vectors: 60 real pings compressed by each codec, and the cells a
// correct decoder returns):
//   kec_v1.vec, kec_v2.vec   ID_CHART_Z streams, KEC before and after feature blocks: one decoder for both
//   echocodec_v3.vec         ID_CHART V3 range blocks (SonarN), uniform and progressive pings, put together by
//                            ChartV3Assembler from frames given in reverse order
// and the assembler's own rules: a missing block from the previous ping of the same geometry, a late block of a
// published ping ignored, a broken block refused, a progressive ping mapped onto the native grid.
// Exit code 0 when every check passes.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "chart_codec_host.h"
#include "chart_v3.h"
extern "C" {
#include "kec.h"   // plain C, no guard of its own: as src/id_binnary.cpp includes it
}

static int g_checks = 0, g_failed = 0;

static void check(const char* what, bool ok, const std::string& detail)
{
    ++g_checks;
    if (!ok)
        ++g_failed;
    std::printf("%s  %s  -- %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
}

static std::vector<uint8_t> readFile(const std::string& path)
{
    std::vector<uint8_t> d;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return d;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    return d;
}

struct Reader {
    const std::vector<uint8_t>& d;
    size_t pos = 0;
    bool ok = true;
    template <typename T> T get()
    {
        T v{};
        if (pos + sizeof(T) > d.size()) { ok = false; return v; }
        std::memcpy(&v, d.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    const uint8_t* take(size_t n)
    {
        if (pos + n > d.size()) { ok = false; return nullptr; }
        const uint8_t* p = d.data() + pos;
        pos += n;
        return p;
    }
};

// "KECV" u32 count, then per ping: u16 n, u32 len, u8 coded[len], u8 expected[n]
static void kecFile(const std::string& dir, const char* name)
{
    const std::vector<uint8_t> d = readFile(dir + "/" + name);
    Reader r{ d };
    const uint8_t* magic = r.take(4);
    const uint32_t count = r.get<uint32_t>();
    if (!magic || std::memcmp(magic, "KECV", 4) != 0 || !r.ok) {
        check(name, false, "cannot read " + dir + "/" + name);
        return;
    }
    static uint8_t out[13000];
    uint32_t bad = 0, pings = 0;
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        const uint16_t n = r.get<uint16_t>();
        const uint32_t len = r.get<uint32_t>();
        const uint8_t* coded = r.take(len);
        const uint8_t* expected = r.take(n);
        if (!r.ok)
            break;
        ++pings;
        const int got = kec_decode_ping(coded, int(len), out, int(sizeof out));
        if (got != n || std::memcmp(out, expected, n) != 0)
            ++bad;
    }
    check(name, r.ok && pings == count && count > 0 && bad == 0,
          std::to_string(pings) + " of " + std::to_string(count) + " pings, " + std::to_string(bad) + " mismatched");
}

struct V3Ping {
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> expected;
};

// "ECV3" u32 count, then per ping: u8 nframes, nframes x (u8 len, u8 payload[len]), u16 n, u8 expected[n]
static std::vector<V3Ping> v3File(const std::string& path)
{
    std::vector<V3Ping> pings;
    const std::vector<uint8_t> d = readFile(path);
    Reader r{ d };
    const uint8_t* magic = r.take(4);
    const uint32_t count = r.get<uint32_t>();
    if (!magic || std::memcmp(magic, "ECV3", 4) != 0 || !r.ok)
        return pings;
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        V3Ping p;
        const uint8_t nf = r.get<uint8_t>();
        for (int k = 0; k < nf && r.ok; ++k) {
            const uint8_t len = r.get<uint8_t>();
            const uint8_t* f = r.take(len);
            if (f)
                p.frames.emplace_back(f, f + len);
        }
        const uint16_t n = r.get<uint16_t>();
        const uint8_t* e = r.take(n);
        if (!r.ok)
            break;
        p.expected.assign(e, e + n);
        pings.push_back(std::move(p));
    }
    return pings;
}

static std::string geometry(const std::vector<uint8_t>& f)
{
    return "mode " + std::to_string(f[0]) + ", " + std::to_string(f[1]) + " mm, count " + std::to_string(f[2] | (f[3] << 8));
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    kecFile(dir, "kec_v1.vec");
    kecFile(dir, "kec_v2.vec");

    const std::vector<V3Ping> v3 = v3File(dir + "/echocodec_v3.vec");
    check("echocodec_v3.vec read", !v3.empty(), std::to_string(v3.size()) + " pings");
    if (v3.empty()) {
        std::printf("\n%d checks, %d failed\nCODEC_CHECK_%s\n", g_checks, g_failed, "FAILED");
        return 1;
    }

    // Every ping, its frames in reverse order, one after another as a sonar sends them.
    {
        ChartV3Assembler a;
        std::vector<ChartV3Assembler::Ping> got;
        int refused = 0, uniform = 0, progressive = 0, missing = 0;
        for (const V3Ping& p : v3)
            for (auto it = p.frames.rbegin(); it != p.frames.rend(); ++it)
                if (!a.feed(it->data(), int(it->size()), got))
                    ++refused;
        a.flush(got);
        int bad = 0;
        bool native = true;
        for (size_t i = 0; i < got.size() && i < v3.size(); ++i) {
            if (got[i].bins != v3[i].expected)
                ++bad;
            const std::vector<uint8_t>& f = v3[i].frames.front();
            const uint16_t count = uint16_t(f[2] | (f[3] << 8));
            (got[i].mode ? progressive : uniform)++;
            missing += got[i].missingBlocks;
            if (got[i].cells.size() != count || got[i].resolMm != f[1])
                native = false;
            if (got[i].mode == 0 && got[i].cells != got[i].bins)
                native = false;
        }
        check("echocodec V3: every ping as the reference decodes it, blocks in reverse order",
              refused == 0 && got.size() == v3.size() && bad == 0,
              std::to_string(got.size()) + " of " + std::to_string(v3.size()) + " pings (" + std::to_string(uniform) +
                  " uniform, " + std::to_string(progressive) + " progressive), " + std::to_string(bad) + " mismatched, " +
                  std::to_string(refused) + " frames refused, " + std::to_string(missing) + " blocks not sent");
        check("echocodec V3: published on the native grid (count cells of resol_mm; uniform = the bins)", native,
              native ? "every ping" : "a ping off the native grid");
    }

    // A progressive ping on the native grid: the first band is native (10 mm) so it maps one to one; past it each
    // cell takes the bin of the band its middle falls in.
    {
        const V3Ping* prog = nullptr;
        for (const V3Ping& p : v3)
            if (p.frames.front()[0] == 1) { prog = &p; break; }
        bool ok = prog != nullptr;
        std::string detail = "no progressive ping in the vectors";
        if (prog) {
            ChartV3Assembler a;
            std::vector<ChartV3Assembler::Ping> got;
            for (const auto& f : prog->frames)
                a.feed(f.data(), int(f.size()), got);
            a.flush(got);
            const std::vector<uint8_t>& f0 = prog->frames.front();
            cch_band b[CCH_MAXBANDS];
            uint16_t n = 0;
            const int nb = cch_bands(f0[0], f0[1], uint16_t(f0[2] | (f0[3] << 8)), b, CCH_MAXBANDS, &n);
            ok = got.size() == 1 && nb >= 2 && b[0].resol_mm == f0[1] && b[0].start_mm == 0;
            if (ok) {
                const ChartV3Assembler::Ping& p = got[0];
                for (int i = 0; i < b[0].count && ok; ++i)
                    ok = p.cells[i] == p.bins[i];
                // the cell at the middle of band 1's first bin (start + resol/2) is that bin
                const uint32_t mid = b[1].start_mm + b[1].resol_mm / 2;
                const uint32_t cell = mid / f0[1];
                ok = ok && p.cells[cell] == p.bins[b[0].count] && p.bins.size() == n;
            }
            detail = geometry(f0) + ", " + std::to_string(nb) + " bands, " + std::to_string(n) + " bins -> " +
                     (got.empty() ? std::string("none") : std::to_string(got[0].cells.size())) + " cells";
        }
        check("echocodec V3: a progressive ping mapped onto the native grid band by band", ok, detail);
    }

    // Bands start at the ladder's edges (3 m, 6 m), not at the running sum of count x resol: at 11 mm a native cell
    // does not divide 3 m, and the running sum would put band 1 at 2992 mm. A mode the codec does not know is no
    // geometry at all.
    {
        cch_band b[CCH_MAXBANDS];
        uint16_t n = 0;
        const int nb = cch_bands(1, 11, 1000, b, CCH_MAXBANDS, &n);
        const bool edges = nb == 3 && b[0].start_mm == 0 && b[1].start_mm == 3000 && b[2].start_mm == 6000 &&
                           b[0].resol_mm == 11 && b[0].count == 272;
        uint16_t n2 = 0;
        const bool unknownMode = cch_bands(2, 10, 1000, b, CCH_MAXBANDS, &n2) == 0;
        std::vector<uint8_t> frame = v3[0].frames[0];
        frame[0] = 2;
        ChartV3Assembler a;
        std::vector<ChartV3Assembler::Ping> got;
        const bool refused = !a.feed(frame.data(), int(frame.size()), got);
        check("echocodec V3: band starts at the ladder's edges (11 mm cells); an unknown mode refused",
              edges && unknownMode && refused,
              std::to_string(nb) + " bands from " + std::to_string(nb > 1 ? b[1].start_mm : 0) + " mm; mode 2 " +
                  (unknownMode && refused ? "refused" : "taken"));
    }

    // A block lost on the way: taken from the previous published ping of the same geometry. Two uniform pings of one
    // log, nothing between them; the second without one block.
    {
        int ia = -1, ib = -1;
        for (size_t i = 0; i + 2 < v3.size() && ia < 0; ++i) {
            const auto& a = v3[i].frames.front();
            const auto& b = v3[i + 2].frames.front();
            if (a[0] == 0 && b[0] == 0 && a[1] == b[1] && a[2] == b[2] && a[3] == b[3] && v3[i + 2].frames.size() >= 2 &&
                v3[i].frames.size() == v3[i + 2].frames.size()) {
                ia = int(i);
                ib = int(i + 2);
            }
        }
        bool ok = ia >= 0;
        std::string detail = "no two uniform pings of one geometry";
        if (ok) {
            ChartV3Assembler a;
            std::vector<ChartV3Assembler::Ping> got;
            for (const auto& f : v3[ia].frames)
                a.feed(f.data(), int(f.size()), got);
            const auto& dropped = v3[ib].frames[1];
            const int blk = dropped[5];
            for (const auto& f : v3[ib].frames)
                if (&f != &dropped)
                    a.feed(f.data(), int(f.size()), got);
            a.flush(got);
            ok = got.size() == 2 && got[1].missingBlocks == 1;
            if (ok) {
                const size_t s = size_t(blk) * CCH_BLOCK_BINS;
                const size_t e = std::min(s + CCH_BLOCK_BINS, got[1].bins.size());
                ok = std::equal(got[1].bins.begin() + s, got[1].bins.begin() + e, got[0].bins.begin() + s) &&
                     !std::equal(got[1].bins.begin() + s, got[1].bins.begin() + e, v3[ib].expected.begin() + s);
            }
            detail = "pings " + std::to_string(ia) + " and " + std::to_string(ib) + ", block " + std::to_string(blk) +
                     " of the second dropped: " + (got.size() == 2 ? std::to_string(got[1].missingBlocks) : std::string("?")) +
                     " missing, filled from the first";
        }
        check("echocodec V3: a missing block comes from the previous ping of the same geometry", ok, detail);
    }

    // A late block of a published ping, while the next one is being collected: ignored, the next one goes on.
    {
        ChartV3Assembler a;
        std::vector<ChartV3Assembler::Ping> got;
        for (const auto& f : v3[0].frames)
            a.feed(f.data(), int(f.size()), got);
        const size_t afterFirst = got.size();
        const auto& next = v3[1].frames;
        bool ok = next.size() >= 2 && v3[0].frames.size() >= 1;
        if (ok) {
            a.feed(next[0].data(), int(next[0].size()), got);
            a.feed(v3[0].frames[0].data(), int(v3[0].frames[0].size()), got);   // late
            for (size_t k = 1; k < next.size(); ++k)
                a.feed(next[k].data(), int(next[k].size()), got);
            a.flush(got);
            ok = afterFirst == 1 && got.size() == 2 && got[1].missingBlocks == 0 && got[1].bins == v3[1].expected;
        }
        check("echocodec V3: a late block of a published ping changes nothing", ok,
              std::to_string(got.size()) + " pings published");
    }

    // A broken block is refused and does not end the ping it claims.
    {
        ChartV3Assembler a;
        std::vector<ChartV3Assembler::Ping> got;
        std::vector<uint8_t> bad = v3[0].frames[0];
        bad[5] = 200;                                          // a block far past the ping's end
        const bool refusedFar = !a.feed(bad.data(), int(bad.size()), got);
        const bool refusedShort = !a.feed(bad.data(), 3, got);
        for (const auto& f : v3[0].frames)
            a.feed(f.data(), int(f.size()), got);
        check("echocodec V3: a block past the ping's end, or shorter than its header, is refused",
              refusedFar && refusedShort && got.size() == 1 && got[0].bins == v3[0].expected,
              std::string(refusedFar ? "far refused" : "far taken") + ", " + (refusedShort ? "short refused" : "short taken"));
    }

    std::printf("\n%d checks, %d failed\nCODEC_CHECK_%s\n", g_checks, g_failed, g_failed ? "FAILED" : "DONE");
    return g_failed ? 1 : 0;
}
