#ifndef SST_MITTENS_PACKET_EVENT_H
#define SST_MITTENS_PACKET_EVENT_H

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <sst/core/event.h>

namespace SST {
namespace Mittens {

class PacketEvent final : public SST::Event
{
  public:
    static constexpr std::uint32_t InvalidRouteId =
        std::numeric_limits<std::uint32_t>::max();

    struct Metadata {
        std::uint64_t packetId = 0;
        std::uint64_t readyTick = 0;
        std::uint64_t injectionTick = 0;
        std::uint32_t routeId = InvalidRouteId;
        std::uint64_t executionId = 0;
        std::uint32_t protocolWords = 0;
        std::uint32_t payloadWords = 0;
    };

    explicit PacketEvent(std::uint32_t payload) : payloads_{payload} {}
    explicit PacketEvent(std::vector<std::uint32_t> payloads) :
        payloads_(std::move(payloads))
    {
    }
    PacketEvent(std::vector<std::uint32_t> payloads, Metadata metadata) :
        payloads_(std::move(payloads)),
        metadata_(metadata)
    {
    }

    const std::vector<std::uint32_t>& payloads() const noexcept
    {
        return payloads_;
    }
    const Metadata& metadata() const noexcept { return metadata_; }

    void serialize_order(SST::Core::Serialization::serializer& ser) override
    {
        SST::Event::serialize_order(ser);
        SST_SER(payloads_);
        SST_SER(metadata_.packetId);
        SST_SER(metadata_.readyTick);
        SST_SER(metadata_.injectionTick);
        SST_SER(metadata_.routeId);
        SST_SER(metadata_.executionId);
        SST_SER(metadata_.protocolWords);
        SST_SER(metadata_.payloadWords);
    }

  private:
    PacketEvent() = default;

    std::vector<std::uint32_t> payloads_;
    Metadata metadata_;

    ImplementSerializable(SST::Mittens::PacketEvent);
};

} // namespace Mittens
} // namespace SST

#endif
