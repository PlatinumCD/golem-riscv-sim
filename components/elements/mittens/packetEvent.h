#ifndef SST_MITTENS_PACKET_EVENT_H
#define SST_MITTENS_PACKET_EVENT_H

#include <cstdint>
#include <utility>
#include <vector>

#include <sst/core/event.h>

namespace SST {
namespace Mittens {

class PacketEvent final : public SST::Event
{
  public:
    explicit PacketEvent(std::uint32_t payload) : payloads_{payload} {}
    explicit PacketEvent(std::vector<std::uint32_t> payloads) :
        payloads_(std::move(payloads))
    {
    }

    const std::vector<std::uint32_t>& payloads() const noexcept
    {
        return payloads_;
    }

    void serialize_order(SST::Core::Serialization::serializer& ser) override
    {
        SST::Event::serialize_order(ser);
        SST_SER(payloads_);
    }

  private:
    PacketEvent() = default;

    std::vector<std::uint32_t> payloads_;

    ImplementSerializable(SST::Mittens::PacketEvent);
};

} // namespace Mittens
} // namespace SST

#endif
