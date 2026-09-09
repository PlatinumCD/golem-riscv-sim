#include "../memory/scratchpad/scratchpadTimingModel.h"

#include <cstdio>
#include <stdexcept>

using namespace SST::Mittens;

int main()
{
    ScratchpadTimingModel model({256, 2, 1, 1, 32, 1, 2, 4});
    const ScratchpadSchedule first = model.scheduleCPU(0, 0, 4, false);
    const ScratchpadSchedule conflict = model.scheduleCPU(0, 0, 4, false);
    const ScratchpadSchedule otherBank = model.scheduleCPU(0, 4, 4, false);
    const ScratchpadSchedule dma = model.scheduleDMA(1, 0, 16, true);

    if (first.completionCycle != 1 ||
        conflict.completionCycle != 2 ||
        conflict.bankConflicts != 1 ||
        otherBank.completionCycle != 1 ||
        dma.serviceCycles != 6 ||
        model.statistics().cpuRequests != 3 ||
        model.statistics().dmaTransfers != 1 ||
        model.statistics().dmaBytes != 16) {
        std::fputs("scratchpad timing model test: FAIL\n", stderr);
        return 1;
    }

    ScratchpadTimingModel sharedClients({256, 2, 1, 1, 32, 1, 2, 4});
    (void)sharedClients.scheduleCPU(0, 0, 4, false);
    const ScratchpadSchedule txConflict = sharedClients.scheduleDMA(
        0, 0, 4, false, ScratchpadDMAClient::NetworkTransmit, false);
    const ScratchpadSchedule rxOtherBank = sharedClients.scheduleDMA(
        0, 4, 4, true, ScratchpadDMAClient::NetworkReceive, false);
    if (txConflict.completionCycle != 2 ||
        txConflict.bankConflicts != 1 ||
        rxOtherBank.completionCycle != 1 ||
        sharedClients.statistics().dmaTransfers != 2) {
        std::fputs("scratchpad DMA client arbitration: FAIL\n", stderr);
        return 1;
    }

    ScratchpadTimingModel scalarReplay({
        512, 8, 1, 1, 256, 3, 8, 32});
    ScratchpadTimingModel compactReplay({
        512, 8, 1, 1, 256, 3, 8, 32});
    std::uint64_t scalarCycle = 7;
    for (std::uint32_t index = 0; index < 32; ++index) {
        scalarCycle = scalarReplay.scheduleCPU(
            scalarCycle, index * 8, 8, false).completionCycle;
    }
    const ScratchpadSchedule compact =
        compactReplay.scheduleCPUContiguousRun(
            7, 0, 8, 32, false);
    const ScratchpadSchedule scalarFollow =
        scalarReplay.scheduleCPU(scalarCycle, 256, 8, false);
    const ScratchpadSchedule compactFollow =
        compactReplay.scheduleCPU(
            compact.completionCycle, 256, 8, false);
    if (compact.completionCycle != scalarCycle ||
        compact.serviceCycles != scalarCycle - 7 ||
        compact.bankConflicts != 0 ||
        compactFollow.completionCycle != scalarFollow.completionCycle ||
        compactReplay.statistics().cpuRequests !=
            scalarReplay.statistics().cpuRequests ||
        compactReplay.statistics().activeCycles !=
            scalarReplay.statistics().activeCycles ||
        compactReplay.statistics().bankConflicts !=
            scalarReplay.statistics().bankConflicts) {
        std::fputs("scratchpad compact run equivalence: FAIL\n", stderr);
        return 1;
    }

    ScratchpadTimingModel multibeatScalar({
        256, 2, 1, 1, 32, 2, 2, 4});
    ScratchpadTimingModel multibeatCompact({
        256, 2, 1, 1, 32, 2, 2, 4});
    std::uint64_t multibeatCycle = 0;
    for (std::uint32_t index = 0; index < 4; ++index) {
        multibeatCycle = multibeatScalar.scheduleCPU(
            multibeatCycle, index * 8, 8, true).completionCycle;
    }
    const ScratchpadSchedule multibeatRun =
        multibeatCompact.scheduleCPUContiguousRun(
            0, 0, 8, 4, true);
    if (multibeatRun.completionCycle != multibeatCycle ||
        multibeatCompact.statistics().cpuRequests != 4 ||
        multibeatCompact.statistics().activeCycles !=
            multibeatScalar.statistics().activeCycles ||
        multibeatCompact.statistics().bankConflicts !=
            multibeatScalar.statistics().bankConflicts) {
        std::fputs(
            "scratchpad compact multibeat equivalence: FAIL\n", stderr);
        return 1;
    }

    try {
        (void)model.scheduleCPU(0, 255, 4, false);
        std::fputs("scratchpad range check: FAIL\n", stderr);
        return 1;
    } catch (const std::out_of_range&) {
    }

    std::puts("scratchpad timing model test: PASS");
    return 0;
}
