#ifndef SST_MITTENS_PACKET_EVENT_H
#define SST_MITTENS_PACKET_EVENT_H

#include <cstdint>

#include <sst/core/event.h>

namespace SST {
namespace Mittens {

class PacketEvent final : public SST::Event
{
  public:
    explicit PacketEvent(std::uint32_t payload) : payload_(payload) {}

    std::uint32_t payload() const noexcept { return payload_; }

    void serialize_order(SST::Core::Serialization::serializer& ser) override
    {
        SST::Event::serialize_order(ser);
        SST_SER(payload_);
    }

  private:
    PacketEvent() = default;

    std::uint32_t payload_ = 0;

    ImplementSerializable(SST::Mittens::PacketEvent);
};

} // namespace Mittens
} // namespace SST

#endif
