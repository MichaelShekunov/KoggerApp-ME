// A sampling profiler for the threads of a running Windows x64 process.
//   sampler <pid> <seconds> <interval_ms> <exe_symbols.txt> [<module_name_of_exe> [<focus>|- [<timeline file>]]]
// Every interval each thread of the process that has run since its last sample (QueryThreadCycleTime)
// is suspended, its stack walked (dbghelp StackWalk64 on the target's unwind data), and resumed. At the
// end, per thread with samples: the leaf functions and the functions on the stack (inclusive), ranked.
// Symbols: dbghelp for DLLs (exports), and for the exe a table "hexaddr name" (llvm-nm -C, preferred
// image base), since a MinGW exe has no PDB.
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

struct Sym { unsigned long long addr; std::string name; };

static std::vector<Sym> loadSyms(const char* path)
{
    std::vector<Sym> v;
    FILE* f = std::fopen(path, "rb");
    if (!f) return v;
    char line[4096];
    while (std::fgets(line, sizeof line, f)) {
        char* end = nullptr;
        unsigned long long a = std::strtoull(line, &end, 16);
        if (!end || end == line) continue;
        while (*end == ' ') ++end;
        // "T name" or "t name"
        if ((end[0] == 'T' || end[0] == 't') && end[1] == ' ') {
            std::string n(end + 2);
            while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
            v.push_back({a, n});
        }
    }
    std::fclose(f);
    std::sort(v.begin(), v.end(), [](const Sym& x, const Sym& y) { return x.addr < y.addr; });
    return v;
}

