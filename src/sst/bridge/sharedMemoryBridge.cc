#include "sst_config.h"

#include "sharedMemoryBridge.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#include <linux/memfd.h>
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
        syscall(SYS_memfd_create, name.c_str(), MFD_CLOEXEC));
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
    mapping_->rx_dma_timing_enabled = 1;
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

std::optional<MittensBridgeTxBurst> SharedMemoryBridge::popTransmitBurst()
{
    if (!open()) {
        throw std::logic_error("cannot read a closed Mittens bridge");
    }

    MittensBridgeTxBurst burst{};
    if (!mittens_bridge_tx_burst_pop(mapping_, &burst)) {
        return std::nullopt;
    }
    return burst;
}

std::optional<MittensBridgeTxBurst> SharedMemoryBridge::peekTransmitBurst(
    std::uint32_t offset) const noexcept
{
    if (!open()) {
        return std::nullopt;
    }
    const std::uint32_t readIndex = mittens_bridge_load_relaxed(
        &mapping_->burst_transmit.read_index);
    const std::uint32_t writeIndex = mittens_bridge_load_acquire(
        &mapping_->burst_transmit.write_index);
    if (offset >= static_cast<std::uint32_t>(writeIndex - readIndex)) {
        return std::nullopt;
    }
    return mapping_->burst_transmit.bursts[
        (readIndex + offset) % MITTENS_BRIDGE_BURST_QUEUE_CAPACITY];
}

std::uint32_t SharedMemoryBridge::transmitCount() const noexcept
{
    if (!open()) {
        return 0;
    }
    return static_cast<std::uint32_t>(
        mittens_bridge_load_acquire(
            &mapping_->transmit.write_index) -
        mittens_bridge_load_acquire(
            &mapping_->transmit.read_index));
}

std::uint32_t SharedMemoryBridge::transmitBurstCount() const noexcept
{
    if (!open()) {
        return 0;
    }
    return static_cast<std::uint32_t>(
        mittens_bridge_load_acquire(
            &mapping_->burst_transmit.write_index) -
        mittens_bridge_load_acquire(
            &mapping_->burst_transmit.read_index));
}

bool SharedMemoryBridge::transmitHasSpace() const noexcept
{
    return open() && mittens_bridge_tx_ready(mapping_);
}

bool SharedMemoryBridge::transmitBurstHasSpace() const noexcept
{
    return open() && mittens_bridge_tx_burst_ready(mapping_);
}

bool SharedMemoryBridge::receiveHasData() const noexcept
{
    return open() &&
           (mittens_bridge_rx_valid(mapping_) ||
            mittens_bridge_rx_burst_valid(mapping_));
}

bool SharedMemoryBridge::receiveHasWordData() const noexcept
{
    return open() && mittens_bridge_rx_valid(mapping_);
}

bool SharedMemoryBridge::receiveHasSpace() const noexcept
{
    return open() && mittens_bridge_rx_space(mapping_);
}

bool SharedMemoryBridge::pushReceive(std::uint32_t source,
                                     std::uint32_t payload)
{
    if (!open()) {
        throw std::logic_error("cannot write a closed Mittens bridge");
    }
    return mittens_bridge_rx_push(
        mapping_, MittensBridgeRxPacket{source, payload});
}

bool SharedMemoryBridge::receiveHasBurstSpace() const noexcept
{
    return open() && mittens_bridge_rx_burst_space(mapping_);
}

std::uint32_t SharedMemoryBridge::receiveBurstCount() const noexcept
{
    if (!open()) {
        return 0;
    }
    return mittens_bridge_rx_burst_count(mapping_);
}

std::uint32_t SharedMemoryBridge::receiveBurstReadIndex() const noexcept
{
    if (!open()) {
        return 0;
    }
    return mittens_bridge_rx_burst_read_index(mapping_);
}

std::uint32_t SharedMemoryBridge::receiveBurstWriteIndex() const noexcept
{
    if (!open()) {
        return 0;
    }
    return mittens_bridge_rx_burst_write_index(mapping_);
}

std::optional<ReceiveBurstInfo> SharedMemoryBridge::peekReceiveBurst(
    std::uint32_t offset) const noexcept
{
    if (!open()) {
        return std::nullopt;
    }

    std::uint32_t absoluteIndex = 0;
    const MittensBridgeRxBurst* burst = nullptr;
    if (!mittens_bridge_rx_burst_peek_at(
            mapping_, offset, &absoluteIndex, &burst)) {
        return std::nullopt;
    }
    return ReceiveBurstInfo{
        absoluteIndex,
        burst->source,
        burst->word_count,
        burst->software_visible != 0,
    };
}

bool SharedMemoryBridge::pushReceiveBurst(
    std::uint32_t source,
    const std::vector<std::uint32_t>& payload,
    bool softwareVisible)
{
    if (!open() ||
        payload.empty() ||
        payload.size() > MITTENS_BRIDGE_BURST_WORD_CAPACITY) {
        return false;
    }
    return mittens_bridge_rx_burst_push_tagged(
        mapping_,
        source,
        payload.data(),
        static_cast<std::uint32_t>(payload.size()),
        softwareVisible);
}

bool SharedMemoryBridge::receiveDMAAuthorizationAvailable() const noexcept
{
    return open() &&
           mittens_bridge_rx_dma_authorization_available(mapping_);
}

bool SharedMemoryBridge::receiveDMACompletionAvailable() const noexcept
{
    return open() &&
           mittens_bridge_rx_dma_completion_available(mapping_);
}

bool SharedMemoryBridge::authorizeReceiveDMA()
{
    if (!open()) {
        throw std::logic_error(
            "cannot authorize RX DMA on a closed Mittens bridge");
    }
    return mittens_bridge_rx_dma_authorize(mapping_);
}

} // namespace Mittens
} // namespace SST
