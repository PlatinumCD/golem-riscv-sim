#include "scratchpadTimingModel.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace SST {
namespace Mittens {

namespace {

bool isPowerOfTwo(std::uint32_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

std::uint64_t checkedAdd(std::uint64_t left, std::uint64_t right)
{
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        throw std::overflow_error("scratchpad cycle count overflowed");
    }
    return left + right;
}

} // namespace

ScratchpadTimingModel::ScratchpadTimingModel(
    ScratchpadTimingConfiguration configuration) :
    configuration_(configuration)
{
    if (configuration_.capacityBytes == 0 ||
        configuration_.capacityBytes > 16 * 1024 * 1024 ||
        !isPowerOfTwo(configuration_.banks) ||
        configuration_.readPortsPerBank == 0 ||
        configuration_.writePortsPerBank == 0 ||
        configuration_.accessWidthBits == 0 ||
        configuration_.accessWidthBits % 8 != 0 ||
        configuration_.latencyCycles == 0 ||
        configuration_.dmaBytesPerCycle == 0) {
        throw std::invalid_argument(
            "invalid scratchpad timing configuration");
    }
    readPortAvailable_.assign(
        configuration_.banks,
        std::vector<std::uint64_t>(
            configuration_.readPortsPerBank, 0));
    writePortAvailable_.assign(
        configuration_.banks,
        std::vector<std::uint64_t>(
            configuration_.writePortsPerBank, 0));
}

ScratchpadSchedule ScratchpadTimingModel::scheduleCPU(
    std::uint64_t currentCycle,
    std::uint64_t offset,
    std::uint32_t byteCount,
    bool write)
{
    ++statistics_.cpuRequests;
    return schedule(
        currentCycle,
        offset,
        byteCount,
        write,
        configuration_.accessWidthBits / 8,
        0);
}

ScratchpadSchedule ScratchpadTimingModel::scheduleDMA(
    std::uint64_t currentCycle,
    std::uint64_t offset,
    std::uint64_t byteCount,
    bool write)
{
    ++statistics_.dmaTransfers;
    statistics_.dmaBytes = checkedAdd(statistics_.dmaBytes, byteCount);
    const std::uint64_t engineReady =
        std::max(currentCycle, dmaAvailableCycle_);
    ScratchpadSchedule result = schedule(
        engineReady,
        offset,
        byteCount,
        write,
        configuration_.dmaBytesPerCycle,
        configuration_.dmaSetupCycles);
    dmaAvailableCycle_ = result.completionCycle;
    result.queueCycles = result.startCycle - currentCycle;
    statistics_.queueCycles = checkedAdd(
        statistics_.queueCycles, result.queueCycles);
    return result;
}

ScratchpadSchedule ScratchpadTimingModel::schedule(
    std::uint64_t currentCycle,
    std::uint64_t offset,
    std::uint64_t byteCount,
    bool write,
    std::uint32_t beatBytes,
    std::uint64_t setupCycles)
{
    if (byteCount == 0 ||
        offset > configuration_.capacityBytes ||
        byteCount > configuration_.capacityBytes - offset) {
        throw std::out_of_range("scratchpad access is outside capacity");
    }

    const std::uint64_t firstCycle = checkedAdd(
        currentCycle, setupCycles);
    std::uint64_t issueCycle = firstCycle;
    std::uint64_t completionCycle = firstCycle;
    std::uint64_t conflicts = 0;
    const std::uint64_t bankStripe = configuration_.accessWidthBits / 8;

    for (std::uint64_t processed = 0;
         processed < byteCount;
         processed = checkedAdd(processed, beatBytes)) {
        const std::uint64_t address = checkedAdd(offset, processed);
        const std::uint32_t bank = static_cast<std::uint32_t>(
            (address / bankStripe) % configuration_.banks);
        auto& ports = write
            ? writePortAvailable_[bank]
            : readPortAvailable_[bank];
        auto port = std::min_element(ports.begin(), ports.end());
        const std::uint64_t selectedCycle =
            std::max(issueCycle, *port);
        if (selectedCycle > issueCycle) {
            ++conflicts;
        }
        *port = checkedAdd(selectedCycle, 1);
        completionCycle = std::max(
            completionCycle,
            checkedAdd(selectedCycle, configuration_.latencyCycles));
        issueCycle = checkedAdd(issueCycle, 1);
    }

    const std::uint64_t serviceCycles = completionCycle - currentCycle;
    statistics_.bankConflicts = checkedAdd(
        statistics_.bankConflicts, conflicts);
    statistics_.activeCycles = checkedAdd(
        statistics_.activeCycles, serviceCycles);
    return {
        currentCycle,
        completionCycle,
        serviceCycles,
        0,
        conflicts,
    };
}

} // namespace Mittens
} // namespace SST
