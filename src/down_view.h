#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

/**
 * @brief A view straight down made from the two sides of a two-sided sonar (sides 0 and 1 of one ping, as DSSS and
 * nanoSSS send them in ID_CHART v1): the two echoes at the same range combined, as one wide beam pointing down would
 * have them. Nothing of it is received or stored: Epoch::chartTo makes it when an echogram draws one of its
 * sub-channels; for everything else (range, bottom) such a sub-channel reads side 0.
 *
 * Echogram bytes are logarithmic, 15 a doubling of the amplitude (l412_adc dk_logbyte, about 2.5 a dB), so
 * - Energy: the root mean square of the amplitudes, 15 * log2(sqrt((A0^2 + A1^2) / 2)) with A = 2^(b / 15): what one
 *   wide beam would get; whatever one side sees stays, at most 7.5 bytes (3 dB) down;
 * - Mean: the mean of the bytes, which is the geometric mean of the amplitudes;
 * - Min: the smaller byte, only what both sides see: the bottom under the boat, a target under it.
 * The owner's choice, 2026-10-28, after pictures of the three from the recording 00014: all three, chosen in the
 * echogram's channel list.
 */
namespace DownView {

enum class Mode : uint8_t { Energy, Mean, Min };

// The sub-channels of the three, after a two-sided channel's sides 0 and 1 in the channel lists.
constexpr uint8_t kSubEnergy = 0xF1;
constexpr uint8_t kSubMean = 0xF2;
constexpr uint8_t kSubMin = 0xF3;

inline bool isSub(uint8_t sub) { return sub >= kSubEnergy && sub <= kSubMin; }

inline Mode modeOf(uint8_t sub)
{
    return sub == kSubMean ? Mode::Mean : sub == kSubMin ? Mode::Min : Mode::Energy;
}

/** The sub-channel's part of a channel name, "<port>|<address>|<this>"; nullptr for a side. */
inline const char* nameOf(uint8_t sub)
{
    switch (sub) {
    case kSubEnergy: return "down-energy";
    case kSubMean:   return "down-mean";
    case kSubMin:    return "down-min";
    default:         return nullptr;
    }
}

/** The sub-channel of a name's last part ("down-energy" ...); 0 when it is none of the three. */
inline uint8_t subOf(const char* name)
{
    for (uint8_t sub : { kSubEnergy, kSubMean, kSubMin }) {
        if (name && std::strcmp(name, nameOf(sub)) == 0) {
            return sub;
        }
    }
    return 0;
}

/** The two bytes' energy mean, a table of the 65536 pairs made once. */
inline uint8_t energy(uint8_t a, uint8_t b)
{
    static const std::array<uint8_t, 256 * 256> table = [] {
        std::array<uint8_t, 256 * 256> t{};
        for (int i = 0; i < 256; ++i) {
            for (int j = 0; j < 256; ++j) {
                const double p = (std::exp2(2.0 * i / 15.0) + std::exp2(2.0 * j / 15.0)) / 2.0;
                const long v = std::lround(7.5 * std::log2(p));
                t[size_t(i) * 256 + size_t(j)] = uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
        return t;
    }();
    return table[size_t(a) * 256 + b];
}

inline uint8_t combine(Mode mode, uint8_t a, uint8_t b)
{
    switch (mode) {
    case Mode::Mean: return uint8_t((unsigned(a) + unsigned(b) + 1u) / 2u);
    case Mode::Min:  return a < b ? a : b;
    default:         return energy(a, b);
    }
}

/** n samples of the two sides into out. */
inline void combineRow(Mode mode, const uint8_t* side0, const uint8_t* side1, int n, uint8_t* out)
{
    for (int i = 0; i < n; ++i) {
        out[i] = combine(mode, side0[i], side1[i]);
    }
}

} // namespace DownView
