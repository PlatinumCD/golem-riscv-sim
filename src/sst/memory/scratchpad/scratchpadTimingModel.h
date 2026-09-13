#ifndef SST_MITTENS_SCRATCHPAD_TIMING_MODEL_H
#define SST_MITTENS_SCRATCHPAD_TIMING_MODEL_H

#include <cstdint>
#include <array>
#include <map>
#include <functional>
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
    std::uint64_t readServiceCycles = 0;
    std::uint64_t writeServiceCycles = 0;
    std::uint64_t bankConflicts = 0;
    std::uint64_t readBankConflicts = 0;
    std::uint64_t writeBankConflicts = 0;
    std::uint64_t readRequests = 0;
    std::uint64_t writeRequests = 0;
    std::uint32_t maxReadsServicedSameCycle = 0;
    std::uint32_t maxWritesServicedSameCycle = 0;
    std::uint64_t queueCycles = 0;
    std::uint64_t activeCycles = 0;
};

enum class ScratchpadDMAClient : std::uint8_t {
    GlobalRAM = 0,
    NetworkTransmit = 1,
    NetworkTransmit1 = 2,
    NetworkTransmit2 = 3,
    NetworkTransmit3 = 4,
    NetworkReceive = 5,
    NetworkReceive1 = 6,
    NetworkReceive2 = 7,
    NetworkReceive3 = 8,
    InstructionFetch = 9,
    Count = 10,
};

// Read-only reservation evidence. Cycles are CPU-domain; a beat occupies its
// port for one cycle, independently of the configured completion latency.
struct ScratchpadBeatObservation {
    int client; // -1 CPU; otherwise ScratchpadDMAClient
    bool write;
    std::uint64_t offset;
    std::uint32_t bytes;
    std::uint32_t bank;
    std::uint32_t port;
    std::uint64_t issueCycle;
    std::uint64_t serviceCycle;
    std::uint64_t completionCycle;
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
    ScratchpadSchedule scheduleCPUContiguousRun(
        std::uint64_t currentCycle,
        std::uint64_t offset,
        std::uint32_t byteCount,
        std::uint32_t repeatCount,
        bool write);
    ScratchpadSchedule scheduleDMA(
        std::uint64_t currentCycle,
        std::uint64_t offset,
        std::uint64_t byteCount,
        bool write,
        ScratchpadDMAClient client = ScratchpadDMAClient::GlobalRAM,
        bool chargeSetup = true);
    ScratchpadSchedule scheduleInstructionFetch(
        std::uint64_t currentCycle,
        std::uint64_t offset,
        std::uint64_t byteCount);

    const ScratchpadTimingConfiguration& configuration() const noexcept
    {
        return configuration_;
    }
    void setObserver(std::function<void(const ScratchpadBeatObservation&)> observer)
    {
        observer_ = std::move(observer);
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
        std::uint64_t setupCycles,
        int client = -1);
    void recordServiceCycles(bool write, std::uint64_t serviceCycles);
    void recordBeat(bool write, std::uint64_t selectedCycle);
    void discardCompletedBeatCounts(std::uint64_t currentCycle);

    ScratchpadTimingConfiguration configuration_;
    std::vector<std::vector<std::uint64_t>> readPortAvailable_;
    std::vector<std::vector<std::uint64_t>> writePortAvailable_;
    std::array<std::uint64_t,
               static_cast<std::size_t>(ScratchpadDMAClient::Count)>
        dmaAvailableCycles_{};
    std::map<std::uint64_t, std::uint32_t> readBeatsByCycle_;
    std::map<std::uint64_t, std::uint32_t> writeBeatsByCycle_;
    ScratchpadTimingStatistics statistics_;
    std::function<void(const ScratchpadBeatObservation&)> observer_;
};

} // namespace Mittens
} // namespace SST

#endif
