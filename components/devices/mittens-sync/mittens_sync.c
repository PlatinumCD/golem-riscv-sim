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
#include "qemu/futex.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/cpus.h"

#include "mittens/SyncTileBridge.h"

typedef struct MittensScratchpadDMAJob {
    uint64_t execution_id;
    uint32_t token_id;
    uint64_t destination;
    GByteArray *bytes;
} MittensScratchpadDMAJob;

typedef struct MittensSyncDeviceState {
    DeviceState parent_obj;
    int32_t bridge_fd;
    MittensSyncBridge *bridge;
    uint64_t accounted_executed;
    uint64_t vector_instructions_executed;
    int64_t accounted_remaining;
    bool terminal_event_published;
    bool memory_timing;
    bool memory_init_batching;
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
    uint64_t dma_source;
    uint64_t dma_destination;
    uint64_t dma_execution_id;
    uint32_t dma_byte_count;
    uint32_t dma_token_id;
    uint32_t dma_direction;
    uint32_t dma_status;
    uint32_t dma_error;
    GPtrArray *dma_jobs;
} MittensSyncDeviceState;

DECLARE_INSTANCE_CHECKER(
    MittensSyncDeviceState, MITTENS_SYNC, TYPE_MITTENS_SYNC)

static MittensSyncDeviceState *mittens_sync_instance;

