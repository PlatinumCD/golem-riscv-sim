#ifndef SST_MITTENS_EPOCH_BARRIER_CONTROLLER_H
#define SST_MITTENS_EPOCH_BARRIER_CONTROLLER_H

#include "epochBarrierEvent.h"

#include <cstdint>
#include <vector>

#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/output.h>
#include <sst/core/timeConverter.h>

namespace SST {
namespace Mittens {

class EpochBarrierController final : public SST::Component
{
  public:
    SST_ELI_REGISTER_COMPONENT(
        EpochBarrierController,
        "mittens",
        "epochBarrierController",
        SST_ELI_ELEMENT_VERSION(0, 1, 0),
        "Timed deployment-wide materialized-execution epoch barrier",
        COMPONENT_CATEGORY_PROCESSOR)

    SST_ELI_DOCUMENT_PARAMS(
        {"tile_count", "Number of addressable tile IDs", "1"},
        {"active_tiles", "Array of active tile IDs; omitted means every tile", ""},
        {"epoch_count", "Number of completed epochs accepted, including boot epoch zero", "1"},
        {"clock", "Barrier-controller clock", "1GHz"},
        {"release_cycles", "Modeled controller cycles after the final arrival before release", "1"},
        {"stop_after_releases", "Optional clean single-thread prefix stop after this many completed releases; zero runs all epochs", "0"},
        {"verbose", "Controller diagnostic verbosity", "0"})

    SST_ELI_DOCUMENT_PORTS(
        {"barrier%(tile)d", "Bidirectional tile epoch-barrier link", {"mittens.EpochBarrierEvent"}})

    SST_ELI_DOCUMENT_STATISTICS(
        {"arrivals", "Accepted tile epoch contributions", "arrivals", 1},
        {"idle_arrivals", "Accepted idle tile epoch contributions", "arrivals", 1},
        {"releases", "Completed deployment epoch releases", "releases", 1},
        {"barrier_wait_cycles", "Cycles from first arrival through release", "cycles", 1})

    EpochBarrierController(SST::ComponentId_t id, SST::Params& params);

    void setup() override;
    void finish() override;

  private:
    void handleEvent(SST::Event* rawEvent);
    bool clockTick(SST::Cycle_t cycle);
    void ensureClock();
    void releaseCurrentEpoch(std::uint64_t cycle);
    std::uint64_t currentClockCycle();

    SST::Output output_;
    std::uint32_t tileCount_;
    std::uint32_t epochCount_;
    std::uint64_t releaseCycles_;
    std::uint32_t stopAfterReleases_;
    std::vector<SST::Link*> links_;
    std::vector<bool> activeTiles_;
    std::vector<bool> arrivedTiles_;
    std::uint32_t activeTileCount_ = 0;
    std::uint32_t arrivalCount_ = 0;
    std::uint32_t idleArrivalCount_ = 0;
    std::uint32_t currentEpoch_ = 0;
    std::uint64_t firstArrivalCycle_ = 0;
    std::uint64_t releaseCycle_ = 0;
    bool releasePending_ = false;
    bool finished_ = false;
    bool prefixComplete_ = false;
    bool clockRegistered_ = false;

    SST::Statistics::Statistic<std::uint64_t>* arrivalStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* idleArrivalStatistic_ =
        nullptr;
    SST::Statistics::Statistic<std::uint64_t>* releaseStatistic_ = nullptr;
    SST::Statistics::Statistic<std::uint64_t>* waitStatistic_ = nullptr;
    SST::TimeConverter clockTimeBase_;
    SST::Clock::HandlerBase* clockHandler_ = nullptr;
};

} // namespace Mittens
} // namespace SST

#endif
