#include <stddef.h>
#include <stdint.h>

#include "abi_accounting.h"
#include "case_oracle.h"
#include "golem/runtime/deployment_runtime.h"
#include "golem/runtime/scratchpad_abi.h"
#include "golem/runtime/tile_abi.h"
#include "mesh-nic.h"
#include "platform.h"
#include "scratchpad-dma.h"

#ifndef MITTENS_MATERIALIZED_SEED_TILE
#error "MITTENS_MATERIALIZED_SEED_TILE must name the unique input owner"
#endif

namespace {

using namespace golem::runtime;
using mittens::materialized_test::CaseContract;
using mittens::materialized_test::DMAAccounting;

constexpr uint32_t kPhysicalShardBytes = 4096;
constexpr uint32_t kSeedTile = MITTENS_MATERIALIZED_SEED_TILE;

void printUnsigned(uint64_t value) {
  char digits[21];
  uint32_t count = 0;
  do {
    digits[count++] = static_cast<char>('0' + value % 10U);
    value /= 10U;
  } while (value != 0);
  while (count != 0)
    uart_putc(digits[--count]);
}

void printField(const char *name, uint64_t value) {
  uart_putc(' ');
  uart_puts(name);
  uart_putc('=');
  printUnsigned(value);
}

int fail(const TileABI *abi, const char *stage, int code) {
  uart_puts("MATERIALIZED_FUNCTIONAL_ERROR case=");
  uart_puts(mittens::materialized_test::caseContract().name);
  if (abi != nullptr)
    printField("tile", abi->core_id);
  uart_puts(" stage=");
  uart_puts(stage);
  uart_putc('\n');
  return code;
}

bool trySendWord(void *, uint32_t destination, uint32_t word) {
  return mesh_nic::try_send(destination, word);
}

bool tryReceiveWord(void *, RoutedWord *word) {
  return word != nullptr &&
         mesh_nic::try_receive_from(&word->source_tile, &word->payload);
}

bool trySendWords(void *, uint32_t destination, const uint32_t *words,
                  uint32_t word_count) {
  return mesh_nic::try_send_words(destination, words, word_count);
}

bool tryStartReceiveWords(void *, uint32_t source, uint32_t route_id,
                          uint64_t logical_iteration, void *destination,
                          uint32_t word_count) {
  return mesh_nic::try_start_receive_words(source, route_id, logical_iteration,
                                           destination, word_count);
}

bool tryReceiveWordsCompletion(void *, uint32_t *source, uint32_t *route_id,
                               uint64_t *logical_iteration) {
  return mesh_nic::try_receive_words_completion(source, route_id,
                                                logical_iteration);
}

bool tryClaimReceiveWords(void *, uint32_t source, uint32_t route_id,
                          uint64_t logical_iteration, uint32_t word_count) {
  return mesh_nic::try_claim_receive_words(source, route_id, logical_iteration,
                                           word_count);
}

const RoutedWordTransport kNoCTransport{
    nullptr,
    trySendWord,
    tryReceiveWord,
    trySendWords,
    tryStartReceiveWords,
    tryReceiveWordsCompletion,
    tryClaimReceiveWords,
};

bool submitGlobalDMA(void *, ExecutionId execution_id, uint32_t token_id,
                     uint64_t logical_iteration, uint64_t global_offset,
                     uint64_t scratchpad_offset, uint32_t byte_count,
                     ScratchpadDMADirection direction, uint32_t request_flags) {
  const auto platform_direction =
      direction == ScratchpadDMADirection::GlobalRAMToScratchpad
          ? golem::platform::ScratchpadDMADirection::GlobalRAMToScratchpad
          : golem::platform::ScratchpadDMADirection::ScratchpadToGlobalRAM;
  return golem::platform::globalDMASubmit(
      global_offset, scratchpad_offset, byte_count, token_id, execution_id,
      logical_iteration, platform_direction, request_flags);
}

bool waitGlobalDMA(void *, ExecutionId execution_id, uint32_t token_id) {
  return golem::platform::globalDMAWait(execution_id, token_id);
}

const GlobalDMATransport kGlobalDMATransport{
    nullptr,
    submitGlobalDMA,
    waitGlobalDMA,
};

bool arriveAndWaitEpoch(void *, uint32_t completed_epoch,
                        EpochContribution contribution) {
  uint32_t encoded = UINT32_MAX;
  if (contribution == EpochContribution::WorkComplete)
    encoded = mesh_nic::kEpochWorkComplete;
  else if (contribution == EpochContribution::Idle)
    encoded = mesh_nic::kEpochIdle;
  return encoded != UINT32_MAX &&
         mesh_nic::arrive_epoch(completed_epoch, encoded);
}

const EpochBarrierTransport kEpochBarrierTransport{
    nullptr,
    arriveAndWaitEpoch,
};

const GlobalBuffer *nthGlobalBuffer(const TileABI &abi, uint32_t flag,
                                    uint32_t ordinal) {
  uint32_t seen = 0;
  for (uint32_t index = 0; index < abi.global_buffer_count; ++index) {
    const GlobalBuffer &buffer = abi.global_buffers[index];
    if ((buffer.flags & flag) == 0)
      continue;
    if (seen++ == ordinal)
      return &buffer;
  }
  return nullptr;
}

bool validateCaseBuffers(const TileABI &abi, const CaseContract &contract) {
  uint32_t inputs = 0;
  uint32_t outputs = 0;
  for (uint32_t index = 0; index < abi.global_buffer_count; ++index) {
    const GlobalBuffer &buffer = abi.global_buffers[index];
    if ((buffer.flags & GlobalBufferInput) != 0) {
      if (inputs >= contract.input_count ||
          buffer.byte_size != contract.input_bytes[inputs] ||
          buffer.byte_size % sizeof(float) != 0)
        return false;
      ++inputs;
    }
    if ((buffer.flags & GlobalBufferOutput) != 0) {
      if (outputs >= contract.output_count ||
          buffer.byte_size != contract.output_bytes[outputs] ||
          buffer.byte_size % sizeof(float) != 0)
        return false;
      ++outputs;
    }
  }
  return inputs == contract.input_count && outputs == contract.output_count;
}

bool seedInputs(const TileABI &abi, const CaseContract &contract) {
  auto *staging =
      reinterpret_cast<float *>(static_cast<uintptr_t>(ScratchpadPhysicalBase));
  for (uint32_t input = 0; input < contract.input_count; ++input) {
    const GlobalBuffer *buffer = nthGlobalBuffer(abi, GlobalBufferInput, input);
    if (buffer == nullptr)
      return false;
    for (uint64_t offset = 0; offset < buffer->byte_size;
         offset += kPhysicalShardBytes) {
      const uint64_t remaining = buffer->byte_size - offset;
      const uint32_t bytes = static_cast<uint32_t>(
          remaining < kPhysicalShardBytes ? remaining : kPhysicalShardBytes);
      if (bytes % sizeof(float) != 0)
        return false;
      const uint64_t first_element = offset / sizeof(float);
      for (uint32_t element = 0; element < bytes / sizeof(float); ++element)
        staging[element] = mittens::materialized_test::inputValue(
            input, first_element + element);
      if (!golem::platform::globalRAMInitialize(buffer->ram_offset + offset, 0,
                                                bytes))
        return false;
    }
  }
  return true;
}

} // namespace

