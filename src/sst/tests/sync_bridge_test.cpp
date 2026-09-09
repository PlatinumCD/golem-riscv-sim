#include "../bridge/sharedSyncMemoryBridge.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
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
    std::uint32_t memoryFlags = MITTENS_SYNC_MEMORY_FLAG_NONE,
    std::uint32_t epochId = UINT32_MAX,
    std::uint32_t epochContribution = UINT32_MAX,
    std::uint32_t memoryBatchCount = 0,
    std::uint32_t globalDMABatchCount = 0,
    std::uint32_t analogBatchCount = 0)
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
    mapping->epoch_id = epochId;
    mapping->epoch_contribution = epochContribution;
    mapping->memory_batch_count = memoryBatchCount;
    mapping->global_dma_batch_count = globalDMABatchCount;
    mapping->analog_batch_count = analogBatchCount;
    ++mapping->event_sequence;
    mittens_sync_store_release(
        &mapping->state, MITTENS_SYNC_STATE_EVENT);
    wake(&mapping->state);
}

void publishEpoch(
    MittensSyncBridge* mapping,
    std::uint64_t executed,
    std::uint64_t vectorExecuted,
    std::uint32_t epochId,
    std::uint32_t contribution)
{
    publish(
        mapping,
        MITTENS_SYNC_STOP_EPOCH_BARRIER_ARRIVE,
        executed,
        vectorExecuted,
        MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION,
        UINT32_MAX,
        0,
        UINT32_MAX,
        0,
        UINT32_MAX,
        UINT32_MAX,
        0,
        0,
        0,
        MITTENS_SYNC_MEMORY_FLAG_NONE,
        epochId,
        contribution);
}

void assertGlobalDMARecordEquals(
    const MittensSyncGlobalDMASubmit& actual,
    const MittensSyncGlobalDMASubmit& expected)
{
    assert(
        actual.instructions_executed ==
        expected.instructions_executed);
    assert(
        actual.vector_instructions_executed ==
        expected.vector_instructions_executed);
    assert(
        actual.wait_instructions_executed ==
        expected.wait_instructions_executed);
    assert(
        actual.wait_vector_instructions_executed ==
        expected.wait_vector_instructions_executed);
    assert(actual.global_offset == expected.global_offset);
    assert(actual.scratchpad_offset == expected.scratchpad_offset);
    assert(actual.execution_id == expected.execution_id);
    assert(actual.logical_iteration == expected.logical_iteration);
    assert(actual.token_id == expected.token_id);
    assert(actual.byte_count == expected.byte_count);
    assert(actual.direction == expected.direction);
    assert(actual.request_flags == expected.request_flags);
    assert(
        actual.submit_event_ordinal ==
        expected.submit_event_ordinal);
    assert(actual.wait_event_ordinal == expected.wait_event_ordinal);
}

