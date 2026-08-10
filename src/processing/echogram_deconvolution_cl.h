#pragma once

#include <cstdint>
#include <string>

namespace echo_deconv {

struct Params;
struct Stats;

// GPU backend for the along-track pass.
//
// OpenCL is opened at runtime, not linked: no SDK to build against, nothing
// extra to deploy, and a machine without a runtime simply keeps the CPU path.
// Every entry point is safe to call when there is no device - it reports so and
// the caller falls back.
class ClBackend {
public:
    static ClBackend& instance();

    // True once a device, context and compiled program exist. First call does
    // the work; later calls are cheap.
    bool available();

    // Empty when unavailable. Otherwise the device and why it was chosen.
    std::string description();

    // Why the GPU is not in use, when it is not. Empty if it is.
    std::string unavailableReason();

    // Same contract as the CPU alongTrack. Returns false without touching
    // `columns` if the work could not be done, so the caller can fall back.
    bool alongTrack(int16_t* const* columns, int width, int height,
                    const float* rangeAt, const float* alongTrackM,
                    const float* pitchRad, float fallbackSpacingM,
                    const Params& params, Stats& stats);

    ClBackend(const ClBackend&) = delete;
    ClBackend& operator=(const ClBackend&) = delete;

private:
    ClBackend() = default;
    ~ClBackend();

    struct Impl;
    Impl* d_ = nullptr;
    bool  tried_ = false;
};

} // namespace echo_deconv
