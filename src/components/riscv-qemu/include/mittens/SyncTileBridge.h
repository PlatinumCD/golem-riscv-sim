#ifndef MITTENS_SYNC_TILE_BRIDGE_H
#define MITTENS_SYNC_TILE_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MITTENS_SYNC_BRIDGE_MAGIC UINT32_C(0x4d53594e)
#define MITTENS_SYNC_BRIDGE_VERSION UINT32_C(36)
#define MITTENS_SYNC_FETCH_SEGMENT_CAPACITY UINT32_C(16)
#define MITTENS_SYNC_MEMORY_BATCH_CAPACITY UINT32_C(1024)
#define MITTENS_SYNC_GLOBAL_DMA_BATCH_CAPACITY UINT32_C(8)
#define MITTENS_SYNC_ANALOG_BATCH_CAPACITY UINT32_C(1024)

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
    MITTENS_SYNC_STOP_MEMORY_ACCESS = 10,
    MITTENS_SYNC_STOP_NIC_TRANSMIT_WAIT = 12,
    MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT = 13,
    MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT = 14,
    MITTENS_SYNC_STOP_MEMORY_BATCH = 15,
    MITTENS_SYNC_STOP_MEMORY_FENCE = 16,
    MITTENS_SYNC_STOP_NIC_RX_SOFTWARE_CLAIM = 17,
    MITTENS_SYNC_STOP_EPOCH_BARRIER_ARRIVE = 18,
    MITTENS_SYNC_STOP_SCRATCHPAD_DMA_SUBMIT_BATCH = 19,
    MITTENS_SYNC_STOP_ANALOG_SUBMIT_BATCH = 20,
    MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_BATCH = 21,
    // One transport envelope containing a compiler-certified DMA event tape.
    // SST replays every submit and wait in explicit event-ordinal order at the
    // recorded guest instruction boundary; this stop is not itself a modeled
    // architectural event.
    MITTENS_SYNC_STOP_SCRATCHPAD_DMA_MACRO = 22,
    MITTENS_SYNC_STOP_INSTRUCTION_FETCH = 23,
    MITTENS_SYNC_STOP_INSTRUCTION_FENCE = 24,
    MITTENS_SYNC_STOP_SCRATCHPAD_DMA_QUERY = 25,
    MITTENS_SYNC_STOP_SCRATCHPAD_DMA_ACK = 26,
    MITTENS_SYNC_STOP_SCRATCHPAD_DMA_WAIT_ANY = 27,
    MITTENS_SYNC_STOP_VECTOR_ANALOG = 28,
    MITTENS_SYNC_STOP_LSQ_SUBMIT = 29,
    MITTENS_SYNC_STOP_LSQ_WAIT = 30,
    MITTENS_SYNC_STOP_ASQ_SUBMIT = 31,
    MITTENS_SYNC_STOP_ASQ_WAIT = 32,
    MITTENS_SYNC_STOP_NETWORK = 33,
    MITTENS_SYNC_STOP_SLQ_SUBMIT = 34,
    MITTENS_SYNC_STOP_SLQ_WAIT = 35,
    MITTENS_SYNC_STOP_COUNT = 36,
};

enum MittensSyncEpochContribution {
    MITTENS_SYNC_EPOCH_WORK_COMPLETE = 0,
    MITTENS_SYNC_EPOCH_IDLE = 1,
};

