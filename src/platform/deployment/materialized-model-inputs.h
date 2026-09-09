#pragma once

#include <stdint.h>

#include "golem/runtime/tile_abi.h"

namespace golem::platform {

// Compact accounting for the authoritative deployment-input transfers encoded
// by the materialized DMA ABI.  A transfer is one physical request represented
// by a descriptor/segment iteration, repeat, and piece coordinate.
struct MaterializedModelInputPlanStats {
  uint64_t descriptor_count = 0;
  uint64_t iteration_count = 0;
  uint64_t transfer_count = 0;
  uint64_t byte_count = 0;
};

namespace materialized_model_input_detail {

inline bool checkedAdd(uint64_t left, uint64_t right, uint64_t *result) {
  if (result == nullptr || right > UINT64_MAX - left) {
    return false;
  }
  *result = left + right;
  return true;
}

inline bool checkedMultiply(uint64_t left, uint64_t right, uint64_t *result) {
  if (result == nullptr || (left != 0 && right > UINT64_MAX / left)) {
    return false;
  }
  *result = left * right;
  return true;
}

inline bool checkedScaledAdd(uint64_t base, uint64_t stride, uint64_t ordinal,
                             uint64_t *result) {
  uint64_t scaled = 0;
  return checkedMultiply(stride, ordinal, &scaled) &&
         checkedAdd(base, scaled, result);
}

inline const runtime::ModelIO *findUniqueModelInput(const runtime::TileABI &abi,
                                                    uint32_t local_slot) {
  if (abi.model_input_count != 0 && abi.model_inputs == nullptr) {
    return nullptr;
  }
  const runtime::ModelIO *match = nullptr;
  for (uint32_t index = 0; index < abi.model_input_count; ++index) {
    const runtime::ModelIO &input = abi.model_inputs[index];
    if (input.local_slot != local_slot) {
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = &input;
  }
  return match;
}

inline const runtime::Resource *findResource(const runtime::TileABI &abi,
                                             uint32_t local_slot) {
  if (abi.resource_count != 0 && abi.resources == nullptr) {
    return nullptr;
  }
  for (uint32_t index = 0; index < abi.resource_count; ++index) {
    if (abi.resources[index].local_slot == local_slot) {
      return &abi.resources[index];
    }
  }
  return nullptr;
}

inline const runtime::GlobalBuffer *
findGlobalBuffer(const runtime::TileABI &abi, uint32_t global_resource_id) {
  if (abi.global_buffer_count != 0 && abi.global_buffers == nullptr) {
    return nullptr;
  }
  for (uint32_t index = 0; index < abi.global_buffer_count; ++index) {
    if (abi.global_buffers[index].global_resource_id == global_resource_id) {
      return &abi.global_buffers[index];
    }
  }
  return nullptr;
}

inline bool modelInputHasEpochZeroDescriptor(const runtime::TileABI &abi,
                                             const runtime::ModelIO &input) {
  if (abi.materialized_dma_descriptor_count != 0 &&
      abi.materialized_dma_descriptors == nullptr) {
    return false;
  }
  for (uint32_t index = 0; index < abi.materialized_dma_descriptor_count;
       ++index) {
    const runtime::MaterializedDMADescriptor &descriptor =
        abi.materialized_dma_descriptors[index];
    if ((descriptor.flags & runtime::MaterializedDMAEpochZeroSource) != 0 &&
        descriptor.local_slot == input.local_slot) {
      return true;
    }
  }
  return false;
}

inline bool validModelInputTable(const runtime::TileABI &abi) {
  if (abi.model_input_count != 0 && abi.model_inputs == nullptr) {
    return false;
  }
  for (uint32_t index = 0; index < abi.model_input_count; ++index) {
    const runtime::ModelIO &input = abi.model_inputs[index];
    const runtime::Resource *resource = findResource(abi, input.local_slot);
    const runtime::GlobalBuffer *buffer =
        findGlobalBuffer(abi, input.global_resource_id);
    if (input.owner_core != abi.core_id || input.byte_size == 0 ||
        resource == nullptr ||
        resource->kind != runtime::ResourceKind::ModelInput ||
        resource->byte_size != input.byte_size || buffer == nullptr ||
        (buffer->flags & runtime::GlobalBufferInput) == 0 ||
        buffer->byte_size == 0 ||
        buffer->ram_offset > abi.global_ram_capacity_bytes ||
        buffer->byte_size >
            abi.global_ram_capacity_bytes - buffer->ram_offset ||
        findUniqueModelInput(abi, input.local_slot) != &input ||
        !modelInputHasEpochZeroDescriptor(abi, input)) {
      return false;
    }
  }
  return true;
}

inline bool
validateDescriptor(const runtime::TileABI &abi,
                   const runtime::MaterializedDMADescriptor &descriptor,
                   MaterializedModelInputPlanStats *stats) {
  const uint32_t maximum_frame_bytes = abi.maximumFrameBytes();
  const uint32_t logical_segment_count =
      abi.logicalMaterializedDMASegmentCount();
  if (stats == nullptr ||
      !runtime::isSupportedPhysicalFrameMaximum(maximum_frame_bytes) ||
      descriptor.direction !=
          runtime::ScratchpadDMADirection::GlobalRAMToScratchpad ||
      descriptor.iteration_step == 0 ||
      descriptor.iteration_begin >= descriptor.iteration_end ||
      descriptor.segment_count == 0 ||
      descriptor.segment_offset > logical_segment_count ||
      descriptor.segment_count >
          logical_segment_count - descriptor.segment_offset ||
      descriptor.bytes_per_iteration == 0) {
    return false;
  }

  const runtime::Resource *resource = findResource(abi, descriptor.local_slot);
  const runtime::ModelIO *input =
      findUniqueModelInput(abi, descriptor.local_slot);
  if (resource == nullptr || input == nullptr ||
      resource->kind != runtime::ResourceKind::ModelInput ||
      resource->byte_size != input->byte_size) {
    return false;
  }
  const uint32_t element_size =
      runtime::elementSizeBytes(resource->element_type);
  if (element_size == 0) {
    return false;
  }

  const uint64_t iterations =
      1U + (descriptor.iteration_end - descriptor.iteration_begin - 1U) /
               descriptor.iteration_step;
  const uint64_t last_iteration = iterations - 1U;
  uint64_t transfers_per_iteration = 0;
  uint64_t bytes_per_iteration = 0;
  for (uint32_t ordinal = 0; ordinal < descriptor.segment_count; ++ordinal) {
    const uint32_t segment_index = descriptor.segment_offset + ordinal;
    runtime::MaterializedDMASegment segment{};
    if (!abi.resolveMaterializedDMASegment(segment_index, &segment)) {
      return false;
    }
    const runtime::GlobalBuffer *buffer =
        findGlobalBuffer(abi, segment.global_resource_id);
    if (segment.id != segment_index || segment.descriptor_id != descriptor.id ||
        segment.byte_size == 0 || segment.byte_size > maximum_frame_bytes ||
        segment.byte_size % element_size != 0 || segment.repeat_count == 0 ||
        segment.piece_count == 0 || buffer == nullptr ||
        segment.global_resource_id != input->global_resource_id ||
        (buffer->flags & runtime::GlobalBufferInput) == 0 ||
        buffer->byte_size == 0 ||
        buffer->ram_offset > abi.global_ram_capacity_bytes ||
        buffer->byte_size >
            abi.global_ram_capacity_bytes - buffer->ram_offset ||
        segment.global_byte_offset % element_size != 0 ||
        (iterations > 1U &&
         segment.global_iteration_stride % element_size != 0) ||
        (segment.repeat_count > 1U &&
         segment.global_repeat_stride % element_size != 0) ||
        (segment.piece_count > 1U &&
         segment.global_piece_stride % element_size != 0)) {
      return false;
    }

    uint64_t segment_transfers = 0;
    uint64_t segment_bytes = 0;
    if (!checkedMultiply(segment.repeat_count, segment.piece_count,
                         &segment_transfers) ||
        !checkedMultiply(segment_transfers, segment.byte_size,
                         &segment_bytes) ||
        !checkedAdd(transfers_per_iteration, segment_transfers,
                    &transfers_per_iteration) ||
        !checkedAdd(bytes_per_iteration, segment_bytes, &bytes_per_iteration)) {
      return false;
    }

    uint64_t maximum_relative_offset = segment.global_byte_offset;
    if (!checkedScaledAdd(maximum_relative_offset,
                          segment.global_iteration_stride, last_iteration,
                          &maximum_relative_offset) ||
        !checkedScaledAdd(maximum_relative_offset, segment.global_repeat_stride,
                          segment.repeat_count - 1U,
                          &maximum_relative_offset) ||
        !checkedScaledAdd(maximum_relative_offset, segment.global_piece_stride,
                          segment.piece_count - 1U, &maximum_relative_offset) ||
        maximum_relative_offset > buffer->byte_size ||
        segment.byte_size > buffer->byte_size - maximum_relative_offset) {
      return false;
    }
    uint64_t maximum_absolute_offset = 0;
    if (!checkedAdd(buffer->ram_offset, maximum_relative_offset,
                    &maximum_absolute_offset) ||
        maximum_absolute_offset > abi.global_ram_capacity_bytes ||
        segment.byte_size >
            abi.global_ram_capacity_bytes - maximum_absolute_offset) {
      return false;
    }
  }
  if (bytes_per_iteration != descriptor.bytes_per_iteration) {
    return false;
  }

  uint64_t descriptor_transfers = 0;
  uint64_t descriptor_bytes = 0;
  return checkedAdd(stats->descriptor_count, 1, &stats->descriptor_count) &&
         checkedAdd(stats->iteration_count, iterations,
                    &stats->iteration_count) &&
         checkedMultiply(transfers_per_iteration, iterations,
                         &descriptor_transfers) &&
         checkedMultiply(bytes_per_iteration, iterations, &descriptor_bytes) &&
         checkedAdd(stats->transfer_count, descriptor_transfers,
                    &stats->transfer_count) &&
         checkedAdd(stats->byte_count, descriptor_bytes, &stats->byte_count);
}

} // namespace materialized_model_input_detail

// Validate the complete materialized model-input plan before any initialization
// side effect occurs.  Legacy parametric DMA is deliberately rejected: the V1
// production path has exactly one authoritative representation.
inline bool validateMaterializedModelInputPlan(
    const runtime::TileABI &abi,
    MaterializedModelInputPlanStats *plan_stats = nullptr) {
  using namespace materialized_model_input_detail;
  if ((abi.abi_features & runtime::TileABIMaterializedDataflow) == 0 ||
      (abi.abi_features & runtime::TileABIGlobalRAMDMA) == 0 ||
      abi.parametric_dma_descriptor_count != 0 ||
      (abi.materialized_dma_descriptor_count != 0 &&
       abi.materialized_dma_descriptors == nullptr) ||
      (abi.materialized_dma_segment_count != 0 &&
       abi.materialized_dma_segments == nullptr) ||
      !validModelInputTable(abi)) {
    return false;
  }

  MaterializedModelInputPlanStats stats{};
  for (uint32_t index = 0; index < abi.materialized_dma_descriptor_count;
       ++index) {
    const runtime::MaterializedDMADescriptor &descriptor =
        abi.materialized_dma_descriptors[index];
    if ((descriptor.flags & runtime::MaterializedDMAEpochZeroSource) == 0) {
      continue;
    }
    if (!validateDescriptor(abi, descriptor, &stats)) {
      return false;
    }
  }
  if ((abi.model_input_count == 0) != (stats.descriptor_count == 0)) {
    return false;
  }
  if (plan_stats != nullptr) {
    *plan_stats = stats;
  }
  return true;
}

// Visit every physical transfer represented by the compact epoch-zero input
// plan.  Validation is a separate first pass, so malformed late segments never
// leave global RAM partially initialized.
template <typename Visitor>
bool forEachMaterializedModelInputTransfer(const runtime::TileABI &abi,
                                           Visitor visitor) {
  using namespace materialized_model_input_detail;
  if (!validateMaterializedModelInputPlan(abi)) {
    return false;
  }
  for (uint32_t index = 0; index < abi.materialized_dma_descriptor_count;
       ++index) {
    const runtime::MaterializedDMADescriptor &descriptor =
        abi.materialized_dma_descriptors[index];
    if ((descriptor.flags & runtime::MaterializedDMAEpochZeroSource) == 0) {
      continue;
    }
    const runtime::Resource *resource =
        findResource(abi, descriptor.local_slot);
    const uint64_t iterations =
        1U + (descriptor.iteration_end - descriptor.iteration_begin - 1U) /
                 descriptor.iteration_step;
    for (uint64_t phase = 0; phase < iterations; ++phase) {
      for (uint32_t ordinal = 0; ordinal < descriptor.segment_count;
           ++ordinal) {
        runtime::MaterializedDMASegment segment{};
        if (!abi.resolveMaterializedDMASegment(
                descriptor.segment_offset + ordinal, &segment)) {
          return false;
        }
        const runtime::GlobalBuffer *buffer =
            findGlobalBuffer(abi, segment.global_resource_id);
        for (uint64_t repeat = 0; repeat < segment.repeat_count; ++repeat) {
          for (uint64_t piece = 0; piece < segment.piece_count; ++piece) {
            uint64_t relative_offset = segment.global_byte_offset;
            uint64_t absolute_offset = 0;
            if (resource == nullptr || buffer == nullptr ||
                !checkedScaledAdd(relative_offset,
                                  segment.global_iteration_stride, phase,
                                  &relative_offset) ||
                !checkedScaledAdd(relative_offset, segment.global_repeat_stride,
                                  repeat, &relative_offset) ||
                !checkedScaledAdd(relative_offset, segment.global_piece_stride,
                                  piece, &relative_offset) ||
                !checkedAdd(buffer->ram_offset, relative_offset,
                            &absolute_offset) ||
                !visitor(*resource, absolute_offset, segment.byte_size)) {
              return false;
            }
          }
        }
      }
    }
  }
  return true;
}

} // namespace golem::platform
