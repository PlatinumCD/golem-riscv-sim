#include "../memory/instructionCache.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

using namespace SST::Mittens;

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "instruction cache test: %s\n", message);
        std::exit(1);
    }
}
} // namespace

int main()
{
    ScratchpadTimingModel timing({4096, 2, 1, 1, 32, 1, 8, 32});
    InstructionCache cache(0x1000, {}, timing);
    const auto first = cache.fetch(0x1000, 4, 0);
    try
    {
        (void)cache.fetch(0x1004, 4, 0);
        require(false, "blocking cache accepted an overlapping fetch");
    }
    catch (const std::logic_error&)
    {
    }
    const auto second = cache.fetch(0x1004, 4, first.readyCycle);
    require(!first.hit && first.fillBytes == 64 && second.hit,
            "cold miss or post-ready hit result was incorrect");
    require(timing.statistics().dmaTransfers == 0 && timing.statistics().readRequests != 0,
            "instruction fill changed DMA counters or made no read request");
    const auto hit = cache.fetch(0x1008, 4, second.readyCycle);
    require(hit.hit && hit.readyCycle == second.readyCycle + 1 && hit.fillBytes == 0,
            "cache hit latency or fill-byte result was incorrect");
    const auto boundary = cache.fetch(0x103e, 4, hit.readyCycle);
    require(!boundary.hit && boundary.fillBytes == 64,
            "line-boundary fetch did not fill exactly one new line");

    ScratchpadTimingModel evictionTiming({4096, 2, 1, 1, 32, 1, 8, 32});
    InstructionCache eviction(0x1000, {256, 64, 2, 1}, evictionTiming);
    const auto e0 = eviction.fetch(0x1000, 4, 0);
    const auto e1 = eviction.fetch(0x1200, 4, e0.readyCycle);
    const auto e2 = eviction.fetch(0x1400, 4, e1.readyCycle);
    const auto evicted = eviction.fetch(0x1000, 4, e2.readyCycle);
    require(!evicted.hit, "LRU line was not evicted");
    eviction.invalidate();
    require(!eviction.fetch(0x1000, 4, evicted.readyCycle).hit,
            "invalidate did not clear instruction cache");

    try
    {
        (void)cache.fetch(0x1ffe, 4, 0);
        require(false, "out-of-SPM fetch was accepted");
    }
    catch (const std::out_of_range&)
    {
    }
    try
    {
        (void)cache.fetch(0x1001, 4, boundary.readyCycle);
        require(false, "odd-PC fetch was accepted");
    }
    catch (const std::invalid_argument&)
    {
    }
    try
    {
        InstructionCache bad(0, {8192, 48, 2, 1}, timing);
        (void)bad;
        require(false, "invalid line geometry was accepted");
    }
    catch (const std::invalid_argument&)
    {
    }
    try
    {
        InstructionCache bad(1, {}, timing);
        (void)bad;
        require(false, "misaligned SPM base was accepted");
    }
    catch (const std::invalid_argument&)
    {
    }
    try
    {
        ScratchpadTimingModel small({32, 1, 1, 1, 32, 1, 8, 32});
        InstructionCache bad(0, {}, small);
        (void)bad;
        require(false, "cache line larger than SPM was accepted");
    }
    catch (const std::invalid_argument&)
    {
    }
    try
    {
        ScratchpadTimingModel nonmultiple({96, 1, 1, 1, 32, 1, 8, 32});
        InstructionCache bad(0, {}, nonmultiple);
        (void)bad;
        require(false, "nonmultiple SPM size was accepted");
    }
    catch (const std::invalid_argument&)
    {
    }

    ScratchpadTimingModel referenceTiming({4096, 2, 1, 1, 32, 1, 8, 32});
    ScratchpadTimingModel contendedTiming({4096, 2, 1, 1, 32, 1, 8, 32});
    InstructionCache reference(0x1000, {}, referenceTiming);
    InstructionCache contended(0x1000, {}, contendedTiming);
    std::vector<ScratchpadBeatObservation> observations;
    contendedTiming.setObserver([&observations](const ScratchpadBeatObservation& observation)
                                { observations.push_back(observation); });
    const auto referenceMiss = reference.fetch(0x1000, 4, 0);
    (void)contendedTiming.scheduleDMA(16, 4, 4, false, ScratchpadDMAClient::GlobalRAM, false);
    const auto dmaTransfersBefore = contendedTiming.statistics().dmaTransfers;
    const auto dmaBytesBefore = contendedTiming.statistics().dmaBytes;
    const auto contendedMiss = contended.fetch(0x1000, 4, 0);
    require(contendedMiss.readyCycle > referenceMiss.readyCycle,
            "bank contention did not delay instruction fill");
    require(contendedTiming.statistics().dmaTransfers == dmaTransfersBefore &&
                contendedTiming.statistics().dmaBytes == dmaBytesBefore,
            "instruction fill changed DMA counters");
    const auto instructionObservation = std::find_if(
        observations.begin(), observations.end(), [](const auto& observation)
        { return observation.client == static_cast<int>(ScratchpadDMAClient::InstructionFetch); });
    require(instructionObservation != observations.end() && instructionObservation->bank == 0,
            "instruction fill observer attribution or bank was incorrect");

    std::puts("instruction cache test: PASS");
}
