#include <stdint.h>

#include "src/platform/deployment/materialized-model-inputs.h"

namespace {

using namespace golem::runtime;
using golem::platform::MaterializedModelInputPlanStats;
using golem::platform::forEachMaterializedModelInputTransfer;
using golem::platform::validateMaterializedModelInputPlan;

struct Fixture {
    ModelIO input{};
    Resource resource{};
    GlobalBuffer buffer{};
    MaterializedDMADescriptor descriptor{};
    MaterializedDMASegment segment{};
    MaterializedDMAAffineSingleSegmentRun affine_run{};
    MaterializedDMAAffineRunCertificate affine_certificate{};
    ParametricDMADescriptor legacy_descriptor{};
    TileABI abi{};

    Fixture() {
        input.model_index = 0;
        input.owner_core = 7;
        input.global_resource_id = 100;
        input.local_slot = 3;
        input.byte_size = 4096;

        resource.global_resource_id = 500;
        resource.local_slot = 3;
        resource.kind = ResourceKind::ModelInput;
        resource.element_type = ElementType::Float32;
        resource.byte_size = 4096;

        buffer.global_resource_id = 100;
        buffer.flags = GlobalBufferInput;
        buffer.ram_offset = 4096;
        buffer.byte_size = 8192;
        buffer.alignment = 4096;

        descriptor.id = 3;
        descriptor.local_slot = 3;
        descriptor.direction =
            ScratchpadDMADirection::GlobalRAMToScratchpad;
        descriptor.flags = MaterializedDMAEpochZeroSource;
        descriptor.iteration_begin = 10;
        descriptor.iteration_end = 16;
        descriptor.iteration_step = 2;
        descriptor.segment_offset = 3;
        descriptor.segment_count = 1;
        descriptor.bytes_per_iteration = 512;

        segment.id = 3;
        segment.descriptor_id = 3;
        segment.boundary_id = MaterializedDMAAggregateBoundary;
        segment.global_resource_id = 100;
        segment.byte_size = 128;
        segment.repeat_count = 2;
        segment.piece_count = 2;
        segment.global_byte_offset = 64;
        segment.global_iteration_stride = 1024;
        segment.global_repeat_stride = 256;
        segment.global_piece_stride = 128;

        affine_run.family_count = 1;
        affine_run.descriptors_per_family = 3;
        affine_run.direction =
            ScratchpadDMADirection::GlobalRAMToScratchpad;
        affine_run.descriptor_id_base = 0;
        affine_run.sequence_id_base = 0;
        affine_run.segment_id_base = 0;
        affine_run.global_resource_id = 100;
        affine_run.global_buffer_index = 0;
        affine_run.descriptor_byte_size = 128;
        affine_run.segment_byte_size = 128;
        affine_run.segment_repeat_count = 1;
        affine_run.segment_piece_count = 1;
        affine_run.iteration_span = 1;
        affine_run.iteration_step = 1;

        affine_certificate.version =
            MaterializedDMAAffineRunCertificateVersion;
        affine_certificate.single_segment_run_count = 1;
        affine_certificate.periodic_segment_run_count = 0;
        affine_certificate.periodic_phase_count = 0;
        affine_certificate.periodic_residue_pattern_count = 0;
        affine_certificate.periodic_segment_template_count = 0;
        affine_certificate.logical_descriptor_count = 4;
        affine_certificate.covered_descriptor_count = 3;
        affine_certificate.literal_descriptor_count = 1;
        affine_certificate.logical_sequence_count = 3;
        affine_certificate.covered_sequence_count = 3;
        affine_certificate.logical_segment_count = 4;
        affine_certificate.covered_segment_count = 3;
        affine_certificate.literal_segment_count = 1;

        abi.core_id = 7;
        abi.model_inputs = &input;
        abi.model_input_count = 1;
        abi.resources = &resource;
        abi.resource_count = 1;
        abi.abi_features =
            TileABIMaterializedDataflow | TileABIGlobalRAMDMA |
            TileABIMaterializedDMAAffineRuns;
        abi.global_buffers = &buffer;
        abi.global_buffer_count = 1;
        abi.global_ram_capacity_bytes = 65536;
        abi.materialized_dma_descriptors = &descriptor;
        abi.materialized_dma_descriptor_count = 1;
        abi.materialized_dma_segments = &segment;
        abi.materialized_dma_segment_count = 1;
        abi.materialized_dma_affine_runs = &affine_run;
        abi.materialized_dma_affine_run_count = 1;
        abi.materialized_dma_affine_run_certificate = &affine_certificate;
    }
};

struct Transfer {
    uint32_t local_slot = 0;
    uint64_t global_offset = 0;
    uint32_t byte_count = 0;
};

int check(bool condition, int code) { return condition ? 0 : code; }

}  // namespace

