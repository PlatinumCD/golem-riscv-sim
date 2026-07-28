/*
 * Mittens shared-memory mesh NIC
 *
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/misc/mittens_nic.h"
#include "hw/misc/mittens_sync.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/dma.h"

#include "mittens/NICTileBridge.h"

#define MITTENS_NIC_MMIO_SIZE 0x1000

#define MITTENS_NIC_STATUS 0x00
#define MITTENS_NIC_TX_DESTINATION 0x04
#define MITTENS_NIC_TX_DATA 0x08
#define MITTENS_NIC_RX_DATA 0x0c
#define MITTENS_NIC_RX_SOURCE 0x10
#define MITTENS_NIC_TX_BURST_ADDRESS_LOW 0x14
#define MITTENS_NIC_TX_BURST_ADDRESS_HIGH 0x18
#define MITTENS_NIC_TX_BURST_WORD_COUNT 0x1c
#define MITTENS_NIC_TX_BURST_SUBMIT 0x20
#define MITTENS_NIC_RX_WAIT 0x24
#define MITTENS_NIC_TRACE_TASK_ID 0x28
#define MITTENS_NIC_TRACE_EXECUTION_ID_LOW 0x2c
#define MITTENS_NIC_TRACE_EXECUTION_ID_HIGH 0x30
#define MITTENS_NIC_TRACE_EVENT 0x34
#define MITTENS_NIC_RX_DMA_SOURCE 0x38
#define MITTENS_NIC_RX_DMA_ROUTE_ID 0x3c
#define MITTENS_NIC_RX_DMA_ADDRESS_LOW 0x40
#define MITTENS_NIC_RX_DMA_ADDRESS_HIGH 0x44
#define MITTENS_NIC_RX_DMA_WORD_COUNT 0x48
#define MITTENS_NIC_RX_DMA_SUBMIT 0x4c
#define MITTENS_NIC_RX_DMA_STATUS 0x50
#define MITTENS_NIC_RX_DMA_COMPLETION_SOURCE 0x54
#define MITTENS_NIC_RX_DMA_COMPLETION_ROUTE_ID 0x58
#define MITTENS_NIC_RX_DMA_COMPLETION_ACK 0x5c

#define MITTENS_NIC_STATUS_TX_READY (1U << 0)
#define MITTENS_NIC_STATUS_RX_VALID (1U << 1)
#define MITTENS_NIC_STATUS_TX_BURST_READY (1U << 2)

#define MITTENS_NIC_RX_DMA_STATUS_SUBMIT_READY (1U << 0)
#define MITTENS_NIC_RX_DMA_STATUS_COMPLETION_VALID (1U << 1)
#define MITTENS_NIC_RX_DMA_CAPACITY 64

#define MITTENS_NIC_TRACE_EVENT_START 1
#define MITTENS_NIC_TRACE_EVENT_FINISH 2

typedef struct MittensNICReceiveDMA {
    bool active;
    uint32_t source;
    uint32_t route_id;
    uint64_t address;
    uint32_t word_count;
    uint32_t received_words;
} MittensNICReceiveDMA;

typedef struct MittensNICReceiveDMACompletion {
    uint32_t source;
    uint32_t route_id;
} MittensNICReceiveDMACompletion;

typedef struct MittensNICState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    int32_t bridge_fd;
    uint32_t transmit_destination;
    uint64_t transmit_burst_address;
    uint32_t transmit_burst_word_count;
    uint32_t trace_task_id;
    uint64_t trace_execution_id;
    MittensBridgeShared *bridge;

    uint32_t receive_dma_source;
    uint32_t receive_dma_route_id;
    uint64_t receive_dma_address;
    uint32_t receive_dma_word_count;
    MittensNICReceiveDMA receive_dma[MITTENS_NIC_RX_DMA_CAPACITY];
    MittensNICReceiveDMACompletion
        receive_dma_completions[MITTENS_NIC_RX_DMA_CAPACITY];
    uint32_t receive_dma_completion_read;
    uint32_t receive_dma_completion_write;
} MittensNICState;

DECLARE_INSTANCE_CHECKER(MittensNICState, MITTENS_NIC, TYPE_MITTENS_NIC)

static void mittens_nic_set_error(MittensNICState *s,
                                  enum MittensBridgeError error,
                                  const char *message)
{
    uint32_t expected = MITTENS_BRIDGE_ERROR_NONE;

    if (s->bridge != NULL) {
        __atomic_compare_exchange_n(&s->bridge->protocol_error,
                                    &expected,
                                    (uint32_t)error,
                                    false,
                                    __ATOMIC_RELEASE,
                                    __ATOMIC_RELAXED);
    }
    qemu_log_mask(LOG_GUEST_ERROR, "mittens-nic: %s\n", message);
}

static MittensNICReceiveDMA *mittens_nic_find_receive_dma(
    MittensNICState *s,
    uint32_t source)
{
    uint32_t index;

    for (index = 0; index < MITTENS_NIC_RX_DMA_CAPACITY; ++index) {
        if (s->receive_dma[index].active &&
            s->receive_dma[index].source == source) {
            return &s->receive_dma[index];
        }
    }
    return NULL;
}

static MittensNICReceiveDMA *mittens_nic_find_free_receive_dma(
    MittensNICState *s)
{
    uint32_t index;

    for (index = 0; index < MITTENS_NIC_RX_DMA_CAPACITY; ++index) {
        if (!s->receive_dma[index].active) {
            return &s->receive_dma[index];
        }
    }
    return NULL;
}

static bool mittens_nic_receive_dma_completion_valid(
    const MittensNICState *s)
{
    return s->receive_dma_completion_read !=
           s->receive_dma_completion_write;
}

static bool mittens_nic_receive_dma_completion_has_space(
    const MittensNICState *s)
{
    return (uint32_t)(
               s->receive_dma_completion_write -
               s->receive_dma_completion_read) <
           MITTENS_NIC_RX_DMA_CAPACITY;
}

static void mittens_nic_complete_receive_dma(
    MittensNICState *s,
    MittensNICReceiveDMA *descriptor)
{
    MittensNICReceiveDMACompletion *completion;

    if (!mittens_nic_receive_dma_completion_has_space(s)) {
        mittens_nic_set_error(
            s,
            MITTENS_BRIDGE_ERROR_DMA,
            "RX DMA completion queue is full");
        return;
    }
    completion = &s->receive_dma_completions[
        s->receive_dma_completion_write %
        MITTENS_NIC_RX_DMA_CAPACITY];
    completion->source = descriptor->source;
    completion->route_id = descriptor->route_id;
    ++s->receive_dma_completion_write;
    descriptor->active = false;
}

static bool mittens_nic_receive_burst_visible(
    MittensNICState *s)
{
    MittensBridgeRxPacket packet;

    if (s->bridge == NULL ||
        !mittens_bridge_rx_burst_peek_word(
            s->bridge, &packet)) {
        return false;
    }
    if (!mittens_bridge_rx_dma_timing_enabled(s->bridge) ||
        mittens_nic_find_receive_dma(
            s, packet.source) == NULL) {
        return true;
    }
    return mittens_bridge_rx_dma_authorization_available(
        s->bridge);
}

static void mittens_nic_service_receive_dma(MittensNICState *s)
{
    const MittensBridgeRxBurst *burst;
    MittensNICReceiveDMA *descriptor;
    uint32_t remaining;
    uint32_t burst_word_count;
    uint64_t byte_offset;
    uint64_t byte_count;

    if (s->bridge == NULL) {
        return;
    }

    while (mittens_bridge_rx_burst_peek(s->bridge, &burst)) {
        descriptor =
            mittens_nic_find_receive_dma(s, burst->source);
        if (descriptor == NULL) {
            return;
        }
        if (mittens_bridge_rx_dma_timing_enabled(s->bridge) &&
            !mittens_bridge_rx_dma_authorization_available(
                s->bridge)) {
            return;
        }
        burst_word_count = burst->word_count;
        remaining =
            descriptor->word_count - descriptor->received_words;
        if (burst_word_count > remaining) {
            mittens_nic_set_error(
                s,
                MITTENS_BRIDGE_ERROR_DMA,
                "RX DMA burst exceeds the registered route size");
            return;
        }
        byte_offset =
            (uint64_t)descriptor->received_words * sizeof(uint32_t);
        byte_count =
            (uint64_t)burst_word_count * sizeof(uint32_t);
        if (dma_memory_write(
                &address_space_memory,
                descriptor->address + byte_offset,
                burst->words,
                (dma_addr_t)byte_count,
                MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            mittens_nic_set_error(
                s,
                MITTENS_BRIDGE_ERROR_DMA,
                "cannot write an RX DMA burst to guest memory");
            return;
        }
        if (!mittens_bridge_rx_burst_consume(s->bridge)) {
            mittens_nic_set_error(
                s,
                MITTENS_BRIDGE_ERROR_DMA,
                "RX DMA could not consume the copied burst");
            return;
        }
        if (mittens_bridge_rx_dma_timing_enabled(s->bridge) &&
            !mittens_bridge_rx_dma_consume_authorization(
                s->bridge)) {
            mittens_nic_set_error(
                s,
                MITTENS_BRIDGE_ERROR_DMA,
                "RX DMA copied a burst without SST authorization");
            return;
        }
        descriptor->received_words += burst_word_count;
        if (descriptor->received_words == descriptor->word_count) {
            mittens_nic_complete_receive_dma(s, descriptor);
        }
    }
}

static bool mittens_nic_receive_dma_submit_ready(MittensNICState *s)
{
    uint32_t active = 0;
    uint32_t index;
    const uint32_t completions =
        s->receive_dma_completion_write -
        s->receive_dma_completion_read;

    for (index = 0; index < MITTENS_NIC_RX_DMA_CAPACITY; ++index) {
        if (s->receive_dma[index].active) {
            ++active;
        }
    }
    return active + completions < MITTENS_NIC_RX_DMA_CAPACITY &&
           mittens_nic_find_free_receive_dma(s) != NULL &&
           mittens_nic_receive_dma_completion_has_space(s);
}

static bool mittens_nic_receive_valid(MittensNICState *s)
{
    return s->bridge != NULL &&
           (mittens_bridge_rx_valid(s->bridge) ||
            mittens_nic_receive_burst_visible(s));
}

static uint64_t mittens_nic_read(void *opaque, hwaddr offset,
                                 unsigned int size)
{
    MittensNICState *s = opaque;
    uint32_t status = 0;
    MittensBridgeRxPacket packet = {0};

    if (size != sizeof(uint32_t) || (offset & 3U) != 0) {
        mittens_nic_set_error(s, MITTENS_BRIDGE_ERROR_BAD_MMIO,
                              "unaligned or non-32-bit read");
        return 0;
    }

    mittens_nic_service_receive_dma(s);

    switch (offset) {
    case MITTENS_NIC_STATUS:
        if (s->bridge == NULL) {
            return 0;
        }
        if (mittens_bridge_tx_ready(s->bridge)) {
            status |= MITTENS_NIC_STATUS_TX_READY;
        }
        if (mittens_nic_receive_valid(s)) {
            status |= MITTENS_NIC_STATUS_RX_VALID;
        }
        if (mittens_bridge_tx_burst_ready(s->bridge)) {
            status |= MITTENS_NIC_STATUS_TX_BURST_READY;
        }
        return status;

    case MITTENS_NIC_RX_DATA:
        if (s->bridge == NULL ||
            (!mittens_bridge_rx_burst_pop_word(s->bridge, &packet) &&
             !mittens_bridge_rx_pop(s->bridge, &packet))) {
            mittens_nic_set_error(s, MITTENS_BRIDGE_ERROR_RX_EMPTY,
                                  "RX_DATA read while RX_VALID is clear");
            return 0;
        }
        return packet.payload;

    case MITTENS_NIC_RX_SOURCE:
        if (s->bridge == NULL ||
            (!mittens_bridge_rx_burst_peek_word(s->bridge, &packet) &&
             !mittens_bridge_rx_peek(s->bridge, &packet))) {
            mittens_nic_set_error(s, MITTENS_BRIDGE_ERROR_RX_EMPTY,
                                  "RX_SOURCE read while RX_VALID is clear");
            return 0;
        }
        return packet.source;

    case MITTENS_NIC_RX_DMA_STATUS:
        if (mittens_nic_receive_dma_submit_ready(s)) {
            status |= MITTENS_NIC_RX_DMA_STATUS_SUBMIT_READY;
        }
        if (mittens_nic_receive_dma_completion_valid(s)) {
            status |= MITTENS_NIC_RX_DMA_STATUS_COMPLETION_VALID;
        }
        return status;

    case MITTENS_NIC_RX_DMA_COMPLETION_SOURCE:
        if (!mittens_nic_receive_dma_completion_valid(s)) {
            mittens_nic_set_error(
                s,
                MITTENS_BRIDGE_ERROR_BAD_MMIO,
                "RX DMA completion source read while no completion exists");
            return 0;
        }
        return s->receive_dma_completions[
            s->receive_dma_completion_read %
            MITTENS_NIC_RX_DMA_CAPACITY].source;

    case MITTENS_NIC_RX_DMA_COMPLETION_ROUTE_ID:
        if (!mittens_nic_receive_dma_completion_valid(s)) {
            mittens_nic_set_error(
                s,
                MITTENS_BRIDGE_ERROR_BAD_MMIO,
                "RX DMA completion route read while no completion exists");
            return 0;
        }
        return s->receive_dma_completions[
            s->receive_dma_completion_read %
            MITTENS_NIC_RX_DMA_CAPACITY].route_id;

    case MITTENS_NIC_TX_DESTINATION:
    case MITTENS_NIC_TX_DATA:
    case MITTENS_NIC_TX_BURST_ADDRESS_LOW:
    case MITTENS_NIC_TX_BURST_ADDRESS_HIGH:
    case MITTENS_NIC_TX_BURST_WORD_COUNT:
    case MITTENS_NIC_TX_BURST_SUBMIT:
    case MITTENS_NIC_RX_WAIT:
    case MITTENS_NIC_TRACE_TASK_ID:
    case MITTENS_NIC_TRACE_EXECUTION_ID_LOW:
    case MITTENS_NIC_TRACE_EXECUTION_ID_HIGH:
    case MITTENS_NIC_TRACE_EVENT:
    case MITTENS_NIC_RX_DMA_SOURCE:
    case MITTENS_NIC_RX_DMA_ROUTE_ID:
    case MITTENS_NIC_RX_DMA_ADDRESS_LOW:
    case MITTENS_NIC_RX_DMA_ADDRESS_HIGH:
    case MITTENS_NIC_RX_DMA_WORD_COUNT:
    case MITTENS_NIC_RX_DMA_SUBMIT:
    case MITTENS_NIC_RX_DMA_COMPLETION_ACK:
        mittens_nic_set_error(s, MITTENS_BRIDGE_ERROR_BAD_MMIO,
                              "read from a write-only register");
        return 0;

    default:
        return 0;
    }
}

static void mittens_nic_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned int size)
{
    MittensNICState *s = opaque;
    MittensBridgePacket packet;
    uint32_t words[MITTENS_BRIDGE_BURST_WORD_CAPACITY];

    if (size != sizeof(uint32_t) || (offset & 3U) != 0) {
        mittens_nic_set_error(s, MITTENS_BRIDGE_ERROR_BAD_MMIO,
                              "unaligned or non-32-bit write");
        return;
    }

    switch (offset) {
    case MITTENS_NIC_TX_DESTINATION:
        s->transmit_destination = (uint32_t)value;
        return;

    case MITTENS_NIC_TX_DATA:
        packet.destination = s->transmit_destination;
        packet.payload = (uint32_t)value;
        if (s->bridge == NULL ||
            !mittens_bridge_tx_push(s->bridge, packet)) {
            mittens_nic_set_error(s, MITTENS_BRIDGE_ERROR_TX_FULL,
                                  "TX_DATA write while TX_READY is clear");
        } else if (mittens_sync_available() &&
                   !mittens_bridge_tx_ready(s->bridge)) {
            mittens_sync_yield_nic(
                MITTENS_SYNC_STOP_NIC_TRANSMIT);
        }
        return;

    case MITTENS_NIC_TX_BURST_ADDRESS_LOW:
        s->transmit_burst_address =
            (s->transmit_burst_address & UINT64_C(0xffffffff00000000)) |
            (uint32_t)value;
        return;

    case MITTENS_NIC_TX_BURST_ADDRESS_HIGH:
        s->transmit_burst_address =
            (s->transmit_burst_address & UINT64_C(0x00000000ffffffff)) |
            ((uint64_t)(uint32_t)value << 32);
        return;

    case MITTENS_NIC_TX_BURST_WORD_COUNT:
        s->transmit_burst_word_count = (uint32_t)value;
        return;

    case MITTENS_NIC_TX_BURST_SUBMIT:
        if (s->bridge == NULL ||
            s->transmit_burst_word_count == 0 ||
            s->transmit_burst_word_count >
                MITTENS_BRIDGE_BURST_WORD_CAPACITY ||
            !mittens_bridge_tx_burst_ready(s->bridge)) {
            mittens_nic_set_error(
                s,
                MITTENS_BRIDGE_ERROR_BAD_BURST,
                "invalid TX burst submission");
            return;
        }
        if (dma_memory_read(
                &address_space_memory,
                s->transmit_burst_address,
                words,
                (dma_addr_t)s->transmit_burst_word_count *
                    sizeof(uint32_t),
                MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            mittens_nic_set_error(
                s,
                MITTENS_BRIDGE_ERROR_DMA,
                "cannot snapshot TX burst guest memory");
            return;
        }
        if (!mittens_bridge_tx_burst_push(
                s->bridge,
                s->transmit_destination,
                words,
                s->transmit_burst_word_count)) {
            mittens_nic_set_error(
                s,
                MITTENS_BRIDGE_ERROR_TX_FULL,
                "TX burst ring became full during submission");
            return;
        }
        if (mittens_sync_available() &&
            !mittens_bridge_tx_burst_ready(s->bridge)) {
            mittens_sync_yield_nic(
                MITTENS_SYNC_STOP_NIC_TRANSMIT);
        }
        return;

    case MITTENS_NIC_RX_WAIT:
        /*
         * Recheck the shared receive rings in the MMIO operation that
         * publishes the wait. A packet may arrive after guest software
         * observed RX_VALID clear but before this write reaches QEMU.
         */
        mittens_nic_service_receive_dma(s);
        if (mittens_nic_receive_valid(s) ||
            mittens_nic_receive_dma_completion_valid(s) ||
            !mittens_sync_available()) {
            return;
        }
        mittens_sync_yield_nic(
            MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT);
        mittens_nic_service_receive_dma(s);
        return;

    case MITTENS_NIC_TRACE_TASK_ID:
        s->trace_task_id = (uint32_t)value;
        return;

    case MITTENS_NIC_TRACE_EXECUTION_ID_LOW:
        s->trace_execution_id =
            (s->trace_execution_id & UINT64_C(0xffffffff00000000)) |
            (uint32_t)value;
        return;

    case MITTENS_NIC_TRACE_EXECUTION_ID_HIGH:
        s->trace_execution_id =
            (s->trace_execution_id & UINT64_C(0x00000000ffffffff)) |
            ((uint64_t)(uint32_t)value << 32);
        return;

    case MITTENS_NIC_TRACE_EVENT:
        if (value == MITTENS_NIC_TRACE_EVENT_START) {
            mittens_sync_yield_task(
                MITTENS_SYNC_STOP_TASK_START,
                s->trace_task_id,
                s->trace_execution_id);
            return;
        }
        if (value == MITTENS_NIC_TRACE_EVENT_FINISH) {
            mittens_sync_yield_task(
                MITTENS_SYNC_STOP_TASK_FINISH,
                s->trace_task_id,
                s->trace_execution_id);
            return;
        }
        mittens_nic_set_error(
            s,
            MITTENS_BRIDGE_ERROR_BAD_MMIO,
            "TRACE_EVENT write used an invalid event ID");
        return;

    case MITTENS_NIC_RX_DMA_SOURCE:
        s->receive_dma_source = (uint32_t)value;
        return;

    case MITTENS_NIC_RX_DMA_ROUTE_ID:
        s->receive_dma_route_id = (uint32_t)value;
        return;

    case MITTENS_NIC_RX_DMA_ADDRESS_LOW:
        s->receive_dma_address =
            (s->receive_dma_address & UINT64_C(0xffffffff00000000)) |
            (uint32_t)value;
        return;

    case MITTENS_NIC_RX_DMA_ADDRESS_HIGH:
        s->receive_dma_address =
            (s->receive_dma_address & UINT64_C(0x00000000ffffffff)) |
            ((uint64_t)(uint32_t)value << 32);
        return;

    case MITTENS_NIC_RX_DMA_WORD_COUNT:
        s->receive_dma_word_count = (uint32_t)value;
        return;

    case MITTENS_NIC_RX_DMA_SUBMIT: {
        MittensNICReceiveDMA *descriptor;
        const uint64_t byte_count =
            (uint64_t)s->receive_dma_word_count * sizeof(uint32_t);

        if (s->bridge == NULL ||
            s->receive_dma_word_count == 0 ||
            (s->receive_dma_address & (sizeof(uint32_t) - 1U)) != 0 ||
            s->receive_dma_address > UINT64_MAX - byte_count ||
            mittens_nic_find_receive_dma(
                s, s->receive_dma_source) != NULL ||
            !mittens_nic_receive_dma_submit_ready(s)) {
            mittens_nic_set_error(
                s,
                MITTENS_BRIDGE_ERROR_DMA,
                "invalid RX DMA submission");
            return;
        }
        descriptor = mittens_nic_find_free_receive_dma(s);
        descriptor->active = true;
        descriptor->source = s->receive_dma_source;
        descriptor->route_id = s->receive_dma_route_id;
        descriptor->address = s->receive_dma_address;
        descriptor->word_count = s->receive_dma_word_count;
        descriptor->received_words = 0;
        if (mittens_bridge_rx_dma_timing_enabled(s->bridge)) {
            mittens_sync_yield_receive_dma(
                descriptor->source,
                descriptor->route_id,
                descriptor->word_count);
        }
        mittens_nic_service_receive_dma(s);
        return;
    }

    case MITTENS_NIC_RX_DMA_COMPLETION_ACK:
        if (!mittens_nic_receive_dma_completion_valid(s)) {
            mittens_nic_set_error(
                s,
                MITTENS_BRIDGE_ERROR_BAD_MMIO,
                "RX DMA completion acknowledged while none exists");
            return;
        }
        ++s->receive_dma_completion_read;
        return;

    case MITTENS_NIC_STATUS:
    case MITTENS_NIC_RX_DATA:
    case MITTENS_NIC_RX_SOURCE:
    case MITTENS_NIC_RX_DMA_STATUS:
    case MITTENS_NIC_RX_DMA_COMPLETION_SOURCE:
    case MITTENS_NIC_RX_DMA_COMPLETION_ROUTE_ID:
        mittens_nic_set_error(s, MITTENS_BRIDGE_ERROR_BAD_MMIO,
                              "write to a read-only register");
        return;

    default:
        return;
    }
}

