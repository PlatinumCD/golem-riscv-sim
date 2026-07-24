#ifndef MITTENS_ANALOG_TILE_BRIDGE_H
#define MITTENS_ANALOG_TILE_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MITTENS_ANALOG_BRIDGE_MAGIC UINT32_C(0x4d414e41)
#define MITTENS_ANALOG_BRIDGE_VERSION UINT32_C(1)
#define MITTENS_ANALOG_LINK_WIDTH_BITS UINT32_C(256)
#define MITTENS_ANALOG_WORD_BITS UINT32_C(32)
#define MITTENS_ANALOG_WORDS_PER_BEAT UINT32_C(8)
#define MITTENS_ANALOG_QUEUE_CAPACITY UINT32_C(4)
#define MITTENS_ANALOG_CACHE_LINE_BYTES UINT32_C(64)

enum MittensAnalogOperation {
    MITTENS_ANALOG_OPERATION_SET_MATRIX = 1,
    MITTENS_ANALOG_OPERATION_LOAD_VECTOR = 2,
    MITTENS_ANALOG_OPERATION_COMPUTE = 3,
    MITTENS_ANALOG_OPERATION_STORE_VECTOR = 4,
    MITTENS_ANALOG_OPERATION_MOVE_VECTOR = 5,
};

enum MittensAnalogStatus {
    MITTENS_ANALOG_STATUS_SUCCESS = 0,
    MITTENS_ANALOG_STATUS_INVALID_OPERATION = 1,
    MITTENS_ANALOG_STATUS_INVALID_ARRAY = 2,
    MITTENS_ANALOG_STATUS_INVALID_PAYLOAD = 3,
    MITTENS_ANALOG_STATUS_BACKEND_ERROR = 4,
};

enum MittensAnalogSlotState {
    MITTENS_ANALOG_SLOT_FREE = 0,
    MITTENS_ANALOG_SLOT_SUBMITTED = 1,
    MITTENS_ANALOG_SLOT_ACCEPTED = 2,
    MITTENS_ANALOG_SLOT_COMPLETED = 3,
};

enum MittensAnalogBridgeError {
    MITTENS_ANALOG_BRIDGE_ERROR_NONE = 0,
    MITTENS_ANALOG_BRIDGE_ERROR_BAD_HEADER = 1,
    MITTENS_ANALOG_BRIDGE_ERROR_BAD_SLOT = 2,
    MITTENS_ANALOG_BRIDGE_ERROR_BAD_PAYLOAD = 3,
};

typedef struct MittensAnalogCommand {
    uint32_t operation;
    uint32_t reserved;
    uint64_t operand0;
    uint64_t operand1;
} MittensAnalogCommand;

typedef struct MittensAnalogDataBeat {
    uint32_t words[MITTENS_ANALOG_WORDS_PER_BEAT];
} MittensAnalogDataBeat;

typedef struct MittensAnalogResponse {
    uint64_t status;
} MittensAnalogResponse;

typedef struct __attribute__((aligned(64))) MittensAnalogBridgeHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t structure_size;
    uint32_t array_count;
    uint32_t array_rows;
    uint32_t array_columns;
    uint32_t queue_capacity;
    uint32_t words_per_slot;
    uint32_t link_width_bits;
    uint64_t channel_stride;
    uint64_t slot_stride;
    uint32_t protocol_error;
    uint32_t tile_id;
} MittensAnalogBridgeHeader;

typedef struct __attribute__((aligned(64))) MittensAnalogBridgeChannel {
    uint32_t write_index;
    uint8_t write_padding[60];
    uint32_t accept_index;
    uint8_t accept_padding[60];
} MittensAnalogBridgeChannel;

typedef struct __attribute__((aligned(64))) MittensAnalogBridgeSlot {
    uint32_t state;
    uint32_t input_word_count;
    uint32_t output_word_count;
    uint32_t reserved0;
    uint64_t sequence;
    MittensAnalogCommand command;
    uint64_t status;
    uint64_t reserved1;
} MittensAnalogBridgeSlot;

static inline size_t mittens_analog_align_cache_line(size_t value)
{
    const size_t alignment = MITTENS_ANALOG_CACHE_LINE_BYTES;
    return (value + alignment - 1) & ~(alignment - 1);
}

static inline size_t mittens_analog_words_per_slot(
    uint32_t array_rows,
    uint32_t array_columns)
{
    return (size_t)array_rows * (size_t)array_columns;
}

static inline size_t mittens_analog_slot_stride(
    uint32_t array_rows,
    uint32_t array_columns)
{
    const size_t payload_bytes =
        mittens_analog_words_per_slot(array_rows, array_columns) *
        sizeof(uint32_t);
    return mittens_analog_align_cache_line(
        sizeof(MittensAnalogBridgeSlot) + payload_bytes);
}

