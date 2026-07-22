#ifndef SST_MITTENS_SHARED_MEMORY_BRIDGE_H
#define SST_MITTENS_SHARED_MEMORY_BRIDGE_H

#include <cstdint>
#include <optional>

#include "mittens/bridge.h"

namespace SST {
namespace Mittens {

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
    bool receiveHasSpace() const noexcept;
    bool pushReceive(std::uint32_t payload);

  private:
    int fileDescriptor_ = -1;
    MittensBridgeShared* mapping_ = nullptr;
};

} // namespace Mittens
} // namespace SST

#endif
