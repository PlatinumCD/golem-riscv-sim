#include "../memory/scratchpad/scratchpadTimingModel.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <tuple>
#include <vector>

using namespace SST::Mittens;

static auto fields(const ScratchpadBeatObservation& b)
{
    return std::make_tuple(b.client, b.write, b.offset, b.bytes, b.bank,
                           b.port, b.issueCycle, b.serviceCycle, b.completionCycle);
}

static void fixedBankControls()
{
    // Synthetic simultaneous requests test the bank backend, not CPU issue rate.
    for (bool sameBank : {false, true}) {
        ScratchpadTimingModel model({4096, 8, 1, 1, 256, 1, 0, 32});
        std::vector<ScratchpadBeatObservation> beats;
        model.setObserver([&](const auto& b) { beats.push_back(b); });
        for (unsigned i = 0; i < 8; ++i) {
            model.scheduleCPU(10, i * (sameBank ? 256 : 32), 32, false);
        }
        assert(beats.size() == 8);
        for (unsigned i = 0; i < 8; ++i) {
            assert(beats[i].bank == (sameBank ? 0 : i));
            assert(beats[i].serviceCycle == 10 + (sameBank ? i : 0));
        }
        assert(model.statistics().maxReadsServicedSameCycle == (sameBank ? 1 : 8));
        // Conflict counters count delayed requests, not accumulated wait cycles.
        assert(model.statistics().readBankConflicts == (sameBank ? 7 : 0));
        std::uint64_t waitSum = 0;
        for (const auto& b : beats) waitSum += b.serviceCycle - b.issueCycle;
        assert(waitSum == (sameBank ? 28 : 0));
        const auto span = beats.back().completionCycle - 10;
        assert(256 / span == (sameBank ? 32 : 256));
    }

    // Actual existing client identities share bank arbitration; read and write
    // ports remain independent even when RX targets the CPU reader's bank.
    for (unsigned rxBank : {0U, 2U}) {
        ScratchpadTimingModel model({4096, 8, 1, 1, 256, 1, 0, 32});
        std::vector<ScratchpadBeatObservation> beats;
        model.setObserver([&](const auto& b) { beats.push_back(b); });
        model.scheduleCPU(20, 0, 32, false);
        model.scheduleDMA(20, 32, 32, false, ScratchpadDMAClient::NetworkTransmit, false);
        model.scheduleDMA(20, rxBank * 32, 32, true, ScratchpadDMAClient::NetworkReceive, false);
        assert(beats.size() == 3);
        for (const auto& b : beats) assert(b.serviceCycle == 20);
        assert(model.statistics().maxReadsServicedSameCycle == 2);
        assert(model.statistics().maxWritesServicedSameCycle == 1);
        assert(model.statistics().bankConflicts == 0);
    }
    ScratchpadTimingModel conflict({4096, 8, 1, 1, 256, 1, 0, 32});
    conflict.scheduleCPU(0, 0, 32, false);
    const auto tx = conflict.scheduleDMA(0, 256, 32, false,
                                         ScratchpadDMAClient::NetworkTransmit, false);
    assert(tx.completionCycle == 2 && tx.bankConflicts == 1);
}

static void observationDoesNotChangeScheduling()
{
    ScratchpadTimingModel plain({4096, 8, 1, 1, 256, 3, 8, 32});
    ScratchpadTimingModel observed({4096, 8, 1, 1, 256, 3, 8, 32});
    std::vector<ScratchpadBeatObservation> beats;
    observed.setObserver([&](const auto& b) { beats.push_back(b); });
    const auto exercise = [](auto& model) {
        std::vector<ScratchpadSchedule> results;
        results.push_back(model.scheduleCPU(10, 0, 32, false));
        results.push_back(model.scheduleDMA(10, 0, 68, false,
                                           ScratchpadDMAClient::NetworkTransmit, false));
        results.push_back(model.scheduleDMA(10, 0, 32, true,
                                           ScratchpadDMAClient::NetworkReceive, false));
        results.push_back(model.scheduleCPUContiguousRun(100, 0, 8, 32, false));
        results.push_back(model.scheduleCPU(300, 0, 32, false));
        return results;
    };
    const auto a = exercise(plain), b = exercise(observed);
    for (unsigned i = 0; i < a.size(); ++i) {
        assert(std::make_tuple(a[i].startCycle, a[i].completionCycle,
                              a[i].serviceCycles, a[i].queueCycles, a[i].bankConflicts) ==
               std::make_tuple(b[i].startCycle, b[i].completionCycle,
                              b[i].serviceCycles, b[i].queueCycles, b[i].bankConflicts));
    }
    assert(plain.statistics().readRequests == observed.statistics().readRequests);
    assert(plain.statistics().writeRequests == observed.statistics().writeRequests);
    assert(plain.statistics().activeCycles == observed.statistics().activeCycles);
    assert(plain.statistics().bankConflicts == observed.statistics().bankConflicts);
    assert(beats[3].bytes == 4); // Partial final DMA beat, not a fabricated 32 B.
    assert(beats[1].client == 1 && beats[4].client == 5);

    ScratchpadTimingModel scalar({4096, 8, 1, 1, 256, 3, 0, 32});
    ScratchpadTimingModel compact({4096, 8, 1, 1, 256, 3, 0, 32});
    std::vector<ScratchpadBeatObservation> scalarBeats, compactBeats;
    scalar.setObserver([&](const auto& beat) { scalarBeats.push_back(beat); });
    compact.setObserver([&](const auto& beat) { compactBeats.push_back(beat); });
    std::uint64_t cycle = 100;
    for (unsigned i = 0; i < 32; ++i) {
        cycle = scalar.scheduleCPU(cycle, i * 8, 8, false).completionCycle;
    }
    assert(compact.scheduleCPUContiguousRun(100, 0, 8, 32, false).completionCycle == cycle);
    assert(scalarBeats.size() == compactBeats.size());
    for (unsigned i = 0; i < scalarBeats.size(); ++i) {
        assert(fields(scalarBeats[i]) == fields(compactBeats[i]));
    }
}

int main()
{
    for (bool sameBank : {false, true}) {
        ScratchpadTimingModel model({4096, 8, 1, 1, 256, 1, 0, 32});
        std::vector<ScratchpadBeatObservation> beats;
        model.setObserver([&](const auto& b) { beats.push_back(b); });
        for (unsigned lane = 0; lane < 4; ++lane) {
            const auto result = model.scheduleDMA(0, lane * (sameBank ? 256 : 32),
                32, true, static_cast<ScratchpadDMAClient>(5 + lane), false);
            assert(result.completionCycle == (sameBank ? lane + 1 : 1));
        }
        assert(beats.size() == 4);
        assert(model.statistics().maxWritesServicedSameCycle == (sameBank ? 1 : 4));
    }
    fixedBankControls();
    observationDoesNotChangeScheduling();
    std::puts("scratchpad observer and fixed-bank controls: PASS");
}
