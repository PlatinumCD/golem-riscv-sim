#include "mesh-nic.h"
#include "platform.h"
#include "scratchpad-dma.h"

#include <stdint.h>

#ifndef MITTENS_TILE_ID
#error "MITTENS_TILE_ID must be supplied"
#endif

#ifndef MITTENS_FANOUT_PATTERN
#define MITTENS_FANOUT_PATTERN 0
#endif

#ifndef MITTENS_FANOUT_BANK_LAYOUT
#define MITTENS_FANOUT_BANK_LAYOUT 0
#endif

#ifndef MITTENS_FANOUT_READY_SKEW_CYCLES
#define MITTENS_FANOUT_READY_SKEW_CYCLES 0
#endif

#ifndef MITTENS_FANOUT_RECORD_READY_CYCLES
#define MITTENS_FANOUT_RECORD_READY_CYCLES 0
#endif

namespace {

// Pattern 0: one source, four independent first-hop directions (N/E/S/W).
// Pattern 1: one source, four destinations behind the same East link.
// Pattern 2: one source, two North and two East destinations.
// Pattern 3: one source, mixed one/two-hop destinations.
// Pattern 4: four sources, each concurrently issuing a two-way fan-out.
// Pattern 5: one source, one North and one East destination.
// Pattern 6: one source, one East destination.
// Pattern 7: one source, three East/North/West destinations.
// Patterns 8-12: fixed-volume first-hop diversity controls.
#if MITTENS_FANOUT_PATTERN == 0
constexpr uint32_t kSources[] = {12, 12, 12, 12};
constexpr uint32_t kDestinations[] = {7, 13, 17, 11};
#elif MITTENS_FANOUT_PATTERN == 1
constexpr uint32_t kSources[] = {10, 10, 10, 10};
constexpr uint32_t kDestinations[] = {11, 12, 13, 14};
#elif MITTENS_FANOUT_PATTERN == 2
constexpr uint32_t kSources[] = {12, 12, 12, 12};
constexpr uint32_t kDestinations[] = {7, 13, 2, 14};
#elif MITTENS_FANOUT_PATTERN == 3
constexpr uint32_t kSources[] = {12, 12, 12, 12};
constexpr uint32_t kDestinations[] = {7, 14, 22, 11};
#elif MITTENS_FANOUT_PATTERN == 4
constexpr uint32_t kSources[] = {6, 6, 8, 8, 16, 16, 18, 18};
constexpr uint32_t kDestinations[] = {1, 5, 3, 9, 21, 15, 23, 19};
#elif MITTENS_FANOUT_PATTERN == 5
constexpr uint32_t kSources[] = {12, 12};
constexpr uint32_t kDestinations[] = {7, 13};
#elif MITTENS_FANOUT_PATTERN == 6
constexpr uint32_t kSources[] = {12};
constexpr uint32_t kDestinations[] = {13};
#elif MITTENS_FANOUT_PATTERN == 7
constexpr uint32_t kSources[] = {12, 12, 12};
constexpr uint32_t kDestinations[] = {13, 7, 11};
#elif MITTENS_FANOUT_PATTERN == 8
constexpr uint32_t kSources[] = {12, 12, 12, 12};
constexpr uint32_t kDestinations[] = {13, 13, 13, 13};
#elif MITTENS_FANOUT_PATTERN == 9
constexpr uint32_t kSources[] = {12, 12, 12, 12};
// Interleave directions so FIFO descriptor submission does not introduce a
// same-direction head-of-line artifact into the load-balance experiment.
constexpr uint32_t kDestinations[] = {13, 7, 13, 13};
#elif MITTENS_FANOUT_PATTERN == 10
constexpr uint32_t kSources[] = {12, 12, 12, 12};
constexpr uint32_t kDestinations[] = {13, 7, 13, 7};
#elif MITTENS_FANOUT_PATTERN == 11
constexpr uint32_t kSources[] = {12, 12, 12, 12};
constexpr uint32_t kDestinations[] = {13, 7, 17, 13};
#elif MITTENS_FANOUT_PATTERN == 12
constexpr uint32_t kSources[] = {12, 12, 12, 12};
constexpr uint32_t kDestinations[] = {13, 7, 17, 11};
#else
#error "MITTENS_FANOUT_PATTERN must be in [0,12]"
#endif

constexpr uint32_t kTransferCount = sizeof(kSources) / sizeof(kSources[0]);
static_assert(kTransferCount ==
              sizeof(kDestinations) / sizeof(kDestinations[0]));
#ifndef MITTENS_FANOUT_PAYLOAD_BYTES
#define MITTENS_FANOUT_PAYLOAD_BYTES (4 * 1024)
#endif
constexpr uint32_t kPayloadBytes = MITTENS_FANOUT_PAYLOAD_BYTES;
constexpr uint32_t kPayloadWords = kPayloadBytes / sizeof(uint32_t);
constexpr uint32_t kHeaderWords = 7;
constexpr uint32_t kFrameMagic = UINT32_C(0x474f4c4d);
constexpr uint32_t kRouteBase = 9300;
constexpr uint32_t kAckBase = UINT32_C(0xfa110000);
constexpr uint32_t kFanoutTaskBase = 9400;
constexpr uint32_t kDestinationTaskBase = 9500;
#if MITTENS_FANOUT_RECORD_READY_CYCLES
constexpr uint64_t kReadySkewCycles = MITTENS_FANOUT_READY_SKEW_CYCLES;

uint64_t readCycle() {
    uint64_t value;
    __asm__ volatile("rdcycle %0" : "=r"(value));
    return value;
}

void waitGuestCycles(uint64_t cycles) {
    const uint64_t start = readCycle();
    while (readCycle() - start < cycles) {
        __asm__ volatile("nop");
    }
}

void uartPutUnsigned(uint64_t value) {
    char digits[21];
    uint32_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0) uart_putc(digits[--count]);
}
#endif

