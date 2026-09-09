/*
 * Mittens shared-memory analog command transport
 *
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/misc/mittens_analog.h"
#include "hw/misc/mittens_sync.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"

#include "mittens/AnalogTileBridge.h"

typedef struct MittensAnalogState {
    DeviceState parent_obj;
    int32_t bridge_fd;
    size_t mapping_size;
    MittensAnalogBridgeHeader *bridge;
} MittensAnalogState;

DECLARE_INSTANCE_CHECKER(
    MittensAnalogState, MITTENS_ANALOG, TYPE_MITTENS_ANALOG)

static MittensAnalogState *mittens_analog_instance;

static uint32_t mittens_analog_primary_array(
    const MittensAnalogCommand *command)
{
    switch (command->operation) {
    case MITTENS_ANALOG_OPERATION_SET_MATRIX:
        if ((command->reserved &
             MITTENS_ANALOG_COMMAND_FLAG_COMPACT_SET_MATRIX) != 0) {
            return mittens_analog_set_matrix_array_id(command);
        }
        return (uint32_t)command->operand1;
    case MITTENS_ANALOG_OPERATION_LOAD_VECTOR:
    case MITTENS_ANALOG_OPERATION_STORE_VECTOR:
        return (uint32_t)command->operand1;
    case MITTENS_ANALOG_OPERATION_COMPUTE:
    case MITTENS_ANALOG_OPERATION_MOVE_VECTOR:
        return (uint32_t)command->operand0;
    default:
        return UINT32_MAX;
    }
}

static void mittens_analog_reclaim_slot(
    CPUState *cpu,
    uint32_t array_id,
    MittensAnalogBridgeSlot *slot)
{
    uint32_t state = mittens_analog_load_acquire(&slot->state);
    if (state == MITTENS_ANALOG_SLOT_ACCEPTED ||
        state == MITTENS_ANALOG_SLOT_SUBMITTED) {
        (void)cpu;
        mittens_sync_yield_analog(
            MITTENS_SYNC_STOP_ANALOG_WAIT,
            array_id,
            slot->sequence,
            true);
        state = mittens_analog_load_acquire(&slot->state);
    }
    if (state == MITTENS_ANALOG_SLOT_COMPLETED) {
        if (slot->status != MITTENS_ANALOG_STATUS_SUCCESS) {
            qemu_log_mask(
                LOG_GUEST_ERROR,
                "mittens-analog: asynchronous command %" PRIu64
                " completed with status %" PRIu64 "\n",
                slot->sequence,
                slot->status);
        }
        mittens_analog_store_release(
            &slot->state, MITTENS_ANALOG_SLOT_FREE);
    }
}

bool mittens_analog_available(void)
{
    return mittens_analog_instance != NULL &&
           mittens_analog_instance->bridge != NULL;
}

uint32_t mittens_analog_array_count(void)
{
    return mittens_analog_available() ?
        mittens_analog_instance->bridge->array_count : 0;
}

uint32_t mittens_analog_array_rows(void)
{
    return mittens_analog_available() ?
        mittens_analog_instance->bridge->array_rows : 0;
}

uint32_t mittens_analog_array_columns(void)
{
    return mittens_analog_available() ?
        mittens_analog_instance->bridge->array_columns : 0;
}

uint64_t mittens_analog_submit(
    CPUState *cpu,
    const MittensAnalogCommand *command,
    const uint32_t *input_words,
    uint32_t input_word_count,
    bool wait_for_completion,
    uint32_t *output_words,
    uint32_t output_capacity,
    uint32_t *output_word_count)
{
    MittensAnalogState *s = mittens_analog_instance;
    MittensAnalogBridgeChannel *channel;
    MittensAnalogBridgeSlot *slot;
    uint32_t array_id;
    uint32_t sequence;
    uint32_t state;

    if (output_word_count != NULL) {
        *output_word_count = 0;
    }
    if (s == NULL || s->bridge == NULL) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "mittens-analog: instruction executed without an SST analog bridge\n");
        return MITTENS_ANALOG_STATUS_BACKEND_ERROR;
    }

    array_id = mittens_analog_primary_array(command);
    if (array_id >= s->bridge->array_count) {
        return MITTENS_ANALOG_STATUS_INVALID_ARRAY;
    }
    if (input_word_count > s->bridge->words_per_slot) {
        return MITTENS_ANALOG_STATUS_INVALID_PAYLOAD;
    }

    channel = mittens_analog_channel(s->bridge, array_id);
    sequence =
        mittens_analog_load_relaxed(&channel->write_index);
    slot = mittens_analog_slot(s->bridge, array_id, sequence);

    state = mittens_analog_load_acquire(&slot->state);
    if (state != MITTENS_ANALOG_SLOT_FREE) {
        mittens_analog_reclaim_slot(cpu, array_id, slot);
    }
    if (mittens_analog_load_acquire(&slot->state) !=
        MITTENS_ANALOG_SLOT_FREE) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "mittens-analog: failed to reclaim command slot\n");
        return MITTENS_ANALOG_STATUS_BACKEND_ERROR;
    }

    slot->sequence = sequence;
    slot->command = *command;
    slot->input_word_count = input_word_count;
    slot->output_word_count = 0;
    slot->status = MITTENS_ANALOG_STATUS_SUCCESS;
    if (input_word_count != 0) {
        memcpy(
            mittens_analog_slot_words(slot),
            input_words,
            (size_t)input_word_count * sizeof(*input_words));
    }

    mittens_analog_store_release(
        &slot->state, MITTENS_ANALOG_SLOT_SUBMITTED);
    mittens_analog_store_release(
        &channel->write_index, sequence + UINT32_C(1));

    (void)cpu;
    mittens_sync_yield_analog(
        MITTENS_SYNC_STOP_ANALOG_SUBMIT,
        array_id,
        sequence,
        wait_for_completion);

    if (!wait_for_completion) {
        if (mittens_analog_load_acquire(&slot->state) ==
            MITTENS_ANALOG_SLOT_COMPLETED) {
            return slot->status;
        }
        return MITTENS_ANALOG_STATUS_SUCCESS;
    }

    if (mittens_analog_load_acquire(&slot->state) !=
        MITTENS_ANALOG_SLOT_COMPLETED) {
        return MITTENS_ANALOG_STATUS_BACKEND_ERROR;
    }

    if (slot->output_word_count > output_capacity) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "mittens-analog: response contains %u words but capacity is %u\n",
            slot->output_word_count,
            output_capacity);
        slot->status = MITTENS_ANALOG_STATUS_INVALID_PAYLOAD;
    } else if (slot->output_word_count != 0 &&
               output_words != NULL) {
        memcpy(
            output_words,
            mittens_analog_slot_words_const(slot),
            (size_t)slot->output_word_count *
                sizeof(*output_words));
    }
    if (output_word_count != NULL) {
        *output_word_count = slot->output_word_count;
    }

    {
        const uint64_t status = slot->status;
        mittens_analog_store_release(
            &slot->state, MITTENS_ANALOG_SLOT_FREE);
        return status;
    }
}

static void mittens_analog_realize(DeviceState *device, Error **errp)
{
    MittensAnalogState *s = MITTENS_ANALOG(device);
    struct stat bridge_stat;
    size_t expected_size;

    if (mittens_analog_instance != NULL) {
        error_setg(
            errp, "only one mittens-analog device is supported");
        return;
    }
    mittens_analog_instance = s;

    if (s->bridge_fd < 0) {
        return;
    }
    if (!mittens_sync_available()) {
        error_setg(
            errp,
            "mittens-analog requires the fd 41 synchronization bridge");
        return;
    }
    if (fstat(s->bridge_fd, &bridge_stat) < 0) {
        error_setg_errno(
            errp, errno, "mittens-analog cannot stat bridge fd");
        return;
    }
    if (bridge_stat.st_size <
        (off_t)sizeof(MittensAnalogBridgeHeader)) {
        error_setg(
            errp,
            "mittens-analog bridge is too small: %jd bytes",
            (intmax_t)bridge_stat.st_size);
        return;
    }

    s->mapping_size = bridge_stat.st_size;
    s->bridge = mmap(
        NULL,
        s->mapping_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        s->bridge_fd,
        0);
    if (s->bridge == MAP_FAILED) {
        s->bridge = NULL;
        error_setg_errno(
            errp, errno, "mittens-analog cannot map bridge fd");
        return;
    }

    close(s->bridge_fd);
    s->bridge_fd = -1;

    expected_size = mittens_analog_bridge_size(
        s->bridge->array_count,
        s->bridge->array_rows,
        s->bridge->array_columns);
    if (s->bridge->magic != MITTENS_ANALOG_BRIDGE_MAGIC ||
        s->bridge->version != MITTENS_ANALOG_BRIDGE_VERSION ||
        s->bridge->structure_size != s->mapping_size ||
        s->bridge->structure_size != expected_size ||
        s->bridge->queue_capacity !=
            MITTENS_ANALOG_QUEUE_CAPACITY ||
        s->bridge->words_per_slot !=
            mittens_analog_words_per_slot(
                s->bridge->array_rows,
                s->bridge->array_columns) ||
        s->bridge->channel_stride !=
            mittens_analog_channel_stride(
                s->bridge->array_rows,
                s->bridge->array_columns) ||
        s->bridge->slot_stride !=
            mittens_analog_slot_stride(
                s->bridge->array_rows,
                s->bridge->array_columns) ||
        s->bridge->link_width_bits !=
            MITTENS_ANALOG_LINK_WIDTH_BITS) {
        munmap(s->bridge, s->mapping_size);
        s->bridge = NULL;
        error_setg(
            errp, "mittens-analog bridge header is incompatible");
    }
}

static void mittens_analog_unrealize(DeviceState *device)
{
    MittensAnalogState *s = MITTENS_ANALOG(device);

    if (s->bridge != NULL) {
        munmap(s->bridge, s->mapping_size);
        s->bridge = NULL;
    }
    if (s->bridge_fd >= 0) {
        close(s->bridge_fd);
        s->bridge_fd = -1;
    }
    s->mapping_size = 0;
    if (mittens_analog_instance == s) {
        mittens_analog_instance = NULL;
    }
}

static Property mittens_analog_properties[] = {
    DEFINE_PROP_INT32(
        "bridge-fd", MittensAnalogState, bridge_fd, -1),
    DEFINE_PROP_END_OF_LIST(),
};

static void mittens_analog_class_init(
    ObjectClass *object_class,
    void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(object_class);

    device_class_set_props(
        device_class, mittens_analog_properties);
    device_class->realize = mittens_analog_realize;
    device_class->unrealize = mittens_analog_unrealize;
}

static const TypeInfo mittens_analog_info = {
    .name = TYPE_MITTENS_ANALOG,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(MittensAnalogState),
    .class_init = mittens_analog_class_init,
};

static void mittens_analog_register_types(void)
{
    type_register_static(&mittens_analog_info);
}

type_init(mittens_analog_register_types)

DeviceState *mittens_analog_create(void)
{
    DeviceState *device = qdev_new(TYPE_MITTENS_ANALOG);

    qdev_realize_and_unref(device, NULL, &error_fatal);
    return device;
}