void testGlobalDMAMacroEvent()
{
    SharedSyncMemoryBridge bridge;
    bridge.create(10);

    void* const address = mmap(
        nullptr,
        MITTENS_SYNC_BRIDGE_MAPPING_SIZE,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        bridge.fileDescriptor(),
        0);
    assert(address != MAP_FAILED);
    auto* const mapping = static_cast<MittensSyncBridge*>(address);

    assert(bridge.grant(100) == 1);
    assert(
        mittens_sync_load_acquire(&mapping->state) ==
        MITTENS_SYNC_STATE_GRANTED);
    mittens_sync_store_release(
        &mapping->state, MITTENS_SYNC_STATE_RUNNING);

    const std::array<MittensSyncGlobalDMASubmit, 2> expected{{
        {
            UINT64_C(10), UINT64_C(1), UINT64_C(30), UINT64_C(3),
            UINT64_C(0x1000), UINT64_C(0x2000),
            UINT64_C(0x100000001), UINT64_C(7),
            UINT32_C(3), UINT32_C(4096), UINT32_C(0), UINT32_C(1),
            UINT32_C(0), UINT32_C(2),
        },
        {
            UINT64_C(20), UINT64_C(2), UINT64_C(63), UINT64_C(6),
            UINT64_C(0x3000), UINT64_C(0x4000),
            UINT64_C(0x100000001), UINT64_C(8),
            UINT32_C(5), UINT32_C(2048), UINT32_C(1), UINT32_C(7),
            UINT32_C(1), UINT32_C(3),
        },
    }};
    MittensSyncGlobalDMASubmit* const records =
        mittens_sync_global_dma_batch(mapping);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        records[index] = expected[index];
    }

    publish(
        mapping,
        MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO,
        expected.back().wait_instructions_executed,
        expected.back().wait_vector_instructions_executed,
        MITTENS_SYNC_EVENT_FLAG_NONE,
        UINT32_MAX,
        0,
        UINT32_MAX,
        0,
        UINT32_MAX,
        UINT32_MAX,
        0,
        0,
        0,
        MITTENS_SYNC_MEMORY_FLAG_NONE,
        UINT32_MAX,
        UINT32_MAX,
        0,
        static_cast<std::uint32_t>(expected.size()));

    const std::optional<QemuSyncEvent> event =
        bridge.waitForEvent(std::chrono::milliseconds::zero());
    assert(event.has_value());
    assert(event->grantEpoch == 1);
    assert(event->eventSequence == 1);
    assert(
        event->instructionsExecuted ==
        expected.back().wait_instructions_executed);
    assert(
        event->vectorInstructionsExecuted ==
        expected.back().wait_vector_instructions_executed);
    assert(
        event->stopReason ==
        MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO);
    assert(event->flags == MITTENS_SYNC_EVENT_FLAG_NONE);
    assert(event->memoryBatch.empty());
    assert(event->analogSubmitBatch.empty());
    assert(event->globalDMASubmitBatch.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        assertGlobalDMARecordEquals(
            event->globalDMASubmitBatch[index], expected[index]);
    }
    assert(
        bridge.protocolError() ==
        MITTENS_SYNC_BRIDGE_ERROR_NONE);

    bridge.resume(*event);
    assert(
        mittens_sync_load_acquire(&mapping->state) ==
        MITTENS_SYNC_STATE_RESUME);
    assert(
        munmap(address, MITTENS_SYNC_BRIDGE_MAPPING_SIZE) == 0);
    bridge.close();
}

void testInvalidGlobalDMAMacroEvent()
{
    SharedSyncMemoryBridge bridge;
    bridge.create(11);

    void* const address = mmap(
        nullptr,
        MITTENS_SYNC_BRIDGE_MAPPING_SIZE,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        bridge.fileDescriptor(),
        0);
    assert(address != MAP_FAILED);
    auto* const mapping = static_cast<MittensSyncBridge*>(address);

    assert(bridge.grant(100) == 1);
    mittens_sync_store_release(
        &mapping->state, MITTENS_SYNC_STATE_RUNNING);
    publish(
        mapping,
        MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO,
        0,
        0,
        MITTENS_SYNC_EVENT_FLAG_NONE,
        UINT32_MAX,
        0,
        UINT32_MAX,
        0);

    bool rejected = false;
    try {
        (void)bridge.waitForEvent(std::chrono::milliseconds::zero());
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
    assert(
        bridge.protocolError() ==
        MITTENS_SYNC_BRIDGE_ERROR_BAD_BUDGET);

    assert(
        munmap(address, MITTENS_SYNC_BRIDGE_MAPPING_SIZE) == 0);
    bridge.close();
}

} // namespace