int destinationSlot(uint32_t tile) {
    for (uint32_t index = 0; index < kTransferCount; ++index) {
        if (kDestinations[index] == tile) return static_cast<int>(index);
    }
    return -1;
}

uint32_t destinationTransferCount(uint32_t tile) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < kTransferCount; ++index) {
        if (kDestinations[index] == tile) ++count;
    }
    return count;
}

uint32_t localTransferCount(uint32_t tile) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < kTransferCount; ++index) {
        if (kSources[index] == tile) ++count;
    }
    return count;
}

uint32_t localTransferOrdinal(uint32_t tile, uint32_t transfer) {
    uint32_t ordinal = 0;
    for (uint32_t index = 0; index < transfer; ++index) {
        if (kSources[index] == tile) ++ordinal;
    }
    return ordinal;
}

uint32_t bufferOffsetWords(uint32_t ordinal) {
#if MITTENS_FANOUT_BANK_LAYOUT == 0
    // 4 KiB is an integer multiple of all eight 32-byte banks, so all four
    // concurrent streams start on the same bank and advance in lockstep.
    return ordinal * kPayloadWords;
#elif MITTENS_FANOUT_BANK_LAYOUT == 1
    // Synthetic read-only bank coloring. The overlapping payload windows are
    // intentional: a 16 KiB SPM cannot hold four padded 4 KiB buffers. Their
    // starting beats map to banks 0,1,2,3 and remain separated in lockstep.
    return ordinal * (kPayloadWords / 2 + 8);
#elif MITTENS_FANOUT_BANK_LAYOUT == 2
    // Non-overlapping 4 KiB windows padded by one 32-byte bank beat. Their
    // starting beats map to banks 0,1,2,3 while preserving independent data.
    return ordinal * (kPayloadWords + 8);
#else
#error "MITTENS_FANOUT_BANK_LAYOUT must be 0, 1, or 2"
#endif
}

uint32_t payloadValue(uint32_t transfer, uint32_t word) {
    return UINT32_C(0x6d2b79f5) ^
           (transfer * UINT32_C(0x9e3779b9)) ^ word;
}

void sendBurst(uint32_t destination, const uint32_t* words, uint32_t count) {
    while (!mesh_nic::try_send_words(destination, words, count)) {
        mesh_nic::wait_for_transmit();
    }
}

bool receiveHeader(uint32_t transfer) {
    uint32_t header[kHeaderWords];
    for (uint32_t word = 0; word < kHeaderWords; ++word) {
        uint32_t receivedSource = UINT32_MAX;
        header[word] = mesh_nic::receive_from(&receivedSource);
        if (receivedSource != kSources[transfer]) return false;
    }
    return header[0] == kFrameMagic && header[1] == kRouteBase + transfer &&
           header[2] == 0 && header[3] == 0 && header[4] == 0 &&
           header[5] == 0 && header[6] == kPayloadWords;
}

bool receiveCompletion(uint32_t transfer) {
    while (true) {
        uint32_t source = UINT32_MAX;
        uint32_t route = UINT32_MAX;
        uint64_t iteration = UINT64_MAX;
        if (mesh_nic::try_receive_words_completion(
                &source, &route, &iteration)) {
            return source == kSources[transfer] &&
                   route == kRouteBase + transfer && iteration == 0;
        }
        mesh_nic::wait_for_receive();
    }
}

}  // namespace

