#pragma once
#include "../execution/deviceSupport.h"
#include "scratchpad/scratchpadTimingModel.h"
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
    // A prepared transport request is RAII-owned until send transfers it.
    // prepare/send must not synchronously deliver a response.
    struct PreparedRequest
    {
        virtual ~PreparedRequest() = default;
        virtual std::uint64_t id() const noexcept = 0;
        virtual void send() = 0;
    };
    struct Host
    {
        std::function<Timing::Ticks()> now;
        std::function<Context()> context;
        std::function<std::unique_ptr<PreparedRequest>(std::uint64_t, std::uint32_t, bool)> prepare;
        std::function<std::optional<QemuSyncEvent>()> pending;
        std::function<bool()> hasMemoryReplay;
        std::function<void(std::uint64_t, bool)> completeMemory;
        std::function<bool()> retry;
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
    }
    CpuDeviceResult executeCpuMemory(const CpuMemoryAction& action);
    void onResponse(std::uint64_t requestId);
    Statistics statistics() const noexcept
    {
        return statistics_;
    }
    std::size_t pendingCount() const noexcept
    {
        return pendingMemoryRequests_.size();
    }
    bool storesDrained() const noexcept
    {
        return statistics_.outstandingMemoryWrites_ == 0;
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
    bool memoryReadHasPendingWriteHazard(std::uint64_t address, std::uint32_t size) const noexcept;
    std::uint64_t sendMemoryRequest(const CpuMemoryAction&);
    bool issueMemoryRequest(const CpuMemoryAction&);
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
    PerformanceProfile& performanceProfile_;
    DeviceDiagnostics output_;
    Host host_;
    Statistics statistics_;
    struct PendingMemoryRequest
    {
        std::uint64_t cpuStep = 0;
        std::uint64_t issueTick = 0;
        std::uint64_t address = 0;
        std::uint64_t timingAddress = 0;
        std::uint64_t programCounter = 0;
        std::uint64_t returnAddress = 0;
        std::uint32_t size = 0;
        bool write = false;
        std::uint32_t taskId = UINT32_MAX;
        std::uint64_t executionId = 0;
        std::string phase = "idle";
    };

    struct ScratchpadAccessDeadline
    {
        std::uint64_t step;
        Timing::Ticks tick;
    };
    std::optional<ScratchpadAccessDeadline> scratchpadAccessDeadline_;
    std::unordered_map<std::uint64_t, PendingMemoryRequest> pendingMemoryRequests_;
    std::optional<std::uint64_t> blockingMemoryRequestId_;
    std::uint64_t blockingMemoryStep_ = 0;

    std::unordered_set<std::uint64_t> cpuMemoryGroupRequestIds_;
};
} // namespace SST::Mittens
