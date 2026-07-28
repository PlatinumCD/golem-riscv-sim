#ifndef SST_MITTENS_SHARED_MEMORY_BRIDGE_H
#define SST_MITTENS_SHARED_MEMORY_BRIDGE_H

#include <cstdint>
#include <optional>
#include <vector>

#include "mittens/NICTileBridge.h"

namespace SST {
namespace Mittens {

struct ReceiveBurstInfo {
    std::uint32_t absoluteIndex;
    std::uint32_t source;
    std::uint32_t wordCount;
};

class SharedMemoryBridge final
{
  public:
    SharedMemoryBridge() = default;
    ~SharedMemoryBridge();

    SharedMemoryBridge(const SharedMemoryBridge&) = delete;
    SharedMemoryBridge& operator=(const SharedMemoryBridge&) = delete;
    SharedMemoryBridge(SharedMemoryBridge&&) = delete;
    SharedMemoryBridge& operator=(SharedMemoryBridge&&) = delete;

    void create(std::uint32_t tileId);
    void close() noexcept;

    int fileDescriptor() const noexcept { return fileDescriptor_; }
    bool open() const noexcept { return mapping_ != nullptr; }
    std::uint32_t protocolError() const noexcept;

    std::optional<MittensBridgePacket> popTransmit();
    std::optional<MittensBridgeTxBurst> popTransmitBurst();
    bool receiveHasData() const noexcept;
    bool receiveHasWordData() const noexcept;
    bool receiveHasSpace() const noexcept;
    bool pushReceive(std::uint32_t source, std::uint32_t payload);
    bool receiveHasBurstSpace() const noexcept;
    std::uint32_t receiveBurstCount() const noexcept;
    std::uint32_t receiveBurstReadIndex() const noexcept;
    std::optional<ReceiveBurstInfo> peekReceiveBurst(
        std::uint32_t offset = 0) const noexcept;
    bool pushReceiveBurst(std::uint32_t source,
                          const std::vector<std::uint32_t>& payload);
    bool receiveDMAAuthorizationAvailable() const noexcept;
    bool authorizeReceiveDMA();

  private:
    int fileDescriptor_ = -1;
    MittensBridgeShared* mapping_ = nullptr;
};

} // namespace Mittens
} // namespace SST

#endif
