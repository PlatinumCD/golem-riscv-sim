#ifndef SST_MITTENS_ANALOG_DEVICE_H
#define SST_MITTENS_ANALOG_DEVICE_H

#include "analogBackend.h"
#include "mittens/AnalogTileBridge.h"

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

    std::uint32_t tileId() const noexcept { return tileId_; }
    std::size_t arrayCount() const noexcept;
    std::size_t queueDepth() const noexcept { return queueDepth_; }
    std::uint64_t computeLatencyCycles() const noexcept
    {
        return computeLatencyCycles_;
    }
    std::uint64_t elapsedCycles() const noexcept { return elapsedCycles_; }

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
        Complete,
    };

    struct Request {
        std::uint64_t ticket;
        MittensAnalogCommand command;
        std::uint32_t primaryArray;
        std::vector<std::uint32_t> arrays;
        std::vector<std::uint32_t> transferWords;
        std::size_t transferredWords = 0;
        std::uint64_t remainingComputeCycles = 0;
        Phase phase = Phase::Queued;
    };

    struct ArrayChannel {
        std::deque<std::shared_ptr<Request>> requests;
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
    void finishInputTransfer(const std::shared_ptr<Request>& request);
    void finishMoveTransfer(const std::shared_ptr<Request>& request);
    void finishRequest(const std::shared_ptr<Request>& request,
                       std::uint64_t status,
                       std::vector<std::uint32_t> outputWords = {});
    void completeImmediately(const MittensAnalogCommand& command,
                             std::uint64_t ticket,
                             std::uint32_t arrayId,
                             std::uint64_t status);

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
};

} // namespace Mittens
} // namespace SST

#endif
