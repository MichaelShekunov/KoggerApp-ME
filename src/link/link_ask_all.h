#pragma once

#include "link_defs.h"

#include <algorithm>

/**
 * @brief When a link asks every device on it for its version (Link::onCheckedTimerEnd, once per
 * linkCheckingTimeInterval, DeviceManager::onSendRequestAll).
 *
 * Upstream (581447cc) asks each tick while the link receives nothing (a device to find, the baud rate search) and
 * every requestAllCntBig ticks while it does: a keep-alive for a quiet device, which DevDriver::process did itself
 * before (ask when quiet for 70 ms). On the head unit's boat link (2026-09-30) that meant every device, every 300 ms,
 * answered by both Wi-Fi modules and the logger while the link was busy with the autopilot's telemetry. While data
 * comes in the link now asks only once it has had no frame for kQuietTicks: a quiet device is still kept alive by
 * its answers, a link busy on its own is not asked. A firmware update keeps upstream's cadence.
 */
class LinkAskAll
{
public:
    static constexpr int kQuietTicks = 2;

    /** The periodic count, as upstream sets it on a firmware update's start and end. */
    void restart(int count) { count_ = count; }

    /** One tick. newData: a frame came since the last tick; receiving: the link counts as receiving data (after this
     *  tick's timeouts); upgrading: a firmware update runs. True: ask every device now. */
    bool tick(bool newData, bool receiving, bool upgrading)
    {
        quiet_ = newData ? 0 : std::min(quiet_ + 1, 1000);
        const bool periodic = count_ <= 0;
        if (periodic)
            count_ = upgrading ? requestAllCntSmall : receiving ? requestAllCntBig : requestAllCntSmall;
        --count_;
        return !receiving || (periodic && (upgrading || quiet_ >= kQuietTicks));
    }

private:
    int count_ = requestAllCntBig;
    int quiet_ = 0;
};
