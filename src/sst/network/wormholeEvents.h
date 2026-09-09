#ifndef SST_MITTENS_WORMHOLE_EVENTS_H
#define SST_MITTENS_WORMHOLE_EVENTS_H

#include <cstdint>

#include <sst/core/event.h>
#include <sst/core/interfaces/simpleNetwork.h>

namespace SST {
namespace Mittens {

class WormholeFlitEvent final : public SST::Event
{
  public:
    WormholeFlitEvent(
        std::uint64_t packetId,
        std::uint32_t source,
        std::uint32_t destination,
        std::uint32_t virtualNetwork,
        std::uint32_t index,
        std::uint32_t count,
        std::uint64_t injectionTick,
        SST::Interfaces::SimpleNetwork::Request* request = nullptr) :
        packetId_(packetId),
        source_(source),
        destination_(destination),
        virtualNetwork_(virtualNetwork),
        index_(index),
        count_(count),
        injectionTick_(injectionTick),
        request_(request)
    {
    }

    ~WormholeFlitEvent() override
    {
        delete request_;
    }

    std::uint64_t packetId() const noexcept { return packetId_; }
    std::uint32_t source() const noexcept { return source_; }
    std::uint32_t destination() const noexcept { return destination_; }
    std::uint32_t virtualNetwork() const noexcept
    {
        return virtualNetwork_;
    }
    std::uint32_t index() const noexcept { return index_; }
    std::uint32_t count() const noexcept { return count_; }
    std::uint64_t injectionTick() const noexcept
    {
        return injectionTick_;
    }
    bool head() const noexcept { return index_ == 0; }
    bool tail() const noexcept { return index_ + 1 == count_; }

    void setInjectionTick(std::uint64_t value) noexcept
    {
        injectionTick_ = value;
    }

    SST::Interfaces::SimpleNetwork::Request* takeRequest() noexcept
    {
        auto* request = request_;
        request_ = nullptr;
        return request;
    }

    void serialize_order(
        SST::Core::Serialization::serializer& ser) override
    {
        SST::Event::serialize_order(ser);
        SST_SER(packetId_);
        SST_SER(source_);
        SST_SER(destination_);
        SST_SER(virtualNetwork_);
        SST_SER(index_);
        SST_SER(count_);
        SST_SER(injectionTick_);
        SST_SER(request_);
    }

  private:
    WormholeFlitEvent() = default;

    std::uint64_t packetId_ = 0;
    std::uint32_t source_ = 0;
    std::uint32_t destination_ = 0;
    std::uint32_t virtualNetwork_ = 0;
    std::uint32_t index_ = 0;
    std::uint32_t count_ = 1;
    std::uint64_t injectionTick_ = 0;
    SST::Interfaces::SimpleNetwork::Request* request_ = nullptr;

    ImplementSerializable(SST::Mittens::WormholeFlitEvent);
};

// One host event can represent a physically serialized, contiguous packet
// without changing its modeled head/tail timing.  The sender may create this
// event only after reserving every downstream flit credit for the packet.  A
// receiver that cannot prove an uninterrupted forwarding reservation expands
// it back into the ordinary per-cycle flit path.
class WormholePacketBurstEvent final : public SST::Event
{
  public:
    WormholePacketBurstEvent(
        std::uint64_t packetId,
        std::uint32_t source,
        std::uint32_t destination,
        std::uint32_t virtualNetwork,
        std::uint32_t count,
        std::uint32_t flitsPerCycle,
        std::uint64_t injectionTick,
        SST::Interfaces::SimpleNetwork::Request* request) :
        packetId_(packetId),
        source_(source),
        destination_(destination),
        virtualNetwork_(virtualNetwork),
        count_(count),
        flitsPerCycle_(flitsPerCycle),
        injectionTick_(injectionTick),
        request_(request)
    {
    }

    ~WormholePacketBurstEvent() override
    {
        delete request_;
    }

    std::uint64_t packetId() const noexcept { return packetId_; }
    std::uint32_t source() const noexcept { return source_; }
    std::uint32_t destination() const noexcept { return destination_; }
    std::uint32_t virtualNetwork() const noexcept
    {
        return virtualNetwork_;
    }
    std::uint32_t count() const noexcept { return count_; }
    std::uint32_t flitsPerCycle() const noexcept
    {
        return flitsPerCycle_;
    }
    std::uint64_t injectionTick() const noexcept
    {
        return injectionTick_;
    }

    SST::Interfaces::SimpleNetwork::Request* takeRequest() noexcept
    {
        auto* request = request_;
        request_ = nullptr;
        return request;
    }

    void serialize_order(
        SST::Core::Serialization::serializer& ser) override
    {
        SST::Event::serialize_order(ser);
        SST_SER(packetId_);
        SST_SER(source_);
        SST_SER(destination_);
        SST_SER(virtualNetwork_);
        SST_SER(count_);
        SST_SER(flitsPerCycle_);
        SST_SER(injectionTick_);
        SST_SER(request_);
    }

  private:
    WormholePacketBurstEvent() = default;

    std::uint64_t packetId_ = 0;
    std::uint32_t source_ = 0;
    std::uint32_t destination_ = 0;
    std::uint32_t virtualNetwork_ = 0;
    std::uint32_t count_ = 1;
    std::uint32_t flitsPerCycle_ = 1;
    std::uint64_t injectionTick_ = 0;
    SST::Interfaces::SimpleNetwork::Request* request_ = nullptr;

    ImplementSerializable(SST::Mittens::WormholePacketBurstEvent);
};

class WormholeCreditEvent final : public SST::Event
{
  public:
    explicit WormholeCreditEvent(std::uint32_t credits = 1) :
        credits_(credits)
    {
    }

    std::uint32_t credits() const noexcept { return credits_; }

    void serialize_order(
        SST::Core::Serialization::serializer& ser) override
    {
        SST::Event::serialize_order(ser);
        SST_SER(credits_);
    }

  private:
    std::uint32_t credits_ = 1;

    ImplementSerializable(SST::Mittens::WormholeCreditEvent);
};

// Credits in a packet reservation become available at the same physical rate
// as the serialized flits.  Carry the schedule in one event and materialize
// the credits lazily when the sender next observes that output.
class WormholeCreditBurstEvent final : public SST::Event
{
  public:
    WormholeCreditBurstEvent(
        std::uint32_t credits,
        std::uint32_t creditsPerCycle) :
        credits_(credits),
        creditsPerCycle_(creditsPerCycle)
    {
    }

    std::uint32_t credits() const noexcept { return credits_; }
    std::uint32_t creditsPerCycle() const noexcept
    {
        return creditsPerCycle_;
    }

    void serialize_order(
        SST::Core::Serialization::serializer& ser) override
    {
        SST::Event::serialize_order(ser);
        SST_SER(credits_);
        SST_SER(creditsPerCycle_);
    }

  private:
    WormholeCreditBurstEvent() = default;

    std::uint32_t credits_ = 1;
    std::uint32_t creditsPerCycle_ = 1;

    ImplementSerializable(SST::Mittens::WormholeCreditBurstEvent);
};

} // namespace Mittens
} // namespace SST

#endif
