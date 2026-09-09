#include "config.h"
#include "mesh-nic.h"
#include "platform.h"
#include <stdint.h>

namespace {
constexpr uint32_t tile = TILE_ID;
constexpr uint32_t magic = 0x474f4c4d;
constexpr uintptr_t spm = 0x90000000;
constexpr uint32_t segments = (PayloadBytes + 16383) / 16384;
constexpr uint32_t messages = segments * Waves;
constexpr uint32_t stride = 16640;
constexpr uint32_t traceRun = 100;
constexpr uint32_t traceCompute = 200;

uint32_t wordsFor(uint32_t sequence) {
    uint32_t segment = sequence % segments;
    uint32_t bytes = PayloadBytes - segment * 16384;
    return (bytes > 16384 ? 16384 : bytes) / 4;
}
uint32_t pattern(uint32_t flow, uint32_t word) {
    return 0xac000000U ^ (flow << 16) ^ word;
}
volatile uint32_t* buffer(uint32_t flow, bool receive) {
    return reinterpret_cast<volatile uint32_t*>(spm + (receive ? 262144 : 4096)
        + flow * stride + (BankPhase ? flow * 32 : 0));
}
void delay(uint32_t count) {
    if (!count) return;
    asm volatile("1: addi %0, %0, -1\n\tbnez %0, 1b" : "+r"(count));
}
void trace(uint32_t event, uint32_t id, uint32_t sequence) {
    mesh_nic::trace_task(event, id, sequence);
}
[[noreturn]] void fail(const char* reason) {
    uart_puts("ENVELOPE_FAIL "); uart_puts(reason); uart_puts("\n"); platform_exit(1);
}
}

