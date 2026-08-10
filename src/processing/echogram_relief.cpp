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

int acrossTrackRelief(const int16_t* samples, const float* slantRangeAt, int count,
                      float altitudeM, const ReliefParams& params,
                      float* groundRangeOut, float* elevationOut, int maxOut)
{
    if (!samples || !slantRangeAt || count < 16 || !groundRangeOut || !elevationOut) {
        return 0;
    }
    if (!(altitudeM > 0.0f) || maxOut <= 0) {
        return 0;
    }

    // Gather the usable stretch: outside the nadir exclusion, with a real return.
    std::vector<float> x, v;
    x.reserve(size_t(count));
    v.reserve(size_t(count));
    for (int i = 0; i < count; ++i) {
        const float g = groundRange(slantRangeAt[i], altitudeM);
        if (g < params.minGroundRangeM) { continue; }
        x.push_back(g);
        v.push_back(float(samples[i]));
    }
    const int n = int(x.size());
    if (n < 16) { return 0; }

    // Divide out the physics before fitting anything. Brightness goes as
    // K(x) * (sin(phi) + s*cos(phi)), and sin(phi) = H/sqrt(x^2+H^2) is known
    // exactly from the altitude - no fitting required. What is left,
    //
    //     u = I / sin(phi) = K(x) * (1 + s*x/H)
    //
    // separates cleanly: K carries TVG, beam pattern and sediment and varies
    // slowly, while the relief term rides on it.
    std::vector<double> u(size_t(n), 0.0);
    for (int i = 0; i < n; ++i) {
        const double sinPhi = double(altitudeM) / std::sqrt(double(x[size_t(i)]) * double(x[size_t(i)])
                                                            + double(altitudeM) * double(altitudeM));
        u[size_t(i)] = (sinPhi > 1.0e-6) ? double(v[size_t(i)]) / sinPhi : 0.0;
    }

    // Fit K as a low-order curve in log range - low order on purpose. A running
    // mean wide enough to be a trend is also wide enough to swallow the broad
    // relief that filling between survey lines is entirely about; a quadratic
    // cannot follow a hill without being told to.
    std::vector<double> logK(size_t(n), 0.0);
    {
        double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, t0 = 0, t1 = 0, t2 = 0;
        for (int i = 0; i < n; ++i) {
            if (!(u[size_t(i)] > 0.0)) { continue; }
            const double t = std::log(std::max(double(x[size_t(i)]), 1.0e-3));
            const double y = std::log(u[size_t(i)]);
            const double t_2 = t * t;
            s0 += 1; s1 += t; s2 += t_2; s3 += t_2 * t; s4 += t_2 * t_2;
            t0 += y; t1 += t * y; t2 += t_2 * y;
        }
        if (s0 < 8.0) { return 0; }

        double m[3][4] = {{s0, s1, s2, t0}, {s1, s2, s3, t1}, {s2, s3, s4, t2}};
        for (int c = 0; c < 3; ++c) {
            int piv = c;
            for (int r2 = c + 1; r2 < 3; ++r2) {
                if (std::fabs(m[r2][c]) > std::fabs(m[piv][c])) { piv = r2; }
            }
            if (std::fabs(m[piv][c]) < 1.0e-12) { return 0; }
            if (piv != c) { for (int k = 0; k < 4; ++k) { std::swap(m[c][k], m[piv][k]); } }
            for (int r2 = 0; r2 < 3; ++r2) {
                if (r2 == c) { continue; }
                const double f = m[r2][c] / m[c][c];
                for (int k = c; k < 4; ++k) { m[r2][k] -= f * m[c][k]; }
            }
        }
        const double a0 = m[0][3] / m[0][0];
        const double a1 = m[1][3] / m[1][1];
        const double a2 = m[2][3] / m[2][2];
        for (int i = 0; i < n; ++i) {
            const double t = std::log(std::max(double(x[size_t(i)]), 1.0e-3));
            logK[size_t(i)] = a0 + a1 * t + a2 * t * t;
        }
    }

    std::vector<float> trend(size_t(n), 0.0f);
    for (int i = 0; i < n; ++i) { trend[size_t(i)] = float(std::exp(logK[size_t(i)])); }

    // Integrate the slope outward from the nadir, where the depth is known.
    int written = 0;
    float z = 0.0f;
    for (int i = 0; i < n && written < maxOut; ++i) {
        const float t = trend[size_t(i)];
        float s = 0.0f;
        if (t > 1.0e-3f && x[size_t(i)] > 0.0f) {
            const float r = float(u[size_t(i)]) / t;   // 1 + s*x/H
            s = (r - 1.0f) * altitudeM / x[size_t(i)];
            s = std::clamp(s, -params.maxSlope, params.maxSlope);
        }

        if (i > 0) {
            const float dx = x[size_t(i)] - x[size_t(i - 1)];
            if (dx > 0.0f) { z += s * dx; }
        }

        groundRangeOut[written] = x[size_t(i)];
        elevationOut[written]   = z;
        ++written;
    }

    return written;
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
