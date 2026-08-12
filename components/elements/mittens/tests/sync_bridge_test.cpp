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
    std::uint64_t vectorExecuted,
    std::uint32_t flags,
    std::uint32_t arrayId,
    std::uint64_t analogSequence,
    std::uint32_t taskId,
    std::uint64_t executionId,
    std::uint32_t receiveDMASource = UINT32_MAX,
    std::uint32_t receiveDMARouteId = UINT32_MAX,
    std::uint32_t receiveDMAWordCount = 0,
    std::uint64_t memoryAddress = 0,
    std::uint32_t memorySize = 0,
    std::uint32_t memoryFlags = MITTENS_SYNC_MEMORY_FLAG_NONE)
{
    assert(
        mittens_sync_load_acquire(&mapping->state) ==
        MITTENS_SYNC_STATE_RUNNING);
    mapping->instructions_executed = executed;
    mapping->vector_instructions_executed = vectorExecuted;
    mapping->stop_reason = reason;
    mapping->event_flags = flags;
    mapping->analog_array_id = arrayId;
    mapping->analog_sequence = analogSequence;
    mapping->task_id = taskId;
    mapping->execution_id = executionId;
    mapping->rx_dma_source = receiveDMASource;
    mapping->rx_dma_route_id = receiveDMARouteId;
    mapping->rx_dma_word_count = receiveDMAWordCount;
    mapping->memory_address = memoryAddress;
    mapping->memory_size = memorySize;
    mapping->memory_flags = memoryFlags;
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
        MITTENS_SYNC_BRIDGE_MAPPING_SIZE,
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
    assert(
        reinterpret_cast<std::uint8_t*>(
            mittens_sync_memory_batch(mapping)) ==
        reinterpret_cast<std::uint8_t*>(mapping) +
            sizeof(MittensSyncBridge));

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
            3,
            MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION,
            2,
            11,
            UINT32_MAX,
            0);

        waitWhile(&mapping->state, MITTENS_SYNC_STATE_EVENT);
        assert(
            mittens_sync_load_acquire(&mapping->state) ==
            MITTENS_SYNC_STATE_RESUME);
        mittens_sync_store_release(
            &mapping->state, MITTENS_SYNC_STATE_RUNNING);
        wake(&mapping->state);

        publish(
            mapping,
            MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT,
            21,
            3,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            0,
            UINT32_MAX,
            0,
            4,
            19,
            300);

        waitWhile(&mapping->state, MITTENS_SYNC_STATE_EVENT);
        assert(
            mittens_sync_load_acquire(&mapping->state) ==
            MITTENS_SYNC_STATE_RESUME);
        mittens_sync_store_release(
            &mapping->state, MITTENS_SYNC_STATE_RUNNING);
        wake(&mapping->state);

        publish(
            mapping,
            MITTENS_SYNC_STOP_MEMORY_ACCESS,
            23,
            3,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            UINT64_C(0x80000420),
            UINT32_MAX,
            UINT64_C(0x80000800),
            UINT32_MAX,
            UINT32_MAX,
            0,
            UINT64_C(0x80001234),
            4,
            MITTENS_SYNC_MEMORY_FLAG_WRITE);

        waitWhile(&mapping->state, MITTENS_SYNC_STATE_EVENT);
        assert(
            mittens_sync_load_acquire(&mapping->state) ==
            MITTENS_SYNC_STATE_RESUME);
        mittens_sync_store_release(
            &mapping->state, MITTENS_SYNC_STATE_RUNNING);
        wake(&mapping->state);

        auto* const batch = mittens_sync_memory_batch(mapping);
        batch[0] = MittensSyncMemoryAccess{
            23, 3, UINT64_C(0x80002000),
            UINT64_C(0x80000100), UINT64_C(0x80000200),
            8, MITTENS_SYNC_MEMORY_FLAG_NONE,
            0, 0, 0, 0};
        batch[1] = MittensSyncMemoryAccess{
            23, 3, UINT64_C(0x80002008),
            UINT64_C(0x80000104), UINT64_C(0x80000200),
            8, MITTENS_SYNC_MEMORY_FLAG_WRITE,
            0, 0, 0, 0};
        publish(
            mapping,
            MITTENS_SYNC_STOP_MEMORY_BATCH,
            23,
            3,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            0,
            UINT32_MAX,
            0,
            UINT32_MAX,
            UINT32_MAX,
            0,
            0,
            2,
            MITTENS_SYNC_MEMORY_FLAG_NONE);

        waitWhile(&mapping->state, MITTENS_SYNC_STATE_EVENT);
        assert(
            mittens_sync_load_acquire(&mapping->state) ==
            MITTENS_SYNC_STATE_RESUME);
        mittens_sync_store_release(
            &mapping->state, MITTENS_SYNC_STATE_RUNNING);
        wake(&mapping->state);

        publish(
            mapping,
            MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE,
            24,
            3,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            1000,
            UINT32_MAX,
            2048,
            UINT32_MAX,
            UINT32_MAX,
            0,
            4096,
            0,
            MITTENS_SYNC_MEMORY_FLAG_NONE);

        waitWhile(&mapping->state, MITTENS_SYNC_STATE_EVENT);
        assert(
            mittens_sync_load_acquire(&mapping->state) ==
            MITTENS_SYNC_STATE_RESUME);
        mittens_sync_store_release(
            &mapping->state, MITTENS_SYNC_STATE_RUNNING);
        wake(&mapping->state);

        publish(
            mapping,
            MITTENS_SYNC_STOP_TASK_START,
            25,
            4,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            0,
            37,
            UINT64_C(0x100000002));

        waitWhile(&mapping->state, MITTENS_SYNC_STATE_EVENT);
        assert(
            mittens_sync_load_acquire(&mapping->state) ==
            MITTENS_SYNC_STATE_RESUME);
        mittens_sync_store_release(
            &mapping->state, MITTENS_SYNC_STATE_RUNNING);
        wake(&mapping->state);

        publish(
            mapping,
            MITTENS_SYNC_STOP_TASK_FINISH,
            40,
            6,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            0,
            37,
            UINT64_C(0x100000002));

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
            12,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            0,
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
            1,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            0,
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
    assert(analog->vectorInstructionsExecuted == 3);
    assert(
        analog->stopReason ==
        MITTENS_SYNC_STOP_ANALOG_SUBMIT);
    assert(
        analog->flags ==
        MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION);
    assert(analog->analogArrayId == 2);
    assert(analog->analogSequence == 11);
    assert(analog->taskId == UINT32_MAX);
    assert(analog->executionId == 0);

    bridge.resume(*analog);

    std::optional<QemuSyncEvent> receiveDMA;
    do {
        receiveDMA = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!receiveDMA.has_value());
    assert(
        receiveDMA->stopReason ==
        MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT);
    assert(receiveDMA->instructionsExecuted == 21);
    assert(receiveDMA->vectorInstructionsExecuted == 3);
    assert(receiveDMA->receiveDMASource == 4);
    assert(receiveDMA->receiveDMARouteId == 19);
    assert(receiveDMA->receiveDMAWordCount == 300);
    bridge.resume(*receiveDMA);

    std::optional<QemuSyncEvent> memory;
    do {
        memory = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!memory.has_value());
    assert(
        memory->stopReason ==
        MITTENS_SYNC_STOP_MEMORY_ACCESS);
    assert(memory->instructionsExecuted == 23);
    assert(memory->memoryAddress == UINT64_C(0x80001234));
    assert(memory->memoryProgramCounter() == UINT64_C(0x80000420));
    assert(memory->memoryReturnAddress() == UINT64_C(0x80000800));
    assert(memory->memorySize == 4);
    assert(
        memory->memoryFlags ==
        MITTENS_SYNC_MEMORY_FLAG_WRITE);
    bridge.resume(*memory);

    std::optional<QemuSyncEvent> memoryBatch;
    do {
        memoryBatch = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!memoryBatch.has_value());
    assert(
        memoryBatch->stopReason ==
        MITTENS_SYNC_STOP_MEMORY_BATCH);
    assert(memoryBatch->memoryBatch.size() == 2);
    assert(
        memoryBatch->memoryBatch[0].address ==
        UINT64_C(0x80002000));
    assert(
        memoryBatch->memoryBatch[0].program_counter ==
        UINT64_C(0x80000100));
    assert(
        memoryBatch->memoryBatch[1].flags ==
        MITTENS_SYNC_MEMORY_FLAG_WRITE);
    bridge.resume(*memoryBatch);

    std::optional<QemuSyncEvent> memoryInitialization;
    do {
        memoryInitialization = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!memoryInitialization.has_value());
    assert(
        memoryInitialization->stopReason ==
        MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE);
    assert(memoryInitialization->instructionsExecuted == 24);
    assert(
        memoryInitialization->memoryInitializationAccesses() ==
        1000);
    assert(
        memoryInitialization->memoryInitializationReadBytes() ==
        4096);
    assert(
        memoryInitialization->memoryInitializationWriteBytes() ==
        2048);
    bridge.resume(*memoryInitialization);

    std::optional<QemuSyncEvent> taskStart;
    do {
        taskStart = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!taskStart.has_value());
    assert(
        taskStart->stopReason ==
        MITTENS_SYNC_STOP_TASK_START);
    assert(taskStart->instructionsExecuted == 25);
    assert(taskStart->vectorInstructionsExecuted == 4);
    assert(taskStart->taskId == 37);
    assert(taskStart->executionId == UINT64_C(0x100000002));
    bridge.resume(*taskStart);

    std::optional<QemuSyncEvent> taskFinish;
    do {
        taskFinish = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!taskFinish.has_value());
    assert(
        taskFinish->stopReason ==
        MITTENS_SYNC_STOP_TASK_FINISH);
    assert(taskFinish->instructionsExecuted == 40);
    assert(taskFinish->vectorInstructionsExecuted == 6);
    assert(taskFinish->taskId == 37);
    assert(taskFinish->executionId == UINT64_C(0x100000002));
    bridge.resume(*taskFinish);

    std::optional<QemuSyncEvent> quantum;
    do {
        quantum = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!quantum.has_value());
    assert(
        quantum->stopReason ==
        MITTENS_SYNC_STOP_QUANTUM_END);
    assert(quantum->instructionsExecuted == 100);
    assert(quantum->vectorInstructionsExecuted == 12);

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
    assert(guestExit->vectorInstructionsExecuted == 1);

    qemu.join();
    assert(
        bridge.protocolError() ==
        MITTENS_SYNC_BRIDGE_ERROR_NONE);
    assert(
        munmap(address, MITTENS_SYNC_BRIDGE_MAPPING_SIZE) == 0);
    bridge.close();
    return 0;
}
