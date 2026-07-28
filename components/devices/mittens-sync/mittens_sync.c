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
#include "qapi/error.h"
#include "qemu/futex.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/cpus.h"

#include "mittens/SyncTileBridge.h"

typedef struct MittensSyncDeviceState {
    DeviceState parent_obj;
    int32_t bridge_fd;
    MittensSyncBridge *bridge;
    uint64_t accounted_executed;
    int64_t accounted_remaining;
    bool terminal_event_published;
} MittensSyncDeviceState;

DECLARE_INSTANCE_CHECKER(
    MittensSyncDeviceState, MITTENS_SYNC, TYPE_MITTENS_SYNC)

static MittensSyncDeviceState *mittens_sync_instance;

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

    bridge->instructions_executed = instructions_executed;
    bridge->stop_reason = reason;
    bridge->event_flags = flags;
    bridge->analog_array_id = array_id;
    bridge->analog_sequence = analog_sequence;
    bridge->task_id = task_id;
    bridge->execution_id = execution_id;
    bridge->rx_dma_source = rx_dma_source;
    bridge->rx_dma_route_id = rx_dma_route_id;
    bridge->rx_dma_word_count = rx_dma_word_count;
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
        mittens_sync_current_executed(),
        true);
}

void mittens_sync_yield_receive_dma(
    uint32_t source,
    uint32_t route_id,
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
        mittens_sync_current_executed(),
        true);
}

static void mittens_sync_realize(DeviceState *device, Error **errp)
{
    MittensSyncDeviceState *s = MITTENS_SYNC(device);
    struct stat bridge_stat;

    if (mittens_sync_instance != NULL) {
        error_setg(errp, "only one mittens-sync device is supported");
        return;
    }
    mittens_sync_instance = s;

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
    }
}

static void mittens_sync_unrealize(DeviceState *device)
{
    MittensSyncDeviceState *s = MITTENS_SYNC(device);

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
