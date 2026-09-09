#ifndef SST_MITTENS_EPOCH_BARRIER_EVENT_H
#define SST_MITTENS_EPOCH_BARRIER_EVENT_H

#include <cstdint>
#include "barrierProtocol.h"

#include <sst/core/event.h>

namespace SST {
namespace Mittens {

// One event represents one tile's contribution to a deployment epoch or the
// controller's matching release.  It is deliberately independent of shard
// iteration count: a tile sends at most one Arrive event per epoch.
class EpochBarrierEvent final : public SST::Event
{
  public:
    EpochBarrierEvent(
        std::uint32_t tileId,
        std::uint32_t completedEpoch,
        EpochBarrierMessage message,
        EpochBarrierContribution contribution) :
        tileId_(tileId),
        completedEpoch_(completedEpoch),
        message_(message),
        contribution_(contribution)
    {
    }

    std::uint32_t tileId() const noexcept { return tileId_; }
    std::uint32_t completedEpoch() const noexcept
    {
        return completedEpoch_;
    }
    std::uint32_t releasedEpoch() const noexcept
    {
        return completedEpoch_ + 1U;
    }
    EpochBarrierMessage message() const noexcept { return message_; }
    EpochBarrierContribution contribution() const noexcept
    {
        return contribution_;
    }

    void serialize_order(
        SST::Core::Serialization::serializer& ser) override
    {
        SST::Event::serialize_order(ser);
        SST_SER(tileId_);
        SST_SER(completedEpoch_);
        SST_SER(message_);
        SST_SER(contribution_);
    }

  private:
    EpochBarrierEvent() = default;

    std::uint32_t tileId_ = 0;
    std::uint32_t completedEpoch_ = 0;
    EpochBarrierMessage message_ = EpochBarrierMessage::Arrive;
    EpochBarrierContribution contribution_ =
        EpochBarrierContribution::None;

    ImplementSerializable(SST::Mittens::EpochBarrierEvent);
};

} // namespace Mittens
} // namespace SST

#endif
