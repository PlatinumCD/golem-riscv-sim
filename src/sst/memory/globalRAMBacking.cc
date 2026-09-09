#include "globalRAMBacking.h"

#include <cerrno>
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

} // namespace Mittens
} // namespace SST
