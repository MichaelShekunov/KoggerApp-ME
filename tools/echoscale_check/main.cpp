// Offline check of src/scene2d/plot2D_columns.h: the steps of the echogram's horizontal scale (the settings row,
// the edit script's hzoom, a restored layout) over develop's timeline zoom (0.1 .. 10, settled to tenths).
// Prints a line per check and FAIL lines; exit code 0 when everything holds.

#include "plot2D_columns.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <string>

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

// develop's Plot2D::snapTimelineZoom and its limits (kTimelineZoomStep, kTimelineZoomMin/Max in plot2D.cpp).
float developSnap(float zoom)
{
    const float snapped = std::round(zoom / 0.1f) * 0.1f;
    return std::min(std::max(snapped, 0.1f), 10.0f);
}

} // namespace

int main()
{
    {
        Check c("the steps run up from 0.1 to 10, 1 at kScaleOne");
        c.expect(kScales[0] == 0.1f && kScales[kScaleCount - 1] == 10.0f && kScales[kScaleOne] == 1.0f,
                 fmt("%g .. %g, [%d] = %g", double(kScales[0]), double(kScales[kScaleCount - 1]), kScaleOne,
                     double(kScales[kScaleOne])));
        for (int i = 1; i < kScaleCount; ++i) {
            c.expect(kScales[i] > kScales[i - 1], fmt("step %d (%g) after %g", i, double(kScales[i]), double(kScales[i - 1])));
        }
        c.done();
    }
    {
        Check c("develop's snap keeps every step where it is");
        for (int i = 0; i < kScaleCount; ++i) {
            c.expect(std::fabs(developSnap(kScales[i]) - kScales[i]) < 1e-5f,
                     fmt("%g snaps to %g", double(kScales[i]), double(developSnap(kScales[i]))));
        }
        c.done();
    }
    {
        Check c("each step is its own index and snaps to itself");
        for (int i = 0; i < kScaleCount; ++i) {
            c.expect(scaleIndex(kScales[i]) == i && snapScale(kScales[i]) == kScales[i],
                     fmt("step %d (%g) -> index %d", i, double(kScales[i]), scaleIndex(kScales[i])));
        }
        c.done();
    }
    {
        Check c("in-between scales go to the nearest step in proportion");
        // develop's smooth zoom leaves any tenth: the settings row shows the nearest step.
        const struct { float in; float want; } cases[] = {
            { 0.13f, 0.1f }, { 0.15f, 0.2f }, { 0.3f, 0.2f }, { 0.35f, 0.5f }, { 0.7f, 0.5f }, { 0.75f, 1.0f },
            { 1.4f, 1.0f }, { 1.5f, 2.0f }, { 2.5f, 3.0f }, { 3.5f, 4.0f }, { 4.9f, 6.0f }, { 7.0f, 8.0f },
            { 9.0f, 10.0f }, { 0.01f, 0.1f }, { 50.0f, 10.0f } };
        for (const auto& t : cases) {
            c.expect(snapScale(t.in) == t.want, fmt("%g -> %g, want %g", double(t.in), double(snapScale(t.in)), double(t.want)));
        }
        c.done();
    }
    {
        Check c("not a scale (0, negative, NaN, infinity) is 1");
        const float bad[] = { 0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() };
        for (float b : bad) {
            c.expect(scaleIndex(b) == kScaleOne && snapScale(b) == 1.0f, fmt("%g -> index %d", double(b), scaleIndex(b)));
        }
        c.done();
    }
    {
        Check c("a step moves one along, held at the ends");
        for (int i = 0; i < kScaleCount; ++i) {
            for (int steps = -12; steps <= 12; ++steps) {
                const int want = std::min(std::max(i + steps, 0), kScaleCount - 1);
                c.expect(stepScale(kScales[i], steps) == kScales[want],
                         fmt("%g by %d -> %g, want %g", double(kScales[i]), steps, double(stepScale(kScales[i], steps)),
                             double(kScales[want])));
            }
        }
        c.done();
    }

    std::printf("\nechoscale_check: %d checks, %d failed: %s\n", g_checks, g_failed, g_failed ? "FAILED" : "PASS");
    return g_failed ? 1 : 0;
}
