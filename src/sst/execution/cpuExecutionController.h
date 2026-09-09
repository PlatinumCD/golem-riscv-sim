#pragma once
#include "cpuExecutionLedger.h"
#include "cpuActions.h"
#include "diagnosticMessage.h"
#include "qemuCaptureCoordinator.h"
#include "../configuration/tileConfiguration.h"
#include "../memory/addressRegion.h"
#include "../memory/scratchpad/scratchpadTimingModel.h"
#include <mittens/MemoryMap.h>
#include <cstdio>
namespace SST::Mittens
{
const char* syncStopReasonName(std::uint32_t reason);

class CpuExecutionController final
{
  public:
    struct Statistics
    {
        std::uint64_t synchronizationGrants_ = 0;
        std::uint64_t synchronizationEvents_ = 0;
        std::array<std::uint64_t, MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO + 1>
            synchronizationStopCounts_{};
        std::uint64_t analogSubmitBatchTransportRecords_ = 0;
        std::uint64_t memoryBatchTransportRecords_ = 0;
        std::uint64_t memoryBatchLogicalAccesses_ = 0;
        std::uint64_t globalDMASubmitBatchTransportRecords_ = 0;
        std::uint64_t globalDMAWaitBatchTransportRecords_ = 0;
        std::uint64_t globalDMAMacroRunTransportRecords_ = 0;
        std::uint64_t taskFinishEvents_ = 0;
        std::array<std::uint64_t, MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO + 1> waitTicks_{};
        std::optional<std::uint64_t> activeWaitStartTick_;
        std::uint32_t activeWaitReason_ = MITTENS_SYNC_STOP_NONE;
        std::uint64_t activeWaitEventSequence_ = 0;
    };

    struct Transport
    {
        std::function<std::uint64_t(std::uint64_t)> grant;
        std::function<void(const QemuSyncEvent&)> resume;
        std::function<std::optional<QemuSyncEvent>()> poll;
        std::function<QemuSyncEvent(const std::atomic<bool>&)> captureHost;
    };
    struct Host
    {
        std::function<bool()> running, initializing, observeExit, watchdogReported;
        std::function<Timing::Ticks()> now;
        std::function<void()> watchdog, terminateAll, scheduleCaptureDispatch, serviceBridge;
        std::function<void(Timing::Cycles<Timing::Cpu>, std::uint32_t, std::uint64_t)> schedule;
        std::function<void(Timing::Cycles<Timing::Cpu>, std::uint64_t)> scheduleWatchdog;
        std::function<void(bool)> progress;
        std::function<void(std::uint64_t, std::uint64_t, const char*, std::uint64_t)> recordWait;
        std::function<void(int, const std::string&)> log;
        std::function<bool()> scratchpadAvailable, storesDrained, receiveReady;
        std::function<bool(bool)> transmitReady;
        std::function<ScratchpadSchedule(const MittensSyncMemoryAccess&, std::uint64_t)> scratchpad;
        std::function<std::uint32_t()> analogArrayCount;
        std::function<bool(std::uint32_t, std::uint64_t)> analogSubmitted;
        std::function<bool(const QemuSyncEvent&)> prepareDeferredAnalog, deferredAnalogMatches;
        std::function<void(const QemuSyncEvent&)> validateInitialDevice;
        std::function<CpuDeviceResult(const CpuMemoryAction&)> memory;
        std::function<CpuDeviceResult(const CpuAnalogAction&)> analog;
        std::function<CpuDeviceResult(const CpuNetworkAction&)> network;
        std::function<CpuDeviceResult(const CpuGlobalDMAAction&)> globalDMA;
        std::function<CpuDeviceResult(const CpuBarrierAction&)> barrier;
        std::function<CpuDeviceResult(const CpuInitializationAction&)> initialization;
        std::function<CpuDeviceResult(const CpuTaskAction&)> task;
        std::function<void()> guestExit;
    };

