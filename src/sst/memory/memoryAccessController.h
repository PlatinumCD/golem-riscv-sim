#pragma once
#include "../execution/deviceSupport.h"
#include "scratchpad/scratchpadTimingModel.h"
#include "instructionCache.h"
#include "../profiling/performanceProfile.h"

namespace SST::Mittens
{
class MemoryAccessController final
{
  public:
    struct Context
    {
        std::uint32_t taskId = UINT32_MAX;
        std::uint64_t executionId = 0;
        std::string phase = "runtime";
    };
    struct Host
    {
        std::function<Timing::Ticks()> now;
    };
    struct Statistics
    {
        std::uint32_t outstandingMemoryWrites_ = 0;
        std::uint32_t outstandingMemoryReads_ = 0;
        std::uint64_t maximumOutstandingMemoryRequests_ = 0;
        std::uint64_t maximumOutstandingMemoryReads_ = 0;
        std::uint64_t maximumStoreBufferOccupancy_ = 0;
        std::uint64_t memoryStoreBufferFullEvents_ = 0;
        std::uint64_t vectorMemoryRequestGroups_ = 0;
        std::uint64_t vectorMemoryGroupRequests_ = 0;
        std::uint64_t scalarMemoryRequestGroups_ = 0;
        std::uint64_t scalarMemoryGroupRequests_ = 0;

        std::uint64_t memoryRequests_ = 0;
        std::uint64_t memoryResponses_ = 0;
        std::uint64_t memoryReads_ = 0;
        std::uint64_t memoryWrites_ = 0;
    };
    MemoryAccessController(TileConfiguration config, Timing::Clock<Timing::Cpu> clock,
                           std::unique_ptr<ScratchpadTimingModel> scratchpad,
                           PerformanceProfile& profile, DeviceDiagnostics diagnostics, Host host)
        : config_(std::move(config)), clock_(clock), scratchpadTimingModel_(std::move(scratchpad)),
          performanceProfile_(profile), output_(std::move(diagnostics)), host_(std::move(host))
    {
        if (config_.scratchpadBoot)
        {
            if (!scratchpadTimingModel_)
                throw std::invalid_argument("instruction cache requires scratchpad");
            instructionCache_ = std::make_unique<InstructionCache>(
                MITTENS_SCRATCHPAD_BASE,
                InstructionCacheConfiguration{config_.instructionCacheBytes,
                    config_.instructionCacheLineBytes, config_.instructionCacheWays,
                    config_.instructionCacheHitCycles}, *scratchpadTimingModel_);
        }
    }
    CpuDeviceResult executeCpuMemory(const CpuMemoryAction& action);
    CpuDeviceResult executeInstruction(const CpuInstructionAction& action);
    InstructionCacheStatistics instructionStatistics() const noexcept
    {
        return instructionCache_ ? instructionCache_->statistics() : InstructionCacheStatistics{};
    }
    std::uint64_t instructionStallCycles() const noexcept { return instructionStallCycles_; }
    std::uint64_t instructionInvalidations() const noexcept { return instructionInvalidations_; }
    Statistics statistics() const noexcept
    {
        return statistics_;
    }
    std::size_t pendingCount() const noexcept
    {
        return scratchpadAccessDeadline_ ? 1 : 0;
    }
    bool storesDrained() const noexcept
    {
        return !scratchpadAccessDeadline_;
    }
    bool scratchpadAvailable() const noexcept
    {
        return bool(scratchpadTimingModel_);
    }
    ScratchpadTimingStatistics scratchpadStatistics() const noexcept
    {
        return scratchpadTimingModel_ ? scratchpadTimingModel_->statistics()
                                      : ScratchpadTimingStatistics{};
    }
    ScratchpadSchedule reserveCPU(const MittensSyncMemoryAccess& access, std::uint64_t cursor);
    ScratchpadSchedule reserveDMA(std::uint64_t cursor, std::uint64_t offset, std::uint64_t bytes,
                                  bool write)
    {
        if (!scratchpadTimingModel_)
            throw std::logic_error("SPM is disabled");
        return scratchpadTimingModel_->scheduleDMA(cursor, offset, bytes, write);
    }

  private:
    Timing::Clock<Timing::Cpu> cpuDomain() const
    {
        return clock_;
    }
    AddressRegion scratchpadRegion() const noexcept
    {
        return {MITTENS_SCRATCHPAD_BASE, config_.scratchpadBytes};
    }
    TileConfiguration config_;
    Timing::Clock<Timing::Cpu> clock_;
    std::unique_ptr<ScratchpadTimingModel> scratchpadTimingModel_;
    std::unique_ptr<InstructionCache> instructionCache_;
    std::uint64_t instructionStallCycles_ = 0;
    std::uint64_t instructionInvalidations_ = 0;
    PerformanceProfile& performanceProfile_;
    DeviceDiagnostics output_;
    Host host_;
    Statistics statistics_;
    struct ScratchpadAccessDeadline
    {
        std::uint64_t step;
        Timing::Ticks tick;
    };
    std::optional<ScratchpadAccessDeadline> scratchpadAccessDeadline_;
    std::optional<ScratchpadAccessDeadline> instructionDeadline_;

};
} // namespace SST::Mittens
