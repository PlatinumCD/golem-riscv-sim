#include "cpuExecutionController.h"
#include "../memory/memoryAccessCoalescer.h"
#include <algorithm>
#include <sstream>
namespace SST::Mittens
{
void CpuExecutionController::grantAndCaptureQemu()
{
    if (!host_.running())
    {
        return;
    }

    beginQemuGrant();
    captureQemuEvent();
}

void CpuExecutionController::beginQemuGrant()
{
    const auto budget = host_.initializing() ? config_.memoryInitializationInstructionQuantum
                                             : config_.syncInstructionQuantum;
    ledger_.beginGrant(transport_.grant(budget), budget);
    ++synchronizationGrants_;
}

void CpuExecutionController::reserveInitialQemuReadySetGrant()
{
    ledger_.beginGrant(Timing::add(ledger_.snapshot().epoch, 1),
                       config_.memoryInitializationInstructionQuantum);
    ++synchronizationGrants_;
}

QemuReadySetExecutor::Task CpuExecutionController::makeReadySetCaptureTask(
    std::uint64_t frontierTick, std::uint64_t grantEpoch, CaptureMode mode,
    std::function<void(const Transport&)> startCapture)
{
    return {
        frontierTick, config_.tileId, grantEpoch,
        // Worker capture owns transport/lease copies, never the controller.
        [transport = transport_, lease = captureLease_, start = std::move(startCapture)]()
        {
            if (!lease->load())
                throw std::runtime_error("capture cancelled");
            start(transport);
            return transport.captureHost(*lease);
        },
        [this, lease = captureLease_, mode](const QemuSyncEvent& event)
        {
            if (!lease->load())
                throw std::runtime_error("capture endpoint revoked");
            validateCapturedQemuEvent(event);
            if (mode == CaptureMode::Initial)
                validateInitialQemuReadySetEvent(event);
        },
        [this, lease = captureLease_, frontierTick](const QemuSyncEvent& event)
        {
            if (!lease->load())
                throw std::runtime_error("capture endpoint revoked");
            return previewQemuEventDeliveryTick(event, frontierTick);
        },
        [this, lease = captureLease_, mode](const QemuSyncEvent& event)
        {
            // A completion can outlive its endpoint: guard before touching this.
            if (!lease->load())
                return;
            if (mode == CaptureMode::Runtime)
            {
                if (!runtimeQemuReadySetCapturePending_)
                    throw std::logic_error("runtime QEMU ready-set commit has no pending capture");
                runtimeQemuReadySetCapturePending_ = false;
            }
            commitCapturedQemuEvent(event);
        },
    };
}

void CpuExecutionController::submitInitialQemuReadySetGrant()
{
    if (!host_.running() || initialQemuReadySetSubmitted_ || !host_.initializing() ||
        synchronizationGrants_ != 0 || ledger_.snapshot().epoch != 0 ||
        config_.qemuReadySetWorkers <= 1)
    {
        output_.fatal(0, -1, "tile %u attempted an ineligible initial QEMU ready-set grant\n",
                      static_cast<unsigned>(config_.tileId));
    }

    /*
     * Reserve the deterministic epoch on the SST thread, but let a bounded
     * executor worker issue the actual bridge grant.  Granting here would
     * wake every QEMU immediately and defeat the worker bound.
     */
    reserveInitialQemuReadySetGrant();
    initialQemuReadySetSubmitted_ = true;
    initialCapturePending_ = true;
    const std::uint64_t frontierTick = host_.now().value;

    const std::uint64_t grantEpoch = ledger_.snapshot().epoch;
    const std::uint64_t instructionBudget = ledger_.snapshot().budget;

    std::optional<std::vector<QemuReadySetExecutor::Completion>> completions;
    try
    {
        completions = QemuCaptureCoordinator::submitInitialQemuReadySetTask(
            qemuReadySetPartitionKey_, config_.qemuReadySetWorkers, qemuReadySetPartitionWorkers_,
            config_.memoryInitializationBarrierTiles,
            makeReadySetCaptureTask(frontierTick, grantEpoch, CaptureMode::Initial,
                [grantEpoch, instructionBudget](const Transport& transport)
                {
                    if (transport.grant(instructionBudget) != grantEpoch)
                    {
                        throw std::runtime_error(
                            "initial bridge grant epoch did not match its reservation");
                    }
                }));

    }
    catch (const std::exception& error)
    {
        output_.fatal(0, -1, "tile %u initial QEMU ready-set execution failed: %s\n",
                      static_cast<unsigned>(config_.tileId), error.what());
    }

    if (!completions.has_value())
    {
        return;
    }
    for (const auto& completion : *completions)
    {
        completion.commit(completion.event);
    }
}

void CpuExecutionController::resumeAndCaptureQemu()
{
    if (!pendingSyncEvent_.has_value() || !host_.running() || !pendingDelayElapsed())
    {
        return;
    }

    pendingReadyTick_.reset();
    scheduledWakeGeneration_.reset();
    cancelProgressWatchdogEvent();
    const QemuSyncEvent event = *pendingSyncEvent_;
    completeWait();
    if (consumeLocalQemuLookahead(event))
    {
        return;
    }
    if (config_.qemuRuntimeReadySet)
    {
        const std::uint64_t frontierTick = host_.now().value;

        pendingSyncEvent_.reset();
        transmitWaitArmed_ = false;
        receiveWaitArmed_ = false;
        runtimeQemuReadySetCapturePending_ = true;
        try
        {
            QemuCaptureCoordinator::enqueueRuntimeQemuReadySetTask(
                makeReadySetCaptureTask(frontierTick, ledger_.snapshot().epoch, CaptureMode::Runtime,
                    [event](const Transport& transport) { transport.resume(event); }));
            host_.scheduleCaptureDispatch();
        }
        catch (const std::exception& error)
        {
            runtimeQemuReadySetCapturePending_ = false;
            host_.terminateAll();
            output_.fatal(0, -1, "tile %u could not enqueue runtime QEMU ready-set work: %s\n",
                          static_cast<unsigned>(config_.tileId), error.what());
        }
        return;
    }
    try
    {
        transport_.resume(event);
    }
    catch (const std::exception& error)
    {
        output_.fatal(0, -1, "tile %u failed to resume synchronized QEMU execution: %s\n",
                      static_cast<unsigned>(config_.tileId), error.what());
    }

    pendingSyncEvent_.reset();
    transmitWaitArmed_ = false;
    receiveWaitArmed_ = false;
    captureQemuEvent();
}

void CpuExecutionController::startLocalQemuLookahead(const QemuSyncEvent& event)
{
    if (!config_.qemuLocalLookahead)
    {
        return;
    }
    const bool standaloneMemoryBatch = event.stopReason == MITTENS_SYNC_STOP_MEMORY_BATCH &&
                                       (event.flags == MITTENS_SYNC_EVENT_FLAG_NONE ||
                                        event.flags == MITTENS_SYNC_EVENT_FLAG_QUANTUM_END);
    const bool deferredAnalogLoad = host_.deferredAnalogMatches(event);
    if (localQemuLookaheadFuture_.has_value() || localQemuLookaheadSourceSequence_ != 0 ||
        localQemuLookaheadSourceReason_ != MITTENS_SYNC_STOP_NONE || event.eventSequence == 0 ||
        (!standaloneMemoryBatch && !deferredAnalogLoad) || event.memoryBatch.empty())
    {
        output_.fatal(0, -1, "tile %u attempted invalid or nested local QEMU lookahead\n",
                      static_cast<unsigned>(config_.tileId));
    }

    const bool quantumEnd =
        standaloneMemoryBatch && event.flags == MITTENS_SYNC_EVENT_FLAG_QUANTUM_END;
    std::uint64_t grantEpoch = ledger_.snapshot().epoch;
    std::uint64_t instructionBudget = ledger_.snapshot().budget;
    if (quantumEnd)
    {
        if (ledger_.snapshot().epoch == UINT64_MAX)
        {
            output_.fatal(0, -1, "tile %u local QEMU lookahead grant epoch overflowed\n",
                          static_cast<unsigned>(config_.tileId));
        }
        ledger_.beginGrant(Timing::add(ledger_.snapshot().epoch, 1),
                           config_.syncInstructionQuantum);
        grantEpoch = ledger_.snapshot().epoch;
        instructionBudget = ledger_.snapshot().budget;
        ++synchronizationGrants_;
    }
    localQemuLookaheadSourceSequence_ = event.eventSequence;
    localQemuLookaheadSourceReason_ = event.stopReason;
    try
    {
        localQemuLookaheadFuture_.emplace(QemuCaptureCoordinator::submitLocalQemuLookahead(
            config_.qemuReadySetWorkers,
            [transport = transport_, lease = captureLease_, event, quantumEnd, grantEpoch,
             instructionBudget]()
            {
                if (!lease->load())
                    throw std::runtime_error("capture cancelled");
                if (quantumEnd)
                {
                    const std::uint64_t observedEpoch = transport.grant(instructionBudget);
                    if (observedEpoch != grantEpoch)
                    {
                        throw std::runtime_error(
                            "local QEMU lookahead grant epoch did not match its reservation");
                    }
                }
                else
                {
                    transport.resume(event);
                }
                return transport.captureHost(*lease);
            }));
    }
    catch (const std::exception& error)
    {
        localQemuLookaheadSourceSequence_ = 0;
        localQemuLookaheadSourceReason_ = MITTENS_SYNC_STOP_NONE;
        output_.fatal(0, -1, "tile %u could not start local QEMU lookahead: %s\n",
                      static_cast<unsigned>(config_.tileId), error.what());
    }
}

bool CpuExecutionController::consumeLocalQemuLookahead(const QemuSyncEvent& event)
{
    if (!localQemuLookaheadFuture_.has_value())
    {
        return false;
    }
    if (!config_.qemuLocalLookahead || localQemuLookaheadSourceSequence_ == 0 ||
        event.eventSequence != localQemuLookaheadSourceSequence_ ||
        event.stopReason != localQemuLookaheadSourceReason_)
    {
        output_.fatal(0, -1, "tile %u reached a mismatched local QEMU lookahead frontier\n",
                      static_cast<unsigned>(config_.tileId));
    }

    const bool ready =
        localQemuLookaheadFuture_->wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    QemuCaptureCoordinator::recordLocalQemuLookaheadConsume(ready);
    std::future<QemuSyncEvent> future = std::move(*localQemuLookaheadFuture_);
    localQemuLookaheadFuture_.reset();
    localQemuLookaheadSourceSequence_ = 0;
    localQemuLookaheadSourceReason_ = MITTENS_SYNC_STOP_NONE;

    QemuSyncEvent captured;
    try
    {
        captured = future.get();
        validateCapturedQemuEvent(captured);
    }
    catch (const std::exception& error)
    {
        host_.terminateAll();
        output_.fatal(0, -1, "tile %u local QEMU lookahead capture failed: %s\n",
                      static_cast<unsigned>(config_.tileId), error.what());
    }

    pendingSyncEvent_.reset();
    transmitWaitArmed_ = false;
    receiveWaitArmed_ = false;
    commitCapturedQemuEvent(captured);
    return true;
}

void CpuExecutionController::captureQemuEvent()
{
    const auto started = std::chrono::steady_clock::now();
    while (host_.running())
    {
        auto event = transport_.poll();
        if (!event)
        {
            if (host_.observeExit())
                return;
            host_.watchdog();
            continue;
        }
        validateCapturedQemuEvent(*event);
        QemuCaptureCoordinator::recordQemuCaptureHostTime(*event, std::chrono::steady_clock::now() -
                                                                      started);
        commitCapturedQemuEvent(*event);
        return;
    }
}

void CpuExecutionController::validateCapturedQemuEvent(const QemuSyncEvent& event) const
{
    ledger_.validateCaptured(event.grantEpoch,
                             {event.instructionsExecuted, event.vectorInstructionsExecuted});
}

void CpuExecutionController::validateInitialQemuReadySetEvent(const QemuSyncEvent& event) const
{
    if (!host_.running() || !host_.initializing() || !initialQemuReadySetSubmitted_ ||
        pendingSyncEvent_.has_value())
    {
        throw std::logic_error("tile state changed while the initial grant was in flight");
    }
    const bool supportedStop = event.stopReason == MITTENS_SYNC_STOP_QUANTUM_END ||
                               event.stopReason == MITTENS_SYNC_STOP_ANALOG_SUBMIT ||
                               event.stopReason == MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH ||
                               event.stopReason == MITTENS_SYNC_STOP_MEMORY_FENCE ||
                               event.stopReason == MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE;
    if (!supportedStop || !event.memoryBatch.empty() || !event.globalDMASubmitBatch.empty() ||
        (!event.analogSubmitBatch.empty() && !config_.analogCommandBatching) ||
        (event.flags & MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH) != 0)
    {
        std::ostringstream message;
        message << "initial parallel grant produced an unsupported event: "
                << "stop=" << syncStopReasonName(event.stopReason) << "(" << event.stopReason << ")"
                << " flags=0x" << std::hex << event.flags << std::dec
                << " batch_records=" << event.memoryBatch.size()
                << " global_dma_batch_records=" << event.globalDMASubmitBatch.size()
                << " analog_batch_records=" << event.analogSubmitBatch.size()
                << " executed=" << event.instructionsExecuted
                << " vectors=" << event.vectorInstructionsExecuted
                << " analog_array=" << event.analogArrayId
                << " analog_sequence=" << event.analogSequence << " memory_address=0x" << std::hex
                << event.memoryAddress << std::dec << " memory_size=" << event.memorySize
                << " memory_flags=0x" << std::hex << event.memoryFlags << std::dec
                << " global_dma_offset=" << event.globalDMAOffset()
                << " global_dma_scratchpad_offset=" << event.globalDMAScratchpadOffsetValue()
                << " global_dma_bytes=" << event.globalDMAByteCount()
                << " global_dma_token=" << event.globalDMATokenId();
        throw std::runtime_error(message.str());
    }
    // With scratchpad-access batching enabled, an ordinary RISC-V fence is
    // the first semantic stop in production guests.  It is eligible because
    // capture stops before crossing the fence, it has no shared payload, and
    // SST drains any tile-local writes and resumes it only after deterministic
    // owner-thread commit.  A fused/batched fence remains rejected above.
    const std::uint32_t expectedFenceFlags = event.analogSubmitBatch.empty()
                                                 ? MITTENS_SYNC_EVENT_FLAG_NONE
                                                 : MITTENS_SYNC_EVENT_FLAG_ANALOG_BATCH;
    if (event.stopReason == MITTENS_SYNC_STOP_MEMORY_FENCE &&
        (event.flags != expectedFenceFlags || event.memoryAddress != 0 || event.memorySize != 0 ||
         event.memoryFlags != MITTENS_SYNC_MEMORY_FLAG_NONE))
    {
        throw std::runtime_error("initial parallel memory-fence event is malformed");
    }
    if (!event.analogSubmitBatch.empty())
    {
        validateAnalogSubmitBatch(event);
    }
    host_.validateInitialDevice(event);
    if (event.stopReason == MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE)
    {
        if (!config_.memoryInitializationBatching || event.memorySize != 0 ||
            event.memoryFlags != MITTENS_SYNC_MEMORY_FLAG_NONE)
        {
            throw std::runtime_error("aggregate memory-initialization event is malformed");
        }
        const std::uint64_t reads = event.memoryInitializationReadBytes();
        const std::uint64_t writes = event.memoryInitializationWriteBytes();
        if (reads > UINT64_MAX - writes)
        {
            throw std::overflow_error("aggregate memory-initialization byte count overflowed");
        }
        const std::uint64_t transferCycles =
            divideRoundUp(reads + writes, config_.memoryInitializationBytesPerCycle);
        if (transferCycles > UINT64_MAX - config_.memoryInitializationLatencyCycles)
        {
            throw std::overflow_error("aggregate memory-initialization cycle count overflowed");
        }
    }
}

std::uint64_t CpuExecutionController::previewQemuEventDeliveryTick(const QemuSyncEvent& event,
                                                                   std::uint64_t frontierTick) const
{
    const auto charge =
        ledger_.previewTo({event.instructionsExecuted, event.vectorInstructionsExecuted});
    (void)Timing::add(scratchpadTimingCycle_, charge.cycles.value);
    return cpuDomain().after({frontierTick}, charge.cycles).value;
}

void CpuExecutionController::commitCapturedQemuEvent(const QemuSyncEvent& event)
{
    initialCapturePending_ = false;
    const unsigned batchKinds = (!event.memoryBatch.empty() ? 1U : 0U) +
                                (!event.globalDMASubmitBatch.empty() ? 1U : 0U) +
                                (!event.analogSubmitBatch.empty() ? 1U : 0U);
    if (batchKinds > 1)
    {
        output_.fatal(0, -1, "tile %u received overlapping synchronization batches\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (!event.memoryBatch.empty())
    {
        beginMemoryBatch(event);
        return;
    }
    if (event.stopReason == MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO)
    {
        beginGlobalDMAMacroRun(event);
        return;
    }
    if (!event.globalDMASubmitBatch.empty() &&
        (event.stopReason == MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT_BATCH ||
         (event.flags & MITTENS_SYNC_EVENT_FLAG_GLOBAL_DMA_SUBMITS) != 0))
    {
        beginGlobalDMASubmitBatch(event);
        return;
    }
    if (!event.analogSubmitBatch.empty())
    {
        beginAnalogSubmitBatch(event);
        return;
    }
    if (event.stopReason == MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH)
    {
        if (event.globalDMASubmitBatch.empty() ||
            event.globalDMASubmitBatch.size() > MITTENS_SYNC_GLOBAL_DMA_BATCH_CAPACITY ||
            event.flags != MITTENS_SYNC_EVENT_FLAG_NONE)
        {
            output_.fatal(0, -1, "tile %u received an invalid global DMA wait batch\n",
                          static_cast<unsigned>(config_.tileId));
        }
        globalDMAWaitBatchTransportRecords_ += event.globalDMASubmitBatch.size();
    }
    else if (!event.globalDMASubmitBatch.empty())
    {
        output_.fatal(0, -1, "tile %u received global DMA records on a scalar event\n",
                      static_cast<unsigned>(config_.tileId));
    }
    // accountCpuTo updates ledger_.snapshot().baseline.instructions.  The stop reason below,
    // rather than raw instruction count, drives the liveness watchdog.
    const std::uint64_t instructionCycles =
        accountCpuTo(event, event.stopReason != MITTENS_SYNC_STOP_QUANTUM_END);
    ++synchronizationEvents_;
    if (event.stopReason < synchronizationStopCounts_.size())
    {
        ++synchronizationStopCounts_[event.stopReason];
    }
    replacePending(event);
    const bool taskFinished = event.stopReason == MITTENS_SYNC_STOP_TASK_FINISH;
    // Before the guest announces memory-initialization completion, a
    // quantum end represents bounded forward execution through ABI
    // setup, runtime-state construction, or analog-weight setup.  Publish
    // that separately from runtime architectural progress so tiles that
    // already reached the deployment barrier do not mistake a long setup
    // interval on another tile for a deadlock.  Once initialization ends,
    // quantum-only execution remains deliberately insufficient to keep
    // the routing watchdog alive.
    const bool initializationExecutionProgress =
        host_.initializing() && event.stopReason == MITTENS_SYNC_STOP_QUANTUM_END &&
        event.instructionsExecuted != 0;
    if (initializationExecutionProgress)
    {
        host_.progress(true);
    }
    // A quantum-end event can represent nothing more than QEMU spinning
    // in the NIC wait loop.  Counting those instructions as deployment
    // progress keeps the watchdog alive forever when a network cycle is
    // deadlocked.  Reset the liveness clock only for a task/bridge/memory
    // boundary that can change architectural state; explicit wait events
    // are intentionally excluded.
    const bool architecturalProgress =
        event.stopReason != MITTENS_SYNC_STOP_QUANTUM_END &&
        event.stopReason != MITTENS_SYNC_STOP_NIC_TRANSMIT_WAIT &&
        event.stopReason != MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT &&
        event.stopReason != MITTENS_SYNC_STOP_ANALOG_WAIT &&
        event.stopReason != MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT &&
        event.stopReason != MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH;
    if (taskFinished)
    {
        ++taskFinishEvents_;
    }
    if (taskFinished || architecturalProgress)
    {
        // A tile blocked on a downstream receive is healthy while any
        // producer on this SST rank continues to execute.  Publish one
        // bounded rank-local epoch instead of treating each passive
        // consumer as an independent liveness domain.  Quantum-end and
        // explicit wait events are not progress signals: they can repeat
        // forever without a task, DMA, memory, or bridge transition.
        host_.progress(false);
    }

    output_.verbose(
        0, 3, 0,
        "tile %u fd 41 event %llu: %s after %llu instructions (%llu vector) in grant %llu\n",
        static_cast<unsigned>(config_.tileId), static_cast<unsigned long long>(event.eventSequence),
        syncStopReasonName(event.stopReason),
        static_cast<unsigned long long>(event.instructionsExecuted),
        static_cast<unsigned long long>(event.vectorInstructionsExecuted),
        static_cast<unsigned long long>(event.grantEpoch));

    scheduleCpuSyncEvent(instructionCycles);
    host_.watchdog();
}

void CpuExecutionController::beginMemoryBatch(const QemuSyncEvent& event)
{
    const bool standaloneBatch = event.stopReason == MITTENS_SYNC_STOP_MEMORY_BATCH;
    const bool fusedEvent = (event.flags & MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH) != 0;
    if (memoryBatchEnvelope_.has_value() || event.memoryBatch.empty() ||
        (standaloneBatch && fusedEvent) || (!standaloneBatch && !fusedEvent) ||
        (standaloneBatch && (event.flags & ~MITTENS_SYNC_EVENT_FLAG_QUANTUM_END) != 0))
    {
        output_.fatal(0, -1, "tile %u received an invalid or nested memory batch\n",
                      static_cast<unsigned>(config_.tileId));
    }
    const bool scratchpadBatch =
        (event.memoryBatch.front().flags & MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD) != 0;
    if (scratchpadBatch && (!config_.scratchpadAccessBatching || !host_.scratchpadAvailable()))
    {
        output_.fatal(0, -1,
                      "tile %u received a scratchpad batch while scratchpad access "
                      "batching is disabled\n",
                      static_cast<unsigned>(config_.tileId));
    }
    std::uint64_t previousInstructions = ledger_.snapshot().baseline.instructions;
    std::uint64_t previousVectors = ledger_.snapshot().baseline.vectors;
    std::uint64_t batchLogicalAccesses = 0;
    for (const MittensSyncMemoryAccess& access : event.memoryBatch)
    {
        const bool accessIsScratchpad = (access.flags & MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD) != 0;
        const bool contiguousRun = (access.flags & MITTENS_SYNC_MEMORY_FLAG_CONTIGUOUS_RUN) != 0;
        const bool vectorTransaction =
            (access.flags & MITTENS_SYNC_MEMORY_FLAG_VECTOR_TRANSACTION) != 0;
        const bool repeatInvalid =
            access.repeat_count == 0 || contiguousRun != (access.repeat_count > 1) ||
            (contiguousRun && (!accessIsScratchpad || !config_.scratchpadAccessRunCompaction)) ||
            (vectorTransaction &&
             (!accessIsScratchpad || !contiguousRun || access.size != sizeof(std::uint32_t) ||
              access.repeat_count != 8 || access.address % 32 != 0));
        const bool runSizeOverflow =
            access.size != 0 && access.repeat_count > UINT64_MAX / access.size;
        const std::uint64_t runBytes =
            runSizeOverflow ? UINT64_MAX
                            : static_cast<std::uint64_t>(access.size) * access.repeat_count;
        const bool scratchpadRangeInvalid =
            accessIsScratchpad && !scratchpadRegion().containsRange(access.address, runBytes);
        if (access.instructions_executed < previousInstructions ||
            access.instructions_executed > event.instructionsExecuted ||
            access.vector_instructions_executed < previousVectors ||
            access.vector_instructions_executed > access.instructions_executed ||
            access.vector_instructions_executed > event.vectorInstructionsExecuted ||
            access.size == 0 || access.size > 16 || repeatInvalid || runSizeOverflow ||
            accessIsScratchpad != scratchpadBatch || scratchpadRangeInvalid ||
            (access.flags &
             ~(MITTENS_SYNC_MEMORY_FLAG_WRITE | MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD |
               MITTENS_SYNC_MEMORY_FLAG_REGISTER_DEPS | MITTENS_SYNC_MEMORY_FLAG_CONTIGUOUS_RUN |
               MITTENS_SYNC_MEMORY_FLAG_VECTOR_TRANSACTION)) != 0)
        {
            output_.fatal(0, -1, "tile %u received an invalid memory batch record\n",
                          static_cast<unsigned>(config_.tileId));
        }
        previousInstructions = access.instructions_executed;
        previousVectors = access.vector_instructions_executed;
        if (batchLogicalAccesses > UINT64_MAX - access.repeat_count)
        {
            output_.fatal(0, -1, "tile %u memory batch logical count overflowed\n",
                          static_cast<unsigned>(config_.tileId));
        }
        batchLogicalAccesses += access.repeat_count;
    }
    if (memoryBatchTransportRecords_ > UINT64_MAX - event.memoryBatch.size() ||
        memoryBatchLogicalAccesses_ > UINT64_MAX - batchLogicalAccesses)
    {
        output_.fatal(0, -1, "tile %u memory batch accounting overflowed\n",
                      static_cast<unsigned>(config_.tileId));
    }
    memoryBatchTransportRecords_ += event.memoryBatch.size();
    memoryBatchLogicalAccesses_ += batchLogicalAccesses;
    memoryBatchEnvelope_ = event;
    memoryBatchScratchpad_ = scratchpadBatch;
    memoryBatchRecords_ =
        scratchpadBatch ? event.memoryBatch
                        : coalesceMemoryAccesses(event.memoryBatch, config_.memoryCacheLineSize);
    memoryBatchIndex_ = 0;
    memoryBatchGroupEndIndex_ = 0;
    ++synchronizationEvents_;
    ++synchronizationStopCounts_[MITTENS_SYNC_STOP_MEMORY_BATCH];
    if (fusedEvent)
    {
        if (config_.qemuLocalLookahead)
        {
            QemuCaptureCoordinator::recordLocalQemuLookaheadFusedTerminal(event.stopReason);
        }
        if (event.stopReason >= synchronizationStopCounts_.size())
        {
            output_.fatal(0, -1,
                          "tile %u received a fused memory batch with an invalid "
                          "stop reason\n",
                          static_cast<unsigned>(config_.tileId));
        }
        ++synchronizationStopCounts_[event.stopReason];
        if (event.stopReason == MITTENS_SYNC_STOP_TASK_FINISH)
        {
            ++taskFinishEvents_;
        }
    }
    host_.progress(false);
    if (memoryBatchScratchpad_)
    {
        if (fusedEvent)
        {
            (void)prepareDeferredLookahead(event);
        }
        scheduleScratchpadMemoryBatch();
        if (standaloneBatch && (event.flags == MITTENS_SYNC_EVENT_FLAG_NONE ||
                                event.flags == MITTENS_SYNC_EVENT_FLAG_QUANTUM_END))
        {
            startLocalQemuLookahead(event);
        }
    }
    else
    {
        scheduleNextMemoryBatchStep();
    }
}

void CpuExecutionController::clearMemoryBatchState()
{
    memoryBatchEnvelope_.reset();
    memoryBatchRecords_.clear();
    memoryBatchScratchpad_ = false;
    memoryBatchIndex_ = 0;
    memoryBatchGroupEndIndex_ = 0;
}

void CpuExecutionController::scheduleScratchpadMemoryBatch()
{
    if (!memoryBatchEnvelope_.has_value() || !memoryBatchScratchpad_ ||
        memoryBatchRecords_.empty() || !host_.scratchpadAvailable())
    {
        output_.fatal(0, -1, "tile %u attempted to replay an invalid scratchpad batch\n",
                      static_cast<unsigned>(config_.tileId));
    }

    /*
     * Scratchpad accesses are scheduled in CPU-clock cycles.  A guest may
     * have slept for a NIC/DMA event since its prior batch; keep the CPU's
     * local cursor at the current architectural frontier before reserving
     * ports alongside asynchronous DMA clients.
     */
    scratchpadTimingCycle_ = std::max(scratchpadTimingCycle_, cpuDomain().floor(host_.now()).value);
    const std::uint64_t startCycle = scratchpadTimingCycle_;
    for (const MittensSyncMemoryAccess& access : memoryBatchRecords_)
    {
        // Scratchpad batches are timed entirely in this owner-thread loop.
        // Only their cumulative instruction counters participate in CPU
        // accounting; copying the batch envelope here also copied its whole
        // memoryBatch vector once per record, making replay quadratic in the
        // batch length before immediately discarding that copy.
        const bool vectorTransaction =
            (access.flags & MITTENS_SYNC_MEMORY_FLAG_VECTOR_TRANSACTION) != 0;
        (void)accountCpuTo(access.instructions_executed, access.vector_instructions_executed, true);
        const ScratchpadSchedule schedule = host_.scratchpad(access, scratchpadTimingCycle_);
        scratchpadTimingCycle_ = schedule.completionCycle;
        synchronizationStopCounts_[MITTENS_SYNC_STOP_MEMORY_ACCESS] +=
            vectorTransaction ? 1 : access.repeat_count;
    }

    QemuSyncEvent envelope = *memoryBatchEnvelope_;
    envelope.memoryBatch.clear();
    const bool fusedEvent = (envelope.flags & MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH) != 0;
    envelope.flags &= ~MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH;
    (void)accountCpuTo(envelope,
                       fusedEvent && envelope.stopReason != MITTENS_SYNC_STOP_QUANTUM_END);
    if (scratchpadTimingCycle_ < startCycle)
    {
        output_.fatal(0, -1, "tile %u scratchpad batch timing went backwards\n",
                      static_cast<unsigned>(config_.tileId));
    }
    memoryBatchIndex_ = memoryBatchRecords_.size();
    clearMemoryBatchState();
    replacePending(std::move(envelope));
    scheduleCpuSyncEvent(scratchpadTimingCycle_ - startCycle);
}

void CpuExecutionController::scheduleNextMemoryBatchStep()
{
    if (!memoryBatchEnvelope_.has_value() || memoryBatchScratchpad_)
    {
        output_.fatal(0, -1, "tile %u attempted to advance an inactive memory batch\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (memoryBatchIndex_ < memoryBatchRecords_.size())
    {
        const MittensSyncMemoryAccess& access = memoryBatchRecords_[memoryBatchIndex_];
        memoryBatchGroupEndIndex_ = memoryBatchIndex_ + 1;
        if ((access.flags & MITTENS_SYNC_MEMORY_FLAG_WRITE) == 0)
        {
            std::uint32_t producedRegisterMask = access.destination_register_mask;
            while (memoryBatchGroupEndIndex_ < memoryBatchRecords_.size() &&
                   memoryBatchGroupEndIndex_ - memoryBatchIndex_ < config_.memoryLoadQueueEntries)
            {
                const MittensSyncMemoryAccess& previous =
                    memoryBatchRecords_[memoryBatchGroupEndIndex_ - 1];
                const MittensSyncMemoryAccess& next =
                    memoryBatchRecords_[memoryBatchGroupEndIndex_];
                const bool vectorFragment = sameDynamicMemoryInstruction(access, next);
                const bool independentScalar =
                    canExtendScalarLoadGroup(previous, next, producedRegisterMask);
                if (!vectorFragment && !independentScalar)
                {
                    break;
                }
                producedRegisterMask |= next.destination_register_mask;
                ++memoryBatchGroupEndIndex_;
            }
        }
        QemuSyncEvent event = *memoryBatchEnvelope_;
        event.instructionsExecuted = access.instructions_executed;
        event.vectorInstructionsExecuted = access.vector_instructions_executed;
        event.stopReason = MITTENS_SYNC_STOP_MEMORY_ACCESS;
        event.flags = MITTENS_SYNC_EVENT_FLAG_NONE;
        event.analogSequence = access.program_counter;
        event.executionId = access.return_address;
        event.memoryAddress = access.address;
        event.memorySize = access.size;
        event.memoryFlags = access.flags & MITTENS_SYNC_MEMORY_FLAG_WRITE;
        event.memoryBatch.clear();
        const std::uint64_t cycles = accountCpuTo(event, true);
        ++synchronizationStopCounts_[MITTENS_SYNC_STOP_MEMORY_ACCESS];
        replacePending(std::move(event));
        scheduleCpuSyncEvent(cycles);
        return;
    }

    QemuSyncEvent event = *memoryBatchEnvelope_;
    event.memoryBatch.clear();
    const bool fusedEvent = (event.flags & MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH) != 0;
    event.flags &= ~MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH;
    const std::uint64_t cycles =
        accountCpuTo(event, fusedEvent && event.stopReason != MITTENS_SYNC_STOP_QUANTUM_END);
    clearMemoryBatchState();
    replacePending(std::move(event));
    scheduleCpuSyncEvent(cycles);
}

void CpuExecutionController::advanceMemoryBatchAccess()
{
    if (!memoryBatchEnvelope_.has_value() || memoryBatchIndex_ >= memoryBatchRecords_.size())
    {
        output_.fatal(0, -1, "tile %u completed an access outside a memory batch\n",
                      static_cast<unsigned>(config_.tileId));
    }
    completeWait();
    pendingSyncEvent_.reset();
    ++memoryBatchIndex_;
    memoryBatchGroupEndIndex_ = 0;
    scheduleNextMemoryBatchStep();
}

void CpuExecutionController::advanceMemoryBatchGroup()
{
    if (!memoryBatchEnvelope_.has_value() || memoryBatchGroupEndIndex_ <= memoryBatchIndex_ ||
        memoryBatchGroupEndIndex_ > memoryBatchRecords_.size())
    {
        output_.fatal(0, -1, "tile %u completed an invalid memory request group\n",
                      static_cast<unsigned>(config_.tileId));
    }
    completeWait();
    pendingSyncEvent_.reset();
    memoryBatchIndex_ = memoryBatchGroupEndIndex_;
    memoryBatchGroupEndIndex_ = 0;
    scheduleNextMemoryBatchStep();
}

void CpuExecutionController::beginGlobalDMASubmitBatch(const QemuSyncEvent& event)
{
    const bool fusedWait = event.stopReason == MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH;
    if (!config_.globalDMASubmitBatching || globalDMASubmitBatchEnvelope_.has_value() ||
        memoryBatchEnvelope_.has_value() ||
        (event.stopReason != MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT_BATCH && !fusedWait) ||
        event.globalDMASubmitBatch.empty() ||
        event.globalDMASubmitBatch.size() > MITTENS_SYNC_GLOBAL_DMA_BATCH_CAPACITY ||
        !event.memoryBatch.empty() ||
        (fusedWait ? event.flags != MITTENS_SYNC_EVENT_FLAG_GLOBAL_DMA_SUBMITS
                   : (event.flags & ~MITTENS_SYNC_EVENT_FLAG_QUANTUM_END) != 0))
    {
        output_.fatal(0, -1, "tile %u received an invalid or nested global DMA submit batch\n",
                      static_cast<unsigned>(config_.tileId));
    }

    std::uint64_t previousInstructions = ledger_.snapshot().baseline.instructions;
    std::uint64_t previousVectors = ledger_.snapshot().baseline.vectors;
    for (const MittensSyncGlobalDMASubmit& submit : event.globalDMASubmitBatch)
    {
        if (submit.instructions_executed < previousInstructions ||
            submit.instructions_executed > event.instructionsExecuted ||
            submit.vector_instructions_executed < previousVectors ||
            submit.vector_instructions_executed > submit.instructions_executed ||
            submit.vector_instructions_executed > event.vectorInstructionsExecuted)
        {
            output_.fatal(0, -1,
                          "tile %u received a non-monotonic global DMA submit "
                          "batch record\n",
                          static_cast<unsigned>(config_.tileId));
        }
        previousInstructions = submit.instructions_executed;
        previousVectors = submit.vector_instructions_executed;
    }

    ++synchronizationEvents_;
    ++synchronizationStopCounts_[event.stopReason];
    globalDMASubmitBatchTransportRecords_ += event.globalDMASubmitBatch.size();
    if (fusedWait)
    {
        globalDMAWaitBatchTransportRecords_ += event.globalDMASubmitBatch.size();
    }
    globalDMASubmitBatchEnvelope_ = event;
    globalDMASubmitBatchRecords_ = event.globalDMASubmitBatch;
    globalDMASubmitBatchIndex_ = 0;
    scheduleNextGlobalDMASubmitBatchStep();
    host_.watchdog();
}

void CpuExecutionController::clearGlobalDMASubmitBatchState()
{
    globalDMASubmitBatchEnvelope_.reset();
    globalDMASubmitBatchRecords_.clear();
    globalDMASubmitBatchIndex_ = 0;
}

void CpuExecutionController::scheduleNextGlobalDMASubmitBatchStep()
{
    if (!globalDMASubmitBatchEnvelope_.has_value())
    {
        output_.fatal(0, -1, "tile %u attempted to replay a missing global DMA batch\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (globalDMASubmitBatchIndex_ < globalDMASubmitBatchRecords_.size())
    {
        const MittensSyncGlobalDMASubmit& submit =
            globalDMASubmitBatchRecords_[globalDMASubmitBatchIndex_];
        QemuSyncEvent replay = *globalDMASubmitBatchEnvelope_;
        replay.instructionsExecuted = submit.instructions_executed;
        replay.vectorInstructionsExecuted = submit.vector_instructions_executed;
        replay.stopReason = MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT;
        replay.flags = MITTENS_SYNC_EVENT_FLAG_NONE;
        replay.analogArrayId = submit.direction;
        replay.analogSequence = submit.global_offset;
        replay.taskId = submit.token_id;
        replay.executionId = submit.execution_id;
        replay.memoryAddress = submit.scratchpad_offset;
        replay.memorySize = submit.byte_count;
        replay.memoryFlags = MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD;
        replay.globalDMALogicalIteration = submit.logical_iteration;
        replay.globalDMAScratchpadOffset = submit.scratchpad_offset;
        replay.globalDMARequestFlags = submit.request_flags;
        replay.memoryBatch.clear();
        replay.globalDMASubmitBatch.clear();
        const std::uint64_t cycles = accountCpuTo(replay, true);
        ++synchronizationStopCounts_[MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT];
        replacePending(std::move(replay));
        scheduleCpuSyncEvent(cycles);
        return;
    }

    QemuSyncEvent envelope = *globalDMASubmitBatchEnvelope_;
    const bool fusedWait = envelope.stopReason == MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH;
    if (!fusedWait)
    {
        envelope.globalDMASubmitBatch.clear();
    }
    envelope.flags &= ~MITTENS_SYNC_EVENT_FLAG_GLOBAL_DMA_SUBMITS;
    const std::uint64_t cycles = accountCpuTo(envelope, false);
    clearGlobalDMASubmitBatchState();
    replacePending(std::move(envelope));
    scheduleCpuSyncEvent(cycles);
}

void CpuExecutionController::advanceGlobalDMASubmitBatch()
{
    if (!globalDMASubmitBatchEnvelope_.has_value() ||
        globalDMASubmitBatchIndex_ >= globalDMASubmitBatchRecords_.size())
    {
        output_.fatal(0, -1, "tile %u completed a submit outside a global DMA batch\n",
                      static_cast<unsigned>(config_.tileId));
    }
    completeWait();
    pendingSyncEvent_.reset();
    ++globalDMASubmitBatchIndex_;
    scheduleNextGlobalDMASubmitBatchStep();
}

void CpuExecutionController::beginGlobalDMAMacroRun(const QemuSyncEvent& event)
{
    if (!config_.globalDMAMacroExecution || globalDMAMacroRunEnvelope_.has_value() ||
        globalDMASubmitBatchEnvelope_.has_value() || analogSubmitBatchEnvelope_.has_value() ||
        memoryBatchEnvelope_.has_value() || event.flags != MITTENS_SYNC_EVENT_FLAG_NONE ||
        event.globalDMASubmitBatch.size() < 2 ||
        event.globalDMASubmitBatch.size() > MITTENS_SYNC_GLOBAL_DMA_BATCH_CAPACITY ||
        !event.memoryBatch.empty() || !event.analogSubmitBatch.empty())
    {
        output_.fatal(0, -1, "tile %u received an invalid or nested global DMA macro run\n",
                      static_cast<unsigned>(config_.tileId));
    }

    const std::size_t eventCount = event.globalDMASubmitBatch.size() * 2U;
    const std::size_t invalidRecord = std::numeric_limits<std::size_t>::max();
    std::vector<GlobalDMAMacroEndpoint> endpoints(eventCount,
                                                  GlobalDMAMacroEndpoint{invalidRecord, false});
    for (std::size_t recordIndex = 0; recordIndex < event.globalDMASubmitBatch.size();
         ++recordIndex)
    {
        const MittensSyncGlobalDMASubmit& record = event.globalDMASubmitBatch[recordIndex];
        if (record.execution_id != event.executionId ||
            record.vector_instructions_executed > record.instructions_executed ||
            record.wait_instructions_executed < record.instructions_executed ||
            record.wait_vector_instructions_executed < record.vector_instructions_executed ||
            record.wait_vector_instructions_executed > record.wait_instructions_executed ||
            record.wait_instructions_executed > event.instructionsExecuted ||
            record.wait_vector_instructions_executed > event.vectorInstructionsExecuted ||
            record.submit_event_ordinal >= eventCount || record.wait_event_ordinal >= eventCount ||
            record.submit_event_ordinal >= record.wait_event_ordinal ||
            endpoints[record.submit_event_ordinal].recordIndex != invalidRecord ||
            endpoints[record.wait_event_ordinal].recordIndex != invalidRecord)
        {
            output_.fatal(0, -1,
                          "tile %u received an invalid or ambiguous global DMA "
                          "macro record\n",
                          static_cast<unsigned>(config_.tileId));
        }
        endpoints[record.submit_event_ordinal] = GlobalDMAMacroEndpoint{recordIndex, false};
        endpoints[record.wait_event_ordinal] = GlobalDMAMacroEndpoint{recordIndex, true};
    }

    std::uint64_t previousInstructions = ledger_.snapshot().baseline.instructions;
    std::uint64_t previousVectors = ledger_.snapshot().baseline.vectors;
    std::unordered_set<std::uint32_t> activeTokens;
    for (const GlobalDMAMacroEndpoint& endpoint : endpoints)
    {
        if (endpoint.recordIndex == invalidRecord ||
            endpoint.recordIndex >= event.globalDMASubmitBatch.size())
        {
            output_.fatal(0, -1, "tile %u received an incomplete global DMA macro event tape\n",
                          static_cast<unsigned>(config_.tileId));
        }
        const MittensSyncGlobalDMASubmit& record = event.globalDMASubmitBatch[endpoint.recordIndex];
        const std::uint64_t instructions =
            endpoint.wait ? record.wait_instructions_executed : record.instructions_executed;
        const std::uint64_t vectors = endpoint.wait ? record.wait_vector_instructions_executed
                                                    : record.vector_instructions_executed;
        const bool validLifecycle = endpoint.wait ? activeTokens.erase(record.token_id) == 1
                                                  : activeTokens.insert(record.token_id).second;
        if (!validLifecycle || instructions < previousInstructions || vectors < previousVectors ||
            vectors > instructions || instructions > event.instructionsExecuted ||
            vectors > event.vectorInstructionsExecuted)
        {
            output_.fatal(0, -1,
                          "tile %u received a non-monotonic global DMA macro event "
                          "tape\n",
                          static_cast<unsigned>(config_.tileId));
        }
        previousInstructions = instructions;
        previousVectors = vectors;
    }
    if (!activeTokens.empty() || event.instructionsExecuted < previousInstructions ||
        event.vectorInstructionsExecuted < previousVectors ||
        event.vectorInstructionsExecuted > event.instructionsExecuted ||
        event.globalDMASubmitBatch.size() > (UINT64_MAX - synchronizationEvents_) / 2U ||
        event.globalDMASubmitBatch.size() > UINT64_MAX - globalDMAMacroRunTransportRecords_)
    {
        output_.fatal(0, -1, "tile %u global DMA macro accounting overflowed or ended early\n",
                      static_cast<unsigned>(config_.tileId));
    }

    synchronizationEvents_ += static_cast<std::uint64_t>(event.globalDMASubmitBatch.size()) * 2U;
    globalDMAMacroRunTransportRecords_ += event.globalDMASubmitBatch.size();
    globalDMAMacroRunEnvelope_ = event;
    globalDMAMacroRunRecords_ = event.globalDMASubmitBatch;
    globalDMAMacroRunEndpoints_ = std::move(endpoints);
    globalDMAMacroRunIndex_ = 0;
    scheduleNextGlobalDMAMacroEvent();
    host_.watchdog();
}

void CpuExecutionController::clearGlobalDMAMacroRunState()
{
    globalDMAMacroRunEnvelope_.reset();
    globalDMAMacroRunRecords_.clear();
    globalDMAMacroRunEndpoints_.clear();
    globalDMAMacroRunIndex_ = 0;
}

void CpuExecutionController::scheduleNextGlobalDMAMacroEvent()
{
    if (!globalDMAMacroRunEnvelope_.has_value())
    {
        output_.fatal(0, -1,
                      "tile %u attempted to replay an invalid global DMA macro "
                      "state\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (globalDMAMacroRunIndex_ >= globalDMAMacroRunEndpoints_.size())
    {
        QemuSyncEvent envelope = *globalDMAMacroRunEnvelope_;
        envelope.globalDMASubmitBatch.clear();

        /*
         * The envelope is a host-transport terminal, not an architectural
         * CPU boundary.  Scalar execution resumes immediately after the last
         * DMA wait and charges the instructions through the delimiter to the
         * next real event.  Leave ledger_.snapshot().baseline.instructions at that same wait so
         * the next captured event reproduces the scalar accounting exactly.
         */
        clearGlobalDMAMacroRunState();
        replacePending(std::move(envelope));
        scheduleCpuSyncEvent(0);
        return;
    }

    const GlobalDMAMacroEndpoint& endpoint = globalDMAMacroRunEndpoints_[globalDMAMacroRunIndex_];
    if (endpoint.recordIndex >= globalDMAMacroRunRecords_.size())
    {
        output_.fatal(0, -1, "tile %u reached an invalid global DMA macro endpoint\n",
                      static_cast<unsigned>(config_.tileId));
    }
    const MittensSyncGlobalDMASubmit& record = globalDMAMacroRunRecords_[endpoint.recordIndex];
    QemuSyncEvent replay = *globalDMAMacroRunEnvelope_;
    replay.instructionsExecuted =
        endpoint.wait ? record.wait_instructions_executed : record.instructions_executed;
    replay.vectorInstructionsExecuted = endpoint.wait ? record.wait_vector_instructions_executed
                                                      : record.vector_instructions_executed;
    replay.stopReason = endpoint.wait ? MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT
                                      : MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT;
    replay.flags = MITTENS_SYNC_EVENT_FLAG_NONE;
    replay.analogArrayId = record.direction;
    replay.analogSequence = record.global_offset;
    replay.taskId = record.token_id;
    replay.executionId = record.execution_id;
    replay.memoryAddress = record.scratchpad_offset;
    replay.memorySize = record.byte_count;
    replay.memoryFlags = MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD;
    replay.globalDMALogicalIteration = record.logical_iteration;
    replay.globalDMAScratchpadOffset = record.scratchpad_offset;
    replay.globalDMARequestFlags = record.request_flags;
    replay.memoryBatch.clear();
    replay.globalDMASubmitBatch.clear();
    replay.analogSubmitBatch.clear();
    const std::uint64_t cycles = accountCpuTo(replay, true);
    ++synchronizationStopCounts_[replay.stopReason];
    replacePending(std::move(replay));
    scheduleCpuSyncEvent(cycles);
}

void CpuExecutionController::advanceGlobalDMAMacroRun()
{
    if (!globalDMAMacroRunEnvelope_.has_value() ||
        globalDMAMacroRunIndex_ >= globalDMAMacroRunEndpoints_.size() ||
        !pendingSyncEvent_.has_value())
    {
        output_.fatal(0, -1, "tile %u completed an event outside a global DMA macro run\n",
                      static_cast<unsigned>(config_.tileId));
    }
    const bool expectedWait = globalDMAMacroRunEndpoints_[globalDMAMacroRunIndex_].wait;
    const bool actualWait = pendingSyncEvent_->stopReason == MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT;
    if (expectedWait != actualWait)
    {
        output_.fatal(0, -1, "tile %u completed the wrong global DMA macro endpoint kind\n",
                      static_cast<unsigned>(config_.tileId));
    }
    completeWait();
    pendingSyncEvent_.reset();
    ++globalDMAMacroRunIndex_;
    scheduleNextGlobalDMAMacroEvent();
}

void CpuExecutionController::beginAnalogSubmitBatch(const QemuSyncEvent& event)
{
    try
    {
        validateAnalogSubmitBatch(event);
    }
    catch (const std::exception& error)
    {
        output_.fatal(0, -1, "tile %u received an invalid analog submit batch: %s\n",
                      static_cast<unsigned>(config_.tileId), error.what());
        return;
    }

    ++synchronizationEvents_;
    if (event.stopReason == MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH)
    {
        ++synchronizationStopCounts_[MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH];
    }
    analogSubmitBatchTransportRecords_ += event.analogSubmitBatch.size();
    analogSubmitBatchEnvelope_ = event;
    analogSubmitBatchRecords_ = event.analogSubmitBatch;
    analogSubmitBatchIndex_ = 0;
    scheduleNextAnalogSubmitBatchStep();
    host_.watchdog();
}

void CpuExecutionController::validateAnalogSubmitBatch(const QemuSyncEvent& event) const
{
    const bool standaloneBatch = event.stopReason == MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH;
    const bool fusedEvent = (event.flags & MITTENS_SYNC_EVENT_FLAG_ANALOG_BATCH) != 0;
    if (!config_.analogCommandBatching || analogSubmitBatchEnvelope_.has_value() ||
        memoryBatchEnvelope_.has_value() || globalDMASubmitBatchEnvelope_.has_value() ||
        event.analogSubmitBatch.empty() ||
        event.analogSubmitBatch.size() > MITTENS_SYNC_ANALOG_BATCH_CAPACITY ||
        !event.memoryBatch.empty() || !event.globalDMASubmitBatch.empty() ||
        standaloneBatch == fusedEvent ||
        (standaloneBatch && (event.flags & ~MITTENS_SYNC_EVENT_FLAG_QUANTUM_END) != 0))
    {
        throw std::runtime_error("invalid envelope or nested analog submit batch");
    }

    std::uint64_t previousInstructions = ledger_.snapshot().baseline.instructions;
    std::uint64_t previousVectors = ledger_.snapshot().baseline.vectors;
    std::vector<std::uint32_t> nextSequences(host_.analogArrayCount(), 0);
    std::vector<bool> sequenceSeen(host_.analogArrayCount(), false);
    for (const MittensSyncAnalogSubmit& submit : event.analogSubmitBatch)
    {
        if (submit.instructions_executed < previousInstructions ||
            submit.instructions_executed > event.instructionsExecuted ||
            submit.vector_instructions_executed < previousVectors ||
            submit.vector_instructions_executed > submit.instructions_executed ||
            submit.vector_instructions_executed > event.vectorInstructionsExecuted ||
            submit.array_id >= host_.analogArrayCount() || submit.sequence > UINT32_MAX ||
            submit.flags != 0 || !host_.analogSubmitted(submit.array_id, submit.sequence) ||
            (sequenceSeen[submit.array_id] && nextSequences[submit.array_id] != submit.sequence))
        {
            throw std::runtime_error("invalid analog submit batch record");
        }
        previousInstructions = submit.instructions_executed;
        previousVectors = submit.vector_instructions_executed;
        nextSequences[submit.array_id] = static_cast<std::uint32_t>(submit.sequence) + UINT32_C(1);
        sequenceSeen[submit.array_id] = true;
    }
}

void CpuExecutionController::clearAnalogSubmitBatchState()
{
    analogSubmitBatchEnvelope_.reset();
    analogSubmitBatchRecords_.clear();
    analogSubmitBatchIndex_ = 0;
}

void CpuExecutionController::scheduleNextAnalogSubmitBatchStep()
{
    if (!analogSubmitBatchEnvelope_.has_value())
    {
        output_.fatal(0, -1, "tile %u attempted to replay a missing analog submit batch\n",
                      static_cast<unsigned>(config_.tileId));
    }
    if (analogSubmitBatchIndex_ < analogSubmitBatchRecords_.size())
    {
        const MittensSyncAnalogSubmit& submit = analogSubmitBatchRecords_[analogSubmitBatchIndex_];
        QemuSyncEvent replay = *analogSubmitBatchEnvelope_;
        replay.instructionsExecuted = submit.instructions_executed;
        replay.vectorInstructionsExecuted = submit.vector_instructions_executed;
        replay.stopReason = MITTENS_SYNC_STOP_ANALOG_SUBMIT;
        replay.flags = MITTENS_SYNC_EVENT_FLAG_NONE;
        replay.analogArrayId = submit.array_id;
        replay.analogSequence = submit.sequence;
        replay.memoryBatch.clear();
        replay.globalDMASubmitBatch.clear();
        replay.analogSubmitBatch.clear();
        const std::uint64_t cycles = accountCpuTo(replay, true);
        ++synchronizationStopCounts_[MITTENS_SYNC_STOP_ANALOG_SUBMIT];
        replacePending(std::move(replay));
        scheduleCpuSyncEvent(cycles);
        return;
    }

    QemuSyncEvent envelope = *analogSubmitBatchEnvelope_;
    envelope.analogSubmitBatch.clear();
    envelope.flags &= ~MITTENS_SYNC_EVENT_FLAG_ANALOG_BATCH;
    const bool fusedEvent = envelope.stopReason != MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH;
    const std::uint64_t cycles =
        accountCpuTo(envelope, fusedEvent && envelope.stopReason != MITTENS_SYNC_STOP_QUANTUM_END);
    clearAnalogSubmitBatchState();
    replacePending(std::move(envelope));
    if (fusedEvent)
    {
        if (pendingSyncEvent_->stopReason > MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH)
        {
            output_.fatal(0, -1, "tile %u received an invalid analog batch terminal stop %u\n",
                          static_cast<unsigned>(config_.tileId),
                          static_cast<unsigned>(pendingSyncEvent_->stopReason));
        }
        ++synchronizationStopCounts_[pendingSyncEvent_->stopReason];
    }
    scheduleCpuSyncEvent(cycles);
}

void CpuExecutionController::advanceAnalogSubmitBatch()
{
    if (!analogSubmitBatchEnvelope_.has_value() ||
        analogSubmitBatchIndex_ >= analogSubmitBatchRecords_.size())
    {
        output_.fatal(0, -1, "tile %u completed a submit outside an analog batch\n",
                      static_cast<unsigned>(config_.tileId));
    }
    completeWait();
    pendingSyncEvent_.reset();
    ++analogSubmitBatchIndex_;
    scheduleNextAnalogSubmitBatchStep();
}

void CpuExecutionController::beginWait(const QemuSyncEvent& event)
{
    if (activeWaitStartTick_.has_value())
    {
        if (activeWaitEventSequence_ == event.eventSequence &&
            activeWaitReason_ == event.stopReason)
        {
            return;
        }
        output_.fatal(0, -1,
                      "tile %u began fd 41 wait event %llu before completing event "
                      "%llu\n",
                      static_cast<unsigned>(config_.tileId),
                      static_cast<unsigned long long>(event.eventSequence),
                      static_cast<unsigned long long>(activeWaitEventSequence_));
    }
    activeWaitStartTick_ = host_.now().value;
    activeWaitReason_ = event.stopReason;
    activeWaitEventSequence_ = event.eventSequence;
}

void CpuExecutionController::completeWait()
{
    if (!activeWaitStartTick_.has_value())
    {
        return;
    }
    const std::uint64_t finishTick = host_.now().value;
    const std::uint64_t duration =
        finishTick >= *activeWaitStartTick_ ? finishTick - *activeWaitStartTick_ : 0;
    if (activeWaitReason_ < waitTicks_.size())
    {
        waitTicks_[activeWaitReason_] += duration;
    }
    host_.recordWait(*activeWaitStartTick_, finishTick, syncStopReasonName(activeWaitReason_),
                     activeWaitEventSequence_);
    activeWaitStartTick_.reset();
    activeWaitReason_ = MITTENS_SYNC_STOP_NONE;
    activeWaitEventSequence_ = 0;
}

bool CpuExecutionController::waitIsProfiled(std::uint32_t reason) const noexcept
{
    switch (reason)
    {
    case MITTENS_SYNC_STOP_NIC_TRANSMIT:
    case MITTENS_SYNC_STOP_NIC_TRANSMIT_WAIT:
    case MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT:
    case MITTENS_SYNC_STOP_ANALOG_SUBMIT:
    case MITTENS_SYNC_STOP_ANALOG_WAIT:
    case MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT:
    case MITTENS_SYNC_STOP_NIC_RX_SOFTWARE_CLAIM:
    case MITTENS_SYNC_STOP_MEMORY_ACCESS:
    case MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE:
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT:
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT:
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH:
    case MITTENS_SYNC_STOP_EPOCH_BARRIER_ARRIVE:
        return true;
    default:
        return false;
    }
}

void CpuExecutionController::scheduleCpuSyncEvent(std::uint64_t cycles)
{
    if (!host_.running())
        return;
    pendingReadyTick_ = cpuDomain().after(host_.now(), {cycles});
    nextWakeGeneration_ = Timing::add(nextWakeGeneration_, 1);
    scheduledWakeGeneration_ = nextWakeGeneration_;
    cancelProgressWatchdogEvent();
    cpuSyncWakeScheduledDuringHandler_ = true;
    host_.schedule({cycles}, pendingSyncEvent_ ? pendingSyncEvent_->stopReason : UINT32_MAX - 3U,
                   *scheduledWakeGeneration_);
}

void CpuExecutionController::scheduleProgressWatchdogEvent()
{
    if (!host_.running() || config_.progressWatchdogMilliseconds == 0 || host_.watchdogReported())
        return;
    constexpr std::uint64_t cycles = 1000000;
    const auto candidate = cpuDomain().after(host_.now(), {cycles}).value;
    if (progressWatchdogWakeTick_ && candidate >= *progressWatchdogWakeTick_)
        return;
    progressWatchdogWakeGeneration_ = Timing::add(progressWatchdogWakeGeneration_, 1);
    progressWatchdogWakeTick_ = candidate;
    host_.scheduleWatchdog({cycles}, progressWatchdogWakeGeneration_);
}

void CpuExecutionController::cancelProgressWatchdogEvent()
{
    progressWatchdogWakeTick_.reset();
}

std::uint64_t CpuExecutionController::accountCpuTo(std::uint64_t instructions,
                                                   std::uint64_t vectors, bool boundary)
{
    std::uint64_t cycles;
    try {
        cycles = ledger_.accountTo({instructions, vectors}, boundary).cycles.value;
    } catch (const std::runtime_error& error) {
        std::ostringstream detail;
        detail << error.what();
        if (memoryBatchEnvelope_) {
            detail << " batch envelope=" << memoryBatchEnvelope_->instructionsExecuted
                   << '/' << memoryBatchEnvelope_->vectorInstructionsExecuted
                   << " reason=" << memoryBatchEnvelope_->stopReason;
            for (const auto& access : memoryBatchRecords_) {
                detail << " [pc=" << std::hex << access.program_counter << std::dec
                       << " counts=" << access.instructions_executed << '/'
                       << access.vector_instructions_executed << ']';
            }
        }
        throw std::runtime_error(detail.str());
    }
    scratchpadTimingCycle_ = Timing::add(scratchpadTimingCycle_, cycles);
    return cycles;
}
const char* syncStopReasonName(std::uint32_t reason)
{
    switch (reason)
    {
    case MITTENS_SYNC_STOP_NONE:
        return "none";
    case MITTENS_SYNC_STOP_QUANTUM_END:
        return "quantum-end";
    case MITTENS_SYNC_STOP_NIC_TRANSMIT:
        return "nic-transmit";
    case MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT:
        return "nic-receive-wait";
    case MITTENS_SYNC_STOP_ANALOG_SUBMIT:
        return "analog-submit";
    case MITTENS_SYNC_STOP_ANALOG_WAIT:
        return "analog-wait";
    case MITTENS_SYNC_STOP_GUEST_EXIT:
        return "guest-exit";
    case MITTENS_SYNC_STOP_TASK_START:
        return "task-start";
    case MITTENS_SYNC_STOP_TASK_FINISH:
        return "task-finish";
    case MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT:
        return "nic-rx-dma-submit";
    case MITTENS_SYNC_STOP_MEMORY_ACCESS:
        return "memory-access";
    case MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE:
        return "memory-init-complete";
    case MITTENS_SYNC_STOP_NIC_TRANSMIT_WAIT:
        return "nic-transmit-wait";
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT:
        return "scratchpad-dma-submit";
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT:
        return "scratchpad-dma-wait";
    case MITTENS_SYNC_STOP_MEMORY_BATCH:
        return "memory-batch";
    case MITTENS_SYNC_STOP_MEMORY_FENCE:
        return "memory-fence";
    case MITTENS_SYNC_STOP_NIC_RX_SOFTWARE_CLAIM:
        return "nic-rx-software-claim";
    case MITTENS_SYNC_STOP_EPOCH_BARRIER_ARRIVE:
        return "epoch-barrier-arrive";
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT_BATCH:
        return "scratchpad-dma-submit-batch";
    case MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH:
        return "analog-submit-batch";
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH:
        return "scratchpad-dma-wait-batch";
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO:
        return "scratchpad-dma-macro";
    default:
        return "unknown";
    }
}

CpuExecutionController::CpuExecutionController(TileConfiguration config,
                                               Timing::Clock<Timing::Cpu> clock,
                                               Transport transport, Host host,
                                               std::uint64_t partition, std::uint32_t workers)
    : config_(std::move(config)), clock_(clock), transport_(std::move(transport)),
      host_(std::move(host)), output_{host_.log}, ledger_(config_.cpuIssueWidth)
{
    QemuCaptureCoordinator::attach(config_.tileId, captureLease_);
    qemuReadySetPartitionKey_ = partition;
    qemuReadySetPartitionWorkers_ = workers;
}
CpuExecutionController::~CpuExecutionController()
{
    stop();
}
CpuExecutionController::Statistics CpuExecutionController::statistics() const
{
    Statistics result;
    result.synchronizationGrants_ = synchronizationGrants_;
    result.synchronizationEvents_ = synchronizationEvents_;
    result.synchronizationStopCounts_ = synchronizationStopCounts_;
    result.analogSubmitBatchTransportRecords_ = analogSubmitBatchTransportRecords_;
    result.memoryBatchTransportRecords_ = memoryBatchTransportRecords_;
    result.memoryBatchLogicalAccesses_ = memoryBatchLogicalAccesses_;
    result.globalDMASubmitBatchTransportRecords_ = globalDMASubmitBatchTransportRecords_;
    result.globalDMAWaitBatchTransportRecords_ = globalDMAWaitBatchTransportRecords_;
    result.globalDMAMacroRunTransportRecords_ = globalDMAMacroRunTransportRecords_;
    result.taskFinishEvents_ = taskFinishEvents_;
    result.waitTicks_ = waitTicks_;
    result.activeWaitStartTick_ = activeWaitStartTick_;
    result.activeWaitReason_ = activeWaitReason_;
    result.activeWaitEventSequence_ = activeWaitEventSequence_;
    return result;
}
std::optional<QemuSyncEvent> CpuExecutionController::pending() const
{
    if (!pendingSyncEvent_)
        return std::nullopt;
    QemuSyncEvent result{};
    result.grantEpoch = pendingSyncEvent_->grantEpoch;
    result.eventSequence = pendingSyncEvent_->eventSequence;
    result.instructionsExecuted = pendingSyncEvent_->instructionsExecuted;
    result.vectorInstructionsExecuted = pendingSyncEvent_->vectorInstructionsExecuted;
    result.stopReason = pendingSyncEvent_->stopReason;
    result.flags = pendingSyncEvent_->flags;
    result.analogArrayId = pendingSyncEvent_->analogArrayId;
    result.analogSequence = pendingSyncEvent_->analogSequence;
    result.taskId = pendingSyncEvent_->taskId;
    result.executionId = pendingSyncEvent_->executionId;
    result.receiveDMASource = pendingSyncEvent_->receiveDMASource;
    result.receiveDMARouteId = pendingSyncEvent_->receiveDMARouteId;
    result.receiveDMALogicalIteration = pendingSyncEvent_->receiveDMALogicalIteration;
    result.receiveDMAWordCount = pendingSyncEvent_->receiveDMAWordCount;
    result.memoryAddress = pendingSyncEvent_->memoryAddress;
    result.memorySize = pendingSyncEvent_->memorySize;
    result.memoryFlags = pendingSyncEvent_->memoryFlags;
    result.globalDMALogicalIteration = pendingSyncEvent_->globalDMALogicalIteration;
    result.globalDMAScratchpadOffset = pendingSyncEvent_->globalDMAScratchpadOffset;
    result.globalDMARequestFlags = pendingSyncEvent_->globalDMARequestFlags;
    result.epochId = pendingSyncEvent_->epochId;
    result.epochContribution = pendingSyncEvent_->epochContribution;
    return result;
}
bool CpuExecutionController::prepareDeferredLookahead(const QemuSyncEvent& event)
{
    if (!host_.prepareDeferredAnalog(event))
        return false;
    startLocalQemuLookahead(event);
    return true;
}
void CpuExecutionController::onWake(std::optional<std::uint64_t> watchdog,
                                    std::optional<std::uint64_t> scheduled)
{
    if (stopped_ || !captureLease_->load())
        return;
    if (scheduled && (!scheduledWakeGeneration_ || *scheduled != *scheduledWakeGeneration_ ||
                      !pendingDelayElapsed()))
        return;
    if (watchdog)
    {
        if (*watchdog != progressWatchdogWakeGeneration_ || !progressWatchdogWakeTick_ ||
            *progressWatchdogWakeTick_ != host_.now().value)
            return;
        progressWatchdogWakeTick_.reset();
    }
    cpuSyncWakeScheduledDuringHandler_ = false;
    if (!host_.running())
    {
        cancelProgressWatchdogEvent();
        return;
    }
    if (runtimeQemuReadySetCapturePending_ || initialCapturePending_)
        return;
    if (pendingSyncEvent_)
    {
        const bool resumed = processPendingSyncEvent();
        if (resumed)
            cancelProgressWatchdogEvent();
        if (!resumed && pendingSyncEvent_)
        {
            host_.watchdog();
            if (host_.running() && pendingSyncEvent_ && !cpuSyncWakeScheduledDuringHandler_)
                scheduleProgressWatchdogEvent();
        }
        return;
    }
    if (config_.qemuReadySetWorkers > 1 && host_.initializing() && synchronizationGrants_ == 0)
    {
        submitInitialQemuReadySetGrant();
        return;
    }
    grantAndCaptureQemu();
}
void CpuExecutionController::dispatchCaptures()
{
    if (!host_.running() || !config_.qemuRuntimeReadySet)
        return;
    const auto completions = QemuCaptureCoordinator::dispatchRuntimeQemuReadySet(host_.now().value);
    for (const auto& completion : completions)
        completion.commit(completion.event);
}
void CpuExecutionController::applyResult(const CpuDeviceResult& result)
{
    if ((result.complete && result.delay) ||
        (result.reportBlockedAfterCompletion && !result.complete))
        throw std::logic_error("inconsistent CPU device completion/delay result");
    if (result.cursor)
        scratchpadTimingCycle_ = result.cursor->value;
    if (result.delay)
        scheduleCpuSyncEvent(result.delay->value);
}
void CpuExecutionController::completeMemory(std::uint64_t step, bool group)
{
    requireStep(step);
    if (!pendingDelayElapsed())
        return;
    if (!pendingSyncEvent_ || pendingSyncEvent_->stopReason != MITTENS_SYNC_STOP_MEMORY_ACCESS)
        throw std::logic_error("memory completion has no pending CPU access");
    if (memoryBatchEnvelope_)
    {
        if (group)
            advanceMemoryBatchGroup();
        else
            advanceMemoryBatchAccess();
    }
    else
        resumeAndCaptureQemu();
}
void CpuExecutionController::completeDevice(std::uint64_t step)
{
    requireStep(step);
    if (!pendingDelayElapsed())
        return;
    if (!pendingSyncEvent_)
        throw std::logic_error("device completion has no pending CPU event");
    const auto reason = pendingSyncEvent_->stopReason;
    if (reason == MITTENS_SYNC_STOP_MEMORY_ACCESS)
    {
        completeMemory(step);
        return;
    }
    if (analogSubmitBatchEnvelope_ && reason == MITTENS_SYNC_STOP_ANALOG_SUBMIT)
        advanceAnalogSubmitBatch();
    else if (globalDMAMacroRunEnvelope_)
        advanceGlobalDMAMacroRun();
    else if (globalDMASubmitBatchEnvelope_ && reason == MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT)
        advanceGlobalDMASubmitBatch();
    else
        resumeAndCaptureQemu();
}
void CpuExecutionController::resumeTransmitWaitIfReady()
{
    if (transmitWaitArmed_ && pendingSyncEvent_ &&
        pendingSyncEvent_->stopReason == MITTENS_SYNC_STOP_NIC_TRANSMIT_WAIT &&
        (host_.transmitReady((pendingSyncEvent_->flags & MITTENS_SYNC_EVENT_FLAG_NIC_BURST) != 0) ||
         host_.receiveReady()))
        resumeAndCaptureQemu();
}
void CpuExecutionController::resumeReceiveWaitIfReady()
{
    if (receiveWaitArmed_ && pendingSyncEvent_ &&
        pendingSyncEvent_->stopReason == MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT && host_.receiveReady())
        resumeAndCaptureQemu();
}
void CpuExecutionController::stop()
{
    if (stopped_)
        return;
    stopped_ = true;
    pendingReadyTick_.reset();
    scheduledWakeGeneration_.reset();
    captureLease_->store(false);
    QemuCaptureCoordinator::cancel(config_.tileId);
    if (localQemuLookaheadFuture_)
    {
        localQemuLookaheadFuture_->wait();
        localQemuLookaheadFuture_.reset();
    }
    initialCapturePending_ = false;
    runtimeQemuReadySetCapturePending_ = false;
    localQemuLookaheadSourceSequence_ = 0;
    localQemuLookaheadSourceReason_ = MITTENS_SYNC_STOP_NONE;
    completeWait();
    pendingSyncEvent_.reset();
    receiveWaitArmed_ = false;
    transmitWaitArmed_ = false;
    clearMemoryBatchState();
    clearAnalogSubmitBatchState();
    clearGlobalDMASubmitBatchState();
    clearGlobalDMAMacroRunState();
    cancelProgressWatchdogEvent();
}
void CpuExecutionController::assertDrained() const
{
    if (localQemuLookaheadFuture_ || initialCapturePending_ || runtimeQemuReadySetCapturePending_)
        throw std::logic_error("unconsumed QEMU capture at shutdown");
}
bool CpuExecutionController::processPendingSyncEvent()
{
    if (!pendingSyncEvent_)
        return true;
    if (stopped_)
        return true;
    if (!pendingDelayElapsed())
        return false;
    pendingReadyTick_.reset();
    scheduledWakeGeneration_.reset();
    const auto step = stepId_;
    const auto event = *pendingSyncEvent_;
    const auto reason = event.stopReason;
    if (waitIsProfiled(reason))
        beginWait(event);
    const bool drainStores =
        reason == MITTENS_SYNC_STOP_NIC_TRANSMIT || reason == MITTENS_SYNC_STOP_NIC_TRANSMIT_WAIT ||
        reason == MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT ||
        reason == MITTENS_SYNC_STOP_ANALOG_SUBMIT || reason == MITTENS_SYNC_STOP_ANALOG_WAIT ||
        reason == MITTENS_SYNC_STOP_TASK_FINISH || reason == MITTENS_SYNC_STOP_MEMORY_FENCE ||
        reason == MITTENS_SYNC_STOP_EPOCH_BARRIER_ARRIVE || reason == MITTENS_SYNC_STOP_GUEST_EXIT;
    if (drainStores && !host_.storesDrained())
        return false;
    CpuDeviceResult result;
    switch (reason)
    {
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO:
        if (event.flags != MITTENS_SYNC_EVENT_FLAG_NONE || !event.memoryBatch.empty() ||
            !event.globalDMASubmitBatch.empty() || !event.analogSubmitBatch.empty() ||
            globalDMAMacroRunEnvelope_)
            throw std::runtime_error("invalid DMA macro terminal");
        resumeAndCaptureQemu();
        return true;
    case MITTENS_SYNC_STOP_QUANTUM_END:
    case MITTENS_SYNC_STOP_MEMORY_BATCH:
    case MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH:
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT_BATCH:
        if (reason == MITTENS_SYNC_STOP_QUANTUM_END ||
            (event.flags & MITTENS_SYNC_EVENT_FLAG_QUANTUM_END))
        {
            host_.serviceBridge();
            if (reason == MITTENS_SYNC_STOP_MEMORY_BATCH && localQemuLookaheadFuture_)
            {
                completeWait();
                consumeLocalQemuLookahead(event);
                return true;
            }
            pendingSyncEvent_.reset();
            grantAndCaptureQemu();
        }
        else
            resumeAndCaptureQemu();
        return true;
    case MITTENS_SYNC_STOP_MEMORY_FENCE:
        resumeAndCaptureQemu();
        return true;
    case MITTENS_SYNC_STOP_NIC_TRANSMIT_WAIT:
    case MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT:
        host_.serviceBridge();
        if (host_.receiveReady() ||
            (reason == MITTENS_SYNC_STOP_NIC_TRANSMIT_WAIT &&
             host_.transmitReady((event.flags & MITTENS_SYNC_EVENT_FLAG_NIC_BURST) != 0)))
        {
            resumeAndCaptureQemu();
            return true;
        }
        if (reason == MITTENS_SYNC_STOP_NIC_TRANSMIT_WAIT)
            transmitWaitArmed_ = true;
        else
            receiveWaitArmed_ = true;
        return false;
    case MITTENS_SYNC_STOP_NIC_TRANSMIT:
    case MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT:
    case MITTENS_SYNC_STOP_NIC_RX_SOFTWARE_CLAIM:
        result = dispatchDevice(
            host_.network, CpuNetworkAction{reason, event.flags, event.receiveDMASource,
                                            event.receiveDMARouteId, event.receiveDMAWordCount,
                                            event.receiveDMALogicalIteration, event.memoryAddress});
        break;
    case MITTENS_SYNC_STOP_ANALOG_SUBMIT:
    case MITTENS_SYNC_STOP_ANALOG_WAIT:
        result = dispatchDevice(host_.analog, CpuAnalogAction{reason, event.analogArrayId,
                                                              event.flags, event.analogSequence});
        break;
    case MITTENS_SYNC_STOP_MEMORY_ACCESS:
    {
        CpuMemoryAction action{event.memoryAddress,         event.memoryProgramCounter(),
                               event.memoryReturnAddress(), event.memorySize,
                               event.memoryFlags,           {},
                               {scratchpadTimingCycle_}};
        action.step = step;
        if (memoryBatchEnvelope_ && memoryBatchGroupEndIndex_ > memoryBatchIndex_ + 1)
            action.group.assign(memoryBatchRecords_.begin() + memoryBatchIndex_,
                                memoryBatchRecords_.begin() + memoryBatchGroupEndIndex_);
        result = dispatchDevice(host_.memory, action);
        break;
    }
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT:
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT:
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH:
        result = dispatchDevice(host_.globalDMA,
                                CpuGlobalDMAAction{reason,
                                                   event.flags,
                                                   event.globalDMADirection(),
                                                   event.globalDMATokenId(),
                                                   event.globalDMAByteCount(),
                                                   event.globalDMARequestFlags,
                                                   event.executionId,
                                                   event.globalDMAOffset(),
                                                   event.globalDMAScratchpadOffsetValue(),
                                                   event.globalDMALogicalIteration,
                                                   event.globalDMASubmitBatch,
                                                   {scratchpadTimingCycle_}});
        break;
    case MITTENS_SYNC_STOP_EPOCH_BARRIER_ARRIVE:
        result = dispatchDevice(
            host_.barrier, CpuBarrierAction{event.epochId, event.epochContribution, event.flags});
        break;
    case MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE:
        result = dispatchDevice(host_.initialization,
                                CpuInitializationAction{event.memoryInitializationAccesses(),
                                                        event.memoryInitializationReadBytes(),
                                                        event.memoryInitializationWriteBytes(),
                                                        event.memorySize, event.memoryFlags});
        break;
    case MITTENS_SYNC_STOP_TASK_START:
    case MITTENS_SYNC_STOP_TASK_FINISH:
        result = dispatchDevice(host_.task,
                                CpuTaskAction{event.taskId, event.executionId, event.eventSequence,
                                              reason == MITTENS_SYNC_STOP_TASK_FINISH});
        break;
    case MITTENS_SYNC_STOP_GUEST_EXIT:
        host_.guestExit();
        return true;
    default:
        throw std::runtime_error("unsupported CPU stop reason");
    }
    applyResult(result);
    if (result.complete)
    {
        completeDevice(step);
        return !result.reportBlockedAfterCompletion;
    }
    return false;
}

void CpuExecutionController::requireStep(std::uint64_t step) const
{
    if (dispatching_)
        throw std::logic_error("device callback must return completion, not reenter CPU");
    if (stopped_ || !pendingSyncEvent_ || step == 0 || step != stepId_)
        throw std::logic_error("stale or repeated CPU device completion");
}

} // namespace SST::Mittens
