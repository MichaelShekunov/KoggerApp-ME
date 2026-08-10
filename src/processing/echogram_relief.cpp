#include "echogram_relief.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace echo_relief {

float groundRange(float slantRangeM, float altitudeM)
{
    if (!(slantRangeM > 0.0f) || !(altitudeM > 0.0f)) {
        return 0.0f;
    }
    const float sq = slantRangeM * slantRangeM - altitudeM * altitudeM;
    return sq > 0.0f ? std::sqrt(sq) : 0.0f;
}

float heightFromShadow(float groundRangeM, float shadowLengthM, float altitudeM)
{
    if (!(shadowLengthM > 0.0f) || !(altitudeM > 0.0f) || !(groundRangeM > 0.0f)) {
        return 0.0f;
    }
    return shadowLengthM * altitudeM / (groundRangeM + shadowLengthM);
}

int findShadows(const int16_t* samples, const float* slantRangeAt, int count,
                float altitudeM, const Params& params, Shadow* out, int maxOut)
{
    if (!samples || !slantRangeAt || count < 8 || !out || maxOut <= 0) {
        return 0;
    }
    if (!(altitudeM > 0.0f)) {
        return 0;
    }

    // Reference level from the median of the usable part of the profile, not the
    // mean: shadows and the odd bright target both drag a mean around, and the
    // threshold would then follow whatever it is meant to be distinguishing.
    std::vector<int16_t> usable;
    usable.reserve(size_t(count));
    for (int i = 0; i < count; ++i) {
        if (groundRange(slantRangeAt[i], altitudeM) >= params.minGroundRangeM) {
            usable.push_back(samples[i]);
        }
    }
    if (usable.size() < 8) {
        return 0;
    }
    std::nth_element(usable.begin(), usable.begin() + usable.size() / 2, usable.end());
    const float reference = float(usable[usable.size() / 2]);
    if (!(reference > 0.0f)) {
        return 0; // a blank profile has no shadows, only absence
    }

    const float threshold = reference * params.shadowFraction;
    const float maxHeight = altitudeM * params.maxHeightFraction;

    int found = 0;
    int runStart = -1;

    auto closeRun = [&](int runEnd) {
        if (runStart < 0 || found >= maxOut) { runStart = -1; return; }

        // The object stands at the shadow's near edge - that is the last place
        // the beam still reached the bottom before the top of the object cut it.
        const float x0 = groundRange(slantRangeAt[runStart], altitudeM);
        const float x1 = groundRange(slantRangeAt[runEnd], altitudeM);
        runStart = -1;

        const float length = x1 - x0;
        if (!(length >= params.minShadowMetres)) { return; }

        const float h = heightFromShadow(x0, length, altitudeM);
        if (!(h > 0.0f) || h > maxHeight) { return; }

        Shadow& s = out[found++];
        s.firstSample  = runEnd;   // filled properly below
        s.lastSample   = runEnd;
        s.groundRangeM = x0;
        s.lengthM      = length;
        s.heightM      = h;
    };

    // Walk near to far. A run of samples under the threshold, wholly outside the
    // nadir exclusion, is a shadow candidate.
    for (int i = 0; i < count; ++i) {
        const float x = groundRange(slantRangeAt[i], altitudeM);
        const bool usableHere = x >= params.minGroundRangeM;
        const bool dark = usableHere && float(samples[i]) < threshold;

        if (dark) {
            if (runStart < 0) { runStart = i; }
            continue;
        }
        if (runStart >= 0) {
            const int begin = runStart;
            closeRun(i);
            if (found > 0 && out[found - 1].lastSample == i) {
                out[found - 1].firstSample = begin;
                out[found - 1].lastSample  = i - 1;
            }
        }
    }
    if (runStart >= 0) {
        // A shadow running off the end of the profile has no far edge, so its
        // length is a lower bound and its height would be understated. Dropped
        // rather than reported low.
        runStart = -1;
    }

    return found;
}

} // namespace echo_relief
