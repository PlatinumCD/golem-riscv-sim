#include "../memory/scratchpadBootImage.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace SST::Mittens;

namespace
{

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "scratchpad boot image test: %s\n", message);
        std::exit(1);
    }
}

void put16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    put16(bytes, offset, static_cast<std::uint16_t>(value));
    put16(bytes, offset + 2, static_cast<std::uint16_t>(value >> 16));
}

void put64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value)
{
    put32(bytes, offset, static_cast<std::uint32_t>(value));
    put32(bytes, offset + 4, static_cast<std::uint32_t>(value >> 32));
}

std::vector<std::uint8_t> goodElf()
{
    std::vector<std::uint8_t> bytes(0x220, 0);
    bytes[0] = 0x7f;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 2;
    bytes[5] = 1;
    bytes[6] = 1;
    put16(bytes, 16, 2);
    put16(bytes, 18, 0xf3);
    put32(bytes, 20, 1);
    put64(bytes, 24, 0x1004);
    put64(bytes, 32, 64);
    put16(bytes, 52, 64);
    put16(bytes, 54, 56);
    put16(bytes, 56, 1);
    put32(bytes, 64, 1);
    put32(bytes, 68, 1);
    put64(bytes, 72, 0x200);
    put64(bytes, 80, 0x1000);
    put64(bytes, 88, 0x1000);
    put64(bytes, 96, 4);
    put64(bytes, 104, 8);
    put64(bytes, 112, 16);
    bytes[0x200] = 1;
    bytes[0x201] = 2;
    bytes[0x202] = 3;
    bytes[0x203] = 4;
    return bytes;
}

class TemporaryElf final
{
  public:
    explicit TemporaryElf(const std::vector<std::uint8_t>& bytes)
    {
        char pattern[] = "/tmp/scratchpad-elf-XXXXXX";
        descriptor_ = ::mkstemp(pattern);
        require(descriptor_ >= 0, "mkstemp failed");
        path_ = pattern;
        const auto written = ::write(descriptor_, bytes.data(), bytes.size());
        require(written == static_cast<ssize_t>(bytes.size()), "temporary ELF write failed");
        ::close(descriptor_);
        descriptor_ = -1;
    }

    ~TemporaryElf()
    {
        if (descriptor_ >= 0)
            ::close(descriptor_);
        if (!path_.empty())
            ::unlink(path_.c_str());
    }

    const std::string& path() const
    {
        return path_;
    }

  private:
    int descriptor_ = -1;
    std::string path_;
};

void rejects(const std::vector<std::uint8_t>& bytes, const char* name)
{
    TemporaryElf elf(bytes);
    try
    {
        (void)loadScratchpadBootImage(elf.path(), 0x1000, 0x1000);
    }
    catch (const std::exception&)
    {
        return;
    }
    require(false, name);
}

} // namespace

int main()
{
    const auto valid = goodElf();
    TemporaryElf validFile(valid);
    const auto image = loadScratchpadBootImage(validFile.path(), 0x1000, 0x1000);
    require(image.imageEntry == 0x1004, "valid ELF entry was not preserved");
    require(image.loadedByteCount == 8 && image.segments.size() == 1,
            "valid ELF segment accounting was incorrect");
    require(image.segments[0].spmOffset == 0 && image.segments[0].bytes[3] == 4 &&
                image.segments[0].bytes[7] == 0,
            "valid ELF payload or BSS zero fill was incorrect");

    auto wrongMagic = valid;
    wrongMagic[0] = 0;
    rejects(wrongMagic, "wrong ELF magic was accepted");
    auto wrongClass = valid;
    wrongClass[4] = 1;
    rejects(wrongClass, "non-ELF64 image was accepted");
    auto wrongEndian = valid;
    wrongEndian[5] = 2;
    rejects(wrongEndian, "big-endian image was accepted");
    auto wrongMachine = valid;
    put16(wrongMachine, 18, 62);
    rejects(wrongMachine, "non-RISC-V image was accepted");
    auto wrongVersion = valid;
    put32(wrongVersion, 20, 2);
    rejects(wrongVersion, "unsupported ELF version was accepted");
    rejects(std::vector<std::uint8_t>(valid.begin(), valid.begin() + 70),
            "truncated program header was accepted");

    auto badFileSize = valid;
    put64(badFileSize, 96, 9);
    put64(badFileSize, 104, 8);
    rejects(badFileSize, "filesz greater than memsz was accepted");
    auto badPhysical = valid;
    put64(badPhysical, 88, 0x2000);
    rejects(badPhysical, "different physical and virtual addresses were accepted");
    auto oddEntry = valid;
    put64(oddEntry, 24, 0x1005);
    rejects(oddEntry, "odd entry was accepted");
    auto nonExecutable = valid;
    put32(nonExecutable, 68, 0);
    rejects(nonExecutable, "entry outside executable segment was accepted");
    auto outsideEntry = valid;
    put64(outsideEntry, 24, 0x1800);
    rejects(outsideEntry, "entry outside load segment was accepted");
    auto outsideSegment = valid;
    put64(outsideSegment, 80, 0x1ff0);
    put64(outsideSegment, 88, 0x1ff0);
    rejects(outsideSegment, "load segment outside SPM was accepted");
    auto overflowingSegment = valid;
    put64(overflowingSegment, 80, UINT64_MAX - 3);
    put64(overflowingSegment, 88, UINT64_MAX - 3);
    rejects(overflowingSegment, "overflowing load segment was accepted");

    auto zeroSegment = valid;
    put64(zeroSegment, 96, 0);
    put64(zeroSegment, 104, 0);
    rejects(zeroSegment, "image with only zero-memory load was accepted");

    auto overlap = valid;
    put16(overlap, 56, 2);
    overlap.resize(0x260, 0);
    put32(overlap, 120, 1);
    put32(overlap, 124, 1);
    put64(overlap, 128, 0x210);
    put64(overlap, 136, 0x1004);
    put64(overlap, 144, 0x1004);
    put64(overlap, 152, 4);
    put64(overlap, 160, 8);
    put64(overlap, 168, 16);
    rejects(overlap, "overlapping load segments were accepted");

    char backingPattern[] = "/tmp/scratchpad-backing-XXXXXX";
    const int backing = ::mkstemp(backingPattern);
    require(backing >= 0, "backing mkstemp failed");
    require(::ftruncate(backing, 0x208) == 0, "backing resize failed");
    const std::uint8_t sentinel = 0xa5;
    require(::pwrite(backing, &sentinel, 1, 0x200) == 1, "backing sentinel write failed");
    struct stat before{};
    require(::fstat(backing, &before) == 0, "backing stat failed");
    try
    {
        seedScratchpadBootImage(backing, 0x204, image);
        require(false, "seed beyond backing capacity was accepted");
    }
    catch (const std::exception&)
    {
    }
    struct stat after{};
    require(::fstat(backing, &after) == 0 && after.st_size == before.st_size,
            "failed seed resized the backing");
    std::uint8_t observed = 0;
    require(::pread(backing, &observed, 1, 0x200) == 1 && observed == sentinel,
            "failed seed modified backing before validation completed");
    ::close(backing);
    ::unlink(backingPattern);

    std::puts("scratchpad boot image test: PASS");
}