enum MittensSyncEventFlags {
    MITTENS_SYNC_EVENT_FLAG_NONE = 0,
    MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION = 1U << 0,
    MITTENS_SYNC_EVENT_FLAG_NIC_BURST = 1U << 1,
    MITTENS_SYNC_EVENT_FLAG_QUANTUM_END = 1U << 2,
    // The semantic stop event also carries the pending memory-access batch.
    // SST replays that batch before servicing the stop reason, preserving
    // architectural order while avoiding a second fd-41 round trip.
    MITTENS_SYNC_EVENT_FLAG_MEMORY_BATCH = 1U << 3,
    // The semantic stop also carries earlier nonblocking analog submissions.
    // SST replays each record at its original guest instruction boundary.
    MITTENS_SYNC_EVENT_FLAG_ANALOG_BATCH = 1U << 4,
    // A global-DMA wait batch also carries the submissions named by its
    // records. SST replays every physical request before collectively waiting
    // for their independently modeled completions.
    MITTENS_SYNC_EVENT_FLAG_GLOBAL_DMA_SUBMITS = 1U << 5,
    // Memory records from one vector instruction, not cross-instruction batching.
    MITTENS_SYNC_EVENT_FLAG_VECTOR_MEMORY = 1U << 6,
};

enum MittensSyncMemoryFlags {
    MITTENS_SYNC_MEMORY_FLAG_NONE = 0,
    MITTENS_SYNC_MEMORY_FLAG_WRITE = 1U << 0,
    MITTENS_SYNC_MEMORY_FLAG_SCRATCHPAD = 1U << 1,
    MITTENS_SYNC_MEMORY_FLAG_REGISTER_DEPS = 1U << 2,
    // One record represents repeat_count adjacent accesses of size bytes from
    // the same dynamic instruction.  This is restricted to scratchpad
    // batches; SST expands its logical accounting.  A vector transaction is
    // an aligned eight-element e32 run represented as one 32-byte CPU beat.
    MITTENS_SYNC_MEMORY_FLAG_CONTIGUOUS_RUN = 1U << 3,
    MITTENS_SYNC_MEMORY_FLAG_VECTOR_TRANSACTION = 1U << 4,
    // One contiguous, active RVV beat, at most VLEN/8 bytes. Published BEFORE
    // any functional access; its element callbacks run only after SST grants
    // completion. This is independent of the legacy post-access batch flags.
    MITTENS_SYNC_MEMORY_FLAG_PREACCESS_VECTOR = 1U << 5,
};

enum MittensSyncBridgeError {
    MITTENS_SYNC_BRIDGE_ERROR_NONE = 0,
    MITTENS_SYNC_BRIDGE_ERROR_BAD_HEADER = 1,
    MITTENS_SYNC_BRIDGE_ERROR_BAD_STATE = 2,
    MITTENS_SYNC_BRIDGE_ERROR_BAD_BUDGET = 3,
};

typedef struct MittensSyncNetwork {
    uint64_t first, second, result;
    uint32_t operation, completed;
} MittensSyncNetwork;

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

    union {
        uint64_t analog_sequence;
        uint64_t memory_program_counter;
        uint64_t rx_dma_logical_iteration;
    };
    uint32_t task_id;
    uint32_t rx_dma_route_id;
    union {
        uint64_t execution_id;
        uint64_t memory_return_address;
    };
    uint32_t rx_dma_word_count;
    uint32_t reserved0;
    uint64_t vector_instructions_executed;
    union {
        uint64_t memory_address;
    };
    uint32_t memory_size;
    uint32_t memory_flags;
    uint64_t global_dma_logical_iteration;
    uint64_t global_dma_scratchpad_offset;
    uint32_t epoch_id;
    uint32_t epoch_contribution;
    uint32_t memory_batch_count;
    // Scratchpad-DMA request policy.  The field occupies the former reserved
    // word, so the bridge remains 192 bytes while version 21 makes its meaning
    // fail closed across QEMU/SST builds.
    uint32_t global_dma_request_flags;
    uint32_t global_dma_batch_count;
    uint32_t analog_batch_count;
    /* Host-only fetch grouping. Published with EVENT/RESUME release stores. */
    uint32_t fetch_segment_limit;
    uint32_t fetch_segment_proposed;
    uint32_t fetch_segment_approved;
    uint32_t fetch_segment_reserved;
    MittensSyncNetwork network;
} MittensSyncBridge;