extern "C" int tile_main() {
    uint32_t sent[FlowCount + 1] = {}, received[FlowCount + 1] = {};
    bool txHeader[FlowCount + 1] = {}, inFlight[FlowCount + 1] = {};
    bool offered[FlowCount + 1] = {};
    bool announced[FlowCount + 1][messages] = {};
    int txOwner = -1;
    uint32_t header[TileCount][7] = {}, headerCount[TileCount] = {};
    uint32_t expectedSend = 0, expectedReceive = 0, totalSent = 0, totalReceived = 0;
    for (uint32_t f = 0; f < FlowCount; ++f) {
        if (Sources[f] == tile) {
            expectedSend += messages;
            for (uint32_t w = 0; w < 4096; ++w) buffer(f, false)[w] = pattern(f,w);
        }
        if (Destinations[f] == tile) {
            expectedReceive += messages;
            for (uint32_t w = 0; w < 4096; ++w) buffer(f, true)[w] = 0;
        }
    }
    for (uint32_t w = 0; w < 8; ++w) reinterpret_cast<volatile uint32_t*>(spm)[w] = w;
    mesh_nic::complete_memory_initialization();
    trace(mesh_nic::kTaskTraceStart, traceRun, 0);
    if (static_cast<int>(tile) == DelayTile) delay(DelayInstructions);
    uint32_t iterations = static_cast<int>(tile) == RVVTile ? RVVIterations : 0;
    uint32_t computed = 0;
    if (iterations) {
        asm volatile("vsetivli zero, 8, e32, m1, ta, ma\n\tvmv.v.i v9, 0" ::: "v8", "v9", "memory");
        trace(mesh_nic::kTaskTraceStart, traceCompute, 0);
    }
    while (totalSent < expectedSend || totalReceived < expectedReceive || computed < iterations) {
        // Service RX and TX cooperatively: no blocking send can prevent a tile
        // from consuming incoming data while it owns outgoing transfers.
        uint32_t source, route;
        uint64_t sequence;
        if (mesh_nic::try_receive_words_completion(&source, &route, &sequence)) {
            if (route < 1000 || route >= 1000 + FlowCount) fail("completion route");
            uint32_t f = route - 1000;
            if (source != Sources[f] || !inFlight[f] || sequence != received[f]) fail("completion identity/order");
            const auto count = wordsFor(received[f]);
            if (buffer(f,true)[0] != pattern(f,0) || buffer(f,true)[count-1] != pattern(f,count-1)) fail("payload boundary");
            trace(mesh_nic::kTaskTraceFinish, 2000+f, received[f]);
            ++received[f]; ++totalReceived; inFlight[f] = false;
        }
        uint32_t word;
        if (mesh_nic::try_receive_from(&source, &word)) {
            if (source >= TileCount) fail("source range");
            auto& count = headerCount[source];
            header[source][count++] = word;
            if (count == 7) {
                uint32_t* h = header[source];
                if (h[0] != magic || h[1] < 1000 || h[1] >= 1000+FlowCount) fail("frame header");
                uint32_t f = h[1]-1000;
                if (Sources[f] != source || Destinations[f] != tile || h[2] || h[3] || h[5] ||
                    h[4] >= messages || h[6] != wordsFor(h[4]) || announced[f][h[4]]) fail("frame identity");
                announced[f][h[4]] = true; count = 0;
                trace(mesh_nic::kTaskTraceStart, 2000+f, h[4]);
            }
        }
        for (uint32_t f = 0; f < FlowCount; ++f) {
            if (received[f] < messages && announced[f][received[f]] && !inFlight[f] &&
                mesh_nic::try_start_receive_words(Sources[f],1000+f,received[f],
                    const_cast<uint32_t*>(buffer(f,true)),wordsFor(received[f]))) {
                inFlight[f] = true;
            }
            if (Sources[f] != tile || sent[f] == messages) continue;
            if (txOwner >= 0 && txOwner != static_cast<int>(f)) continue;
            if (!txHeader[f]) {
                if (!offered[f]) {
                    trace(mesh_nic::kTaskTraceStart,1000+f,sent[f]); offered[f] = true;
                }
                const uint32_t h[] = {magic,1000+f,0,0,sent[f],0,wordsFor(sent[f])};
                if (mesh_nic::try_send_words(Destinations[f],h,7)) {
                    txHeader[f] = true;
                    txOwner = static_cast<int>(f);
                }
            }
            // Header and payload must be adjacent in the per-source protocol
            // stream. Do not issue another frame header while this one waits.
            if (txHeader[f]) {
                if (mesh_nic::try_send_words(Destinations[f],
                    const_cast<const uint32_t*>(buffer(f,false)),wordsFor(sent[f]))) {
                    trace(mesh_nic::kTaskTraceFinish,1000+f,sent[f]);
                    txHeader[f] = false; offered[f] = false; txOwner = -1; ++sent[f]; ++totalSent;
                    delay(GapInstructions);
                } else break;
            } else break;
        }
        if (computed < iterations) {
            for (uint32_t i = 0; i < 8 && computed < iterations; ++i, ++computed) {
                asm volatile("vle32.v v8, (%0)\n\tvadd.vx v9, v9, %1"
                    : : "r"(spm), "r"(1U) : "v8", "v9", "memory");
            }
            if (computed == iterations) {
                trace(mesh_nic::kTaskTraceFinish,traceCompute,0);
                asm volatile("vse32.v v9, (%0)" : : "r"(spm) : "v9", "memory");
            }
        }
        if (computed == iterations && totalSent == expectedSend && totalReceived < expectedReceive)
            mesh_nic::wait_for_receive();
    }
    trace(mesh_nic::kTaskTraceFinish,traceRun,0);
    // Full final-buffer validation is outside the measured region. Every
    // descriptor is separately checked for identity, order and boundary words.
    for (uint32_t f = 0; f < FlowCount; ++f) {
        if (Destinations[f] == tile) {
            for (uint32_t w = 0; w < wordsFor(messages-1); ++w)
                if (buffer(f,true)[w] != pattern(f,w)) fail("final payload");
        }
    }
    if (iterations && reinterpret_cast<volatile uint32_t*>(spm)[0] != iterations) fail("RVV output");
    uart_puts("ENVELOPE_PASS\n");
    return 0;
}
