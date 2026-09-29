#ifndef MITTENS_NIC_TILE_BRIDGE_H
#define MITTENS_NIC_TILE_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MITTENS_BRIDGE_MAGIC UINT32_C(0x4d495454)
#define MITTENS_BRIDGE_VERSION UINT32_C(7)
#define MITTENS_BRIDGE_QUEUE_CAPACITY UINT32_C(64)
#define MITTENS_BRIDGE_BURST_WORD_CAPACITY UINT32_C(4096)
#define MITTENS_BRIDGE_BURST_QUEUE_CAPACITY UINT32_C(4)
#define MITTENS_BRIDGE_RX_DMA_COMPLETION_CAPACITY \
    MITTENS_BRIDGE_QUEUE_CAPACITY

enum MittensBridgeError {
    MITTENS_BRIDGE_ERROR_NONE = 0,
    MITTENS_BRIDGE_ERROR_TX_FULL = 1,
    MITTENS_BRIDGE_ERROR_RX_EMPTY = 2,
    MITTENS_BRIDGE_ERROR_BAD_MMIO = 3,
    MITTENS_BRIDGE_ERROR_BAD_DESTINATION = 4,
    MITTENS_BRIDGE_ERROR_BAD_BURST = 5,
    MITTENS_BRIDGE_ERROR_DMA = 6,
};

typedef struct MittensBridgePacket {
    uint32_t destination;
    uint32_t payload;
} MittensBridgePacket;

typedef struct MittensBridgeRxPacket {
    uint32_t source;
    uint32_t payload;
} MittensBridgeRxPacket;

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
    MittensBridgeRxPacket packets[MITTENS_BRIDGE_QUEUE_CAPACITY];
} MittensBridgeRxRing;

typedef struct MittensBridgeTxBurst {
    uint32_t destination;
    uint32_t word_count;
    uint64_t source_address;
    uint32_t words[MITTENS_BRIDGE_BURST_WORD_CAPACITY];
} MittensBridgeTxBurst;

typedef struct MittensBridgeRxBurst {
    uint32_t source;
    uint32_t word_count;
    uint32_t software_visible;
    uint32_t reserved;
    uint32_t words[MITTENS_BRIDGE_BURST_WORD_CAPACITY];
} MittensBridgeRxBurst;

typedef struct __attribute__((aligned(64))) MittensBridgeTxBurstRing {
    uint32_t write_index;
    uint8_t write_padding[60];
    uint32_t read_index;
    uint8_t read_padding[60];
    MittensBridgeTxBurst bursts[MITTENS_BRIDGE_BURST_QUEUE_CAPACITY];
} MittensBridgeTxBurstRing;

typedef struct __attribute__((aligned(64))) MittensBridgeRxBurstRing {
    uint32_t write_index;
    uint8_t write_padding[60];
    uint32_t read_index;
    uint32_t read_word_offset;
    uint8_t read_padding[56];
    MittensBridgeRxBurst bursts[MITTENS_BRIDGE_BURST_QUEUE_CAPACITY];
} MittensBridgeRxBurstRing;

typedef struct __attribute__((aligned(64))) MittensBridgeShared {
    uint32_t magic;
    uint32_t version;
    uint32_t structure_size;
    uint32_t queue_capacity;
    uint32_t tile_id;
    uint32_t protocol_error;
    uint32_t rx_dma_timing_enabled;
    uint32_t rx_dma_authorization_write_index;
    uint32_t rx_dma_authorization_read_index;
    uint32_t rx_dma_completion_write_index;
    uint32_t rx_dma_completion_read_index;
    uint8_t header_padding[20];
    MittensBridgeTxRing transmit;
    MittensBridgeRxRing receive;
    MittensBridgeTxBurstRing burst_transmit;
    MittensBridgeRxBurstRing burst_receive;
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
    MittensBridgeRxPacket packet)
{
    const uint32_t write_index =
        mittens_bridge_load_relaxed(&bridge->receive.write_index);
    const uint32_t read_index =
        mittens_bridge_load_acquire(&bridge->receive.read_index);
    if ((uint32_t)(write_index - read_index) >=
        MITTENS_BRIDGE_QUEUE_CAPACITY) {
        return 0;
    }

    bridge->receive.packets[
        write_index % MITTENS_BRIDGE_QUEUE_CAPACITY] = packet;
    mittens_bridge_store_release(
        &bridge->receive.write_index, write_index + UINT32_C(1));
    return 1;
}

