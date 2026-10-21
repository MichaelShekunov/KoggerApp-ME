// Offline check of src/device/attitude_arbiter.h: the app follows one attitude source at a time -- the best
// kind heard, the first source of that kind -- and moves to another only when the chosen one falls silent.
// The case it was made for (2026-10-20, recording 00004_2x4.KLF): a nanoSSS's IMU yaw at 16 Hz, 20..55 deg off,
// interleaved with the autopilot's MAVLink attitude at 4 Hz.
#include "attitude_arbiter.h"

#include <cstdio>
#include <string>

namespace {

int fails = 0;
int checks = 0;

void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    ++checks;
    if (!ok) {
        ++fails;
    }
    std::printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
}

using A = AttitudeArbiter;
constexpr uint64_t kSonar = (0x1234ull << 8) | 2;   // a device at address 2
constexpr uint64_t kSonar2 = (0x5678ull << 8) | 3;
constexpr uint64_t kPilot = (1ull << 8) | 1;        // system 1, component 1

} // namespace

int main()
{
    {
        A a;
        const bool first = a.accept(A::KindDeviceImu, kSonar);
        const bool firstChanged = a.changed();
        int accepted = first ? 1 : 0;
        bool changedAgain = false;
        for (int i = 0; i < 50; ++i) {
            accepted += a.accept(A::KindDeviceImu, kSonar) ? 1 : 0;
            changedAgain = changedAgain || a.changed();
        }
        check(accepted == 51 && firstChanged && !changedAgain, "one source: every reading passes, chosen once",
              "passed " + std::to_string(accepted) + " of 51");
    }

    {
        // The sonar first (the autopilot not yet heard), then four sonar readings to one of the autopilot's.
        A a;
        int sonarBefore = 0;
        for (int i = 0; i < 8; ++i) {
            sonarBefore += a.accept(A::KindDeviceImu, kSonar) ? 1 : 0;
        }
        const bool pilotTakes = a.accept(A::KindAutopilot, kPilot);
        const bool pilotChanged = a.changed();
        int sonarAfter = 0;
        int pilotAfter = 0;
        for (int k = 0; k < 400; ++k) {
            for (int i = 0; i < 4; ++i) {
                sonarAfter += a.accept(A::KindDeviceImu, kSonar) ? 1 : 0;
            }
            pilotAfter += a.accept(A::KindAutopilot, kPilot) ? 1 : 0;
        }
        check(sonarBefore == 8, "before the autopilot: the sonar's readings pass", std::to_string(sonarBefore) + " of 8");
        check(pilotTakes && pilotChanged, "the autopilot takes over at its first reading");
        check(sonarAfter == 0 && pilotAfter == 400, "with the autopilot on: none of the sonar's 1600 readings, all 400 of the autopilot's",
              "sonar " + std::to_string(sonarAfter) + ", autopilot " + std::to_string(pilotAfter));

        // The autopilot falls silent: the sonar after 100 of its readings, not before.
        int rejected = 0;
        while (!a.accept(A::KindDeviceImu, kSonar) && rejected < 1000) {
            ++rejected;
        }
        check(rejected == 100 && a.kind() == A::KindDeviceImu && a.changed(), "the autopilot silent: the sonar after 100 readings",
              "rejected " + std::to_string(rejected));

        // ... and the autopilot back: at once.
        int sonarStill = 0;
        for (int i = 0; i < 20; ++i) {
            sonarStill += a.accept(A::KindDeviceImu, kSonar) ? 1 : 0;
        }
        const bool back = a.accept(A::KindAutopilot, kPilot);
        check(sonarStill == 20 && back && a.changed() && a.kind() == A::KindAutopilot, "the autopilot back: chosen again at once");
        check(!a.accept(A::KindDeviceImu, kSonar), "the sonar refused again with the autopilot back");
    }

    {
        // Two sonars with an IMU each: the first holds, the second only once the first is silent.
        A a;
        int one = 0;
        int two = 0;
        for (int i = 0; i < 300; ++i) {
            one += a.accept(A::KindDeviceImu, kSonar) ? 1 : 0;
            two += a.accept(A::KindDeviceImu, kSonar2) ? 1 : 0;
        }
        check(one == 300 && two == 0, "two of a kind: the first holds", "first " + std::to_string(one) + ", second " + std::to_string(two));
        // The loop above ended on a refused reading of the second: one of the 100 is already counted.
        int rejected = 1;
        while (!a.accept(A::KindDeviceImu, kSonar2) && rejected < 1000) {
            ++rejected;
        }
        check(rejected == 100 && a.key() == kSonar2, "two of a kind: the second after the first's silence",
              "rejected " + std::to_string(rejected));
    }

    {
        // The order of kinds: an autopilot over a GNSS compass over a navigation board over a device's IMU.
        A a;
        a.accept(A::KindDeviceImu, kSonar);
        const bool nav = a.accept(A::KindNav, 7);
        const bool imuAfterNav = a.accept(A::KindDeviceImu, kSonar);
        const bool hdt = a.accept(A::KindGnssHeading, 0);
        const bool navAfterHdt = a.accept(A::KindNav, 7);
        const bool pilot = a.accept(A::KindAutopilot, kPilot);
        const bool hdtAfterPilot = a.accept(A::KindGnssHeading, 0);
        check(nav && !imuAfterNav && hdt && !navAfterHdt && pilot && !hdtAfterPilot,
              "kinds: autopilot > GNSS compass > navigation board > device IMU");
    }

    {
        A a;
        for (int i = 0; i < 10; ++i) {
            a.accept(A::KindAutopilot, kPilot);
        }
        a.reset();
        const bool sonar = a.accept(A::KindDeviceImu, kSonar);
        check(sonar && a.changed() && a.kind() == A::KindDeviceImu, "reset: the next reading chooses again");
    }

    {
        // The real pattern of 00004_2x4.KLF in miniature: 16 Hz sonar, 4 Hz autopilot, yaw 50 deg apart. Count the
        // turns over 20 deg in what is passed on -- the jumping arrow -- with the arbiter and without it.
        A a;
        int turns = 0;
        int turnsMixed = 0;
        double last = -1.0;
        double lastMixed = -1.0;
        auto step = [](double from, double to) {
            double d = to - from;
            while (d > 180.0) d -= 360.0;
            while (d < -180.0) d += 360.0;
            return d < 0 ? -d : d;
        };
        for (int k = 0; k < 1000; ++k) {
            for (int i = 0; i < 5; ++i) {
                const bool pilotNow = (i == 4);
                const double yaw = pilotNow ? 350.0 + 0.01 * k : 40.0 + 0.01 * k;
                const bool pass = pilotNow ? a.accept(A::KindAutopilot, kPilot) : a.accept(A::KindDeviceImu, kSonar);
                if (pass) {
                    turns += (last >= 0.0 && step(last, yaw) > 20.0) ? 1 : 0;
                    last = yaw;
                }
                turnsMixed += (lastMixed >= 0.0 && step(lastMixed, yaw) > 20.0) ? 1 : 0;
                lastMixed = yaw;
            }
        }
        check(turns == 1 && turnsMixed > 1500, "the 16 Hz sonar and 4 Hz autopilot of 00004_2x4: one turn (the switch), not thousands",
              "with the arbiter " + std::to_string(turns) + ", mixed " + std::to_string(turnsMixed));
    }

    std::printf("attitude_check: %d of %d checks %s\n", checks - fails, checks, fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
