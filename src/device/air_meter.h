#pragma once

#include "traffic_tally.h"

#include <array>
#include <cstdint>

/**
 * @brief What the link carries now, in three kinds and KB/s, for the "On the link" widget (owner 2026-09-30).
 *
 * DeviceManager adds every complete frame a live link brings (not the frames inside a proxy frame: their bytes are in
 * it already) and reads the rates once a second. A rate is the mean over the last kWindowS whole seconds, so the
 * bars move smoothly and a quiet second does not drop them to zero.
 */
class AirMeter
{
public:
    enum Kind { Echogram = 0, Telemetry, Service, Kinds };

    static constexpr int kWindowS = 3;

    /** A frame's kind from the key TrafficTally counts it under. */
    static int kindOf(const TrafficTally::Key& k)
    {
        if (k.proto == TrafficTally::ProtoMavlink1 || k.proto == TrafficTally::ProtoMavlink2 || k.proxy)
            return Telemetry;                               // the autopilot's MAVLink, raw or relayed in proxy frames
        if ((k.proto == TrafficTally::ProtoKp1 || k.proto == TrafficTally::ProtoKp2) && (k.id == 0x03 || k.id == 0x7D))
            return Echogram;                                // ID_CHART, ID_CHART_Z
        return Service;
    }

    void add(int kind, int bytes, int64_t nowMs)
    {
        if (kind < 0 || kind >= Kinds || bytes <= 0)
            return;
        Slot& s = slotFor(nowMs / 1000);
        s.bytes[kind] += bytes;
    }

    /** KB/s of each kind over the kWindowS whole seconds before the one `nowMs` falls in. */
    std::array<double, Kinds> rates(int64_t nowMs) const
    {
        std::array<double, Kinds> out{};
        const int64_t now = nowMs / 1000;
        for (int64_t sec = now - kWindowS; sec < now; ++sec) {
            const Slot& s = slots_[size_t(((sec % kSlots) + kSlots) % kSlots)];
            if (s.second != sec)
                continue;
            for (int k = 0; k < Kinds; ++k)
                out[size_t(k)] += double(s.bytes[k]);
        }
        for (double& v : out)
            v /= 1024.0 * kWindowS;
        return out;
    }

private:
    static constexpr int kSlots = kWindowS + 2;
    struct Slot {
        int64_t second = -1;
        int64_t bytes[Kinds] = {};
    };
    Slot& slotFor(int64_t second)
    {
        Slot& s = slots_[size_t(((second % kSlots) + kSlots) % kSlots)];
        if (s.second != second) {
            s.second = second;
            for (int64_t& b : s.bytes)
                b = 0;
        }
        return s;
    }
    std::array<Slot, kSlots> slots_{};
};
