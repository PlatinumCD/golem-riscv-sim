#include "globalRAMBacking.h"

#include <cerrno>
#include <array>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace SST {
namespace Mittens {

namespace {

std::mutex backingMutex;
int backingFileDescriptor = -1;
std::uint64_t backingCapacity = 0;

int createMemfd()
{
#if defined(SYS_memfd_create)
    const int descriptor = static_cast<int>(::syscall(
        SYS_memfd_create, "mittens-global-ram", MFD_CLOEXEC));
    if (descriptor >= 0) {
        return descriptor;
    }
    throw std::system_error(
        errno, std::generic_category(), "cannot create global RAM memfd");
#else
    throw std::runtime_error("memfd_create is unavailable on this host");
#endif
}

} // namespace

int GlobalRAMBacking::duplicate(std::uint64_t capacityBytes)
{
    if (capacityBytes == 0 || capacityBytes > INT64_MAX) {
        throw std::invalid_argument("global RAM capacity is invalid");
    }

    const std::lock_guard<std::mutex> lock(backingMutex);
    if (backingFileDescriptor < 0) {
        backingFileDescriptor = createMemfd();
        if (::ftruncate(
                backingFileDescriptor,
                static_cast<off_t>(capacityBytes)) != 0) {
            const int error = errno;
            (void)::close(backingFileDescriptor);
            backingFileDescriptor = -1;
            throw std::system_error(
                error, std::generic_category(),
                "cannot size sparse global RAM backing");
        }
        backingCapacity = capacityBytes;
    } else if (backingCapacity != capacityBytes) {
        throw std::invalid_argument(
            "all global RAM users must request one deployment capacity");
    }

    const int duplicate = ::fcntl(backingFileDescriptor, F_DUPFD_CLOEXEC, 3);
    if (duplicate < 0) {
        throw std::system_error(
            errno, std::generic_category(),
            "cannot duplicate global RAM backing descriptor");
    }
    return duplicate;
}

void GlobalRAMBacking::loadImage(int descriptor, std::uint64_t capacityBytes,
                                 const std::string& path, std::uint64_t offset)
{
    std::ifstream image(path, std::ios::binary | std::ios::ate);
    if (!image || image.tellg() < 0) {
        throw std::runtime_error("cannot open global RAM image: " + path);
    }
    const auto bytes = static_cast<std::uint64_t>(image.tellg());
    if (descriptor < 0 || capacityBytes > INT64_MAX ||
        offset > capacityBytes || bytes > capacityBytes - offset) {
        throw std::invalid_argument("global RAM image exceeds backing capacity");
    }
    image.seekg(0);
    std::array<char, 65536> buffer;
    std::uint64_t remaining = bytes;
    while (remaining != 0) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        if (!image.read(buffer.data(), count)) {
            throw std::runtime_error("cannot read global RAM image: " + path);
        }
        std::size_t done = 0;
        while (done < count) {
            const ssize_t written = ::pwrite(
                descriptor, buffer.data() + done, count - done,
                static_cast<off_t>(offset + done));
            if (written < 0) {
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(),
                                        "cannot load global RAM image");
            }
            if (written == 0) {
                throw std::runtime_error("short global RAM image write");
            }
            done += static_cast<std::size_t>(written);
        }
        offset += count;
        remaining -= count;
    }
}

} // namespace Mittens
} // namespace SST
