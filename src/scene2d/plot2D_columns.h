#pragma once

// The steps of the echogram's horizontal scale: the echogram settings' row, the edit script's hzoom, a restored
// layout. The scale itself is develop's timeline zoom (Plot2D cursor_.horZoom: screen columns a ping, 0.1 .. 10,
// the wheel and the pinch smooth, settled to tenths by snapTimelineZoom); since the develop merge (2026-10-28) the
// columns are develop's too (Plot2D::reindexingCursor), so only the steps live here. Kept free of Qt so
// tools/echoscale_check can test them on their own.

#include <algorithm>
#include <cmath>

namespace plot2d_columns {

// From the most compressed to the most stretched; tenths, so develop's snap keeps each where it is.
inline constexpr float kScales[] = { 0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 10.0f };
inline constexpr int kScaleCount = int(sizeof(kScales) / sizeof(kScales[0]));
inline constexpr int kScaleOne = 3;   // kScales[kScaleOne] == 1: a column a ping

/// The index in kScales nearest to `scale` (in proportion, not difference); kScaleOne for anything not positive.
inline int scaleIndex(float scale)
{
    if (!(scale > 0.0f) || !std::isfinite(scale)) {
        return kScaleOne;
    }
    int best = kScaleOne;
    float bestDist = 1e9f;
    for (int i = 0; i < kScaleCount; ++i) {
        const float d = std::fabs(std::log(scale / kScales[i]));
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

inline float snapScale(float scale) { return kScales[scaleIndex(scale)]; }

/// The scale `steps` steps from `scale` (positive: more columns a ping), held at the ends.
inline float stepScale(float scale, int steps)
{
    return kScales[std::clamp(scaleIndex(scale) + steps, 0, kScaleCount - 1)];
}

} // namespace plot2d_columns