void helper_mittens_sync_vector_instruction(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    if (s != NULL && s->bridge != NULL) {
        ++s->vector_instructions_executed;
    }
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

static void mittens_sync_set_error(
    enum MittensSyncBridgeError error,
    const char *message)
{
    MittensSyncDeviceState *s = mittens_sync_instance;
    uint32_t expected = MITTENS_SYNC_BRIDGE_ERROR_NONE;

    if (s != NULL && s->bridge != NULL) {
        (void)__atomic_compare_exchange_n(
            &s->bridge->protocol_error,
            &expected,
            (uint32_t)error,
            false,
            __ATOMIC_RELEASE,
            __ATOMIC_RELAXED);
    }
    qemu_log_mask(LOG_GUEST_ERROR, "mittens-sync: %s\n", message);
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
    MittensSyncBridge *bridge;
    uint32_t state;

    if (s == NULL || s->bridge == NULL) {
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

bool mittens_sync_available(void)
{
    return mittens_sync_instance != NULL &&
           mittens_sync_instance->bridge != NULL;
}

bool mittens_sync_memory_timing_enabled(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    return s != NULL && s->bridge != NULL &&
           (s->memory_timing || s->scratchpad_enabled);
}

void mittens_sync_memory_init_complete(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    if (s == NULL || s->bridge == NULL ||
        !s->memory_timing || !s->memory_init_batching ||
        !s->memory_init_active) {
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

    if (s == NULL || s->bridge == NULL) {
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

    if (s == NULL || s->bridge == NULL || executed < 0 ||
        remaining < 0) {
        return;
    }
    s->accounted_executed += (uint64_t)executed;
    s->accounted_remaining = remaining;
}

void mittens_sync_quantum_end(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    if (!mittens_sync_available() ||
        s->terminal_event_published) {
        return;
    }
    mittens_sync_publish_event(
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
        mittens_sync_current_executed(),
        false);
}

void mittens_sync_guest_exit(void)
{
    MittensSyncDeviceState *s = mittens_sync_instance;

    if (!mittens_sync_available() ||
        s->terminal_event_published) {
        return;
    }
    mittens_sync_memory_init_complete();
    s->terminal_event_published = true;
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
        0,
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

void mittens_sync_yield_analog(
    uint32_t reason,
    uint32_t array_id,
    uint64_t analog_sequence,
    bool wait_for_completion)
{
    uint32_t flags = wait_for_completion ?
        MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION :
        MITTENS_SYNC_EVENT_FLAG_NONE;

    if (!mittens_sync_available()) {
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
        physical_address >= s->scratchpad_base &&
        physical_address - s->scratchpad_base < s->scratchpad_size;

    if (!mittens_sync_memory_timing_enabled() ||
        (!scratchpad && !s->memory_timing)) {
        return;
    }
    if (!scratchpad && s->memory_init_active) {
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

enum {
    MITTENS_DMA_STATUS_READY = 1U << 0,
    MITTENS_DMA_STATUS_COMPLETE = 1U << 1,
    MITTENS_DMA_STATUS_ERROR = 1U << 2,
    MITTENS_DMA_STATUS_BUSY = 1U << 3,
};

static void mittens_sync_yield_scratchpad_dma(
    MittensSyncDeviceState *s,
    uint32_t reason)
{
    mittens_sync_publish_event(
        reason,
        MITTENS_SYNC_EVENT_FLAG_NONE,
        s->dma_direction,
        s->dma_source,
        s->dma_token_id,
        s->dma_execution_id,
        UINT32_MAX,
        UINT32_MAX,
        0,
        s->dma_destination,
        s->dma_byte_count,
        MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD,
        mittens_sync_current_executed(),
        true);
}

static bool mittens_sync_dma_addresses_valid(
    MittensSyncDeviceState *s)
{
    uint64_t scratchpad_end = s->scratchpad_base + s->scratchpad_size;
    bool source_scratchpad =
        s->dma_source >= s->scratchpad_base &&
        s->dma_source < scratchpad_end;
    bool destination_scratchpad =
        s->dma_destination >= s->scratchpad_base &&
        s->dma_destination < scratchpad_end;
    uint64_t scratchpad_address = source_scratchpad
        ? s->dma_source : s->dma_destination;

    return s->dma_byte_count != 0 &&
           source_scratchpad != destination_scratchpad &&
           scratchpad_address <= scratchpad_end &&
           s->dma_byte_count <= scratchpad_end - scratchpad_address;
}

static void mittens_sync_dma_job_free(gpointer opaque)
{
    MittensScratchpadDMAJob *job = opaque;

    if (job != NULL) {
        g_clear_pointer(&job->bytes, g_byte_array_unref);
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

static void mittens_sync_dma_command(
    MittensSyncDeviceState *s,
    uint32_t command)
{
    MemTxResult result;

    if (command == 1) {
        MittensScratchpadDMAJob *job;

        if (s->dma_jobs == NULL) {
            s->dma_jobs = g_ptr_array_new_with_free_func(
                mittens_sync_dma_job_free);
        }
        if (s->dma_jobs->len >= 8 ||
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
        job->destination = s->dma_destination;
        job->bytes = g_byte_array_sized_new(s->dma_byte_count);
        g_byte_array_set_size(job->bytes, s->dma_byte_count);
        result = address_space_read(
            &address_space_memory,
            s->dma_source,
            MEMTXATTRS_UNSPECIFIED,
            job->bytes->data,
            s->dma_byte_count);
        if (result != MEMTX_OK) {
            mittens_sync_dma_job_free(job);
            s->dma_error = 2;
            s->dma_status |= MITTENS_DMA_STATUS_ERROR;
            return;
        }
        g_ptr_array_add(s->dma_jobs, job);
        s->dma_status = MITTENS_DMA_STATUS_READY |
                        MITTENS_DMA_STATUS_BUSY;
        mittens_sync_yield_scratchpad_dma(
            s, MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT);
        return;
    }
    if (command == 2) {
        guint job_index = 0;
        MittensScratchpadDMAJob *job = mittens_sync_find_dma_job(
            s, s->dma_execution_id, s->dma_token_id, &job_index);
        if (job == NULL) {
            s->dma_error = 3;
            s->dma_status |= MITTENS_DMA_STATUS_ERROR;
            return;
        }
        mittens_sync_yield_scratchpad_dma(
            s, MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT);
        result = address_space_write(
            &address_space_memory,
            job->destination,
            MEMTXATTRS_UNSPECIFIED,
            job->bytes->data,
            job->bytes->len);
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
    if (bridge_stat.st_size != sizeof(MittensSyncBridge)) {
        error_setg(
            errp,
            "mittens-sync bridge has invalid size: %jd bytes",
            (intmax_t)bridge_stat.st_size);
        return;
    }

    s->bridge = mmap(
        NULL,
        sizeof(MittensSyncBridge),
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
        munmap(s->bridge, sizeof(MittensSyncBridge));
        s->bridge = NULL;
        error_setg(errp, "mittens-sync bridge header is incompatible");
        return;
    }
    s->memory_init_active =
        s->memory_timing && s->memory_init_batching;
}

static void mittens_sync_unrealize(DeviceState *device)
{
    MittensSyncDeviceState *s = MITTENS_SYNC(device);

    g_clear_pointer(&s->dma_jobs, g_ptr_array_unref);
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
        munmap(s->bridge, sizeof(MittensSyncBridge));
        s->bridge = NULL;
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
    DEFINE_PROP_BOOL(
        "memory-timing", MittensSyncDeviceState, memory_timing, false),
    DEFINE_PROP_BOOL(
        "memory-init-batching",
        MittensSyncDeviceState,
        memory_init_batching,
        false),
    DEFINE_PROP_BOOL(
        "scratchpad-enabled",
        MittensSyncDeviceState,
        scratchpad_enabled,
        false),
    DEFINE_PROP_UINT64(
        "scratchpad-base",
        MittensSyncDeviceState,
        scratchpad_base,
        UINT64_C(0x90000000)),
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
