#include "analogController.h"
#include "nativeAnalogBackend.h"
#include "timingAnalogBackend.h"
#include "crossSimAnalogBackend.h"
namespace SST::Mittens
{
AnalogController::AnalogController(TileConfiguration config, Timing::Clock<Timing::Analog> clock,
                                   PerformanceProfile& profile, DeviceDiagnostics diagnostics,
                                   Host host)
    : config_(std::move(config)), clock_(clock), performanceProfile_(profile),
      output_(std::move(diagnostics)), host_(std::move(host))
{
    if (config_.analogArrayCount != 0)
    {
        std::unique_ptr<AnalogBackend> backend;
        try
        {
            if (config_.analogBackend == "native")
            {
                backend = std::make_unique<NativeAnalogBackend>(
                    config_.analogArrayCount, config_.analogArrayRows, config_.analogArrayColumns);
            }
            else if (config_.analogBackend == "timing")
            {
                backend = std::make_unique<TimingAnalogBackend>(
                    config_.analogArrayCount, config_.analogArrayRows, config_.analogArrayColumns);
            }
            else
            {
                backend = std::make_unique<CrossSimAnalogBackend>(
                    config_.analogArrayCount, config_.analogArrayRows, config_.analogArrayColumns,
                    config_.crossSimConfig);
            }
        }
        catch (const std::exception& error)
        {
            output_.fatal(

                -1, "tile %u failed to initialize analog_backend '%s': %s\n",
                static_cast<unsigned>(config_.tileId), config_.analogBackend.c_str(), error.what());
        }

        analogDevice_ = std::make_unique<AnalogDevice>(
            config_.tileId, config_.analogComputeLatencyCycles, std::move(backend));
        analogDevice_->setTraceEnabled(performanceProfile_.traceEnabled());

        analogLastUpdateCycle_ = clock_.floor(host_.now()).value;
    }
}
bool AnalogController::startDeferredAnalogLoadLookahead(const QemuSyncEvent& event)
{
    if (!config_.qemuLocalLookahead || event.stopReason != MITTENS_SYNC_STOP_ANALOG_SUBMIT ||
        event.flags != MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH ||
        event.analogArrayId >= analogBridge_.arrayCount() || event.analogSequence > UINT32_MAX ||
        deferredAnalogSubmission_.has_value())
    {
        return false;
    }

    const AnalogBridgeToken token{
        event.analogArrayId,
        static_cast<std::uint32_t>(event.analogSequence),
    };
    std::optional<AnalogBridgeSubmission> submission;
    try
    {
        submission = analogBridge_.nextSubmission(token.arrayId);
    }
    catch (const std::exception& error)
    {
        output_.fatal(

            -1, "tile %u could not inspect fused analog load: %s\n",
            static_cast<unsigned>(config_.tileId), error.what());
    }
    if (!submission.has_value() || submission->token.arrayId != token.arrayId ||
        submission->token.sequence != token.sequence ||
        submission->command.operation != MITTENS_ANALOG_OPERATION_LOAD_VECTOR)
    {
        return false;
    }

    try
    {
        deferredAnalogSubmission_.emplace(analogBridge_.takeAndAcceptSubmission(token));
    }
    catch (const std::exception& error)
    {
        output_.fatal(

            -1, "tile %u could not defer fused analog load: %s\n",
            static_cast<unsigned>(config_.tileId), error.what());
    }
    return true;
}

bool AnalogController::pendingAnalogEventReady() const
{
    if (!host_.pending().has_value() || !analogBridge_.open())
    {
        return false;
    }

    const QemuSyncEvent event = *host_.pending();
    const AnalogBridgeToken token{
        event.analogArrayId,
        static_cast<std::uint32_t>(event.analogSequence),
    };
    if (deferredAnalogSubmission_.has_value())
    {
        return false;
    }
    const std::uint32_t slotState = analogBridge_.slotState(token);
    const bool waitForCompletion =
        (event.flags & MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION) != 0 ||
        event.stopReason == MITTENS_SYNC_STOP_ANALOG_WAIT;

    if (waitForCompletion)
    {
        return slotState == MITTENS_ANALOG_SLOT_COMPLETED;
    }
    return slotState == MITTENS_ANALOG_SLOT_ACCEPTED || slotState == MITTENS_ANALOG_SLOT_COMPLETED;
}

void AnalogController::updateAnalogDeviceToCurrentCycle()
{
    if (analogDevice_ == nullptr)
    {
        return;
    }
    const std::uint64_t factor = clock_.factor();
    if (factor == 0)
    {
        throw std::logic_error("analog clock has a zero time factor");
    }
    const std::uint64_t currentCycle = analogDomain().floor({host_.now().value}).value;
    if (currentCycle < analogLastUpdateCycle_)
    {
        throw std::logic_error("analog device time moved backwards");
    }
    const std::uint64_t elapsed = currentCycle - analogLastUpdateCycle_;
    if (elapsed != 0 && analogDevice_->requiresTick())
    {
        analogDevice_->advance(elapsed);
    }
    analogLastUpdateCycle_ = currentCycle;
}

void AnalogController::scheduleAnalogWake()
{
    if (analogWakeServicing_ || analogDevice_ == nullptr || !host_.scheduleWake || !host_.active())
    {
        return;
    }

    updateAnalogDeviceToCurrentCycle();
    if (!analogDevice_->requiresTick())
    {
        analogWakeTick_.reset();
        return;
    }

    const std::uint64_t cycles = analogDevice_->cyclesUntilNextTransition();
    const std::uint64_t factor = clock_.factor();
    const std::uint64_t now = host_.now().value;
    if (cycles > (std::numeric_limits<std::uint64_t>::max() - now) / factor)
    {
        throw std::overflow_error("analog wake time overflowed");
    }
    const std::uint64_t candidate = analogDomain().after({now}, {cycles}).value;
    if (analogWakeTick_.has_value() && candidate >= *analogWakeTick_)
    {
        return;
    }

    if (analogWakeGeneration_ == std::numeric_limits<std::uint64_t>::max())
    {
        throw std::overflow_error("analog wake generation overflowed");
    }
    ++analogWakeGeneration_;
    analogWakeTick_ = candidate;
    host_.scheduleWake(Timing::Cycles<Timing::Analog>{cycles}, analogWakeGeneration_);
}

void AnalogController::serviceAnalogBridge(bool permitDeferredSubmission)
{
    if (!analogBridge_.open() || analogDevice_ == nullptr)
    {
        return;
    }

    try
    {
        updateAnalogDeviceToCurrentCycle();
        checkAnalogBridgeError();

        if (deferredAnalogSubmission_.has_value())
        {
            if (permitDeferredSubmission)
            {
                if (!host_.pending().has_value() ||
                    host_.pending()->stopReason != MITTENS_SYNC_STOP_ANALOG_SUBMIT ||
                    (host_.pending()->flags & MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION) != 0 ||
                    deferredAnalogSubmission_->token.arrayId != host_.pending()->analogArrayId ||
                    deferredAnalogSubmission_->token.sequence != host_.pending()->analogSequence ||
                    deferredAnalogSubmission_->command.operation !=
                        MITTENS_ANALOG_OPERATION_LOAD_VECTOR)
                {
                    throw std::runtime_error(
                        "deferred analog load reached a mismatched modeled frontier");
                }
                if (analogDevice_->canSubmit(deferredAnalogSubmission_->command))
                {
                    AnalogBridgeSubmission submission = std::move(*deferredAnalogSubmission_);
                    deferredAnalogSubmission_.reset();
                    serviceAnalogSubmission(std::move(submission), false);
                }
            }

            // Lookahead may already have published the following command.
            // Leave it submitted until its captured event is committed by
            // the SST owner at that command's own modeled frontier.
            serviceAnalogTrace();
            serviceAnalogCompletions();
            scheduleAnalogWake();
            checkAnalogBridgeError();
            return;
        }

        if (host_.hasAnalogReplay() && host_.pending().has_value() &&
            host_.pending()->stopReason == MITTENS_SYNC_STOP_ANALOG_SUBMIT)
        {
            const AnalogBridgeToken token{
                host_.pending()->analogArrayId,
                static_cast<std::uint32_t>(host_.pending()->analogSequence),
            };
            std::optional<AnalogBridgeSubmission> submission =
                analogBridge_.nextSubmission(token.arrayId);
            if (submission.has_value())
            {
                if (submission->token.arrayId != token.arrayId ||
                    submission->token.sequence != token.sequence)
                {
                    throw std::runtime_error("analog submit batch violated fd-43 acceptance order");
                }
                if (analogDevice_->canSubmit(submission->command))
                {
                    serviceAnalogSubmission(std::move(*submission));
                }
            }
            serviceAnalogTrace();
            serviceAnalogCompletions();
            scheduleAnalogWake();
            checkAnalogBridgeError();
            return;
        }

        bool accepted;
        do
        {
            accepted = false;
            for (std::uint32_t arrayId = 0; arrayId < analogBridge_.arrayCount(); ++arrayId)
            {
                std::optional<AnalogBridgeSubmission> submission =
                    analogBridge_.nextSubmission(arrayId);
                if (!submission.has_value() || !analogDevice_->canSubmit(submission->command))
                {
                    continue;
                }
                serviceAnalogSubmission(std::move(*submission));
                accepted = true;
            }
        } while (accepted);

        serviceAnalogTrace();
        serviceAnalogCompletions();
        scheduleAnalogWake();
        checkAnalogBridgeError();
    }
    catch (const std::exception& error)
    {
        output_.fatal(

            -1, "tile %u analog bridge failure: %s\n", static_cast<unsigned>(config_.tileId),
            error.what());
    }
}

void AnalogController::serviceAnalogSubmission(AnalogBridgeSubmission submission,
                                               bool publishAcceptance)
{
    const std::size_t inputWordCount = submission.inputWords.size();
    const std::uint32_t operation = submission.command.operation;
    const std::uint64_t ticket =
        analogDevice_->submit(submission.command, std::move(submission.inputWords));
    if (operation < analogOperationCounts_.size())
    {
        ++analogOperationCounts_[operation];
    }
    analogInputWords_ += inputWordCount;
    const auto inserted = analogRequests_.emplace(ticket, submission.token);
    if (!inserted.second)
    {
        throw std::logic_error("duplicate analog device ticket");
    }
    if (publishAcceptance)
    {
        analogBridge_.markAccepted(submission.token);
    }
}

void AnalogController::serviceAnalogTrace()
{
    if (analogDevice_ == nullptr || !performanceProfile_.traceEnabled())
    {
        return;
    }

    while (true)
    {
        const std::optional<AnalogTraceEvent> event = analogDevice_->takeTraceEvent();
        if (!event.has_value())
        {
            return;
        }

        const char* phase = "unknown";
        switch (event->phase)
        {
        case AnalogTracePhase::Submitted:
            phase = "submitted";
            break;
        case AnalogTracePhase::InputTransferStart:
            phase = "input-transfer-start";
            break;
        case AnalogTracePhase::InputTransferFinish:
            phase = "input-transfer-finish";
            break;
        case AnalogTracePhase::ComputeStart:
            phase = "compute-start";
            break;
        case AnalogTracePhase::ComputeFinish:
            phase = "compute-finish";
            break;
        case AnalogTracePhase::OutputTransferStart:
            phase = "output-transfer-start";
            break;
        case AnalogTracePhase::OutputTransferFinish:
            phase = "output-transfer-finish";
            break;
        case AnalogTracePhase::MoveOutputStart:
            phase = "move-output-start";
            break;
        case AnalogTracePhase::MoveOutputFinish:
            phase = "move-output-finish";
            break;
        case AnalogTracePhase::MoveInputStart:
            phase = "move-input-start";
            break;
        case AnalogTracePhase::MoveInputFinish:
            phase = "move-input-finish";
            break;
        case AnalogTracePhase::Complete:
            phase = "complete";
            break;
        }
        performanceProfile_.recordAnalog(phase, event->ticket, event->operation, event->arrayId,
                                         event->deviceCycle, host_.now().value);
    }
}

void AnalogController::serviceAnalogCompletions()
{
    if (!analogBridge_.open() || analogDevice_ == nullptr)
    {
        return;
    }

    while (analogDevice_->completionReady())
    {
        std::optional<AnalogCompletion> completion = analogDevice_->takeCompletion();
        if (!completion.has_value())
        {
            return;
        }

        const auto request = analogRequests_.find(completion->ticket);
        if (request == analogRequests_.end())
        {
            output_.fatal(

                -1, "tile %u has no bridge request for analog ticket %llu\n",
                static_cast<unsigned>(config_.tileId),
                static_cast<unsigned long long>(completion->ticket));
        }
        analogBridge_.complete(request->second, completion->response.status,
                               completion->outputWords);
        analogOutputWords_ += completion->outputWords.size();
        analogRequests_.erase(request);
    }
}

void AnalogController::checkAnalogBridgeError() const
{
    const std::uint32_t error = analogBridge_.protocolError();
    if (error != MITTENS_ANALOG_BRIDGE_ERROR_NONE)
    {
        output_.fatal(

            -1, "tile %u QEMU analog device reported bridge protocol error %u\n",
            static_cast<unsigned>(config_.tileId), static_cast<unsigned>(error));
    }
}

CpuDeviceResult AnalogController::executeCpuAnalog(const CpuAnalogAction& action)
{
    if (action.reason != MITTENS_SYNC_STOP_ANALOG_SUBMIT &&
        action.reason != MITTENS_SYNC_STOP_ANALOG_WAIT)
        throw std::logic_error("invalid analog CPU action");
    serviceAnalogBridge(true);
    CpuDeviceResult result;
    result.complete = pendingAnalogEventReady();
    return result;
}

void AnalogController::validateInitialCpuDevice(const QemuSyncEvent& event) const
{
    // Analog queues and bridges are private to one tile.  Capturing a submit
    // records the already-published private slot but does not accept, time, or
    // execute it; serviceAnalogBridge() remains on the SST owner thread after
    // deterministic commit.  Validate the token and slot here so a malformed
    // submission cannot partially commit the ready set.
    if (event.stopReason == MITTENS_SYNC_STOP_ANALOG_SUBMIT)
    {
        std::uint32_t allowedFlags = MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION;
        if (!event.analogSubmitBatch.empty())
        {
            allowedFlags |= MITTENS_SYNC_EVENT_FLAG_ANALOG_BATCH;
        }
        if (analogDevice_ == nullptr || !analogBridge_.open() ||
            event.analogArrayId >= analogBridge_.arrayCount() ||
            event.analogSequence > UINT32_MAX || (event.flags & ~allowedFlags) != 0 ||
            event.memoryAddress != 0 || event.memorySize != 0 ||
            event.memoryFlags != MITTENS_SYNC_MEMORY_FLAG_NONE ||
            analogBridge_.slotState(AnalogBridgeToken{
                event.analogArrayId,
                static_cast<std::uint32_t>(event.analogSequence),
            }) != MITTENS_ANALOG_SLOT_SUBMITTED)
        {
            throw std::runtime_error("initial parallel analog-submit event is malformed");
        }
    }
}

void AnalogController::onWake(std::uint64_t generation)
{
    if (generation != analogWakeGeneration_ || !analogWakeTick_.has_value() ||
        *analogWakeTick_ != host_.now().value)
    {
        return;
    }

    analogWakeTick_.reset();
    analogWakeServicing_ = true;
    try
    {
        updateAnalogDeviceToCurrentCycle();
        serviceAnalogTrace();
        serviceAnalogCompletions();
        if (host_.pending().has_value() &&
            (host_.pending()->stopReason == MITTENS_SYNC_STOP_ANALOG_SUBMIT ||
             host_.pending()->stopReason == MITTENS_SYNC_STOP_ANALOG_WAIT) &&
            pendingAnalogEventReady())
        {
            host_.completeDevice(host_.pendingStepId());
        }
        analogWakeServicing_ = false;
        scheduleAnalogWake();
    }
    catch (const std::exception& error)
    {
        analogWakeServicing_ = false;
        output_.fatal(

            -1, "tile %u analog event-driven wake failed: %s\n",
            static_cast<unsigned>(config_.tileId), error.what());
    }
}

} // namespace SST::Mittens
