#include <stdint.h>
#include <stddef.h>
#include "../../components/mordred/guestNetwork.h"

#define REPORT ((volatile uint64_t*)UINT64_C(0x900e0000))
#define OUTPUT ((float*)UINT64_C(0x900a0000))
#define DRAM_BASE UINT64_C(0x100000000)
#define RX_BASE UINT64_C(0x90060000)
#define CHUNK_BYTES 512
#define WEIGHT_BYTES 4096
static GolemNetDescriptor descriptor;
static float input[32];
static void check(int condition, unsigned code) {
    if (condition) return;
    *(volatile uint32_t*)UINT64_C(0x100000)=(code<<16)|0x3333;
    for (;;) __asm__ volatile("nop");
}
static void program(const float* weights, size_t offset) {
    for (size_t n=0; n<CHUNK_BYTES/4;) {
        size_t vl;
        __asm__ volatile("vsetvli %0,%4,e32,m1,tu,mu\n"
                         "vle32.v v8,(%1)\n"
                         "mvm.vset v8,%2,%3"
            : "=&r"(vl) : "r"(weights+n), "r"(UINT64_C(0)),
              "r"(offset+n), "r"(CHUNK_BYTES/4-n) : "v8", "memory");
        n+=vl;
    }
}
static void compute(void) {
    for (unsigned i=0;i<32;++i) input[i]=(float)(i+1)*0.5f;
    for (size_t n=0;n<32;) {
        size_t vl;
        __asm__ volatile("vsetvli %0,%4,e32,m1,tu,mu\n"
                         "vle32.v v8,(%1)\n"
                         "mvm.vl v8,%2,%3"
            : "=&r"(vl) : "r"(input+n), "r"(UINT64_C(0)), "r"(n), "r"(32-n) : "v8", "memory");
        n+=vl;
    }
    uint64_t status;
    __asm__ volatile("mvm %0,%1,%1" : "=r"(status) : "r"(UINT64_C(0)) : "memory");
    check(status==0,50);
    for (size_t n=0;n<32;) {
        size_t vl;
        __asm__ volatile("vsetvli %0,%4,e32,m1,tu,mu\n"
                         "mvm.vs v8,%2,%3\n"
                         "vse32.v v8,(%1)"
            : "=&r"(vl) : "r"(OUTPUT+n), "r"(UINT64_C(0)), "r"(n), "r"(32-n) : "v8", "memory");
        n+=vl;
    }
    __asm__ volatile("fence rw,rw" ::: "memory");
}
int main(void) {
    if (TILE==0) {
        // Descriptors remain in control SPM, even on the DRAM Tile.
        descriptor=(GolemNetDescriptor){DRAM_BASE+65536+SOURCE_SKEW,CHUNK_BYTES,101,0};
        check(golem_net_send_raw(1,DRAM_BASE)==GOLEM_NET_RANGE,1);
        descriptor.source_address=DRAM_BASE-1;
        check(golem_net_send(1,&descriptor)==GOLEM_NET_RANGE,2);
        descriptor.source_address=DRAM_BASE+DRAM_CAPACITY-256;
        check(golem_net_send(1,&descriptor)==GOLEM_NET_RANGE,3);
        descriptor.source_address=RX_BASE;
        check(golem_net_send(1,&descriptor)==GOLEM_NET_RANGE,4);
        descriptor.source_address=DRAM_BASE+65536+SOURCE_SKEW;
        descriptor.bytes=0;
        check(golem_net_send(1,&descriptor)==GOLEM_NET_RANGE,5);
        uint64_t tickets[4]={0};
        unsigned next=0;
        for (unsigned chunk=0;chunk<WEIGHT_BYTES/CHUNK_BYTES;++chunk) {
            for (unsigned destination=1;destination<=3;destination+=2) {
                if (tickets[next]) { check(golem_net_wait(tickets[next])==0,6); tickets[next]=0; }
                descriptor=(GolemNetDescriptor){DRAM_BASE+destination*65536+SOURCE_SKEW+chunk*CHUNK_BYTES,
                                                CHUNK_BYTES,100+destination,chunk};
                int64_t ticket;
                do { ticket=golem_net_send(destination,&descriptor); } while (ticket==GOLEM_NET_WOULD_BLOCK);
                check(ticket>0,7);
                tickets[next]=(uint64_t)ticket;
                next=(next+1)%4;
                // Successful submission must capture the descriptor immediately.
                descriptor=(GolemNetDescriptor){UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX};
            }
        }
        for (unsigned i=0;i<4;++i) if (tickets[i]) check(golem_net_wait(tickets[i])==0,8);
        REPORT[1]=16;
    } else if (TILE==1 || TILE==3) {
        for (unsigned chunk=0;chunk<WEIGHT_BYTES/CHUNK_BYTES;++chunk) {
            int64_t token=golem_net_recv(); check(token>0,10);
            check(golem_net_info(token,GOLEM_NET_TRANSFER_ID)==100+TILE,11);
            check(golem_net_info(token,GOLEM_NET_INVOCATION_ID)==chunk,12);
            check(golem_net_info(token,GOLEM_NET_SEQUENCE)==chunk+1,13);
            check(golem_net_info(token,GOLEM_NET_LENGTH)==CHUNK_BYTES,14);
            check(golem_net_info(token,GOLEM_NET_SOURCE)==0,15);
            const int64_t pointer=golem_net_info(token,GOLEM_NET_POINTER);
            check(pointer>=RX_BASE && pointer<RX_BASE+CHUNK_BYTES*RECEIVE_SLOTS,16);
            if (TILE==3 && chunk==0)
                for (unsigned delay=0;delay<DELAY;++delay) __asm__ volatile("nop");
            program((const float*)(uintptr_t)pointer,chunk*CHUNK_BYTES/4);
            __asm__ volatile("fence rw,rw" ::: "memory");
            check(golem_net_release(token)==0,17);
            check(golem_net_info(token,GOLEM_NET_POINTER)==GOLEM_NET_STALE,18);
        }
        compute();
        REPORT[1]=8;
    }
    REPORT[0]=UINT64_C(0x4452414d4f4b);
    return 0;
}
