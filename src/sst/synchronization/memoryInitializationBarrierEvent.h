#ifndef SST_MITTENS_MEMORY_INITIALIZATION_BARRIER_EVENT_H
#define SST_MITTENS_MEMORY_INITIALIZATION_BARRIER_EVENT_H

#include <cstdint>
#include "barrierProtocol.h"

#include <sst/core/event.h>

namespace SST {
namespace Mittens {

// One event represents either one active tile's completed initialization or
// the controller's matching one-shot release.  Initialization is deployment
// scoped, so there is deliberately no epoch or runtime-iteration field.
class MemoryInitializationBarrierEvent final : public SST::Event
{
  public:
    MemoryInitializationBarrierEvent(
        std::uint32_t tileId,
        MemoryInitializationBarrierMessage message) :
        tileId_(tileId), message_(message)
    {
    }

    std::uint32_t tileId() const noexcept { return tileId_; }
    MemoryInitializationBarrierMessage message() const noexcept
    {
        return message_;
    }

    void serialize_order(
        SST::Core::Serialization::serializer& ser) override
    {
        SST::Event::serialize_order(ser);
        SST_SER(tileId_);
        SST_SER(message_);
    }

  private:
    MemoryInitializationBarrierEvent() = default;

    std::uint32_t tileId_ = 0;
    MemoryInitializationBarrierMessage message_ =
        MemoryInitializationBarrierMessage::Arrive;

    ImplementSerializable(
        SST::Mittens::MemoryInitializationBarrierEvent);
};

} // namespace Mittens
} // namespace SST

#endif