static inline int mittens_bridge_rx_pop(
    MittensBridgeShared* bridge,
    MittensBridgeRxPacket* packet)
{
    const uint32_t read_index =
        mittens_bridge_load_relaxed(&bridge->receive.read_index);
    const uint32_t write_index =
        mittens_bridge_load_acquire(&bridge->receive.write_index);
    if (read_index == write_index) {
        return 0;
    }

    *packet = bridge->receive.packets[
        read_index % MITTENS_BRIDGE_QUEUE_CAPACITY];
    mittens_bridge_store_release(
        &bridge->receive.read_index, read_index + UINT32_C(1));
    return 1;
}

static inline int mittens_bridge_rx_peek(
    const MittensBridgeShared* bridge,
    MittensBridgeRxPacket* packet)
{
    const uint32_t read_index =
        mittens_bridge_load_relaxed(&bridge->receive.read_index);
    const uint32_t write_index =
        mittens_bridge_load_acquire(&bridge->receive.write_index);
    if (read_index == write_index) {
        return 0;
    }

    *packet = bridge->receive.packets[
        read_index % MITTENS_BRIDGE_QUEUE_CAPACITY];
    return 1;
}

static inline int mittens_bridge_tx_burst_ready(
    const MittensBridgeShared* bridge)
{
    const uint32_t write_index =
        mittens_bridge_load_relaxed(&bridge->burst_transmit.write_index);
    const uint32_t read_index =
        mittens_bridge_load_acquire(&bridge->burst_transmit.read_index);
    return (uint32_t)(write_index - read_index) <
           MITTENS_BRIDGE_BURST_QUEUE_CAPACITY;
}

static inline int mittens_bridge_tx_burst_push(
    MittensBridgeShared* bridge,
    uint32_t destination,
    uint64_t source_address,
    const uint32_t* words,
    uint32_t word_count)
{
    uint32_t index;
    MittensBridgeTxBurst* burst;

    if (words == NULL ||
        word_count == 0 ||
        word_count > MITTENS_BRIDGE_BURST_WORD_CAPACITY) {
        return 0;
    }
    index = mittens_bridge_load_relaxed(
        &bridge->burst_transmit.write_index);
    if ((uint32_t)(
            index -
            mittens_bridge_load_acquire(
                &bridge->burst_transmit.read_index)) >=
        MITTENS_BRIDGE_BURST_QUEUE_CAPACITY) {
        return 0;
    }

    burst = &bridge->burst_transmit.bursts[
        index % MITTENS_BRIDGE_BURST_QUEUE_CAPACITY];
    burst->destination = destination;
    burst->word_count = word_count;
    burst->source_address = source_address;
    for (uint32_t word = 0; word < word_count; ++word) {
        burst->words[word] = words[word];
    }
    mittens_bridge_store_release(
        &bridge->burst_transmit.write_index, index + UINT32_C(1));
    return 1;
}

static inline int mittens_bridge_tx_burst_pop(
    MittensBridgeShared* bridge,
    MittensBridgeTxBurst* burst)
{
    const uint32_t read_index =
        mittens_bridge_load_relaxed(&bridge->burst_transmit.read_index);
    const uint32_t write_index =
        mittens_bridge_load_acquire(&bridge->burst_transmit.write_index);
    if (burst == NULL || read_index == write_index) {
        return 0;
    }

    *burst = bridge->burst_transmit.bursts[
        read_index % MITTENS_BRIDGE_BURST_QUEUE_CAPACITY];
    mittens_bridge_store_release(
        &bridge->burst_transmit.read_index,
        read_index + UINT32_C(1));
    return 1;
}

static inline int mittens_bridge_rx_burst_valid(
    const MittensBridgeShared* bridge)
{
    const uint32_t read_index =
        mittens_bridge_load_relaxed(&bridge->burst_receive.read_index);
    const uint32_t write_index =
        mittens_bridge_load_acquire(&bridge->burst_receive.write_index);
    return read_index != write_index;
}