typedef struct MittensSyncMemoryAccess {
    uint64_t instructions_executed;
    uint64_t vector_instructions_executed;
    uint64_t address;
    uint64_t program_counter;
    uint64_t return_address;
    uint32_t size;
    uint32_t flags;
    uint32_t source_register_mask;
    uint32_t destination_register_mask;
    uint32_t instruction_length;
    uint32_t repeat_count;
} MittensSyncMemoryAccess;

/*
 * One physical global-RAM DMA job. Submit-only records leave both wait
 * instruction fields and both event ordinals at their maximum values. A
 * compiler-certified macro records the submit and wait event ordinals so SST
 * can replay an arbitrary bounded interleaving without inferring serial order.
 * In every representation each physical request retains its own readiness,
 * service, and completion state.
 */
typedef struct MittensSyncGlobalDMASubmit {
    uint64_t instructions_executed;
    uint64_t vector_instructions_executed;
    uint64_t wait_instructions_executed;
    uint64_t wait_vector_instructions_executed;
    uint64_t global_offset;
    uint64_t scratchpad_offset;
    uint64_t execution_id;
    uint64_t logical_iteration;
    uint32_t token_id;
    uint32_t byte_count;
    uint32_t direction;
    uint32_t request_flags;
    uint32_t submit_event_ordinal;
    uint32_t wait_event_ordinal;
} MittensSyncGlobalDMASubmit;

/*
 * One already-published, nonblocking analog submission.  The command and its
 * immutable payload live in the fd-43 slot identified by array_id/sequence;
 * this record preserves the exact guest CPU boundary at which it was issued.
 */
typedef struct MittensSyncAnalogSubmit {
    uint64_t instructions_executed;
    uint64_t vector_instructions_executed;
    uint64_t sequence;
    uint32_t array_id;
    uint32_t flags;
} MittensSyncAnalogSubmit;

/* One synchronous register-to-array transaction. Data holds little-endian
 * float32 bit patterns; no guest memory address or SPM access is implied. */
#define MITTENS_SYNC_VECTOR_ANALOG_BYTES UINT32_C(1024)
#define MITTENS_SYNC_VECTOR_ANALOG_PENDING UINT32_MAX
enum MittensSyncVectorAnalogOperation {
    MITTENS_SYNC_VECTOR_ANALOG_PROGRAM = 0,
    MITTENS_SYNC_VECTOR_ANALOG_LOAD = 1,
    MITTENS_SYNC_VECTOR_ANALOG_EXECUTE = 2,
    MITTENS_SYNC_VECTOR_ANALOG_STORE = 3,
    MITTENS_SYNC_VECTOR_ANALOG_CONFIGURE = 4,
};
typedef struct MittensSyncVectorAnalog {
    uint64_t array_id;
    uint64_t element_offset;
    uint32_t operation;
    uint32_t element_count;
    uint32_t status;
    uint32_t reserved;
    uint8_t data[MITTENS_SYNC_VECTOR_ANALOG_BYTES];
} MittensSyncVectorAnalog;

/* Deferred, non-speculative RVV memory requests. QEMU owns FREE/SUBMITTED
 * and consumes COMPLETE; SST owns INFLIGHT/COMPLETE. A monotonically increasing
 * token disambiguates reuse of the same slot. No process-local pointer crosses
 * the bridge. Payload bytes use guest little-endian element order. */
#define MITTENS_SYNC_LSQ_CAPACITY UINT32_C(64)
#define MITTENS_SYNC_LSQ_BYTES UINT32_C(128)
enum MittensSyncLoadStoreState {
    MITTENS_SYNC_LSQ_FREE = 0,
    MITTENS_SYNC_LSQ_SUBMITTED = 1,
    MITTENS_SYNC_LSQ_INFLIGHT = 2,
    MITTENS_SYNC_LSQ_COMPLETE = 3,
};
enum MittensSyncLoadStoreWaitReason {
    MITTENS_SYNC_LSQ_WAIT_NONE = 0,
    MITTENS_SYNC_LSQ_WAIT_REGISTER = 1,
    MITTENS_SYNC_LSQ_WAIT_DRAIN = 2,
};
typedef struct MittensSyncLoadStoreSlot {
    uint64_t token;
    uint64_t address;
    uint64_t program_counter;
    uint32_t size;
    uint32_t write;
    uint32_t destination;
    uint32_t element_bytes;
    uint32_t state;
    uint32_t reserved;
    uint8_t data[MITTENS_SYNC_LSQ_BYTES];
} MittensSyncLoadStoreSlot;

