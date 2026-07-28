#include "receiveDMAEngine.h"

#include <cassert>
#include <stdexcept>

using SST::Mittens::ReceiveDMAEngine;
using SST::Mittens::ReceiveDMASchedule;

int main()
{
    ReceiveDMAEngine first(256, 8);
    assert(ReceiveDMAEngine::transferCycles(1, 256) == 1);
    assert(ReceiveDMAEngine::transferCycles(8, 256) == 1);
    assert(ReceiveDMAEngine::transferCycles(9, 256) == 2);
    assert(ReceiveDMAEngine::transferCycles(64, 256) == 8);

    const ReceiveDMASchedule firstTransfer =
        first.schedule(100, 64, true);
    assert(firstTransfer.startCycle == 100);
    assert(firstTransfer.serviceCycles == 16);
    assert(firstTransfer.completionCycle == 116);

    const ReceiveDMASchedule serialized =
        first.schedule(105, 32, true);
    assert(serialized.startCycle == 116);
    assert(serialized.serviceCycles == 12);
    assert(serialized.completionCycle == 128);

    const ReceiveDMASchedule sameDescriptor =
        first.schedule(120, 16, false);
    assert(sameDescriptor.startCycle == 128);
    assert(sameDescriptor.serviceCycles == 2);
    assert(sameDescriptor.completionCycle == 130);

    ReceiveDMAEngine second(256, 8);
    const ReceiveDMASchedule overlapping =
        second.schedule(105, 32, true);
    assert(overlapping.startCycle == 105);
    assert(overlapping.completionCycle == 117);

    bool rejected = false;
    try {
        ReceiveDMAEngine invalid(48, 0);
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    return 0;
}
