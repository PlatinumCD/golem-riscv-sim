#ifndef MITTENS_BRIDGE_H
#define MITTENS_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MITTENS_BRIDGE_MAGIC UINT32_C(0x4d495454)
#define MITTENS_BRIDGE_VERSION UINT32_C(1)
#define MITTENS_BRIDGE_QUEUE_CAPACITY UINT32_C(64)

enum MittensBridgeError {
    MITTENS_BRIDGE_ERROR_NONE = 0,
    MITTENS_BRIDGE_ERROR_TX_FULL = 1,
    MITTENS_BRIDGE_ERROR_RX_EMPTY = 2,
    MITTENS_BRIDGE_ERROR_BAD_MMIO = 3,
    MITTENS_BRIDGE_ERROR_BAD_DESTINATION = 4,
};

typedef struct MittensBridgePacket {
    uint32_t destination;
    uint32_t payload;
} MittensBridgePacket;

typedef struct __attribute__((aligned(64))) MittensBridgeTxRing {
    uint32_t write_index;
    uint8_t write_padding[60];
    uint32_t read_index;
    uint8_t read_padding[60];
    MittensBridgePacket packets[MITTENS_BRIDGE_QUEUE_CAPACITY];
} MittensBridgeTxRing;

typedef struct __attribute__((aligned(64))) MittensBridgeRxRing {
    uint32_t write_index;
    uint8_t write_padding[60];
    uint32_t read_index;
    uint8_t read_padding[60];
    uint32_t payloads[MITTENS_BRIDGE_QUEUE_CAPACITY];
} MittensBridgeRxRing;

typedef struct __attribute__((aligned(64))) MittensBridgeShared {
    uint32_t magic;
    uint32_t version;
    uint32_t structure_size;
    uint32_t queue_capacity;
    uint32_t tile_id;
    uint32_t protocol_error;
    uint8_t header_padding[40];
    MittensBridgeTxRing transmit;
    MittensBridgeRxRing receive;
} MittensBridgeShared;

static inline uint32_t mittens_bridge_load_acquire(const uint32_t* value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline uint32_t mittens_bridge_load_relaxed(const uint32_t* value)
{
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static inline void mittens_bridge_store_release(uint32_t* value, uint32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static inline void mittens_bridge_store_relaxed(uint32_t* value, uint32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELAXED);
}

static inline int mittens_bridge_tx_ready(const MittensBridgeShared* bridge)
{
    const uint32_t write_index =
        mittens_bridge_load_relaxed(&bridge->transmit.write_index);
    const uint32_t read_index =
        mittens_bridge_load_acquire(&bridge->transmit.read_index);
    return (uint32_t)(write_index - read_index) <
           MITTENS_BRIDGE_QUEUE_CAPACITY;
}

static inline int mittens_bridge_tx_push(
    MittensBridgeShared* bridge,
    MittensBridgePacket packet)
{
    const uint32_t write_index =
        mittens_bridge_load_relaxed(&bridge->transmit.write_index);
    const uint32_t read_index =
        mittens_bridge_load_acquire(&bridge->transmit.read_index);
    if ((uint32_t)(write_index - read_index) >=
        MITTENS_BRIDGE_QUEUE_CAPACITY) {
        return 0;
    }

    bridge->transmit.packets[
        write_index % MITTENS_BRIDGE_QUEUE_CAPACITY] = packet;
    mittens_bridge_store_release(
        &bridge->transmit.write_index, write_index + UINT32_C(1));
    return 1;
}

static inline int mittens_bridge_tx_pop(
    MittensBridgeShared* bridge,
    MittensBridgePacket* packet)
{
    const uint32_t read_index =
        mittens_bridge_load_relaxed(&bridge->transmit.read_index);
    const uint32_t write_index =
        mittens_bridge_load_acquire(&bridge->transmit.write_index);
    if (read_index == write_index) {
        return 0;
    }

    *packet = bridge->transmit.packets[
        read_index % MITTENS_BRIDGE_QUEUE_CAPACITY];
    mittens_bridge_store_release(
        &bridge->transmit.read_index, read_index + UINT32_C(1));
    return 1;
}

static inline int mittens_bridge_rx_valid(const MittensBridgeShared* bridge)
{
    const uint32_t read_index =
        mittens_bridge_load_relaxed(&bridge->receive.read_index);
    const uint32_t write_index =
        mittens_bridge_load_acquire(&bridge->receive.write_index);
    return read_index != write_index;
}

static inline int mittens_bridge_rx_space(const MittensBridgeShared* bridge)
{
    const uint32_t write_index =
        mittens_bridge_load_relaxed(&bridge->receive.write_index);
    const uint32_t read_index =
        mittens_bridge_load_acquire(&bridge->receive.read_index);
    return (uint32_t)(write_index - read_index) <
           MITTENS_BRIDGE_QUEUE_CAPACITY;
}

static inline int mittens_bridge_rx_push(
    MittensBridgeShared* bridge,
    uint32_t payload)
{
    const uint32_t write_index =
        mittens_bridge_load_relaxed(&bridge->receive.write_index);
    const uint32_t read_index =
        mittens_bridge_load_acquire(&bridge->receive.read_index);
    if ((uint32_t)(write_index - read_index) >=
        MITTENS_BRIDGE_QUEUE_CAPACITY) {
        return 0;
    }

    bridge->receive.payloads[
        write_index % MITTENS_BRIDGE_QUEUE_CAPACITY] = payload;
    mittens_bridge_store_release(
        &bridge->receive.write_index, write_index + UINT32_C(1));
    return 1;
}

static inline int mittens_bridge_rx_pop(
    MittensBridgeShared* bridge,
    uint32_t* payload)
{
    const uint32_t read_index =
        mittens_bridge_load_relaxed(&bridge->receive.read_index);
    const uint32_t write_index =
        mittens_bridge_load_acquire(&bridge->receive.write_index);
    if (read_index == write_index) {
        return 0;
    }

    *payload = bridge->receive.payloads[
        read_index % MITTENS_BRIDGE_QUEUE_CAPACITY];
    mittens_bridge_store_release(
        &bridge->receive.read_index, read_index + UINT32_C(1));
    return 1;
}

#ifdef __cplusplus
}

static_assert(sizeof(MittensBridgePacket) == 8);
static_assert(sizeof(MittensBridgeShared) == 1088);
#endif

#endif
