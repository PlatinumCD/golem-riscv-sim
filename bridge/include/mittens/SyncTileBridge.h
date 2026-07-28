#ifndef MITTENS_SYNC_TILE_BRIDGE_H
#define MITTENS_SYNC_TILE_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MITTENS_SYNC_BRIDGE_MAGIC UINT32_C(0x4d53594e)
#define MITTENS_SYNC_BRIDGE_VERSION UINT32_C(3)

enum MittensSyncBridgeState {
    MITTENS_SYNC_STATE_IDLE = 0,
    MITTENS_SYNC_STATE_GRANTED = 1,
    MITTENS_SYNC_STATE_RUNNING = 2,
    MITTENS_SYNC_STATE_EVENT = 3,
    MITTENS_SYNC_STATE_RESUME = 4,
};

enum MittensSyncStopReason {
    MITTENS_SYNC_STOP_NONE = 0,
    MITTENS_SYNC_STOP_QUANTUM_END = 1,
    MITTENS_SYNC_STOP_NIC_TRANSMIT = 2,
    MITTENS_SYNC_STOP_NIC_RECEIVE_WAIT = 3,
    MITTENS_SYNC_STOP_ANALOG_SUBMIT = 4,
    MITTENS_SYNC_STOP_ANALOG_WAIT = 5,
    MITTENS_SYNC_STOP_GUEST_EXIT = 6,
    MITTENS_SYNC_STOP_TASK_START = 7,
    MITTENS_SYNC_STOP_TASK_FINISH = 8,
    MITTENS_SYNC_STOP_NIC_RX_DMA_SUBMIT = 9,
};

enum MittensSyncEventFlags {
    MITTENS_SYNC_EVENT_FLAG_NONE = 0,
    MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION = 1U << 0,
};

enum MittensSyncBridgeError {
    MITTENS_SYNC_BRIDGE_ERROR_NONE = 0,
    MITTENS_SYNC_BRIDGE_ERROR_BAD_HEADER = 1,
    MITTENS_SYNC_BRIDGE_ERROR_BAD_STATE = 2,
    MITTENS_SYNC_BRIDGE_ERROR_BAD_BUDGET = 3,
};

typedef struct __attribute__((aligned(64))) MittensSyncBridge {
    uint32_t magic;
    uint32_t version;
    uint32_t structure_size;
    uint32_t tile_id;

    uint32_t state;
    uint32_t protocol_error;
    uint64_t grant_epoch;

    uint64_t instruction_budget;
    uint64_t event_sequence;
    uint64_t instructions_executed;

    uint32_t stop_reason;
    uint32_t event_flags;
    uint32_t analog_array_id;
    uint32_t rx_dma_source;

    uint64_t analog_sequence;
    uint32_t task_id;
    uint32_t rx_dma_route_id;
    uint64_t execution_id;
    uint32_t rx_dma_word_count;
    uint8_t reserved1[28];
} MittensSyncBridge;

static inline uint32_t mittens_sync_load_acquire(
    const uint32_t* value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline uint32_t mittens_sync_load_relaxed(
    const uint32_t* value)
{
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static inline uint64_t mittens_sync_load_u64_acquire(
    const uint64_t* value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline void mittens_sync_store_release(
    uint32_t* value,
    uint32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static inline void mittens_sync_store_relaxed(
    uint32_t* value,
    uint32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELAXED);
}

static inline void mittens_sync_store_u64_relaxed(
    uint64_t* value,
    uint64_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELAXED);
}

#ifdef __cplusplus
}

static_assert(sizeof(MittensSyncBridge) == 128);
#else
_Static_assert(sizeof(MittensSyncBridge) == 128,
               "MittensSyncBridge ABI changed");
#endif

#endif