static inline uint32_t mittens_bridge_rx_burst_count(
    const MittensBridgeShared* bridge)
{
    const uint32_t write_index =
        mittens_bridge_load_acquire(
            &bridge->burst_receive.write_index);
    const uint32_t read_index =
        mittens_bridge_load_acquire(
            &bridge->burst_receive.read_index);
    return write_index - read_index;
}

static inline uint32_t mittens_bridge_rx_burst_read_index(
    const MittensBridgeShared* bridge)
{
    return mittens_bridge_load_acquire(
        &bridge->burst_receive.read_index);
}

static inline uint32_t mittens_bridge_rx_burst_write_index(
    const MittensBridgeShared* bridge)
{
    return mittens_bridge_load_acquire(
        &bridge->burst_receive.write_index);
}

static inline int mittens_bridge_rx_burst_space(
    const MittensBridgeShared* bridge)
{
    const uint32_t write_index =
        mittens_bridge_load_relaxed(&bridge->burst_receive.write_index);
    const uint32_t read_index =
        mittens_bridge_load_acquire(&bridge->burst_receive.read_index);
    return (uint32_t)(write_index - read_index) <
           MITTENS_BRIDGE_BURST_QUEUE_CAPACITY;
}

static inline int mittens_bridge_rx_burst_software_visible(
    const MittensBridgeShared* bridge)
{
    const uint32_t read_index =
        mittens_bridge_load_relaxed(&bridge->burst_receive.read_index);
    const uint32_t write_index =
        mittens_bridge_load_acquire(&bridge->burst_receive.write_index);
    if (read_index == write_index) {
        return 0;
    }
    return bridge->burst_receive.bursts[
               read_index % MITTENS_BRIDGE_BURST_QUEUE_CAPACITY]
               .software_visible != 0;
}

static inline int mittens_bridge_rx_burst_push_tagged(
    MittensBridgeShared* bridge,
    uint32_t source,
    const uint32_t* words,
    uint32_t word_count,
    int software_visible)
{
    uint32_t index;
    MittensBridgeRxBurst* burst;

    if (words == NULL ||
        word_count == 0 ||
        word_count > MITTENS_BRIDGE_BURST_WORD_CAPACITY) {
        return 0;
    }
    index = mittens_bridge_load_relaxed(
        &bridge->burst_receive.write_index);
    if ((uint32_t)(
            index -
            mittens_bridge_load_acquire(
                &bridge->burst_receive.read_index)) >=
        MITTENS_BRIDGE_BURST_QUEUE_CAPACITY) {
        return 0;
    }

    burst = &bridge->burst_receive.bursts[
        index % MITTENS_BRIDGE_BURST_QUEUE_CAPACITY];
    burst->source = source;
    burst->word_count = word_count;
    burst->software_visible = software_visible ? UINT32_C(1) : UINT32_C(0);
    burst->reserved = 0;
    for (uint32_t word = 0; word < word_count; ++word) {
        burst->words[word] = words[word];
    }
    mittens_bridge_store_release(
        &bridge->burst_receive.write_index, index + UINT32_C(1));
    return 1;
}

static inline int mittens_bridge_rx_burst_push(
    MittensBridgeShared* bridge,
    uint32_t source,
    const uint32_t* words,
    uint32_t word_count)
{
    return mittens_bridge_rx_burst_push_tagged(
        bridge, source, words, word_count, 1);
}

static inline int mittens_bridge_rx_burst_peek(
    const MittensBridgeShared* bridge,
    const MittensBridgeRxBurst** burst)
{
    const uint32_t read_index =
        mittens_bridge_load_relaxed(&bridge->burst_receive.read_index);
    const uint32_t write_index =
        mittens_bridge_load_acquire(&bridge->burst_receive.write_index);
    const uint32_t word_offset =
        mittens_bridge_load_relaxed(
            &bridge->burst_receive.read_word_offset);

    if (burst == NULL ||
        read_index == write_index ||
        word_offset != 0) {
        return 0;
    }
    *burst = &bridge->burst_receive.bursts[
        read_index % MITTENS_BRIDGE_BURST_QUEUE_CAPACITY];
    if ((*burst)->word_count == 0 ||
        (*burst)->word_count > MITTENS_BRIDGE_BURST_WORD_CAPACITY) {
        *burst = NULL;
        return 0;
    }
    return 1;
}

