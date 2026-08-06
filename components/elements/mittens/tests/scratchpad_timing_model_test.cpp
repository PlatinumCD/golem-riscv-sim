#include "scratchpad/scratchpadTimingModel.h"

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

    try {
        (void)model.scheduleCPU(0, 255, 4, false);
        std::fputs("scratchpad range check: FAIL\n", stderr);
        return 1;
    } catch (const std::out_of_range&) {
    }

    std::puts("scratchpad timing model test: PASS");
    return 0;
}
