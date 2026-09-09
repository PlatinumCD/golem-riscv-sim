#ifndef SST_MITTENS_SHARED_SYNC_MEMORY_BRIDGE_H
#define SST_MITTENS_SHARED_SYNC_MEMORY_BRIDGE_H

#include "../../bridge/include/mittens/SyncTileBridge.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace SST {
namespace Mittens {

struct QemuSyncEvent {
    std::uint64_t grantEpoch;
    std::uint64_t eventSequence;
    std::uint64_t instructionsExecuted;
    std::uint64_t vectorInstructionsExecuted;
    std::uint32_t stopReason;
    std::uint32_t flags;
    std::uint32_t analogArrayId;
    std::uint64_t analogSequence;
    std::uint32_t taskId;
    std::uint64_t executionId;
    std::uint32_t receiveDMASource;
    std::uint32_t receiveDMARouteId;
    std::uint64_t receiveDMALogicalIteration;
    std::uint32_t receiveDMAWordCount;
    std::uint64_t memoryAddress;
    std::uint32_t memorySize;
    std::uint32_t memoryFlags;
    std::uint64_t globalDMALogicalIteration;
    std::uint64_t globalDMAScratchpadOffset;
    std::uint32_t globalDMARequestFlags;
    std::uint32_t epochId;
    std::uint32_t epochContribution;
    std::vector<MittensSyncMemoryAccess> memoryBatch;
    std::vector<MittensSyncGlobalDMASubmit> globalDMASubmitBatch;
    std::vector<MittensSyncAnalogSubmit> analogSubmitBatch;
    std::uint64_t memoryProgramCounter() const noexcept
    {
        return analogSequence;
    }
    std::uint64_t memoryReturnAddress() const noexcept
    {
        return executionId;
    }
    std::uint64_t memoryInitializationAccesses() const noexcept
    {
        return analogSequence;
    }
    std::uint64_t memoryInitializationReadBytes() const noexcept
    {
        return memoryAddress;
    }
    std::uint64_t memoryInitializationWriteBytes() const noexcept
    {
        return executionId;
    }
    std::uint32_t globalDMADirection() const noexcept
    {
        return analogArrayId;
    }
    std::uint64_t globalDMAOffset() const noexcept
    {
        return analogSequence;
    }
    std::uint64_t globalDMAScratchpadOffsetValue() const noexcept
    {
        return globalDMAScratchpadOffset;
    }
    std::uint32_t globalDMAByteCount() const noexcept
    {
        return memorySize;
    }
    std::uint32_t globalDMATokenId() const noexcept
    {
        return taskId;
    }
};

class SharedSyncMemoryBridge final
{
  public:
    SharedSyncMemoryBridge() = default;
    ~SharedSyncMemoryBridge();

    SharedSyncMemoryBridge(const SharedSyncMemoryBridge&) = delete;
    SharedSyncMemoryBridge& operator=(
        const SharedSyncMemoryBridge&) = delete;
    SharedSyncMemoryBridge(SharedSyncMemoryBridge&&) = delete;
    SharedSyncMemoryBridge& operator=(
        SharedSyncMemoryBridge&&) = delete;

    void create(std::uint32_t tileId);
    void close() noexcept;

    int fileDescriptor() const noexcept { return fileDescriptor_; }
    bool open() const noexcept { return mapping_ != nullptr; }
    std::uint32_t protocolError() const noexcept;

    std::uint64_t grant(std::uint64_t instructionBudget);
    std::optional<QemuSyncEvent> waitForEvent(
        std::chrono::milliseconds timeout,
        std::chrono::microseconds spinTimeout =
            std::chrono::microseconds::zero());
    void resume(const QemuSyncEvent& event);

  private:
    static void wake(std::uint32_t* state) noexcept;
    static void wait(std::uint32_t* state,
                     std::uint32_t expected,
                     std::chrono::milliseconds timeout) noexcept;
    void requireState(std::uint32_t first,
                      std::uint32_t second,
                      const char* operation);
    void setProtocolError(
        enum MittensSyncBridgeError error) noexcept;

    int fileDescriptor_ = -1;
    MittensSyncBridge* mapping_ = nullptr;
    std::uint64_t observedEventSequence_ = 0;
};

} // namespace Mittens
} // namespace SST

#endif
