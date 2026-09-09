#include <stdint.h>

#include "abi_accounting.h"

namespace {

using namespace golem::runtime;
using mittens::materialized_test::DMAAccounting;

int check(bool condition, int code) { return condition ? 0 : code; }

} // namespace

int main() {
  GlobalBuffer buffer{};
  buffer.global_resource_id = 7;
  buffer.byte_size = 4128;

  MaterializedDMASegment segments[2]{};
  segments[0].id = 0;
  segments[0].descriptor_id = 0;
  segments[0].global_resource_id = 7;
  segments[0].byte_size = 4096;
  segments[0].repeat_count = 1;
  segments[0].piece_count = 1;
  segments[1].id = 1;
  segments[1].id = 3;
  segments[1].descriptor_id = 3;
  segments[1].global_resource_id = 7;
  segments[1].byte_size = 32;
  segments[1].repeat_count = 1;
  segments[1].piece_count = 1;

  MaterializedDMADescriptor descriptors[2]{};
  for (uint32_t index = 0; index < 2; ++index) {
    descriptors[index].id = index == 0 ? 0 : 3;
    descriptors[index].iteration_begin = 0;
    descriptors[index].iteration_end = 1;
    descriptors[index].iteration_step = 1;
    descriptors[index].segment_offset = index == 0 ? 0 : 3;
    descriptors[index].segment_count = 1;
  }
  descriptors[0].bytes_per_iteration = 4096;
  descriptors[1].bytes_per_iteration = 32;

  ShardLoop loop{};
  loop.iteration_begin = 0;
  loop.iteration_end = 1;
  loop.iteration_step = 1;
  loop.ring_slots = 1;
  loop.epoch_id = 1;

  MaterializedDMAAffineSingleSegmentRun affine_run{};
  affine_run.family_count = 1;
  affine_run.descriptors_per_family = 2;
  affine_run.direction = ScratchpadDMADirection::GlobalRAMToScratchpad;
  affine_run.descriptor_id_base = 1;
  affine_run.sequence_id_base = 0;
  affine_run.segment_id_base = 1;
  affine_run.global_resource_id = 7;
  affine_run.global_buffer_index = 0;
  affine_run.descriptor_byte_size = 128;
  affine_run.segment_byte_size = 64;
  affine_run.segment_repeat_count = 1;
  affine_run.segment_piece_count = 2;
  affine_run.iteration_span = 2;
  affine_run.iteration_step = 1;

  // Five logical sequences select two alternating segment patterns. Two
  // descriptor phases account for 2*2 + 1*3 = 7 descriptor executions per
  // sequence without materializing the represented D/Q/S records.
  MaterializedDMAPeriodicSegmentRun periodic_run{};
  periodic_run.id = 0;
  periodic_run.sequence_count = 5;
  periodic_run.descriptors_per_sequence = 3;
  periodic_run.residue_period = 2;
  periodic_run.phase_count = 2;
  periodic_run.residue_pattern_count = 2;
  periodic_run.segment_template_count = 3;
  periodic_run.sequence_id_base = 2;
  periodic_run.descriptor_id_base = 4;
  periodic_run.segment_id_base = 4;
  periodic_run.direction = ScratchpadDMADirection::GlobalRAMToScratchpad;
  periodic_run.template_kind = MaterializedDMATemplateKind::Main;
  periodic_run.descriptor_byte_size = 4096;
  periodic_run.iteration_step = 1;

  MaterializedDMAPeriodicDescriptorPhase periodic_phases[2]{};
  periodic_phases[0].run_id = 0;
  periodic_phases[0].inner_begin = 0;
  periodic_phases[0].inner_count = 2;
  periodic_phases[0].iteration_span = 2;
  periodic_phases[1].run_id = 0;
  periodic_phases[1].inner_begin = 2;
  periodic_phases[1].inner_count = 1;
  periodic_phases[1].iteration_span = 3;

  MaterializedDMAPeriodicResiduePattern periodic_patterns[2]{};
  periodic_patterns[0].run_id = 0;
  periodic_patterns[0].residue_ordinal = 0;
  periodic_patterns[0].segment_template_offset = 0;
  periodic_patterns[0].segment_template_count = 1;
  periodic_patterns[0].prefix_segment_count = 0;
  periodic_patterns[1].run_id = 0;
  periodic_patterns[1].residue_ordinal = 1;
  periodic_patterns[1].segment_template_offset = 1;
  periodic_patterns[1].segment_template_count = 2;
  periodic_patterns[1].prefix_segment_count = 1;

  MaterializedDMAPeriodicSegmentTemplate periodic_templates[3]{};
  for (uint32_t index = 0; index < 3; ++index) {
    periodic_templates[index].run_id = 0;
    periodic_templates[index].global_resource_id = 7;
    periodic_templates[index].global_buffer_index = 0;
    periodic_templates[index].repeat_count = 1;
    periodic_templates[index].piece_count = 1;
  }
  periodic_templates[0].byte_size = 4096;
  periodic_templates[1].byte_size = 2048;
  periodic_templates[2].byte_size = 2048;

  MaterializedDMAAffineRunCertificate affine_certificate{};
  affine_certificate.version = MaterializedDMAAffineRunCertificateVersion;
  affine_certificate.single_segment_run_count = 1;
  affine_certificate.periodic_segment_run_count = 1;
  affine_certificate.periodic_phase_count = 2;
  affine_certificate.periodic_residue_pattern_count = 2;
  affine_certificate.periodic_segment_template_count = 3;
  affine_certificate.logical_descriptor_count = 19;
  affine_certificate.covered_descriptor_count = 17;
  affine_certificate.literal_descriptor_count = 2;
  affine_certificate.logical_sequence_count = 7;
  affine_certificate.covered_sequence_count = 7;
  affine_certificate.literal_sequence_count = 0;
  affine_certificate.logical_segment_count = 25;
  affine_certificate.covered_segment_count = 23;
  affine_certificate.literal_segment_count = 2;

  TileABI abi{};
  abi.abi_version = TileABIVersionCurrent;
  abi.abi_features = TileABIMaterializedDataflow | TileABIEpochSchedule |
                     TileABIGlobalRAMDMA |
                     TileABIMaterializedDMAAffineRuns;
  abi.epoch_count = 2;
  abi.shard_loops = &loop;
  abi.shard_loop_count = 1;
  abi.global_buffers = &buffer;
  abi.global_buffer_count = 1;
  abi.materialized_dma_descriptors = descriptors;
  abi.materialized_dma_descriptor_count = 2;
  abi.materialized_dma_segments = segments;
  abi.materialized_dma_segment_count = 2;
  abi.materialized_dma_affine_runs = &affine_run;
  abi.materialized_dma_affine_run_count = 1;
  abi.materialized_dma_periodic_runs = &periodic_run;
  abi.materialized_dma_periodic_run_count = 1;
  abi.materialized_dma_periodic_phases = periodic_phases;
  abi.materialized_dma_periodic_phase_count = 2;
  abi.materialized_dma_periodic_residue_patterns = periodic_patterns;
  abi.materialized_dma_periodic_residue_pattern_count = 2;
  abi.materialized_dma_periodic_segment_templates = periodic_templates;
  abi.materialized_dma_periodic_segment_template_count = 3;
  abi.materialized_dma_affine_run_certificate = &affine_certificate;
  abi.scratchpad_required_bytes = 4096;

  if (int result = check(
          mittens::materialized_test::validMaterializedABIShape(abi), 1))
    return result;
  DMAAccounting accounting{};
  if (int result = check(
          mittens::materialized_test::expectedDMAAccounting(
              abi, &accounting) &&
              accounting.full_requests == 22 &&
              accounting.tail_requests == 37 && accounting.requests == 59 &&
              accounting.bytes == 148000,
          2))
    return result;
  uint64_t iterations = 0;
  if (int result = check(
          mittens::materialized_test::expectedShardIterations(
              abi, &iterations) &&
              iterations == 1,
          3))
    return result;

  abi.abi_version = TileABIVersionEpochs;
  if (int result = check(
          !mittens::materialized_test::validMaterializedABIShape(abi), 4))
    return result;
  abi.abi_version = TileABIVersionCurrent;
  abi.parametric_route_count = 1;
  if (int result = check(
          !mittens::materialized_test::validMaterializedABIShape(abi), 5))
    return result;
  abi.parametric_route_count = 0;
  abi.parametric_dma_descriptor_count = 1;
  if (int result = check(
          !mittens::materialized_test::validMaterializedABIShape(abi), 6))
    return result;
  abi.parametric_dma_descriptor_count = 0;

  abi.materialized_dma_descriptor_count = 0;
  if (int result = check(
          !mittens::materialized_test::validMaterializedABIShape(abi), 7))
    return result;
  abi.materialized_dma_descriptor_count = 2;

  abi.materialized_dma_segments = nullptr;
  if (int result = check(
          !mittens::materialized_test::validMaterializedABIShape(abi), 8))
    return result;
  abi.materialized_dma_segments = segments;

  segments[0].byte_size = 4097;
  descriptors[0].bytes_per_iteration = 4097;
  if (int result = check(
          !mittens::materialized_test::expectedDMAAccounting(
              abi, &accounting),
          9))
    return result;
  segments[0].byte_size = 4096;
  descriptors[0].bytes_per_iteration = 4095;
  if (int result = check(
          !mittens::materialized_test::expectedDMAAccounting(
              abi, &accounting),
          10))
    return result;
  descriptors[0].bytes_per_iteration = 4096;
  segments[0].descriptor_id = 3;
  if (int result = check(
          !mittens::materialized_test::expectedDMAAccounting(
              abi, &accounting),
          11))
    return result;
  segments[0].descriptor_id = 0;
  segments[0].repeat_count = UINT32_MAX;
  segments[0].piece_count = UINT32_MAX;
  if (int result = check(
          !mittens::materialized_test::expectedDMAAccounting(
              abi, &accounting),
          12))
    return result;

  segments[0].repeat_count = 1;
  segments[0].piece_count = 1;
  affine_run.descriptor_byte_size = 127;
  if (int result = check(
          !mittens::materialized_test::expectedDMAAccounting(abi, &accounting),
          13))
    return result;
  affine_run.descriptor_byte_size = 128;
  affine_run.direction = ScratchpadDMADirection::ScratchpadToGlobalRAM;
  if (int result = check(
          !mittens::materialized_test::expectedDMAAccounting(abi, &accounting),
          14))
    return result;
  affine_run.direction = ScratchpadDMADirection::GlobalRAMToScratchpad;

  // Periodic compression is categorically input-only and excludes deployment
  // epoch-zero inputs. Keeping both fail-closed properties here proves the
  // model-input and output-ownership consumers only need physical literals.
  periodic_run.direction = ScratchpadDMADirection::ScratchpadToGlobalRAM;
  if (int result = check(
          !mittens::materialized_test::expectedDMAAccounting(abi, &accounting),
          15))
    return result;
  periodic_run.direction = ScratchpadDMADirection::GlobalRAMToScratchpad;
  periodic_run.descriptor_flags = MaterializedDMAEpochZeroSource;
  if (int result = check(
          !mittens::materialized_test::expectedDMAAccounting(abi, &accounting),
          16))
    return result;
  periodic_run.descriptor_flags = 0;

  periodic_templates[2].byte_size = 1024;
  if (int result = check(
          !mittens::materialized_test::expectedDMAAccounting(abi, &accounting),
          17))
    return result;
  periodic_templates[2].byte_size = 2048;
  periodic_templates[2].global_buffer_index = 1;
  if (int result = check(
          !mittens::materialized_test::expectedDMAAccounting(abi, &accounting),
          18))
    return result;
  periodic_templates[2].global_buffer_index = 0;

  periodic_phases[1].inner_begin = 1;
  if (int result = check(
          !mittens::materialized_test::expectedDMAAccounting(abi, &accounting),
          19))
    return result;
  periodic_phases[1].inner_begin = 2;
  --affine_certificate.covered_segment_count;
  if (int result = check(
          !mittens::materialized_test::validMaterializedABIShape(abi), 20))
    return result;
  ++affine_certificate.covered_segment_count;

  return 0;
}
