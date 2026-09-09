#pragma once
#include "../execution/deviceSupport.h"
#include "globalDMAProtocol.h"
#include "scratchpad/scratchpadTimingModel.h"
namespace SST::Mittens
{
class GlobalDMAClient final
{
  public:
    struct Host
    {
        std::function<Timing::Ticks()> now;
        std::function<bool()> scratchpadAvailable;
        std::function<ScratchpadSchedule(std::uint64_t, std::uint64_t, std::uint64_t, bool)>
            reserveDMA;
        std::function<void(GlobalDMAMessage)> send;
        std::function<std::optional<QemuSyncEvent>()> pending;
        std::function<void()> wake;
    };
    struct Statistics
    {
        std::uint64_t submitted, completed, pending;
    };
    GlobalDMAClient(TileConfiguration config, Timing::Clock<Timing::Cpu> clock,
                    DeviceDiagnostics diagnostics, Host host)
        : config_(std::move(config)), clock_(clock), output_(std::move(diagnostics)),
          host_(std::move(host))
    {
    }
    CpuDeviceResult executeCpuGlobalDMA(const CpuGlobalDMAAction& action);
    void onCompletion(const GlobalDMAMessage& message);
    bool drained() const noexcept
    {
        return globalDMACompletions_.empty();
    }
    Statistics statistics() const noexcept
    {
        std::uint64_t pending = 0;
        for (const auto& execution : globalDMACompletions_)
            pending += execution.second.size();
        return {physicalGlobalDMASubmitted_, physicalGlobalDMACompleted_, pending};
    }

  private:
    Timing::Clock<Timing::Cpu> cpuDomain() const
    {
        return clock_;
    }
    TileConfiguration config_;
    Timing::Clock<Timing::Cpu> clock_;
    DeviceDiagnostics output_;
    Host host_;
    struct GlobalDMACompletion
    {
        std::uint64_t logicalIteration;
        std::uint64_t localCompletionTick;
        std::uint64_t scratchpadCompletionCycle;
        std::uint32_t requestFlags;
        bool controllerComplete;
    };

    std::unordered_map<std::uint64_t, std::unordered_map<std::uint32_t, GlobalDMACompletion>>
        globalDMACompletions_;
    std::uint64_t physicalGlobalDMASubmitted_ = 0;
    std::uint64_t physicalGlobalDMACompleted_ = 0;
    bool scratchpadWaitDelayScheduled_ = false;
};
} // namespace SST::Mittens
