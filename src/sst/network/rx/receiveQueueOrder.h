#pragma once

#include <algorithm>

namespace SST::Mittens {

// A completed frame's suffix must follow every queued piece of that frame,
// including device-private pieces emitted while the frame was still arriving.
template <typename Queue, typename MatchesFrame>
auto receivePayloadInsertionPoint(Queue& queue, MatchesFrame matchesFrame)
{
    const auto last = std::find_if(queue.rbegin(), queue.rend(), matchesFrame);
    if (last != queue.rend()) {
        return last.base();
    }
    return std::find_if(queue.begin(), queue.end(),
                        [](const auto& burst) { return burst.softwareVisible; });
}

} // namespace SST::Mittens
