#pragma once
#include "../execution/deviceSupport.h"
#include "barrierProtocol.h"
namespace SST::Mittens
{
class EpochBarrierClient final
{
  public:
    struct Host
    {
        std::function<bool()> running, globalDMADrained;
        std::function<bool()> analogDrained;
        std::function<std::optional<QemuSyncEvent>()> pending;
        std::function<void()> completeWait, wakeEpoch;
        std::function<void(std::uint32_t, EpochBarrierContribution)> sendEpoch;
    };
    struct Snapshot
    {
        std::uint32_t expectedEpochBarrier_ = 0;
        bool epochBarrierArrivalSent_ = false;
        bool epochBarrierReleaseReady_ = false;
        std::uint64_t epochBarrierArrivals_ = 0;
        std::uint64_t epochBarrierReleases_ = 0;
    };
    EpochBarrierClient(TileConfiguration config, DeviceDiagnostics diagnostics, Host host)
        : config_(std::move(config)), output_(std::move(diagnostics)), host_(std::move(host))
    {
    }
    CpuDeviceResult executeCpuBarrier(const CpuBarrierAction& action);
    void onAnalogProgress();
    // true requests deployment prefix teardown AFTER the owner commits release.
    bool onEpochRelease(std::uint32_t tile, std::uint32_t epoch, EpochBarrierMessage message,
                        EpochBarrierContribution contribution);
    void validateExit() const;
    Snapshot snapshot() const noexcept
    {
        return state_;
    }

  private:
    TileConfiguration config_;
    DeviceDiagnostics output_;
    Host host_;
    Snapshot state_;
    bool waitingForAnalog_ = false;
};
} // namespace SST::Mittens