static inline int mittens_bridge_rx_burst_peek_at(
    const MittensBridgeShared* bridge,
    uint32_t offset,
    uint32_t* absolute_index,
    const MittensBridgeRxBurst** burst)
{
    const uint32_t read_index =
        mittens_bridge_load_acquire(
            &bridge->burst_receive.read_index);
    const uint32_t write_index =
        mittens_bridge_load_acquire(
            &bridge->burst_receive.write_index);
    const uint32_t word_offset =
        mittens_bridge_load_acquire(
            &bridge->burst_receive.read_word_offset);

    if (absolute_index == NULL ||
        burst == NULL ||
        offset >= write_index - read_index ||
        (offset == 0 && word_offset != 0)) {
        return 0;
    }
    *absolute_index = read_index + offset;
    *burst = &bridge->burst_receive.bursts[
        *absolute_index % MITTENS_BRIDGE_BURST_QUEUE_CAPACITY];
    if ((*burst)->word_count == 0 ||
        (*burst)->word_count > MITTENS_BRIDGE_BURST_WORD_CAPACITY) {
        *burst = NULL;
        return 0;
    }
    return 1;
}

static inline int mittens_bridge_rx_burst_consume(
    MittensBridgeShared* bridge)
{
    const uint32_t read_index =
        mittens_bridge_load_relaxed(&bridge->burst_receive.read_index);
    const uint32_t write_index =
        mittens_bridge_load_acquire(&bridge->burst_receive.write_index);
    const uint32_t word_offset =
        mittens_bridge_load_relaxed(
            &bridge->burst_receive.read_word_offset);

    if (read_index == write_index || word_offset != 0) {
        return 0;
    }
    mittens_bridge_store_release(
        &bridge->burst_receive.read_index,
        read_index + UINT32_C(1));
    return 1;
}

static inline int mittens_bridge_rx_dma_timing_enabled(
    const MittensBridgeShared* bridge)
{
    return mittens_bridge_load_acquire(
               &bridge->rx_dma_timing_enabled) != 0;
}

static inline int mittens_bridge_rx_dma_authorization_available(
    const MittensBridgeShared* bridge)
{
    const uint32_t write_index =
        mittens_bridge_load_acquire(
            &bridge->rx_dma_authorization_write_index);
    const uint32_t read_index =
        mittens_bridge_load_acquire(
            &bridge->rx_dma_authorization_read_index);
    return read_index != write_index;
}

static inline int mittens_bridge_rx_dma_authorize(
    MittensBridgeShared* bridge)
{
    const uint32_t write_index =
        mittens_bridge_load_relaxed(
            &bridge->rx_dma_authorization_write_index);
    const uint32_t read_index =
        mittens_bridge_load_acquire(
            &bridge->rx_dma_authorization_read_index);
    if (write_index - read_index >=
        MITTENS_BRIDGE_BURST_QUEUE_CAPACITY) {
        return 0;
    }
    mittens_bridge_store_release(
        &bridge->rx_dma_authorization_write_index,
        write_index + UINT32_C(1));
    return 1;
}

static inline int mittens_bridge_rx_dma_consume_authorization(
    MittensBridgeShared* bridge)
{
    const uint32_t read_index =
        mittens_bridge_load_relaxed(
            &bridge->rx_dma_authorization_read_index);
    const uint32_t write_index =
        mittens_bridge_load_acquire(
            &bridge->rx_dma_authorization_write_index);
    if (read_index == write_index) {
        return 0;
    }
    mittens_bridge_store_release(
        &bridge->rx_dma_authorization_read_index,
        read_index + UINT32_C(1));
    return 1;
}

static inline int mittens_bridge_rx_dma_completion_available(
    const MittensBridgeShared* bridge)
{
    const uint32_t write_index =
        mittens_bridge_load_acquire(
            &bridge->rx_dma_completion_write_index);
    const uint32_t read_index =
        mittens_bridge_load_acquire(
            &bridge->rx_dma_completion_read_index);
    return read_index != write_index;
}