int main() {
    Fixture fixture;
    MaterializedModelInputPlanStats stats{};
    if (int result = check(
            validateMaterializedModelInputPlan(fixture.abi, &stats) &&
                stats.descriptor_count == 1 &&
                stats.iteration_count == 3 &&
                stats.transfer_count == 12 && stats.byte_count == 1536,
            1)) {
        return result;
    }

    Transfer transfers[12]{};
    uint32_t transfer_count = 0;
    const bool visited = forEachMaterializedModelInputTransfer(
        fixture.abi,
        [&transfers, &transfer_count](const Resource& resource,
                                      uint64_t global_offset,
                                      uint32_t byte_count) {
            if (transfer_count >= 12) {
                return false;
            }
            transfers[transfer_count++] =
                Transfer{resource.local_slot, global_offset, byte_count};
            return true;
        });
    if (int result = check(visited && transfer_count == 12, 2)) {
        return result;
    }
    uint32_t ordinal = 0;
    for (uint64_t phase = 0; phase < 3; ++phase) {
        for (uint64_t repeat = 0; repeat < 2; ++repeat) {
            for (uint64_t piece = 0; piece < 2; ++piece, ++ordinal) {
                const uint64_t expected =
                    4096 + 64 + phase * 1024 + repeat * 256 + piece * 128;
                if (int result = check(
                        transfers[ordinal].local_slot == 3 &&
                            transfers[ordinal].global_offset == expected &&
                            transfers[ordinal].byte_count == 128,
                        3)) {
                    return result;
                }
            }
        }
    }

    {
        Fixture invalid;
        invalid.abi.parametric_dma_descriptors = &invalid.legacy_descriptor;
        invalid.abi.parametric_dma_descriptor_count = 1;
        if (int result = check(
                !validateMaterializedModelInputPlan(invalid.abi), 4)) {
            return result;
        }
    }
    {
        Fixture invalid;
        invalid.descriptor.flags = 0;
        if (int result = check(
                !validateMaterializedModelInputPlan(invalid.abi), 5)) {
            return result;
        }
    }
    {
        Fixture invalid;
        invalid.descriptor.direction =
            ScratchpadDMADirection::ScratchpadToGlobalRAM;
        if (int result = check(
                !validateMaterializedModelInputPlan(invalid.abi), 6)) {
            return result;
        }
    }
    {
        Fixture invalid;
        invalid.buffer.flags = GlobalBufferTemporary;
        if (int result = check(
                !validateMaterializedModelInputPlan(invalid.abi), 7)) {
            return result;
        }
    }
    {
        Fixture invalid;
        invalid.segment.global_resource_id = 101;
        if (int result = check(
                !validateMaterializedModelInputPlan(invalid.abi), 8)) {
            return result;
        }
    }
    {
        Fixture invalid;
        invalid.segment.global_byte_offset = invalid.buffer.byte_size - 64;
        if (int result = check(
                !validateMaterializedModelInputPlan(invalid.abi), 9)) {
            return result;
        }
    }
    {
        Fixture invalid;
        invalid.segment.global_iteration_stride = UINT64_MAX - 63;
        if (int result = check(
                !validateMaterializedModelInputPlan(invalid.abi), 10)) {
            return result;
        }
    }
    {
        Fixture invalid;
        invalid.descriptor.bytes_per_iteration = 511;
        if (int result = check(
                !validateMaterializedModelInputPlan(invalid.abi), 11)) {
            return result;
        }
    }
    {
        Fixture invalid;
        invalid.segment.byte_size = 4097;
        invalid.descriptor.bytes_per_iteration = 16388;
        if (int result = check(
                !validateMaterializedModelInputPlan(invalid.abi), 12)) {
            return result;
        }
    }
    {
        Fixture invalid;
        invalid.descriptor.segment_count = 0;
        if (int result = check(
                !validateMaterializedModelInputPlan(invalid.abi), 13)) {
            return result;
        }
    }
    {
        Fixture invalid;
        invalid.descriptor.segment_offset = 1;
        if (int result = check(
                !validateMaterializedModelInputPlan(invalid.abi), 14)) {
            return result;
        }
    }
    {
        Fixture rejected;
        uint32_t callbacks = 0;
        if (int result = check(
                !forEachMaterializedModelInputTransfer(
                    rejected.abi,
                    [&callbacks](const Resource&, uint64_t, uint32_t) {
                        return ++callbacks < 2;
                    }) &&
                    callbacks == 2,
                15)) {
            return result;
        }
    }

    return 0;
}
