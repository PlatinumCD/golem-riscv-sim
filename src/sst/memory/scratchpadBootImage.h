#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SST::Mittens
{

struct ScratchpadBootSegment
{
    std::uint64_t spmOffset;
    std::vector<std::uint8_t> bytes;
};

struct ScratchpadBootImage
{
    std::uint64_t imageEntry;
    std::uint64_t loadedByteCount;
    std::vector<ScratchpadBootSegment> segments;
};

// Parses and validates a little-endian RV64 ELF image. No backing-file state
// is changed unless parsing and all validation succeed.
ScratchpadBootImage loadScratchpadBootImage(const std::string& path, std::uint64_t spmBase,
                                            std::uint64_t spmBytes,
                                            std::uint64_t maxLoadedBytes = 64 * 1024 * 1024);

// Seeds the caller-provided GlobalRAMBacking descriptor. The caller owns and
// validates globalRegionOffset; this function performs checked pwrite calls.
void seedScratchpadBootImage(int globalRAMFd, std::uint64_t globalRegionOffset,
                             const ScratchpadBootImage& image);

} // namespace SST::Mittens
