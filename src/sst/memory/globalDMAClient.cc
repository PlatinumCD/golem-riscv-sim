#include "globalDMAClient.h"

namespace SST::Mittens
{
CpuDeviceResult GlobalDMAClient::executeCpuGlobalDMA(const CpuGlobalDMAAction& action)
{
    CpuDeviceResult result;
    auto scratchpadTimingCycle_ = action.cursor.value;
    const auto finish = [&](bool complete)
    {
        result.complete = complete;
        result.cursor = Timing::Cycles<Timing::Cpu>{scratchpadTimingCycle_};
        return result;
    };
    const auto scheduleDelay = [&](std::uint64_t cycles)
    { result.delay = Timing::Cycles<Timing::Cpu>{cycles}; };
    switch (action.reason)
    {
    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT:
    {
        const std::uint32_t requestFlags = action.requestFlags;
        const bool exact = (requestFlags & GlobalDMAExactReadiness) != 0;
        const bool epochZero = (requestFlags & GlobalDMAEpochZeroSource) != 0;
        const bool teardown = (requestFlags & GlobalDMAExactExecutionTeardown) != 0;
        const bool validFlags = (requestFlags & ~GlobalDMAKnownRequestFlags) == 0 &&
                                (exact || requestFlags == GlobalDMARequestNone) &&
                                (!epochZero || (exact && action.direction == 0));
        const bool validTeardown =
            !teardown || (exact && !epochZero && action.direction == 1 && action.bytes == 0 &&
                          action.globalOffset == 0 && action.scratchpadOffset == 0 &&
                          action.iteration == UINT64_MAX);
        if (!host_.scratchpadAvailable() || !host_.send || !validFlags || !validTeardown ||
            (!teardown && action.bytes == 0) || action.direction > 1)
        {
            output_.fatal(-1, "tile %u received an invalid global DMA submit\n",
                          static_cast<unsigned>(config_.tileId));
        }
        const std::uint64_t globalOffset = action.globalOffset;
        const std::uint64_t scratchpadOffset = action.scratchpadOffset;
        const std::uint32_t byteCount = action.bytes;
        if (!teardown && (globalOffset > config_.globalRAMBytes ||
                          byteCount > config_.globalRAMBytes - globalOffset ||
                          scratchpadOffset > config_.scratchpadBytes ||
                          byteCount > config_.scratchpadBytes - scratchpadOffset))
        {
            output_.fatal(-1, "tile %u global DMA range is out of bounds\n",
                          static_cast<unsigned>(config_.tileId));
        }
        const std::uint64_t submitTick = host_.now().value;
        std::uint64_t localCompletionTick = submitTick;
        std::uint64_t scratchpadCompletionCycle = scratchpadTimingCycle_;
        if (!teardown)
        {
            const bool writesScratchpad = action.direction == 0;
            const ScratchpadSchedule schedule = host_.reserveDMA(
                scratchpadTimingCycle_, scratchpadOffset, byteCount, writesScratchpad);
            const std::uint64_t localService = schedule.completionCycle - scratchpadTimingCycle_;
            try
            {
                localCompletionTick =
                    cpuDomain()
                        .after(Timing::Ticks{submitTick}, Timing::Cycles<Timing::Cpu>{localService})
                        .value;
            }
            catch (const std::overflow_error&)
            {
                output_.fatal(-1, "tile %u global DMA local completion overflow\n",
                              static_cast<unsigned>(config_.tileId));
            }
            scratchpadCompletionCycle = schedule.completionCycle;
        }
        auto& execution = globalDMACompletions_[action.execution];
        if (!execution
                 .emplace(action.token,
                          GlobalDMACompletion{action.iteration, localCompletionTick,
                                              scratchpadCompletionCycle, requestFlags, false})
                 .second)
        {
            output_.fatal(-1, "tile %u reused global DMA token %u for execution %llu\n",
                          static_cast<unsigned>(config_.tileId),
                          static_cast<unsigned>(action.token),
                          static_cast<unsigned long long>(action.execution));
        }
        host_.send(GlobalDMAMessage(config_.tileId, action.execution, action.token,
                                    action.iteration, globalOffset, scratchpadOffset, byteCount,
                                    static_cast<GlobalDMADirection>(action.direction), requestFlags,
                                    false));
        if (!teardown)
        {
            if (physicalGlobalDMASubmitted_ == UINT64_MAX)
            {
                output_.fatal(-1,
                              "tile %u physical global DMA submitted counter "
                              "overflowed\n",
                              static_cast<unsigned>(config_.tileId));
            }
            ++physicalGlobalDMASubmitted_;
        }
        return finish(true);
    }

    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT:
    {
        auto execution = globalDMACompletions_.find(action.execution);
        if (execution == globalDMACompletions_.end())
        {
            output_.fatal(-1, "tile %u waited for an unknown global DMA execution\n",
                          static_cast<unsigned>(config_.tileId));
        }
        auto token = execution->second.find(action.token);
        if (token == execution->second.end())
        {
            output_.fatal(-1, "tile %u waited for an unknown global DMA token\n",
                          static_cast<unsigned>(config_.tileId));
        }
        if (!token->second.controllerComplete)
        {
            return finish(false);
        }
        const std::uint64_t now = host_.now().value;
        if (token->second.localCompletionTick > now)
        {
            if (!scratchpadWaitDelayScheduled_)
            {
                scratchpadWaitDelayScheduled_ = true;
                scheduleDelay(
                    cpuDomain().ceil(Timing::Ticks{token->second.localCompletionTick - now}).value);
            }
            // Other DMA completions can wake this same pending wait. A timer
            // being scheduled is not evidence that its deadline has elapsed.
            return finish(false);
        }
        scratchpadWaitDelayScheduled_ = false;
        scratchpadTimingCycle_ =
            std::max(scratchpadTimingCycle_, token->second.scratchpadCompletionCycle);
        execution->second.erase(token);
        if (execution->second.empty())
        {
            globalDMACompletions_.erase(execution);
        }
        return finish(true);
    }

    case MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH:
    {
        const auto& records = action.waits;
        if (records.empty() || records.size() > MITTENS_SYNC_GLOBAL_DMA_BATCH_CAPACITY ||
            action.flags != MITTENS_SYNC_EVENT_FLAG_NONE)
        {
            output_.fatal(-1, "tile %u received an invalid global DMA wait batch\n",
                          static_cast<unsigned>(config_.tileId));
        }
        const std::uint64_t executionId = records.front().execution_id;
        const std::uint32_t firstToken = records.front().token_id;
        if (action.execution != executionId || action.token != firstToken ||
            firstToken > UINT32_MAX - (records.size() - 1U))
        {
            output_.fatal(-1,
                          "tile %u received an inconsistent global DMA wait batch "
                          "envelope\n",
                          static_cast<unsigned>(config_.tileId));
        }

        auto execution = globalDMACompletions_.find(executionId);
        if (execution == globalDMACompletions_.end())
        {
            output_.fatal(-1, "tile %u batch-waited for an unknown global DMA execution\n",
                          static_cast<unsigned>(config_.tileId));
        }
        std::uint64_t latestLocalCompletion = 0;
        std::uint64_t latestScratchpadCompletion = scratchpadTimingCycle_;
        for (std::size_t index = 0; index < records.size(); ++index)
        {
            const MittensSyncGlobalDMASubmit& record = records[index];
            const std::uint32_t expectedToken = firstToken + static_cast<std::uint32_t>(index);
            auto token = execution->second.find(expectedToken);
            if (record.execution_id != executionId || record.token_id != expectedToken ||
                token == execution->second.end() ||
                token->second.logicalIteration != record.logical_iteration ||
                token->second.requestFlags != record.request_flags)
            {
                output_.fatal(-1,
                              "tile %u batch-waited for an inconsistent global DMA "
                              "token\n",
                              static_cast<unsigned>(config_.tileId));
            }
            if (!token->second.controllerComplete)
            {
                return finish(false);
            }
            latestLocalCompletion =
                std::max(latestLocalCompletion, token->second.localCompletionTick);
            latestScratchpadCompletion =
                std::max(latestScratchpadCompletion, token->second.scratchpadCompletionCycle);
        }
        const std::uint64_t now = host_.now().value;
        if (latestLocalCompletion > now)
        {
            if (!scratchpadWaitDelayScheduled_)
            {
                scratchpadWaitDelayScheduled_ = true;
                scheduleDelay(cpuDomain().ceil(Timing::Ticks{latestLocalCompletion - now}).value);
            }
            return finish(false);
        }
        scratchpadWaitDelayScheduled_ = false;
        scratchpadTimingCycle_ = latestScratchpadCompletion;
        for (std::size_t index = 0; index < records.size(); ++index)
        {
            execution->second.erase(firstToken + static_cast<std::uint32_t>(index));
        }
        if (execution->second.empty())
        {
            globalDMACompletions_.erase(execution);
        }
        return finish(true);
    }

    default:
        throw std::logic_error("invalid typed CPU device action");
    }
    return finish(false);
}

void GlobalDMAClient::onCompletion(const GlobalDMAMessage& message)
{
    const auto* event = &message;
    if (event == nullptr || !event->completion() || event->tileId() != config_.tileId)
    {
        output_.fatal(-1, "tile %u received an invalid global DMA completion\n",
                      static_cast<unsigned>(config_.tileId));
    }
    auto execution = globalDMACompletions_.find(event->executionId());
    if (execution == globalDMACompletions_.end())
    {
        output_.fatal(-1, "tile %u received an unknown global DMA execution\n",
                      static_cast<unsigned>(config_.tileId));
    }
    const std::uint32_t completionTokenId = event->tokenId();
    auto token = execution->second.find(completionTokenId);
    if (token == execution->second.end() || token->second.controllerComplete ||
        token->second.logicalIteration != event->logicalIteration() ||
        token->second.requestFlags != event->requestFlags())
    {
        output_.fatal(-1, "tile %u received an unknown global DMA completion token %u\n",
                      static_cast<unsigned>(config_.tileId),
                      static_cast<unsigned>(completionTokenId));
    }
    if ((event->requestFlags() & GlobalDMAExactExecutionTeardown) == 0U)
    {
        if (physicalGlobalDMACompleted_ == physicalGlobalDMASubmitted_)
        {
            output_.fatal(-1,
                          "tile %u physical global DMA completion accounting "
                          "exceeded submissions\n",
                          static_cast<unsigned>(config_.tileId));
        }
        ++physicalGlobalDMACompleted_;
    }
    token->second.controllerComplete = true;
    if (host_.pending().has_value() &&
        (host_.pending()->stopReason == MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT ||
         host_.pending()->stopReason == MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH))
    {
        host_.wake();
    }
}

} // namespace SST::Mittens
