#pragma once
#include "../execution/deviceSupport.h"
#include "barrierProtocol.h"
namespace SST::Mittens
{
class InitializationBarrierClient final
{
  public:
    struct Host
    {
        std::function<bool()> running, globalDMADrained;
        std::function<std::optional<QemuSyncEvent>()> pending;
        std::function<void()> completeWait, sendInitialization, wakeInitialization, wakeEpoch;
        std::function<void(std::uint32_t, EpochBarrierContribution)> sendEpoch;
    };
    struct Snapshot
    {
        bool memoryInitializationDelayScheduled_ = false;
        bool memoryInitializationPhase_ = false;
        std::uint64_t memoryInitializationHandshakes_ = 0;
        std::uint64_t memoryInitializationAccesses_ = 0;
        std::uint64_t memoryInitializationReadBytes_ = 0;
        std::uint64_t memoryInitializationWriteBytes_ = 0;
        std::uint64_t memoryInitializationCycles_ = 0;
        bool memoryInitializationBarrierArrived_ = false;
        bool memoryInitializationBarrierReleaseReady_ = false;
        std::uint32_t expectedEpochBarrier_ = 0;
        bool epochBarrierArrivalSent_ = false;
        bool epochBarrierReleaseReady_ = false;
        std::uint64_t epochBarrierArrivals_ = 0;
        std::uint64_t epochBarrierReleases_ = 0;
    };
    InitializationBarrierClient(TileConfiguration config, DeviceDiagnostics diagnostics, Host host)
        : config_(std::move(config)), output_(std::move(diagnostics)), host_(std::move(host))
    {
    }
    void start() noexcept
    {
        state_.memoryInitializationPhase_ = config_.memoryInitializationBatching;
    }
    CpuDeviceResult executeCpuBarrier(const CpuBarrierAction& action);
    CpuDeviceResult executeCpuInitialization(const CpuInitializationAction& action);
    void onInitializationRelease(std::uint32_t tile, MemoryInitializationBarrierMessage message);
    // true requests deployment prefix teardown AFTER the owner commits release.
    bool onEpochRelease(std::uint32_t tile, std::uint32_t epoch, EpochBarrierMessage message,
                        EpochBarrierContribution contribution);
    void validateExit() const;
    Snapshot snapshot() const noexcept
    {
        return state_;
    }

  private:
    static std::uint64_t divideRoundUp(std::uint64_t n, std::uint64_t d)
    {
        return Timing::ceilDivide(n, d);
    }
    TileConfiguration config_;
    DeviceDiagnostics output_;
    Host host_;
    Snapshot state_;
};
} // namespace SST::Mittens
