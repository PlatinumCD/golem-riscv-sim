#ifndef SST_MITTENS_MEMORY_INITIALIZATION_BARRIER_CONTROLLER_H
#define SST_MITTENS_MEMORY_INITIALIZATION_BARRIER_CONTROLLER_H

#include "memoryInitializationBarrierEvent.h"
#include "../profiling/performanceProfile.h"

#include <cstdint>
#include <vector>

#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/output.h>
#include <sst/core/timeConverter.h>

namespace SST {
namespace Mittens {

class MemoryInitializationBarrierController final : public SST::Component
{
  public:
    SST_ELI_REGISTER_COMPONENT(
        MemoryInitializationBarrierController,
        "mittens",
        "memoryInitializationBarrierController",
        SST_ELI_ELEMENT_VERSION(0, 1, 0),
        "Event-driven deployment memory-initialization barrier",
        COMPONENT_CATEGORY_PROCESSOR)

    SST_ELI_DOCUMENT_PARAMS(
        {"tile_count", "Number of addressable tile IDs", "1"},
        {"active_tiles", "Array of active tile IDs", "[]"},
        {"clock", "Initialization-barrier controller clock", "1GHz"},
        {"release_cycles", "Modeled cycles after the final arrival before release", "1"},
        {"release_tiles_per_cycle", "Maximum one-shot tile releases issued per controller cycle", "1"},
        {"profile_output_directory", "Directory for controller-owned initialization-barrier timelines", ""},
        {"verbose", "Controller diagnostic verbosity", "0"})

    SST_ELI_DOCUMENT_PORTS(
        {"barrier%(tile)d", "Bidirectional tile initialization-barrier link", {"mittens.MemoryInitializationBarrierEvent"}})

    SST_ELI_DOCUMENT_STATISTICS(
        {"arrivals", "Accepted tile initialization arrivals", "arrivals", 1},
        {"releases", "Completed initialization-barrier releases", "releases", 1},
        {"barrier_wait_cycles", "Cycles from first arrival through final release", "cycles", 1},
        {"tile_wait_cycles", "Sum of per-tile cycles from arrival through that tile's release", "cycles", 1})

    MemoryInitializationBarrierController(
        SST::ComponentId_t id, SST::Params& params);

    void setup() override;
    void finish() override;

  private:
    void handleEvent(SST::Event* rawEvent);
    bool clockTick(SST::Cycle_t cycle);
    void ensureClock();
    void releaseBatch(std::uint64_t cycle);
    std::uint64_t currentClockCycle();

    SST::Output output_;
    std::uint32_t tileCount_;
    std::uint64_t releaseCycles_;
    std::uint32_t releaseTilesPerCycle_;
    std::vector<SST::Link*> links_;
    std::vector<std::uint32_t> activeTileIds_;
    std::vector<bool> activeTiles_;
    std::vector<bool> arrivedTiles_;
    std::vector<std::uint64_t> arrivalCycles_;
    std::uint32_t activeTileCount_ = 0;
    std::uint32_t arrivalCount_ = 0;
    std::uint64_t firstArrivalCycle_ = 0;
    std::uint64_t releaseCycle_ = 0;
    std::uint64_t releaseStartCycle_ = 0;
    std::uint64_t lastReleaseCycle_ = 0;
    std::uint64_t tileWaitCycles_ = 0;
    std::uint32_t releasedTileCount_ = 0;
    bool releasePending_ = false;
    bool released_ = false;
    bool clockRegistered_ = false;

    SST::Statistics::Statistic<std::uint64_t>* arrivalStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* releaseStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* waitStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* tileWaitStatistic_ = nullptr;
    MemoryInitializationBarrierPerformanceProfile performanceProfile_;
    SST::TimeConverter clockTimeBase_;
    SST::Clock::HandlerBase* clockHandler_ = nullptr;
};

} // namespace Mittens
} // namespace SST

#endif
