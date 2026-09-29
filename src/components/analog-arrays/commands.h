#pragma once
#include <sst/core/event.h>
#include <cstdint>
#include <vector>

namespace TileComponents {
enum class Operation : std::uint8_t { Program, Load, Execute, Store };
enum class CommandStatus : std::uint8_t {
    Request = 0, Accepted = 1, Complete = 2, Busy = 3, Error = 4, Started = 5,
    Guaranteed = 6, Captured = 7
};

// Program/Load carry little-endian float32 register bytes. Store returns the
// selected output elements in its Complete reply. Execute computes y=W*x and
// requires an empty range/payload. Accepted tokens stay live until completion.
// With array_pipeline_enabled, Execute additionally replies Started after its
// input snapshot is captured and a compute/output slot is reserved. Complete
// follows after execution; errors are reported before Started. Store consumes
// the oldest result only once successful chunks cover every output row.
// Deferred transfers may receive Guaranteed instead of Accepted: all guest
// errors have been ruled out and subsequent completion must succeed. Deferred
// Program/Load additionally receive Captured after their last input byte is
// consumed by the timed link, before any programming delay. Execute is never
// deferred and retains its Started contract.
class ArrayCommand final : public SST::Event {
public:
    Operation operation = Operation::Execute;
    CommandStatus status = CommandStatus::Request;
    std::uint32_t array = 0;
    std::uint64_t token = 0, cycle = 0;
    std::uint64_t elementOffset = 0;
    std::uint32_t elementCount = 0;
    std::vector<std::uint8_t> payload;
    bool deferred = false;
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        SST::Event::serialize_order(ser);
        SST_SER(operation); SST_SER(status); SST_SER(array);
        SST_SER(token); SST_SER(cycle);
        SST_SER(elementOffset); SST_SER(elementCount);
        SST_SER(payload);
        SST_SER(deferred);
    }
    ImplementSerializable(TileComponents::ArrayCommand)
};
}