    CpuExecutionController(TileConfiguration config, Timing::Clock<Timing::Cpu> clock,
                           Transport transport, Host host, std::uint64_t partition = 0,
                           std::uint32_t workers = 1);
    ~CpuExecutionController();
    CpuExecutionController(const CpuExecutionController&) = delete;
    CpuExecutionController& operator=(const CpuExecutionController&) = delete;
    void onWake(std::optional<std::uint64_t> watchdog = std::nullopt,
                std::optional<std::uint64_t> scheduled = std::nullopt);
    void dispatchCaptures();
    bool processPendingSyncEvent();
    std::uint64_t pendingStepId() const noexcept
    {
        return pendingSyncEvent_ ? stepId_ : 0;
    }
    void completeDevice(std::uint64_t step);
    void completeMemory(std::uint64_t step, bool group = false);
    void resumeTransmitWaitIfReady();
    void resumeReceiveWaitIfReady();
    void stop();
    void assertDrained() const;
    bool hasMemoryReplay() const noexcept
    {
        return memoryBatchEnvelope_.has_value();
    }
    bool hasAnalogReplay() const noexcept
    {
        return analogSubmitBatchEnvelope_.has_value();
    }
    bool hasPendingWork() const noexcept
    {
        return pendingSyncEvent_.has_value() || initialCapturePending_ ||
               runtimeQemuReadySetCapturePending_ || localQemuLookaheadFuture_.has_value() ||
               memoryBatchEnvelope_ || analogSubmitBatchEnvelope_ ||
               globalDMASubmitBatchEnvelope_ || globalDMAMacroRunEnvelope_;
    }
    // Copy scalar metadata only; no replay payload is exposed, even const.
    std::optional<QemuSyncEvent> pending() const;
    CpuExecutionLedger::Snapshot accounting() const noexcept
    {
        return ledger_.snapshot();
    }
    Statistics statistics() const;
    void scheduleCpuSyncEvent(std::uint64_t cycles);
    void completeWait();
    // Captured input is committed only on the simulator owner thread.
    void commitCapturedQemuEvent(const QemuSyncEvent& event);

