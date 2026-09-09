#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <unordered_map>

namespace SST::Mittens
{
// A destination may migrate between physical injection lanes only after all
// its preceding flits leave the source router's input. Local router credits
// certify that frontier. Emptying a TX-DMA burst or NIC queue does not.
// This is ordering state, not additional buffering, bandwidth or a TX lane.
class InjectionOrder final
{
  public:
    bool accepts(std::uint32_t destination, std::uint32_t lane) const
    {
        if (lane >= lanes_.size())
            throw std::out_of_range("invalid ordered injection lane");
        const auto found = destinations_.find(destination);
        return found == destinations_.end() || found->second.lane == lane;
    }

    void enqueue(std::uint32_t destination, std::uint32_t lane, std::uint32_t flits)
    {
        if (flits == 0 || !accepts(destination, lane))
            throw std::logic_error("unordered or empty injection");
        auto& queue = lanes_.at(lane);
        auto& state = destinations_.try_emplace(destination, Destination{lane, 0}).first->second;
        state.outstanding += flits;
        outstanding_.at(lane) += flits;
        if (!queue.empty() && queue.back().destination == destination)
            queue.back().flits += flits;
        else
            queue.push_back({destination, flits});
    }

    void returnedCredits(std::uint32_t lane, std::uint32_t flits)
    {
        if (flits > outstanding_.at(lane))
            throw std::logic_error("injection ordering credit underflow");
        outstanding_[lane] -= flits;
        auto& queue = lanes_[lane];
        while (flits != 0)
        {
            auto& head = queue.front();
            const auto count = head.flits < flits ? head.flits : flits;
            auto& state = destinations_.at(head.destination);
            state.outstanding -= count;
            if (state.outstanding == 0)
                destinations_.erase(head.destination);
            head.flits -= count;
            flits -= static_cast<std::uint32_t>(count);
            if (head.flits == 0)
                queue.pop_front();
        }
    }

    bool empty() const noexcept { return destinations_.empty(); }

  private:
    struct Destination { std::uint32_t lane; std::uint64_t outstanding; };
    struct Reservation { std::uint32_t destination; std::uint64_t flits; };
    std::unordered_map<std::uint32_t, Destination> destinations_;
    std::array<std::deque<Reservation>, 4> lanes_;
    std::array<std::uint64_t, 4> outstanding_{};
};
} // namespace SST::Mittens
