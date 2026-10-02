#pragma once
#include <sst/core/event.h>
#include <cstdint>
#include <vector>

namespace TileComponents {
// Private NIU work record. Reads fetch a local descriptor or payload through
// timed StandardMem. Only payload writes enter the network, after reserving
// destination message storage. No remote reads or response packets exist.
// Addresses are byte offsets in the destination SPM; the NIU supplies all
// packet identities and validates them against the deployed whole message.
class MordredMessagePacket final : public SST::Event {
public:
    enum Status : std::uint32_t { Success=0, Malformed=1, OutOfRange=2, ForbiddenBank=3, Busy=4, Accepted=5 };
    // Modeled wire format: 40 bytes of transport fields, followed by 56 bytes
    // of whole-message identity and storage metadata. One 96-byte format is
    // supported. Local bookkeeping fields do not introduce wire operations.
    static constexpr std::uint32_t HeaderBytes = 96;
    std::uint64_t requestId = 0, address = 0;
    std::uint32_t sourceTile = UINT32_MAX, destinationTile = 0, bytes = 0;
    bool write = false, metadata = false;
    std::uint32_t status = Success;
    // Per-peer packet order, assigned at NIC admission.
    std::uint32_t transportSequence = 0;
    // 56-byte message extension: two 32-bit words and six 64-bit words.
    std::uint32_t messageSlot=0, messageReserved=0;
    std::uint64_t messageGeneration=0, messageSequence=0, messageOffset=0, messageBytes=0;
    std::uint64_t transferId=0, invocationId=0;
    std::vector<std::uint8_t> data;
    SST::Event* clone() override { return new MordredMessagePacket(*this); }
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        SST::Event::serialize_order(ser);
        SST_SER(requestId); SST_SER(address); SST_SER(sourceTile); SST_SER(destinationTile);
        SST_SER(bytes); SST_SER(write); SST_SER(metadata);
        SST_SER(status); SST_SER(transportSequence); SST_SER(data);
        SST_SER(messageSlot);
        SST_SER(messageReserved); SST_SER(messageGeneration); SST_SER(messageSequence);
        SST_SER(messageOffset); SST_SER(messageBytes); SST_SER(transferId); SST_SER(invocationId);
    }
    ImplementSerializable(TileComponents::MordredMessagePacket);
};

// Internal packet-credit and initialization metadata.
// Advertisements are untimed initialization. Credits consume real modeled
// network bandwidth: four uint32 fields, padded to at least two whole flits.
class MordredSpmCredit final : public SST::Event {
public:
    static constexpr std::uint32_t HeaderBytes = 16;
    std::uint32_t sourceTile=0, destinationTile=0, slots=0, protocol=1;
    SST::Event* clone() override { return new MordredSpmCredit(*this); }
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        SST::Event::serialize_order(ser);
        SST_SER(sourceTile); SST_SER(destinationTile); SST_SER(slots); SST_SER(protocol);
    }
    ImplementSerializable(TileComponents::MordredSpmCredit);
};
class MordredSpmAdvertisement final : public SST::Event {
public:
    std::uint32_t tile=0, tiles=0, banks=0, bankWidth=0, maxBytes=0, slots=0, protocol=2;
    std::uint64_t capacity=0;
    std::vector<std::uint32_t> allowedBanks;
    SST::Event* clone() override { return new MordredSpmAdvertisement(*this); }
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        SST::Event::serialize_order(ser);
        SST_SER(tile); SST_SER(tiles); SST_SER(banks); SST_SER(bankWidth); SST_SER(maxBytes);
        SST_SER(slots); SST_SER(protocol); SST_SER(capacity); SST_SER(allowedBanks);
    }
    ImplementSerializable(TileComponents::MordredSpmAdvertisement);
};
}
