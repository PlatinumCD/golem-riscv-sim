/*
 * Mittens SST/QEMU execution synchronization transport
 *
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/core/cpu.h"
#include "hw/misc/mittens_sync.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/futex.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/cpus.h"

#include "mittens/SyncTileBridge.h"
#include "mittens/MemoryMap.h"

typedef struct MittensScratchpadDMAJob {
    uint64_t execution_id;
    uint32_t token_id;
    uint32_t direction;
    uint32_t request_flags;
    uint64_t global_offset;
    uint64_t scratchpad_offset;
    uint64_t logical_iteration;
    uint32_t byte_count;
    bool write_staged;
} MittensScratchpadDMAJob;

typedef struct MittensSyncDeviceState {
    DeviceState parent_obj;
    int32_t bridge_fd;
    MittensSyncBridge *bridge;
    uint64_t accounted_executed;
    uint64_t vector_instructions_executed;
    uint64_t fetch_vector_baseline;
    uint64_t memory_instruction_program_counter;
    uint32_t memory_instruction_source_register_mask;
    uint32_t memory_instruction_destination_register_mask;
    uint32_t memory_instruction_length;
    int64_t accounted_remaining;
    bool terminal_event_published;
    bool memory_timing;
    bool instruction_fetch_timing;
    bool memory_init_batching;
    bool memory_access_batching;
    bool scratchpad_access_batching;
    bool scratchpad_access_run_compaction;
    bool memory_event_batching;
    bool global_dma_submit_batching;
    bool global_dma_macro_execution;
    bool analog_command_batching;
    uint32_t memory_access_batch_records;
    MittensSyncMemoryAccess *memory_batch;
    uint32_t memory_batch_count;
    uint32_t memory_batch_logical_count;
    MittensSyncGlobalDMASubmit *global_dma_batch;
    uint32_t global_dma_batch_count;
    MittensSyncAnalogSubmit *analog_batch;
    uint32_t analog_batch_count;
    bool memory_init_active;
    uint64_t memory_init_accesses;
    uint64_t memory_init_read_bytes;
    uint64_t memory_init_write_bytes;
    uint64_t last_event_grant_epoch;
    uint64_t last_event_instructions;
    bool scratchpad_enabled;
    uint64_t scratchpad_base;
    uint64_t scratchpad_size;
    uint64_t scratchpad_dma_base;
    MemoryRegion scratchpad;
    MemoryRegion scratchpad_dma_mmio;
    bool scratchpad_mapped;
    bool scratchpad_dma_mapped;
    int32_t global_ram_fd;
    uint64_t global_ram_size;
    uint8_t *global_ram;
    uint64_t dma_source;
    uint64_t dma_destination;
    uint64_t dma_execution_id;
    uint32_t dma_byte_count;
    uint32_t dma_token_id;
    uint32_t dma_direction;
    uint32_t dma_request_flags;
    uint64_t dma_logical_iteration;
    uint32_t dma_status;
    uint32_t dma_error;
    GPtrArray *dma_jobs;
    bool global_dma_macro_active;
    uint64_t global_dma_macro_execution_id;
    uint32_t global_dma_macro_expected_count;
    uint32_t global_dma_macro_wait_count;
    uint32_t global_dma_macro_event_count;
    uint64_t global_dma_macro_begin_instructions;
    uint64_t global_dma_macro_maximum_instruction_span;
} MittensSyncDeviceState;

DECLARE_INSTANCE_CHECKER(
    MittensSyncDeviceState, MITTENS_SYNC, TYPE_MITTENS_SYNC)

static MittensSyncDeviceState *mittens_sync_instance;

static uint64_t mittens_sync_current_executed(void);
static uint64_t mittens_sync_preinstruction_executed(void);
static bool mittens_sync_flush_memory_batch(uint32_t event_flags,
                                            bool wait_for_resume);
static void mittens_sync_set_error(
    enum MittensSyncBridgeError error, const char *message);
static void mittens_sync_publish_event(
    uint32_t reason, uint32_t flags, uint32_t array_id,
    uint64_t analog_sequence, uint32_t task_id, uint64_t execution_id,
    uint32_t rx_dma_source, uint32_t rx_dma_route_id,
    uint32_t rx_dma_word_count, uint64_t memory_address,
    uint32_t memory_size, uint32_t memory_flags,
    uint64_t instructions_executed, bool wait_for_resume);

void helper_mittens_sync_vector_instruction(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    if (s != NULL && s->bridge != NULL) {
        ++s->vector_instructions_executed;
    }
}

void helper_mittens_sync_memory_instruction(
    uint32_t source_register_mask,
    uint32_t destination_register_mask,
    uint32_t instruction_length,
    uint64_t program_counter)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    if (s == NULL || s->bridge == NULL) {
        return;
    }
    s->memory_instruction_source_register_mask = source_register_mask;
    s->memory_instruction_destination_register_mask =
        destination_register_mask;
    s->memory_instruction_length = instruction_length;
    s->memory_instruction_program_counter = program_counter;
}

void helper_mittens_sync_instruction_fetch(
    uint64_t program_counter,
    uint32_t instruction_length)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    if (s == NULL || s->bridge == NULL || !s->instruction_fetch_timing ||
        s->terminal_event_published || instruction_length == 0) {
        return;
    }
    /* The opt-in model is SPM instruction fetch only.  Do not silently run
     * an enabled guest outside the modeled fetch domain. */
    if (!mittens_sync_scratchpad_contains(program_counter, instruction_length)) {
        mittens_sync_set_error(
            MITTENS_SYNC_BRIDGE_ERROR_BAD_STATE,
            "instruction fetch fell outside the configured scratchpad");
        if (current_cpu != NULL) {
            cpu_loop_exit(current_cpu);
        }
        return;
    }
    /* This helper is emitted at the start of a one-instruction TB.  The
     * boundary therefore reports the count before the instruction, while
     * QEMU's normal icount/vector ledger remains unchanged. */
    mittens_sync_publish_event(
        MITTENS_SYNC_STOP_INSTRUCTION_FETCH,
        MITTENS_SYNC_EVENT_FLAG_NONE,
        UINT32_MAX, 0, UINT32_MAX, 0, UINT32_MAX, UINT32_MAX, 0,
        program_counter, instruction_length,
        MITTENS_SYNC_MEMORY_FLAG_NONE,
        mittens_sync_preinstruction_executed(), true);
    s->fetch_vector_baseline = s->vector_instructions_executed;
    /* Dependency metadata belongs to this instruction only. In particular,
     * code rewritten at the same PC must not inherit a prior scalar access. */
    s->memory_instruction_program_counter = UINT64_MAX;
    s->memory_instruction_length = 0;
    s->memory_instruction_source_register_mask = 0;
    s->memory_instruction_destination_register_mask = 0;
}

/* End the current vector memory instruction before another instruction can
 * execute or fetch. This is transaction assembly, not instruction batching. */
void helper_mittens_sync_vector_memory_end(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;
    if (s != NULL && s->instruction_fetch_timing) {
        (void)mittens_sync_flush_memory_batch(
            MITTENS_SYNC_EVENT_FLAG_NONE, true);
    }
}

void helper_mittens_sync_instruction_fence(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    if (s == NULL || s->bridge == NULL || !s->instruction_fetch_timing ||
        s->terminal_event_published) {
        return;
    }
    /* QEMU invalidates translated blocks for FENCE.I; this event invalidates
     * the modeled instruction cache at the same architectural boundary. */
    mittens_sync_publish_event(
        MITTENS_SYNC_STOP_INSTRUCTION_FENCE,
        MITTENS_SYNC_EVENT_FLAG_NONE,
        UINT32_MAX, 0, UINT32_MAX, 0, UINT32_MAX, UINT32_MAX, 0,
        0, 0, MITTENS_SYNC_MEMORY_FLAG_NONE,
        mittens_sync_preinstruction_executed(), true);
}

static void mittens_sync_wake(uint32_t *state)
{
    qemu_futex_wake(state, INT_MAX);
}

static void mittens_sync_wait_while(
    uint32_t *state,
    uint32_t expected)
{
    while (mittens_sync_load_acquire(state) == expected) {
        qemu_futex_wait(state, expected);
    }
}

static uint64_t mittens_sync_current_executed(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;
    CPUState *cpu = current_cpu;
    int64_t remaining;
    uint64_t unaccounted;

    if (s == NULL || s->bridge == NULL ||
        cpu == NULL || !icount_enabled()) {
        return 0;
    }

    remaining =
        cpu->neg.icount_decr.u16.low + cpu->icount_extra;
    if (remaining > s->accounted_remaining) {
        return s->accounted_executed;
    }
    unaccounted =
        (uint64_t)(s->accounted_remaining - remaining);
    return s->accounted_executed + unaccounted;
}

static uint64_t mittens_sync_preinstruction_executed(void)
{
    uint64_t executed = mittens_sync_current_executed();

    /* icount charges the current one-instruction TB before entering its
     * helper.  Fetch and FENCE.I events are architectural pre-execution
     * boundaries, so remove exactly that current instruction charge. */
    return executed == 0 ? 0 : executed - 1;
}

static void mittens_sync_set_error(
    enum MittensSyncBridgeError error,
    const char *message)
{
    MittensSyncDeviceState *s = mittens_sync_instance;
    uint32_t expected = MITTENS_SYNC_BRIDGE_ERROR_NONE;
    uint32_t state = UINT32_MAX;
    uint64_t grant_epoch = 0;
    uint64_t event_sequence = 0;
    uint32_t stop_reason = MITTENS_SYNC_STOP_NONE;

    if (s != NULL && s->bridge != NULL) {
        state = mittens_sync_load_acquire(&s->bridge->state);
        grant_epoch = s->bridge->grant_epoch;
        event_sequence = s->bridge->event_sequence;
        stop_reason = s->bridge->stop_reason;
        (void)__atomic_compare_exchange_n(
            &s->bridge->protocol_error,
            &expected,
            (uint32_t)error,
            false,
            __ATOMIC_RELEASE,
            __ATOMIC_RELAXED);
    }
    /*
     * Protocol errors are fatal to the SST/QEMU contract.  Emit their state
     * unconditionally: qemu_log_mask is silent unless a -d mask was supplied,
     * which previously reduced large-model terminal races to an opaque error
     * number after the responsible QEMU had already exited.
     */
    error_report(
        "mittens-sync: %s (error=%u state=%u grant=%" PRIu64
        " event=%" PRIu64 " stop=%u terminal=%u memory_batch=%u"
        " global_dma_batch=%u analog_batch=%u)",
        message, (unsigned)error, (unsigned)state, grant_epoch,
        event_sequence, (unsigned)stop_reason,
        s != NULL && s->terminal_event_published ? 1U : 0U,
        s != NULL ? s->memory_batch_count : 0U,
        s != NULL ? s->global_dma_batch_count : 0U,
        s != NULL ? s->analog_batch_count : 0U);
}

