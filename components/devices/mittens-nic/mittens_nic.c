/*
 * Mittens shared-memory mesh NIC
 *
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/misc/mittens_nic.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"

#include "mittens/bridge.h"

#define MITTENS_NIC_MMIO_SIZE 0x1000

#define MITTENS_NIC_STATUS 0x00
#define MITTENS_NIC_TX_DESTINATION 0x04
#define MITTENS_NIC_TX_DATA 0x08
#define MITTENS_NIC_RX_DATA 0x0c

#define MITTENS_NIC_STATUS_TX_READY (1U << 0)
#define MITTENS_NIC_STATUS_RX_VALID (1U << 1)

typedef struct MittensNICState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    int32_t bridge_fd;
    uint32_t transmit_destination;
    MittensBridgeShared *bridge;
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

static uint64_t mittens_nic_read(void *opaque, hwaddr offset,
                                 unsigned int size)
{
    MittensNICState *s = opaque;
    uint32_t status = 0;
    uint32_t payload = 0;

    if (size != sizeof(uint32_t) || (offset & 3U) != 0) {
        mittens_nic_set_error(s, MITTENS_BRIDGE_ERROR_BAD_MMIO,
                              "unaligned or non-32-bit read");
        return 0;
    }

    switch (offset) {
    case MITTENS_NIC_STATUS:
        if (s->bridge == NULL) {
            return 0;
        }
        if (mittens_bridge_tx_ready(s->bridge)) {
            status |= MITTENS_NIC_STATUS_TX_READY;
        }
        if (mittens_bridge_rx_valid(s->bridge)) {
            status |= MITTENS_NIC_STATUS_RX_VALID;
        }
        return status;

    case MITTENS_NIC_RX_DATA:
        if (s->bridge == NULL ||
            !mittens_bridge_rx_pop(s->bridge, &payload)) {
            mittens_nic_set_error(s, MITTENS_BRIDGE_ERROR_RX_EMPTY,
                                  "RX_DATA read while RX_VALID is clear");
            return 0;
        }
        return payload;

    case MITTENS_NIC_TX_DESTINATION:
    case MITTENS_NIC_TX_DATA:
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
        }
        return;

    case MITTENS_NIC_STATUS:
    case MITTENS_NIC_RX_DATA:
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
