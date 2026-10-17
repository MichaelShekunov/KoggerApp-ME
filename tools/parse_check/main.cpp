// Feeds a log file to Parsers::FrameParser as DeviceManager::openFile does (1 MiB chunks, process()
// until the chunk is used up) and prints a fingerprint of every completed frame plus the parser's
// counters and the time taken. Built against two versions of proto_binnary.h (compare.ps1), the two
// outputs must match but for the time.
#include "proto_binnary.h"

#include <chrono>
#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: parsecheck <file> [repeats]\n"); return 2; }
    const int repeats = argc > 2 ? std::atoi(argv[2]) : 1;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::fprintf(stderr, "cannot open\n"); return 1; }
    std::vector<uint8_t> all;
    {
        std::vector<uint8_t> buf(1 << 20);
        size_t n;
        while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) all.insert(all.end(), buf.begin(), buf.begin() + n);
        std::fclose(f);
    }
    double best = 1e9;
    unsigned long long hash = 0, frames = 0, bytes = 0, byProto[8] = {};
    uint32_t frameError = 0, binError = 0, complete = 0;
    for (int r = 0; r < repeats; ++r) {
        Parsers::FrameParser parser;
        hash = 1469598103934665603ULL; frames = 0; bytes = 0;
        for (auto& c : byProto) c = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t off = 0; off < all.size(); off += (1 << 20)) {
            const size_t len = std::min<size_t>(1 << 20, all.size() - off);
            parser.setContext(all.data() + off, uint32_t(len));
            while (parser.availContext() > 0) {
                parser.process();
                if (parser.isComplete()) {
                    const uint8_t* fr = parser.frame();
                    const unsigned fl = parser.frameLen();
                    hash = (hash ^ uint64_t(parser.proto())) * 1099511628211ULL;
                    hash = (hash ^ fl) * 1099511628211ULL;
                    for (unsigned i = 0; i < fl; ++i) hash = (hash ^ fr[i]) * 1099511628211ULL;
                    frames++; bytes += fl; byProto[parser.proto() & 7]++;
                }
            }
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (s < best) best = s;
        frameError = parser.frameError(); binError = parser.binError(); complete = parser.getCompleteTotal();
    }
    std::printf("frames %llu, bytes %llu, hash %016llx, frameError %u, binError %u, completeTotal %u\n",
                frames, bytes, hash, frameError, binError, complete);
    std::printf("by proto:");
    for (int i = 0; i < 8; ++i) std::printf(" %llu", byProto[i]);
    std::printf("\nparse %.3f s best of %d (%.0f MB/s)\n", best, repeats, double(all.size()) / 1e6 / best);
    return 0;
}
