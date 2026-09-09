#ifndef SST_MITTENS_GLOBAL_DMA_EVENT_H
#define SST_MITTENS_GLOBAL_DMA_EVENT_H

#include <cstdint>
#include "globalDMAProtocol.h"

#include <sst/core/event.h>

namespace SST {
namespace Mittens {

class GlobalDMAEvent final : public SST::Event
{
  public:
    GlobalDMAEvent(
        std::uint32_t tileId,
        std::uint64_t executionId,
        std::uint32_t tokenId,
        std::uint64_t logicalIteration,
        std::uint64_t globalOffset,
        std::uint64_t scratchpadOffset,
        std::uint32_t byteCount,
        GlobalDMADirection direction,
        bool completion = false) :
        GlobalDMAEvent(
            tileId,
            executionId,
            tokenId,
            logicalIteration,
            globalOffset,
            scratchpadOffset,
            byteCount,
            direction,
            GlobalDMARequestNone,
            completion)
    {
    }

    GlobalDMAEvent(
        std::uint32_t tileId,
        std::uint64_t executionId,
        std::uint32_t tokenId,
        std::uint64_t logicalIteration,
        std::uint64_t globalOffset,
        std::uint64_t scratchpadOffset,
        std::uint32_t byteCount,
        GlobalDMADirection direction,
        std::uint32_t requestFlags,
        bool completion) :
        tileId_(tileId),
        executionId_(executionId),
        tokenId_(tokenId),
        logicalIteration_(logicalIteration),
        globalOffset_(globalOffset),
        scratchpadOffset_(scratchpadOffset),
        byteCount_(byteCount),
        direction_(direction),
        requestFlags_(requestFlags),
        completion_(completion)
    {
    }

    std::uint32_t tileId() const noexcept { return tileId_; }
    std::uint64_t executionId() const noexcept { return executionId_; }
    std::uint32_t tokenId() const noexcept { return tokenId_; }
    std::uint64_t logicalIteration() const noexcept
    {
        return logicalIteration_;
    }
    std::uint64_t globalOffset() const noexcept { return globalOffset_; }
    std::uint64_t scratchpadOffset() const noexcept
    {
        return scratchpadOffset_;
    }
    std::uint32_t byteCount() const noexcept { return byteCount_; }
    GlobalDMADirection direction() const noexcept { return direction_; }
    std::uint32_t requestFlags() const noexcept { return requestFlags_; }
    bool completion() const noexcept { return completion_; }

    void markCompletion() noexcept { completion_ = true; }

    void serialize_order(
        SST::Core::Serialization::serializer& ser) override
    {
        SST::Event::serialize_order(ser);
        SST_SER(tileId_);
        SST_SER(executionId_);
        SST_SER(tokenId_);
        SST_SER(logicalIteration_);
        SST_SER(globalOffset_);
        SST_SER(scratchpadOffset_);
        SST_SER(byteCount_);
        SST_SER(direction_);
        SST_SER(requestFlags_);
        SST_SER(completion_);
    }

  private:
    GlobalDMAEvent() = default;

    std::uint32_t tileId_ = 0;
    std::uint64_t executionId_ = 0;
    std::uint32_t tokenId_ = 0;
    std::uint64_t logicalIteration_ = 0;
    std::uint64_t globalOffset_ = 0;
    std::uint64_t scratchpadOffset_ = 0;
    std::uint32_t byteCount_ = 0;
    GlobalDMADirection direction_ =
        GlobalDMADirection::GlobalRAMToScratchpad;
    std::uint32_t requestFlags_ = GlobalDMARequestNone;
    bool completion_ = false;

    ImplementSerializable(SST::Mittens::GlobalDMAEvent);
};

} // namespace Mittens
} // namespace SST

#endif