static void mittens_sync_publish_event_raw(
    uint32_t reason,
    uint32_t flags,
    uint32_t array_id,
    uint64_t analog_sequence,
    uint32_t task_id,
    uint64_t execution_id,
    uint32_t rx_dma_source,
    uint32_t rx_dma_route_id,
    uint32_t rx_dma_word_count,
    uint64_t memory_address,
    uint32_t memory_size,
    uint32_t memory_flags,
    uint32_t epoch_id,
    uint32_t epoch_contribution,
    uint32_t memory_batch_count,
    uint32_t global_dma_batch_count,
    uint32_t analog_batch_count,
    uint64_t instructions_executed,
    bool wait_for_resume)
{
    MittensSyncDeviceState *s = mittens_sync_instance;
    MittensSyncBridge *bridge;
    uint32_t state;

    if (s == NULL || s->bridge == NULL) {
        return;
    }
    /*
     * Guest exit is an irrevocable fd-41 boundary.  QEMU can still execute
     * helpers already translated after the SiFive finisher MMIO write (the
     * platform's mandatory I/O fence is one example), but none may publish a
     * second synchronization event after the terminal event.
     */
    if (s->terminal_event_published) {
        return;
    }
    bridge = s->bridge;
    state = mittens_sync_load_acquire(&bridge->state);
    if (state != MITTENS_SYNC_STATE_RUNNING) {
        mittens_sync_set_error(
            MITTENS_SYNC_BRIDGE_ERROR_BAD_STATE,
            "QEMU attempted to yield while it was not running");
        return;
    }

    if (s->last_event_grant_epoch == bridge->grant_epoch &&
        instructions_executed < s->last_event_instructions) {
        instructions_executed = s->last_event_instructions;
    }
    s->last_event_grant_epoch = bridge->grant_epoch;
    s->last_event_instructions = instructions_executed;

    bridge->instructions_executed = instructions_executed;
    bridge->vector_instructions_executed =
        s->vector_instructions_executed;
    bridge->stop_reason = reason;
    bridge->event_flags = flags;
    bridge->analog_array_id = array_id;
    bridge->analog_sequence = analog_sequence;
    bridge->task_id = task_id;
    bridge->execution_id = execution_id;
    bridge->rx_dma_source = rx_dma_source;
    bridge->rx_dma_route_id = rx_dma_route_id;
    bridge->rx_dma_word_count = rx_dma_word_count;
    bridge->memory_address = memory_address;
    bridge->memory_size = memory_size;
    bridge->memory_flags = memory_flags;
    bridge->epoch_id = epoch_id;
    bridge->epoch_contribution = epoch_contribution;
    bridge->memory_batch_count = memory_batch_count;
    bridge->global_dma_batch_count = global_dma_batch_count;
    bridge->analog_batch_count = analog_batch_count;
    bridge->event_sequence += UINT64_C(1);
    mittens_sync_store_release(
        &bridge->state, MITTENS_SYNC_STATE_EVENT);
    mittens_sync_wake(&bridge->state);

    if (!wait_for_resume) {
        return;
    }

    for (;;) {
        state = mittens_sync_load_acquire(&bridge->state);
        if (state == MITTENS_SYNC_STATE_RESUME) {
            mittens_sync_store_release(
                &bridge->state, MITTENS_SYNC_STATE_RUNNING);
            mittens_sync_wake(&bridge->state);
            return;
        }
        if (state != MITTENS_SYNC_STATE_EVENT) {
            mittens_sync_set_error(
                MITTENS_SYNC_BRIDGE_ERROR_BAD_STATE,
                "QEMU observed an invalid state while waiting for resume");
            return;
        }
        mittens_sync_wait_while(
            &bridge->state, MITTENS_SYNC_STATE_EVENT);
    }
}

static bool mittens_sync_flush_analog_batch(
    uint32_t event_flags,
    bool wait_for_resume)
{
    MittensSyncDeviceState *s = mittens_sync_instance;
    uint32_t count;

    if (s == NULL || s->bridge == NULL || s->analog_batch_count == 0) {
        return false;
    }
    count = s->analog_batch_count;
    mittens_sync_publish_event_raw(
        MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH,
        event_flags,
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
        count,
        mittens_sync_current_executed(),
        wait_for_resume);
    s->analog_batch_count = 0;
    return true;
}

static bool mittens_sync_flush_memory_batch(
    uint32_t event_flags,
    bool wait_for_resume)
{
    MittensSyncDeviceState *s = mittens_sync_instance;
    uint32_t count;

    if (s == NULL || s->bridge == NULL ||
        s->memory_batch_count == 0) {
        return false;
    }
    count = s->memory_batch_count;
    if (s->instruction_fetch_timing) {
        event_flags |= MITTENS_SYNC_EVENT_FLAG_VECTOR_MEMORY;
    }
    mittens_sync_publish_event_raw(
        MITTENS_SYNC_STOP_MEMORY_BATCH,
        event_flags,
        UINT32_MAX,
        0,
        UINT32_MAX,
        0,
        UINT32_MAX,
        UINT32_MAX,
        0,
        0,
        count,
        MITTENS_SYNC_MEMORY_FLAG_NONE,
        UINT32_MAX,
        UINT32_MAX,
        count,
        0,
        0,
        mittens_sync_current_executed(),
        wait_for_resume);
    s->memory_batch_count = 0;
    s->memory_batch_logical_count = 0;
    return true;
}

static bool mittens_sync_flush_global_dma_batch(
    uint32_t event_flags,
    bool wait_for_resume)
{
    MittensSyncDeviceState *s = mittens_sync_instance;
    uint32_t count;

    if (s != NULL && s->global_dma_macro_active) {
        mittens_sync_set_error(
            MITTENS_SYNC_BRIDGE_ERROR_BAD_STATE,
            "an event attempted to cross an active global DMA macro");
        return false;
    }
    if (s == NULL || s->bridge == NULL ||
        s->global_dma_batch_count == 0) {
        return false;
    }
    count = s->global_dma_batch_count;
    mittens_sync_publish_event_raw(
        MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT_BATCH,
        event_flags,
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
        count,
        0,
        mittens_sync_current_executed(),
        wait_for_resume);
    s->global_dma_batch_count = 0;
    return true;
}

static void mittens_sync_publish_event(
    uint32_t reason,
    uint32_t flags,
    uint32_t array_id,
    uint64_t analog_sequence,
    uint32_t task_id,
    uint64_t execution_id,
    uint32_t rx_dma_source,
    uint32_t rx_dma_route_id,
    uint32_t rx_dma_word_count,
    uint64_t memory_address,
    uint32_t memory_size,
    uint32_t memory_flags,
    uint64_t instructions_executed,
    bool wait_for_resume)
{
    MittensSyncDeviceState *s = mittens_sync_instance;
    uint32_t memory_batch_count = 0;
    uint32_t analog_batch_count = 0;

    if (s != NULL && s->global_dma_macro_active) {
        mittens_sync_set_error(
            MITTENS_SYNC_BRIDGE_ERROR_BAD_STATE,
            "a semantic event crossed an active global DMA macro");
        return;
    }

    (void)mittens_sync_flush_global_dma_batch(
        MITTENS_SYNC_EVENT_FLAG_NONE, true);
    if (s != NULL && s->bridge != NULL &&
        s->analog_batch_count != 0) {
        if (s->memory_batch_count != 0) {
            mittens_sync_set_error(
                MITTENS_SYNC_BRIDGE_ERROR_BAD_STATE,
                "analog and memory batches overlapped");
            return;
        }
        analog_batch_count = s->analog_batch_count;
        flags |= MITTENS_SYNC_EVENT_FLAG_ANALOG_BATCH;
    }
    if (s != NULL && s->bridge != NULL &&
        s->memory_event_batching && s->memory_batch_count != 0) {
        memory_batch_count = s->memory_batch_count;
        flags |= MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH;
    } else {
        (void)mittens_sync_flush_memory_batch(
            MITTENS_SYNC_EVENT_FLAG_NONE, true);
    }
    mittens_sync_publish_event_raw(
        reason, flags, array_id, analog_sequence, task_id,
        execution_id, rx_dma_source, rx_dma_route_id,
        rx_dma_word_count, memory_address, memory_size,
        memory_flags, UINT32_MAX, UINT32_MAX,
        memory_batch_count,
        0,
        analog_batch_count,
        instructions_executed, wait_for_resume);
    if (memory_batch_count != 0) {
        s->memory_batch_count = 0;
        s->memory_batch_logical_count = 0;
    }
    if (analog_batch_count != 0) {
        s->analog_batch_count = 0;
    }
}

bool mittens_sync_available(void)
{
    return mittens_sync_instance != NULL &&
           mittens_sync_instance->bridge != NULL &&
           !mittens_sync_instance->terminal_event_published;
}

bool mittens_sync_memory_timing_enabled(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    return s != NULL && s->bridge != NULL &&
           !s->terminal_event_published &&
           (s->memory_timing || s->scratchpad_enabled);
}

bool mittens_sync_instruction_fetch_timing_enabled(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    return s != NULL && s->bridge != NULL &&
           !s->terminal_event_published && s->instruction_fetch_timing &&
           s->scratchpad_enabled;
}

