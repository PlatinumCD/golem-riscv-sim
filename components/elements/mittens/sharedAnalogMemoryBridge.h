#ifndef SST_MITTENS_SHARED_ANALOG_MEMORY_BRIDGE_H
#define SST_MITTENS_SHARED_ANALOG_MEMORY_BRIDGE_H

#include "mittens/AnalogTileBridge.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace SST {
namespace Mittens {

struct AnalogBridgeToken {
    std::uint32_t arrayId;
    std::uint32_t sequence;
};

struct AnalogBridgeSubmission {
    AnalogBridgeToken token;
    MittensAnalogCommand command;
    std::vector<std::uint32_t> inputWords;
};

class SharedAnalogMemoryBridge final
{
  public:
    SharedAnalogMemoryBridge() = default;
    ~SharedAnalogMemoryBridge();

    SharedAnalogMemoryBridge(const SharedAnalogMemoryBridge&) = delete;
    SharedAnalogMemoryBridge& operator=(
        const SharedAnalogMemoryBridge&) = delete;
    SharedAnalogMemoryBridge(SharedAnalogMemoryBridge&&) = delete;
    SharedAnalogMemoryBridge& operator=(
        SharedAnalogMemoryBridge&&) = delete;

    void create(std::uint32_t tileId,
                std::uint32_t arrayCount,
                std::uint32_t arrayRows,
                std::uint32_t arrayColumns);
    void close() noexcept;

    int fileDescriptor() const noexcept { return fileDescriptor_; }
    bool open() const noexcept { return mapping_ != nullptr; }
    std::size_t mappingSize() const noexcept { return mappingSize_; }
    std::uint32_t arrayCount() const noexcept;
    std::uint32_t protocolError() const noexcept;

    std::optional<AnalogBridgeSubmission> nextSubmission(
        std::uint32_t arrayId);
    std::uint32_t slotState(const AnalogBridgeToken& token) const;
    void markAccepted(const AnalogBridgeToken& token);
    void complete(const AnalogBridgeToken& token,
                  std::uint64_t status,
                  const std::vector<std::uint32_t>& outputWords);

  private:
    MittensAnalogBridgeSlot& slot(const AnalogBridgeToken& token);
    void setProtocolError(
        enum MittensAnalogBridgeError error) noexcept;

    int fileDescriptor_ = -1;
    MittensAnalogBridgeHeader* mapping_ = nullptr;
    std::size_t mappingSize_ = 0;
};

} // namespace Mittens
} // namespace SST

#endif
