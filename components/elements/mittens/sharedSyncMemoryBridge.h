#ifndef SST_MITTENS_SHARED_SYNC_MEMORY_BRIDGE_H
#define SST_MITTENS_SHARED_SYNC_MEMORY_BRIDGE_H

#include "mittens/SyncTileBridge.h"

#include <chrono>
#include <cstdint>
#include <optional>

namespace SST {
namespace Mittens {

struct QemuSyncEvent {
    std::uint64_t grantEpoch;
    std::uint64_t eventSequence;
    std::uint64_t instructionsExecuted;
    std::uint32_t stopReason;
    std::uint32_t flags;
    std::uint32_t analogArrayId;
    std::uint64_t analogSequence;
    std::uint32_t taskId;
    std::uint64_t executionId;
    std::uint32_t receiveDMASource;
    std::uint32_t receiveDMARouteId;
    std::uint32_t receiveDMAWordCount;
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
        std::chrono::milliseconds timeout);
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
