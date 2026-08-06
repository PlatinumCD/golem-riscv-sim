#ifndef SST_MITTENS_SCRATCHPAD_TIMING_MODEL_H
#define SST_MITTENS_SCRATCHPAD_TIMING_MODEL_H

#include <cstdint>
#include <vector>

namespace SST {
namespace Mittens {

struct ScratchpadTimingConfiguration {
    std::uint64_t capacityBytes = 256 * 1024;
    std::uint32_t banks = 8;
    std::uint32_t readPortsPerBank = 1;
    std::uint32_t writePortsPerBank = 1;
    std::uint32_t accessWidthBits = 256;
    std::uint64_t latencyCycles = 1;
    std::uint64_t dmaSetupCycles = 8;
    std::uint32_t dmaBytesPerCycle = 32;
};

struct ScratchpadSchedule {
    std::uint64_t startCycle;
    std::uint64_t completionCycle;
    std::uint64_t serviceCycles;
    std::uint64_t queueCycles;
    std::uint64_t bankConflicts;
};

struct ScratchpadTimingStatistics {
    std::uint64_t cpuRequests = 0;
    std::uint64_t dmaTransfers = 0;
    std::uint64_t dmaBytes = 0;
    std::uint64_t bankConflicts = 0;
    std::uint64_t queueCycles = 0;
    std::uint64_t activeCycles = 0;
};

class ScratchpadTimingModel final
{
  public:
    explicit ScratchpadTimingModel(
        ScratchpadTimingConfiguration configuration);

    ScratchpadSchedule scheduleCPU(
        std::uint64_t currentCycle,
        std::uint64_t offset,
        std::uint32_t byteCount,
        bool write);
    ScratchpadSchedule scheduleDMA(
        std::uint64_t currentCycle,
        std::uint64_t offset,
        std::uint64_t byteCount,
        bool write);

    const ScratchpadTimingConfiguration& configuration() const noexcept
    {
        return configuration_;
    }
    const ScratchpadTimingStatistics& statistics() const noexcept
    {
        return statistics_;
    }

  private:
    ScratchpadSchedule schedule(
        std::uint64_t currentCycle,
        std::uint64_t offset,
        std::uint64_t byteCount,
        bool write,
        std::uint32_t beatBytes,
        std::uint64_t setupCycles);

    ScratchpadTimingConfiguration configuration_;
    std::vector<std::vector<std::uint64_t>> readPortAvailable_;
    std::vector<std::vector<std::uint64_t>> writePortAvailable_;
    std::uint64_t dmaAvailableCycle_ = 0;
    ScratchpadTimingStatistics statistics_;
};

} // namespace Mittens
} // namespace SST

#endif
