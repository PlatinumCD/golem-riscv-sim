#ifndef SST_MITTENS_RECEIVE_DMA_ENGINE_H
#define SST_MITTENS_RECEIVE_DMA_ENGINE_H

#include <cstdint>

namespace SST {
namespace Mittens {

struct ReceiveDMASchedule {
    std::uint64_t startCycle;
    std::uint64_t completionCycle;
    std::uint64_t serviceCycles;
};

class ReceiveDMAEngine final
{
  public:
    ReceiveDMAEngine(
        std::uint32_t widthBits,
        std::uint64_t setupCycles);

    ReceiveDMASchedule schedule(
        std::uint64_t currentCycle,
        std::uint32_t wordCount,
        bool chargeSetup);

    std::uint32_t widthBits() const noexcept { return widthBits_; }
    std::uint64_t setupCycles() const noexcept { return setupCycles_; }
    std::uint64_t nextAvailableCycle() const noexcept
    {
        return nextAvailableCycle_;
    }

    static std::uint64_t transferCycles(
        std::uint32_t wordCount,
        std::uint32_t widthBits);

  private:
    std::uint32_t widthBits_;
    std::uint64_t setupCycles_;
    std::uint64_t nextAvailableCycle_ = 0;
};

} // namespace Mittens
} // namespace SST

#endif