static inline int mittens_bridge_rx_dma_publish_completion(
    MittensBridgeShared* bridge)
{
    const uint32_t write_index =
        mittens_bridge_load_relaxed(
            &bridge->rx_dma_completion_write_index);
    const uint32_t read_index =
        mittens_bridge_load_acquire(
            &bridge->rx_dma_completion_read_index);
    if (write_index - read_index >=
        MITTENS_BRIDGE_RX_DMA_COMPLETION_CAPACITY) {
        return 0;
    }
    mittens_bridge_store_release(
        &bridge->rx_dma_completion_write_index,
        write_index + UINT32_C(1));
    return 1;
}

static inline int mittens_bridge_rx_dma_consume_completion(
    MittensBridgeShared* bridge)
{
    const uint32_t read_index =
        mittens_bridge_load_relaxed(
            &bridge->rx_dma_completion_read_index);
    const uint32_t write_index =
        mittens_bridge_load_acquire(
            &bridge->rx_dma_completion_write_index);
    if (read_index == write_index) {
        return 0;
    }
    mittens_bridge_store_release(
        &bridge->rx_dma_completion_read_index,
        read_index + UINT32_C(1));
    return 1;
}

static inline int mittens_bridge_rx_burst_peek_word(
    const MittensBridgeShared* bridge,
    MittensBridgeRxPacket* packet)
{
    const uint32_t read_index =
        mittens_bridge_load_relaxed(&bridge->burst_receive.read_index);
    const uint32_t write_index =
        mittens_bridge_load_acquire(&bridge->burst_receive.write_index);
    const uint32_t word_offset =
        mittens_bridge_load_relaxed(
            &bridge->burst_receive.read_word_offset);
    const MittensBridgeRxBurst* burst;

    if (packet == NULL || read_index == write_index) {
        return 0;
    }
    burst = &bridge->burst_receive.bursts[
        read_index % MITTENS_BRIDGE_BURST_QUEUE_CAPACITY];
    if (burst->word_count == 0 ||
        burst->word_count > MITTENS_BRIDGE_BURST_WORD_CAPACITY ||
        word_offset >= burst->word_count) {
        return 0;
    }
    packet->source = burst->source;
    packet->payload = burst->words[word_offset];
    return 1;
}

static inline int mittens_bridge_rx_burst_pop_word(
    MittensBridgeShared* bridge,
    MittensBridgeRxPacket* packet)
{
    uint32_t read_index;
    uint32_t write_index;
    uint32_t word_offset;
    const MittensBridgeRxBurst* burst;

    if (packet == NULL) {
        return 0;
    }
    read_index =
        mittens_bridge_load_relaxed(&bridge->burst_receive.read_index);
    write_index =
        mittens_bridge_load_acquire(&bridge->burst_receive.write_index);
    if (read_index == write_index) {
        return 0;
    }

    word_offset =
        mittens_bridge_load_relaxed(
            &bridge->burst_receive.read_word_offset);
    burst = &bridge->burst_receive.bursts[
        read_index % MITTENS_BRIDGE_BURST_QUEUE_CAPACITY];
    if (burst->word_count == 0 ||
        burst->word_count > MITTENS_BRIDGE_BURST_WORD_CAPACITY ||
        word_offset >= burst->word_count) {
        return 0;
    }

    packet->source = burst->source;
    packet->payload = burst->words[word_offset];
    ++word_offset;
    if (word_offset == burst->word_count) {
        mittens_bridge_store_relaxed(
            &bridge->burst_receive.read_word_offset, 0);
        mittens_bridge_store_release(
            &bridge->burst_receive.read_index,
            read_index + UINT32_C(1));
    } else {
        mittens_bridge_store_relaxed(
            &bridge->burst_receive.read_word_offset, word_offset);
    }
    return 1;
}

#ifdef __cplusplus
}

static_assert(sizeof(MittensBridgePacket) == 8);
static_assert(sizeof(MittensBridgeRxPacket) == 8);
static_assert(sizeof(MittensBridgeTxBurst) == 16400);
static_assert(sizeof(MittensBridgeRxBurst) == 16400);
static_assert(sizeof(MittensBridgeShared) == 132800);
#endif

#endif
