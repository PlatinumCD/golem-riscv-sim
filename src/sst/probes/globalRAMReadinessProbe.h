#ifndef SST_MITTENS_GLOBAL_RAM_READINESS_PROBE_H
#define SST_MITTENS_GLOBAL_RAM_READINESS_PROBE_H

#include "../memory/globalDMAEvent.h"

#include <cstdint>
#include <string>

#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/output.h>

namespace SST {
namespace Mittens {

class GlobalRAMReadinessProbe final : public SST::Component
{
  public:
    SST_ELI_REGISTER_COMPONENT(
        GlobalRAMReadinessProbe,
        "mittens",
        "globalRAMReadinessProbe",
        SST_ELI_ELEMENT_VERSION(0, 1, 0),
        "Exact global-RAM readiness protocol test probe",
        COMPONENT_CATEGORY_UNCATEGORIZED)

    SST_ELI_DOCUMENT_PARAMS(
        {"tile_id", "Probe endpoint index", "0"},
        {"scenario", "Probe scenario: coverage, demand, priority, reservation, or duplicate", "coverage"})

    SST_ELI_DOCUMENT_PORTS(
        {"ram", "Bidirectional global RAM link", {"mittens.GlobalDMAEvent"}})

    GlobalRAMReadinessProbe(SST::ComponentId_t id, SST::Params& params);

  private:
    bool start(SST::Cycle_t cycle);
    void handleCompletion(SST::Event* event);
    void handleProducerStart(SST::Event* event);
    void submitCoveragePhase();
    void submitPriorityScenario();
    void submitReservationScenario();
    void submitDemandScenario();
    void submitDuplicateScenario();
    void submit(
        std::uint64_t executionId,
        std::uint32_t tokenId,
        std::uint64_t globalOffset,
        std::uint32_t byteCount,
        GlobalDMADirection direction,
        std::uint32_t flags);
    void submitTeardown(std::uint64_t executionId);

    SST::Output output_;
    std::uint32_t tileId_ = 0;
    std::string scenario_;
    SST::Link* ramLink_ = nullptr;
    SST::Link* producerStartLink_ = nullptr;
    std::uint32_t phase_ = 0;
    std::uint32_t pendingData_ = 0;
    std::uint32_t priorityCompletionIndex_ = 0;
    std::uint32_t reservationCompletionIndex_ = 0;
    std::uint32_t demandCompletionIndex_ = 0;
};

} // namespace Mittens
} // namespace SST

#endif