typedef struct MittensSyncLoadStoreQueue {
    uint32_t depth;
    uint32_t wait_reason;
    uint64_t wait_mask;
    MittensSyncLoadStoreSlot slots[MITTENS_SYNC_LSQ_CAPACITY];
} MittensSyncLoadStoreQueue;

/* Deferred register/array transfers. QEMU owns FREE/SUBMITTED and snapshots
 * active FP32LE elements. SST guarantees successful backend admission before
 * resuming SUBMIT, then independently releases the source pin when the modeled
 * register-link read has captured all source data. Destination payloads become
 * architectural only after COMPLETE. ERROR is a synchronous admission result;
 * admitted requests must not acquire a later architectural exception. */
#define MITTENS_SYNC_ASQ_CAPACITY UINT32_C(16)
#define MITTENS_SYNC_ASQ_BYTES UINT32_C(1024)
enum MittensSyncAnalogQueueState {
    MITTENS_SYNC_ASQ_FREE = 0,
    MITTENS_SYNC_ASQ_SUBMITTED = 1,
    MITTENS_SYNC_ASQ_INFLIGHT = 2,
    MITTENS_SYNC_ASQ_COMPLETE = 3,
    MITTENS_SYNC_ASQ_ERROR = 4,
};
enum MittensSyncAnalogQueueWaitReason {
    MITTENS_SYNC_ASQ_WAIT_NONE = 0,
    MITTENS_SYNC_ASQ_WAIT_REGISTER = 1,
    MITTENS_SYNC_ASQ_WAIT_DRAIN = 2,
    MITTENS_SYNC_ASQ_WAIT_FULL = 3,
};
typedef struct MittensSyncAnalogQueueSlot {
    uint64_t token;
    uint64_t program_counter;
    uint64_t array_id;
    uint64_t element_offset;
    uint32_t operation;
    uint32_t element_count;
    uint32_t vector_register;
    uint32_t register_mask;
    uint32_t state;
    uint32_t status;
    uint32_t source_captured;
    uint32_t reserved;
    uint8_t data[MITTENS_SYNC_ASQ_BYTES];
} MittensSyncAnalogQueueSlot;

typedef struct MittensSyncAnalogQueue {
    uint32_t depth;
    uint32_t wait_reason;
    uint64_t wait_mask;
    MittensSyncAnalogQueueSlot slots[MITTENS_SYNC_ASQ_CAPACITY];
} MittensSyncAnalogQueue;

#define MITTENS_SYNC_SLQ_CAPACITY UINT32_C(64)
typedef struct MittensSyncScalarSlot {
    uint64_t token;
    uint64_t address;
    uint64_t program_counter;
    uint32_t size;
    uint32_t write;
    uint32_t destination;
    uint32_t element_bytes;
    uint32_t state;
    uint32_t reserved;
    uint8_t data[8];
} MittensSyncScalarSlot;

typedef struct MittensSyncScalarQueue {
    uint32_t depth;
    uint32_t wait_reason;
    uint64_t wait_mask;
    MittensSyncScalarSlot slots[MITTENS_SYNC_SLQ_CAPACITY];
} MittensSyncScalarQueue;


