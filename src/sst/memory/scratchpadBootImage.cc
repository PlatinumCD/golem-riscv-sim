#include "scratchpadBootImage.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace SST::Mittens
{
namespace
{
constexpr std::size_t HeaderSize = 64;
constexpr std::size_t ProgramHeaderSize = 56;
constexpr std::uint64_t MaximumFileBytes = 128 * 1024 * 1024;
constexpr std::uint32_t Load = 1;
constexpr std::uint32_t Execute = 1;
std::uint16_t u16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return bytes[offset] | (std::uint16_t(bytes[offset + 1]) << 8);
}

std::uint32_t u32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return u16(bytes, offset) | (std::uint32_t(u16(bytes, offset + 2)) << 16);
}

std::uint64_t u64(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return u32(bytes, offset) | (std::uint64_t(u32(bytes, offset + 4)) << 32);
}
std::uint64_t add(std::uint64_t left, std::uint64_t right, const char* what)
{
    if (left > std::numeric_limits<std::uint64_t>::max() - right)
        throw std::invalid_argument(what);
    return left + right;
}
bool representable(std::uint64_t base, std::uint64_t bytes)
{
    return base <= std::numeric_limits<std::uint64_t>::max() - bytes;
}
void requireRange(std::uint64_t offset, std::uint64_t bytes, std::uint64_t total, const char* what)
{
    if (offset > total || bytes > total - offset)
        throw std::invalid_argument(what);
}
} // namespace

ScratchpadBootImage loadScratchpadBootImage(const std::string& path, std::uint64_t base,
                                            std::uint64_t spmBytes, std::uint64_t maxBytes)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::system_error(errno, std::generic_category(), "cannot open boot ELF");
    const auto size = file.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) < HeaderSize ||
        static_cast<std::uint64_t>(size) > MaximumFileBytes)
        throw std::invalid_argument("ELF file size is invalid or exceeds limit");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file)
        throw std::runtime_error("cannot read boot ELF");
    if (data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F' || data[4] != 2 ||
        data[5] != 1 || data[6] != 1 || u16(data, 16) != 2 || u16(data, 18) != 0xf3 ||
        u32(data, 20) != 1 || u16(data, 52) != HeaderSize || u16(data, 54) != ProgramHeaderSize)
        throw std::invalid_argument("unsupported ELF64 little-endian RISC-V image");
    const auto entry = u64(data, 24), phoff = u64(data, 32);
    if ((entry & 1) != 0)
        throw std::invalid_argument("ELF entry is not halfword aligned");
    const auto phents = u16(data, 54), phnum = u16(data, 56);
    requireRange(phoff, std::uint64_t(phents) * phnum, data.size(), "program headers outside ELF");
    ScratchpadBootImage result{entry, 0, {}};
    bool entryOK = false;
    for (std::uint16_t i = 0; i < phnum; ++i)
    {
        const auto p = phoff + std::uint64_t(i) * phents;
        if (u32(data, p) != Load)
            continue;
        const auto flags = u32(data, p + 4);
        const auto offset = u64(data, p + 8);
        const auto vaddr = u64(data, p + 16);
        const auto paddr = u64(data, p + 24);
        const auto filesz = u64(data, p + 32);
        const auto memsz = u64(data, p + 40);
        if (filesz > memsz)
            throw std::invalid_argument("ELF PT_LOAD filesz exceeds memsz");
        if (paddr != vaddr)
            throw std::invalid_argument("ELF PT_LOAD physical address differs");
        if (memsz == 0)
            continue;
        requireRange(offset, filesz, data.size(), "ELF PT_LOAD file range outside image");
        if (!representable(base, spmBytes) || vaddr < base || memsz > spmBytes ||
            vaddr - base > spmBytes - memsz)
            throw std::invalid_argument("ELF PT_LOAD outside SPM");
        if (add(result.loadedByteCount, memsz, "ELF loaded-byte overflow") > maxBytes)
            throw std::invalid_argument("ELF image exceeds bounded load size");
        for (const auto& old : result.segments)
        {
            if (vaddr - base < old.spmOffset + old.bytes.size() &&
                old.spmOffset < vaddr - base + memsz)
                throw std::invalid_argument("ELF PT_LOAD segments overlap");
        }
        ScratchpadBootSegment segment{
            vaddr - base, std::vector<std::uint8_t>(static_cast<std::size_t>(memsz), 0)};
        std::copy_n(data.begin() + offset, static_cast<std::size_t>(filesz), segment.bytes.begin());
        result.segments.push_back(std::move(segment));
        result.loadedByteCount = add(result.loadedByteCount, memsz, "ELF loaded-byte overflow");
        if ((flags & Execute) && entry >= vaddr && entry - vaddr < memsz)
            entryOK = true;
    }
    if (!entryOK || result.segments.empty())
        throw std::invalid_argument("ELF entry is not in an executable PT_LOAD");
    return result;
}

void seedScratchpadBootImage(int fd, std::uint64_t region, const ScratchpadBootImage& image)
{
    if (fd < 0)
        throw std::invalid_argument("invalid GlobalRAMBacking descriptor");
    struct stat status{};
    if (::fstat(fd, &status) != 0)
        throw std::system_error(errno, std::generic_category(), "cannot stat GlobalRAM backing");
    if (status.st_size < 0)
        throw std::invalid_argument("GlobalRAM backing has invalid size");
    const auto capacity = static_cast<std::uint64_t>(status.st_size);
    for (const auto& segment : image.segments)
    {
        const auto at = add(region, segment.spmOffset, "GlobalRAM seed offset overflow");
        requireRange(at, segment.bytes.size(), capacity,
                     "boot image does not fit GlobalRAM backing");
    }
    for (const auto& segment : image.segments)
    {
        const auto base = add(region, segment.spmOffset, "GlobalRAM seed offset overflow");
        std::size_t done = 0;
        while (done < segment.bytes.size())
        {
            const auto at = add(base, done, "GlobalRAM seed offset overflow");
            if (at > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
                throw std::invalid_argument("GlobalRAM seed offset exceeds host file offset");
            const auto count = std::min<std::size_t>(segment.bytes.size() - done, 1 << 20);
            ssize_t written;
            do
            {
                written = ::pwrite(fd, segment.bytes.data() + done, count, static_cast<off_t>(at));
            } while (written < 0 && errno == EINTR);
            if (written < 0)
                throw std::system_error(errno, std::generic_category(),
                                        "cannot seed GlobalRAM boot image");
            if (written == 0)
                throw std::runtime_error("GlobalRAM boot image write made no progress");
            done += static_cast<std::size_t>(written);
        }
    }
}
} // namespace SST::Mittens
