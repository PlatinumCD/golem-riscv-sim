#pragma once
#include <sst/core/event.h>
#include <cstdint>
#include <vector>

namespace TileComponents {
// Explicit transaction on the endpoint's optional "requests" port.
// The endpoint fills sourceTile; identity fields survive the response unchanged.
// Addresses are byte offsets in destinationTile's SPM. Reads carry no request
// data and return exactly bytes on success. Writes carry exactly bytes; write
// responses and errors carry no data. Only live requestIds must be unique.
// Busy can be retried later. The caller owns its simulation lifetime until all
// returned response events are consumed, and owns those returned events.
// destinationTile equal to this endpoint executes directly on its local,
// router-connected SPM banks, through the same timed StandardMem service.
// Such requests never enter the network. This is an explicit request only;
// the endpoint never schedules a follow-up transfer or exposes remote CPU loads.
// posted remote writes return Accepted after reserving destination capacity and
// transferring payload ownership to the NIC. Accepted does not mean SPM commit.
// Posted request IDs must be positive and strictly increase at each origin;
// failed/Busy submissions do not advance that sequence. Posted reads and posted
// self-transactions are invalid. The optional destination "arrivals" port receives
// one Success event after every remote write commits, with response=arrival=true
// and no data; this includes ordinary writes when the port is connected.
class MordredSpmRequest final : public SST::Event {
public:
    enum Status : std::uint32_t { Success=0, Malformed=1, OutOfRange=2, ForbiddenBank=3, Busy=4, Accepted=5 };
    // Wire format: request ID/address (2x64), source/destination/byte count/
    // status/packed flags/per-peer transport sequence (6x32). The sequence
    // occupies the previously reserved sixth word; the header stays 40 bytes.
    static constexpr std::uint32_t HeaderBytes = 40;
    std::uint64_t requestId = 0, address = 0;
    std::uint32_t sourceTile = UINT32_MAX, destinationTile = 0, bytes = 0;
    bool write = false, response = false, posted = false, arrival = false;
    std::uint32_t status = Success;
    // Internal posted-packet ordering, assigned at NIC admission. The caller
    // leaves this zero; requestId remains the caller-visible identity.
    std::uint32_t transportSequence = 0;
    std::vector<std::uint8_t> data;
    SST::Event* clone() override { return new MordredSpmRequest(*this); }
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        SST::Event::serialize_order(ser);
        SST_SER(requestId); SST_SER(address); SST_SER(sourceTile); SST_SER(destinationTile);
        SST_SER(bytes); SST_SER(write); SST_SER(response); SST_SER(posted); SST_SER(arrival);
        SST_SER(status); SST_SER(transportSequence); SST_SER(data);
    }
    ImplementSerializable(TileComponents::MordredSpmRequest);
};

// Internal transport metadata; neither event is a requests/arrivals-port API.
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
    bool arrivals=false;
    std::vector<std::uint32_t> allowedBanks;
    SST::Event* clone() override { return new MordredSpmAdvertisement(*this); }
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        SST::Event::serialize_order(ser);
        SST_SER(tile); SST_SER(tiles); SST_SER(banks); SST_SER(bankWidth); SST_SER(maxBytes);
        SST_SER(slots); SST_SER(protocol); SST_SER(capacity); SST_SER(arrivals); SST_SER(allowedBanks);
    }
    ImplementSerializable(TileComponents::MordredSpmAdvertisement);
};
}