extern "C" int tile_main() {
    using namespace golem::platform;
    const uint32_t tile = MITTENS_TILE_ID;
    const int destination = destinationSlot(tile);
    const uint32_t ownedTransfers = localTransferCount(tile);
    const uint32_t receivedTransfers = destinationTransferCount(tile);
    volatile uint32_t* const scratchpad =
        reinterpret_cast<volatile uint32_t*>(ScratchpadBase);

    if (ownedTransfers != 0) {
        for (uint32_t transfer = 0; transfer < kTransferCount; ++transfer) {
            if (kSources[transfer] != tile) continue;
            const uint32_t ordinal = localTransferOrdinal(tile, transfer);
            volatile uint32_t* const buffer =
                scratchpad + bufferOffsetWords(ordinal);
            for (uint32_t word = 0; word < kPayloadWords; ++word) {
                buffer[word] = payloadValue(transfer, word);
            }
        }
    } else if (destination >= 0) {
        for (uint32_t word = 0; word < kPayloadWords; ++word) {
            scratchpad[word] = 0;
        }
    }
    mesh_nic::complete_memory_initialization();

    if (ownedTransfers != 0) {
#if MITTENS_FANOUT_RECORD_READY_CYCLES
        uint64_t readyCycles[kTransferCount] = {};
        uint32_t submitted = 0;
#endif
        mesh_nic::trace_task(
            mesh_nic::kTaskTraceStart, kFanoutTaskBase + tile, 0);
        for (uint32_t transfer = 0; transfer < kTransferCount; ++transfer) {
            if (kSources[transfer] != tile) continue;
#if MITTENS_FANOUT_RECORD_READY_CYCLES
            if (submitted != 0) waitGuestCycles(kReadySkewCycles);
#endif
            const uint32_t header[kHeaderWords] = {
                kFrameMagic, kRouteBase + transfer, 0, 0, 0, 0,
                kPayloadWords};
            const uint32_t ordinal = localTransferOrdinal(tile, transfer);
            sendBurst(kDestinations[transfer], header, kHeaderWords);
#if MITTENS_FANOUT_RECORD_READY_CYCLES
            readyCycles[submitted] = readCycle();
#endif
            sendBurst(kDestinations[transfer],
                      const_cast<const uint32_t*>(
                          scratchpad + bufferOffsetWords(ordinal)),
                      kPayloadWords);
#if MITTENS_FANOUT_RECORD_READY_CYCLES
            ++submitted;
#endif
        }
        uint32_t acknowledgementMask = 0;
        for (uint32_t acknowledgement = 0;
             acknowledgement < ownedTransfers; ++acknowledgement) {
            uint32_t source = UINT32_MAX;
            const uint32_t received = mesh_nic::receive_from(&source);
            if (received < kAckBase || received >= kAckBase + kTransferCount) {
                platform_exit(1);
            }
            const uint32_t transfer = received - kAckBase;
            if (kSources[transfer] != tile ||
                source != kDestinations[transfer] ||
                (acknowledgementMask & (UINT32_C(1) << transfer)) != 0) {
                platform_exit(1);
            }
            acknowledgementMask |= UINT32_C(1) << transfer;
        }
        mesh_nic::trace_task(
            mesh_nic::kTaskTraceFinish, kFanoutTaskBase + tile, 0);
#if MITTENS_FANOUT_RECORD_READY_CYCLES
        uart_puts("TX_FANOUT_READY_CYCLES");
        for (uint32_t index = 0; index < submitted; ++index) {
            uart_putc(' ');
            uartPutUnsigned(readyCycles[index]);
        }
        uart_putc('\n');
#endif
        uart_puts("TX_FANOUT_SOURCE_PASS\n");
        return 0;
    }

    if (destination < 0) return 0;
    mesh_nic::trace_task(
        mesh_nic::kTaskTraceStart, kDestinationTaskBase + tile, 0);
    uint32_t completed = 0;
    for (uint32_t transfer = 0; transfer < kTransferCount; ++transfer) {
        if (kDestinations[transfer] != tile) continue;
        if (!receiveHeader(transfer)) platform_exit(1);
        while (!mesh_nic::try_start_receive_words(
            kSources[transfer], kRouteBase + transfer, 0,
            const_cast<uint32_t*>(scratchpad), kPayloadWords)) {
            mesh_nic::wait_for_receive();
        }
        if (!receiveCompletion(transfer)) platform_exit(1);
        while (!mesh_nic::try_send(kSources[transfer], kAckBase + transfer)) {
            mesh_nic::wait_for_transmit();
        }
        ++completed;
    }
    if (completed != receivedTransfers) platform_exit(1);
    mesh_nic::trace_task(
        mesh_nic::kTaskTraceFinish, kDestinationTaskBase + tile, 0);
    // Verify the final received payload after the measured completion marker.
    // H1 uses one independent destination per transfer, so this checks every word.
#if MITTENS_FANOUT_BANK_LAYOUT != 1
    uint32_t finalTransfer = 0;
    for (uint32_t transfer = 0; transfer < kTransferCount; ++transfer) {
        if (kDestinations[transfer] == tile) finalTransfer = transfer;
    }
    for (uint32_t word = 0; word < kPayloadWords; ++word) {
        if (scratchpad[word] != payloadValue(finalTransfer, word)) {
            uart_puts("TX_FANOUT_PAYLOAD_FAIL\n");
            platform_exit(1);
        }
    }
#endif
    uart_puts("TX_FANOUT_SINK_PASS\n");
    return 0;
}
