#include <stdint.h>

#include "golem/runtime/runtime.h"
#include "mesh-nic.h"
#include "platform.h"

namespace {

using namespace golem::runtime;

constexpr uint32_t kTaskId = 152;
constexpr uint32_t kMaximumTensors = 8;
constexpr uint32_t kMaximumRank = 4;
constexpr uint64_t kStorageBytes = 256 * 1024;

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
uint32_t inputSlots[kMaximumTensors];

bool trySend(void*, uint32_t, uint32_t) {
    return true;
}

bool tryReceive(void*, uint32_t*) {
    return false;
}

const WordTransport transport{nullptr, trySend, tryReceive};

void putHex64(uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        uart_putc(digits[(value >> shift) & 0xfU]);
    }
}

uint64_t align64(uint64_t value) {
    return (value + 63U) & ~UINT64_C(63);
}

float inputValue(uint32_t slot, uint64_t index) {
    return static_cast<float>(slot % 17U) * 0.125F +
           static_cast<float>(index % 97U) * 0.00390625F;
}

void printResource(
    const char* role,
    uint32_t slot,
    const Resource& resource,
    const Descriptor& descriptor
) {
    uart_puts("VECTOR_RESOURCE role=");
    uart_puts(role);
    uart_puts(" slot=");
    putHex64(slot);
    uart_puts(" base=");
    putHex64(reinterpret_cast<uintptr_t>(descriptor.aligned));
    uart_puts(" bytes=");
    putHex64(resource.byte_size);
    uart_puts("\n");
}

bool makeTensor(
    const TileABI& abi,
    uint32_t slot,
    uint64_t& storageOffset,
    Tensor& tensor,
    Descriptor& descriptor,
    bool initialize,
    const char* role
) {
    const Resource* resource = abi.findResource(slot);
    if (resource == nullptr || resource->rank > kMaximumRank ||
        resource->byte_size == 0 || resource->byte_size % sizeof(float) != 0) {
        return false;
    }
    storageOffset = align64(storageOffset);
    if (storageOffset + resource->byte_size > kStorageBytes) {
        return false;
    }

    uint8_t* data = storage + storageOffset;
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

    auto* values = reinterpret_cast<float*>(data);
    const uint64_t elementCount = resource->byte_size / sizeof(float);
    for (uint64_t index = 0; index < elementCount; ++index) {
        values[index] = initialize ? inputValue(slot, index) : 0.0F;
    }

    tensor = {resource->element_type, resource->rank, &descriptor};
    printResource(role, slot, *resource, descriptor);
    storageOffset += resource->byte_size;
    return true;
}

bool validateOutputs(const TileABI& abi, const TaskBinding& binding) {
    if (binding.input_count != 2 || binding.output_count != 2) {
        return false;
    }
    for (uint32_t outputIndex = 0; outputIndex < binding.output_count;
         ++outputIndex) {
        const uint32_t slot =
            abi.task_binding_data[binding.output_offset + outputIndex];
        const Resource* resource = abi.findResource(slot);
        if (resource == nullptr) {
            return false;
        }
        const auto* actual = static_cast<const float*>(
            descriptors[binding.input_count + outputIndex].aligned
        );
        const uint64_t elementCount = resource->byte_size / sizeof(float);
        for (uint64_t index = 0; index < elementCount; ++index) {
            const float expected =
                inputValue(inputSlots[0], index) +
                inputValue(inputSlots[1], index);
            if (actual[index] != expected) {
                return false;
            }
        }
    }
    return true;
}

uint32_t hashOutputs(const TileABI& abi, const TaskBinding& binding) {
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
        uart_puts("VECTOR_ISOLATED_INVALID_ABI\n");
        return 1;
    }
    if (!runtime.boot()) {
        uart_puts("VECTOR_ISOLATED_BOOT_FAILED\n");
        return 2;
    }

    uint64_t storageOffset = 0;
    for (uint32_t index = 0; index < binding->input_count; ++index) {
        const uint32_t slot =
            abi.task_binding_data[binding->input_offset + index];
        inputSlots[index] = slot;
        if (!makeTensor(
                abi, slot, storageOffset, inputs[index], descriptors[index],
                true, "input")) {
            uart_puts("VECTOR_ISOLATED_INPUT_FAILED\n");
            return 3;
        }
    }
    for (uint32_t index = 0; index < binding->output_count; ++index) {
        const uint32_t slot =
            abi.task_binding_data[binding->output_offset + index];
        if (!makeTensor(
                abi, slot, storageOffset, outputs[index],
                descriptors[binding->input_count + index], false,
                "output")) {
            uart_puts("VECTOR_ISOLATED_OUTPUT_FAILED\n");
            return 4;
        }
    }

    mesh_nic::complete_memory_initialization();
    mesh_nic::trace_task(mesh_nic::kTaskTraceStart, kTaskId, 0);
    const bool executed = runtime.execute(
        kTaskId,
        inputs,
        binding->input_count,
        outputs,
        binding->output_count
    );
    mesh_nic::trace_task(mesh_nic::kTaskTraceFinish, kTaskId, 0);
    if (!executed) {
        uart_puts("VECTOR_ISOLATED_EXECUTE_FAILED\n");
        return 5;
    }
    if (!validateOutputs(abi, *binding)) {
        uart_puts("VECTOR_ISOLATED_NUMERICAL_FAILED\n");
        return 6;
    }

    uart_puts("VECTOR_ISOLATED_PASS checksum=");
    putHex64(hashOutputs(abi, *binding));
    uart_puts("\n");
    return 0;
}
