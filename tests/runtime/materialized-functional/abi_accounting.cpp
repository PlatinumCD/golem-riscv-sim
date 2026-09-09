#include "abi_accounting.h"

#include <stddef.h>

namespace mittens::materialized_test {
namespace {

constexpr uint32_t kPhysicalShardBytes = 4096;

bool checkedAdd(uint64_t left, uint64_t right, uint64_t *result) {
  if (result == nullptr || right > UINT64_MAX - left)
    return false;
  *result = left + right;
  return true;
}

bool checkedMultiply(uint64_t left, uint64_t right, uint64_t *result) {
  if (result == nullptr || (left != 0 && right > UINT64_MAX / left))
    return false;
  *result = left * right;
  return true;
}

bool validTableSlice(uint32_t offset, uint32_t count, uint32_t table_count) {
  return offset <= table_count && count <= table_count - offset;
}

bool hasGlobalBuffer(const golem::runtime::TileABI &abi,
                     uint32_t global_resource_id) {
  if (abi.global_buffer_count != 0 && abi.global_buffers == nullptr)
    return false;
  for (uint32_t index = 0; index < abi.global_buffer_count; ++index)
    if (abi.global_buffers[index].global_resource_id == global_resource_id)
      return true;
  return false;
}

bool accumulateDMA(uint64_t descriptor_multiplicity, uint64_t iterations,
                   uint64_t requests_per_iteration,
                   uint64_t bytes_per_iteration,
                   uint64_t full_per_iteration,
                   uint64_t tail_per_iteration,
                   DMAAccounting *accounting) {
  uint64_t execution_count = 0;
  uint64_t descriptor_requests = 0;
  uint64_t descriptor_bytes = 0;
  uint64_t descriptor_full = 0;
  uint64_t descriptor_tail = 0;
  return accounting != nullptr && descriptor_multiplicity != 0 &&
         iterations != 0 &&
         checkedMultiply(descriptor_multiplicity, iterations,
                         &execution_count) &&
         checkedMultiply(requests_per_iteration, execution_count,
                         &descriptor_requests) &&
         checkedMultiply(bytes_per_iteration, execution_count,
                         &descriptor_bytes) &&
         checkedMultiply(full_per_iteration, execution_count,
                         &descriptor_full) &&
         checkedMultiply(tail_per_iteration, execution_count,
                         &descriptor_tail) &&
         checkedAdd(accounting->requests, descriptor_requests,
                    &accounting->requests) &&
         checkedAdd(accounting->bytes, descriptor_bytes,
                    &accounting->bytes) &&
         checkedAdd(accounting->full_requests, descriptor_full,
                    &accounting->full_requests) &&
         checkedAdd(accounting->tail_requests, descriptor_tail,
                    &accounting->tail_requests);
}

bool accumulateAffineRun(
    const golem::runtime::TileABI &abi,
    const golem::runtime::MaterializedDMAAffineSingleSegmentRun &run,
    DMAAccounting *accounting) {
  using namespace golem::runtime;
  if (accounting == nullptr || run.family_count == 0 ||
      run.descriptors_per_family == 0 || run.iteration_span == 0 ||
      run.iteration_step == 0 || run.segment_byte_size == 0 ||
      run.segment_byte_size > kPhysicalShardBytes ||
      run.segment_repeat_count == 0 || run.segment_piece_count == 0 ||
      run.direction != ScratchpadDMADirection::GlobalRAMToScratchpad ||
      (run.descriptor_flags & MaterializedDMAEpochZeroSource) != 0 ||
      run.global_buffer_index >= abi.global_buffer_count ||
      abi.global_buffers == nullptr ||
      abi.global_buffers[run.global_buffer_index].global_resource_id !=
          run.global_resource_id)
    return false;

  uint64_t descriptor_count = 0;
  uint64_t requests_per_iteration = 0;
  uint64_t bytes_per_iteration = 0;
  if (!checkedMultiply(run.family_count, run.descriptors_per_family,
                       &descriptor_count) ||
      !checkedMultiply(run.segment_repeat_count, run.segment_piece_count,
                       &requests_per_iteration) ||
      !checkedMultiply(requests_per_iteration, run.segment_byte_size,
                       &bytes_per_iteration) ||
      bytes_per_iteration != run.descriptor_byte_size)
    return false;
  const uint64_t iterations =
      1U + (run.iteration_span - 1U) / run.iteration_step;
  const uint64_t full_per_iteration =
      run.segment_byte_size == kPhysicalShardBytes ? requests_per_iteration : 0;
  const uint64_t tail_per_iteration =
      run.segment_byte_size == kPhysicalShardBytes ? 0 : requests_per_iteration;
  return accumulateDMA(descriptor_count, iterations, requests_per_iteration,
                       bytes_per_iteration, full_per_iteration,
                       tail_per_iteration, accounting);
}

struct PeriodicRunSummary {
  uint64_t descriptor_count = 0;
  uint64_t sequence_count = 0;
  uint64_t segment_count = 0;
  DMAAccounting accounting{};
};

bool summarizePeriodicRun(
    const golem::runtime::TileABI &abi, uint32_t run_index,
    const golem::runtime::MaterializedDMAPeriodicSegmentRun &run,
    PeriodicRunSummary *summary) {
  using namespace golem::runtime;
  if (summary == nullptr || run.id != run_index || run.sequence_count < 2 ||
      run.descriptors_per_sequence < 2 || run.residue_period == 0 ||
      run.residue_period > run.sequence_count || run.phase_count == 0 ||
      run.residue_pattern_count != run.residue_period ||
      run.segment_template_count == 0 || run.iteration_step == 0 ||
      run.descriptor_byte_size == 0 ||
      run.direction != ScratchpadDMADirection::GlobalRAMToScratchpad ||
      run.template_kind != MaterializedDMATemplateKind::Main ||
      (run.descriptor_flags & MaterializedDMAEpochZeroSource) != 0 ||
      !validTableSlice(run.phase_offset, run.phase_count,
                       abi.materialized_dma_periodic_phase_count) ||
      !validTableSlice(run.residue_pattern_offset,
                       run.residue_pattern_count,
                       abi.materialized_dma_periodic_residue_pattern_count) ||
      !validTableSlice(run.segment_template_offset,
                       run.segment_template_count,
                       abi.materialized_dma_periodic_segment_template_count) ||
      abi.materialized_dma_periodic_phases == nullptr ||
      abi.materialized_dma_periodic_residue_patterns == nullptr ||
      abi.materialized_dma_periodic_segment_templates == nullptr ||
      abi.global_buffer_count == 0 || abi.global_buffers == nullptr)
    return false;

  *summary = {};
  if (!checkedMultiply(run.sequence_count, run.descriptors_per_sequence,
                       &summary->descriptor_count))
    return false;
  summary->sequence_count = run.sequence_count;

  // Every sequence shares the same descriptor-phase geometry. Aggregate
  // descriptor executions once per sequence instead of expanding the
  // sequence x descriptor logical domain.
  uint64_t descriptor_executions_per_sequence = 0;
  uint32_t expected_inner_begin = 0;
  for (uint32_t ordinal = 0; ordinal < run.phase_count; ++ordinal) {
    const MaterializedDMAPeriodicDescriptorPhase &phase =
        abi.materialized_dma_periodic_phases[run.phase_offset + ordinal];
    if (phase.run_id != run.id || phase.reserved != 0 ||
        phase.inner_begin != expected_inner_begin || phase.inner_count == 0 ||
        phase.inner_count >
            run.descriptors_per_sequence - expected_inner_begin ||
        phase.iteration_span == 0)
      return false;
    const uint64_t iterations =
        1U + (phase.iteration_span - 1U) / run.iteration_step;
    uint64_t phase_executions = 0;
    if (!checkedMultiply(phase.inner_count, iterations, &phase_executions) ||
        !checkedAdd(descriptor_executions_per_sequence, phase_executions,
                    &descriptor_executions_per_sequence))
      return false;
    expected_inner_begin += phase.inner_count;
  }
  if (expected_inner_begin != run.descriptors_per_sequence ||
      descriptor_executions_per_sequence == 0)
    return false;

  // A residue pattern is selected by sequenceOrdinal % residuePeriod. Count
  // how many sequences select each pattern, then account its templates once.
  // This is linear in phase/pattern/template metadata regardless of the
  // logical descriptor or segment count represented by the run.
  uint32_t expected_template_count = 0;
  for (uint32_t residue = 0; residue < run.residue_period; ++residue) {
    const MaterializedDMAPeriodicResiduePattern &pattern =
        abi.materialized_dma_periodic_residue_patterns[
            run.residue_pattern_offset + residue];
    if (pattern.run_id != run.id || pattern.residue_ordinal != residue ||
        pattern.reserved != 0 || pattern.segment_template_count == 0 ||
        pattern.prefix_segment_count != expected_template_count ||
        pattern.segment_template_offset !=
            run.segment_template_offset + expected_template_count ||
        pattern.segment_template_count >
            run.segment_template_count - expected_template_count)
      return false;

    uint64_t requests_per_iteration = 0;
    uint64_t bytes_per_iteration = 0;
    uint64_t full_per_iteration = 0;
    uint64_t tail_per_iteration = 0;
    for (uint32_t template_ordinal = 0;
         template_ordinal < pattern.segment_template_count;
         ++template_ordinal) {
      const MaterializedDMAPeriodicSegmentTemplate &segment_template =
          abi.materialized_dma_periodic_segment_templates[
              pattern.segment_template_offset + template_ordinal];
      if (segment_template.run_id != run.id ||
          segment_template.byte_size == 0 ||
          segment_template.byte_size > kPhysicalShardBytes ||
          segment_template.repeat_count == 0 ||
          segment_template.piece_count == 0 ||
          segment_template.global_buffer_index >= abi.global_buffer_count ||
          abi.global_buffers[segment_template.global_buffer_index]
                  .global_resource_id != segment_template.global_resource_id)
        return false;
      uint64_t requests = 0;
      uint64_t bytes = 0;
      if (!checkedMultiply(segment_template.repeat_count,
                           segment_template.piece_count, &requests) ||
          !checkedMultiply(requests, segment_template.byte_size, &bytes) ||
          !checkedAdd(requests_per_iteration, requests,
                      &requests_per_iteration) ||
          !checkedAdd(bytes_per_iteration, bytes, &bytes_per_iteration))
        return false;
      uint64_t *classification =
          segment_template.byte_size == kPhysicalShardBytes
              ? &full_per_iteration
              : &tail_per_iteration;
      if (!checkedAdd(*classification, requests, classification))
        return false;
    }
    if (bytes_per_iteration != run.descriptor_byte_size)
      return false;

    const uint64_t sequence_occurrences =
        run.sequence_count / run.residue_period +
        (residue < run.sequence_count % run.residue_period ? 1U : 0U);
    uint64_t residue_segments = 0;
    if (!checkedMultiply(sequence_occurrences,
                         run.descriptors_per_sequence, &residue_segments) ||
        !checkedMultiply(residue_segments, pattern.segment_template_count,
                         &residue_segments) ||
        !checkedAdd(summary->segment_count, residue_segments,
                    &summary->segment_count) ||
        !accumulateDMA(sequence_occurrences,
                       descriptor_executions_per_sequence,
                       requests_per_iteration, bytes_per_iteration,
                       full_per_iteration, tail_per_iteration,
                       &summary->accounting))
      return false;
    expected_template_count += pattern.segment_template_count;
  }
  return expected_template_count == run.segment_template_count &&
         summary->segment_count != 0;
}

bool accumulatePeriodicRun(
    const golem::runtime::TileABI &abi, uint32_t run_index,
    const golem::runtime::MaterializedDMAPeriodicSegmentRun &run,
    DMAAccounting *accounting) {
  PeriodicRunSummary summary{};
  return accounting != nullptr &&
         summarizePeriodicRun(abi, run_index, run, &summary) &&
         checkedAdd(accounting->requests, summary.accounting.requests,
                    &accounting->requests) &&
         checkedAdd(accounting->bytes, summary.accounting.bytes,
                    &accounting->bytes) &&
         checkedAdd(accounting->full_requests,
                    summary.accounting.full_requests,
                    &accounting->full_requests) &&
         checkedAdd(accounting->tail_requests,
                    summary.accounting.tail_requests,
                    &accounting->tail_requests);
}

bool validAffineAccountingShape(const golem::runtime::TileABI &abi) {
  using namespace golem::runtime;
  const bool enabled =
      (abi.abi_features & TileABIMaterializedDMAAffineRuns) != 0;
  if (!enabled)
    return abi.materialized_dma_affine_run_count == 0 &&
           abi.materialized_dma_periodic_run_count == 0 &&
           abi.materialized_dma_periodic_phase_count == 0 &&
           abi.materialized_dma_periodic_residue_pattern_count == 0 &&
           abi.materialized_dma_periodic_segment_template_count == 0 &&
           abi.materialized_dma_affine_run_certificate == nullptr;
  const MaterializedDMAAffineRunCertificate *certificate =
      abi.materialized_dma_affine_run_certificate;
  if (certificate == nullptr ||
      certificate->version != MaterializedDMAAffineRunCertificateVersion ||
      certificate->single_segment_run_count !=
          abi.materialized_dma_affine_run_count ||
      certificate->periodic_segment_run_count !=
          abi.materialized_dma_periodic_run_count ||
      certificate->periodic_phase_count !=
          abi.materialized_dma_periodic_phase_count ||
      certificate->periodic_residue_pattern_count !=
          abi.materialized_dma_periodic_residue_pattern_count ||
      certificate->periodic_segment_template_count !=
          abi.materialized_dma_periodic_segment_template_count ||
      certificate->loop_index_count !=
          abi.materialized_dma_affine_run_loop_index_count ||
      certificate->literal_descriptor_count !=
          abi.materialized_dma_descriptor_count ||
      certificate->literal_sequence_count !=
          abi.materialized_dma_sequence_count ||
      certificate->literal_segment_count != abi.materialized_dma_segment_count ||
      (abi.materialized_dma_affine_run_count != 0 &&
       abi.materialized_dma_affine_runs == nullptr) ||
      (abi.materialized_dma_periodic_run_count != 0 &&
       abi.materialized_dma_periodic_runs == nullptr) ||
      (abi.materialized_dma_periodic_phase_count != 0 &&
       abi.materialized_dma_periodic_phases == nullptr) ||
      (abi.materialized_dma_periodic_residue_pattern_count != 0 &&
       abi.materialized_dma_periodic_residue_patterns == nullptr) ||
      (abi.materialized_dma_periodic_segment_template_count != 0 &&
       abi.materialized_dma_periodic_segment_templates == nullptr))
    return false;

  uint64_t covered_descriptors = 0;
  uint64_t covered_sequences = 0;
  uint64_t covered_segments = 0;
  for (uint32_t index = 0; index < abi.materialized_dma_affine_run_count;
       ++index) {
    const MaterializedDMAAffineSingleSegmentRun &run =
        abi.materialized_dma_affine_runs[index];
    uint64_t records = 0;
    if (!checkedMultiply(run.family_count, run.descriptors_per_family,
                         &records) ||
        records == 0 ||
        !checkedAdd(covered_descriptors, records, &covered_descriptors) ||
        !checkedAdd(covered_sequences, records, &covered_sequences) ||
        !checkedAdd(covered_segments, records, &covered_segments))
      return false;
  }
  for (uint32_t index = 0; index < abi.materialized_dma_periodic_run_count;
       ++index) {
    PeriodicRunSummary summary{};
    if (!summarizePeriodicRun(abi, index,
                              abi.materialized_dma_periodic_runs[index],
                              &summary) ||
        !checkedAdd(covered_descriptors, summary.descriptor_count,
                    &covered_descriptors) ||
        !checkedAdd(covered_sequences, summary.sequence_count,
                    &covered_sequences) ||
        !checkedAdd(covered_segments, summary.segment_count,
                    &covered_segments))
      return false;
  }
  return covered_descriptors <= UINT32_MAX &&
         covered_sequences <= UINT32_MAX && covered_segments <= UINT32_MAX &&
         certificate->covered_descriptor_count == covered_descriptors &&
         certificate->covered_sequence_count == covered_sequences &&
         certificate->covered_segment_count == covered_segments &&
         certificate->logical_descriptor_count ==
             static_cast<uint64_t>(certificate->literal_descriptor_count) +
                 covered_descriptors &&
         certificate->logical_sequence_count ==
             static_cast<uint64_t>(certificate->literal_sequence_count) +
                 covered_sequences &&
         certificate->logical_segment_count ==
             static_cast<uint64_t>(certificate->literal_segment_count) +
                 covered_segments;
}

} // namespace

bool validMaterializedABIShape(const golem::runtime::TileABI &abi) {
  using namespace golem::runtime;
  return abi.abi_version == TileABIVersionCurrent &&
         (abi.abi_features & TileABIMaterializedDataflow) != 0 &&
         (abi.abi_features & TileABIEpochSchedule) != 0 &&
         (abi.abi_features & TileABIGlobalRAMDMA) != 0 &&
         abi.epoch_count >= 2 && abi.shard_loops != nullptr &&
         abi.shard_loop_count != 0 && abi.global_buffers != nullptr &&
         abi.global_buffer_count != 0 &&
         (abi.materialized_dma_descriptor_count == 0 ||
          abi.materialized_dma_descriptors != nullptr) &&
         abi.logicalMaterializedDMADescriptorCount() != 0 &&
         (abi.materialized_dma_segment_count == 0 ||
          abi.materialized_dma_segments != nullptr) &&
         abi.logicalMaterializedDMASegmentCount() != 0 &&
         (abi.materialized_dma_affine_run_count == 0 ||
          abi.materialized_dma_affine_runs != nullptr) &&
         (abi.materialized_dma_periodic_run_count == 0 ||
          abi.materialized_dma_periodic_runs != nullptr) &&
         validAffineAccountingShape(abi) &&
         abi.parametric_route_count == 0 &&
         abi.parametric_dma_descriptor_count == 0 &&
         abi.scratchpad_required_bytes <= ScratchpadMaximumBytes;
}

bool expectedDMAAccounting(const golem::runtime::TileABI &abi,
                           DMAAccounting *accounting) {
  using namespace golem::runtime;
  if (accounting == nullptr)
    return false;
  *accounting = {};
  const uint32_t logical_segment_count =
      abi.logicalMaterializedDMASegmentCount();
  if ((abi.materialized_dma_descriptor_count != 0 &&
       abi.materialized_dma_descriptors == nullptr) ||
      (abi.materialized_dma_segment_count != 0 &&
       abi.materialized_dma_segments == nullptr) ||
      abi.global_buffer_count == 0 || abi.global_buffers == nullptr ||
      !validAffineAccountingShape(abi))
    return false;
  for (uint32_t index = 0;
       index < abi.materialized_dma_descriptor_count; ++index) {
    const MaterializedDMADescriptor &descriptor =
        abi.materialized_dma_descriptors[index];
    if (descriptor.iteration_step == 0 ||
        descriptor.iteration_begin >= descriptor.iteration_end ||
        descriptor.segment_count == 0 ||
        descriptor.segment_offset > logical_segment_count ||
        descriptor.segment_count >
            logical_segment_count - descriptor.segment_offset)
      return false;
    const uint64_t iterations =
        1U + (descriptor.iteration_end - descriptor.iteration_begin - 1U) /
                 descriptor.iteration_step;
    uint64_t bytes_per_iteration = 0;
    uint64_t requests_per_iteration = 0;
    uint64_t full_per_iteration = 0;
    uint64_t tail_per_iteration = 0;
    for (uint32_t segment_index = 0;
         segment_index < descriptor.segment_count; ++segment_index) {
      const uint32_t segment_id = descriptor.segment_offset + segment_index;
      MaterializedDMASegment segment{};
      if (!abi.resolveMaterializedDMASegment(segment_id, &segment) ||
          segment.id != segment_id ||
          segment.descriptor_id != descriptor.id || segment.byte_size == 0 ||
          segment.byte_size > kPhysicalShardBytes ||
          segment.repeat_count == 0 || segment.piece_count == 0 ||
          !hasGlobalBuffer(abi, segment.global_resource_id))
        return false;
      uint64_t requests = 0;
      uint64_t bytes = 0;
      if (!checkedMultiply(segment.repeat_count, segment.piece_count,
                           &requests) ||
          !checkedMultiply(requests, segment.byte_size, &bytes) ||
          !checkedAdd(requests_per_iteration, requests,
                      &requests_per_iteration) ||
          !checkedAdd(bytes_per_iteration, bytes, &bytes_per_iteration))
        return false;
      uint64_t *classification = segment.byte_size == kPhysicalShardBytes
                                     ? &full_per_iteration
                                     : &tail_per_iteration;
      if (!checkedAdd(*classification, requests, classification))
        return false;
    }
    if (bytes_per_iteration != descriptor.bytes_per_iteration)
      return false;
    if (!accumulateDMA(1, iterations, requests_per_iteration,
                       bytes_per_iteration, full_per_iteration,
                       tail_per_iteration, accounting))
      return false;
  }
  for (uint32_t index = 0; index < abi.materialized_dma_affine_run_count;
       ++index)
    if (!accumulateAffineRun(abi, abi.materialized_dma_affine_runs[index],
                             accounting))
      return false;
  for (uint32_t index = 0; index < abi.materialized_dma_periodic_run_count;
       ++index)
    if (!accumulatePeriodicRun(abi, index,
                               abi.materialized_dma_periodic_runs[index],
                               accounting))
      return false;
  uint64_t classified = 0;
  return checkedAdd(accounting->full_requests, accounting->tail_requests,
                    &classified) &&
         classified == accounting->requests && accounting->requests != 0;
}

bool expectedShardIterations(const golem::runtime::TileABI &abi,
                             uint64_t *iterations) {
  using namespace golem::runtime;
  if (iterations == nullptr)
    return false;
  *iterations = 0;
  for (uint32_t index = 0; index < abi.shard_loop_count; ++index) {
    const ShardLoop &loop = abi.shard_loops[index];
    if (loop.iteration_step == 0 || loop.iteration_begin >= loop.iteration_end)
      return false;
    const uint64_t count =
        1U + (loop.iteration_end - loop.iteration_begin - 1U) /
                 loop.iteration_step;
    if (!checkedAdd(*iterations, count, iterations))
      return false;
  }
  return *iterations != 0;
}

} // namespace mittens::materialized_test
