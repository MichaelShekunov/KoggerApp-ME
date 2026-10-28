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

    // A device without the mark is asked (restart, mark, VERSION v0-v2) each process() tick, 200 ms, for
    // kUnansweredFastMs, then every kUnansweredSlowMs. The link's search address 0 is one: every device answers it
    // under its own address, so its driver never gets a frame (2026-09-30, the head unit: 5 of the 11.7 VERSION
    // answers a second of each device on the boat link). So is a device whose frames carry the mark only now and
    // then (the sonar behind the logger, *Inference*): each restart re-read its setup. It starts over after
    // kStableMarkMs of marked ticks (the next loss of the mark is a restart, asked at once) or on a frame after
    // kSilenceMs without one (a device found or back: the baud rate search on a direct link).
    static constexpr int64_t kUnansweredFastMs = 10000;
    static constexpr int64_t kUnansweredSlowMs = 5000;
    static constexpr int64_t kStableMarkMs = 2000;
    static constexpr int64_t kSilenceMs = 1000;

    /** The search's first asks on a connection, made whatever the line does (searchHeld). */
    static constexpr int kSearchFirstAsks = 3;

    /** Whether the link's search waits. Address 0's driver is only the search while it has never had a frame
     *  (ownLastFrameMs 0); a device at address 0 answers it and is asked as any other. While another device of the
     *  line has been heard within kStaleMs (otherLastFrameMs), the search asks nothing: every device answers it under
     *  its own address, so it finds nothing new, and the broadcast restart, mark (a SETTING) and VERSION only
     *  disturb -- the following board of a GODIO ring answers them instead of waiting for the leader's BREAK and
     *  drops a ping each time (L412_GODIO docs/RING_HOLES_2026-10-23.md; 6 asks, 6 pings of [2] missing). When the
     *  line goes quiet (a cable out, a rate change), it asks again.
     *  The first kSearchFirstAsks asks of a connection (asksMade) are never held: the logger (Recorder) sends
     *  nothing by itself, and a sonar streaming through it reaches the app before the search's first tick, so the
     *  logger was never found (2026-10-28, COM5: the logger answers VERSION at 0 and 250, the line carried only the
     *  sonar's frames). A ring loses at most that many pings on connecting. A device joined later to a line that
     *  answers already and sends nothing by itself is found when the line goes quiet, or on reconnecting. */
    static bool searchHeld(int64_t ownLastFrameMs, int64_t otherLastFrameMs, int asksMade, int64_t nowMs)
    {
        return asksMade >= kSearchFirstAsks && ownLastFrameMs == 0 && otherLastFrameMs > 0
               && nowMs - otherLastFrameMs < kStaleMs;
    }

    /** The back-off of one device: frames from protoComplete, ticks from process(). */
    struct Unanswered
    {
        int64_t sinceMs = 0;         // since when it has been without the mark, 0: starts over at the next tick
        int64_t lastAskMs = 0;       // the last ask
        int64_t markedSinceMs = 0;   // the current run of marked ticks, 0: none
        int64_t lastFrameMs = 0;     // its last frame, 0: none yet

        /** A frame from the device. */
        void heard(int64_t nowMs)
        {
            if (lastFrameMs == 0 || nowMs - lastFrameMs >= kSilenceMs)
                sinceMs = 0;
            lastFrameMs = nowMs;
        }

        /** A tick with the mark. */
        void marked(int64_t nowMs)
        {
            if (markedSinceMs == 0)
                markedSinceMs = nowMs;
            if (nowMs - markedSinceMs >= kStableMarkMs)
                sinceMs = 0;
        }

        /** A tick without the mark: whether to ask now. */
        bool askDue(int64_t nowMs)
        {
            markedSinceMs = 0;
            if (sinceMs == 0)
                sinceMs = nowMs;
            if (nowMs - sinceMs >= kUnansweredFastMs && nowMs - lastAskMs < kUnansweredSlowMs)
                return false;
            lastAskMs = nowMs;
            return true;
        }
    };
};