static const MemoryRegionOps mittens_nic_ops = {
    .read = mittens_nic_read,
    .write = mittens_nic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void mittens_nic_realize(DeviceState *device, Error **errp)
{
    MittensNICState *s = MITTENS_NIC(device);
    struct stat bridge_stat;

    if (s->bridge_fd < 0) {
        return;
    }

    if (fstat(s->bridge_fd, &bridge_stat) < 0) {
        error_setg_errno(errp, errno, "mittens-nic cannot stat bridge fd");
        return;
    }
    if (bridge_stat.st_size < sizeof(MittensBridgeShared)) {
        error_setg(errp,
                   "mittens-nic bridge is too small: %jd bytes",
                   (intmax_t)bridge_stat.st_size);
        return;
    }

    s->bridge = mmap(NULL,
                     sizeof(MittensBridgeShared),
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED,
                     s->bridge_fd,
                     0);
    if (s->bridge == MAP_FAILED) {
        s->bridge = NULL;
        error_setg_errno(errp, errno, "mittens-nic cannot map bridge fd");
        return;
    }

    close(s->bridge_fd);
    s->bridge_fd = -1;

    if (s->bridge->magic != MITTENS_BRIDGE_MAGIC ||
        s->bridge->version != MITTENS_BRIDGE_VERSION ||
        s->bridge->structure_size != sizeof(MittensBridgeShared) ||
        s->bridge->queue_capacity != MITTENS_BRIDGE_QUEUE_CAPACITY) {
        munmap(s->bridge, sizeof(MittensBridgeShared));
        s->bridge = NULL;
        error_setg(errp, "mittens-nic bridge header is incompatible");
    }
}

static void mittens_nic_unrealize(DeviceState *device)
{
    MittensNICState *s = MITTENS_NIC(device);

    if (s->bridge != NULL) {
        munmap(s->bridge, sizeof(MittensBridgeShared));
        s->bridge = NULL;
    }
}

static void mittens_nic_init(Object *object)
{
    MittensNICState *s = MITTENS_NIC(object);

    memory_region_init_io(&s->mmio,
                          object,
                          &mittens_nic_ops,
                          s,
                          TYPE_MITTENS_NIC,
                          MITTENS_NIC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(object), &s->mmio);
}

static Property mittens_nic_properties[] = {
    DEFINE_PROP_INT32("bridge-fd", MittensNICState, bridge_fd, -1),
    DEFINE_PROP_END_OF_LIST(),
};

static void mittens_nic_class_init(ObjectClass *object_class, void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(object_class);

    device_class_set_props(device_class, mittens_nic_properties);
    device_class->realize = mittens_nic_realize;
    device_class->unrealize = mittens_nic_unrealize;
}

static const TypeInfo mittens_nic_info = {
    .name = TYPE_MITTENS_NIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MittensNICState),
    .instance_init = mittens_nic_init,
    .class_init = mittens_nic_class_init,
};

static void mittens_nic_register_types(void)
{
    type_register_static(&mittens_nic_info);
}

type_init(mittens_nic_register_types)

DeviceState *mittens_nic_create(hwaddr address)
{
    DeviceState *device = qdev_new(TYPE_MITTENS_NIC);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(device), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(device), 0, address);
    return device;
}