#define MITTENS_SYNC_BRIDGE_MAPPING_SIZE \
    (sizeof(MittensSyncBridge) + \
     sizeof(MittensSyncMemoryAccess) * \
         MITTENS_SYNC_MEMORY_BATCH_CAPACITY + \
     sizeof(MittensSyncGlobalDMASubmit) * \
         MITTENS_SYNC_GLOBAL_DMA_BATCH_CAPACITY + \
     sizeof(MittensSyncAnalogSubmit) * \
         MITTENS_SYNC_ANALOG_BATCH_CAPACITY + \
     sizeof(MittensSyncScalarQueue) + \
     sizeof(MittensSyncLoadStoreQueue) + \
     sizeof(MittensSyncVectorAnalog) + \
     sizeof(MittensSyncAnalogQueue))

static inline MittensSyncScalarQueue*
mittens_sync_scalar_queue(MittensSyncBridge* bridge)
{
    return (MittensSyncScalarQueue*)((uint8_t*)bridge +
        MITTENS_SYNC_BRIDGE_MAPPING_SIZE - sizeof(MittensSyncAnalogQueue) -
        sizeof(MittensSyncVectorAnalog) - sizeof(MittensSyncLoadStoreQueue) -
        sizeof(MittensSyncScalarQueue));
}

static inline MittensSyncAnalogQueue*
mittens_sync_analog_queue(MittensSyncBridge* bridge)
{
    return (MittensSyncAnalogQueue*)((uint8_t*)bridge +
        MITTENS_SYNC_BRIDGE_MAPPING_SIZE - sizeof(MittensSyncAnalogQueue));
}

static inline const MittensSyncAnalogQueue*
mittens_sync_analog_queue_const(const MittensSyncBridge* bridge)
{
    return (const MittensSyncAnalogQueue*)((const uint8_t*)bridge +
        MITTENS_SYNC_BRIDGE_MAPPING_SIZE - sizeof(MittensSyncAnalogQueue));
}

static inline MittensSyncLoadStoreQueue*
mittens_sync_load_store_queue(MittensSyncBridge* bridge)
{
    return (MittensSyncLoadStoreQueue*)((uint8_t*)bridge +
        MITTENS_SYNC_BRIDGE_MAPPING_SIZE - sizeof(MittensSyncAnalogQueue) -
        sizeof(MittensSyncVectorAnalog) - sizeof(MittensSyncLoadStoreQueue));
}

static inline MittensSyncVectorAnalog*
mittens_sync_vector_analog_payload(MittensSyncBridge* bridge)
{
    return (MittensSyncVectorAnalog*)((uint8_t*)bridge +
        MITTENS_SYNC_BRIDGE_MAPPING_SIZE - sizeof(MittensSyncAnalogQueue) -
        sizeof(MittensSyncVectorAnalog));
}

static inline const MittensSyncVectorAnalog*
mittens_sync_vector_analog_payload_const(const MittensSyncBridge* bridge)
{
    return (const MittensSyncVectorAnalog*)((const uint8_t*)bridge +
        MITTENS_SYNC_BRIDGE_MAPPING_SIZE - sizeof(MittensSyncAnalogQueue) -
        sizeof(MittensSyncVectorAnalog));
}

static inline MittensSyncMemoryAccess* mittens_sync_memory_batch(
    MittensSyncBridge* bridge)
{
    return (MittensSyncMemoryAccess*)((uint8_t*)bridge +
                                      sizeof(MittensSyncBridge));
}

static inline const MittensSyncMemoryAccess*
mittens_sync_memory_batch_const(const MittensSyncBridge* bridge)
{
    return (const MittensSyncMemoryAccess*)((const uint8_t*)bridge +
                                            sizeof(MittensSyncBridge));
}

static inline MittensSyncGlobalDMASubmit*
mittens_sync_global_dma_batch(MittensSyncBridge* bridge)
{
    return (MittensSyncGlobalDMASubmit*)(
        (uint8_t*)bridge + sizeof(MittensSyncBridge) +
        sizeof(MittensSyncMemoryAccess) *
            MITTENS_SYNC_MEMORY_BATCH_CAPACITY);
}

