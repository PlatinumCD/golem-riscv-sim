#include <stdint.h>
#include "layout.h"
#include <stddef.h>
#include "../../components/mordred/guestNetwork.h"
#include "payload.h"
#include "deployment.h"

static const float weights[1024] = {
#define DIAGONAL(n) [33*(n)] = TILE ? 2.0f : 1.0f
    DIAGONAL(0), DIAGONAL(1), DIAGONAL(2), DIAGONAL(3), DIAGONAL(4), DIAGONAL(5),
    DIAGONAL(6), DIAGONAL(7), DIAGONAL(8), DIAGONAL(9), DIAGONAL(10), DIAGONAL(11),
    DIAGONAL(12), DIAGONAL(13), DIAGONAL(14), DIAGONAL(15), DIAGONAL(16), DIAGONAL(17),
    DIAGONAL(18), DIAGONAL(19), DIAGONAL(20), DIAGONAL(21), DIAGONAL(22), DIAGONAL(23),
    DIAGONAL(24), DIAGONAL(25), DIAGONAL(26), DIAGONAL(27), DIAGONAL(28), DIAGONAL(29),
    DIAGONAL(30), DIAGONAL(31)
#undef DIAGONAL
};
static GolemNetDescriptor descriptor={UINT64_C(0x90050000),128,TRANSFER_A,INVOCATION_FIRST};
static void check(int condition, unsigned code) {
    if (condition) return;
    *(volatile uint32_t*)UINT64_C(0x100000)=(code<<16)|0x3333;
    for (;;) __asm__ volatile("nop");
}
static void program(uint64_t array) {
    for (size_t offset=0; offset<1024;) {
        size_t vl;
        __asm__ volatile("vsetvli %0,%4,e32,m1,tu,mu\n"
                         "vle32.v v8,(%1)\n"
                         "mvm.vset v8,%2,%3"
            : "=&r"(vl) : "r"(weights+offset),"r"(array),"r"(offset),"r"(1024-offset) : "v8","memory");
        offset+=vl;
    }
}
static void load(uint64_t array, const void* input) {
    for (size_t offset=0; offset<32;) {
        size_t vl;
        __asm__ volatile("vsetvli %0,%4,e32,m1,tu,mu\n"
                         "vle32.v v8,(%1)\n"
                         "mvm.vl v8,%2,%3"
            : "=&r"(vl) : "r"((const uint32_t*)input+offset),"r"(array),"r"(offset),"r"(32-offset) : "v8","memory");
        offset+=vl;
    }
}
static inline void start(uint64_t array) {
    uint64_t status;
    __asm__ volatile("mvm %0,%1,%1" : "=r"(status) : "r"(array) : "memory");
    check(!status,80);
}
static __attribute__((always_inline)) inline void store(uint64_t array, void* output) {
    for (size_t offset=0; offset<32;) {
        size_t vl;
        __asm__ volatile("vsetvli %0,%4,e32,m1,tu,mu\n"
                         "mvm.vs v8,%2,%3\n"
                         "vse32.v v8,(%1)"
            : "=&r"(vl) : "r"((uint32_t*)output+offset),"r"(array),"r"(offset),"r"(32-offset) : "v8","memory");
        offset+=vl;
    }
}
static int64_t store_and_send(void* output) {
    for (size_t offset=0; offset<32;) {
        size_t vl;
        __asm__ volatile("vsetvli %0,%1,e32,m1,tu,mu" : "=r"(vl) : "r"(32-offset));
        if (offset+vl==32) {
            int64_t ticket;
            // Issue send immediately after the last asynchronous RVV store.
            // Keep loop bookkeeping ahead of it so the release-order check
            // really sees a pending store, independent of compiler layout.
            __asm__ volatile("mvm.vs v8,%2,%3\n"
                             "vse32.v v8,(%1)\n"
                             ".insn r 0x2b,0,0,%0,%4,%5"
                : "=r"(ticket) : "r"((uint32_t*)output+offset),"r"(UINT64_C(0)),
                  "r"(offset),"r"((uint64_t)DESTINATION),"r"(&descriptor) : "v8","memory");
            return ticket;
        }
        __asm__ volatile("mvm.vs v8,%1,%2\n"
                         "vse32.v v8,(%0)"
            :: "r"((uint32_t*)output+offset),"r"(UINT64_C(0)),"r"(offset) : "v8","memory");
        offset+=vl;
    }
    return GOLEM_NET_INVALID;
}
int main(void) {
    *SPM_LAST_WORD=0x53504d00+TILE;
    if (TILE!=SENDER && TILE!=DESTINATION) {
        REPORT[0]=UINT64_C(0x4e45544f4b); REPORT[1]=4; REPORT[2]=128; return 0;
    }
    program(0); if (BACKGROUND_ARRAY) program(1);
    if (TILE==SENDER) {
        load(0,source_a); start(0);
        // No fence: net.send itself must observe the queued RVV output stores.
        int64_t ticket=store_and_send((void*)UINT64_C(0x90050000)); check(ticket>0,82);
        if (BACKGROUND_ARRAY) { load(1,source_b); start(1); }
        check(golem_net_wait(ticket)==0,83);
        // Safe source reuse while the unrelated array continues computing.
        for (unsigned i=0; i<32; ++i) ((volatile uint32_t*)UINT64_C(0x90050000))[i]=0xffffffff;
        if (BACKGROUND_ARRAY) store(1,(void*)UINT64_C(0x90051000));
    } else {
        if (BACKGROUND_ARRAY) { load(1,source_b); start(1); }
        int64_t token=golem_net_recv(); check(token>0,84);
        check(golem_net_info(token,GOLEM_NET_TRANSFER_ID)==TRANSFER_A,86);
        check(golem_net_info(token,GOLEM_NET_INVOCATION_ID)==INVOCATION_FIRST,87);
        const void* input=(const void*)(uintptr_t)golem_net_info(token,GOLEM_NET_POINTER);
        load(0,input);
        check(golem_net_release(token)==0,85);
        start(0); store(0,(void*)OUTPUT);
        if (BACKGROUND_ARRAY) store(1,(void*)UINT64_C(0x90051000));
    }
    REPORT[0]=UINT64_C(0x4e45544f4b); REPORT[1]=4; REPORT[2]=128;
    return 0;
}
