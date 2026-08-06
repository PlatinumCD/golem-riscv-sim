#include <stdint.h>

#include "golem/runtime/runtime.h"
#include "mesh-nic.h"
#include "platform.h"

namespace {

using namespace golem::runtime;

constexpr uint32_t kTaskId = 130;
constexpr uint32_t kMaximumTensors = 64;
constexpr uint32_t kMaximumRank = 4;
constexpr uint64_t kStorageBytes = 4 * 1024 * 1024;

struct Descriptor {
    void* allocated;
    void* aligned;
    int64_t offset;
    int64_t dimensions[2 * kMaximumRank];
};

alignas(64) uint8_t storage[kStorageBytes];
Descriptor descriptors[kMaximumTensors];
Tensor inputs[kMaximumTensors];
Tensor outputs[kMaximumTensors];

bool trySend(void*, uint32_t, uint32_t) {
    return true;
}

bool tryReceive(void*, uint32_t*) {
    return false;
}

const WordTransport transport{nullptr, trySend, tryReceive};

void putHex(uint32_t value) {
    constexpr char digits[] = "0123456789abcdef";
    uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        uart_putc(digits[(value >> shift) & 0xfU]);
    }
}

uint64_t align64(uint64_t value) {
    return (value + 63U) & ~UINT64_C(63);
}

bool makeTensor(
    const TileABI& abi,
    uint32_t slot,
    uint64_t& storage_offset,
    Tensor& tensor,
    Descriptor& descriptor,
    bool initialize
) {
    const Resource* resource = abi.findResource(slot);
    if (resource == nullptr || resource->rank > kMaximumRank ||
        resource->byte_size == 0) {
        return false;
    }
    storage_offset = align64(storage_offset);
    if (storage_offset + resource->byte_size > kStorageBytes) {
        return false;
    }

    uint8_t* data = storage + storage_offset;
    descriptor.allocated = data;
    descriptor.aligned = data;
    descriptor.offset = 0;
    int64_t stride = 1;
    for (uint32_t index = 0; index < resource->rank; ++index) {
        descriptor.dimensions[index] =
            abi.resource_dimensions[resource->dimension_offset + index];
    }
    for (uint32_t reverse = resource->rank; reverse != 0; --reverse) {
        const uint32_t index = reverse - 1;
        descriptor.dimensions[resource->rank + index] = stride;
        stride *= descriptor.dimensions[index];
    }

    if (initialize) {
        for (uint64_t offset = 0; offset < resource->byte_size; ++offset) {
            data[offset] = static_cast<uint8_t>(
                (slot * 37U + offset * 13U + 17U) & 0xffU
            );
        }
    } else {
        for (uint64_t offset = 0; offset < resource->byte_size; ++offset) {
            data[offset] = 0;
        }
    }

    tensor = {resource->element_type, resource->rank, &descriptor};
    storage_offset += resource->byte_size;
    return true;
}

uint32_t hashOutputs(
    const TileABI& abi,
    const TaskBinding& binding
) {
    uint32_t hash = UINT32_C(2166136261);
    for (uint32_t index = 0; index < binding.output_count; ++index) {
        const uint32_t slot =
            abi.task_binding_data[binding.output_offset + index];
        const Resource* resource = abi.findResource(slot);
        const auto* bytes = static_cast<const uint8_t*>(
            descriptors[binding.input_count + index].aligned
        );
        for (uint64_t offset = 0; offset < resource->byte_size; ++offset) {
            hash ^= bytes[offset];
            hash *= UINT32_C(16777619);
        }
    }
    return hash;
}

}  // namespace

extern "C" int tile_main() {
    const TileABI abi = linkedTileABI();
    BasicTileRuntime runtime{abi, transport};
    const TaskBinding* binding = abi.findTaskBinding(kTaskId);
    const Task* task = abi.dispatch_tasks.find(kTaskId);
    if (!runtime.valid() || binding == nullptr || task == nullptr ||
        binding->input_count > kMaximumTensors ||
        binding->output_count > kMaximumTensors ||
        binding->input_count + binding->output_count > kMaximumTensors) {
        uart_puts("C3_ISOLATED_INVALID_ABI\n");
        return 1;
    }
    if (!runtime.boot()) {
        uart_puts("C3_ISOLATED_BOOT_FAILED\n");
        return 2;
    }

    uint64_t storage_offset = 0;
    for (uint32_t index = 0; index < binding->input_count; ++index) {
        const uint32_t slot =
            abi.task_binding_data[binding->input_offset + index];
        if (!makeTensor(
                abi, slot, storage_offset, inputs[index],
                descriptors[index], true)) {
            uart_puts("C3_ISOLATED_INPUT_FAILED\n");
            return 3;
        }
    }
    for (uint32_t index = 0; index < binding->output_count; ++index) {
        const uint32_t slot =
            abi.task_binding_data[binding->output_offset + index];
        if (!makeTensor(
                abi, slot, storage_offset, outputs[index],
                descriptors[binding->input_count + index], false)) {
            uart_puts("C3_ISOLATED_OUTPUT_FAILED\n");
            return 4;
        }
    }

    mesh_nic::complete_memory_initialization();

    if (!runtime.execute(
            kTaskId,
            inputs,
            binding->input_count,
            outputs,
            binding->output_count)) {
        uart_puts("C3_ISOLATED_EXECUTE_FAILED\n");
        return 5;
    }

    uart_puts("C3_ISOLATED_PASS checksum=");
    putHex(hashOutputs(abi, *binding));
    uart_puts("\n");
    return 0;
}
