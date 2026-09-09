#pragma once
#include "../execution/deviceSupport.h"
#include "analogDevice.h"
#include "../bridge/sharedAnalogMemoryBridge.h"
#include "../profiling/performanceProfile.h"
namespace SST::Mittens
{
class AnalogController final
{
  public:
    struct Host
    {
        std::function<Timing::Ticks()> now;
        std::function<bool()> active, hasAnalogReplay;
        std::function<std::optional<QemuSyncEvent>()> pending;
        std::function<std::uint64_t()> pendingStepId;
        std::function<void(std::uint64_t)> completeDevice;
        std::function<void(Timing::Cycles<Timing::Analog>, std::uint64_t)> scheduleWake;
    };
    struct Statistics
    {
        std::array<std::uint64_t, MITTENS_ANALOG_OPERATION_MOVE_VECTOR + 1> operations;
        std::uint64_t inputWords, outputWords, submitted, completed, elapsed, linkBeats,
            maximumOutstanding;
    };
    AnalogController(TileConfiguration config, Timing::Clock<Timing::Analog> clock,
                     PerformanceProfile& profile, DeviceDiagnostics diagnostics, Host host);
    bool enabled() const noexcept
    {
        return bool(analogDevice_);
    }
    void setup()
    {
        if (enabled())
            analogBridge_.create(config_.tileId, config_.analogArrayCount, config_.analogArrayRows,
                                 config_.analogArrayColumns);
    }
    // Caller must stop/join every capturer before closing any bridge mapping.
    void close()
    {
        analogBridge_.close();
    }
    int fileDescriptor() const noexcept
    {
        return analogBridge_.fileDescriptor();
    }
    std::uint32_t arrayCount() const noexcept
    {
        return analogBridge_.arrayCount();
    }
    bool submitted(std::uint32_t array, std::uint64_t sequence) const
    {
        return sequence <= UINT32_MAX &&
               analogBridge_.slotState({array, static_cast<std::uint32_t>(sequence)}) ==
                   MITTENS_ANALOG_SLOT_SUBMITTED;
    }
    bool hasDeferred() const noexcept
    {
        return deferredAnalogSubmission_.has_value();
    }
    bool deferredMatches(const QemuSyncEvent& e) const
    {
        return e.stopReason == MITTENS_SYNC_STOP_ANALOG_SUBMIT &&
               e.flags == MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH && deferredAnalogSubmission_ &&
               deferredAnalogSubmission_->token.arrayId == e.analogArrayId &&
               deferredAnalogSubmission_->token.sequence == e.analogSequence &&
               deferredAnalogSubmission_->command.operation == MITTENS_ANALOG_OPERATION_LOAD_VECTOR;
    }
    bool startDeferredAnalogLoadLookahead(const QemuSyncEvent& event);
    void validateInitialCpuDevice(const QemuSyncEvent& event) const;
    CpuDeviceResult executeCpuAnalog(const CpuAnalogAction& action);
    void serviceAnalogBridge(bool permitDeferredSubmission = false);
    void flushTrace()
    {
        serviceAnalogTrace();
    }
    void onWake(std::uint64_t generation);
    Statistics statistics() const noexcept
    {
        return {analogOperationCounts_,
                analogInputWords_,
                analogOutputWords_,
                analogDevice_ ? analogDevice_->submittedCommandCount() : 0,
                analogDevice_ ? analogDevice_->completedCommandCount() : 0,
                analogDevice_ ? analogDevice_->elapsedCycles() : 0,
                analogDevice_ ? analogDevice_->linkBeats() : 0,
                analogDevice_ ? analogDevice_->maximumOutstandingCommandCount() : 0};
    }

  private:
    bool pendingAnalogEventReady() const;
    void updateAnalogDeviceToCurrentCycle();
    void scheduleAnalogWake();
    void serviceAnalogSubmission(AnalogBridgeSubmission submission, bool publishAcceptance = true);
    void serviceAnalogTrace();
    void serviceAnalogCompletions();
    void checkAnalogBridgeError() const;
    Timing::Clock<Timing::Analog> analogDomain() const
    {
        return clock_;
    }
    TileConfiguration config_;
    Timing::Clock<Timing::Analog> clock_;
    PerformanceProfile& performanceProfile_;
    DeviceDiagnostics output_;
    Host host_;
    SharedAnalogMemoryBridge analogBridge_;
    std::unique_ptr<AnalogDevice> analogDevice_;
    std::unordered_map<std::uint64_t, AnalogBridgeToken> analogRequests_;
    std::optional<std::uint64_t> analogWakeTick_;
    std::uint64_t analogLastUpdateCycle_ = 0;
    std::uint64_t analogWakeGeneration_ = 0;
    bool analogWakeServicing_ = false;
    std::optional<AnalogBridgeSubmission> deferredAnalogSubmission_;
    std::array<std::uint64_t, MITTENS_ANALOG_OPERATION_MOVE_VECTOR + 1> analogOperationCounts_{};
    std::uint64_t analogInputWords_ = 0;
    std::uint64_t analogOutputWords_ = 0;
};
} // namespace SST::Mittens
