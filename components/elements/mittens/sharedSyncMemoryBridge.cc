#include "sharedSyncMemoryBridge.h"

#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

#include <linux/memfd.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace SST {
namespace Mittens {

SharedSyncMemoryBridge::~SharedSyncMemoryBridge()
{
    close();
}

void SharedSyncMemoryBridge::create(std::uint32_t tileId)
{
    if (open()) {
        throw std::logic_error(
            "shared synchronization bridge is already open");
    }

#if defined(SYS_memfd_create)
    const std::string name =
        "mittens-sync-tile-" + std::to_string(tileId);
    fileDescriptor_ = static_cast<int>(
        syscall(SYS_memfd_create, name.c_str(), MFD_CLOEXEC));
#else
    throw std::runtime_error(
        "memfd_create is not supported on this host");
#endif

    if (fileDescriptor_ < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "memfd_create failed for Mittens synchronization bridge");
    }

    if (ftruncate(
            fileDescriptor_,
            static_cast<off_t>(sizeof(MittensSyncBridge))) < 0) {
        const int error = errno;
        close();
        throw std::system_error(
            error,
            std::generic_category(),
            "ftruncate failed for Mittens synchronization bridge");
    }

    void* const address = mmap(
        nullptr,
        sizeof(MittensSyncBridge),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fileDescriptor_,
        0);
    if (address == MAP_FAILED) {
        const int error = errno;
        mapping_ = nullptr;
        close();
        throw std::system_error(
            error,
            std::generic_category(),
            "mmap failed for Mittens synchronization bridge");
    }

    mapping_ = static_cast<MittensSyncBridge*>(address);
    std::memset(mapping_, 0, sizeof(*mapping_));
    mapping_->magic = MITTENS_SYNC_BRIDGE_MAGIC;
    mapping_->version = MITTENS_SYNC_BRIDGE_VERSION;
    mapping_->structure_size = sizeof(MittensSyncBridge);
    mapping_->tile_id = tileId;
    mapping_->state = MITTENS_SYNC_STATE_IDLE;
    observedEventSequence_ = 0;
}

void SharedSyncMemoryBridge::close() noexcept
{
    if (mapping_ != nullptr) {
        wake(&mapping_->state);
        (void)munmap(mapping_, sizeof(MittensSyncBridge));
        mapping_ = nullptr;
    }

    if (fileDescriptor_ >= 0) {
        (void)::close(fileDescriptor_);
        fileDescriptor_ = -1;
    }
    observedEventSequence_ = 0;
}

std::uint32_t SharedSyncMemoryBridge::protocolError() const noexcept
{
    if (!open()) {
        return MITTENS_SYNC_BRIDGE_ERROR_NONE;
    }
    return mittens_sync_load_acquire(&mapping_->protocol_error);
}

std::uint64_t SharedSyncMemoryBridge::grant(
    std::uint64_t instructionBudget)
{
    if (!open()) {
        throw std::logic_error(
            "cannot grant a closed synchronization bridge");
    }
    if (instructionBudget == 0 ||
        instructionBudget >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
        setProtocolError(MITTENS_SYNC_BRIDGE_ERROR_BAD_BUDGET);
        throw std::invalid_argument(
            "QEMU synchronization instruction budget is invalid");
    }

    requireState(
        MITTENS_SYNC_STATE_IDLE,
        MITTENS_SYNC_STATE_EVENT,
        "grant");

    const std::uint64_t epoch = mapping_->grant_epoch + UINT64_C(1);
    mapping_->grant_epoch = epoch;
    mapping_->instruction_budget = instructionBudget;
    mapping_->instructions_executed = 0;
    mapping_->stop_reason = MITTENS_SYNC_STOP_NONE;
    mapping_->event_flags = MITTENS_SYNC_EVENT_FLAG_NONE;
    mapping_->analog_array_id = UINT32_MAX;
    mapping_->analog_sequence = 0;
    mittens_sync_store_release(
        &mapping_->state, MITTENS_SYNC_STATE_GRANTED);
    wake(&mapping_->state);
    return epoch;
}