int main(int argc, char** argv)
{
    if (argc < 5) {
        std::fprintf(stderr, "usage: sampler <pid> <seconds> <interval_ms> <exe_symbols.txt> [exe module name]\n");
        return 2;
    }
    const DWORD pid = DWORD(std::strtoul(argv[1], nullptr, 10));
    const double seconds = std::atof(argv[2]);
    const DWORD intervalMs = DWORD(std::strtoul(argv[3], nullptr, 10));
    const std::vector<Sym> exeSyms = loadSyms(argv[4]);
    const std::string exeName = argc > 5 ? argv[5] : "KoggerApp.exe";
    std::string focus = argc > 6 ? argv[6] : "";   // print who calls functions named like this ("-": none)
    if (focus == "-") focus.clear();
    // every sample, in time order: "<thread> <ms from the start> <function>;<function>;..." (innermost first), to
    // find what one thread did in a short stretch (a jerk) that the totals hide
    const std::string timelinePath = argc > 7 ? argv[7] : "";
    struct TimelineRec { DWORD tid; double ms; std::vector<unsigned long long> fs; };
    std::vector<TimelineRec> timeline;

    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { std::fprintf(stderr, "OpenProcess failed %lu\n", GetLastError()); return 1; }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS);
    if (!SymInitialize(proc, nullptr, TRUE)) { std::fprintf(stderr, "SymInitialize failed %lu\n", GetLastError()); }

    // exe module base, for the symbol table (preferred base 0x140000000)
    unsigned long long exeBase = 0;
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        MODULEENTRY32 me; me.dwSize = sizeof me;
        if (snap != INVALID_HANDLE_VALUE && Module32First(snap, &me)) {
            do {
                if (_stricmp(me.szModule, exeName.c_str()) == 0) exeBase = (unsigned long long)me.modBaseAddr;
            } while (Module32Next(snap, &me));
        }
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    }
    const unsigned long long exeSize = 0x10000000ULL;

    struct ThreadAcc {
        HANDLE h = nullptr;
        ULONG64 lastCycles = 0;
        unsigned long long samples = 0;
        std::unordered_map<unsigned long long, unsigned long long> leaf;
        std::unordered_map<unsigned long long, unsigned long long> incl;   // by function start
        std::map<std::vector<unsigned long long>, unsigned long long> stacks; // by function starts, top 6
        std::vector<std::vector<unsigned long long>> full;                    // every sample, outermost last
    };
    std::map<DWORD, ThreadAcc> threads;

    auto funcStart = [&](unsigned long long a, std::string* name) -> unsigned long long {
        if (exeBase && a >= exeBase && a < exeBase + exeSize && !exeSyms.empty()) {
            const unsigned long long rel = a - exeBase + 0x140000000ULL;
            auto it = std::upper_bound(exeSyms.begin(), exeSyms.end(), rel,
                                       [](unsigned long long v, const Sym& s) { return v < s.addr; });
            if (it != exeSyms.begin()) {
                --it;
                if (name) *name = it->name;
                return it->addr - 0x140000000ULL + exeBase;
            }
        }
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
        auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 511;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, a, &disp, si)) {
            if (name) {
                char mod[MAX_PATH] = "";
                IMAGEHLP_MODULE64 mi; mi.SizeOfStruct = sizeof mi;
                if (SymGetModuleInfo64(proc, a, &mi)) std::snprintf(mod, sizeof mod, "%s!", mi.ModuleName);
                *name = std::string(mod) + si->Name;
            }
            return a - disp;
        }
        if (name) {
            IMAGEHLP_MODULE64 mi; mi.SizeOfStruct = sizeof mi;
            char tmp[128];
            if (SymGetModuleInfo64(proc, a, &mi)) std::snprintf(tmp, sizeof tmp, "%s+0x%llx", mi.ModuleName, a - mi.BaseOfImage);
            else std::snprintf(tmp, sizeof tmp, "0x%llx", a);
            *name = tmp;
        }
        return a;
    };

    const DWORD self = GetCurrentThreadId();
    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    DWORD lastEnum = 0;
    timeBeginPeriod(1);
    for (;;) {
        QueryPerformanceCounter(&now);
        const double el = double(now.QuadPart - t0.QuadPart) / double(freq.QuadPart);
        if (el >= seconds) break;
        DWORD exitCode = 0;
        if (GetExitCodeProcess(proc, &exitCode) && exitCode != STILL_ACTIVE) break;
        const DWORD tick = GetTickCount();
        if (tick - lastEnum > 500 || lastEnum == 0) {
            lastEnum = tick;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te; te.dwSize = sizeof te;
            if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                    if (threads.count(te.th32ThreadID)) continue;
                    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                    if (h) { threads[te.th32ThreadID].h = h; }
                } while (Thread32Next(snap, &te));
            }
            if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        }
        for (auto& [tid, acc] : threads) {
            ULONG64 cyc = 0;
            if (!QueryThreadCycleTime(acc.h, &cyc)) continue;
            // Suspending a thread runs a little of it (the suspend APC): a waiting thread moves too.
            const bool ran = acc.lastCycles != 0 && cyc - acc.lastCycles > 200000;
            acc.lastCycles = cyc;
            if (!ran) continue;
            if (SuspendThread(acc.h) == DWORD(-1)) continue;
            CONTEXT ctx; ZeroMemory(&ctx, sizeof ctx);
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(acc.h, &ctx)) {
                STACKFRAME64 sf; ZeroMemory(&sf, sizeof sf);
                sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
                sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
                sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
                std::vector<unsigned long long> pcs;
                for (int i = 0; i < 64; ++i) {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, acc.h, &sf, &ctx, nullptr,
                                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
                    if (!sf.AddrPC.Offset) break;
                    pcs.push_back(sf.AddrPC.Offset);
                }
                ResumeThread(acc.h);
                if (!pcs.empty()) {
                    acc.samples++;
                    std::vector<unsigned long long> fs;
                    fs.reserve(pcs.size());
                    for (auto pc : pcs) fs.push_back(funcStart(pc, nullptr));
                    acc.leaf[fs.front()]++;
                    std::vector<unsigned long long> seen;
                    for (auto f : fs) {
                        if (std::find(seen.begin(), seen.end(), f) != seen.end()) continue;
                        seen.push_back(f);
                        acc.incl[f]++;
                    }
                    std::vector<unsigned long long> top(fs.begin(), fs.begin() + std::min<size_t>(fs.size(), 6));
                    acc.stacks[top]++;
                    if (!focus.empty()) acc.full.push_back(fs);
                    if (!timelinePath.empty()) timeline.push_back({ tid, el * 1000.0, fs });
                }
            } else {
                ResumeThread(acc.h);
            }
        }
        Sleep(intervalMs);
    }
    timeEndPeriod(1);

    std::vector<std::pair<DWORD, ThreadAcc*>> order;
    for (auto& [tid, acc] : threads) if (acc.samples > 0) order.push_back({tid, &acc});
    std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.second->samples > b.second->samples; });
    for (auto& [tid, accRef] : order) {
        ThreadAcc* acc = accRef;
        std::printf("=== thread %lx: %llu samples\n", tid, acc->samples);
        auto rank = [&](const std::unordered_map<unsigned long long, unsigned long long>& m, const char* what, int n) {
            std::vector<std::pair<unsigned long long, unsigned long long>> v(m.begin(), m.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
            std::printf("  %s:\n", what);
            for (int i = 0; i < int(v.size()) && i < n; ++i) {
                std::string nm;
                funcStart(v[i].first, &nm);
                std::printf("    %5.1f%%  %s\n", 100.0 * double(v[i].second) / double(acc->samples), nm.c_str());
            }
        };
        rank(acc->leaf, "leaf", 25);
        rank(acc->incl, "inclusive", 40);
        std::vector<std::pair<std::vector<unsigned long long>, unsigned long long>> sv(acc->stacks.begin(), acc->stacks.end());
        std::sort(sv.begin(), sv.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if (!focus.empty()) {
            // The 6 frames outward from the outermost frame whose name has the focus text.
            std::map<std::vector<unsigned long long>, unsigned long long> callers;
            std::unordered_map<unsigned long long, std::string> names;
            unsigned long long hits = 0;
            for (const auto& fs : acc->full) {
                int at = -1;
                for (int i = int(fs.size()) - 1; i >= 0; --i) {
                    auto it = names.find(fs[i]);
                    if (it == names.end()) { std::string nm; funcStart(fs[i], &nm); it = names.emplace(fs[i], nm).first; }
                    if (it->second.find(focus) != std::string::npos) { at = i; break; }
                }
                if (at < 0) continue;
                ++hits;
                std::vector<unsigned long long> chain(fs.begin() + at, fs.begin() + std::min<size_t>(fs.size(), size_t(at) + 7));
                callers[chain]++;
            }
            std::vector<std::pair<std::vector<unsigned long long>, unsigned long long>> cv(callers.begin(), callers.end());
            std::sort(cv.begin(), cv.end(), [](auto& a, auto& b) { return a.second > b.second; });
            std::printf("  callers of '%s' (%llu samples, %.1f%%), outward:\n", focus.c_str(), hits, 100.0 * double(hits) / double(acc->samples));
            for (int i = 0; i < int(cv.size()) && i < 12; ++i) {
                std::printf("    %5.1f%%", 100.0 * double(cv[i].second) / double(acc->samples));
                for (auto f : cv[i].first) { std::printf("  -> %s", names.count(f) ? names[f].c_str() : "?"); }
                std::printf("\n");
            }
        }
        std::printf("  top stacks (innermost first):\n");
        for (int i = 0; i < int(sv.size()) && i < 8; ++i) {
            std::printf("    %5.1f%%", 100.0 * double(sv[i].second) / double(acc->samples));
            for (auto f : sv[i].first) { std::string nm; funcStart(f, &nm); std::printf("  <- %s", nm.c_str()); }
            std::printf("\n");
        }
    }
    if (!timelinePath.empty()) {
        FILE* out = std::fopen(timelinePath.c_str(), "w");
        if (!out) { std::fprintf(stderr, "timeline: cannot write %s\n", timelinePath.c_str()); return 1; }
        std::unordered_map<unsigned long long, std::string> names;
        for (const TimelineRec& r : timeline) {
            std::fprintf(out, "%lx %.2f ", r.tid, r.ms);
            for (size_t i = 0; i < r.fs.size() && i < 40; ++i) {
                auto it = names.find(r.fs[i]);
                if (it == names.end()) { std::string nm; funcStart(r.fs[i], &nm); it = names.emplace(r.fs[i], nm).first; }
                std::fprintf(out, "%s%s", i ? ";" : "", it->second.c_str());
            }
            std::fprintf(out, "\n");
        }
        std::fclose(out);
    }
    return 0;
}
