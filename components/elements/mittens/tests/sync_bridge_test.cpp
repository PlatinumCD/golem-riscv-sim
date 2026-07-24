#include "sharedSyncMemoryBridge.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

using SST::Mittens::QemuSyncEvent;
using SST::Mittens::SharedSyncMemoryBridge;

namespace {

void wake(std::uint32_t* state)
{
    (void)syscall(
        SYS_futex,
        state,
        FUTEX_WAKE,
        std::numeric_limits<int>::max(),
        nullptr,
        nullptr,
        0);
}

void waitWhile(std::uint32_t* state, std::uint32_t expected)
{
    while (mittens_sync_load_acquire(state) == expected) {
        (void)syscall(
            SYS_futex,
            state,
            FUTEX_WAIT,
            expected,
            nullptr,
            nullptr,
            0);
    }
}

void publish(
    MittensSyncBridge* mapping,
    std::uint32_t reason,
    std::uint64_t executed,
    std::uint32_t flags,
    std::uint32_t arrayId,
    std::uint64_t analogSequence)
{
    assert(
        mittens_sync_load_acquire(&mapping->state) ==
        MITTENS_SYNC_STATE_RUNNING);
    mapping->instructions_executed = executed;
    mapping->stop_reason = reason;
    mapping->event_flags = flags;
    mapping->analog_array_id = arrayId;
    mapping->analog_sequence = analogSequence;
    ++mapping->event_sequence;
    mittens_sync_store_release(
        &mapping->state, MITTENS_SYNC_STATE_EVENT);
    wake(&mapping->state);
}

} // namespace

int main()
{
    SharedSyncMemoryBridge bridge;
    bridge.create(9);

    void* const address = mmap(
        nullptr,
        sizeof(MittensSyncBridge),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        bridge.fileDescriptor(),
        0);
    assert(address != MAP_FAILED);
    auto* const mapping =
        static_cast<MittensSyncBridge*>(address);

    assert(mapping->magic == MITTENS_SYNC_BRIDGE_MAGIC);
    assert(mapping->version == MITTENS_SYNC_BRIDGE_VERSION);
    assert(mapping->tile_id == 9);

    std::thread qemu([mapping]() {
        waitWhile(&mapping->state, MITTENS_SYNC_STATE_IDLE);
        assert(
            mittens_sync_load_acquire(&mapping->state) ==
            MITTENS_SYNC_STATE_GRANTED);
        assert(mapping->instruction_budget == 100);
        mittens_sync_store_release(
            &mapping->state, MITTENS_SYNC_STATE_RUNNING);
        wake(&mapping->state);

        publish(
            mapping,
            MITTENS_SYNC_STOP_ANALOG_SUBMIT,
            17,
            MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION,
            2,
            11);

        waitWhile(&mapping->state, MITTENS_SYNC_STATE_EVENT);
        assert(
            mittens_sync_load_acquire(&mapping->state) ==
            MITTENS_SYNC_STATE_RESUME);
        mittens_sync_store_release(
            &mapping->state, MITTENS_SYNC_STATE_RUNNING);
        wake(&mapping->state);

        publish(
            mapping,
            MITTENS_SYNC_STOP_QUANTUM_END,
            100,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            0);

        waitWhile(&mapping->state, MITTENS_SYNC_STATE_EVENT);
        assert(
            mittens_sync_load_acquire(&mapping->state) ==
            MITTENS_SYNC_STATE_GRANTED);
        assert(mapping->instruction_budget == 50);
        mittens_sync_store_release(
            &mapping->state, MITTENS_SYNC_STATE_RUNNING);
        wake(&mapping->state);

        publish(
            mapping,
            MITTENS_SYNC_STOP_GUEST_EXIT,
            3,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            0);
    });

    const std::uint64_t epoch = bridge.grant(100);
    assert(epoch == 1);

    std::optional<QemuSyncEvent> analog;
    do {
        analog = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!analog.has_value());
    assert(analog->grantEpoch == 1);
    assert(analog->instructionsExecuted == 17);
    assert(
        analog->stopReason ==
        MITTENS_SYNC_STOP_ANALOG_SUBMIT);
    assert(
        analog->flags ==
        MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION);
    assert(analog->analogArrayId == 2);
    assert(analog->analogSequence == 11);

    bridge.resume(*analog);

    std::optional<QemuSyncEvent> quantum;
    do {
        quantum = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!quantum.has_value());
    assert(
        quantum->stopReason ==
        MITTENS_SYNC_STOP_QUANTUM_END);
    assert(quantum->instructionsExecuted == 100);

    assert(bridge.grant(50) == 2);
    std::optional<QemuSyncEvent> guestExit;
    do {
        guestExit = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!guestExit.has_value());
    assert(
        guestExit->stopReason ==
        MITTENS_SYNC_STOP_GUEST_EXIT);
    assert(guestExit->grantEpoch == 2);
    assert(guestExit->instructionsExecuted == 3);

    qemu.join();
    assert(
        bridge.protocolError() ==
        MITTENS_SYNC_BRIDGE_ERROR_NONE);
    assert(
        munmap(address, sizeof(MittensSyncBridge)) == 0);
    bridge.close();
    return 0;
}
