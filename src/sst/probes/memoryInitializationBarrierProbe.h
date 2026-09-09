#ifndef SST_MITTENS_MEMORY_INITIALIZATION_BARRIER_PROBE_H
#define SST_MITTENS_MEMORY_INITIALIZATION_BARRIER_PROBE_H

#include "../synchronization/memoryInitializationBarrierEvent.h"

#include <cstdint>
#include <string>

#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/output.h>
#include <sst/core/timeConverter.h>

namespace SST {
namespace Mittens {

// SST-only endpoint for deterministic controller validation. Production tiles
// use the same event type after their aggregate initialization handshake.
class MemoryInitializationBarrierProbe final : public SST::Component
{
  public:
    SST_ELI_REGISTER_COMPONENT(
        MemoryInitializationBarrierProbe,
        "mittens",
        "memoryInitializationBarrierProbe",
        SST_ELI_ELEMENT_VERSION(0, 1, 0),
        "Synthetic endpoint for memory-initialization barrier validation",
        COMPONENT_CATEGORY_UNCATEGORIZED)

    SST_ELI_DOCUMENT_PARAMS(
        {"tile_id", "Tile ID represented by this endpoint", "0"},
        {"arrival_delay", "Arrival delay in probe-clock cycles", "1"},
        {"invalid_mode", "Focused invalid transition: none or duplicate", "none"},
        {"clock", "Probe clock", "1GHz"},
        {"verbose", "Probe diagnostic verbosity", "0"})

    SST_ELI_DOCUMENT_PORTS(
        {"barrier", "Bidirectional initialization-barrier controller link", {"mittens.MemoryInitializationBarrierEvent"}})

    SST_ELI_DOCUMENT_STATISTICS()

    MemoryInitializationBarrierProbe(
        SST::ComponentId_t id, SST::Params& params);

  private:
    bool clockTick(SST::Cycle_t cycle);
    void handleEvent(SST::Event* rawEvent);

    SST::Output output_;
    std::uint32_t tileId_;
    std::uint64_t arrivalDelay_;
    std::string invalidMode_;
    SST::Link* barrierLink_ = nullptr;
    SST::TimeConverter clockTimeBase_;
    std::uint64_t arrivalCycle_ = 0;
    bool arrivalSent_ = false;
    bool released_ = false;
};

} // namespace Mittens
} // namespace SST

#endif
