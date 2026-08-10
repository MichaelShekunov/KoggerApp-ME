// Does a shadow give back the height that cast it?
//
// Builds a side-scan profile over a flat bottom with an object of known height
// standing on it, casts the shadow the geometry demands, and asks the detector
// to recover the height it started from.
#include "echogram_relief.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr float kAltitude   = 12.0f;  // sonar height above the bottom
constexpr float kBackground = 140.0f;
constexpr int   kSamples    = 1200;
constexpr float kMaxSlant   = 90.0f;

// Ground range where the shadow of an object of height h at ground range x ends.
// Ray from (0, kAltitude) through (x, h), continued to z = 0.
float shadowEndsAt(float x, float h)
{
    return x * kAltitude / (kAltitude - h);
}

// One profile: flat bottom at `kBackground`, one object, one shadow behind it.
std::vector<int16_t> makeProfile(float objectAt, float objectHeight,
                                 std::vector<float>& slantOut)
{
    std::vector<int16_t> p(size_t(kSamples), 0);
    slantOut.assign(size_t(kSamples), 0.0f);

    const float shadowFrom = objectAt;
    const float shadowTo   = shadowEndsAt(objectAt, objectHeight);

    for (int i = 0; i < kSamples; ++i) {
        const float slant = kAltitude + (kMaxSlant - kAltitude) * float(i) / float(kSamples - 1);
        slantOut[size_t(i)] = slant;
        const float x = echo_relief::groundRange(slant, kAltitude);

        float v = (x > 0.5f) ? kBackground : 20.0f;      // water column before the bottom
        if (x >= shadowFrom && x < shadowTo) { v = 3.0f; } // shadow
        // The object itself returns strongly, just before its shadow.
        if (x > shadowFrom - 0.6f && x < shadowFrom) { v = 240.0f; }
        p[size_t(i)] = int16_t(v);
    }
    return p;
}

} // namespace


int main()
{
    std::printf("shadow geometry\n");
    bool ok = true;

    // The formula against a case worked by hand: altitude 12, object 2 m tall at
    // ground range 30. Shadow ends at 30*12/(12-2) = 36, so it is 6 m long, and
    // 6*12/(30+6) = 2 back again.
    {
        const float h = echo_relief::heightFromShadow(30.0f, 6.0f, 12.0f);
        const bool good = std::fabs(h - 2.0f) < 1.0e-3f;
        if (!good) { ok = false; }
        std::printf("  x=30 L=6 H=12 -> h=%.3f (expect 2.000)  %s\n", h, good ? "ok" : "BAD");
    }
    // Slant to ground, the step that is easy to skip: at altitude 12 a slant
    // range of 13 is only 5 m out along the bottom, not 13.
    {
        const float x = echo_relief::groundRange(13.0f, 12.0f);
        const bool good = std::fabs(x - 5.0f) < 1.0e-3f;
        if (!good) { ok = false; }
        std::printf("  slant=13 H=12 -> ground=%.3f (expect 5.000)  %s\n", x, good ? "ok" : "BAD");
    }
    std::printf("\n");

    std::printf("recovery from a synthetic profile, altitude %.0f m\n", kAltitude);
    std::printf("%-10s %-10s %-10s %-10s %s\n", "at (m)", "true h", "found h", "err", "");

    echo_relief::Params params;
    int cases = 0, good = 0;

    for (float objectAt : {15.0f, 25.0f, 40.0f, 60.0f}) {
        for (float trueH : {0.5f, 1.0f, 2.0f, 4.0f}) {
            std::vector<float> slant;
            const std::vector<int16_t> profile = makeProfile(objectAt, trueH, slant);

            echo_relief::Shadow found[8];
            const int n = echo_relief::findShadows(profile.data(), slant.data(),
                                                   int(profile.size()), kAltitude,
                                                   params, found, 8);
            // A shadow that reaches the end of the profile has no far edge, so
            // its length is only a lower bound. Reporting it would understate
            // the height, quietly; dropping it is the correct answer, and this
            // asks for exactly that rather than tolerating either outcome.
            const float lastGround = echo_relief::groundRange(kMaxSlant, kAltitude);
            const bool runsOffTheEnd = shadowEndsAt(objectAt, trueH) >= lastGround;

            ++cases;
            if (runsOffTheEnd) {
                const bool dropped = (n < 1);
                if (dropped) { ++good; }
                std::printf("%-10.0f %-10.1f %-10s %-10s %s\n", objectAt, trueH, "-", "-",
                            dropped ? "ok (shadow leaves the profile, correctly dropped)"
                                    : "BAD (truncated shadow reported anyway)");
                continue;
            }
            if (n < 1) {
                std::printf("%-10.0f %-10.1f %-10s %-10s %s\n", objectAt, trueH, "-", "-", "MISSED");
                continue;
            }
            // The largest shadow is the object's; the near-nadir water column can
            // produce a short one.
            int best = 0;
            for (int i = 1; i < n; ++i) {
                if (found[i].lengthM > found[best].lengthM) { best = i; }
            }
            const float err = found[best].heightM - trueH;
            const bool within = std::fabs(err) <= 0.15f * trueH + 0.05f;
            if (within) { ++good; }
            std::printf("%-10.0f %-10.1f %-10.2f %-+10.2f %s\n",
                        objectAt, trueH, found[best].heightM, err, within ? "ok" : "BAD");
        }
    }

    std::printf("\n%d of %d recovered within 15 %%\n", good, cases);
    const bool pass = ok && good == cases;
    std::printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
