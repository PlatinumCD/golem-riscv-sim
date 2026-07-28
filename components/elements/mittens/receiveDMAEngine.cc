#include "receiveDMAEngine.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace SST {
namespace Mittens {

ReceiveDMAEngine::ReceiveDMAEngine(
    std::uint32_t widthBits,
    std::uint64_t setupCycles) :
    widthBits_(widthBits),
    setupCycles_(setupCycles)
{
    if (widthBits_ == 0 || widthBits_ % 32 != 0) {
        throw std::invalid_argument(
            "RX DMA width must be a positive multiple of 32 bits");
    }
}

std::uint64_t ReceiveDMAEngine::transferCycles(
    std::uint32_t wordCount,
    std::uint32_t widthBits)
{
    if (wordCount == 0) {
        throw std::invalid_argument(
            "RX DMA transfer must contain at least one word");
    }
    if (widthBits == 0 || widthBits % 32 != 0) {
        throw std::invalid_argument(
            "RX DMA width must be a positive multiple of 32 bits");
    }

    const std::uint64_t wordsPerCycle = widthBits / 32;
    return (
        static_cast<std::uint64_t>(wordCount) +
        wordsPerCycle - 1) / wordsPerCycle;
}

ReceiveDMASchedule ReceiveDMAEngine::schedule(
    std::uint64_t currentCycle,
    std::uint32_t wordCount,
    bool chargeSetup)
{
    const std::uint64_t transfer =
        transferCycles(wordCount, widthBits_);
    const std::uint64_t setup = chargeSetup ? setupCycles_ : 0;
    if (setup >
        std::numeric_limits<std::uint64_t>::max() - transfer) {
        throw std::overflow_error(
            "RX DMA service time overflow");
    }
    const std::uint64_t service = setup + transfer;
    const std::uint64_t start =
        std::max(currentCycle, nextAvailableCycle_);
    if (start >
        std::numeric_limits<std::uint64_t>::max() - service) {
        throw std::overflow_error(
            "RX DMA completion time overflow");
    }

    nextAvailableCycle_ = start + service;
    return ReceiveDMASchedule{
        start,
        nextAvailableCycle_,
        service,
    };
}

} // namespace Mittens
} // namespace SST