static inline const MittensSyncGlobalDMASubmit*
mittens_sync_global_dma_batch_const(const MittensSyncBridge* bridge)
{
    return (const MittensSyncGlobalDMASubmit*)(
        (const uint8_t*)bridge + sizeof(MittensSyncBridge) +
        sizeof(MittensSyncMemoryAccess) *
            MITTENS_SYNC_MEMORY_BATCH_CAPACITY);
}

static inline MittensSyncAnalogSubmit* mittens_sync_analog_batch(
    MittensSyncBridge* bridge)
{
    return (MittensSyncAnalogSubmit*)(
        (uint8_t*)bridge + sizeof(MittensSyncBridge) +
        sizeof(MittensSyncMemoryAccess) *
            MITTENS_SYNC_MEMORY_BATCH_CAPACITY +
        sizeof(MittensSyncGlobalDMASubmit) *
            MITTENS_SYNC_GLOBAL_DMA_BATCH_CAPACITY);
}

static inline const MittensSyncAnalogSubmit*
mittens_sync_analog_batch_const(const MittensSyncBridge* bridge)
{
    return (const MittensSyncAnalogSubmit*)(
        (const uint8_t*)bridge + sizeof(MittensSyncBridge) +
        sizeof(MittensSyncMemoryAccess) *
            MITTENS_SYNC_MEMORY_BATCH_CAPACITY +
        sizeof(MittensSyncGlobalDMASubmit) *
            MITTENS_SYNC_GLOBAL_DMA_BATCH_CAPACITY);
}

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

static_assert(sizeof(MittensSyncBridge) == 256);
static_assert(sizeof(MittensSyncMemoryAccess) == 64);
static_assert(sizeof(MittensSyncGlobalDMASubmit) == 88);
static_assert(sizeof(MittensSyncAnalogSubmit) == 32);
static_assert(sizeof(MittensSyncVectorAnalog) == 1056);
static_assert(sizeof(MittensSyncLoadStoreSlot) == 176);
static_assert(sizeof(MittensSyncLoadStoreQueue) == 11280);
static_assert(sizeof(MittensSyncAnalogQueueSlot) == 1088);
static_assert(sizeof(MittensSyncAnalogQueue) == 17424);
static_assert(sizeof(MittensSyncScalarSlot) == 56);
static_assert(sizeof(MittensSyncScalarQueue) == 3600);
#else
_Static_assert(sizeof(MittensSyncScalarSlot) == 56, "scalar slot ABI changed");
_Static_assert(sizeof(MittensSyncScalarQueue) == 3600, "scalar queue ABI changed");
_Static_assert(sizeof(MittensSyncBridge) == 256,
               "MittensSyncBridge ABI changed");
_Static_assert(sizeof(MittensSyncMemoryAccess) == 64,
               "MittensSyncMemoryAccess ABI changed");
_Static_assert(sizeof(MittensSyncGlobalDMASubmit) == 88,
               "MittensSyncGlobalDMASubmit ABI changed");
_Static_assert(sizeof(MittensSyncAnalogSubmit) == 32,
               "MittensSyncAnalogSubmit ABI changed");
_Static_assert(sizeof(MittensSyncVectorAnalog) == 1056,
               "MittensSyncVectorAnalog ABI changed");
_Static_assert(sizeof(MittensSyncLoadStoreSlot) == 176,
               "MittensSyncLoadStoreSlot ABI changed");
_Static_assert(sizeof(MittensSyncLoadStoreQueue) == 11280,
               "MittensSyncLoadStoreQueue ABI changed");
_Static_assert(sizeof(MittensSyncAnalogQueueSlot) == 1088,
               "MittensSyncAnalogQueueSlot ABI changed");
_Static_assert(sizeof(MittensSyncAnalogQueue) == 17424,
               "MittensSyncAnalogQueue ABI changed");
#endif

#endif
