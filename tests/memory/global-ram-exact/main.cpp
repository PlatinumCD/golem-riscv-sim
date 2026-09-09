#include "platform.h"
#include "scratchpad-dma.h"

#include <stdint.h>

#ifndef MITTENS_TILE_ID
#error "MITTENS_TILE_ID must be defined"
#endif

namespace {

constexpr uint32_t kTileId = MITTENS_TILE_ID;
constexpr uint32_t kConsumerTile = 2;
constexpr uint32_t kHalfBytes = 4096;
constexpr uint32_t kTotalBytes = 2 * kHalfBytes;
constexpr uint64_t kGlobalOffset = 65536;
constexpr uint64_t kExecution = 91;
constexpr uint32_t kExact = golem::platform::ScratchpadDMAExactReadiness;
constexpr uint32_t kTeardown =
    kExact | golem::platform::ScratchpadDMAExactExecutionTeardown;

uint8_t expectedByte(uint32_t index) {
  return static_cast<uint8_t>((index * 37U + 19U) ^ (index >> 3U));
}

int fail(const char *message) {
  uart_puts(message);
  uart_putc('\n');
  return 1;
}

void staggerProducer(uint32_t iterations) {
  volatile uint32_t state = kTileId + 1U;
  for (uint32_t iteration = 0; iteration < iterations; ++iteration)
    state = state * UINT32_C(1664525) + UINT32_C(1013904223);
  if (state == 0)
    uart_putc(' ');
}

bool submit(uint64_t globalOffset, uint64_t scratchpadOffset,
            uint32_t byteCount, uint32_t token,
            golem::platform::ScratchpadDMADirection direction,
            uint32_t flags = kExact) {
  return golem::platform::globalDMASubmit(globalOffset, scratchpadOffset,
                                          byteCount, token, kExecution, token,
                                          direction, flags);
}

bool wait(uint32_t token) {
  return golem::platform::globalDMAWait(kExecution, token);
}

bool teardown() {
  constexpr uint32_t token = UINT32_MAX;
  return golem::platform::globalDMASubmit(
             0, 0, 0, token, kExecution, UINT64_MAX,
             golem::platform::ScratchpadDMADirection::ScratchpadToGlobalRAM,
             kTeardown) &&
         wait(token);
}

void leaveTerminalScratchpadAccessPending() {
  // Exercise the real-model terminal path: the model runtime reads its final
  // scratchpad-resident state after the exact-readiness teardown has completed.
  // With scratchpad access batching enabled this access is still buffered when
  // platform_exit publishes the guest-exit event.
  volatile uint8_t *scratchpad = reinterpret_cast<volatile uint8_t *>(
      golem::platform::ScratchpadBase);
  scratchpad[kTotalBytes + kTileId] = static_cast<uint8_t>(kTileId + 1U);
  if (scratchpad[kTotalBytes + kTileId] == 0)
    uart_putc(' ');
}

int runFirstProducer() {
  using Direction = golem::platform::ScratchpadDMADirection;
  staggerProducer(100000);
  uart_puts("exact RAM tile 0: publishing first half\n");

  auto *scratchpad =
      reinterpret_cast<uint8_t *>(golem::platform::ScratchpadBase);
  for (uint32_t index = 0; index < kHalfBytes; ++index)
    scratchpad[index] = expectedByte(index);
  if (!submit(kGlobalOffset, 0, kHalfBytes, 0,
              Direction::ScratchpadToGlobalRAM) ||
      !wait(0))
    return fail("exact RAM first publication failed");

  if (!teardown())
    return fail("exact RAM tile 0 teardown failed");
  leaveTerminalScratchpadAccessPending();
  uart_puts("exact global RAM tile 0: PASS\n");
  return 0;
}

int runSecondProducer() {
  using Direction = golem::platform::ScratchpadDMADirection;
  staggerProducer(400000);
  uart_puts("exact RAM tile 1: publishing second half\n");

  auto *scratchpad =
      reinterpret_cast<uint8_t *>(golem::platform::ScratchpadBase);
  for (uint32_t index = 0; index < kHalfBytes; ++index)
    scratchpad[index] = expectedByte(index + kHalfBytes);
  if (!submit(kGlobalOffset + kHalfBytes, 0, kHalfBytes, 0,
              Direction::ScratchpadToGlobalRAM) ||
      !wait(0))
    return fail("exact RAM second publication failed");
  if (!teardown())
    return fail("exact RAM tile 1 teardown failed");
  leaveTerminalScratchpadAccessPending();
  uart_puts("exact global RAM tile 1: PASS\n");
  return 0;
}

int runConsumer() {
  using Direction = golem::platform::ScratchpadDMADirection;
  auto *first = reinterpret_cast<uint8_t *>(golem::platform::ScratchpadBase);
  auto *second = first + kTotalBytes;

  // Both fanout copies are decomposed into exact 4-KiB physical requests and
  // submitted before either producer is allowed to publish. The grouped wait
  // resumes the guest only after all four independently released requests have
  // completed.
  if (!submit(kGlobalOffset, 0, kHalfBytes, 0,
              Direction::GlobalRAMToScratchpad) ||
      !submit(kGlobalOffset + kHalfBytes, kHalfBytes, kHalfBytes, 1,
              Direction::GlobalRAMToScratchpad) ||
      !submit(kGlobalOffset, kTotalBytes, kHalfBytes, 2,
              Direction::GlobalRAMToScratchpad) ||
      !submit(kGlobalOffset + kHalfBytes, kTotalBytes + kHalfBytes,
              kHalfBytes, 3,
              Direction::GlobalRAMToScratchpad))
    return fail("exact RAM early consumer submission failed");
  uart_puts("exact RAM tile 2: four reads blocked\n");

  if (!golem::platform::globalDMAWaitBatch(kExecution, 0, 4))
    return fail("exact RAM fanout batch wait failed");
  uart_puts("exact RAM tile 2: all reads released\n");

  for (uint32_t index = 0; index < kTotalBytes; ++index) {
    const uint8_t expected = expectedByte(index);
    if (first[index] != expected || second[index] != expected)
      return fail("exact RAM staged payload mismatch");
  }
  if (!teardown())
    return fail("exact RAM tile 2 teardown failed");
  leaveTerminalScratchpadAccessPending();
  uart_puts("exact global RAM tile 2: PASS\n");
  return 0;
}

} // namespace

extern "C" int tile_main() {
  if (kTileId == 0)
    return runFirstProducer();
  if (kTileId == 1)
    return runSecondProducer();
  if (kTileId == kConsumerTile)
    return runConsumer();
  return fail("exact RAM invalid tile ID");
}
