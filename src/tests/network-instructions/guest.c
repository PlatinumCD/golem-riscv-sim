#include <stdint.h>
#include "layout.h"
#include <stddef.h>
#include "../../components/mordred/guestNetwork.h"
#include "payload.h"
#include "deployment.h"

#define RX_BASE(id) ((id)==TRANSFER_A ? RX_A : RX_B)
#if CHECK_BANKS
/* Keep the descriptor inside the two shared 32-byte bank stripes. */
static GolemNetDescriptor descriptor __attribute__((aligned(128)));
#else
static GolemNetDescriptor descriptor;
#endif
static void fail(unsigned code) {
    *(volatile uint32_t*)UINT64_C(0x100000)=(code<<16)|0x3333;
    for (;;) __asm__ volatile("nop");
}
#define CHECK(condition, code) do { if (!(condition)) fail(code); } while (0)
static void delay(unsigned n) {
    __asm__ volatile("1: addi %0,%0,-1; bnez %0,1b" : "+r"(n) :: "memory");
}
static int64_t send_bytes(unsigned destination, const uint32_t* source, uint64_t transfer,
                          uint64_t invocation, unsigned bytes) {
    descriptor=(GolemNetDescriptor){(uintptr_t)source,bytes,transfer,invocation};
    const int64_t result=golem_net_send(destination,&descriptor);
    /* Successful submission must retain all four words privately. */
    descriptor=(GolemNetDescriptor){0,0,UINT64_MAX,UINT64_MAX};
    return result;
}
static int64_t send(unsigned destination, const uint32_t* source, uint64_t transfer, uint64_t invocation) {
    return send_bytes(destination,source,transfer,invocation,MESSAGE_BYTES);
}
static void copy_vector(const uint32_t* source, uint32_t* destination, unsigned bytes) {
    size_t left=bytes/4;
    while (left) {
        size_t vl;
        __asm__ volatile("vsetvli %0,%3,e32,m1,tu,mu\n"
                         "vle32.v v8,(%1)\n"
                         "vse32.v v8,(%2)"
            : "=&r"(vl) : "r"(source),"r"(destination),"r"(left) : "v8","memory");
        source+=vl; destination+=vl; left-=vl;
    }
    /* The application must finish all asynchronous readers before release. */
    __asm__ volatile("fence rw,rw" ::: "memory");
}
static int64_t receive_bytes(uint64_t transfer, unsigned sequence, uint64_t invocation,
                             unsigned source, unsigned output, unsigned bytes) {
    int64_t token=TRY_RECEIVE ? golem_net_try_recv() : golem_net_recv();
    CHECK(token>0,30);
    CHECK(golem_net_info(token,GOLEM_NET_LENGTH)==bytes,31);
    CHECK(golem_net_info(token,GOLEM_NET_SEQUENCE)==sequence,32);
    CHECK(golem_net_info(token,GOLEM_NET_TRANSFER_ID)==transfer,33);
    CHECK(golem_net_info(token,GOLEM_NET_SOURCE)==source,34);
    CHECK(golem_net_info(token,GOLEM_NET_INVOCATION_ID)==invocation,39);
    int64_t address=golem_net_info(token,GOLEM_NET_POINTER);
    CHECK(address>=RX_BASE(transfer) && address+bytes<=RX_BASE(transfer)+RECEIVE_SLOTS*MESSAGE_BYTES,35);
    CHECK(golem_net_info(token,GOLEM_NET_POINTER)==address,36);
    CHECK(golem_net_wait(token)==GOLEM_NET_STALE,37);
    CHECK(golem_net_info(token,99)==GOLEM_NET_INVALID,38);
    // This copy is application verification, never part of message transport.
    copy_vector((const uint32_t*)(uintptr_t)address,(uint32_t*)(OUTPUT+output*65536),bytes);
    REPORT[4+output]=token;
    return token;
}
static int64_t receive(uint64_t transfer, unsigned sequence, uint64_t invocation, unsigned source, unsigned output) {
    return receive_bytes(transfer,sequence,invocation,source,output,MESSAGE_BYTES);
}
static void release(uint64_t token) {
    CHECK(golem_net_release(token)==0,40);
    CHECK(golem_net_release(token)==GOLEM_NET_STALE,41);
    CHECK(golem_net_info(token,GOLEM_NET_POINTER)==GOLEM_NET_STALE,42);
}
static void invalid_sends(unsigned destination, uint64_t transfer) {
#if CHECK_BANKS
    CHECK(golem_net_send(destination,(const GolemNetDescriptor*)UINT64_C(0x90010040))==GOLEM_NET_BANK,98);
    descriptor=(GolemNetDescriptor){(uintptr_t)source_a+64,MESSAGE_BYTES,transfer,INVOCATION_FIRST};
    CHECK(golem_net_send(destination,&descriptor)==GOLEM_NET_BANK,99);
    /* Reject a range starting in an allowed bank but crossing into bank 2. */
    descriptor.source_address=(uintptr_t)source_a+32;
    CHECK(golem_net_send(destination,&descriptor)==GOLEM_NET_BANK,100);
#endif
    CHECK(golem_net_wait(42)==GOLEM_NET_STALE,50);
    CHECK(golem_net_try_recv()==GOLEM_NET_EMPTY,45);
    CHECK(golem_net_send(UINT64_MAX,&descriptor)==GOLEM_NET_INVALID,51);
    CHECK(golem_net_send(TILE,&descriptor)==GOLEM_NET_INVALID,46);
    descriptor=(GolemNetDescriptor){0,MESSAGE_BYTES,transfer,INVOCATION_FIRST};
    CHECK(golem_net_send(destination,&descriptor)==GOLEM_NET_RANGE,52);
    descriptor=(GolemNetDescriptor){(uintptr_t)source_a,MESSAGE_BYTES,UINT64_C(1)<<63,0};
    CHECK(golem_net_send(destination,&descriptor)==GOLEM_NET_INVALID,90);
    descriptor.transfer_id=transfer; descriptor.invocation_id=UINT64_C(1)<<63;
    CHECK(golem_net_send(destination,&descriptor)==GOLEM_NET_INVALID,91);
    descriptor.invocation_id=0; descriptor.transfer_id=UNCONFIGURED_TRANSFER;
    CHECK(golem_net_send(destination,&descriptor)==GOLEM_NET_INVALID,92);
    descriptor.transfer_id=transfer; descriptor.bytes=MESSAGE_BYTES+1;
    CHECK(golem_net_send(destination,&descriptor)==GOLEM_NET_INVALID,93);
    if (TILE_COUNT>2) {
        descriptor.bytes=MESSAGE_BYTES;
        CHECK(golem_net_send((destination+1)%TILE_COUNT==TILE ? (TILE+1)%TILE_COUNT :
                             (destination+1)%TILE_COUNT,&descriptor)==GOLEM_NET_INVALID,94);
    }
}
int main(void) {
    *SPM_LAST_WORD=0x53504d00+TILE;
    REPORT[0]=0; REPORT[1]=CASE; REPORT[2]=MESSAGE_BYTES;
    if (CASE==8) {
        if (TILE==0) {
            invalid_sends(DESTINATION,TRANSFER_A); delay(8000);
            int64_t a=send(DESTINATION,source_a,TRANSFER_A,INVOCATION_FIRST); CHECK(a>0,53);
            CHECK(golem_net_wait(a)==0,56);
        } else if (TILE==1) {
            invalid_sends(DESTINATION,TRANSFER_B);
            // Transfer A belongs to a different producer, even at the same receiver.
            descriptor=(GolemNetDescriptor){(uintptr_t)source_a,MESSAGE_BYTES,TRANSFER_A,INVOCATION_FIRST};
            CHECK(golem_net_send(DESTINATION,&descriptor)==GOLEM_NET_INVALID,95);
            int64_t b=send(DESTINATION,source_b,TRANSFER_B,INVOCATION_FIRST); CHECK(b>0,53);
            CHECK(golem_net_wait(b)==0,56);
            int64_t next=send(DESTINATION,source_c,TRANSFER_B,INVOCATION_SECOND); CHECK(next>0,58);
            CHECK(golem_net_try_wait(next)==GOLEM_NET_PENDING,59);
            CHECK(golem_net_wait(next)==0,62);
        } else if (TILE==DESTINATION) {
            int64_t b=receive(TRANSFER_B,1,INVOCATION_FIRST,1,1);
            int64_t pointer=golem_net_info(b,GOLEM_NET_POINTER);
            // A can arrive while B owns its only slot and B's next send waits.
            int64_t a=receive(TRANSFER_A,1,INVOCATION_FIRST,0,0);
            CHECK(golem_net_info(b,GOLEM_NET_POINTER)==pointer,96);
            CHECK(golem_net_info(b,GOLEM_NET_INVOCATION_ID)==INVOCATION_FIRST,97);
            copy_vector((const uint32_t*)(uintptr_t)pointer,(uint32_t*)(OUTPUT+3*65536),MESSAGE_BYTES);
            release(a); release(b);
            int64_t next=receive(TRANSFER_B,2,INVOCATION_SECOND,1,2); release(next);
        }
    } else if (CASE==6) {
        uint64_t outgoing=TILE==SENDER ? TRANSFER_A : TRANSFER_B;
        uint64_t incoming=TILE==SENDER ? TRANSFER_B : TRANSFER_A;
        unsigned other=TILE==SENDER ? DESTINATION : SENDER;
        int64_t ticket=send(other,TILE==SENDER ? source_a : source_b,outgoing,INVOCATION_FIRST); CHECK(ticket>0,47);
        CHECK(golem_net_wait(ticket)==0,48);
        int64_t token=receive(incoming,1,INVOCATION_FIRST,other,0); release(token);
    } else if (TILE==SENDER) {
        invalid_sends(DESTINATION,TRANSFER_A);
        int64_t a=send(DESTINATION,source_a,TRANSFER_A,INVOCATION_FIRST); CHECK(a>0,53);
        CHECK(golem_net_release(a)==GOLEM_NET_STALE,66);
        if (CASE==5) {
            int64_t second=send_bytes(DESTINATION,source_b,TRANSFER_A,INVOCATION_SECOND,64); CHECK(second>0,67);
            CHECK(golem_net_wait(second)==0,68);
        }
        if (CASE==2) {
            delay(4000); // Retained completion consumes a ticket but no command.
            CHECK(send(DESTINATION,source_b,TRANSFER_A,INVOCATION_SECOND)==GOLEM_NET_WOULD_BLOCK,54);
        }
        if (CASE==3) CHECK(send(DESTINATION,source_b,TRANSFER_A,INVOCATION_SECOND)==GOLEM_NET_WOULD_BLOCK,55);
        CHECK((CASE==2 ? golem_net_try_wait(a) : golem_net_wait(a))==0,56);
        CHECK(golem_net_wait(a)==GOLEM_NET_STALE,57);
        if (REUSE_SOURCE) {
            for (unsigned i=0; i<MESSAGE_BYTES/4; ++i) ((volatile uint32_t*)source_a)[i]=UINT32_MAX;
            __asm__ volatile("fence rw,rw" ::: "memory");
            REPORT[3]=1;
        }
        if (CASE==1) {
            int64_t b=send(DESTINATION,source_b,TRANSFER_A,INVOCATION_SECOND); CHECK(b>0,58);
            CHECK(golem_net_try_wait(b)==GOLEM_NET_PENDING,59);
            int64_t c=send(DESTINATION,source_c,TRANSFER_B,INVOCATION_FIRST); CHECK(c>0,60);
            CHECK(golem_net_wait(c)==0,61);
            CHECK(golem_net_wait(b)==0,62);
        } else if (CASE!=5) {
            int64_t b=send(DESTINATION,source_b,TRANSFER_A,INVOCATION_SECOND); CHECK(b>0 && b!=a,63);
            CHECK(golem_net_wait(a)==GOLEM_NET_STALE,64);
            CHECK(golem_net_wait(b)==0,65);
        }
    } else if (TILE==DESTINATION) {
        if (CASE==5) delay(500);
        if (DELAY) delay(DELAY);
        if (CASE==1) {
            int64_t a=receive(TRANSFER_A,1,INVOCATION_FIRST,SENDER,0);
            int64_t c=receive(TRANSFER_B,1,INVOCATION_FIRST,SENDER,2);
            release(c); delay(1000); release(a);
            int64_t b=receive(TRANSFER_A,2,INVOCATION_SECOND,SENDER,1); CHECK(a!=b,71); release(b);
        } else if (CASE==5) {
            int64_t a=receive(TRANSFER_A,1,INVOCATION_FIRST,SENDER,0); release(a);
            int64_t b=receive_bytes(TRANSFER_A,2,INVOCATION_SECOND,SENDER,1,64); CHECK(a!=b,74); release(b);
        } else {
            int64_t a=receive(TRANSFER_A,1,INVOCATION_FIRST,SENDER,0); release(a);
            int64_t b=receive(TRANSFER_A,2,INVOCATION_SECOND,SENDER,1); CHECK(a!=b,72); release(b);
        }
        CHECK(golem_net_try_recv()==GOLEM_NET_EMPTY,73);
    }
    REPORT[0]=UINT64_C(0x4e45544f4b);
    return 0;
}