bool mittens_sync_memory_initialization_active(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    return s != NULL && s->bridge != NULL &&
           !s->terminal_event_published && s->memory_init_active;
}

bool mittens_sync_scratchpad_contains(uint64_t address, uint64_t byte_count)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    return s != NULL && s->scratchpad_enabled && byte_count != 0 &&
           mittens_memory_contains_range(s->scratchpad_base, s->scratchpad_size,
                                        address, byte_count);
}

void mittens_sync_memory_init_complete(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    if (s == NULL || s->bridge == NULL ||
        !s->memory_init_batching || !s->memory_init_active) {
        return;
    }
    s->memory_init_active = false;
    mittens_sync_publish_event(
        MITTENS_SYNC_STOP_MEMORY_INIT_COMPLETE,
        MITTENS_SYNC_EVENT_FLAG_NONE,
        UINT32_MAX,
        s->memory_init_accesses,
        UINT32_MAX,
        s->memory_init_write_bytes,
        UINT32_MAX,
        UINT32_MAX,
        0,
        s->memory_init_read_bytes,
        0,
        MITTENS_SYNC_MEMORY_FLAG_NONE,
        mittens_sync_current_executed(),
        true);
}

int64_t mittens_sync_wait_for_grant(int64_t qemu_budget)
{
    MittensSyncDeviceState *s = mittens_sync_instance;
    MittensSyncBridge *bridge;
    uint32_t state;
    uint64_t granted;

    if (s == NULL || s->bridge == NULL) {
        return qemu_budget;
    }
    if (s->terminal_event_published) {
        return 1;
    }
    (void)qemu_budget;
    bridge = s->bridge;

    for (;;) {
        state = mittens_sync_load_acquire(&bridge->state);
        if (state == MITTENS_SYNC_STATE_GRANTED) {
            break;
        }
        mittens_sync_wait_while(&bridge->state, state);
    }

    granted = mittens_sync_load_u64_acquire(
        &bridge->instruction_budget);
    if (granted == 0 || granted > INT64_MAX) {
        mittens_sync_set_error(
            MITTENS_SYNC_BRIDGE_ERROR_BAD_BUDGET,
            "SST supplied an invalid instruction budget");
        return 1;
    }

    mittens_sync_store_release(
        &bridge->state, MITTENS_SYNC_STATE_RUNNING);
    mittens_sync_wake(&bridge->state);

    return (int64_t)granted;
}

void mittens_sync_begin_quantum(int64_t instruction_budget)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    if (!mittens_sync_available()) {
        return;
    }
    s->accounted_executed = 0;
    s->vector_instructions_executed = 0;
    s->accounted_remaining = instruction_budget;
}

void mittens_sync_account_icount(
    int64_t executed,
    int64_t remaining)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    if (!mittens_sync_available() || executed < 0 ||
        remaining < 0) {
        return;
    }
    s->accounted_executed += (uint64_t)executed;
    s->accounted_remaining = remaining;
}

void mittens_sync_quantum_end(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    if (!mittens_sync_available()) {
        return;
    }
    if (s->global_dma_macro_active) {
        mittens_sync_set_error(
            MITTENS_SYNC_BRIDGE_ERROR_BAD_BUDGET,
            "global DMA macro crossed its certified instruction bound");
        mittens_sync_publish_event_raw(
            MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            0,
            UINT32_MAX,
            s->global_dma_macro_execution_id,
            UINT32_MAX,
            UINT32_MAX,
            0,
            0,
            0,
            MITTENS_SYNC_MEMORY_FLAG_NONE,
            UINT32_MAX,
            UINT32_MAX,
            0,
            s->global_dma_batch_count,
            0,
            mittens_sync_current_executed(),
            false);
        return;
    }
    if (mittens_sync_flush_analog_batch(
            MITTENS_SYNC_EVENT_FLAG_QUANTUM_END, false)) {
        return;
    }
    if (mittens_sync_flush_global_dma_batch(
            MITTENS_SYNC_EVENT_FLAG_QUANTUM_END, false)) {
        return;
    }
    if (mittens_sync_flush_memory_batch(
            MITTENS_SYNC_EVENT_FLAG_QUANTUM_END, false)) {
        return;
    }
    mittens_sync_publish_event_raw(
        MITTENS_SYNC_STOP_QUANTUM_END,
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
        0,
        mittens_sync_current_executed(),
        false);
}

void mittens_sync_guest_exit(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    if (!mittens_sync_available()) {
        return;
    }
    mittens_sync_memory_init_complete();
    /*
     * Keep the bridge nonterminal while publish_event drains or fuses every
     * earlier memory/DMA/analog batch.  Only the completed guest-exit envelope
     * closes the producer side of the state machine.
     */
    mittens_sync_publish_event(
        MITTENS_SYNC_STOP_GUEST_EXIT,
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
        mittens_sync_current_executed(),
        false);
    s->terminal_event_published = true;
    if (current_cpu != NULL) {
        cpu_exit(current_cpu);
    }
}

void mittens_sync_yield_nic(uint32_t reason)
{
    if (!mittens_sync_available()) {
        return;
    }
    mittens_sync_publish_event(
        reason,
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
        mittens_sync_current_executed(),
        true);
}

void mittens_sync_yield_nic_transmit(
    uint32_t reason,
    bool burst)
{
    if (!mittens_sync_available()) {
        return;
    }
    mittens_sync_publish_event(
        reason,
        burst
            ? MITTENS_SYNC_EVENT_FLAG_NIC_BURST
            : MITTENS_SYNC_EVENT_FLAG_NONE,
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
        mittens_sync_current_executed(),
        true);
}

void mittens_sync_yield_receive_dma(
    uint32_t source,
    uint32_t route_id,
    uint64_t logical_iteration,
    uint64_t destination,
    uint32_t word_count)
{
    if (!mittens_sync_available()) {
        return;
    }
    mittens_sync_publish_event(
        MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT,
        MITTENS_SYNC_EVENT_FLAG_NONE,
        UINT32_MAX,
        logical_iteration,
        UINT32_MAX,
        0,
        source,
        route_id,
        word_count,
        destination,
        0,
        MITTENS_SYNC_MEMORY_FLAG_NONE,
        mittens_sync_current_executed(),
        true);
}

void mittens_sync_yield_receive_software_claim(
    uint32_t source,
    uint32_t route_id,
    uint64_t logical_iteration,
    uint32_t word_count)
{
    if (!mittens_sync_available()) {
        return;
    }
    mittens_sync_publish_event(
        MITTENS_SYNC_STOP_NIC_RX_SOFTWARE_CLAIM,
        MITTENS_SYNC_EVENT_FLAG_NONE,
        UINT32_MAX,
        logical_iteration,
        UINT32_MAX,
        0,
        source,
        route_id,
        word_count,
        0,
        0,
        MITTENS_SYNC_MEMORY_FLAG_NONE,
        mittens_sync_current_executed(),
        true);
}

void mittens_sync_yield_task(
    uint32_t reason,
    uint32_t task_id,
    uint64_t execution_id)
{
    if (!mittens_sync_available()) {
        return;
    }
    mittens_sync_publish_event(
        reason,
        MITTENS_SYNC_EVENT_FLAG_NONE,
        UINT32_MAX,
        0,
        task_id,
        execution_id,
        UINT32_MAX,
        UINT32_MAX,
        0,
        0,
        0,
        MITTENS_SYNC_MEMORY_FLAG_NONE,
        mittens_sync_current_executed(),
        true);
}

void mittens_sync_yield_epoch(
    uint32_t epoch_id,
    uint32_t contribution)
{
    MittensSyncDeviceState *s = mittens_sync_instance;
    uint32_t flags = MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION;
    uint32_t memory_batch_count = 0;

    if (!mittens_sync_available()) {
        return;
    }
    if (contribution != MITTENS_SYNC_EPOCH_WORK_COMPLETE &&
        contribution != MITTENS_SYNC_EPOCH_IDLE) {
        mittens_sync_set_error(
            MITTENS_SYNC_BRIDGE_ERROR_BAD_BUDGET,
            "guest supplied an invalid epoch contribution");
        return;
    }

    /*
     * Make all earlier timed guest writes visible to SST before publishing
     * the epoch arrival.  The following event blocks this QEMU process until
     * the modeled controller releases the exact completed epoch.
     */
    (void)mittens_sync_flush_global_dma_batch(
        MITTENS_SYNC_EVENT_FLAG_NONE, true);
    (void)mittens_sync_flush_analog_batch(
        MITTENS_SYNC_EVENT_FLAG_NONE, true);
    if (s->memory_event_batching && s->memory_batch_count != 0) {
        memory_batch_count = s->memory_batch_count;
        flags |= MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH;
    } else {
        (void)mittens_sync_flush_memory_batch(
            MITTENS_SYNC_EVENT_FLAG_NONE, true);
    }
    mittens_sync_publish_event_raw(
        MITTENS_SYNC_STOP_EPOCH_BARRIER_ARRIVE,
        flags,
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
        epoch_id,
        contribution,
        memory_batch_count,
        0,
        0,
        mittens_sync_current_executed(),
        true);
    if (memory_batch_count != 0) {
        s->memory_batch_count = 0;
        s->memory_batch_logical_count = 0;
    }
}

