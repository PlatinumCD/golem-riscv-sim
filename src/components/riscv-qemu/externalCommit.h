#pragma once

#include <sst/core/event.h>
#include <cstdint>
#include <utility>
#include <vector>

namespace TileComponents {

// The CPU has performed its authorized access to the shared backing. Release
// the exact byte ranges retained after their StandardMem completions. The
// controller never admits overlapping ranges simultaneously, so an exact
// (address, size) pair uniquely identifies a completed held access.
class ExternalCommit final : public SST::Event {
public:
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    std::vector<Range> ranges;

    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        SST::Event::serialize_order(ser);
        SST_SER(ranges);
    }
    ImplementSerializable(TileComponents::ExternalCommit)
};

}
