#pragma once

#include <cstdint>

/// Picks one source of the boat's attitude when several report it.
///
/// An autopilot (MAVLink ATTITUDE), a GNSS compass (NMEA HDT), a navigation board (ID_NAV) and a sonar's own
/// IMU (ID_ATTITUDE) can all be on one boat, and they do not agree: on 2026-10-20 a nanoSSS's IMU yaw ran
/// 20..55 deg off the autopilot's, whose yaw matched the GPS course. Passed on together, the heading jumped
/// between them at every reading (5758 steps over 20 deg in 16 minutes) and the mosaic laid the pings along
/// both. The arbiter passes one source: the best kind heard, the first source of that kind, and it moves to
/// another only after the chosen one has stayed silent through `staleReadings` readings of the others --
/// readings rather than time, so a recording read at disk speed behaves as it did live.
class AttitudeArbiter
{
public:
    /// Higher is preferred.
    enum Kind : int {
        KindNone        = 0,
        KindDeviceImu   = 1,  ///< a device's own IMU (ID_ATTITUDE), or the course of the platform's location
        KindNav         = 2,  ///< a navigation board (ID_NAV)
        KindGnssHeading = 3,  ///< a GNSS compass (NMEA HDT)
        KindAutopilot   = 4   ///< the autopilot's estimate (MAVLink ATTITUDE)
    };

    explicit AttitudeArbiter(int staleReadings = 100) : stale_(staleReadings) {}

    /// True when this reading is to be used. `key` tells apart sources of one kind (devices, autopilots).
    bool accept(Kind kind, uint64_t key)
    {
        const bool current = kind_ != KindNone && kind == kind_ && key == key_;
        if (current || kind_ == KindNone || kind > kind_ || silent_ >= stale_) {
            changed_ = !current;
            kind_ = kind;
            key_ = key;
            silent_ = 0;
            return true;
        }
        ++silent_;
        changed_ = false;
        return false;
    }

    /// The last accept() moved to another source (or took the first one).
    bool changed() const { return changed_; }
    Kind kind() const { return kind_; }
    uint64_t key() const { return key_; }

    /// A new recording or a new set of devices: the next reading chooses again.
    void reset()
    {
        kind_ = KindNone;
        key_ = 0;
        silent_ = 0;
        changed_ = false;
    }

private:
    int stale_;
    Kind kind_ = KindNone;
    uint64_t key_ = 0;
    int silent_ = 0;
    bool changed_ = false;
};