void mittens_sync_yield_analog(
    uint32_t reason,
    uint32_t array_id,
    uint64_t analog_sequence,
    bool wait_for_completion)
{
    MittensSyncDeviceState *s = mittens_sync_instance;
    uint32_t flags = wait_for_completion ?
        MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION :
        MITTENS_SYNC_EVENT_FLAG_NONE;

    if (!mittens_sync_available()) {
        return;
    }
    if (s->analog_command_batching &&
        reason == MITTENS_SYNC_STOP_ANALOG_SUBMIT &&
        !wait_for_completion && s->memory_batch_count == 0 &&
        s->global_dma_batch_count == 0) {
        MittensSyncAnalogSubmit *record;
        if (s->analog_batch_count ==
            MITTENS_SYNC_ANALOG_BATCH_CAPACITY) {
            (void)mittens_sync_flush_analog_batch(
                MITTENS_SYNC_EVENT_FLAG_NONE, true);
        }
        if (s->analog_batch == NULL ||
            s->analog_batch_count >=
                MITTENS_SYNC_ANALOG_BATCH_CAPACITY) {
            mittens_sync_set_error(
                MITTENS_SYNC_BRIDGE_ERROR_BAD_BUDGET,
                "analog submit batch exceeded bridge capacity");
            return;
        }
        record = &s->analog_batch[s->analog_batch_count++];
        record->instructions_executed =
            mittens_sync_current_executed();
        record->vector_instructions_executed =
            s->vector_instructions_executed;
        record->sequence = analog_sequence;
        record->array_id = array_id;
        record->flags = MITTENS_SYNC_EVENT_FLAG_NONE;
        return;
    }
    mittens_sync_publish_event(
        reason,
        flags,
        array_id,
        analog_sequence,
        UINT32_MAX,
        0,
        UINT32_MAX,
        UINT32_MAX,
        0,
        0,
        0,
        MITTENS_SYNC_MEMORY_FLAG_NONE,
        mittens_sync_current_executed(),
        true);
}

void mittens_sync_yield_memory(
    uint64_t physical_address,
    uint32_t size,
    bool write,
    uint64_t program_counter,
    uint64_t return_address)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    const bool scratchpad =
        s != NULL && s->scratchpad_enabled &&
        mittens_memory_contains(s->scratchpad_base, s->scratchpad_size, physical_address);
    const bool vector_transaction = scratchpad && s->instruction_fetch_timing &&
        s->vector_instructions_executed != s->fetch_vector_baseline;

    if (!mittens_sync_memory_timing_enabled() ||
        (!scratchpad && !s->memory_timing)) {
        return;
    }
    /* A later memory access may observe an analog result or alias its input. */
    (void)mittens_sync_flush_analog_batch(
        MITTENS_SYNC_EVENT_FLAG_NONE, true);
    /* Preserve DMA-submit-before-memory ordering across the fd-41 batch. */
    (void)mittens_sync_flush_global_dma_batch(
        MITTENS_SYNC_EVENT_FLAG_NONE, true);
    if (s->memory_init_active) {
        if (s->memory_init_accesses == UINT64_MAX ||
            (write &&
             s->memory_init_write_bytes > UINT64_MAX - size) ||
            (!write &&
             s->memory_init_read_bytes > UINT64_MAX - size)) {
            mittens_sync_set_error(
                MITTENS_SYNC_BRIDGE_ERROR_BAD_BUDGET,
                "initialization memory counters overflowed");
            return;
        }
        ++s->memory_init_accesses;
        if (write) {
            s->memory_init_write_bytes += size;
        } else {
            s->memory_init_read_bytes += size;
        }
        return;
    }
    if ((!scratchpad && s->memory_access_batching) ||
        (scratchpad && s->scratchpad_access_batching) || vector_transaction) {
        MittensSyncMemoryAccess *access;
        MittensSyncMemoryAccess *previous;
        const uint64_t instructions_executed =
            mittens_sync_current_executed();
        const uint64_t vector_instructions_executed =
            s->vector_instructions_executed;
        uint32_t access_flags =
            (write ? MITTENS_SYNC_MEMORY_FLAG_WRITE : 0) |
            (scratchpad ? MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD : 0);
        uint32_t source_register_mask = 0;
        uint32_t destination_register_mask = 0;
        uint32_t instruction_length = 0;

        if (s->memory_instruction_program_counter == program_counter) {
            access_flags |= MITTENS_SYNC_MEMORY_FLAG_REGISTER_DEPS;
            source_register_mask =
                s->memory_instruction_source_register_mask;
            destination_register_mask =
                s->memory_instruction_destination_register_mask;
            instruction_length = s->memory_instruction_length;
        }

        /*
         * Keep each batch in one architectural address domain.  The SST
         * consumer coalesces ordinary memory for MemHierarchy, while it must
         * replay every scratchpad access through ScratchpadTimingModel.
         */
        if (s->memory_batch_count != 0 &&
            ((s->memory_batch[0].flags ^ access_flags) &
             MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD) != 0) {
            (void)mittens_sync_flush_memory_batch(
                MITTENS_SYNC_EVENT_FLAG_NONE, true);
        }

        /*
         * Preserve the exact baseline synchronization boundaries.  The
         * configured limit counts logical accesses, not compact records, so
         * compaction cannot change simulated request arbitration.
         */
        if (!vector_transaction && s->memory_batch_logical_count ==
            s->memory_access_batch_records) {
            (void)mittens_sync_flush_memory_batch(
                MITTENS_SYNC_EVENT_FLAG_NONE, true);
        }

        /*
         * QEMU reports one callback per RVV element.  Adjacent elements from
         * the same dynamic scratchpad instruction have identical retirement
         * metadata, so encode them as one bounded run.  An aligned eight-word
         * e32 run is additionally tagged as one 32-byte CPU transaction;
         * SST preserves the eight logical elements for accounting while
         * scheduling one 256-bit scratchpad beat.
         */
        previous = s->memory_batch_count == 0 ? NULL :
            &s->memory_batch[s->memory_batch_count - 1];
        if (scratchpad && (s->scratchpad_access_run_compaction || vector_transaction) &&
            previous != NULL && size != 0 &&
            previous->size == size &&
            previous->repeat_count != 0 &&
            previous->repeat_count != UINT32_MAX &&
            (previous->flags &
             ~MITTENS_SYNC_MEMORY_FLAG_CONTIGUOUS_RUN) == access_flags &&
            previous->instructions_executed == instructions_executed &&
            previous->vector_instructions_executed ==
                vector_instructions_executed &&
            previous->program_counter == program_counter &&
            previous->return_address == return_address &&
            previous->source_register_mask == source_register_mask &&
            previous->destination_register_mask ==
                destination_register_mask &&
            previous->instruction_length == instruction_length &&
            previous->repeat_count <= UINT64_MAX / previous->size) {
            const uint64_t run_bytes =
                (uint64_t)previous->size * previous->repeat_count;
            if (previous->address <= UINT64_MAX - run_bytes &&
                previous->address + run_bytes == physical_address) {
                previous->flags |=
                    MITTENS_SYNC_MEMORY_FLAG_CONTIGUOUS_RUN;
                ++previous->repeat_count;
                if (previous->size == sizeof(uint32_t) &&
                    previous->repeat_count == 8 &&
                    previous->address % 32 == 0 &&
                    previous->instruction_length == 0 &&
                    previous->vector_instructions_executed != 0) {
                    previous->flags |=
                        MITTENS_SYNC_MEMORY_FLAG_VECTOR_TRANSACTION;
                }
                ++s->memory_batch_logical_count;
                return;
            }
        }

        /*
         * The configured bound is a logical-access batching horizon, while
         * the bridge has an independent fixed transport-record capacity.
         * Compacted vector runs commonly encode thousands of logical
         * accesses in far fewer records. Flush only when a non-compacting
         * access would consume record 1025; this permits a larger logical
         * horizon without ever writing beyond the bridge ABI.
         */
        if (s->memory_batch_count ==
            MITTENS_SYNC_MEMORY_BATCH_CAPACITY) {
            (void)mittens_sync_flush_memory_batch(
                MITTENS_SYNC_EVENT_FLAG_NONE, true);
        }

        access = &s->memory_batch[s->memory_batch_count++];
        access->instructions_executed = instructions_executed;
        access->vector_instructions_executed =
            vector_instructions_executed;
        access->address = physical_address;
        access->program_counter = program_counter;
        access->return_address = return_address;
        access->size = size;
        access->flags = access_flags;
        access->source_register_mask = source_register_mask;
        access->destination_register_mask = destination_register_mask;
        access->instruction_length = instruction_length;
        access->repeat_count = 1;
        ++s->memory_batch_logical_count;
        return;
    }
    mittens_sync_publish_event(
        MITTENS_SYNC_STOP_MEMORY_ACCESS,
        MITTENS_SYNC_EVENT_FLAG_NONE,
        UINT32_MAX,
        program_counter,
        UINT32_MAX,
        return_address,
        UINT32_MAX,
        UINT32_MAX,
        0,
        physical_address,
        size,
        (write ? MITTENS_SYNC_MEMORY_FLAG_WRITE : 0) |
        (scratchpad ? MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD : 0),
        mittens_sync_current_executed(),
        true);
}

void mittens_sync_yield_memory_atomic(
    uint64_t physical_address,
    uint32_t size,
    uint64_t program_counter,
    uint64_t return_address)
{
    MittensSyncDeviceState *s = mittens_sync_instance;
    bool memory_batching;
    bool scratchpad_batching;

    if (s == NULL) {
        return;
    }
    /* Atomic accesses are ordering boundaries and remain synchronous. */
    memory_batching = s->memory_access_batching;
    scratchpad_batching = s->scratchpad_access_batching;
    s->memory_access_batching = false;
    s->scratchpad_access_batching = false;
    mittens_sync_yield_memory(
        physical_address, size, true,
        program_counter, return_address);
    s->memory_access_batching = memory_batching;
    s->scratchpad_access_batching = scratchpad_batching;
}

void helper_mittens_sync_memory_fence(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    if (s == NULL || s->bridge == NULL || s->terminal_event_published ||
        (!s->memory_timing && !s->scratchpad_access_batching)) {
        return;
    }
    mittens_sync_publish_event(
        MITTENS_SYNC_STOP_MEMORY_FENCE,
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
        mittens_sync_current_executed(),
        true);
}