static inline size_t mittens_analog_channel_stride(
    uint32_t array_rows,
    uint32_t array_columns)
{
    return sizeof(MittensAnalogBridgeChannel) +
           MITTENS_ANALOG_QUEUE_CAPACITY *
               mittens_analog_slot_stride(array_rows, array_columns);
}

static inline size_t mittens_analog_bridge_size(
    uint32_t array_count,
    uint32_t array_rows,
    uint32_t array_columns)
{
    return sizeof(MittensAnalogBridgeHeader) +
           (size_t)array_count *
               mittens_analog_channel_stride(
                   array_rows, array_columns);
}

static inline MittensAnalogBridgeChannel* mittens_analog_channel(
    MittensAnalogBridgeHeader* bridge,
    uint32_t array_id)
{
    return (MittensAnalogBridgeChannel*)(
        (uint8_t*)bridge + sizeof(MittensAnalogBridgeHeader) +
        (size_t)array_id * (size_t)bridge->channel_stride);
}

static inline const MittensAnalogBridgeChannel*
mittens_analog_channel_const(
    const MittensAnalogBridgeHeader* bridge,
    uint32_t array_id)
{
    return (const MittensAnalogBridgeChannel*)(
        (const uint8_t*)bridge + sizeof(MittensAnalogBridgeHeader) +
        (size_t)array_id * (size_t)bridge->channel_stride);
}

static inline MittensAnalogBridgeSlot* mittens_analog_slot(
    MittensAnalogBridgeHeader* bridge,
    uint32_t array_id,
    uint32_t sequence)
{
    MittensAnalogBridgeChannel* channel =
        mittens_analog_channel(bridge, array_id);
    return (MittensAnalogBridgeSlot*)(
        (uint8_t*)channel + sizeof(MittensAnalogBridgeChannel) +
        (size_t)(sequence % MITTENS_ANALOG_QUEUE_CAPACITY) *
            (size_t)bridge->slot_stride);
}

static inline const MittensAnalogBridgeSlot*
mittens_analog_slot_const(
    const MittensAnalogBridgeHeader* bridge,
    uint32_t array_id,
    uint32_t sequence)
{
    const MittensAnalogBridgeChannel* channel =
        mittens_analog_channel_const(bridge, array_id);
    return (const MittensAnalogBridgeSlot*)(
        (const uint8_t*)channel + sizeof(MittensAnalogBridgeChannel) +
        (size_t)(sequence % MITTENS_ANALOG_QUEUE_CAPACITY) *
            (size_t)bridge->slot_stride);
}

static inline uint32_t* mittens_analog_slot_words(
    MittensAnalogBridgeSlot* slot)
{
    return (uint32_t*)((uint8_t*)slot +
                       sizeof(MittensAnalogBridgeSlot));
}

static inline const uint32_t* mittens_analog_slot_words_const(
    const MittensAnalogBridgeSlot* slot)
{
    return (const uint32_t*)((const uint8_t*)slot +
                             sizeof(MittensAnalogBridgeSlot));
}

static inline uint32_t mittens_analog_load_acquire(
    const uint32_t* value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline uint32_t mittens_analog_load_relaxed(
    const uint32_t* value)
{
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static inline void mittens_analog_store_release(
    uint32_t* value,
    uint32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static inline void mittens_analog_store_relaxed(
    uint32_t* value,
    uint32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELAXED);
}

#ifdef __cplusplus
}

static_assert(sizeof(MittensAnalogCommand) == 24);
static_assert(sizeof(MittensAnalogDataBeat) == 32);
static_assert(sizeof(MittensAnalogResponse) == 8);
static_assert(sizeof(MittensAnalogBridgeHeader) == 64);
static_assert(sizeof(MittensAnalogBridgeChannel) == 128);
static_assert(sizeof(MittensAnalogBridgeSlot) == 64);
#else
_Static_assert(sizeof(MittensAnalogCommand) == 24,
               "MittensAnalogCommand ABI changed");
_Static_assert(sizeof(MittensAnalogDataBeat) == 32,
               "MittensAnalogDataBeat ABI changed");
_Static_assert(sizeof(MittensAnalogResponse) == 8,
               "MittensAnalogResponse ABI changed");
_Static_assert(sizeof(MittensAnalogBridgeHeader) == 64,
               "MittensAnalogBridgeHeader ABI changed");
_Static_assert(sizeof(MittensAnalogBridgeChannel) == 128,
               "MittensAnalogBridgeChannel ABI changed");
_Static_assert(sizeof(MittensAnalogBridgeSlot) == 64,
               "MittensAnalogBridgeSlot ABI changed");
#endif

#endif
