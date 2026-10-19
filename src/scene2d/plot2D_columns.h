#pragma once

// Pings to screen columns at the echogram's horizontal scale (Plot2D::reindexingCursor). Kept free of Qt and
// of the plot so tools/echoscale_check can test it on its own.
//
// The scale is screen columns a ping: 2..8 stretch the history (a ping takes that many columns), 1/2..1/16
// compress it (a column shows the newest ping of a group of n, groups counted from ping 0). Either way a
// ping, or a group, keeps one absolute column ("virtual column": pings times the stretch, or the group's
// number) however the view scrolls; the screen shows virtual columns V0 .. V0 + width - 1. The newest ping
// shown sits at the right edge, the partial group there showing its newest ping, so a live view moves on
// every ping.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace plot2d_columns {

// The steps the scale snaps to, from the most compressed to the most stretched.
inline constexpr float kScales[] = { 1.0f / 16, 1.0f / 8, 1.0f / 4, 1.0f / 2, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f };
inline constexpr int kScaleCount = int(sizeof(kScales) / sizeof(kScales[0]));

/// The index in kScales nearest to `scale` (in proportion, not difference); 1 for anything not positive.
inline int scaleIndex(float scale)
{
    if (!(scale > 0.0f) || !std::isfinite(scale)) {
        return 4;
    }
    int best = 4;
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

/// Columns a ping when stretched (scale >= 1), else 1.
inline int stretchOf(float scale) { return scale >= 1.0f ? std::max(1, int(std::lround(scale))) : 1; }
/// Pings a column when compressed (scale < 1), else 1.
inline int compressOf(float scale) { return scale < 1.0f ? std::max(1, int(std::lround(1.0f / scale))) : 1; }

/// Fills out[0 .. width-1] with the ping each screen column shows, -1 for none, when the pings shown are
/// 0 .. shown-1 (the newest at the right edge) and the dataset holds dataWidth of them. Returns the virtual
/// column of screen column 0.
inline long long fillColumns(int shown, int width, float scale, int dataWidth, int* out)
{
    const int k = stretchOf(scale);
    const int n = compressOf(scale);
    long long v0 = 0;
    if (n == 1) {
        v0 = (long long)shown * k - width;
    }
    else {
        const long long groups = shown > 0 ? (shown + n - 1) / n : 0;
        v0 = groups - width;
    }
    for (int i = 0; i < width; ++i) {
        const long long v = v0 + i;
        long long e = -1;
        if (v >= 0 && shown > 0) {
            e = (n == 1) ? v / k : std::min<long long>(v * n + n - 1, shown - 1);
        }
        out[i] = (e >= 0 && e < dataWidth && e < shown) ? int(e) : -1;
    }
    return v0;
}

/// The head (Plot2D: the timeline position times the dataset size; fillColumns gets head - 1 as `shown`) at
/// which `width` columns at `scale` show `anchorPing` in column `anchorColumn` (compressed: its group there).
/// A change of the scale goes through it to keep the ping under the pointer where it was.
inline long long headKeeping(int anchorPing, int anchorColumn, int width, float scale)
{
    const int k = stretchOf(scale);
    const int n = compressOf(scale);
    const long long right = (long long)width - 1 - anchorColumn;   // columns right of the anchor
    // The newest ping shown: compressed, `right` groups after the anchor's; stretched, as many pings after the
    // anchor as leave the anchor column inside its run (ceil((right + 1) / k) - 1).
    const long long shown = (n > 1) ? anchorPing + right * n + 1
                                    : anchorPing + (right + k) / k;
    return shown + 1;
}

/// The first ping of the group a column stands for when it shows `shownPing`: compressed, the group's first;
/// otherwise the ping itself.
inline int groupFirst(int shownPing, float scale)
{
    const int n = compressOf(scale);
    return (n > 1 && shownPing >= 0) ? (shownPing / n) * n : shownPing;
}

/// The first of the `width` columns fillColumns filled in `cols` that shows `ping` or, compressed, its group;
/// -1 when it is not on screen.
inline int columnOf(const int* cols, int width, int ping, float scale)
{
    if (ping < 0) {
        return -1;
    }
    const int n = compressOf(scale);
    // Left to right the pings only grow (a stretched one repeats, -1 only before the data starts).
    for (int column = 0; column < width; ++column) {
        const int e = cols[column];
        if (e < ping) {
            continue;
        }
        return (e == ping || (n > 1 && e / n == ping / n)) ? column : -1;
    }
    return -1;
}

/// The oldest ping the `width` columns stand for when fillColumns shows pings 0 .. shown-1 (compressed: the first
/// of the leftmost column's group); -1 when none is shown.
inline int oldestShown(int shown, int width, float scale)
{
    if (shown <= 0 || width <= 0) {
        return -1;
    }
    const int k = stretchOf(scale);
    const int n = compressOf(scale);
    if (n == 1) {
        const long long v0 = (long long)shown * k - width;
        return int(std::max<long long>(v0, 0) / k);
    }
    const long long groups = ((long long)shown + n - 1) / n;
    return int(std::max<long long>(groups - width, 0) * n);
}

/// The ring position of a virtual column in a ring of `width` columns.
inline int ringColumn(long long virtualColumn, int width)
{
    if (width <= 0) {
        return 0;
    }
    const long long r = virtualColumn % width;
    return int(r < 0 ? r + width : r);
}

} // namespace plot2d_columns
