#include "mesh-nic.h"
#include "platform.h"
#include "scratchpad-dma.h"
#include <stdint.h>
extern "C" uint64_t kernel(uint64_t);
extern "C" char __buffer_start[], __buffer_end[], kernel_end[];
namespace {
constexpr uint32_t DataBytes=65548, Guard=0xfaceb00c, Poison=0xdeadc0de;
constexpr uint64_t Input=0x100000, Output=0x200000;
[[noreturn]] void fail() { uart_puts("CODE_DATA_FAIL\n"); platform_exit(1); }
uint32_t value(uint32_t i) { return i ^ 0x13570000U; }
void mark(uint32_t event,uint32_t id,uint32_t iteration=0) {
    mesh_nic::trace_task(event,id,iteration);
}
}
extern "C" int tile_main() {
    using namespace golem::platform;
    auto* data=reinterpret_cast<volatile uint32_t*>(__buffer_start);
    const auto bytes=reinterpret_cast<uintptr_t>(__buffer_end)-reinterpret_cast<uintptr_t>(__buffer_start);
    const auto adds=(reinterpret_cast<uintptr_t>(kernel_end)-reinterpret_cast<uintptr_t>(kernel))/4-1;
    const auto spm_offset=reinterpret_cast<uintptr_t>(data)-ScratchpadBase;
    if (!bytes || bytes%32) fail();
    for (uint32_t i=0;i<8;++i) { data[-8+static_cast<int>(i)]=Guard; data[bytes/4+i]=Guard; }
    auto submit=[&](uint64_t address,uint32_t count,uint32_t phase,uint32_t iteration,
                    ScratchpadDMADirection direction) {
        if(!globalDMASubmit(address,spm_offset,count,1,phase,iteration,direction)) fail();
    };
    for(uint32_t phase=1;phase<=3;++phase) {
        mark(mesh_nic::kTaskTraceStart,phase);
        uint32_t iteration=0;
        for(uint32_t offset=0;offset<DataBytes;offset+=bytes,++iteration) {
            const uint32_t count=DataBytes-offset<bytes ? DataBytes-offset : bytes;
            for(uint32_t i=0;i<bytes/4;++i) data[i]=Poison;
            if(phase==1) {
                for(uint32_t i=0;i<count/4;++i) data[i]=value(offset/4+i);
                submit(Input+offset,count,phase,iteration,ScratchpadDMADirection::ScratchpadToGlobalRAM);
                if(!globalDMAWait(phase,1)) fail();
            } else {
                submit((phase==2 ? Input : Output)+offset,count,phase,iteration,
                       ScratchpadDMADirection::GlobalRAMToScratchpad);
                uint64_t salt=adds+7;
                if(phase==2) {
                    // Execute real arithmetic after submission and before wait.
                    // Traces determine whether DMA service actually overlaps.
                    for(uint32_t pass=0;pass<3;++pass) {
                        mark(mesh_nic::kTaskTraceStart,10+pass,iteration);
                        salt=kernel(7);
                        mark(mesh_nic::kTaskTraceFinish,10+pass,iteration);
                        if(salt!=adds+7) fail();
                    }
                }
                if(!globalDMAWait(phase,1)) fail();
                for(uint32_t i=0;i<count/4;++i) {
                    const uint32_t expected=value(offset/4+i);
                    if(data[i]!=(phase==2 ? expected : expected+salt)) fail();
                    if(phase==2) data[i]+=salt;
                }
                if(phase==2) {
                    submit(Output+offset,count,phase,iteration,ScratchpadDMADirection::ScratchpadToGlobalRAM);
                    if(!globalDMAWait(phase,1)) fail();
                }
            }
            for(uint32_t i=0;i<8;++i)
                if(data[-8+static_cast<int>(i)]!=Guard || data[bytes/4+i]!=Guard) fail();
            for(uint32_t i=count/4;i<bytes/4;++i) if(data[i]!=Poison) fail();
        }
        mark(mesh_nic::kTaskTraceFinish,phase);
    }
    uart_puts("CODE_DATA_PASS\n");
    return 0;
}