enum {
    MITTENS_DMA_MAX_JOBS = 8,
    MITTENS_DMA_COMMAND_SUBMIT = 1,
    MITTENS_DMA_COMMAND_WAIT = 2,
    MITTENS_DMA_COMMAND_INITIALIZE_GLOBAL_RAM = 3,
    MITTENS_DMA_COMMAND_WAIT_BATCH = 4,
    MITTENS_DMA_COMMAND_MACRO_BEGIN = 5,
    MITTENS_DMA_COMMAND_MACRO_END = 6,
    MITTENS_DMA_STATUS_READY = 1U << 0,
    MITTENS_DMA_STATUS_COMPLETE = 1U << 1,
    MITTENS_DMA_STATUS_ERROR = 1U << 2,
    MITTENS_DMA_STATUS_BUSY = 1U << 3,
    MITTENS_DMA_STATUS_MACRO_ACTIVE = 1U << 4,
    MITTENS_DMA_EXACT_READINESS = 1U << 0,
    MITTENS_DMA_EPOCH_ZERO_SOURCE = 1U << 1,
    MITTENS_DMA_EXACT_EXECUTION_TEARDOWN = 1U << 2,
    MITTENS_DMA_KNOWN_REQUEST_FLAGS =
        MITTENS_DMA_EXACT_READINESS |
        MITTENS_DMA_EPOCH_ZERO_SOURCE |
        MITTENS_DMA_EXACT_EXECUTION_TEARDOWN,
};

static void mittens_sync_yield_scratchpad_dma(
    MittensSyncDeviceState *s,
    uint32_t reason,
    const MittensScratchpadDMAJob *job)
{
    const uint64_t logical_iteration =
        job != NULL ? job->logical_iteration : s->dma_logical_iteration;
    const uint64_t scratchpad_offset =
        job != NULL ? job->scratchpad_offset : s->dma_destination;
    const uint32_t direction =
        job != NULL ? job->direction : s->dma_direction;
    const uint64_t global_offset =
        job != NULL ? job->global_offset : s->dma_source;
    const uint32_t token_id =
        job != NULL ? job->token_id : s->dma_token_id;
    const uint64_t execution_id =
        job != NULL ? job->execution_id : s->dma_execution_id;
    const uint32_t byte_count =
        job != NULL ? job->byte_count : s->dma_byte_count;
    const uint32_t request_flags =
        job != NULL ? job->request_flags : s->dma_request_flags;

    s->bridge->global_dma_logical_iteration =
        logical_iteration;
    s->bridge->global_dma_scratchpad_offset =
        scratchpad_offset;
    s->bridge->global_dma_request_flags = request_flags;
    mittens_sync_publish_event(
        reason,
        MITTENS_SYNC_EVENT_FLAG_NONE,
        direction,
        global_offset,
        token_id,
        execution_id,
        UINT32_MAX,
        UINT32_MAX,
        0,
        scratchpad_offset,
        byte_count,
        MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD,
        mittens_sync_current_executed(),
        true);
}

static bool mittens_sync_dma_addresses_valid(
    MittensSyncDeviceState *s)
{
    const bool exact =
        (s->dma_request_flags & MITTENS_DMA_EXACT_READINESS) != 0;
    const bool epoch_zero =
        (s->dma_request_flags & MITTENS_DMA_EPOCH_ZERO_SOURCE) != 0;
    const bool teardown =
        (s->dma_request_flags &
         MITTENS_DMA_EXACT_EXECUTION_TEARDOWN) != 0;
    if (s->global_ram == NULL || s->dma_direction > 1 ||
        (s->dma_request_flags & ~MITTENS_DMA_KNOWN_REQUEST_FLAGS) != 0 ||
        (!exact && s->dma_request_flags != 0) ||
        (epoch_zero && (!exact || s->dma_direction != 0))) {
        return false;
    }
    if (teardown) {
        return exact && !epoch_zero && s->dma_direction == 1 &&
               s->dma_byte_count == 0 && s->dma_source == 0 &&
               s->dma_destination == 0 &&
               s->dma_logical_iteration == UINT64_MAX;
    }
    return s->dma_byte_count != 0 &&
           mittens_memory_contains_range(0, s->global_ram_size, s->dma_source, s->dma_byte_count) &&
           mittens_memory_contains_range(0, s->scratchpad_size, s->dma_destination, s->dma_byte_count);
}

static void mittens_sync_dma_job_free(gpointer opaque)
{
    MittensScratchpadDMAJob *job = opaque;

    if (job != NULL) {
        g_free(job);
    }
}

static MittensScratchpadDMAJob *mittens_sync_find_dma_job(
    MittensSyncDeviceState *s,
    uint64_t execution_id,
    uint32_t token_id,
    guint *job_index)
{
    guint index;

    if (s->dma_jobs == NULL) {
        return NULL;
    }
    for (index = 0; index < s->dma_jobs->len; ++index) {
        MittensScratchpadDMAJob *job =
            g_ptr_array_index(s->dma_jobs, index);
        if (job->execution_id == execution_id &&
            job->token_id == token_id) {
            if (job_index != NULL) {
                *job_index = index;
            }
            return job;
        }
    }
    return NULL;
}

static void mittens_sync_record_global_dma_job(
    MittensSyncDeviceState *s,
    MittensSyncGlobalDMASubmit *record,
    const MittensScratchpadDMAJob *job)
{
    record->instructions_executed = mittens_sync_current_executed();
    record->vector_instructions_executed = s->vector_instructions_executed;
    record->wait_instructions_executed = UINT64_MAX;
    record->wait_vector_instructions_executed = UINT64_MAX;
    record->global_offset = job->global_offset;
    record->scratchpad_offset = job->scratchpad_offset;
    record->execution_id = job->execution_id;
    record->logical_iteration = job->logical_iteration;
    record->token_id = job->token_id;
    record->byte_count = job->byte_count;
    record->direction = job->direction;
    record->request_flags = job->request_flags;
    record->submit_event_ordinal = UINT32_MAX;
    record->wait_event_ordinal = UINT32_MAX;
}