  private:
    struct Diagnostics
    {
        std::function<void(int, const std::string&)> log;
        template <class... A> [[noreturn]] void fatal(int, int, const char* format, A... args) const
        {
            throw std::runtime_error(message(format, args...));
        }
        template <class... A> void verbose(int, int level, int, const char* format, A... args) const
        {
            if (log)
                log(level, message(format, args...));
        }
        template <class... A> static std::string message(const char* format, A... args)
        {
            return diagnosticMessage("execution diagnostic formatting error", format, args...);
        }
    };
    struct GlobalDMAMacroEndpoint
    {
        std::size_t recordIndex;
        bool wait;
    };
    Timing::Clock<Timing::Cpu> cpuDomain() const
    {
        return clock_;
    }
    AddressRegion scratchpadRegion() const noexcept
    {
        return {MITTENS_SCRATCHPAD_BASE, config_.scratchpadBytes};
    }
    static std::uint64_t divideRoundUp(std::uint64_t n, std::uint64_t d)
    {
        return Timing::ceilDivide(n, d);
    }
    bool prepareDeferredLookahead(const QemuSyncEvent& event);
    void applyResult(const CpuDeviceResult& result);
    void requireStep(std::uint64_t step) const;
    bool pendingDelayElapsed() const noexcept
    {
        return !pendingReadyTick_ || host_.now().value >= pendingReadyTick_->value;
    }
    void replacePending(QemuSyncEvent event)
    {
        stepId_ = Timing::add(stepId_, 1);
        pendingReadyTick_.reset();
        scheduledWakeGeneration_.reset();
        pendingSyncEvent_ = std::move(event);
    }
    template <class Callback, class Action>
    CpuDeviceResult dispatchDevice(Callback& callback, const Action& action)
    {
        if (dispatching_)
            throw std::logic_error("recursive CPU device dispatch");
        dispatching_ = true;
        try
        {
            auto result = callback(action);
            dispatching_ = false;
            return result;
        }
        catch (...)
        {
            dispatching_ = false;
            throw;
        }
    }
    bool dispatching_ = false;
    std::uint64_t stepId_ = 0;
    // Absolute eligibility for captured instruction/replay and endpoint delays.
    // Independent notifications may retry a pending step but cannot deliver it
    // before this timestamp. Scheduled wakes are revocable, not completions.
    std::optional<Timing::Ticks> pendingReadyTick_;
    std::optional<std::uint64_t> scheduledWakeGeneration_;
    std::uint64_t nextWakeGeneration_ = 0;
    std::uint64_t accountCpuTo(const QemuSyncEvent& e, bool boundary)
    {
        return accountCpuTo(e.instructionsExecuted, e.vectorInstructionsExecuted, boundary);
    }
    TileConfiguration config_;
    Timing::Clock<Timing::Cpu> clock_;
    Transport transport_;
    Host host_;
    Diagnostics output_;
    CpuExecutionLedger ledger_;
    std::shared_ptr<std::atomic<bool>> captureLease_ = std::make_shared<std::atomic<bool>>(true);
    bool stopped_ = false;
    bool initialCapturePending_ = false;
    enum class CaptureMode { Initial, Runtime };
    QemuReadySetExecutor::Task makeReadySetCaptureTask(
        std::uint64_t frontierTick, std::uint64_t grantEpoch, CaptureMode mode,
        std::function<void(const Transport&)> startCapture);
    void grantAndCaptureQemu();
    void beginQemuGrant();
    void reserveInitialQemuReadySetGrant();
    void submitInitialQemuReadySetGrant();
    void resumeAndCaptureQemu();
    void startLocalQemuLookahead(const QemuSyncEvent& event);
    bool consumeLocalQemuLookahead(const QemuSyncEvent& event);
    void captureQemuEvent();
    void validateCapturedQemuEvent(const QemuSyncEvent& event) const;
    void validateInitialQemuReadySetEvent(const QemuSyncEvent& event) const;
    std::uint64_t previewQemuEventDeliveryTick(const QemuSyncEvent& event,
                                               std::uint64_t frontierTick) const;
    void beginMemoryBatch(const QemuSyncEvent& event);
    void clearMemoryBatchState();
    void scheduleScratchpadMemoryBatch();
    void scheduleNextMemoryBatchStep();
    void advanceMemoryBatchAccess();
    void advanceMemoryBatchGroup();
    void beginGlobalDMASubmitBatch(const QemuSyncEvent& event);
    void clearGlobalDMASubmitBatchState();
    void scheduleNextGlobalDMASubmitBatchStep();
    void advanceGlobalDMASubmitBatch();
    void beginGlobalDMAMacroRun(const QemuSyncEvent& event);
    void clearGlobalDMAMacroRunState();
    void scheduleNextGlobalDMAMacroEvent();
    void advanceGlobalDMAMacroRun();
    void beginAnalogSubmitBatch(const QemuSyncEvent& event);
    void validateAnalogSubmitBatch(const QemuSyncEvent& event) const;
    void clearAnalogSubmitBatchState();
    void scheduleNextAnalogSubmitBatchStep();
    void advanceAnalogSubmitBatch();
    void beginWait(const QemuSyncEvent& event);
    bool waitIsProfiled(std::uint32_t reason) const noexcept;
    void scheduleProgressWatchdogEvent();
    void cancelProgressWatchdogEvent();
    std::uint64_t accountCpuTo(std::uint64_t instructions, std::uint64_t vectors, bool boundary);
    std::optional<QemuSyncEvent> pendingSyncEvent_;
    // Suppress the fallback watchdog when this handler already scheduled work.
    bool cpuSyncWakeScheduledDuringHandler_ = false;
    std::optional<std::uint64_t> progressWatchdogWakeTick_;
    std::uint64_t progressWatchdogWakeGeneration_ = 0;
    std::uint64_t qemuReadySetPartitionKey_ = 0;
    std::uint32_t qemuReadySetPartitionWorkers_ = 1;
    bool initialQemuReadySetSubmitted_ = false;
    bool runtimeQemuReadySetCapturePending_ = false;
    std::optional<std::future<QemuSyncEvent>> localQemuLookaheadFuture_;
    std::uint64_t localQemuLookaheadSourceSequence_ = 0;
    std::uint32_t localQemuLookaheadSourceReason_ = MITTENS_SYNC_STOP_NONE;
    std::uint64_t synchronizationGrants_ = 0;
    std::uint64_t synchronizationEvents_ = 0;
    std::array<std::uint64_t, MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO + 1>
        synchronizationStopCounts_{};
    std::optional<QemuSyncEvent> analogSubmitBatchEnvelope_;
    std::vector<MittensSyncAnalogSubmit> analogSubmitBatchRecords_;
    std::size_t analogSubmitBatchIndex_ = 0;
    std::uint64_t analogSubmitBatchTransportRecords_ = 0;
    std::optional<std::uint64_t> activeWaitStartTick_;
    std::uint32_t activeWaitReason_ = MITTENS_SYNC_STOP_NONE;
    std::uint64_t activeWaitEventSequence_ = 0;
    std::array<std::uint64_t, MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO + 1> waitTicks_{};
    std::uint64_t scratchpadTimingCycle_ = 0;
    std::optional<QemuSyncEvent> memoryBatchEnvelope_;
    std::vector<MittensSyncMemoryAccess> memoryBatchRecords_;
    std::uint64_t memoryBatchTransportRecords_ = 0;
    std::uint64_t memoryBatchLogicalAccesses_ = 0;
    bool memoryBatchScratchpad_ = false;
    std::size_t memoryBatchIndex_ = 0;
    std::size_t memoryBatchGroupEndIndex_ = 0;
    std::optional<QemuSyncEvent> globalDMASubmitBatchEnvelope_;
    std::vector<MittensSyncGlobalDMASubmit> globalDMASubmitBatchRecords_;
    std::size_t globalDMASubmitBatchIndex_ = 0;
    std::uint64_t globalDMASubmitBatchTransportRecords_ = 0;
    std::uint64_t globalDMAWaitBatchTransportRecords_ = 0;
    std::optional<QemuSyncEvent> globalDMAMacroRunEnvelope_;
    std::vector<MittensSyncGlobalDMASubmit> globalDMAMacroRunRecords_;
    std::vector<GlobalDMAMacroEndpoint> globalDMAMacroRunEndpoints_;
    std::size_t globalDMAMacroRunIndex_ = 0;
    std::uint64_t globalDMAMacroRunTransportRecords_ = 0;
    std::uint64_t taskFinishEvents_ = 0;
    bool transmitWaitArmed_ = false;
    bool receiveWaitArmed_ = false;
};
} // namespace SST::Mittens
