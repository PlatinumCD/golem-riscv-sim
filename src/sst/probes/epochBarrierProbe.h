#ifndef SST_MITTENS_EPOCH_BARRIER_PROBE_H
#define SST_MITTENS_EPOCH_BARRIER_PROBE_H

#include "../synchronization/epochBarrierEvent.h"

#include <cstdint>
#include <string>
#include <vector>

#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/output.h>
#include <sst/core/timeConverter.h>

namespace SST {
namespace Mittens {

// SST-only validation endpoint for EpochBarrierController.  Production tiles
// use the same event contract after their fd-41 runtime arrival is integrated.
class EpochBarrierProbe final : public SST::Component
{
  public:
    SST_ELI_REGISTER_COMPONENT(
        EpochBarrierProbe,
        "mittens",
        "epochBarrierProbe",
        SST_ELI_ELEMENT_VERSION(0, 1, 0),
        "Synthetic endpoint for deployment epoch-barrier validation",
        COMPONENT_CATEGORY_UNCATEGORIZED)

    SST_ELI_DOCUMENT_PARAMS(
        {"tile_id", "Tile ID represented by this endpoint", "0"},
        {"epoch_count", "Number of completed epochs to contribute", "1"},
        {"arrival_delays", "Per-epoch delay in probe-clock cycles after the preceding release", "[1]"},
        {"idle_epochs", "Epoch IDs contributed with explicit idle status", "[]"},
        {"invalid_mode", "Focused invalid transition: none, duplicate, future, or stale", "none"},
        {"clock", "Probe clock", "1GHz"},
        {"verbose", "Probe diagnostic verbosity", "0"})

    SST_ELI_DOCUMENT_PORTS(
        {"barrier", "Bidirectional epoch-barrier controller link", {"mittens.EpochBarrierEvent"}})

    SST_ELI_DOCUMENT_STATISTICS()

    EpochBarrierProbe(SST::ComponentId_t id, SST::Params& params);

  private:
    bool clockTick(SST::Cycle_t cycle);
    void handleEvent(SST::Event* rawEvent);
    bool idleEpoch(std::uint32_t epoch) const;
    void sendArrival(std::uint64_t cycle);

    SST::Output output_;
    std::uint32_t tileId_;
    std::uint32_t epochCount_;
    std::vector<std::uint64_t> arrivalDelays_;
    std::vector<std::uint32_t> idleEpochs_;
    std::string invalidMode_;
    SST::Link* barrierLink_ = nullptr;
    SST::TimeConverter clockTimeBase_;
    std::uint32_t currentEpoch_ = 0;
    std::uint64_t nextArrivalCycle_ = 0;
    std::uint64_t arrivalCycle_ = 0;
    bool arrivalSent_ = false;
    bool finished_ = false;
};

} // namespace Mittens
} // namespace SST

#endif