static void mittens_sync_dma_command(
    MittensSyncDeviceState *s,
    uint32_t command)
{
    MemTxResult result;

    if (command == MITTENS_DMA_COMMAND_MACRO_BEGIN) {
        const uint32_t count = s->dma_byte_count;
        const uint64_t required_instruction_budget = s->dma_source;

        /*
         * The command is deliberately a no-op when macro execution is
         * disabled or too close to a quantum boundary. The guest runs the
         * identical submit/wait loop in both modes, so this fail-closed path
         * falls back to ordinary scalar synchronization without a new branch.
         */
        if (!s->global_dma_macro_execution || s->memory_timing ||
            s->accounted_remaining < 0 ||
            required_instruction_budget == 0 ||
            (uint64_t)s->accounted_remaining < required_instruction_budget) {
            s->dma_status = MITTENS_DMA_STATUS_READY |
                            MITTENS_DMA_STATUS_COMPLETE;
            return;
        }
        (void)mittens_sync_flush_analog_batch(
            MITTENS_SYNC_EVENT_FLAG_NONE, true);
        (void)mittens_sync_flush_global_dma_batch(
            MITTENS_SYNC_EVENT_FLAG_NONE, true);
        (void)mittens_sync_flush_memory_batch(
            MITTENS_SYNC_EVENT_FLAG_NONE, true);
        if (s->global_dma_submit_batching ||
            s->global_dma_macro_active || count < 2 ||
            count > MITTENS_DMA_MAX_JOBS ||
            (s->dma_jobs != NULL && s->dma_jobs->len != 0) ||
            s->global_dma_batch == NULL ||
            s->global_dma_batch_count != 0) {
            s->dma_error = 9;
            s->dma_status = MITTENS_DMA_STATUS_READY |
                            MITTENS_DMA_STATUS_ERROR;
            return;
        }
        s->global_dma_macro_active = true;
        s->global_dma_macro_execution_id = s->dma_execution_id;
        s->global_dma_macro_expected_count = count;
        s->global_dma_macro_wait_count = 0;
        s->global_dma_macro_event_count = 0;
        s->global_dma_macro_begin_instructions =
            mittens_sync_current_executed();
        s->global_dma_macro_maximum_instruction_span =
            required_instruction_budget;
        s->dma_status = MITTENS_DMA_STATUS_READY |
                        MITTENS_DMA_STATUS_COMPLETE |
                        MITTENS_DMA_STATUS_MACRO_ACTIVE;
        return;
    }

    if (command == MITTENS_DMA_COMMAND_SUBMIT) {
        MittensScratchpadDMAJob *job;
        const bool exact_write =
            (s->dma_request_flags & MITTENS_DMA_EXACT_READINESS) != 0 &&
            (s->dma_request_flags &
             MITTENS_DMA_EXACT_EXECUTION_TEARDOWN) == 0 &&
            s->dma_direction == 1;

        (void)mittens_sync_flush_analog_batch(
            MITTENS_SYNC_EVENT_FLAG_NONE, true);
        if (s->dma_jobs == NULL) {
            s->dma_jobs = g_ptr_array_new_with_free_func(
                mittens_sync_dma_job_free);
        }
        if (s->dma_jobs->len >= MITTENS_DMA_MAX_JOBS ||
            mittens_sync_find_dma_job(
                s, s->dma_execution_id, s->dma_token_id, NULL) != NULL ||
            !mittens_sync_dma_addresses_valid(s)) {
            s->dma_error = 1;
            s->dma_status |= MITTENS_DMA_STATUS_ERROR;
            return;
        }
        job = g_new0(MittensScratchpadDMAJob, 1);
        job->execution_id = s->dma_execution_id;
        job->token_id = s->dma_token_id;
        job->direction = s->dma_direction;
        job->request_flags = s->dma_request_flags;
        job->global_offset = s->dma_source;
        job->scratchpad_offset = s->dma_destination;
        job->logical_iteration = s->dma_logical_iteration;
        job->byte_count = s->dma_byte_count;
        if (exact_write) {
            result = address_space_read(
                &address_space_memory,
                s->scratchpad_base + job->scratchpad_offset,
                MEMTXATTRS_UNSPECIFIED,
                s->global_ram + job->global_offset,
                job->byte_count);
            if (result != MEMTX_OK) {
                g_free(job);
                s->dma_error = 4;
                s->dma_status = MITTENS_DMA_STATUS_READY |
                                MITTENS_DMA_STATUS_ERROR;
                return;
            }
            __atomic_thread_fence(__ATOMIC_RELEASE);
            job->write_staged = true;
        }
        g_ptr_array_add(s->dma_jobs, job);
        s->dma_status = MITTENS_DMA_STATUS_BUSY |
                        (s->global_dma_macro_active
                            ? MITTENS_DMA_STATUS_MACRO_ACTIVE : 0) |
                        (s->dma_jobs->len < MITTENS_DMA_MAX_JOBS
                            ? MITTENS_DMA_STATUS_READY : 0);
        if (s->global_dma_macro_active) {
            MittensSyncGlobalDMASubmit *record;

            if (job->execution_id != s->global_dma_macro_execution_id ||
                s->global_dma_batch == NULL ||
                s->global_dma_batch_count >=
                    s->global_dma_macro_expected_count ||
                s->global_dma_batch_count >=
                    MITTENS_SYNC_GLOBAL_DMA_BATCH_CAPACITY) {
                s->dma_error = 9;
                s->dma_status |= MITTENS_DMA_STATUS_ERROR;
                return;
            }
            record = &s->global_dma_batch[s->global_dma_batch_count++];
            mittens_sync_record_global_dma_job(s, record, job);
            if (s->global_dma_macro_event_count >=
                2U * s->global_dma_macro_expected_count) {
                s->dma_error = 9;
                s->dma_status |= MITTENS_DMA_STATUS_ERROR;
                return;
            }
            record->submit_event_ordinal =
                s->global_dma_macro_event_count++;
            return;
        }
        if (s->global_dma_submit_batching) {
            MittensSyncGlobalDMASubmit *record;

            /* Earlier timed memory accesses must reach SST first. */
            (void)mittens_sync_flush_memory_batch(
                MITTENS_SYNC_EVENT_FLAG_NONE, true);
            if (s->global_dma_batch == NULL ||
                s->global_dma_batch_count >=
                    MITTENS_SYNC_GLOBAL_DMA_BATCH_CAPACITY) {
                mittens_sync_set_error(
                    MITTENS_SYNC_BRIDGE_ERROR_BAD_BUDGET,
                    "global DMA submit batch exceeded bridge capacity");
                s->dma_error = 7;
                s->dma_status |= MITTENS_DMA_STATUS_ERROR;
                return;
            }
            record = &s->global_dma_batch[s->global_dma_batch_count++];
            mittens_sync_record_global_dma_job(s, record, job);
            return;
        }
        mittens_sync_yield_scratchpad_dma(
            s, MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT, job);
        return;
    }
    if (command == MITTENS_DMA_COMMAND_WAIT_BATCH) {
        MittensScratchpadDMAJob *jobs[MITTENS_DMA_MAX_JOBS] = {0};
        const uint32_t count = s->dma_byte_count;
        const uint32_t first_token = s->dma_token_id;
        const bool carries_submits = s->global_dma_batch_count != 0;
        uint32_t event_flags = MITTENS_SYNC_EVENT_FLAG_NONE;
        uint32_t index;
        bool copy_failed = false;

        if (s->global_dma_macro_active ||
            s->dma_jobs == NULL || count == 0 ||
            count > MITTENS_DMA_MAX_JOBS || s->dma_jobs->len != count ||
            first_token > UINT32_MAX - (count - 1U) ||
            s->global_dma_batch == NULL ||
            (carries_submits && s->global_dma_batch_count != count)) {
            s->dma_error = 8;
            s->dma_status |= MITTENS_DMA_STATUS_ERROR;
            return;
        }
        for (index = 0; index < count; ++index) {
            jobs[index] = mittens_sync_find_dma_job(
                s, s->dma_execution_id, first_token + index, NULL);
            if (jobs[index] == NULL ||
                (carries_submits &&
                 (s->global_dma_batch[index].execution_id !=
                      s->dma_execution_id ||
                  s->global_dma_batch[index].token_id !=
                      first_token + index))) {
                s->dma_error = 8;
                s->dma_status |= MITTENS_DMA_STATUS_ERROR;
                return;
            }
        }

        /*
         * Any intervening timed event flushes the submit batch before reaching
         * this command. Otherwise one bridge event carries the exact submit
         * records and the collective wait, eliminating the scalar rendezvouses
         * without changing a physical request.
         */
        if (carries_submits) {
            event_flags |= MITTENS_SYNC_EVENT_FLAG_GLOBAL_DMA_SUBMITS;
        } else {
            (void)mittens_sync_flush_analog_batch(
                MITTENS_SYNC_EVENT_FLAG_NONE, true);
            (void)mittens_sync_flush_memory_batch(
                MITTENS_SYNC_EVENT_FLAG_NONE, true);
            for (index = 0; index < count; ++index) {
                mittens_sync_record_global_dma_job(
                    s, &s->global_dma_batch[index], jobs[index]);
            }
        }
        mittens_sync_publish_event_raw(
            MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH,
            event_flags,
            UINT32_MAX,
            0,
            first_token,
            s->dma_execution_id,
            UINT32_MAX,
            UINT32_MAX,
            0,
            0,
            0,
            MITTENS_SYNC_MEMORY_FLAG_NONE,
            UINT32_MAX,
            UINT32_MAX,
            0,
            count,
            0,
            mittens_sync_current_executed(),
            true);
        s->global_dma_batch_count = 0;

        for (index = 0; index < count; ++index) {
            MittensScratchpadDMAJob *job = jobs[index];
            guint job_index = 0;

            if ((job->request_flags &
                 MITTENS_DMA_EXACT_EXECUTION_TEARDOWN) != 0) {
                result = MEMTX_OK;
            } else if (job->direction == 0) {
                if ((job->request_flags & MITTENS_DMA_EXACT_READINESS) != 0) {
                    __atomic_thread_fence(__ATOMIC_ACQUIRE);
                }
                result = address_space_write(
                    &address_space_memory,
                    s->scratchpad_base + job->scratchpad_offset,
                    MEMTXATTRS_UNSPECIFIED,
                    s->global_ram + job->global_offset,
                    job->byte_count);
            } else if (!job->write_staged) {
                result = address_space_read(
                    &address_space_memory,
                    s->scratchpad_base + job->scratchpad_offset,
                    MEMTXATTRS_UNSPECIFIED,
                    s->global_ram + job->global_offset,
                    job->byte_count);
            } else {
                result = MEMTX_OK;
            }
            if (result != MEMTX_OK) {
                copy_failed = true;
            }
            if (mittens_sync_find_dma_job(
                    s, job->execution_id, job->token_id, &job_index) == NULL) {
                copy_failed = true;
                continue;
            }
            g_ptr_array_remove_index(s->dma_jobs, job_index);
        }
        if (copy_failed) {
            s->dma_error = 4;
            s->dma_status = MITTENS_DMA_STATUS_READY |
                            MITTENS_DMA_STATUS_ERROR;
            return;
        }
        s->dma_status = MITTENS_DMA_STATUS_READY |
                        MITTENS_DMA_STATUS_COMPLETE |
                        (s->dma_jobs->len != 0
                             ? MITTENS_DMA_STATUS_BUSY : 0);
        return;
    }
    if (command == MITTENS_DMA_COMMAND_WAIT) {
        guint job_index = 0;
        MittensScratchpadDMAJob *job = mittens_sync_find_dma_job(
            s, s->dma_execution_id, s->dma_token_id, &job_index);
        if (job == NULL) {
            s->dma_error = 3;
            s->dma_status |= MITTENS_DMA_STATUS_ERROR;
            return;
        }
        if (s->global_dma_macro_active) {
            MittensSyncGlobalDMASubmit *record;
            uint32_t record_index;

            if (s->global_dma_macro_wait_count >=
                    s->global_dma_batch_count ||
                s->global_dma_macro_wait_count >=
                    s->global_dma_macro_expected_count ||
                s->global_dma_macro_event_count >=
                    2U * s->global_dma_macro_expected_count) {
                s->dma_error = 9;
                s->dma_status |= MITTENS_DMA_STATUS_ERROR;
                return;
            }
            record = NULL;
            for (record_index = 0;
                 record_index < s->global_dma_batch_count;
                 ++record_index) {
                MittensSyncGlobalDMASubmit *candidate =
                    &s->global_dma_batch[record_index];
                if (candidate->execution_id == s->dma_execution_id &&
                    candidate->token_id == s->dma_token_id &&
                    candidate->wait_instructions_executed == UINT64_MAX &&
                    candidate->wait_event_ordinal == UINT32_MAX) {
                    record = candidate;
                    break;
                }
            }
            if (record == NULL ||
                record->wait_instructions_executed != UINT64_MAX ||
                record->wait_vector_instructions_executed != UINT64_MAX ||
                record->submit_event_ordinal == UINT32_MAX ||
                record->wait_event_ordinal != UINT32_MAX) {
                s->dma_error = 9;
                s->dma_status |= MITTENS_DMA_STATUS_ERROR;
                return;
            }
            record->wait_instructions_executed =
                mittens_sync_current_executed();
            record->wait_vector_instructions_executed =
                s->vector_instructions_executed;
            record->wait_event_ordinal =
                s->global_dma_macro_event_count++;
            ++s->global_dma_macro_wait_count;
            /*
             * A scalar descriptor may legally reuse one token after its wait.
             * Retire the live job now while retaining the immutable tape record;
             * functional payload movement remains deferred to macro end.
             */
            g_ptr_array_remove_index(s->dma_jobs, job_index);
            s->dma_status = MITTENS_DMA_STATUS_READY |
                            MITTENS_DMA_STATUS_COMPLETE |
                            MITTENS_DMA_STATUS_MACRO_ACTIVE |
                            (s->dma_jobs->len != 0
                                 ? MITTENS_DMA_STATUS_BUSY : 0);
            return;
        }
        /* Publish every preceding submit before its completion wait. */
        (void)mittens_sync_flush_global_dma_batch(
            MITTENS_SYNC_EVENT_FLAG_NONE, true);
        mittens_sync_yield_scratchpad_dma(
            s, MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT, job);
        if ((job->request_flags &
             MITTENS_DMA_EXACT_EXECUTION_TEARDOWN) != 0) {
            result = MEMTX_OK;
        } else if (job->direction == 0) {
            if ((job->request_flags & MITTENS_DMA_EXACT_READINESS) != 0) {
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
            }
            result = address_space_write(
                &address_space_memory,
                s->scratchpad_base + job->scratchpad_offset,
                MEMTXATTRS_UNSPECIFIED,
                s->global_ram + job->global_offset,
                job->byte_count);
        } else if (!job->write_staged) {
            result = address_space_read(
                &address_space_memory,
                s->scratchpad_base + job->scratchpad_offset,
                MEMTXATTRS_UNSPECIFIED,
                s->global_ram + job->global_offset,
                job->byte_count);
        } else {
            result = MEMTX_OK;
        }
        g_ptr_array_remove_index(s->dma_jobs, job_index);
        if (result != MEMTX_OK) {
            s->dma_error = 4;
            s->dma_status = MITTENS_DMA_STATUS_READY |
                            MITTENS_DMA_STATUS_ERROR;
            return;
        }
        s->dma_status = MITTENS_DMA_STATUS_READY |
                        MITTENS_DMA_STATUS_COMPLETE |
                        (s->dma_jobs->len != 0
                            ? MITTENS_DMA_STATUS_BUSY : 0);
        return;
    }
    if (command == MITTENS_DMA_COMMAND_MACRO_END) {
        const uint32_t count = s->dma_byte_count;
        const uint64_t end_instructions = mittens_sync_current_executed();
        uint32_t index;
        bool copy_failed = false;

        /* Disabled or quantum-declined macro execution is a scalar no-op. */
        if (!s->global_dma_macro_active) {
            s->dma_status = MITTENS_DMA_STATUS_READY |
                            MITTENS_DMA_STATUS_COMPLETE |
                            ((s->dma_jobs != NULL && s->dma_jobs->len != 0)
                                 ? MITTENS_DMA_STATUS_BUSY : 0);
            return;
        }
        if (s->dma_execution_id != s->global_dma_macro_execution_id ||
            count != s->global_dma_macro_expected_count ||
            s->global_dma_batch_count != count ||
            s->global_dma_macro_wait_count != count ||
            s->global_dma_macro_event_count != 2U * count ||
            s->global_dma_macro_maximum_instruction_span == 0 ||
            end_instructions < s->global_dma_macro_begin_instructions ||
            end_instructions - s->global_dma_macro_begin_instructions >
                s->global_dma_macro_maximum_instruction_span ||
            s->dma_jobs == NULL || s->dma_jobs->len != 0) {
            s->dma_error = 9;
            s->dma_status = MITTENS_DMA_STATUS_READY |
                            MITTENS_DMA_STATUS_ERROR;
            return;
        }
        for (index = 0; index < count; ++index) {
            const MittensSyncGlobalDMASubmit *record =
                &s->global_dma_batch[index];
            if (record->execution_id != s->global_dma_macro_execution_id ||
                record->wait_instructions_executed == UINT64_MAX ||
                record->wait_vector_instructions_executed == UINT64_MAX ||
                record->submit_event_ordinal == UINT32_MAX ||
                record->wait_event_ordinal == UINT32_MAX ||
                record->submit_event_ordinal >= record->wait_event_ordinal ||
                record->wait_event_ordinal >= 2U * count) {
                s->dma_error = 9;
                s->dma_status = MITTENS_DMA_STATUS_READY |
                                MITTENS_DMA_STATUS_ERROR;
                return;
            }
        }

        mittens_sync_publish_event_raw(
            MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO,
            MITTENS_SYNC_EVENT_FLAG_NONE,
            UINT32_MAX,
            0,
            UINT32_MAX,
            s->global_dma_macro_execution_id,
            UINT32_MAX,
            UINT32_MAX,
            0,
            0,
            0,
            MITTENS_SYNC_MEMORY_FLAG_NONE,
            UINT32_MAX,
            UINT32_MAX,
            0,
            count,
            0,
            mittens_sync_current_executed(),
            true);

        /*
         * SST has copied the immutable records before resuming this event.
         * End capture before functional payload copies so their ordinary
         * scratchpad timing callbacks remain visible exactly as in scalar
         * wait handling. The jobs themselves remain live until each copy is
         * complete below.
         */
        s->global_dma_macro_active = false;
        s->global_dma_macro_execution_id = 0;
        s->global_dma_macro_expected_count = 0;
        s->global_dma_macro_wait_count = 0;
        s->global_dma_macro_event_count = 0;
        s->global_dma_macro_begin_instructions = 0;
        s->global_dma_macro_maximum_instruction_span = 0;
        s->global_dma_batch_count = 0;

        for (index = 0; index < count; ++index) {
            const MittensSyncGlobalDMASubmit *record =
                &s->global_dma_batch[index];
            const bool exact_write =
                (record->request_flags & MITTENS_DMA_EXACT_READINESS) != 0 &&
                (record->request_flags &
                 MITTENS_DMA_EXACT_EXECUTION_TEARDOWN) == 0 &&
                record->direction == 1;

            if ((record->request_flags &
                 MITTENS_DMA_EXACT_EXECUTION_TEARDOWN) != 0) {
                result = MEMTX_OK;
            } else if (record->direction == 0) {
                if ((record->request_flags & MITTENS_DMA_EXACT_READINESS) != 0) {
                    __atomic_thread_fence(__ATOMIC_ACQUIRE);
                }
                result = address_space_write(
                    &address_space_memory,
                    s->scratchpad_base + record->scratchpad_offset,
                    MEMTXATTRS_UNSPECIFIED,
                    s->global_ram + record->global_offset,
                    record->byte_count);
            } else if (!exact_write) {
                result = address_space_read(
                    &address_space_memory,
                    s->scratchpad_base + record->scratchpad_offset,
                    MEMTXATTRS_UNSPECIFIED,
                    s->global_ram + record->global_offset,
                    record->byte_count);
            } else {
                result = MEMTX_OK;
            }
            if (result != MEMTX_OK) {
                copy_failed = true;
            }
        }
        if (copy_failed) {
            s->dma_error = 4;
            s->dma_status = MITTENS_DMA_STATUS_READY |
                            MITTENS_DMA_STATUS_ERROR;
            return;
        }
        s->dma_status = MITTENS_DMA_STATUS_READY |
                        MITTENS_DMA_STATUS_COMPLETE;
        return;
    }
    if (command == MITTENS_DMA_COMMAND_INITIALIZE_GLOBAL_RAM) {
        /*
         * Model input bytes are deployment state, not an inference-time DMA
         * request. Populate the shared backing immediately and account the
         * transfer in the existing aggregate initialization phase. Later
         * global-RAM reads still use submit/wait and remain fully timed.
         */
        if (s->global_dma_macro_active ||
            !s->memory_init_active || s->dma_direction != 1 ||
            (s->dma_jobs != NULL && s->dma_jobs->len != 0) ||
            !mittens_sync_dma_addresses_valid(s) ||
            s->memory_init_accesses == UINT64_MAX ||
            s->memory_init_write_bytes >
                UINT64_MAX - s->dma_byte_count) {
            s->dma_error = 6;
            s->dma_status = MITTENS_DMA_STATUS_READY |
                            MITTENS_DMA_STATUS_ERROR;
            return;
        }
        result = address_space_read(
            &address_space_memory,
            s->scratchpad_base + s->dma_destination,
            MEMTXATTRS_UNSPECIFIED,
            s->global_ram + s->dma_source,
            s->dma_byte_count);
        if (result != MEMTX_OK) {
            s->dma_error = 4;
            s->dma_status = MITTENS_DMA_STATUS_READY |
                            MITTENS_DMA_STATUS_ERROR;
            return;
        }
        ++s->memory_init_accesses;
        s->memory_init_write_bytes += s->dma_byte_count;
        s->dma_status = MITTENS_DMA_STATUS_READY |
                        MITTENS_DMA_STATUS_COMPLETE;
        return;
    }
    s->dma_error = 5;
    s->dma_status = MITTENS_DMA_STATUS_READY |
                    MITTENS_DMA_STATUS_ERROR;
}

