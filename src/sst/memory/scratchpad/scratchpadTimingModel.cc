#include "scratchpadTimingModel.h"
#include "../addressRegion.h"

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

std::uint64_t checkedMultiply(std::uint64_t left, std::uint64_t right)
{
    if (left != 0 &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        throw std::overflow_error("scratchpad count overflowed");
    }
    return left * right;
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
    const ScratchpadSchedule result = schedule(
        currentCycle,
        offset,
        byteCount,
        write,
        configuration_.accessWidthBits / 8,
        0);
    recordServiceCycles(write, result.serviceCycles);
    return result;
}

ScratchpadSchedule ScratchpadTimingModel::scheduleCPUContiguousRun(
    std::uint64_t currentCycle,
    std::uint64_t offset,
    std::uint32_t byteCount,
    std::uint32_t repeatCount,
    bool write)
{
    if (byteCount == 0 || repeatCount == 0) {
        throw std::out_of_range("scratchpad run is empty");
    }
    const std::uint64_t runBytes = checkedMultiply(
        byteCount, repeatCount);
    if (!AddressRegion{0, configuration_.capacityBytes}.containsRange(offset, runBytes)) {
        throw std::out_of_range("scratchpad run is outside capacity");
    }
    statistics_.cpuRequests = checkedAdd(
        statistics_.cpuRequests, repeatCount);

    const std::uint64_t beatBytes =
        configuration_.accessWidthBits / 8;
    auto& availability = write
        ? writePortAvailable_
        : readPortAvailable_;
    const bool onePortPerBank = write
        ? configuration_.writePortsPerBank == 1
        : configuration_.readPortsPerBank == 1;
    const bool portsReady = std::all_of(
        availability.begin(), availability.end(),
        [currentCycle](const std::vector<std::uint64_t>& ports) {
            return std::all_of(
                ports.begin(), ports.end(),
                [currentCycle](std::uint64_t ready) {
                    return ready <= currentCycle;
                });
        });

    /*
     * Every compacted record is a sequence of one-beat CPU accesses.  When
     * the selected one-port banks are already ready (the normal Tile call
     * contract), each original request completes exactly latencyCycles after
     * the prior one.  Update the final per-bank availability from only the
     * bounded address-map period instead of replaying every vector element.
     */
    if (byteCount <= beatBytes && onePortPerBank && portsReady) {
        const std::uint64_t serviceCycles = checkedMultiply(
            configuration_.latencyCycles, repeatCount);
        const std::uint64_t completionCycle = checkedAdd(
            currentCycle, serviceCycles);
        const std::uint64_t bankStripe = beatBytes;
        const std::uint64_t periodBound = checkedMultiply(
            configuration_.banks, bankStripe);
        const std::uint64_t inspectCount = std::min<std::uint64_t>(
            repeatCount, periodBound);
        std::vector<bool> updated(configuration_.banks, false);
        for (std::uint64_t back = 0; back < inspectCount; ++back) {
            const std::uint64_t index = repeatCount - 1 - back;
            const std::uint64_t address = checkedAdd(
                offset, checkedMultiply(index, byteCount));
            const std::uint32_t bank = static_cast<std::uint32_t>(
                (address / bankStripe) % configuration_.banks);
            if (!updated[bank]) {
                availability[bank][0] = checkedAdd(
                    checkedAdd(
                        currentCycle,
                        checkedMultiply(
                            index, configuration_.latencyCycles)),
                    1);
                updated[bank] = true;
            }
        }
        statistics_.activeCycles = checkedAdd(
            statistics_.activeCycles, serviceCycles);
        std::uint64_t& requests = write
            ? statistics_.writeRequests
            : statistics_.readRequests;
        requests = checkedAdd(requests, repeatCount);
        std::uint32_t& maximum = write
            ? statistics_.maxWritesServicedSameCycle
            : statistics_.maxReadsServicedSameCycle;
        maximum = std::max(maximum, UINT32_C(1));
        const ScratchpadSchedule result = {
            currentCycle,
            completionCycle,
            serviceCycles,
            0,
            0,
        };
        if (observer_) {
            for (std::uint64_t i = 0; i < repeatCount; ++i) {
                const auto address = offset + i * byteCount;
                const auto cycle = currentCycle + i * configuration_.latencyCycles;
                observer_({-1, write, address, byteCount,
                    static_cast<std::uint32_t>((address / bankStripe) % configuration_.banks),
                    0, cycle, cycle, cycle + configuration_.latencyCycles});
            }
        }
        recordServiceCycles(write, result.serviceCycles);
        return result;
    }

    std::uint64_t cycle = currentCycle;
    std::uint64_t conflicts = 0;
    for (std::uint64_t index = 0; index < repeatCount; ++index) {
        const ScratchpadSchedule result = schedule(
            cycle,
            checkedAdd(offset, checkedMultiply(index, byteCount)),
            byteCount,
            write,
            static_cast<std::uint32_t>(beatBytes),
            0);
        cycle = result.completionCycle;
        conflicts = checkedAdd(conflicts, result.bankConflicts);
    }
    const ScratchpadSchedule result = {
        currentCycle,
        cycle,
        cycle - currentCycle,
        0,
        conflicts,
    };
    recordServiceCycles(write, result.serviceCycles);
    return result;
}