extern "C" int tile_main() {
  const TileABI abi = linkedTileABI();
  const CaseContract &contract = mittens::materialized_test::caseContract();
  if (!abi.valid() || !abi.hasDeploymentPlan() || !abi.validDeploymentPlan() ||
      !mittens::materialized_test::validMaterializedABIShape(abi))
    return fail(&abi, "abi", 1);
  if (!validateCaseBuffers(abi, contract))
    return fail(&abi, "case-buffers", 2);

  DMAAccounting expected_dma{};
  uint64_t expected_iterations = 0;
  if (!mittens::materialized_test::expectedDMAAccounting(abi, &expected_dma) ||
      !mittens::materialized_test::expectedShardIterations(
          abi, &expected_iterations))
    return fail(&abi, "expected-accounting", 3);

  DeploymentProfile profile{};
  DeploymentRuntime runtime{
      abi,
      kNoCTransport,
      &profile,
      nullptr,
      DeploymentTransmitPolicy::Blocking,
      nullptr,
      kGlobalDMATransport,
      nullptr,
      kEpochBarrierTransport,
  };
  if (!runtime.initialize())
    return fail(&abi, "initialize", 4);
  if (abi.core_id == kSeedTile && !seedInputs(abi, contract))
    return fail(&abi, "seed", 5);
  if (!runtime.boot())
    return fail(&abi, "boot", 6);
  mesh_nic::complete_memory_initialization();

  uint64_t watchdog = 0;
  while (!runtime.complete() && !runtime.failed() &&
         watchdog++ < UINT64_C(100000000)) {
    const DeploymentStep step = runtime.step();
    if (step == DeploymentStep::WaitForReceive)
      mesh_nic::wait_for_receive();
    else if (step == DeploymentStep::WaitForTransmit)
      mesh_nic::wait_for_transmit();
  }
  if (runtime.failed() || !runtime.complete() ||
      profile.shard_iterations_issued != expected_iterations ||
      profile.shard_iterations_executed != expected_iterations ||
      profile.shard_iterations_retired != expected_iterations ||
      profile.global_ram_dma_requests != expected_dma.requests ||
      profile.global_ram_dma_completions != expected_dma.requests ||
      profile.global_ram_dma_bytes != expected_dma.bytes ||
      profile.logical_frames_sent != 0 || profile.logical_frames_received != 0)
    return fail(&abi, "runtime-accounting", 7);

  DMAAccounting validation{};

  uart_puts("MATERIALIZED_FUNCTIONAL_ACCOUNT case=");
  uart_puts(contract.name);
  printField("tile", abi.core_id);
  printField("iterations", expected_iterations);
  printField("full_requests", expected_dma.full_requests);
  printField("tail_requests", expected_dma.tail_requests);
  printField("runtime_requests", expected_dma.requests);
  printField("runtime_bytes", expected_dma.bytes);
  printField("validation_requests", validation.requests);
  printField("validation_bytes", validation.bytes);
  uart_putc('\n');
  uart_puts("MATERIALIZED_FUNCTIONAL_PASS case=");
  uart_puts(contract.name);
  printField("tile", abi.core_id);
  uart_putc('\n');
  return 0;
}