static uint64_t mittens_sync_dma_mmio_read(
    void *opaque,
    hwaddr offset,
    unsigned size)
{
    MittensSyncDeviceState *s = opaque;
    (void)size;
    switch (offset) {
    case 0x00: return (uint32_t)s->dma_source;
    case 0x04: return (uint32_t)(s->dma_source >> 32);
    case 0x08: return (uint32_t)s->dma_destination;
    case 0x0c: return (uint32_t)(s->dma_destination >> 32);
    case 0x10: return s->dma_byte_count;
    case 0x14: return s->dma_token_id;
    case 0x18: return (uint32_t)s->dma_execution_id;
    case 0x1c: return (uint32_t)(s->dma_execution_id >> 32);
    case 0x20: return s->dma_direction;
    case 0x28: return s->dma_status;
    case 0x2c: return s->dma_error;
    case 0x30: return (uint32_t)s->dma_logical_iteration;
    case 0x34: return (uint32_t)(s->dma_logical_iteration >> 32);
    case 0x38: return s->dma_request_flags;
    default: return 0;
    }
}

static void mittens_sync_dma_mmio_write(
    void *opaque,
    hwaddr offset,
    uint64_t value,
    unsigned size)
{
    MittensSyncDeviceState *s = opaque;
    uint32_t word = value;
    (void)size;
    switch (offset) {
    case 0x00: s->dma_source = (s->dma_source & UINT64_C(0xffffffff00000000)) | word; break;
    case 0x04: s->dma_source = (s->dma_source & UINT32_MAX) | ((uint64_t)word << 32); break;
    case 0x08: s->dma_destination = (s->dma_destination & UINT64_C(0xffffffff00000000)) | word; break;
    case 0x0c: s->dma_destination = (s->dma_destination & UINT32_MAX) | ((uint64_t)word << 32); break;
    case 0x10: s->dma_byte_count = word; break;
    case 0x14: s->dma_token_id = word; break;
    case 0x18: s->dma_execution_id = (s->dma_execution_id & UINT64_C(0xffffffff00000000)) | word; break;
    case 0x1c: s->dma_execution_id = (s->dma_execution_id & UINT32_MAX) | ((uint64_t)word << 32); break;
    case 0x20: s->dma_direction = word; break;
    case 0x24: mittens_sync_dma_command(s, word); break;
    case 0x30: s->dma_logical_iteration = (s->dma_logical_iteration & UINT64_C(0xffffffff00000000)) | word; break;
    case 0x34: s->dma_logical_iteration = (s->dma_logical_iteration & UINT32_MAX) | ((uint64_t)word << 32); break;
    case 0x38: s->dma_request_flags = word; break;
    default: break;
    }
}

