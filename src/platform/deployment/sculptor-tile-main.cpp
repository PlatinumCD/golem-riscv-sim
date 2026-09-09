#include <stddef.h>
#include <stdint.h>

#include "golem/runtime/runtime.h"
#include "materialized-model-inputs.h"
#include "../devices/mesh-nic.h"
#include "../devices/platform.h"
#include "../devices/scratchpad-dma.h"

#ifndef GOLEM_GLOBAL_DMA_DESCRIPTOR_WINDOW
#define GOLEM_GLOBAL_DMA_DESCRIPTOR_WINDOW UINT32_C(1)
#endif

static_assert(GOLEM_GLOBAL_DMA_DESCRIPTOR_WINDOW >= 1 &&
              GOLEM_GLOBAL_DMA_DESCRIPTOR_WINDOW <= 8,
              "global DMA descriptor window must be in [1, 8]");

namespace {

using namespace golem::runtime;

extern "C" void *malloc(size_t size);

void printUnsigned(uint64_t value) {
  char digits[20];
  size_t count = 0;
  do {
    digits[count++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  while (count != 0) {
    uart_putc(digits[--count]);
  }
}

void printMovementCounters(uint32_t tile,
                           const DeploymentMovementCounters &counters) {
  uart_puts("SCULPTOR_RA_MOVEMENT tile=");
  printUnsigned(tile);
  uart_puts(" physical_global_ram_dma_requests=");
  printUnsigned(counters.global_ram_dma_requests);
  uart_puts(" physical_global_ram_dma_completions=");
  printUnsigned(counters.global_ram_dma_completions);
  uart_puts(" physical_global_ram_dma_bytes=");
  printUnsigned(counters.global_ram_dma_bytes);
  uart_puts(" physical_noc_frames_sent=");
  printUnsigned(counters.noc_frames_sent);
  uart_puts(" physical_noc_frames_received=");
  printUnsigned(counters.noc_frames_received);
  uart_puts(" physical_noc_payload_bytes_sent=");
  printUnsigned(counters.noc_payload_bytes_sent);
  uart_puts(" physical_noc_payload_bytes_received=");
  printUnsigned(counters.noc_payload_bytes_received);
  uart_puts(" local_copy_transfers=");
  printUnsigned(counters.local_copy_transfers);
  uart_puts(" local_copy_bytes=");
  printUnsigned(counters.local_copy_bytes);
  uart_puts(" retained_forwarded_logical_transfers=");
  printUnsigned(counters.retained_forwarded_logical_transfers);
  uart_puts(" retained_forwarded_logical_bytes=");
  printUnsigned(counters.retained_forwarded_logical_bytes);
  uart_putc('\n');
}

void printExecutionCounters(uint32_t tile,
                            const DeploymentExecutionCounters &counters) {
  uart_puts("SCULPTOR_RA_EXECUTION tile=");
  printUnsigned(tile);
  uart_puts(" wave_attempts=");
  printUnsigned(counters.wave_attempts);
  uart_puts(" wave_groups=");
  printUnsigned(counters.wave_groups);
  uart_puts(" wave_iterations=");
  printUnsigned(counters.wave_iterations);
  uart_puts(" wave_scalar_fallbacks=");
  printUnsigned(counters.wave_scalar_fallbacks);
  uart_puts(" wave_incomplete_ready_groups=");
  printUnsigned(counters.wave_incomplete_ready_groups);
  uart_puts(" wave_formation_waits=");
  printUnsigned(counters.wave_formation_waits);
  uart_puts(" maximum_wave_width=");
  printUnsigned(counters.maximum_wave_width);
  uart_putc('\n');
}

#if defined(GOLEM_ENABLE_STARTUP_PROFILE)
#ifndef GOLEM_STARTUP_PROFILE_TILE
#define GOLEM_STARTUP_PROFILE_TILE UINT32_MAX
#endif

uint64_t readStartupCycle(void *) {
  uint64_t value = 0;
  __asm__ volatile("rdcycle %0" : "=r"(value));
  return value;
}

void printStartupProfile(uint32_t tile,
                         const DeploymentInitializationProfile &profile) {
  uart_puts("SCULPTOR_RA_STARTUP_PROFILE tile=");
  printUnsigned(tile);
  uart_puts(" validation_cycles=");
  printUnsigned(profile.validation_cycles);
  uart_puts(" shard_loop_cycles=");
  printUnsigned(profile.shard_loop_cycles);
  uart_puts(" global_buffer_cycles=");
  printUnsigned(profile.global_buffer_cycles);
  uart_puts(" affine_run_cycles=");
  printUnsigned(profile.materialized_affine_run_cycles);
  uart_puts(" descriptor_cycles=");
  printUnsigned(profile.materialized_descriptor_cycles);
  uart_puts(" descriptor_header_join_cycles=");
  printUnsigned(profile.materialized_descriptor_header_join_cycles);
  uart_puts(" segment_arithmetic_bounds_cycles=");
  printUnsigned(profile.materialized_segment_arithmetic_bounds_cycles);
  uart_puts(" descriptor_provenance_coverage_cycles=");
  printUnsigned(profile.materialized_descriptor_provenance_coverage_cycles);
  uart_puts(" spm_disjointness_cycles=");
  printUnsigned(profile.materialized_spm_disjointness_cycles);
  uart_puts(" exhaustive_output_proof_cycles=");
  printUnsigned(profile.materialized_exhaustive_output_proof_cycles);
  uart_puts(" descriptor_residual_cycles=");
  printUnsigned(profile.materialized_descriptor_residual_cycles);
  uart_puts(" ownership_cycles=");
  printUnsigned(profile.output_ownership_cycles);
  uart_puts(" ownership_structure_cycles=");
  printUnsigned(profile.output_ownership_structure_cycles);
  uart_puts(" ownership_digest_cycles=");
  printUnsigned(profile.output_ownership_digest_cycles);
  uart_puts(" ownership_regeneration_cycles=");
  printUnsigned(profile.output_ownership_regeneration_cycles);
  uart_puts(" indexed_dma_cycles=");
  printUnsigned(profile.indexed_dma_cycles);
  uart_puts(" port_coverage_cycles=");
  printUnsigned(profile.materialized_port_coverage_cycles);
  uart_puts(" allocation_cycles=");
  printUnsigned(profile.state_allocation_cycles);
  uart_puts(" resource_cycles=");
  printUnsigned(profile.resource_initialization_cycles);
  uart_puts(" descriptor_pairs=");
  printUnsigned(profile.output_descriptor_pairs);
  uart_puts(" segment_pairs=");
  printUnsigned(profile.output_segment_pairs);
  uart_putc('\n');
}
#endif

#if defined(GOLEM_ENABLE_HEAP_PROFILE)
void printHeapProfile(uint32_t tile, const HeapProfile &profile) {
  uart_puts("SCULPTOR_HEAP_PROFILE tile=");
  printUnsigned(tile);
  uart_puts(" allocation_count=");
  printUnsigned(profile.allocation_count);
  uart_puts(" current_live_bytes=");
  printUnsigned(profile.current_live_bytes);
  uart_puts(" peak_live_bytes=");
  printUnsigned(profile.peak_live_bytes);
  uart_puts(" failed_allocation_count=");
  printUnsigned(profile.failed_allocation_count);
  uart_puts(" failed_allocation_size=");
  printUnsigned(profile.failed_allocation_size);
  uart_putc('\n');
}
#endif

void **allocatePointerTable(uint32_t count) {
  if (count == 0) {
    return nullptr;
  }
  auto **table =
      static_cast<void **>(malloc(static_cast<size_t>(count) * sizeof(void *)));
  if (table == nullptr) {
    return nullptr;
  }
  for (uint32_t index = 0; index < count; ++index) {
    table[index] = nullptr;
  }
  return table;
}

#if defined(GOLEM_ENABLE_OUTPUT_FINGERPRINT)
constexpr uint64_t kFingerprintOffsetBasis = UINT64_C(14695981039346656037);
constexpr uint64_t kFingerprintPrime = UINT64_C(1099511628211);

struct ModelOutputFingerprint {
  uint64_t exact = kFingerprintOffsetBasis;
  uint64_t tolerance = kFingerprintOffsetBasis;
  uint32_t samples[16]{};
  uint32_t sample_count = 0;
};

void updateFingerprintBytes(uint64_t &fingerprint, const void *data,
                            uint64_t byte_count) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (uint64_t index = 0; index < byte_count; ++index) {
    fingerprint ^= bytes[index];
    fingerprint *= kFingerprintPrime;
  }
}

void updateFloat32Tolerance(ModelOutputFingerprint &fingerprint,
                            const void *data, uint64_t byte_count) {
  const auto *values = static_cast<const uint32_t *>(data);
  for (uint64_t index = 0; index < byte_count / sizeof(uint32_t); ++index) {
    uint32_t bits = values[index];
    if (fingerprint.sample_count < 16) {
      fingerprint.samples[fingerprint.sample_count++] = bits;
    }
    if ((bits & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000)) {
      bits = (bits + UINT32_C(0x00001fff) + ((bits >> 14U) & UINT32_C(1))) &
             UINT32_C(0xffffc000);
    }
    for (unsigned shift = 0; shift < 32; shift += 8) {
      fingerprint.tolerance ^= static_cast<uint8_t>(bits >> shift);
      fingerprint.tolerance *= kFingerprintPrime;
    }
  }
}

void updateModelOutputFingerprint(ModelOutputFingerprint &fingerprint,
                                  bool is_float32, const void *data,
                                  uint64_t byte_count) {
  updateFingerprintBytes(fingerprint.exact, data, byte_count);
  if (is_float32) {
    updateFloat32Tolerance(fingerprint, data, byte_count);
  }
}

void printModelOutputFingerprint(uint32_t tile, const ModelIO &output,
                                 bool is_float32,
                                 const ModelOutputFingerprint &fingerprint) {
  uart_puts("SCULPTOR_MODEL_OUTPUT tile=");
  printUnsigned(tile);
  uart_puts(" index=");
  printUnsigned(output.model_index);
  uart_puts(" bytes=");
  printUnsigned(output.byte_size);
  uart_puts(" fingerprint=");
  printUnsigned(fingerprint.exact);
  if (is_float32) {
    uart_puts(" tolerance_fingerprint=");
    printUnsigned(fingerprint.tolerance);
    for (uint32_t index = 0; index < fingerprint.sample_count; ++index) {
      uart_puts(" sample");
      printUnsigned(index);
      uart_puts("_bits=");
      printUnsigned(fingerprint.samples[index]);
    }
  }
  uart_putc('\n');
}

bool printLocalModelOutput(const TileABI &abi, uint32_t tile,
                           const ModelIO &output, const void *data) {
  const Resource *resource = abi.findResource(output.local_slot);
  if (resource == nullptr || data == nullptr) {
    return false;
  }
  const bool is_float32 = resource->element_type == ElementType::Float32 &&
                          output.byte_size % sizeof(uint32_t) == 0;
  ModelOutputFingerprint fingerprint{};
  updateModelOutputFingerprint(fingerprint, is_float32, data, output.byte_size);
  printModelOutputFingerprint(tile, output, is_float32, fingerprint);
  return true;
}

bool printGlobalModelOutput(const TileABI &abi, uint32_t tile,
                            const ModelIO &output) {
  constexpr uint64_t kFingerprintExecution = UINT64_MAX - UINT64_C(1);
  const Resource *resource = abi.findResource(output.local_slot);
  const GlobalBuffer *buffer = abi.findGlobalBuffer(output.global_resource_id);
  if (resource == nullptr || buffer == nullptr ||
      resource->kind != ResourceKind::ModelOutput ||
      (buffer->flags & GlobalBufferOutput) == 0 ||
      buffer->byte_size != output.byte_size || output.byte_size == 0 ||
      output.byte_size > abi.global_ram_capacity_bytes ||
      buffer->ram_offset > abi.global_ram_capacity_bytes - output.byte_size) {
    return false;
  }
  const bool is_float32 = resource->element_type == ElementType::Float32 &&
                          output.byte_size % sizeof(uint32_t) == 0;
  ModelOutputFingerprint fingerprint{};
  const uint64_t maximum_frame_bytes = abi.maximumFrameBytes();
  if (!isSupportedPhysicalFrameMaximum(
          static_cast<uint32_t>(maximum_frame_bytes))) {
    return false;
  }
  for (uint64_t offset = 0, shard = 0; offset < output.byte_size;
       offset += maximum_frame_bytes, ++shard) {
    const uint64_t remaining = output.byte_size - offset;
    const uint32_t bytes = static_cast<uint32_t>(
        remaining < maximum_frame_bytes ? remaining : maximum_frame_bytes);
    const uint32_t token = UINT32_C(0xe0000000) |
                           ((output.model_index & UINT32_C(0xffff)) << 8U) |
                           static_cast<uint32_t>(shard & UINT64_C(0xff));
    golem::platform::globalDMASubmit(
        buffer->ram_offset + offset, 0, bytes, token, kFingerprintExecution,
        shard, golem::platform::ScratchpadDMADirection::GlobalRAMToScratchpad);
    if (!golem::platform::globalDMAWait(kFingerprintExecution, token)) {
      return false;
    }
    const void *scratchpad = reinterpret_cast<const void *>(
        static_cast<uintptr_t>(ScratchpadPhysicalBase));
    updateModelOutputFingerprint(fingerprint, is_float32, scratchpad, bytes);
  }
  printModelOutputFingerprint(tile, output, is_float32, fingerprint);
  return true;
}
#endif

template <typename Element>
void fillElements(void *data, size_t count, Element value) {
  auto *elements = static_cast<Element *>(data);
  for (size_t index = 0; index < count; ++index) {
    elements[index] = value;
  }
}

bool initializeResourceBuffer(const Resource &resource, void *data,
                              uint64_t byte_count) {
  const uint32_t element_size = elementSizeBytes(resource.element_type);
  if (element_size == 0 || byte_count == 0 || byte_count % element_size != 0 ||
      byte_count / element_size > SIZE_MAX) {
    return false;
  }
  const size_t element_count = static_cast<size_t>(byte_count / element_size);

  switch (resource.element_type) {
  case ElementType::Float32:
    fillElements(data, element_count, 1.0F);
    return true;
  case ElementType::Int8:
    fillElements(data, element_count, static_cast<int8_t>(1));
    return true;
  case ElementType::UInt8:
    fillElements(data, element_count, static_cast<uint8_t>(1));
    return true;
  case ElementType::Int16:
    fillElements(data, element_count, static_cast<int16_t>(1));
    return true;
  case ElementType::UInt16:
    fillElements(data, element_count, static_cast<uint16_t>(1));
    return true;
  case ElementType::Int32:
    fillElements(data, element_count, static_cast<int32_t>(1));
    return true;
  case ElementType::UInt32:
    fillElements(data, element_count, static_cast<uint32_t>(1));
    return true;
  case ElementType::Int64:
    fillElements(data, element_count, static_cast<int64_t>(1));
    return true;
  case ElementType::UInt64:
    fillElements(data, element_count, static_cast<uint64_t>(1));
    return true;
  case ElementType::Invalid:
    return false;
  }

  return false;
}

bool initializeModelInput(const TileABI &abi, const ModelIO &input,
                          void *data) {
  const Resource *resource = abi.findResource(input.local_slot);
  return resource != nullptr && resource->kind == ResourceKind::ModelInput &&
         resource->byte_size == input.byte_size &&
         initializeResourceBuffer(*resource, data, input.byte_size);
}

void *allocateModelIO(const TileABI &abi, const ModelIO &model_io,
                      ResourceKind expected_kind) {
  const Resource *resource = abi.findResource(model_io.local_slot);
  if (resource == nullptr || resource->kind != expected_kind ||
      resource->byte_size != model_io.byte_size ||
      model_io.owner_core != abi.core_id || model_io.byte_size == 0 ||
      model_io.byte_size > SIZE_MAX) {
    return nullptr;
  }
#if defined(GOLEM_ENABLE_HEAP_PROFILE)
  return golem_runtime_profiled_malloc(static_cast<size_t>(model_io.byte_size));
#else
  return malloc(static_cast<size_t>(model_io.byte_size));
#endif
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

const RoutedWordTransport kTransport{
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
                     golem::runtime::ScratchpadDMADirection direction,
                     uint32_t request_flags) {
  return golem::platform::globalDMASubmit(
      global_offset, scratchpad_offset, byte_count, token_id, execution_id,
      logical_iteration,
      direction == golem::runtime::ScratchpadDMADirection::GlobalRAMToScratchpad
          ? golem::platform::ScratchpadDMADirection::GlobalRAMToScratchpad
          : golem::platform::ScratchpadDMADirection::ScratchpadToGlobalRAM,
      request_flags);
}

bool waitGlobalDMA(void *, ExecutionId execution_id, uint32_t token_id) {
  return golem::platform::globalDMAWait(execution_id, token_id);
}

bool waitGlobalDMABatch(void *, ExecutionId execution_id,
                        uint32_t first_token_id, uint32_t token_count) {
  return golem::platform::globalDMAWaitBatch(execution_id, first_token_id,
                                             token_count);
}

bool beginGlobalDMAMacro(
    void *, ExecutionId execution_id, uint32_t transfer_count,
    uint64_t maximum_instruction_span) {
  return golem::platform::globalDMAMacroBegin(
      execution_id, transfer_count, maximum_instruction_span);
}

bool endGlobalDMAMacro(void *, ExecutionId execution_id,
                       uint32_t transfer_count) {
  return golem::platform::globalDMAMacroEnd(execution_id, transfer_count);
}

const GlobalDMATransport kGlobalDMATransport{
    nullptr,
    submitGlobalDMA,
    waitGlobalDMA,
    GOLEM_GLOBAL_DMA_DESCRIPTOR_WINDOW,
    waitGlobalDMABatch,
    beginGlobalDMAMacro,
    endGlobalDMAMacro,
};

bool arriveAndWaitEpoch(void *, uint32_t completed_epoch,
                        EpochContribution contribution) {
  uint32_t encoded = UINT32_MAX;
  switch (contribution) {
  case EpochContribution::WorkComplete:
    encoded = mesh_nic::kEpochWorkComplete;
    break;
  case EpochContribution::Idle:
    encoded = mesh_nic::kEpochIdle;
    break;
  }
  return encoded != UINT32_MAX &&
         mesh_nic::arrive_epoch(completed_epoch, encoded);
}

const EpochBarrierTransport kEpochBarrierTransport{
    nullptr,
    arriveAndWaitEpoch,
};

bool seedGlobalModelInputs(const TileABI &abi) {
  constexpr uint64_t kStagingOffset = 0;
  void *scratchpad = reinterpret_cast<void *>(
      static_cast<uintptr_t>(ScratchpadPhysicalBase + kStagingOffset));
  return golem::platform::forEachMaterializedModelInputTransfer(
      abi, [scratchpad](const Resource &resource, uint64_t global_offset,
                        uint32_t byte_count) {
        return initializeResourceBuffer(resource, scratchpad, byte_count) &&
               golem::platform::globalRAMInitialize(global_offset,
                                                    kStagingOffset, byte_count);
      });
}

#if defined(GOLEM_ENABLE_TASK_TRACE)
void emitTaskTrace(void *, TaskTraceEvent event, uint32_t task_id,
                   ExecutionId execution_id) {
  mesh_nic::trace_task(static_cast<uint32_t>(event), task_id, execution_id);
}

DeploymentTrace kTaskTrace{nullptr, emitTaskTrace};
#endif

#ifndef GOLEM_RUNTIME_PROGRESS_SNAPSHOT_STEPS
#define GOLEM_RUNTIME_PROGRESS_SNAPSHOT_STEPS UINT64_C(1000000)
#endif
#ifndef GOLEM_RUNTIME_PROGRESS_WATCHDOG_STEPS
#define GOLEM_RUNTIME_PROGRESS_WATCHDOG_STEPS UINT64_C(10000000)
#endif
#ifndef GOLEM_RUNTIME_PROGRESS_TILE
#define GOLEM_RUNTIME_PROGRESS_TILE UINT32_MAX
#endif

void emitDeploymentProgress(void *,
                            const DeploymentProgressSnapshot &snapshot) {
  if (GOLEM_RUNTIME_PROGRESS_TILE != UINT32_MAX &&
      snapshot.core_id != GOLEM_RUNTIME_PROGRESS_TILE) {
    return;
  }
  uart_puts("SCULPTOR_RA_PROGRESS kind=");
  printUnsigned(snapshot.kind);
  uart_puts(" tile=");
  printUnsigned(snapshot.core_id);
  uart_puts(" steps=");
  printUnsigned(snapshot.step_count);
  uart_puts(" last_progress=");
  printUnsigned(snapshot.last_progress_step);
  uart_puts(" last_retirement=");
  printUnsigned(snapshot.last_retirement_step);
  uart_puts(" issued=");
  printUnsigned(snapshot.iterations_issued);
  uart_puts(" executed=");
  printUnsigned(snapshot.iterations_executed);
  uart_puts(" retired=");
  printUnsigned(snapshot.iterations_retired);
  uart_puts(" tasks_retired=");
  printUnsigned(snapshot.tasks_retired);
  uart_puts(" physical_global_ram_dma_submitted=");
  printUnsigned(snapshot.physical_global_ram_dma_submitted);
  uart_puts(" physical_global_ram_dma_completed=");
  printUnsigned(snapshot.physical_global_ram_dma_completed);
  uart_puts(" current_epoch=");
  printUnsigned(snapshot.active_epoch_id);
  uart_puts(" earliest_incomplete_epoch=");
  printUnsigned(snapshot.earliest_incomplete_epoch_id);
  uart_puts(" active_max=");
  printUnsigned(snapshot.maximum_active_slots);
  uart_puts(" active_loop=");
  printUnsigned(snapshot.active_loop_id);
  uart_puts(" next_issue=");
  printUnsigned(snapshot.active_next_issue);
  uart_puts(" active_retired=");
  printUnsigned(snapshot.active_retired_count);
  uart_puts(" active_total=");
  printUnsigned(snapshot.active_total_count);
  uart_puts(" active_count=");
  printUnsigned(snapshot.active_count);
  uart_puts(" ring_slots=");
  printUnsigned(snapshot.active_ring_slots);
  uart_puts(" ready_queue=");
  printUnsigned(snapshot.ready_queue_occupancy);
  uart_puts(" pending_receive=");
  printUnsigned(snapshot.pending_receive_routes);
  uart_puts(" pending_transmit=");
  printUnsigned(snapshot.pending_transmit);
  uart_puts(" pending_dma=");
  printUnsigned(snapshot.pending_dma);
  uart_puts(" wait_loop=");
  printUnsigned(snapshot.oldest_wait_loop_id);
  uart_puts(" wait_route=");
  printUnsigned(snapshot.oldest_wait_route_id);
  uart_puts(" wait_iteration=");
  printUnsigned(snapshot.oldest_wait_iteration);
  uart_puts(" wait_slot=");
  printUnsigned(snapshot.oldest_wait_slot);
  uart_puts(" wait_phase=");
  printUnsigned(snapshot.oldest_wait_phase);
  uart_puts(" active_receive_states=");
  printUnsigned(snapshot.active_receive_state_count);
  uart_puts(" reported_receive_states=");
  printUnsigned(snapshot.reported_receive_state_count);
  for (uint32_t receive = 0; receive < snapshot.reported_receive_state_count;
       ++receive) {
    const DeploymentProgressReceiveState &state =
        snapshot.receive_states[receive];
    uart_puts(" rx");
    printUnsigned(receive);
    uart_puts("_source=");
    printUnsigned(state.source_tile);
    uart_puts("_route=");
    printUnsigned(state.route_id);
    uart_puts("_iteration=");
    printUnsigned(state.logical_iteration);
    uart_puts("_phase=");
    printUnsigned(state.phase);
    uart_puts("_loop=");
    printUnsigned(state.loop_id);
    uart_puts("_slot=");
    printUnsigned(state.slot);
    uart_puts("_slot_tag=");
    printUnsigned(state.slot_iteration);
    uart_puts("_slot_phase=");
    printUnsigned(state.slot_phase);
    uart_puts("_slot_in=");
    printUnsigned(state.slot_pending_incoming);
    uart_puts("_slot_out=");
    printUnsigned(state.slot_pending_outgoing);
    uart_puts("_slot_dma=");
    printUnsigned(state.slot_pending_dma);
  }
  for (uint32_t slot = 0; slot < snapshot.slot_count; ++slot) {
    const DeploymentProgressSlot &state = snapshot.slots[slot];
    uart_puts(" slot");
    printUnsigned(slot);
    uart_puts("_tag=");
    printUnsigned(state.iteration_tag);
    uart_puts("_phase=");
    printUnsigned(state.phase);
    uart_puts("_in=");
    printUnsigned(state.pending_incoming);
    uart_puts("_out=");
    printUnsigned(state.pending_outgoing);
    uart_puts("_dma=");
    printUnsigned(state.pending_dma);
  }
  uart_putc('\n');
}

DeploymentDiagnostics kDeploymentDiagnostics{
    nullptr,
    emitDeploymentProgress,
    GOLEM_RUNTIME_PROGRESS_SNAPSHOT_STEPS,
    GOLEM_RUNTIME_PROGRESS_WATCHDOG_STEPS,
};

} // namespace

extern "C" int tile_main() {
  const TileABI abi = linkedTileABI();
  const bool global_ram_dma = (abi.abi_features & TileABIGlobalRAMDMA) != 0;
  void **model_inputs = allocatePointerTable(abi.model_input_count);
  void **model_outputs = allocatePointerTable(abi.model_output_count);
  if ((abi.model_input_count != 0 && model_inputs == nullptr) ||
      (abi.model_output_count != 0 && model_outputs == nullptr)) {
    uart_puts("SCULPTOR_RA_SIM_ERROR allocate model I/O table\n");
    return 1;
  }

  DeploymentTrace *deployment_trace = nullptr;
#if defined(GOLEM_ENABLE_TASK_TRACE)
  deployment_trace = &kTaskTrace;
#endif
  HeapProfile *heap_profile = nullptr;
#if defined(GOLEM_ENABLE_HEAP_PROFILE)
  HeapProfile heap_counters{};
  heap_profile = &heap_counters;
#endif
  DeploymentInitializationProfile *initialization_profile = nullptr;
#if defined(GOLEM_ENABLE_STARTUP_PROFILE)
  DeploymentInitializationProfile initialization_counters{};
  if (GOLEM_STARTUP_PROFILE_TILE == UINT32_MAX ||
      abi.core_id == GOLEM_STARTUP_PROFILE_TILE) {
    initialization_counters.read_cycle = readStartupCycle;
    initialization_profile = &initialization_counters;
  }
#endif
  DeploymentRuntime runtime{
      abi,
      kTransport,
      nullptr,
      deployment_trace,
      DeploymentTransmitPolicy::Blocking,
      heap_profile,
      kGlobalDMATransport,
      &kDeploymentDiagnostics,
      kEpochBarrierTransport,
      initialization_profile,
  };
  if (!runtime.initialize()) {
    uart_puts("SCULPTOR_RA_SIM_ERROR initialize code=");
    printUnsigned(static_cast<uint32_t>(runtime.error()));
    uart_puts(" abi_stage=");
    printUnsigned(tileABIValidationStage());
    uart_putc('\n');
    return 2;
  }
#if defined(GOLEM_ENABLE_STARTUP_PROFILE)
  if (initialization_profile != nullptr) {
    printStartupProfile(abi.core_id, *initialization_profile);
  }
#endif
  if (global_ram_dma && !seedGlobalModelInputs(abi)) {
    uart_puts("SCULPTOR_RA_SIM_ERROR seed global inputs\n");
    return 3;
  }
  for (uint32_t index = 0; !global_ram_dma && index < abi.model_input_count;
       ++index) {
    const ModelIO &input = abi.model_inputs[index];
    model_inputs[index] = allocateModelIO(abi, input, ResourceKind::ModelInput);
    if (model_inputs[index] == nullptr ||
        !initializeModelInput(abi, input, model_inputs[index])) {
      uart_puts("SCULPTOR_RA_SIM_ERROR allocate input\n");
      return 3;
    }
    if (!runtime.bindModelInput(input.model_index, model_inputs[index])) {
      uart_puts("SCULPTOR_RA_SIM_ERROR bind input\n");
      return 4;
    }
  }
  for (uint32_t index = 0; !global_ram_dma && index < abi.model_output_count;
       ++index) {
    const ModelIO &output = abi.model_outputs[index];
    model_outputs[index] =
        allocateModelIO(abi, output, ResourceKind::ModelOutput);
    if (model_outputs[index] == nullptr) {
      uart_puts("SCULPTOR_RA_SIM_ERROR allocate output\n");
      return 5;
    }
    if (!runtime.bindModelOutputResource(output.global_resource_id,
                                         model_outputs[index])) {
      uart_puts("SCULPTOR_RA_SIM_ERROR bind output\n");
      return 6;
    }
  }
  if (!runtime.boot()) {
    uart_puts("SCULPTOR_RA_SIM_ERROR boot\n");
    return 7;
  }
  mesh_nic::complete_memory_initialization();

  uart_puts("SCULPTOR_RA_INIT_PASS tile=");
  printUnsigned(abi.core_id);
  uart_putc('\n');

  bool wait_snapshot_reported = false;
  while (!runtime.complete() && !runtime.failed()) {
    const DeploymentStep step = runtime.step();
    if (step == DeploymentStep::WaitForReceive) {
      if (!wait_snapshot_reported) {
        runtime.reportProgressSnapshot();
        wait_snapshot_reported = true;
      }
      mesh_nic::wait_for_receive();
    } else if (step == DeploymentStep::WaitForTransmit) {
      if (!wait_snapshot_reported) {
        runtime.reportProgressSnapshot();
        wait_snapshot_reported = true;
      }
      mesh_nic::wait_for_transmit();
    } else {
      // A later wait is a new architectural state.  Retain only one
      // snapshot while a tile remains blocked at the same state, but
      // report again after any intervening progress so a deployment
      // watchdog does not leave us with a stale startup snapshot.
      wait_snapshot_reported = false;
    }
  }
  if (runtime.failed()) {
    uart_puts("SCULPTOR_RA_SIM_ERROR execute code=");
    printUnsigned(static_cast<uint32_t>(runtime.error()));
    if (runtime.error() == DeploymentError::InvalidFrame) {
      const InvalidFrameDiagnostic &diagnostic =
          runtime.invalidFrameDiagnostic();
      uart_puts(" reason=");
      printUnsigned(static_cast<uint32_t>(diagnostic.reason));
      uart_puts(" source=");
      printUnsigned(diagnostic.source_tile);
      uart_puts(" route=");
      printUnsigned(diagnostic.route_id);
      uart_puts(" iteration=");
      printUnsigned(diagnostic.logical_iteration);
      uart_puts(" expected=");
      printUnsigned(diagnostic.expected);
      uart_puts(" actual=");
      printUnsigned(diagnostic.actual);
    } else {
      const ShardDiagnostic &diagnostic = runtime.shardDiagnostic();
      uart_puts(" task=");
      printUnsigned(diagnostic.task_id);
      uart_puts(" loop=");
      printUnsigned(diagnostic.loop_id);
      uart_puts(" route_dma=");
      printUnsigned(diagnostic.route_dma_id);
      uart_puts(" iteration=");
      printUnsigned(diagnostic.logical_iteration);
      uart_puts(" slot=");
      printUnsigned(diagnostic.ring_slot);
      uart_puts(" expected=");
      printUnsigned(diagnostic.expected);
      uart_puts(" actual=");
      printUnsigned(diagnostic.actual);
    }
    uart_putc('\n');
    return 8;
  }
#if defined(GOLEM_ENABLE_OUTPUT_FINGERPRINT)
  for (uint32_t index = 0; index < abi.model_output_count; ++index) {
    const bool printed =
        global_ram_dma
            ? printGlobalModelOutput(abi, abi.core_id, abi.model_outputs[index])
            : printLocalModelOutput(abi, abi.core_id, abi.model_outputs[index],
                                    model_outputs[index]);
    if (!printed) {
      uart_puts("SCULPTOR_RA_SIM_ERROR fingerprint output\n");
      return 9;
    }
  }
#endif
#if defined(GOLEM_ENABLE_HEAP_PROFILE)
  printHeapProfile(abi.core_id, heap_counters);
#endif
  printMovementCounters(abi.core_id, runtime.movementCounters());
  printExecutionCounters(abi.core_id, runtime.executionCounters());
  uart_puts("SCULPTOR_RA_SIM_PASS\n");
  return 0;
}
