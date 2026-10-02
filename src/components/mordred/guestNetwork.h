#ifndef GOLEM_GUEST_NETWORK_H
#define GOLEM_GUEST_NETWORK_H
#include <stdint.h>

/* Xgolemnet v1: RV64 CUSTOM_1, funct3 = operation, funct7 = try flag.
 * Descriptor pointers and receive pointers are CPU-visible local SPM addresses.
 * Payload source addresses identify SPM on compute tiles and the configured
 * DRAM region on DRAM Tiles; DRAM payloads need not be CPU-mapped.
 * Positive results are tickets/tokens; successful release/wait return 0.
 * Descriptor records are little endian and 8-byte aligned. */
enum GolemNetOperation {
    GOLEM_NET_SEND = 0, GOLEM_NET_RECV = 1, GOLEM_NET_INFO = 2,
    GOLEM_NET_RELEASE = 3, GOLEM_NET_WAIT = 4,
    GOLEM_NET_TRY_RECV = 257, GOLEM_NET_TRY_WAIT = 260
};
enum GolemNetStatus {
    GOLEM_NET_OK = 0, GOLEM_NET_WOULD_BLOCK = -1, GOLEM_NET_INVALID = -2,
    GOLEM_NET_STALE = -3, GOLEM_NET_RANGE = -4, GOLEM_NET_BANK = -5,
    GOLEM_NET_PENDING = -6, GOLEM_NET_EMPTY = -7, GOLEM_NET_UNAVAILABLE = -8
};
enum GolemNetField {
    GOLEM_NET_POINTER = 0, GOLEM_NET_LENGTH = 1, GOLEM_NET_SEQUENCE = 2,
    GOLEM_NET_TRANSFER_ID = 3, GOLEM_NET_TAG = GOLEM_NET_TRANSFER_ID,
    GOLEM_NET_SOURCE = 4, GOLEM_NET_INVOCATION_ID = 5
};
typedef struct GolemNetDescriptor {
    uint64_t source_address, bytes, transfer_id, invocation_id;
} GolemNetDescriptor;
#if defined(__cplusplus)
static_assert(sizeof(GolemNetDescriptor) == 32,
              "network records must match the wire ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(GolemNetDescriptor) == 32,
               "network records must match the wire ABI");
#endif

#if defined(__riscv) && __riscv_xlen == 64
/* .insn works with both the existing LLVM and GNU assemblers. No compiler
 * connection scans or payload loops are hidden in these wrappers. */
#define GOLEM_NET_WRAPPER(name, operation, variant) \
    static inline int64_t name(uint64_t first, uint64_t second) { \
        int64_t result; \
        __asm__ volatile (".insn r 0x2b, " #operation ", " #variant ", %0, %1, %2" \
                          : "=r"(result) : "r"(first), "r"(second) : "memory"); \
        return result; \
    }
GOLEM_NET_WRAPPER(golem_net_send_raw, 0, 0)
GOLEM_NET_WRAPPER(golem_net_info, 2, 0)
GOLEM_NET_WRAPPER(golem_net_release_raw, 3, 0)
GOLEM_NET_WRAPPER(golem_net_wait_raw, 4, 0)
GOLEM_NET_WRAPPER(golem_net_try_wait_raw, 4, 1)
#undef GOLEM_NET_WRAPPER
static inline int64_t golem_net_send(uint64_t destination_tile, const GolemNetDescriptor *descriptor) {
    return golem_net_send_raw(destination_tile, (uintptr_t)descriptor);
}
static inline int64_t golem_net_recv(void) {
    int64_t result;
    __asm__ volatile (".insn r 0x2b,1,0,%0,zero,zero" : "=r"(result) :: "memory");
    return result;
}
static inline int64_t golem_net_try_recv(void) {
    int64_t result;
    __asm__ volatile (".insn r 0x2b,1,1,%0,zero,zero" : "=r"(result) :: "memory");
    return result;
}
static inline int64_t golem_net_release(uint64_t token) { return golem_net_release_raw(token, 0); }
static inline int64_t golem_net_wait(uint64_t ticket) { return golem_net_wait_raw(ticket, 0); }
static inline int64_t golem_net_try_wait(uint64_t ticket) { return golem_net_try_wait_raw(ticket, 0); }
#endif
#endif
