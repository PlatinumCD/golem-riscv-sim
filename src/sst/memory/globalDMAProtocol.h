#pragma once
#include <cstdint>
namespace SST::Mittens
{
enum class GlobalDMADirection : std::uint32_t
{
    GlobalRAMToScratchpad = 0,
    ScratchpadToGlobalRAM = 1,
};

enum GlobalDMARequestFlag : std::uint32_t
{
    GlobalDMARequestNone = 0,
    GlobalDMAExactReadiness = UINT32_C(1) << 0U,
    GlobalDMAEpochZeroSource = UINT32_C(1) << 1U,
    GlobalDMAExactExecutionTeardown = UINT32_C(1) << 2U,
};

constexpr std::uint32_t GlobalDMAKnownRequestFlags =
    GlobalDMAExactReadiness | GlobalDMAEpochZeroSource | GlobalDMAExactExecutionTeardown;

class GlobalDMAMessage final
{
  public:
    GlobalDMAMessage(std::uint32_t tileId, std::uint64_t executionId, std::uint32_t tokenId,
                     std::uint64_t logicalIteration, std::uint64_t globalOffset,
                     std::uint64_t scratchpadOffset, std::uint32_t byteCount,
                     GlobalDMADirection direction, bool completion = false)
        : GlobalDMAMessage(tileId, executionId, tokenId, logicalIteration, globalOffset,
                           scratchpadOffset, byteCount, direction, GlobalDMARequestNone, completion)
    {
    }

    GlobalDMAMessage(std::uint32_t tileId, std::uint64_t executionId, std::uint32_t tokenId,
                     std::uint64_t logicalIteration, std::uint64_t globalOffset,
                     std::uint64_t scratchpadOffset, std::uint32_t byteCount,
                     GlobalDMADirection direction, std::uint32_t requestFlags, bool completion)
        : tileId_(tileId), executionId_(executionId), tokenId_(tokenId),
          logicalIteration_(logicalIteration), globalOffset_(globalOffset),
          scratchpadOffset_(scratchpadOffset), byteCount_(byteCount), direction_(direction),
          requestFlags_(requestFlags), completion_(completion)
    {
    }

    std::uint32_t tileId() const noexcept
    {
        return tileId_;
    }
    std::uint64_t executionId() const noexcept
    {
        return executionId_;
    }
    std::uint32_t tokenId() const noexcept
    {
        return tokenId_;
    }
    std::uint64_t logicalIteration() const noexcept
    {
        return logicalIteration_;
    }
    std::uint64_t globalOffset() const noexcept
    {
        return globalOffset_;
    }
    std::uint64_t scratchpadOffset() const noexcept
    {
        return scratchpadOffset_;
    }
    std::uint32_t byteCount() const noexcept
    {
        return byteCount_;
    }
    GlobalDMADirection direction() const noexcept
    {
        return direction_;
    }
    std::uint32_t requestFlags() const noexcept
    {
        return requestFlags_;
    }
    bool completion() const noexcept
    {
        return completion_;
    }

    void markCompletion() noexcept
    {
        completion_ = true;
    }

  private:
    GlobalDMAMessage() = default;

    std::uint32_t tileId_ = 0;
    std::uint64_t executionId_ = 0;
    std::uint32_t tokenId_ = 0;
    std::uint64_t logicalIteration_ = 0;
    std::uint64_t globalOffset_ = 0;
    std::uint64_t scratchpadOffset_ = 0;
    std::uint32_t byteCount_ = 0;
    GlobalDMADirection direction_ = GlobalDMADirection::GlobalRAMToScratchpad;
    std::uint32_t requestFlags_ = GlobalDMARequestNone;
    bool completion_ = false;
};
} // namespace SST::Mittens