std::optional<QemuSyncEvent> SharedSyncMemoryBridge::waitForEvent(
    std::chrono::milliseconds timeout)
{
    if (!open()) {
        throw std::logic_error(
            "cannot wait on a closed synchronization bridge");
    }

    const std::uint32_t state =
        mittens_sync_load_acquire(&mapping_->state);
    if (state == MITTENS_SYNC_STATE_EVENT &&
        mapping_->event_sequence != observedEventSequence_) {
        observedEventSequence_ = mapping_->event_sequence;
        return QemuSyncEvent{
            mapping_->grant_epoch,
            mapping_->event_sequence,
            mapping_->instructions_executed,
            mapping_->stop_reason,
            mapping_->event_flags,
            mapping_->analog_array_id,
            mapping_->analog_sequence,
        };
    }

    wait(&mapping_->state, state, timeout);

    if (mittens_sync_load_acquire(&mapping_->state) !=
        MITTENS_SYNC_STATE_EVENT) {
        return std::nullopt;
    }
    if (mapping_->event_sequence == observedEventSequence_) {
        return std::nullopt;
    }

    observedEventSequence_ = mapping_->event_sequence;
    return QemuSyncEvent{
        mapping_->grant_epoch,
        mapping_->event_sequence,
        mapping_->instructions_executed,
        mapping_->stop_reason,
        mapping_->event_flags,
        mapping_->analog_array_id,
        mapping_->analog_sequence,
    };
}

void SharedSyncMemoryBridge::resume(const QemuSyncEvent& event)
{
    if (!open()) {
        throw std::logic_error(
            "cannot resume a closed synchronization bridge");
    }
    requireState(
        MITTENS_SYNC_STATE_EVENT,
        MITTENS_SYNC_STATE_EVENT,
        "resume");
    if (mapping_->grant_epoch != event.grantEpoch ||
        mapping_->event_sequence != event.eventSequence) {
        setProtocolError(MITTENS_SYNC_BRIDGE_ERROR_BAD_STATE);
        throw std::logic_error(
            "cannot resume a stale QEMU synchronization event");
    }

    mittens_sync_store_release(
        &mapping_->state, MITTENS_SYNC_STATE_RESUME);
    wake(&mapping_->state);
}

void SharedSyncMemoryBridge::wake(
    std::uint32_t* state) noexcept
{
#if defined(SYS_futex)
    (void)syscall(
        SYS_futex,
        state,
        FUTEX_WAKE,
        std::numeric_limits<int>::max(),
        nullptr,
        nullptr,
        0);
#else
    (void)state;
#endif
}

void SharedSyncMemoryBridge::wait(
    std::uint32_t* state,
    std::uint32_t expected,
    std::chrono::milliseconds timeout) noexcept
{
#if defined(SYS_futex)
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            timeout - seconds);
    const struct timespec interval{
        static_cast<time_t>(seconds.count()),
        static_cast<long>(nanoseconds.count()),
    };
    (void)syscall(
        SYS_futex,
        state,
        FUTEX_WAIT,
        expected,
        &interval,
        nullptr,
        0);
#else
    (void)state;
    (void)expected;
    (void)timeout;
#endif
}

void SharedSyncMemoryBridge::requireState(
    std::uint32_t first,
    std::uint32_t second,
    const char* operation)
{
    const std::uint32_t state =
        mittens_sync_load_acquire(&mapping_->state);
    if (state == first || state == second) {
        return;
    }

    setProtocolError(MITTENS_SYNC_BRIDGE_ERROR_BAD_STATE);
    throw std::logic_error(
        std::string("cannot ") + operation +
        " QEMU synchronization bridge in state " +
        std::to_string(state));
}

void SharedSyncMemoryBridge::setProtocolError(
    enum MittensSyncBridgeError error) noexcept
{
    if (!open()) {
        return;
    }
    std::uint32_t expected = MITTENS_SYNC_BRIDGE_ERROR_NONE;
    (void)__atomic_compare_exchange_n(
        &mapping_->protocol_error,
        &expected,
        static_cast<std::uint32_t>(error),
        false,
        __ATOMIC_RELEASE,
        __ATOMIC_RELAXED);
}

} // namespace Mittens
} // namespace SST