ScratchpadSchedule ScratchpadTimingModel::scheduleDMA(
    std::uint64_t currentCycle,
    std::uint64_t offset,
    std::uint64_t byteCount,
    bool write,
    ScratchpadDMAClient client,
    bool chargeSetup)
{
    ++statistics_.dmaTransfers;
    statistics_.dmaBytes = checkedAdd(statistics_.dmaBytes, byteCount);
    const std::size_t clientIndex = static_cast<std::size_t>(client);
    if (clientIndex >= dmaAvailableCycles_.size()) {
        throw std::invalid_argument("invalid scratchpad DMA client");
    }
    const std::uint64_t engineReady =
        std::max(currentCycle, dmaAvailableCycles_[clientIndex]);
    ScratchpadSchedule result = schedule(
        engineReady,
        offset,
        byteCount,
        write,
        configuration_.dmaBytesPerCycle,
        chargeSetup ? configuration_.dmaSetupCycles : 0,
        static_cast<int>(client));
    dmaAvailableCycles_[clientIndex] = result.completionCycle;
    recordServiceCycles(write, result.serviceCycles);
    result.queueCycles = result.startCycle - currentCycle;
    statistics_.queueCycles = checkedAdd(
        statistics_.queueCycles, result.queueCycles);
    return result;
}

ScratchpadSchedule ScratchpadTimingModel::scheduleInstructionFetch(
    std::uint64_t currentCycle, std::uint64_t offset, std::uint64_t byteCount)
{
    const ScratchpadSchedule result = schedule(
        currentCycle, offset, byteCount, false,
        configuration_.accessWidthBits / 8, 0,
        static_cast<int>(ScratchpadDMAClient::InstructionFetch));
    recordServiceCycles(false, result.serviceCycles);
    return result;
}

void ScratchpadTimingModel::recordServiceCycles(
    bool write,
    std::uint64_t serviceCycles)
{
    std::uint64_t& total = write
        ? statistics_.writeServiceCycles
        : statistics_.readServiceCycles;
    total = checkedAdd(total, serviceCycles);
}

void ScratchpadTimingModel::discardCompletedBeatCounts(
    std::uint64_t currentCycle)
{
    readBeatsByCycle_.erase(
        readBeatsByCycle_.begin(), readBeatsByCycle_.lower_bound(currentCycle));
    writeBeatsByCycle_.erase(
        writeBeatsByCycle_.begin(), writeBeatsByCycle_.lower_bound(currentCycle));
}

void ScratchpadTimingModel::recordBeat(
    bool write,
    std::uint64_t selectedCycle)
{
    std::uint64_t& requests = write
        ? statistics_.writeRequests
        : statistics_.readRequests;
    requests = checkedAdd(requests, UINT64_C(1));
    auto& counts = write ? writeBeatsByCycle_ : readBeatsByCycle_;
    const std::uint32_t serviced = ++counts[selectedCycle];
    std::uint32_t& maximum = write
        ? statistics_.maxWritesServicedSameCycle
        : statistics_.maxReadsServicedSameCycle;
    maximum = std::max(maximum, serviced);
}

ScratchpadSchedule ScratchpadTimingModel::schedule(
    std::uint64_t currentCycle,
    std::uint64_t offset,
    std::uint64_t byteCount,
    bool write,
    std::uint32_t beatBytes,
    std::uint64_t setupCycles,
    int client)
{
    if (byteCount == 0 ||
        !AddressRegion{0, configuration_.capacityBytes}.containsRange(offset, byteCount)) {
        throw std::out_of_range("scratchpad access is outside capacity");
    }

    const std::uint64_t firstCycle = checkedAdd(
        currentCycle, setupCycles);
    discardCompletedBeatCounts(currentCycle);
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
        if (observer_) {
            observer_({client, write, address,
                static_cast<std::uint32_t>(std::min<std::uint64_t>(beatBytes, byteCount - processed)),
                bank, static_cast<std::uint32_t>(port - ports.begin()), issueCycle,
                selectedCycle, checkedAdd(selectedCycle, configuration_.latencyCycles)});
        }
        if (selectedCycle > issueCycle) {
            ++conflicts;
        }
        recordBeat(write, selectedCycle);
        *port = checkedAdd(selectedCycle, 1);
        completionCycle = std::max(
            completionCycle,
            checkedAdd(selectedCycle, configuration_.latencyCycles));
        issueCycle = checkedAdd(issueCycle, 1);
    }

    const std::uint64_t serviceCycles = completionCycle - currentCycle;
    statistics_.bankConflicts = checkedAdd(
        statistics_.bankConflicts, conflicts);
    std::uint64_t& directionalConflicts = write
        ? statistics_.writeBankConflicts
        : statistics_.readBankConflicts;
    directionalConflicts = checkedAdd(directionalConflicts, conflicts);
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
