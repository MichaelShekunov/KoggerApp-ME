// Offline check of src/scene2d/plot2D_columns.h: which ping each screen column of the 2D echogram shows at a
// horizontal scale (Plot2D::reindexingCursor), where the echogram's ring cache keeps it
// (Plot2DEchogram::updateCash), which pings an edit over a column reaches (Plot2D::epochSpanOfColumn,
// columnOfEpoch) and where a change of the scale puts the view (Plot2D::stepHorizontalScale).
// Prints a line per check and FAIL lines; exit code 0 when everything holds.

#include "plot2D_columns.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace plot2d_columns;

namespace {

int g_checks = 0;
int g_failed = 0;

std::string fmt(const char* f, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

// A named check: the first failing cases are printed, the check counts once.
struct Check {
    const char* name;
    int failures = 0;
    long long cases = 0;

    explicit Check(const char* n) : name(n) {}
    void expect(bool ok, const std::string& what)
    {
        ++cases;
        if (!ok) {
            if (failures < 3) {
                std::printf("FAIL %s: %s\n", name, what.c_str());
            }
            ++failures;
        }
    }
    void done()
    {
        ++g_checks;
        if (failures > 0) {
            ++g_failed;
            std::printf("     %s: %d of %lld cases failed\n", name, failures, cases);
        }
        else {
            std::printf("ok   %s (%lld cases)\n", name, cases);
        }
    }
};

const int kWidths[] = { 1, 2, 3, 7, 64, 640, 1921 };
const int kSizes[] = { 0, 1, 3, 50, 5000, 200000 };
const int kBig = 1000000;   // a recording long enough that no view reaches its end

// What Plot2D::reindexingCursor asks for: the columns for a head (position times the dataset size).
std::vector<int> view(long long head, int width, float scale, int dataWidth, long long* v0 = nullptr)
{
    std::vector<int> out(size_t(width), -2);
    const long long first = fillColumns(int(head - 1), width, scale, dataWidth, out.data());
    if (v0) {
        *v0 = first;
    }
    return out;
}

std::vector<long long> headsFor(int size)
{
    std::vector<long long> h = { 0, 1, 2, 3, 4, 5, 17, size / 3, size / 2, size - 2, size - 1, size };
    h.erase(std::remove_if(h.begin(), h.end(), [&](long long x) { return x < 0 || x > size; }), h.end());
    std::sort(h.begin(), h.end());
    h.erase(std::unique(h.begin(), h.end()), h.end());
    return h;
}

// The column the scale-1 code before the horizontal scale gave (git 8352bc4b, Plot2D::reindexingCursor).
int oldIndex(long long head, int i, int width, int dataWidth)
{
    const float hor_ratio = 1.0f;
    const long long d = head + std::lround((i - width) / hor_ratio) - 1;
    return (d >= 0 && d < dataWidth) ? int(d) : -1;
}

// The ring turn of the code before (Plot2DEchogram::updateCash), when some column shows a ping above 0.
bool oldWrap(const std::vector<int>& idx, int width, int* wrap)
{
    for (int i = 0; i < width; ++i) {
        if (idx[size_t(i)] > 0) {
            *wrap = std::abs((idx[size_t(i)] + (width - i)) % width);
            return true;
        }
    }
    return false;
}

// The view column Plot2DEchogram::updateCash draws into ring slot `slot` at the turn `wrap`.
int columnInSlot(int slot, int wrap, int width)
{
    int cursor_pos = slot - wrap;
    if (slot < wrap) {
        cursor_pos += width;
    }
    return cursor_pos;
}

// The ping in each ring slot, as updateCash fills the ring for a view.
std::vector<int> ring(const std::vector<int>& cols, long long v0, int width)
{
    const int wrap = ringColumn(v0, width);
    std::vector<int> slots(size_t(width), -2);
    for (int s = 0; s < width; ++s) {
        const int c = columnInSlot(s, wrap, width);
        slots[size_t(s)] = (c >= 0 && c < width) ? cols[size_t(c)] : -3;
    }
    return slots;
}

void checkSteps()
{
    {
        Check c("steps: each step is its own nearest");
        for (int i = 0; i < kScaleCount; ++i) {
            c.expect(scaleIndex(kScales[i]) == i && snapScale(kScales[i]) == kScales[i],
                     fmt("step %d (%g) -> index %d", i, double(kScales[i]), scaleIndex(kScales[i])));
        }
        c.done();
    }
    {
        Check c("steps: zero, negative, NaN and infinity are 1");
        const float bad[] = { 0.0f, -1.0f, -0.25f, std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() };
        for (float b : bad) {
            c.expect(snapScale(b) == 1.0f, fmt("%g -> %g", double(b), double(snapScale(b))));
        }
        c.done();
    }
    {
        // Between two steps the nearer in proportion: 0.72 is past the geometric middle of 1/2 and 1 (0.707)
        // but short of the arithmetic one (0.75).
        Check c("steps: the nearest in proportion");
        const float in[][2] = { { 0.72f, 1.0f }, { 0.70f, 0.5f }, { 1.40f, 1.0f }, { 1.43f, 2.0f }, { 4.8f, 4.0f },
                                { 5.0f, 6.0f }, { 100.0f, 8.0f }, { 0.001f, 1.0f / 16 }, { 0.085f, 1.0f / 16 },
                                { 0.09f, 1.0f / 8 }, { 0.18f, 1.0f / 4 }, { 2.4f, 2.0f }, { 2.5f, 3.0f } };
        for (const auto& p : in) {
            c.expect(snapScale(p[0]) == p[1], fmt("%g -> %g, want %g", double(p[0]), double(snapScale(p[0])),
                                                  double(p[1])));
        }
        c.done();
    }
    {
        Check c("steps: a step up and down, held at the ends");
        for (int i = 0; i < kScaleCount; ++i) {
            for (int steps = -12; steps <= 12; ++steps) {
                const int want = std::clamp(i + steps, 0, kScaleCount - 1);
                c.expect(stepScale(kScales[i], steps) == kScales[want],
                         fmt("%g by %d -> %g, want %g", double(kScales[i]), steps,
                             double(stepScale(kScales[i], steps)), double(kScales[want])));
            }
        }
        c.done();
    }
    {
        Check c("steps: stretch and compression are the scale");
        for (int i = 0; i < kScaleCount; ++i) {
            const float s = kScales[i];
            const bool ok = s >= 1.0f ? (stretchOf(s) == int(s) && compressOf(s) == 1)
                                      : (compressOf(s) == int(std::lround(1.0f / s)) && stretchOf(s) == 1);
            c.expect(ok, fmt("%g: stretch %d, compression %d", double(s), stretchOf(s), compressOf(s)));
        }
        c.done();
    }
}

void checkScaleOne()
{
    Check cols("scale 1: the columns of the code before");
    Check turn("scale 1: the ring turns as before");
    for (int w : kWidths) {
        for (int size : kSizes) {
            for (long long head : headsFor(size)) {
                long long v0 = 0;
                const std::vector<int> v = view(head, w, 1.0f, size, &v0);
                bool same = true;
                int at = -1;
                for (int i = 0; i < w && same; ++i) {
                    if (v[size_t(i)] != oldIndex(head, i, w, size)) {
                        same = false;
                        at = i;
                    }
                }
                cols.expect(same, fmt("width %d size %d head %lld: column %d shows %d, before %d", w, size, head,
                                      at, at >= 0 ? v[size_t(at)] : 0, at >= 0 ? oldIndex(head, at, w, size) : 0));
                int wrap = 0;
                if (oldWrap(v, w, &wrap)) {
                    turn.expect(ringColumn(v0, w) == wrap, fmt("width %d size %d head %lld: turn %d, before %d", w,
                                                               size, head, ringColumn(v0, w), wrap));
                }
            }
        }
    }
    cols.done();
    turn.done();
}

void checkLayout()
{
    Check edge("every scale: the newest ping shown at the right edge");
    Check prefix("every scale: empty columns only left of the first ping");
    Check stretch("stretch: a ping takes its columns, in order");
    Check groups("compression: a column shows the newest ping of its group, in order");
    for (int si = 0; si < kScaleCount; ++si) {
        const float s = kScales[si];
        const int k = stretchOf(s);
        const int n = compressOf(s);
        for (int w : kWidths) {
            for (int size : kSizes) {
                for (long long head : headsFor(size)) {
                    const std::vector<int> v = view(head, w, s, size);
                    const long long shown = head - 1;
                    const int newest = shown >= 1 && shown <= size ? int(shown - 1) : -1;
                    edge.expect(v[size_t(w - 1)] == newest, fmt("scale %g width %d size %d head %lld: %d, want %d",
                                                                double(s), w, size, head, v[size_t(w - 1)], newest));

                    int firstData = 0;
                    while (firstData < w && v[size_t(firstData)] == -1) {
                        ++firstData;
                    }
                    bool ok = true;
                    for (int i = firstData; i < w; ++i) {
                        ok = ok && v[size_t(i)] >= 0 && v[size_t(i)] < size && v[size_t(i)] <= newest;
                    }
                    prefix.expect(ok, fmt("scale %g width %d size %d head %lld", double(s), w, size, head));

                    if (k > 1) {
                        bool runs = true;
                        int i = firstData;
                        while (i < w && runs) {
                            int j = i;
                            while (j < w && v[size_t(j)] == v[size_t(i)]) {
                                ++j;
                            }
                            // Only the run cut by the left edge may be short; every next run is the next ping.
                            runs = (j - i == k || (i == 0 && j - i < k))
                                && (j == w || v[size_t(j)] == v[size_t(i)] + 1);
                            i = j;
                        }
                        stretch.expect(runs, fmt("scale %g width %d size %d head %lld", double(s), w, size, head));
                    }
                    if (n > 1) {
                        bool newestOfGroup = true;
                        for (int i = firstData; i < w; ++i) {
                            const int e = v[size_t(i)];
                            const int want = std::min(groupFirst(e, s) + n - 1, newest);
                            newestOfGroup = newestOfGroup && e == want
                                && (i == firstData || e / n == v[size_t(i - 1)] / n + 1);
                        }
                        groups.expect(newestOfGroup, fmt("scale %g width %d size %d head %lld", double(s), w, size,
                                                         head));
                    }
                }
            }
        }
    }
    edge.done();
    prefix.done();
    stretch.done();
    groups.done();
}

void checkRing()
{
    Check place("ring: each view column in one slot, the one its virtual column turns to");
    Check redraw("ring: a new ping redraws only the columns it brings");
    for (int si = 0; si < kScaleCount; ++si) {
        const float s = kScales[si];
        const int k = stretchOf(s);
        const int n = compressOf(s);
        for (int w : kWidths) {
            for (long long head : { 2LL, 5LL, 3000LL, 50001LL, 123457LL }) {
                long long v0 = 0;
                const std::vector<int> cols = view(head, w, s, kBig, &v0);
                const std::vector<int> slots = ring(cols, v0, w);
                std::vector<int> seen(size_t(w), 0);
                bool ok = true;
                for (int i = 0; i < w; ++i) {
                    const int slot = ringColumn(v0 + i, w);
                    ok = ok && slot >= 0 && slot < w && slots[size_t(slot)] == cols[size_t(i)];
                    if (slot >= 0 && slot < w) {
                        ++seen[size_t(slot)];
                    }
                }
                ok = ok && std::all_of(seen.begin(), seen.end(), [](int x) { return x == 1; });
                place.expect(ok, fmt("scale %g width %d head %lld", double(s), w, head));

                if (v0 < 0) {
                    continue;   // the redraw count below is for a screen full of pings
                }
                for (long long d : { 1LL, 2LL, 3LL, 5LL, 16LL, 37LL, 700LL }) {
                    long long v0b = 0;
                    const std::vector<int> colsB = view(head + d, w, s, kBig, &v0b);
                    const std::vector<int> slotsB = ring(colsB, v0b, w);
                    long long changed = 0;
                    for (int x = 0; x < w; ++x) {
                        changed += slots[size_t(x)] != slotsB[size_t(x)] ? 1 : 0;
                    }
                    long long want = 0;
                    if (n == 1) {
                        want = std::min<long long>(d * k, w);
                    }
                    else {
                        const long long sh1 = head - 1;
                        const long long sh2 = head + d - 1;
                        const long long newGroups = (sh2 + n - 1) / n - (sh1 + n - 1) / n;
                        want = std::min<long long>(newGroups + (sh1 % n != 0 ? 1 : 0), w);
                    }
                    redraw.expect(changed == want, fmt("scale %g width %d head %lld +%lld: %lld slots, want %lld",
                                                       double(s), w, head, d, changed, want));
                }
            }
        }
    }
    place.done();
    redraw.done();
}

void checkEdits()
{
    Check tile("edits: a column reaches its group, and the groups tile the pings shown");
    Check off("edits: a ping off the screen has no column");
    Check oldest("the oldest ping shown, as the columns have it (the history the screen holds)");
    for (int si = 0; si < kScaleCount; ++si) {
        const float s = kScales[si];
        for (int w : kWidths) {
            for (int size : kSizes) {
                for (long long head : headsFor(size)) {
                    const std::vector<int> v = view(head, w, s, size);
                    const int newest = int(head - 2);
                    int expectNext = -1;   // the ping the next column's group must start at
                    int firstShown = -1;
                    bool ok = true;
                    std::string why;
                    for (int i = 0; i < w && ok; ++i) {
                        const int e = v[size_t(i)];
                        if (e < 0 || (i > 0 && v[size_t(i - 1)] == e)) {
                            continue;
                        }
                        const int first = groupFirst(e, s);
                        if (firstShown < 0) {
                            firstShown = first;
                        }
                        else if (first != expectNext) {
                            ok = false;
                            why = fmt("column %d starts at %d, want %d", i, first, expectNext);
                        }
                        for (int p = first; p <= e && ok; ++p) {
                            if (columnOf(v.data(), w, p, s) != i) {
                                ok = false;
                                why = fmt("ping %d in column %d, want %d", p, columnOf(v.data(), w, p, s), i);
                            }
                        }
                        expectNext = e + 1;
                    }
                    if (ok && firstShown >= 0 && expectNext != newest + 1) {
                        ok = false;
                        why = fmt("the groups end at %d, want %d", expectNext - 1, newest);
                    }
                    tile.expect(ok, fmt("scale %g width %d size %d head %lld: %s", double(s), w, size, head,
                                        why.c_str()));
                    oldest.expect(oldestShown(int(head - 1), w, s) == firstShown,
                                  fmt("scale %g width %d size %d head %lld: %d, the columns %d", double(s), w, size,
                                      head, oldestShown(int(head - 1), w, s), firstShown));

                    std::vector<int> outside = { -1, newest + 1, newest + 2, newest + 1000 };
                    if (firstShown >= 1) {
                        outside.push_back(firstShown - 1);
                        outside.push_back(0);
                    }
                    for (int p : outside) {
                        if (p >= 0 && p >= firstShown && p <= newest && firstShown >= 0) {
                            continue;   // on screen after all (an empty view, or newest + 1 below 0)
                        }
                        off.expect(columnOf(v.data(), w, p, s) == -1,
                                   fmt("scale %g width %d size %d head %lld: ping %d in column %d", double(s), w,
                                       size, head, p, columnOf(v.data(), w, p, s)));
                    }
                }
            }
        }
    }
    tile.done();
    off.done();
    oldest.done();
}

void checkAnchor()
{
    Check keep("a step keeps the ping under the pointer in its column");
    for (int si = 0; si < kScaleCount; ++si) {
        const float s = kScales[si];
        const int n = compressOf(s);
        for (int w : { 1, 7, 64, 640, 1921 }) {
            for (int column : { 0, 1, w / 3, w / 2, w - 2, w - 1 }) {
                if (column < 0 || column >= w) {
                    continue;
                }
                for (int ping : { 0, 1, 2, 15, 16, 1000, 77777 }) {
                    const long long head = headKeeping(ping, column, w, s);
                    const std::vector<int> v = view(head, w, s, kBig);
                    const int e = v[size_t(column)];
                    const bool ok = n > 1 ? (e >= ping && e / n == ping / n) : e == ping;
                    keep.expect(ok, fmt("scale %g width %d column %d ping %d: head %lld shows %d there", double(s),
                                        w, column, ping, head, e));
                }
            }
        }
    }
    keep.done();
}

} // namespace

int main()
{
    checkSteps();
    checkScaleOne();
    checkLayout();
    checkRing();
    checkEdits();
    checkAnchor();
    std::printf("echoscale_check: %d of %d checks pass\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
