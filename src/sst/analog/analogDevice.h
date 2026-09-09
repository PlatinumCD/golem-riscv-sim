#ifndef SST_MITTENS_ANALOG_DEVICE_H
#define SST_MITTENS_ANALOG_DEVICE_H

#include "analogBackend.h"
#include "../../bridge/include/mittens/AnalogTileBridge.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace SST {
namespace Mittens {

struct AnalogCompletion {
    MittensAnalogResponse response;
    std::uint64_t ticket;
    std::uint32_t operation;
    std::uint32_t arrayId;
    std::vector<std::uint32_t> outputWords;
};

enum class AnalogTracePhase : std::uint32_t {
    Submitted = 0,
    InputTransferStart = 1,
    InputTransferFinish = 2,
    ComputeStart = 3,
    ComputeFinish = 4,
    OutputTransferStart = 5,
    OutputTransferFinish = 6,
    MoveOutputStart = 7,
    MoveOutputFinish = 8,
    MoveInputStart = 9,
    MoveInputFinish = 10,
    Complete = 11,
};

struct AnalogTraceEvent {
    std::uint64_t ticket;
    std::uint32_t operation;
    std::uint32_t arrayId;
    AnalogTracePhase phase;
    std::uint64_t deviceCycle;
};

class AnalogDevice final
{
  public:
    static constexpr std::size_t DefaultQueueDepth = 4;
    static constexpr std::uint32_t InvalidArrayId = UINT32_MAX;

    AnalogDevice(std::uint32_t tileId,
                 std::uint64_t computeLatencyCycles,
                 std::unique_ptr<AnalogBackend> backend,
                 std::size_t queueDepth = DefaultQueueDepth);

    bool canSubmit(const MittensAnalogCommand& command) const noexcept;
    std::uint64_t submit(
        const MittensAnalogCommand& command,
        std::vector<std::uint32_t> inputWords = {});
    void tick();
    void advance(std::uint64_t cycles);

    // Returns the exact number of active device cycles until a request
    // changes phase or completes.  No externally visible device state can
    // change before this horizon, so an event-driven simulator may replace
    // the intervening per-cycle clock callbacks with one wakeup.
    std::uint64_t cyclesUntilNextTransition() const;

    bool busy() const noexcept;
    bool busy(std::uint32_t arrayId) const noexcept;
    bool requiresTick() const noexcept;
    std::size_t queuedCommands(std::uint32_t arrayId) const;

    bool completionReady() const noexcept;
    bool completionReady(std::uint32_t arrayId) const noexcept;
    bool completionReadyForTicket(std::uint64_t ticket) const noexcept;
    std::optional<AnalogCompletion> takeCompletion();
    std::optional<AnalogCompletion> takeCompletion(
        std::uint32_t arrayId);
    std::optional<AnalogCompletion> takeCompletionForTicket(
        std::uint64_t ticket);
    void setTraceEnabled(bool enabled) noexcept;
    bool traceEnabled() const noexcept { return traceEnabled_; }
    std::optional<AnalogTraceEvent> takeTraceEvent();

    std::uint32_t tileId() const noexcept { return tileId_; }
    std::size_t arrayCount() const noexcept;
    std::size_t queueDepth() const noexcept { return queueDepth_; }
    std::uint64_t computeLatencyCycles() const noexcept
    {
        return computeLatencyCycles_;
    }
    std::uint64_t elapsedCycles() const noexcept { return elapsedCycles_; }
    std::uint64_t linkBeats() const noexcept { return linkBeats_; }
    std::uint64_t submittedCommandCount() const noexcept
    {
        return submittedCommandCount_;
    }
    std::uint64_t completedCommandCount() const noexcept
    {
        return completedCommandCount_;
    }
    std::uint64_t maximumOutstandingCommandCount() const noexcept
    {
        return maximumOutstandingCommandCount_;
    }

    static std::uint64_t linkCyclesForWords(
        std::size_t wordCount) noexcept;
    static std::uint64_t ingressCyclesForWords(
        std::size_t wordCount) noexcept
    {
        return linkCyclesForWords(wordCount);
    }

  private:
    enum class Phase {
        Queued,
        TransferringInput,
        Computing,
        TransferringOutput,
        MovingOutput,
        MovingInput,
        Complete,
    };

    struct Request {
        std::uint64_t ticket;
        MittensAnalogCommand command;
        std::uint32_t primaryArray;
        std::vector<std::uint32_t> arrays;
        std::vector<std::uint32_t> transferWords;
        std::size_t linkWordCount = 0;
        std::size_t transferredWords = 0;
        std::uint64_t remainingComputeCycles = 0;
        Phase phase = Phase::Queued;
    };

    struct ArrayChannel {
        std::deque<std::shared_ptr<Request>> requests;
        std::uint32_t validRows = 0;
        std::uint32_t validColumns = 0;
    };

    bool supportedOperation(std::uint32_t operation) const noexcept;
    bool routeCommand(
        const MittensAnalogCommand& command,
        std::vector<std::uint32_t>& arrays,
        std::uint32_t& primaryArray) const noexcept;
    bool validArray(std::uint64_t arrayId) const noexcept;
    std::optional<std::uint64_t> validatePayload(
        const MittensAnalogCommand& command,
        const std::vector<std::uint32_t>& inputWords) const noexcept;

    bool requestIsReady(const std::shared_ptr<Request>& request) const;
    void startReadyRequests();
    void startRequest(const std::shared_ptr<Request>& request);
    void advanceRequest(const std::shared_ptr<Request>& request);
    bool usesLink(const std::shared_ptr<Request>& request) const noexcept;
    std::uint64_t remainingLinkBeats(
        const std::shared_ptr<Request>& request) const;
    std::uint32_t linkArray(
        const std::shared_ptr<Request>& request) const noexcept;
    void finishInputTransfer(const std::shared_ptr<Request>& request);
    void finishMoveTransfer(const std::shared_ptr<Request>& request);
    void finishRequest(const std::shared_ptr<Request>& request,
                       std::uint64_t status,
                       std::vector<std::uint32_t> outputWords = {});
    void completeImmediately(const MittensAnalogCommand& command,
                             std::uint64_t ticket,
                             std::uint32_t arrayId,
                             std::uint64_t status);
    void recordSubmittedCommand();
    void recordCompletedCommand();
    void recordTrace(const std::shared_ptr<Request>& request,
                     AnalogTracePhase phase);
    void recordTrace(std::uint64_t ticket,
                     std::uint32_t operation,
                     std::uint32_t arrayId,
                     AnalogTracePhase phase);

    std::vector<float> decodeFloatWords(
        const std::vector<std::uint32_t>& words) const;
    std::vector<std::uint32_t> encodeFloatWords(
        const std::vector<float>& values) const;

    std::uint32_t tileId_;
    std::uint64_t computeLatencyCycles_;
    std::unique_ptr<AnalogBackend> backend_;
    std::size_t queueDepth_;
    std::vector<ArrayChannel> channels_;
    std::deque<AnalogCompletion> completions_;
    std::uint64_t nextTicket_ = 1;
    std::uint64_t elapsedCycles_ = 0;
    std::uint64_t linkBeats_ = 0;
    std::uint64_t submittedCommandCount_ = 0;
    std::uint64_t completedCommandCount_ = 0;
    std::uint64_t maximumOutstandingCommandCount_ = 0;
    std::uint32_t nextLinkArray_ = 0;
    bool traceEnabled_ = false;
    std::deque<AnalogTraceEvent> traceEvents_;
};

} // namespace Mittens
} // namespace SST

#endif
