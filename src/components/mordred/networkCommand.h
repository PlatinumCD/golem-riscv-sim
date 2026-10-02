#pragma once
#include <sst/core/event.h>
#include <cstdint>

namespace TileComponents {
// CPU/NIU control link. Payload bytes never travel on this link.
class NetworkCommand final : public SST::Event {
public:
    std::uint64_t token=0, first=0, second=0;
    std::uint32_t operation=0;
    std::int64_t result=0;
    bool response=false;
    SST::Event* clone() override { return new NetworkCommand(*this); }
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        SST::Event::serialize_order(ser);
        SST_SER(token); SST_SER(first); SST_SER(second); SST_SER(operation);
        SST_SER(result); SST_SER(response);
    }
    ImplementSerializable(TileComponents::NetworkCommand);
};
// Static receive configuration is exchanged during SST initialization. Only
// application-slot credits use timed packets; no payload ACKs.
class NetworkSlotControl final : public SST::Event {
public:
    static constexpr std::uint32_t HeaderBytes=80;
    enum Kind : std::uint32_t { Configuration=0, Credit=1 };
    std::uint32_t kind=Configuration, source=0, destination=0, slot=0;
    std::uint64_t transferId=0, generation=1, address=0, stride=0, capacity=0, slots=0, reserved=0, protocol=2;
    SST::Event* clone() override { return new NetworkSlotControl(*this); }
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        SST::Event::serialize_order(ser);
        SST_SER(kind); SST_SER(source); SST_SER(destination); SST_SER(slot);
        SST_SER(transferId); SST_SER(generation); SST_SER(address);
        SST_SER(stride); SST_SER(capacity); SST_SER(slots); SST_SER(reserved); SST_SER(protocol);
    }
    ImplementSerializable(TileComponents::NetworkSlotControl);
};
}