int main()
{
    testGlobalDMAMacroEvent();
    testInvalidGlobalDMAMacroEvent();

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
    assert(
        reinterpret_cast<std::uint8_t*>(
            mittens_sync_global_dma_batch(mapping)) ==
        reinterpret_cast<std::uint8_t*>(mapping) +
            sizeof(MittensSyncBridge) +
            sizeof(MittensSyncMemoryAccess) *
                MITTENS_SYNC_MEMORY_BATCH_CAPACITY);
    assert(
        reinterpret_cast<std::uint8_t*>(
            mittens_sync_analog_batch(mapping)) ==
        reinterpret_cast<std::uint8_t*>(mapping) +
            sizeof(MittensSyncBridge) +
            sizeof(MittensSyncMemoryAccess) *
                MITTENS_SYNC_MEMORY_BATCH_CAPACITY +
            sizeof(MittensSyncGlobalDMASubmit) *
                MITTENS_SYNC_GLOBAL_DMA_BATCH_CAPACITY);

    std::thread qemu([mapping]() {
        waitWhile(&mapping->state, MITTENS_SYNC_STATE_IDLE);
        assert(
            mittens_sync_load_acquire(&mapping->state) ==
            MITTENS_SYNC_STATE_GRANTED);
        assert(mapping->instruction_budget == 100);
        assert(mapping->epoch_id == UINT32_MAX);
        assert(mapping->epoch_contribution == UINT32_MAX);
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

        auto* const analogBatch = mittens_sync_analog_batch(mapping);
        analogBatch[0] = MittensSyncAnalogSubmit{
            18, 3, UINT64_C(12), 2, 0};
        analogBatch[1] = MittensSyncAnalogSubmit{
            19, 4, UINT64_C(13), 2, 0};
        publish(
            mapping,
            MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH,
            19,
            4,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            0,
            UINT32_MAX,
            0,
            UINT32_MAX,
            UINT32_MAX,
            0,
            0,
            0,
            MITTENS_SYNC_MEMORY_FLAG_NONE,
            UINT32_MAX,
            UINT32_MAX,
            0,
            0,
            2);

        waitWhile(&mapping->state, MITTENS_SYNC_STATE_EVENT);
        assert(
            mittens_sync_load_acquire(&mapping->state) ==
            MITTENS_SYNC_STATE_RESUME);
        mittens_sync_store_release(
            &mapping->state, MITTENS_SYNC_STATE_RUNNING);
        wake(&mapping->state);

        auto* const dmaBatch = mittens_sync_global_dma_batch(mapping);
        dmaBatch[0] = MittensSyncGlobalDMASubmit{
            18, 3, UINT64_MAX, UINT64_MAX,
            UINT64_C(0x1000), UINT64_C(0x2000),
            UINT64_C(41), UINT64_C(7), 5, 4096, 0,
            1, UINT32_MAX, UINT32_MAX};
        dmaBatch[1] = MittensSyncGlobalDMASubmit{
            19, 4, UINT64_MAX, UINT64_MAX,
            UINT64_C(0x3000), UINT64_C(0x4000),
            UINT64_C(41), UINT64_C(8), 6, 4096, 1,
            1, UINT32_MAX, UINT32_MAX};
        publish(
            mapping,
            MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT_BATCH,
            20,
            4,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            0,
            UINT32_MAX,
            0,
            UINT32_MAX,
            UINT32_MAX,
            0,
            0,
            0,
            MITTENS_SYNC_MEMORY_FLAG_NONE,
            UINT32_MAX,
            UINT32_MAX,
            0,
            2);

        waitWhile(&mapping->state, MITTENS_SYNC_STATE_EVENT);
        assert(
            mittens_sync_load_acquire(&mapping->state) ==
            MITTENS_SYNC_STATE_RESUME);
        mittens_sync_store_release(
            &mapping->state, MITTENS_SYNC_STATE_RUNNING);
        wake(&mapping->state);

        publish(
            mapping,
            MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH,
            20,
            4,
            MITTENS_SYNC_EVENT_FLAG_GLOBAL_DMA_SUBMITS,
            UINT32_MAX,
            0,
            5,
            UINT64_C(41),
            UINT32_MAX,
            UINT32_MAX,
            0,
            0,
            0,
            MITTENS_SYNC_MEMORY_FLAG_NONE,
            UINT32_MAX,
            UINT32_MAX,
            0,
            2);

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
            MITTENS_SYNC_STOP_NIC_RX_SOFTWARE_CLAIM,
            22,
            3,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            5,
            UINT32_MAX,
            0,
            4,
            20,
            1022);

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
            0, 0, 0, 1};
        batch[1] = MittensSyncMemoryAccess{
            23, 3, UINT64_C(0x80002008),
            UINT64_C(0x80000104), UINT64_C(0x80000200),
            8, MITTENS_SYNC_MEMORY_FLAG_WRITE,
            0, 0, 0, 1};
        publish(
            mapping,
            MITTENS_SYNC_STOP_MEMORY_FENCE,
            23,
            3,
            MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH,
            UINT32_MAX,
            0,
            UINT32_MAX,
            0,
            UINT32_MAX,
            UINT32_MAX,
            0,
            0,
            0,
            MITTENS_SYNC_MEMORY_FLAG_NONE,
            UINT32_MAX,
            UINT32_MAX,
            2);

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

        publishEpoch(
            mapping,
            41,
            6,
            7,
            MITTENS_SYNC_EPOCH_IDLE);

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

    std::optional<QemuSyncEvent> analogBatch;
    do {
        analogBatch = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!analogBatch.has_value());
    assert(
        analogBatch->stopReason ==
        MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH);
    assert(analogBatch->memoryBatch.empty());
    assert(analogBatch->globalDMASubmitBatch.empty());
    assert(analogBatch->analogSubmitBatch.size() == 2);
    assert(analogBatch->analogSubmitBatch[0].instructions_executed == 18);
    assert(analogBatch->analogSubmitBatch[0].array_id == 2);
    assert(analogBatch->analogSubmitBatch[0].sequence == 12);
    assert(
        analogBatch->analogSubmitBatch[1].vector_instructions_executed == 4);
    assert(analogBatch->analogSubmitBatch[1].array_id == 2);
    assert(analogBatch->analogSubmitBatch[1].sequence == 13);
    bridge.resume(*analogBatch);

    std::optional<QemuSyncEvent> globalDMABatch;
    do {
        globalDMABatch = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!globalDMABatch.has_value());
    assert(
        globalDMABatch->stopReason ==
        MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT_BATCH);
    assert(globalDMABatch->memoryBatch.empty());
    assert(globalDMABatch->globalDMASubmitBatch.size() == 2);
    assert(
        globalDMABatch->globalDMASubmitBatch[0].global_offset ==
        UINT64_C(0x1000));
    assert(
        globalDMABatch->globalDMASubmitBatch[0].byte_count == 4096);
    assert(
        globalDMABatch->globalDMASubmitBatch[1].scratchpad_offset ==
        UINT64_C(0x4000));
    assert(
        globalDMABatch->globalDMASubmitBatch[1].direction == 1);
    bridge.resume(*globalDMABatch);

    std::optional<QemuSyncEvent> globalDMAWaitBatch;
    do {
        globalDMAWaitBatch = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!globalDMAWaitBatch.has_value());
    assert(
        globalDMAWaitBatch->stopReason ==
        MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH);
    assert(
        globalDMAWaitBatch->flags ==
        MITTENS_SYNC_EVENT_FLAG_GLOBAL_DMA_SUBMITS);
    assert(globalDMAWaitBatch->taskId == 5);
    assert(globalDMAWaitBatch->executionId == UINT64_C(41));
    assert(globalDMAWaitBatch->globalDMASubmitBatch.size() == 2);
    assert(globalDMAWaitBatch->globalDMASubmitBatch[0].token_id == 5);
    assert(globalDMAWaitBatch->globalDMASubmitBatch[1].token_id == 6);
    bridge.resume(*globalDMAWaitBatch);

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

    std::optional<QemuSyncEvent> softwareClaim;
    do {
        softwareClaim = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!softwareClaim.has_value());
    assert(
        softwareClaim->stopReason ==
        MITTENS_SYNC_STOP_NIC_RX_SOFTWARE_CLAIM);
    assert(softwareClaim->instructionsExecuted == 22);
    assert(softwareClaim->receiveDMASource == 4);
    assert(softwareClaim->receiveDMARouteId == 20);
    assert(softwareClaim->receiveDMALogicalIteration == 5);
    assert(softwareClaim->receiveDMAWordCount == 1022);
    bridge.resume(*softwareClaim);

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
        MITTENS_SYNC_STOP_MEMORY_FENCE);
    assert(
        memoryBatch->flags ==
        MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH);
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

    std::optional<QemuSyncEvent> epochArrival;
    do {
        epochArrival = bridge.waitForEvent(
            std::chrono::milliseconds(100));
    } while (!epochArrival.has_value());
    assert(
        epochArrival->stopReason ==
        MITTENS_SYNC_STOP_EPOCH_BARRIER_ARRIVE);
    assert(
        epochArrival->flags ==
        MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION);
    assert(epochArrival->instructionsExecuted == 41);
    assert(epochArrival->vectorInstructionsExecuted == 6);
    assert(epochArrival->epochId == 7);
    assert(
        epochArrival->epochContribution ==
        MITTENS_SYNC_EPOCH_IDLE);
    bridge.resume(*epochArrival);

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
