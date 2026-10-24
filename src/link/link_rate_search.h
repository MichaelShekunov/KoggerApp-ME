#pragma once

#include <QList>

#include <algorithm>
#include <cstdint>
#include <functional>

/**
 * @brief The order of a serial link's baud rate search and whether a step may send (Link::onCheckedTimerEnd, a step
 * every linkNumTimeoutsSmall checks while the link receives nothing).
 *
 * Bytes sent well below a line's rate look like a BREAK to a receiver on it: one request at 115200 on a GODIO ring
 * running at 460800 takes the leader out of its role for two periods, and both boards lose pings (the firmware's
 * session, 2026-10-24, five times of five). Bytes faster than the line do no such harm. So a pass first listens:
 * every rate from the fastest down, nothing sent, which finds any device that talks unasked (a ring streams its
 * echograms). A pass that heard nothing is followed by one that asks, again from the fastest rate down: a quiet
 * device answers at its own rate before the search sends below it. Listening and asking passes alternate until
 * something comes in (docs/RING_UPDATE.md).
 */
class LinkRateSearch
{
public:
    struct Step {
        uint32_t rate = 0;
        bool asks = false;   // the devices may send while the link holds this rate
    };

    /** The link's search list, kept once each and fastest first; the search starts over with a listening pass. */
    void setRates(const QList<uint32_t>& rates)
    {
        rates_.clear();
        for (uint32_t r : rates)
            if (r && !rates_.contains(r))
                rates_.append(r);
        std::sort(rates_.begin(), rates_.end(), std::greater<uint32_t>());
        reset();
    }

    /** Back to the start of a listening pass: something came in, or the port opened. */
    void reset()
    {
        index_ = 0;
        asking_ = false;
    }

    bool empty() const { return rates_.isEmpty(); }

    /** The next step: its rate and whether it asks. After a pass's last rate the other kind of pass begins. */
    Step next()
    {
        if (rates_.isEmpty())
            return {};
        const Step s{ rates_.at(index_), asking_ };
        if (++index_ >= rates_.size()) {
            index_ = 0;
            asking_ = !asking_;
        }
        return s;
    }

private:
    QList<uint32_t> rates_;
    int index_ = 0;
    bool asking_ = false;
};
