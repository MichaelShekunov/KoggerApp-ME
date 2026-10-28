// Offline check of the down view's arithmetic (src/down_view.h): the three ways to combine two sides' bytes, the
// sub-channels and their names. The bytes are logarithmic, 15 a doubling of the amplitude, so the energy mean is
// worked out here again from the amplitudes, independently of the table.
#include "down_view.h"

#include <cmath>
#include <cstdio>
#include <string>

static int failed = 0;
static int passed = 0;

static void check(const char* what, bool ok, const std::string& detail = std::string())
{
    if (ok) {
        ++passed;
        std::printf("PASS %s\n", what);
    } else {
        ++failed;
        std::printf("FAIL %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
    }
}

// What one wide beam would get from the two sides: the root mean square of the amplitudes, back in bytes.
static int energyReference(int a, int b)
{
    const double amplitudeA = std::pow(2.0, a / 15.0);
    const double amplitudeB = std::pow(2.0, b / 15.0);
    const double rms = std::sqrt((amplitudeA * amplitudeA + amplitudeB * amplitudeB) / 2.0);
    const long v = std::lround(15.0 * std::log2(rms));
    return v < 0 ? 0 : v > 255 ? 255 : int(v);
}

int main()
{
    using namespace DownView;

    int wrong = 0;
    std::string first;
    for (int a = 0; a < 256; ++a) {
        for (int b = 0; b < 256; ++b) {
            const int got = energy(uint8_t(a), uint8_t(b));
            if (got != energyReference(a, b)) {
                if (!wrong) {
                    first = "energy(" + std::to_string(a) + ", " + std::to_string(b) + ") = " + std::to_string(got) +
                            ", the amplitudes say " + std::to_string(energyReference(a, b));
                }
                ++wrong;
            }
        }
    }
    check("energy: every pair as the root mean square of the amplitudes", wrong == 0,
          std::to_string(wrong) + " pairs differ, first " + first);

    bool same = true, symmetric = true, bounded = true, monotonic = true;
    for (int a = 0; a < 256; ++a) {
        same = same && energy(uint8_t(a), uint8_t(a)) == a;
        for (int b = 0; b < 256; ++b) {
            const int e = energy(uint8_t(a), uint8_t(b));
            const int hi = a > b ? a : b;
            symmetric = symmetric && e == energy(uint8_t(b), uint8_t(a));
            bounded = bounded && e <= hi && e >= hi - 8;
            if (b > 0) {
                monotonic = monotonic && e >= energy(uint8_t(a), uint8_t(b - 1));
            }
        }
    }
    check("energy: two equal sides give that byte", same);
    check("energy: the same either way round", symmetric);
    check("energy: the louder side, at most 8 bytes (3 dB) down", bounded);
    check("energy: never less for a louder side", monotonic);
    check("energy: a side alone is 7.5 bytes down (200 with 0)", energy(200, 0) == 193 || energy(200, 0) == 192,
          std::to_string(energy(200, 0)));

    check("mean: the bytes' mean, a half up", combine(Mode::Mean, 10, 20) == 15 && combine(Mode::Mean, 10, 11) == 11
                                                   && combine(Mode::Mean, 255, 255) == 255 && combine(Mode::Mean, 0, 1) == 1);
    check("min: the smaller byte", combine(Mode::Min, 10, 20) == 10 && combine(Mode::Min, 200, 7) == 7
                                       && combine(Mode::Min, 0, 255) == 0);
    check("energy mode: the table", combine(Mode::Energy, 120, 40) == energy(120, 40));

    const uint8_t side0[5] = { 0, 60, 120, 200, 255 };
    const uint8_t side1[5] = { 255, 60, 30, 210, 0 };
    uint8_t out[5] = {};
    bool rowOk = true;
    for (Mode m : { Mode::Energy, Mode::Mean, Mode::Min }) {
        combineRow(m, side0, side1, 5, out);
        for (int i = 0; i < 5; ++i) {
            rowOk = rowOk && out[i] == combine(m, side0[i], side1[i]);
        }
    }
    check("a row: each sample of the two sides at the same index", rowOk);

    check("sub-channels: the three, nothing else", isSub(kSubEnergy) && isSub(kSubMean) && isSub(kSubMin) && !isSub(0)
                                                       && !isSub(1) && !isSub(2) && !isSub(0xF0) && !isSub(0xF4) && !isSub(0xFF));
    check("sub-channels: their modes", modeOf(kSubEnergy) == Mode::Energy && modeOf(kSubMean) == Mode::Mean
                                           && modeOf(kSubMin) == Mode::Min);
    check("names: each sub-channel's own, back to it", subOf(nameOf(kSubEnergy)) == kSubEnergy
                                                            && subOf(nameOf(kSubMean)) == kSubMean && subOf(nameOf(kSubMin)) == kSubMin
                                                            && std::string(nameOf(kSubEnergy)) == "down-energy"
                                                            && std::string(nameOf(kSubMean)) == "down-mean"
                                                            && std::string(nameOf(kSubMin)) == "down-min");
    check("names: a side or another name is none", nameOf(0) == nullptr && nameOf(1) == nullptr && subOf("1") == 0
                                                       && subOf("down") == 0 && subOf(nullptr) == 0);

    std::printf("downview_check: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
