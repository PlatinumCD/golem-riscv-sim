#include "sst_config.h"

#include "sharedMemoryBridge.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace SST {
namespace Mittens {

SharedMemoryBridge::~SharedMemoryBridge()
{
    close();
}

void SharedMemoryBridge::create(std::uint32_t tileId)
{
    if (open()) {
        throw std::logic_error("shared-memory bridge is already open");
    }

#if defined(SYS_memfd_create)
    const std::string name = "mittens-tile-" + std::to_string(tileId);
    fileDescriptor_ = static_cast<int>(
        syscall(SYS_memfd_create, name.c_str(), 0));
#else
    throw std::runtime_error("memfd_create is not supported on this host");
#endif

    if (fileDescriptor_ < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "memfd_create failed for Mittens bridge");
    }

    if (ftruncate(fileDescriptor_, sizeof(MittensBridgeShared)) < 0) {
        const int error = errno;
        close();
        throw std::system_error(error, std::generic_category(),
                                "ftruncate failed for Mittens bridge");
    }

    void* const address = mmap(nullptr,
                               sizeof(MittensBridgeShared),
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED,
                               fileDescriptor_,
                               0);
    if (address == MAP_FAILED) {
        const int error = errno;
        mapping_ = nullptr;
        close();
        throw std::system_error(error, std::generic_category(),
                                "mmap failed for Mittens bridge");
    }

    mapping_ = static_cast<MittensBridgeShared*>(address);
    std::memset(mapping_, 0, sizeof(*mapping_));
    mapping_->magic = MITTENS_BRIDGE_MAGIC;
    mapping_->version = MITTENS_BRIDGE_VERSION;
    mapping_->structure_size = sizeof(MittensBridgeShared);
    mapping_->queue_capacity = MITTENS_BRIDGE_QUEUE_CAPACITY;
    mapping_->tile_id = tileId;
}

void SharedMemoryBridge::close() noexcept
{
    if (mapping_ != nullptr) {
        (void)munmap(mapping_, sizeof(MittensBridgeShared));
        mapping_ = nullptr;
    }

    if (fileDescriptor_ >= 0) {
        (void)::close(fileDescriptor_);
        fileDescriptor_ = -1;
    }
}

std::uint32_t SharedMemoryBridge::protocolError() const noexcept
{
    if (!open()) {
        return MITTENS_BRIDGE_ERROR_NONE;
    }
    return mittens_bridge_load_acquire(&mapping_->protocol_error);
}

std::optional<MittensBridgePacket> SharedMemoryBridge::popTransmit()
{
    if (!open()) {
        throw std::logic_error("cannot read a closed Mittens bridge");
    }

    MittensBridgePacket packet{};
    if (!mittens_bridge_tx_pop(mapping_, &packet)) {
        return std::nullopt;
    }
    return packet;
}

bool SharedMemoryBridge::receiveHasSpace() const noexcept
{
    return open() && mittens_bridge_rx_space(mapping_);
}

bool SharedMemoryBridge::pushReceive(std::uint32_t payload)
{
    if (!open()) {
        throw std::logic_error("cannot write a closed Mittens bridge");
    }
    return mittens_bridge_rx_push(mapping_, payload);
}

} // namespace Mittens
} // namespace SST
