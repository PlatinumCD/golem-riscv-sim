#include "sst_config.h"
#include "addressRegion.h"

#include "globalRAMController.h"

#include "globalRAMBacking.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace SST
{
namespace Mittens
{

namespace
{

bool parseSnapshotInteger(const char* name, std::uint64_t* value)
{
    const char* const text = std::getenv(name);
    if (text == nullptr || *text == '\0' || value == nullptr)
    {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
    {
        throw std::invalid_argument(std::string("invalid ") + name + " value: " + text);
    }
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

void writeAll(int descriptor, const std::uint8_t* data, std::size_t size)
{
    while (size != 0)
    {
        const ssize_t written = ::write(descriptor, data, size);
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            throw std::system_error(errno, std::generic_category(),
                                    "cannot write global RAM snapshot");
        }
        if (written == 0)
        {
            throw std::runtime_error("short global RAM snapshot write");
        }
        data += static_cast<std::size_t>(written);
        size -= static_cast<std::size_t>(written);
    }
}

void snapshotGlobalRAM(int backingDescriptor, std::uint64_t capacityBytes)
{
    const char* const path = std::getenv("MITTENS_SCULPTOR_GLOBAL_RAM_SNAPSHOT_PATH");
    if (path == nullptr || *path == '\0')
    {
        return;
    }

    std::uint64_t offset = 0;
    std::uint64_t byteCount = 0;
    if (backingDescriptor < 0 ||
        !parseSnapshotInteger("MITTENS_SCULPTOR_GLOBAL_RAM_SNAPSHOT_OFFSET", &offset) ||
        !parseSnapshotInteger("MITTENS_SCULPTOR_GLOBAL_RAM_SNAPSHOT_BYTES", &byteCount) ||
        byteCount == 0 || !AddressRegion{0, capacityBytes}.containsRange(offset, byteCount) ||
        byteCount > SIZE_MAX)
    {
        throw std::invalid_argument("invalid global RAM snapshot range");
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(byteCount));
    std::size_t completed = 0;
    while (completed != bytes.size())
    {
        const ssize_t result =
            ::pread(backingDescriptor, bytes.data() + completed, bytes.size() - completed,
                    static_cast<off_t>(offset + completed));
        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            throw std::system_error(errno, std::generic_category(),
                                    "cannot read global RAM snapshot");
        }
        if (result == 0)
        {
            throw std::runtime_error("short global RAM snapshot read");
        }
        completed += static_cast<std::size_t>(result);
    }

    const int output = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0)
    {
        throw std::system_error(errno, std::generic_category(),
                                "cannot create global RAM snapshot");
    }
    try
    {
        writeAll(output, bytes.data(), bytes.size());
    }
    catch (...)
    {
        (void)::close(output);
        throw;
    }
    if (::close(output) != 0)
    {
        throw std::system_error(errno, std::generic_category(), "cannot close global RAM snapshot");
    }
}

} // namespace

GlobalRAMController::GlobalRAMController(SST::ComponentId_t id, SST::Params& params)
    : SST::Component(id), configuration_(GlobalRAMConfiguration::read(params)),
      output_("mittens-global-ram: ", configuration_.verbose, 0, SST::Output::STDOUT),
      capacityBytes_(configuration_.capacity_bytes), tileCount_(configuration_.tile_count),
      channelCount_(configuration_.channels), queueDepth_(configuration_.queue_depth),
      perTileQueueDepth_(configuration_.per_tile_queue_depth),
      setupCycles_(configuration_.setup_cycles), bytesPerCycle_(configuration_.bytes_per_cycle),
      burstBytes_(configuration_.burst_bytes),
      fixedLatencyCycles_(configuration_.fixed_latency_cycles),
      maximumRequestBytes_(configuration_.maximum_request_bytes),
      demandWriteBurstLimit_(configuration_.demand_write_burst),
      readPriorityBurstLimit_(configuration_.read_priority_burst),
      reservedReadChannels_(configuration_.reserved_read_channels),
      progressSnapshotIntervalMilliseconds_(configuration_.progress_snapshot_interval_ms)
{
    configuration_.validate(output_);
    configuration_.emit();
    dependencyMode_ = configuration_.dependency_mode == "bulk_barrier"
                          ? DependencyMode::BulkBarrier
                          : DependencyMode::ExactDependencies;
    try
    {
        performanceProfile_.configure(configuration_.profile_output_directory);
    }
    catch (const std::exception& error)
    {
        output_.fatal(CALL_INFO, -1, "cannot configure global RAM progress profile: %s\n",
                      error.what());
    }
    lastProgressSnapshotWallTime_ = std::chrono::steady_clock::now();

    const auto& activeTileIds = configuration_.active_tiles;
    activeTiles_.resize(tileCount_, false);
    for (const std::uint32_t tile : activeTileIds)
    {
        activeTiles_[tile] = true;
        ++activeTileCount_;
    }

    try
    {
        backingDescriptor_ = GlobalRAMBacking::duplicate(capacityBytes_);
    }
    catch (const std::exception& error)
    {
        output_.fatal(CALL_INFO, -1, "cannot create sparse backing: %s\n", error.what());
    }

    links_.resize(tileCount_, nullptr);
    queues_.resize(tileCount_);
    for (std::uint32_t tile = 0; tile < tileCount_; ++tile)
    {
        const std::string name = "dma" + std::to_string(tile);
        links_[tile] = configureLink(
            name, new SST::Event::Handler<GlobalRAMController, &GlobalRAMController::handleRequest>(
                      this));
    }
    channels_.resize(channelCount_);

    requestStatistic_ = registerStatistic<std::uint64_t>("requests");
    byteStatistic_ = registerStatistic<std::uint64_t>("bytes");
    readinessDelayStatistic_ = registerStatistic<std::uint64_t>("readiness_delay_cycles");
    queueDelayStatistic_ = registerStatistic<std::uint64_t>("queue_delay_cycles");
    serviceStatistic_ = registerStatistic<std::uint64_t>("service_cycles");
    maximumQueueStatistic_ = registerStatistic<std::uint64_t>("maximum_queue_occupancy");
    blockedReadStatistic_ = registerStatistic<std::uint64_t>("readiness_blocked_reads");
    readinessReleaseStatistic_ = registerStatistic<std::uint64_t>("readiness_releases");
    intervalLookupStatistic_ = registerStatistic<std::uint64_t>("readiness_interval_lookups");
    publicationStatistic_ = registerStatistic<std::uint64_t>("readiness_publications");
    duplicatePublicationStatistic_ =
        registerStatistic<std::uint64_t>("readiness_duplicate_publications");
    maximumWaiterStatistic_ = registerStatistic<std::uint64_t>("readiness_maximum_waiters");
    executionTeardownStatistic_ = registerStatistic<std::uint64_t>("readiness_execution_teardowns");
    executionTeardownWaitStatistic_ =
        registerStatistic<std::uint64_t>("execution_teardown_wait_cycles");
    demandWriteWaiterLinkStatistic_ = registerStatistic<std::uint64_t>("demand_write_waiter_links");
    demandWritePromotionStatistic_ = registerStatistic<std::uint64_t>("demand_write_promotions");
    demandWriteSelectionStatistic_ = registerStatistic<std::uint64_t>("demand_write_selections");

    clockTimeBase_ = getTimeConverter(configuration_.clock);
    clockHandler_ =
        new SST::Clock::Handler<GlobalRAMController, &GlobalRAMController::clockTick>(this);
    registerClock(clockTimeBase_, clockHandler_);
    clockRegistered_ = true;
    wakeLink_ = configureSelfLink(
        "service-wakeup", clockTimeBase_,
        new SST::Event::Handler<GlobalRAMController, &GlobalRAMController::handleWake>(this));
}

GlobalRAMController::~GlobalRAMController()
{
    for (auto& queue : queues_)
    {
        while (!queue.empty())
        {
            delete queue.front().event;
            queue.pop_front();
        }
    }
    for (ActiveRequest& channel : channels_)
    {
        delete channel.event;
        channel.event = nullptr;
    }
    for (auto& execution : exactExecutions_)
    {
        for (GlobalDMAEvent* event : execution.second.teardownEvents)
        {
            delete event;
        }
    }
    if (backingDescriptor_ >= 0)
    {
        (void)::close(backingDescriptor_);
    }
}

void GlobalRAMController::setup()
{
    for (std::uint32_t tile = 0; tile < tileCount_; ++tile)
    {
        if (activeTiles_[tile] && links_[tile] == nullptr)
        {
            output_.fatal(CALL_INFO, -1, "global RAM active tile %u has no controller link\n",
                          static_cast<unsigned>(tile));
        }
        if (!activeTiles_[tile] && links_[tile] != nullptr)
        {
            output_.fatal(CALL_INFO, -1,
                          "global RAM inactive tile %u has an unexpected controller link\n",
                          static_cast<unsigned>(tile));
        }
    }
    maybeRecordProgressSnapshot("initial", true);
}

GlobalRAMController::ExactExecutionState&
GlobalRAMController::exactExecution(std::uint64_t executionId)
{
    auto result = exactExecutions_.try_emplace(executionId);
    if (result.second)
    {
        result.first->second.teardownEvents.resize(tileCount_, nullptr);
        result.first->second.teardownArrivalCycles.resize(tileCount_, UINT64_MAX);
    }
    return result.first->second;
}

bool GlobalRAMController::isExactDataRequest(const GlobalDMAEvent& event) const noexcept
{
    return (event.requestFlags() & GlobalDMAExactReadiness) != 0U && !isTeardown(event);
}

bool GlobalRAMController::isEpochZeroRead(const GlobalDMAEvent& event) const noexcept
{
    return (event.requestFlags() & GlobalDMAEpochZeroSource) != 0U;
}

bool GlobalRAMController::isTeardown(const GlobalDMAEvent& event) const noexcept
{
    return (event.requestFlags() & GlobalDMAExactExecutionTeardown) != 0U;
}

bool GlobalRAMController::requestIsSchedulable(GlobalDMAEvent& event)
{
    if (dependencyMode_ == DependencyMode::BulkBarrier)
    {
        return true;
    }

    ExactExecutionState& execution = exactExecution(event.executionId());
    if (event.direction() == GlobalDMADirection::ScratchpadToGlobalRAM || isEpochZeroRead(event))
    {
        return true;
    }

    intervalLookupStatistic_->addData(1);
    const std::uint64_t end = event.globalOffset() + event.byteCount();
    if (execution.committedRanges.covers(event.globalOffset(), end))
    {
        return true;
    }

    blockedReadStatistic_->addData(1);
    if (readinessBlocked_ == UINT64_MAX)
    {
        output_.fatal(CALL_INFO, -1, "exact global RAM blocked-read counter overflow\n");
    }
    ++readinessBlocked_;
    ++blockedWaiters_;
    maximumWaiters_ = std::max(maximumWaiters_, blockedWaiters_);
    return false;
}

void GlobalRAMController::releaseCoveredReads(std::uint64_t executionId,
                                              std::uint64_t publicationBegin)
{
    const auto execution = exactExecutions_.find(executionId);
    if (execution == exactExecutions_.end())
    {
        output_.fatal(CALL_INFO, -1, "exact global RAM publication has no execution state\n");
    }

    std::uint64_t committedBegin = 0;
    std::uint64_t committedEnd = 0;
    if (!execution->second.committedRanges.coveringRange(publicationBegin, &committedBegin,
                                                         &committedEnd))
    {
        output_.fatal(CALL_INFO, -1, "exact global RAM publication has no committed interval\n");
    }

    auto waiter = execution->second.waitersByBegin.lower_bound(committedBegin);
    while (waiter != execution->second.waitersByBegin.end() && waiter->first < committedEnd)
    {
        PendingRequest* request = waiter->second;
        intervalLookupStatistic_->addData(1);
        const std::uint64_t end = request->event->globalOffset() + request->event->byteCount();
        if (!execution->second.committedRanges.covers(request->event->globalOffset(), end))
        {
            ++waiter;
            continue;
        }
        if (demandWriteBurstLimit_ != 0)
        {
            adjustQueuedWriteDemand(execution->second, *request->event, false);
        }
        if (request->readinessCycle != UINT64_MAX)
        {
            output_.fatal(CALL_INFO, -1, "exact global RAM request became ready more than once\n");
        }
        request->readinessCycle = getCurrentSimCycle() / clockTimeBase_.getFactor();
        if (request->readinessCycle < request->arrivalCycle)
        {
            output_.fatal(CALL_INFO, -1, "exact global RAM readiness preceded request arrival\n");
        }
        request->schedulable = true;
        if (blockedWaiters_ == 0)
        {
            output_.fatal(CALL_INFO, -1, "exact global RAM waiter accounting underflow\n");
        }
        --blockedWaiters_;
        readinessReleaseStatistic_->addData(1);
        if (readinessReleased_ == UINT64_MAX)
        {
            output_.fatal(CALL_INFO, -1, "exact global RAM readiness-release counter overflow\n");
        }
        ++readinessReleased_;
        waiter = execution->second.waitersByBegin.erase(waiter);
    }
}

std::uint32_t GlobalRAMController::blockedReadDemandCount(const ExactExecutionState& execution,
                                                          const GlobalDMAEvent& write)
{
    const std::uint64_t writeBegin = write.globalOffset();
    const std::uint64_t writeEnd = writeBegin + write.byteCount();
    const std::uint64_t overlapSpan = maximumRequestBytes_ - 1U;
    const std::uint64_t earliestReadBegin = writeBegin > overlapSpan ? writeBegin - overlapSpan : 0;
    std::uint32_t count = 0;
    for (auto waiter = execution.waitersByBegin.lower_bound(earliestReadBegin);
         waiter != execution.waitersByBegin.end() && waiter->first < writeEnd; ++waiter)
    {
        const GlobalDMAEvent& read = *waiter->second->event;
        if (read.globalOffset() + read.byteCount() <= writeBegin)
            continue;
        if (count == UINT32_MAX)
        {
            output_.fatal(CALL_INFO, -1, "global RAM demand-write waiter count overflow\n");
        }
        ++count;
    }
    if (count != 0)
    {
        demandWriteWaiterLinkStatistic_->addData(count);
        demandWritePromotionStatistic_->addData(1);
    }
    return count;
}

void GlobalRAMController::adjustQueuedWriteDemand(ExactExecutionState& execution,
                                                  const GlobalDMAEvent& read, bool increment)
{
    const std::uint64_t readBegin = read.globalOffset();
    const std::uint64_t readEnd = readBegin + read.byteCount();
    const std::uint64_t overlapSpan = maximumRequestBytes_ - 1U;
    const std::uint64_t earliestWriteBegin = readBegin > overlapSpan ? readBegin - overlapSpan : 0;
    for (auto write = execution.queuedWritesByBegin.lower_bound(earliestWriteBegin);
         write != execution.queuedWritesByBegin.end() && write->first < readEnd; ++write)
    {
        PendingRequest& request = *write->second;
        const std::uint64_t writeEnd = request.event->globalOffset() + request.event->byteCount();
        if (writeEnd <= readBegin)
            continue;
        if (increment)
        {
            if (request.demandingWaiters == UINT32_MAX)
            {
                output_.fatal(CALL_INFO, -1, "global RAM queued-write demand count overflow\n");
            }
            if (request.demandingWaiters == 0)
                demandWritePromotionStatistic_->addData(1);
            ++request.demandingWaiters;
            demandWriteWaiterLinkStatistic_->addData(1);
        }
        else
        {
            if (request.demandingWaiters == 0)
            {
                output_.fatal(CALL_INFO, -1, "global RAM queued-write demand count underflow\n");
            }
            --request.demandingWaiters;
        }
    }
}

void GlobalRAMController::indexQueuedWrite(ExactExecutionState& execution, PendingRequest& request)
{
    execution.queuedWritesByBegin.emplace(request.event->globalOffset(), &request);
}

void GlobalRAMController::unindexQueuedWrite(PendingRequest& request)
{
    auto execution = exactExecutions_.find(request.event->executionId());
    if (execution == exactExecutions_.end())
    {
        output_.fatal(CALL_INFO, -1, "queued demand write has no exact execution state\n");
    }
    auto range = execution->second.queuedWritesByBegin.equal_range(request.event->globalOffset());
    for (auto candidate = range.first; candidate != range.second; ++candidate)
    {
        if (candidate->second == &request)
        {
            execution->second.queuedWritesByBegin.erase(candidate);
            return;
        }
    }
    output_.fatal(CALL_INFO, -1, "queued demand write is absent from the demand index\n");
}

void GlobalRAMController::publishCompletedWrite(const GlobalDMAEvent& event)
{
    if (dependencyMode_ != DependencyMode::ExactDependencies ||
        event.direction() != GlobalDMADirection::ScratchpadToGlobalRAM)
    {
        return;
    }
    if (!isExactDataRequest(event))
    {
        output_.fatal(CALL_INFO, -1, "exact global RAM write completed without readiness flag\n");
    }

    ExactExecutionState& execution = exactExecution(event.executionId());
    const std::uint64_t end = event.globalOffset() + event.byteCount();
    if (execution.committedRanges.publish(event.globalOffset(), end) !=
        GlobalRAMCommittedRanges::PublishResult::Published)
    {
        duplicatePublicationStatistic_->addData(1);
        output_.fatal(CALL_INFO, -1,
                      "duplicate or overlapping exact global RAM publication: "
                      "execution=%llu range=[%llu,%llu)\n",
                      static_cast<unsigned long long>(event.executionId()),
                      static_cast<unsigned long long>(event.globalOffset()),
                      static_cast<unsigned long long>(end));
    }
    publicationStatistic_->addData(1);
    if (readinessPublications_ == UINT64_MAX)
    {
        output_.fatal(CALL_INFO, -1, "exact global RAM publication counter overflow\n");
    }
    ++readinessPublications_;
    releaseCoveredReads(event.executionId(), event.globalOffset());
}

bool GlobalRAMController::executionHasDataRequests(std::uint64_t executionId) const noexcept
{
    if (executionHasActiveRequest(executionId))
    {
        return true;
    }
    for (const auto& queue : queues_)
    {
        for (const PendingRequest& request : queue)
        {
            if (request.event->executionId() == executionId)
            {
                return true;
            }
        }
    }
    return false;
}

bool GlobalRAMController::executionHasActiveRequest(std::uint64_t executionId) const noexcept
{
    for (const ActiveRequest& channel : channels_)
    {
        if (channel.event != nullptr && channel.event->executionId() == executionId)
        {
            return true;
        }
    }
    return false;
}

bool GlobalRAMController::hasSchedulableQueuedRequest() const noexcept
{
    for (const auto& queue : queues_)
    {
        for (const PendingRequest& request : queue)
        {
            if (request.schedulable)
            {
                return true;
            }
        }
    }
    return false;
}

bool GlobalRAMController::hasFullyArrivedTeardown(std::uint64_t executionId) const noexcept
{
    const auto execution = exactExecutions_.find(executionId);
    return execution != exactExecutions_.end() &&
           execution->second.teardownCount == activeTileCount_;
}

void GlobalRAMController::tryCompleteTeardown(std::uint64_t executionId, std::uint64_t releaseCycle)
{
    auto execution = exactExecutions_.find(executionId);
    if (execution == exactExecutions_.end() || execution->second.teardownCount != activeTileCount_)
    {
        return;
    }
    if (executionHasDataRequests(executionId))
    {
        if (!executionHasActiveRequest(executionId))
        {
            bool hasReadyRequest = false;
            for (const auto& queue : queues_)
            {
                for (const PendingRequest& request : queue)
                {
                    if (request.event->executionId() == executionId && request.schedulable)
                    {
                        hasReadyRequest = true;
                        break;
                    }
                }
                if (hasReadyRequest)
                {
                    break;
                }
            }
            if (!hasReadyRequest)
            {
                output_.fatal(CALL_INFO, -1,
                              "exact global RAM execution %llu tore down with "
                              "unresolved blocked reads\n",
                              static_cast<unsigned long long>(executionId));
            }
        }
        return;
    }

    std::vector<GlobalDMAEvent*> teardownEvents = std::move(execution->second.teardownEvents);
    std::vector<std::uint64_t> teardownArrivalCycles =
        std::move(execution->second.teardownArrivalCycles);
    exactExecutions_.erase(execution);
    if (!closedExecutions_.insert(executionId).second)
    {
        output_.fatal(CALL_INFO, -1, "exact global RAM execution closed more than once\n");
    }
    executionTeardownStatistic_->addData(1);
    for (std::uint32_t tile = 0; tile < tileCount_; ++tile)
    {
        GlobalDMAEvent* event = teardownEvents[tile];
        if (!activeTiles_[tile])
        {
            if (event != nullptr)
            {
                delete event;
                output_.fatal(CALL_INFO, -1,
                              "inactive exact global RAM tile has a teardown event\n");
            }
            continue;
        }
        if (event == nullptr || event->tileId() != tile || links_[tile] == nullptr ||
            teardownArrivalCycles[tile] == UINT64_MAX || teardownArrivalCycles[tile] > releaseCycle)
        {
            delete event;
            output_.fatal(CALL_INFO, -1,
                          "exact global RAM teardown has an invalid tile timeline\n");
        }
        const std::uint64_t waitCycles = releaseCycle - teardownArrivalCycles[tile];
        if (waitCycles > UINT64_MAX - executionTeardownWaitCycles_ ||
            teardownCompletionCount_ == UINT64_MAX)
        {
            delete event;
            output_.fatal(CALL_INFO, -1,
                          "exact global RAM teardown latency accounting overflowed\n");
        }
        try
        {
            performanceProfile_.recordTeardownTimeline(GlobalRAMTeardownTimeline{
                executionId, tile, teardownArrivalCycles[tile], releaseCycle});
        }
        catch (const std::exception& error)
        {
            delete event;
            output_.fatal(CALL_INFO, -1, "cannot record global RAM teardown timeline: %s\n",
                          error.what());
        }
        executionTeardownWaitCycles_ += waitCycles;
        ++teardownCompletionCount_;
        executionTeardownWaitStatistic_->addData(waitCycles);
        event->markCompletion();
        links_[tile]->send(event);
    }
}

void GlobalRAMController::acceptTeardown(GlobalDMAEvent* event)
{
    ExactExecutionState& execution = exactExecution(event->executionId());
    if (execution.teardownEvents[event->tileId()] != nullptr)
    {
        const std::uint64_t executionId = event->executionId();
        const std::uint32_t tileId = event->tileId();
        delete event;
        output_.fatal(CALL_INFO, -1,
                      "duplicate exact global RAM teardown: execution=%llu tile=%u\n",
                      static_cast<unsigned long long>(executionId), static_cast<unsigned>(tileId));
    }
    const std::uint64_t executionId = event->executionId();
    const std::uint64_t arrivalCycle = getNextClockCycle(clockTimeBase_) - 1U;
    execution.teardownEvents[event->tileId()] = event;
    execution.teardownArrivalCycles[event->tileId()] = arrivalCycle;
    ++execution.teardownCount;
    tryCompleteTeardown(executionId, arrivalCycle);
}

void GlobalRAMController::handleRequest(SST::Event* rawEvent)
{
    auto* event = dynamic_cast<GlobalDMAEvent*>(rawEvent);
    if (event == nullptr || event->completion() || event->tileId() >= tileCount_ ||
        !activeTiles_[event->tileId()] ||
        (event->requestFlags() & ~GlobalDMAKnownRequestFlags) != 0U ||
        (event->direction() != GlobalDMADirection::GlobalRAMToScratchpad &&
         event->direction() != GlobalDMADirection::ScratchpadToGlobalRAM))
    {
        delete rawEvent;
        output_.fatal(CALL_INFO, -1, "invalid global DMA request\n");
    }
    if (dependencyMode_ == DependencyMode::BulkBarrier)
    {
        if (event->requestFlags() != GlobalDMARequestNone)
        {
            delete event;
            output_.fatal(CALL_INFO, -1, "bulk global RAM request carries exact-readiness flags\n");
        }
    }
    else
    {
        if ((event->requestFlags() & GlobalDMAExactReadiness) == 0U ||
            closedExecutions_.count(event->executionId()) != 0U)
        {
            const std::uint32_t tile = event->tileId();
            const std::uint64_t execution = event->executionId();
            const std::uint32_t token = event->tokenId();
            const std::uint64_t iteration = event->logicalIteration();
            const std::uint64_t offset = event->globalOffset();
            const std::uint32_t bytes = event->byteCount();
            const std::uint32_t direction = static_cast<std::uint32_t>(event->direction());
            const std::uint32_t flags = event->requestFlags();
            const bool closed = closedExecutions_.count(execution) != 0U;
            delete event;
            output_.fatal(CALL_INFO, -1,
                          "invalid or late exact global RAM request: tile=%u "
                          "execution=%llu token=%u iteration=%llu range=[%llu,%llu) "
                          "direction=%u flags=0x%x closed=%u\n",
                          static_cast<unsigned>(tile), static_cast<unsigned long long>(execution),
                          static_cast<unsigned>(token), static_cast<unsigned long long>(iteration),
                          static_cast<unsigned long long>(offset),
                          static_cast<unsigned long long>(offset + bytes),
                          static_cast<unsigned>(direction), static_cast<unsigned>(flags),
                          static_cast<unsigned>(closed));
        }
        if (isTeardown(*event))
        {
            if (event->requestFlags() !=
                    (GlobalDMAExactReadiness | GlobalDMAExactExecutionTeardown) ||
                event->byteCount() != 0 || event->globalOffset() != 0 ||
                event->scratchpadOffset() != 0)
            {
                delete event;
                output_.fatal(CALL_INFO, -1, "malformed exact global RAM teardown request\n");
            }
            acceptTeardown(event);
            maybeRecordProgressSnapshot("periodic");
            return;
        }
        ExactExecutionState& execution = exactExecution(event->executionId());
        if (execution.teardownEvents[event->tileId()] != nullptr)
        {
            delete event;
            output_.fatal(CALL_INFO, -1, "exact global RAM data request follows tile teardown\n");
        }
        if (isEpochZeroRead(*event) &&
            event->direction() != GlobalDMADirection::GlobalRAMToScratchpad)
        {
            delete event;
            output_.fatal(CALL_INFO, -1, "exact global RAM write is marked epoch-zero\n");
        }
    }
    if (event->byteCount() == 0 || event->byteCount() > maximumRequestBytes_ ||
        !AddressRegion{0, capacityBytes_}.containsRange(event->globalOffset(), event->byteCount()))
    {
        delete event;
        output_.fatal(CALL_INFO, -1, "invalid global DMA data range\n");
    }
    if (queuedRequests_ >= queueDepth_ || queues_[event->tileId()].size() >= perTileQueueDepth_)
    {
        const std::uint32_t tile = event->tileId();
        const std::size_t tileQueued = queues_[tile].size();
        delete event;
        output_.fatal(
            CALL_INFO, -1, "global DMA queue is full: total=%u/%u tile=%u tile_queue=%zu/%u\n",
            static_cast<unsigned>(queuedRequests_), static_cast<unsigned>(queueDepth_),
            static_cast<unsigned>(tile), tileQueued, static_cast<unsigned>(perTileQueueDepth_));
    }
    const bool schedulable = requestIsSchedulable(*event);
    if (physicalDMASubmitted_ == UINT64_MAX)
    {
        delete event;
        output_.fatal(CALL_INFO, -1, "global RAM physical DMA submitted counter overflow\n");
    }
    const std::uint64_t arrivalCycle = getNextClockCycle(clockTimeBase_) - 1U;
    const std::uint64_t requestSequence = physicalDMASubmitted_ + 1U;
    auto& queue = queues_[event->tileId()];
    queue.push_back(PendingRequest{event, arrivalCycle, schedulable ? arrivalCycle : UINT64_MAX,
                                   requestSequence, schedulable, 0});
    PendingRequest& pending = queue.back();
    if (!schedulable)
    {
        exactExecution(event->executionId())
            .waitersByBegin.emplace(event->globalOffset(), &pending);
    }
    if (demandWriteBurstLimit_ != 0 && dependencyMode_ == DependencyMode::ExactDependencies)
    {
        ExactExecutionState& execution = exactExecution(event->executionId());
        if (event->direction() == GlobalDMADirection::ScratchpadToGlobalRAM)
        {
            pending.demandingWaiters = blockedReadDemandCount(execution, *event);
            indexQueuedWrite(execution, pending);
        }
        else if (!schedulable)
        {
            adjustQueuedWriteDemand(execution, *event, true);
        }
    }
    ++queuedRequests_;
    ++physicalDMASubmitted_;
    maximumQueueOccupancy_ = std::max<std::uint64_t>(maximumQueueOccupancy_, queuedRequests_);
    const bool firstActivity = !activitySnapshotRecorded_;
    const bool firstBlocked = !schedulable && !blockedSnapshotRecorded_;
    activitySnapshotRecorded_ = true;
    blockedSnapshotRecorded_ |= !schedulable;
    maybeRecordProgressSnapshot(firstBlocked ? "readiness-blocked" : "activity",
                                firstActivity || firstBlocked);
    ensureClock();
}

void GlobalRAMController::handleWake(SST::Event* event)
{
    delete event;
    const std::uint64_t cycle = getCurrentSimCycle() / clockTimeBase_.getFactor();
    if (cycle != scheduledWakeCycle_)
    {
        return;
    }
    scheduledWakeCycle_ = 0;
    maybeRecordProgressSnapshot("periodic");
    ensureClock();
}

std::uint64_t GlobalRAMController::serviceCycles(std::uint32_t byteCount) const
{
    const std::uint64_t bursts =
        (static_cast<std::uint64_t>(byteCount) + burstBytes_ - 1U) / burstBytes_;
    const std::uint64_t transfer =
        (static_cast<std::uint64_t>(byteCount) + bytesPerCycle_ - 1U) / bytesPerCycle_;
    if (bursts != 0 && fixedLatencyCycles_ > (UINT64_MAX - setupCycles_ - transfer) / bursts)
    {
        output_.fatal(CALL_INFO, -1, "global DMA service overflow\n");
    }
    return setupCycles_ + bursts * fixedLatencyCycles_ + transfer;
}

GlobalRAMController::ReadySelection GlobalRAMController::selectReadyRequest()
{
    if (demandWriteBurstLimit_ != 0)
    {
        if (consecutiveDemandWrites_ < demandWriteBurstLimit_)
        {
            ReadySelection demand = selectReadyRequestByDemand(true);
            if (demand.tile != UINT32_MAX)
            {
                ++consecutiveDemandWrites_;
                demandWriteSelectionStatistic_->addData(1);
                return demand;
            }
        }
        else
        {
            ReadySelection other = selectReadyRequestByDemand(false);
            if (other.tile != UINT32_MAX)
            {
                consecutiveDemandWrites_ = 0;
                return other;
            }
        }
    }
    if (readPriorityBurstLimit_ != 0)
    {
        if (consecutivePriorityReads_ < readPriorityBurstLimit_)
        {
            ReadySelection read =
                selectReadyRequestByDirection(GlobalDMADirection::GlobalRAMToScratchpad);
            if (read.tile != UINT32_MAX)
            {
                consecutiveDemandWrites_ = 0;
                ++consecutivePriorityReads_;
                return read;
            }
        }
        else
        {
            ReadySelection write =
                selectReadyRequestByDirection(GlobalDMADirection::ScratchpadToGlobalRAM);
            if (write.tile != UINT32_MAX)
            {
                if (write.request->demandingWaiters != 0 &&
                    consecutiveDemandWrites_ < demandWriteBurstLimit_)
                {
                    ++consecutiveDemandWrites_;
                }
                else if (write.request->demandingWaiters == 0)
                {
                    consecutiveDemandWrites_ = 0;
                }
                if (write.request->demandingWaiters != 0)
                    demandWriteSelectionStatistic_->addData(1);
                consecutivePriorityReads_ = 0;
                return write;
            }
        }
    }

    for (std::uint32_t offset = 0; offset < tileCount_; ++offset)
    {
        const std::uint32_t tile = (roundRobinCursor_ + offset) % tileCount_;
        const auto ready = std::find_if(
            queues_[tile].begin(), queues_[tile].end(), [this](const PendingRequest& request)
            { return request.schedulable && directionIsAdmissible(request.event->direction()); });
        if (ready != queues_[tile].end())
        {
            roundRobinCursor_ = (tile + 1U) % tileCount_;
            if (ready->event->direction() == GlobalDMADirection::ScratchpadToGlobalRAM)
            {
                consecutivePriorityReads_ = 0;
            }
            if (ready->event->direction() == GlobalDMADirection::ScratchpadToGlobalRAM &&
                ready->demandingWaiters != 0)
            {
                demandWriteSelectionStatistic_->addData(1);
                if (consecutiveDemandWrites_ < demandWriteBurstLimit_)
                    ++consecutiveDemandWrites_;
            }
            else
            {
                consecutiveDemandWrites_ = 0;
            }
            return ReadySelection{tile, &*ready};
        }
    }
    return ReadySelection{};
}

GlobalRAMController::ReadySelection GlobalRAMController::selectReadyRequestByDemand(bool demanded)
{
    for (std::uint32_t offset = 0; offset < tileCount_; ++offset)
    {
        const std::uint32_t tile = (roundRobinCursor_ + offset) % tileCount_;
        const auto ready = std::find_if(
            queues_[tile].begin(), queues_[tile].end(),
            [this, demanded](const PendingRequest& request)
            {
                const bool isDemandWrite =
                    request.event->direction() == GlobalDMADirection::ScratchpadToGlobalRAM &&
                    request.demandingWaiters != 0;
                return request.schedulable && isDemandWrite == demanded &&
                       directionIsAdmissible(request.event->direction());
            });
        if (ready != queues_[tile].end())
        {
            roundRobinCursor_ = (tile + 1U) % tileCount_;
            return ReadySelection{tile, &*ready};
        }
    }
    return ReadySelection{};
}

GlobalRAMController::ReadySelection
GlobalRAMController::selectReadyRequestByDirection(GlobalDMADirection direction)
{
    if (!directionIsAdmissible(direction))
    {
        return ReadySelection{};
    }
    for (std::uint32_t offset = 0; offset < tileCount_; ++offset)
    {
        const std::uint32_t tile = (roundRobinCursor_ + offset) % tileCount_;
        const auto ready = std::find_if(
            queues_[tile].begin(), queues_[tile].end(), [direction](const PendingRequest& request)
            { return request.schedulable && request.event->direction() == direction; });
        if (ready != queues_[tile].end())
        {
            roundRobinCursor_ = (tile + 1U) % tileCount_;
            return ReadySelection{tile, &*ready};
        }
    }
    return ReadySelection{};
}

bool GlobalRAMController::clockTick(SST::Cycle_t cycle)
{
    const std::uint64_t now = static_cast<std::uint64_t>(cycle);
    std::vector<std::uint64_t> completedExecutions;
    completedExecutions.reserve(channels_.size());
    for (ActiveRequest& channel : channels_)
    {
        if (channel.event != nullptr && channel.completionCycle <= now)
        {
            GlobalDMAEvent* event = channel.event;
            channel.event = nullptr;
            if (physicalDMACompleted_ == physicalDMASubmitted_)
            {
                delete event;
                output_.fatal(CALL_INFO, -1,
                              "global RAM physical DMA completions exceeded "
                              "submissions\n");
            }
            ++physicalDMACompleted_;
            if (event->byteCount() > UINT64_MAX - physicalDMABytesCompleted_)
            {
                delete event;
                output_.fatal(CALL_INFO, -1, "global RAM completed-byte counter overflow\n");
            }
            physicalDMABytesCompleted_ += event->byteCount();
            publishCompletedWrite(*event);
            completedExecutions.push_back(event->executionId());
            try
            {
                performanceProfile_.recordRequestTimeline(GlobalRAMRequestTimeline{
                    channel.requestSequence, event->tileId(), event->executionId(),
                    event->tokenId(), event->logicalIteration(), event->globalOffset(),
                    event->scratchpadOffset(), event->byteCount(), event->requestFlags(),
                    event->direction() == GlobalDMADirection::ScratchpadToGlobalRAM,
                    channel.arrivalCycle, channel.readinessCycle, channel.serviceStartCycle,
                    channel.completionCycle, channel.serviceCycles});
            }
            catch (const std::exception& error)
            {
                delete event;
                output_.fatal(CALL_INFO, -1, "cannot record global RAM request timeline: %s\n",
                              error.what());
            }
            event->markCompletion();
            requestStatistic_->addData(1);
            byteStatistic_->addData(event->byteCount());
            serviceStatistic_->addData(channel.serviceCycles);
            if (channel.serviceCycles > UINT64_MAX - serviceCycles_)
            {
                delete event;
                output_.fatal(CALL_INFO, -1, "global RAM service-cycle accounting overflowed\n");
            }
            serviceCycles_ += channel.serviceCycles;
            if (links_[event->tileId()] == nullptr)
            {
                delete event;
                output_.fatal(CALL_INFO, -1, "global DMA completion has no tile endpoint\n");
            }
            links_[event->tileId()]->send(event);
        }
    }

    for (ActiveRequest& channel : channels_)
    {
        if (channel.event != nullptr)
        {
            continue;
        }
        const ReadySelection selection = selectReadyRequest();
        if (selection.tile == UINT32_MAX)
        {
            break;
        }
        auto& queue = queues_[selection.tile];
        const auto selected =
            std::find_if(queue.begin(), queue.end(), [&selection](const PendingRequest& request)
                         { return &request == selection.request; });
        if (selected == queue.end() || !selected->schedulable)
        {
            output_.fatal(CALL_INFO, -1, "global DMA ready selection became stale\n");
        }
        PendingRequest request = *selected;
        if (demandWriteBurstLimit_ != 0 &&
            request.event->direction() == GlobalDMADirection::ScratchpadToGlobalRAM)
        {
            unindexQueuedWrite(*selected);
        }
        queue.erase(selected);
        --queuedRequests_;
        const std::uint64_t service = serviceCycles(request.event->byteCount());
        if (service > UINT64_MAX - now)
        {
            output_.fatal(CALL_INFO, -1, "global DMA completion overflow\n");
        }
        if (request.readinessCycle == UINT64_MAX || request.readinessCycle > now ||
            request.arrivalCycle > now)
        {
            output_.fatal(CALL_INFO, -1, "global DMA request has an invalid service timeline\n");
        }
        const std::uint64_t readinessDelay = request.readinessCycle - request.arrivalCycle;
        const std::uint64_t queueDelay = now - request.readinessCycle;
        if (readinessDelay > UINT64_MAX - readinessDelayCycles_ ||
            queueDelay > UINT64_MAX - queueDelayCycles_)
        {
            output_.fatal(CALL_INFO, -1, "global RAM delay accounting overflowed\n");
        }
        readinessDelayCycles_ += readinessDelay;
        queueDelayCycles_ += queueDelay;
        readinessDelayStatistic_->addData(readinessDelay);
        queueDelayStatistic_->addData(queueDelay);
        channel = ActiveRequest{request.event,
                                request.requestSequence,
                                request.arrivalCycle,
                                request.readinessCycle,
                                now,
                                now + service,
                                service};
    }

    std::sort(completedExecutions.begin(), completedExecutions.end());
    completedExecutions.erase(std::unique(completedExecutions.begin(), completedExecutions.end()),
                              completedExecutions.end());
    for (const std::uint64_t executionId : completedExecutions)
    {
        tryCompleteTeardown(executionId, now);
    }
    const bool firstRelease = readinessReleased_ != 0 && !releaseSnapshotRecorded_;
    releaseSnapshotRecorded_ |= readinessReleased_ != 0;
    maybeRecordProgressSnapshot(firstRelease ? "readiness-released" : "periodic", firstRelease);
    return sleepUntilNextCompletion(now);
}

void GlobalRAMController::ensureClock()
{
    if (clockRegistered_)
    {
        return;
    }
    reregisterClock(clockTimeBase_, clockHandler_);
    clockRegistered_ = true;
}

bool GlobalRAMController::sleepUntilNextCompletion(std::uint64_t cycle)
{
    std::uint64_t nextCompletion = UINT64_MAX;
    for (const ActiveRequest& channel : channels_)
    {
        if (channel.event != nullptr)
        {
            nextCompletion = std::min(nextCompletion, channel.completionCycle);
        }
    }

    if (nextCompletion == UINT64_MAX)
    {
        if (queuedRequests_ != 0)
        {
            if (hasSchedulableQueuedRequest())
            {
                output_.fatal(CALL_INFO, -1,
                              "global DMA has schedulable queued requests without "
                              "an active channel\n");
            }
            for (const auto& execution : exactExecutions_)
            {
                if (hasFullyArrivedTeardown(execution.first))
                {
                    output_.fatal(CALL_INFO, -1,
                                  "exact global RAM execution %llu has unresolved "
                                  "blocked reads after teardown\n",
                                  static_cast<unsigned long long>(execution.first));
                }
            }
        }
        scheduledWakeCycle_ = 0;
        clockRegistered_ = false;
        return true;
    }
    if (nextCompletion <= cycle + 1U)
    {
        scheduledWakeCycle_ = 0;
        return false;
    }

    // A link event runs after the clock at a shared timestamp.  Wake one
    // cycle before the modeled completion; re-registering from that event
    // therefore makes the next clock callback fire on nextCompletion.
    const std::uint64_t wakeCycle = nextCompletion - 1U;
    if (scheduledWakeCycle_ == 0 || wakeCycle < scheduledWakeCycle_)
    {
        wakeLink_->send(wakeCycle - cycle, new SST::Event());
        scheduledWakeCycle_ = wakeCycle;
    }
    clockRegistered_ = false;
    return true;
}

std::uint64_t GlobalRAMController::activeRequestCount() const noexcept
{
    return static_cast<std::uint64_t>(std::count_if(channels_.begin(), channels_.end(),
                                                    [](const ActiveRequest& request)
                                                    { return request.event != nullptr; }));
}

std::uint32_t GlobalRAMController::activeWriteCount() const noexcept
{
    return static_cast<std::uint32_t>(std::count_if(
        channels_.begin(), channels_.end(),
        [](const ActiveRequest& request)
        {
            return request.event != nullptr &&
                   request.event->direction() == GlobalDMADirection::ScratchpadToGlobalRAM;
        }));
}

bool GlobalRAMController::directionIsAdmissible(GlobalDMADirection direction) const noexcept
{
    return direction == GlobalDMADirection::GlobalRAMToScratchpad ||
           activeWriteCount() < channelCount_ - reservedReadChannels_;
}

void GlobalRAMController::recordProgressSnapshot(const char* kind)
{
    const auto now = std::chrono::steady_clock::now();
    const auto wallMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    performanceProfile_.recordProgressSnapshot({
        kind,
        static_cast<std::uint64_t>(wallMilliseconds),
        getCurrentSimCycle() / clockTimeBase_.getFactor(),
        physicalDMASubmitted_,
        physicalDMACompleted_,
        physicalDMABytesCompleted_,
        readinessBlocked_,
        readinessReleased_,
        blockedWaiters_,
        readinessPublications_,
        queuedRequests_,
        activeRequestCount(),
    });
    lastProgressSnapshotWallTime_ = now;
}

void GlobalRAMController::maybeRecordProgressSnapshot(const char* kind, bool force)
{
    if (!performanceProfile_.enabled())
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgressSnapshotWallTime_)
            .count();
    if (!force && (progressSnapshotIntervalMilliseconds_ == 0 ||
                   elapsed < static_cast<std::int64_t>(progressSnapshotIntervalMilliseconds_)))
    {
        return;
    }
    try
    {
        recordProgressSnapshot(kind);
    }
    catch (const std::exception& error)
    {
        output_.fatal(CALL_INFO, -1, "cannot record global RAM progress snapshot: %s\n",
                      error.what());
    }
}