static const MemoryRegionOps mittens_sync_dma_mmio_ops = {
    .read = mittens_sync_dma_mmio_read,
    .write = mittens_sync_dma_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void mittens_sync_realize(DeviceState *device, Error **errp)
{
    MittensSyncDeviceState *s = MITTENS_SYNC(device);
    struct stat bridge_stat;

    if (mittens_sync_instance != NULL) {
        error_setg(errp, "only one mittens-sync device is supported");
        return;
    }
    mittens_sync_instance = s;

    if (s->global_ram_fd >= 0) {
        if (s->global_ram_size == 0) {
            error_setg(errp, "mittens-sync global RAM size is zero");
            return;
        }
        s->global_ram = mmap(
            NULL,
            s->global_ram_size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            s->global_ram_fd,
            0);
        if (s->global_ram == MAP_FAILED) {
            s->global_ram = NULL;
            error_setg_errno(errp, errno, "mittens-sync cannot map global RAM");
            return;
        }
        close(s->global_ram_fd);
        s->global_ram_fd = -1;
    } else if (s->scratchpad_enabled) {
        error_setg(
            errp, "mittens-sync scratchpad DMA requires global RAM backing");
        return;
    }

    if (s->scratchpad_enabled) {
        if (s->scratchpad_size == 0 ||
            s->scratchpad_size > UINT64_C(16) * 1024 * 1024) {
            error_setg(errp, "mittens-sync scratchpad size is invalid");
            return;
        }
        memory_region_init_ram(
            &s->scratchpad,
            OBJECT(device),
            "mittens-scratchpad",
            s->scratchpad_size,
            errp);
        if (*errp != NULL) {
            return;
        }
        memory_region_add_subregion(
            get_system_memory(), s->scratchpad_base, &s->scratchpad);
        s->scratchpad_mapped = true;
        memory_region_init_io(
            &s->scratchpad_dma_mmio,
            OBJECT(device),
            &mittens_sync_dma_mmio_ops,
            s,
            "mittens-scratchpad-dma",
            0x1000);
        memory_region_add_subregion(
            get_system_memory(),
            s->scratchpad_dma_base,
            &s->scratchpad_dma_mmio);
        s->scratchpad_dma_mapped = true;
        s->dma_status = MITTENS_DMA_STATUS_READY;
    }

    if (s->bridge_fd < 0) {
        return;
    }
    if (fstat(s->bridge_fd, &bridge_stat) < 0) {
        error_setg_errno(
            errp, errno, "mittens-sync cannot stat bridge fd");
        return;
    }
    if (bridge_stat.st_size != MITTENS_SYNC_BRIDGE_MAPPING_SIZE) {
        error_setg(
            errp,
            "mittens-sync bridge has invalid size: %jd bytes",
            (intmax_t)bridge_stat.st_size);
        return;
    }

    s->bridge = mmap(
        NULL,
        MITTENS_SYNC_BRIDGE_MAPPING_SIZE,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        s->bridge_fd,
        0);
    if (s->bridge == MAP_FAILED) {
        s->bridge = NULL;
        error_setg_errno(
            errp, errno, "mittens-sync cannot map bridge fd");
        return;
    }
    close(s->bridge_fd);
    s->bridge_fd = -1;

    if (s->bridge->magic != MITTENS_SYNC_BRIDGE_MAGIC ||
        s->bridge->version != MITTENS_SYNC_BRIDGE_VERSION ||
        s->bridge->structure_size != sizeof(MittensSyncBridge)) {
        munmap(s->bridge, MITTENS_SYNC_BRIDGE_MAPPING_SIZE);
        s->bridge = NULL;
        error_setg(errp, "mittens-sync bridge header is incompatible");
        return;
    }
    if (s->memory_access_batch_records == 0) {
        munmap(s->bridge, MITTENS_SYNC_BRIDGE_MAPPING_SIZE);
        s->bridge = NULL;
        error_setg(
            errp,
            "mittens-sync logical memory access batch size is invalid");
        return;
    }
    s->memory_batch = mittens_sync_memory_batch(s->bridge);
    s->memory_batch_count = 0;
    s->memory_batch_logical_count = 0;
    s->global_dma_batch = mittens_sync_global_dma_batch(s->bridge);
    s->global_dma_batch_count = 0;
    s->analog_batch = mittens_sync_analog_batch(s->bridge);
    s->analog_batch_count = 0;
    s->memory_init_active =
        s->memory_init_batching &&
        (s->memory_timing ||
         (s->scratchpad_enabled && s->global_ram != NULL));
}

static void mittens_sync_unrealize(DeviceState *device)
{
    MittensSyncDeviceState *s = MITTENS_SYNC(device);

    g_clear_pointer(&s->dma_jobs, g_ptr_array_unref);
    if (s->global_ram != NULL) {
        munmap(s->global_ram, s->global_ram_size);
        s->global_ram = NULL;
    }
    if (s->global_ram_fd >= 0) {
        close(s->global_ram_fd);
        s->global_ram_fd = -1;
    }
    if (s->scratchpad_dma_mapped) {
        memory_region_del_subregion(
            get_system_memory(), &s->scratchpad_dma_mmio);
        memory_region_unref(&s->scratchpad_dma_mmio);
        s->scratchpad_dma_mapped = false;
    }
    if (s->scratchpad_mapped) {
        memory_region_del_subregion(
            get_system_memory(), &s->scratchpad);
        memory_region_unref(&s->scratchpad);
        s->scratchpad_mapped = false;
    }

    if (s->bridge != NULL) {
        mittens_sync_wake(&s->bridge->state);
        munmap(s->bridge, MITTENS_SYNC_BRIDGE_MAPPING_SIZE);
        s->bridge = NULL;
        s->memory_batch = NULL;
        s->memory_batch_count = 0;
        s->memory_batch_logical_count = 0;
        s->global_dma_batch = NULL;
        s->global_dma_batch_count = 0;
        s->analog_batch = NULL;
        s->analog_batch_count = 0;
    }
    if (s->bridge_fd >= 0) {
        close(s->bridge_fd);
        s->bridge_fd = -1;
    }
    if (mittens_sync_instance == s) {
        mittens_sync_instance = NULL;
    }
}

static Property mittens_sync_properties[] = {
    DEFINE_PROP_INT32(
        "bridge-fd", MittensSyncDeviceState, bridge_fd, -1),
    DEFINE_PROP_INT32(
        "global-ram-fd", MittensSyncDeviceState, global_ram_fd, -1),
    DEFINE_PROP_UINT64(
        "global-ram-size",
        MittensSyncDeviceState,
        global_ram_size,
        UINT64_C(34359738368)),
    DEFINE_PROP_BOOL(
        "memory-timing", MittensSyncDeviceState, memory_timing, false),
    DEFINE_PROP_BOOL(
        "instruction-fetch-timing",
        MittensSyncDeviceState,
        instruction_fetch_timing,
        false),
    DEFINE_PROP_BOOL(
        "memory-init-batching",
        MittensSyncDeviceState,
        memory_init_batching,
        false),
    DEFINE_PROP_BOOL(
        "memory-access-batching",
        MittensSyncDeviceState,
        memory_access_batching,
        false),
    DEFINE_PROP_BOOL(
        "scratchpad-access-batching",
        MittensSyncDeviceState,
        scratchpad_access_batching,
        false),
    DEFINE_PROP_BOOL(
        "scratchpad-access-run-compaction",
        MittensSyncDeviceState,
        scratchpad_access_run_compaction,
        false),
    DEFINE_PROP_BOOL(
        "memory-event-batching",
        MittensSyncDeviceState,
        memory_event_batching,
        false),
    DEFINE_PROP_BOOL(
        "global-dma-submit-batching",
        MittensSyncDeviceState,
        global_dma_submit_batching,
        false),
    DEFINE_PROP_BOOL(
        "global-dma-macro-execution",
        MittensSyncDeviceState,
        global_dma_macro_execution,
        false),
    DEFINE_PROP_BOOL(
        "analog-command-batching",
        MittensSyncDeviceState,
        analog_command_batching,
        false),
    DEFINE_PROP_UINT32(
        "memory-access-batch-records",
        MittensSyncDeviceState,
        memory_access_batch_records,
        16),
    DEFINE_PROP_BOOL(
        "scratchpad-enabled",
        MittensSyncDeviceState,
        scratchpad_enabled,
        false),
    DEFINE_PROP_UINT64(
        "scratchpad-base",
        MittensSyncDeviceState,
        scratchpad_base,
        MITTENS_SCRATCHPAD_BASE),
    DEFINE_PROP_UINT64(
        "scratchpad-size",
        MittensSyncDeviceState,
        scratchpad_size,
        UINT64_C(256) * 1024),
    DEFINE_PROP_UINT64(
        "scratchpad-dma-base",
        MittensSyncDeviceState,
        scratchpad_dma_base,
        UINT64_C(0x10011000)),
    DEFINE_PROP_END_OF_LIST(),
};

static void mittens_sync_class_init(
    ObjectClass *object_class,
    void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(object_class);

    device_class_set_props(device_class, mittens_sync_properties);
    device_class->realize = mittens_sync_realize;
    device_class->unrealize = mittens_sync_unrealize;
}

static const TypeInfo mittens_sync_info = {
    .name = TYPE_MITTENS_SYNC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(MittensSyncDeviceState),
    .class_init = mittens_sync_class_init,
};

static void mittens_sync_register_types(void)
{
    type_register_static(&mittens_sync_info);
}

type_init(mittens_sync_register_types)

DeviceState *mittens_sync_create(void)
{
    DeviceState *device = qdev_new(TYPE_MITTENS_SYNC);

    qdev_realize_and_unref(device, NULL, &error_fatal);
    return device;
}
