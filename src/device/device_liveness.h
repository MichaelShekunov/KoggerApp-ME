#pragma once

#include <cstdint>

/**
 * @brief When a device on a live connection counts as without data, and when a quiet one is asked whether it is
 * still there (DevDriver::dataStale).
 *
 * Field test 2026-09-29: with the radio link gone the boat's devices still looked present. The link's own "receives
 * data" cannot tell, another device on it (the head unit's Wi-Fi module) keeps it busy, so each device watches its
 * own frames. A quiet device that is there (a stopped sonar) is asked for its mark (ID_MARK GET), so only silence to
 * that counts. Times in ms of one clock.
 */
struct DeviceLiveness
{
    static constexpr int64_t kQuietProbeMs = 1000;   // quiet this long: ask for the mark, then once per this
    static constexpr int64_t kStaleMs = 3000;        // nothing at all this long: without data

    /** connected: answered as a device; marked: its frames carry the app's mark; busy: in the bootloader or an
     *  update; lastMs: its last marked frame, 0 when none came yet. */
    static bool stale(bool connected, bool marked, bool busy, int64_t lastMs, int64_t nowMs)
    {
        return connected && marked && !busy && lastMs > 0 && nowMs - lastMs >= kStaleMs;
    }

    /** lastProbeMs: when the mark was last asked for, 0 never. */
    static bool probeDue(int64_t lastMs, int64_t lastProbeMs, int64_t nowMs)
    {
        return nowMs - lastMs >= kQuietProbeMs && nowMs - lastProbeMs >= kQuietProbeMs;
    }
};