void GlobalRAMController::finish()
{
    maybeRecordProgressSnapshot("final", true);
    maximumQueueStatistic_->addData(maximumQueueOccupancy_);
    maximumWaiterStatistic_->addData(maximumWaiters_);
    if (dependencyMode_ == DependencyMode::ExactDependencies &&
        (queuedRequests_ != 0 || blockedWaiters_ != 0 || !exactExecutions_.empty()))
    {
        output_.fatal(CALL_INFO, -1,
                      "exact global RAM finished with live execution readiness state\n");
    }
    if (!performanceProfile_.requestTotalsReconcile(physicalDMACompleted_, readinessDelayCycles_,
                                                    queueDelayCycles_, serviceCycles_))
    {
        output_.fatal(
            CALL_INFO, -1,
            "global RAM request latency counters do not reconcile with the request timeline\n");
    }
    if (!performanceProfile_.teardownTotalsReconcile(teardownCompletionCount_,
                                                     executionTeardownWaitCycles_))
    {
        output_.fatal(
            CALL_INFO, -1,
            "global RAM teardown latency counters do not reconcile with the teardown timeline\n");
    }
    try
    {
        snapshotGlobalRAM(backingDescriptor_, capacityBytes_);
    }
    catch (const std::exception& error)
    {
        output_.fatal(CALL_INFO, -1, "cannot snapshot global RAM: %s\n", error.what());
    }
}

void GlobalRAMController::emergencyShutdown()
{
    if (!performanceProfile_.enabled())
    {
        return;
    }
    try
    {
        recordProgressSnapshot("emergency");
    }
    catch (const std::exception&)
    {
        // Preserve the originating SST fatal diagnostic. The most recent
        // periodic or semantic checkpoint was already flushed synchronously.
    }
}

} // namespace Mittens
} // namespace SST
