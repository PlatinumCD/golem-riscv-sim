#include <stdint.h>

#include "golem/runtime/runtime.h"
#include "mesh-nic.h"
#include "platform.h"

#ifndef MITTENS_TILE_ID
#error "MITTENS_TILE_ID must be defined"
#endif

namespace {

using namespace golem::runtime;

constexpr uint32_t kElementCount = 300;
constexpr uint64_t kTensorBytes =
    static_cast<uint64_t>(kElementCount) * sizeof(float);
constexpr uint32_t kProbeStride = 1024;
alignas(64) volatile uint64_t cacheProbe[4 * kProbeStride + 1] = {};

__attribute__((noinline)) uint64_t memoryAttributionSharedSite(
    volatile uint64_t* address
) {
    return *address;
}

__attribute__((noinline)) uint64_t memoryAttributionSiteA() {
    uint64_t value = memoryAttributionSharedSite(&cacheProbe[0]);
    asm volatile("" : "+r"(value));
    return value;
}

__attribute__((noinline)) uint64_t memoryAttributionSiteB() {
    uint64_t value =
        memoryAttributionSharedSite(&cacheProbe[kProbeStride]);
    asm volatile("" : "+r"(value));
    return value;
}

struct VectorMemRefDescriptor {
    void* allocated;
    void* aligned;
    int64_t offset;
    int64_t sizes[1];
    int64_t strides[1];
};

void exerciseMemoryHierarchy() {
    volatile uint64_t sink = 0;
    sink ^= cacheProbe[0 * kProbeStride];
    sink ^= cacheProbe[1 * kProbeStride];
    sink ^= cacheProbe[2 * kProbeStride];
    sink ^= cacheProbe[3 * kProbeStride];
    sink ^= cacheProbe[0 * kProbeStride];
    sink ^= cacheProbe[4 * kProbeStride];
    sink ^= cacheProbe[1 * kProbeStride];
    (void)sink;
}

TaskStatus addOne(
    const Tensor* inputs,
    uint32_t input_count,
    Tensor* outputs,
    uint32_t output_count
) {
    const uint64_t attributedA = memoryAttributionSiteA();
    const uint64_t attributedB = memoryAttributionSiteB();
    exerciseMemoryHierarchy();
    if (inputs == nullptr ||
        outputs == nullptr ||
        input_count != 1 ||
        output_count != 1 ||
        inputs[0].rank != 1 ||
        outputs[0].rank != 1 || attributedA != 0 || attributedB != 0) {
        return TaskStatus::Failure;
    }
    const auto* input =
        static_cast<const VectorMemRefDescriptor*>(inputs[0].descriptor);
    auto* output =
        static_cast<VectorMemRefDescriptor*>(outputs[0].descriptor);
    if (input == nullptr ||
        output == nullptr ||
        input->aligned == nullptr ||
        output->aligned == nullptr ||
        input->sizes[0] != kElementCount ||
        output->sizes[0] != kElementCount) {
        return TaskStatus::Failure;
    }
    const auto* input_data = static_cast<const float*>(input->aligned);
    auto* output_data = static_cast<float*>(output->aligned);
    for (uint32_t index = 0; index < kElementCount; ++index) {
        output_data[
            output->offset +
            static_cast<int64_t>(index) * output->strides[0]
        ] =
            input_data[
                input->offset +
                static_cast<int64_t>(index) * input->strides[0]
            ] + 1.0F;
    }
    return TaskStatus::Success;
}

bool trySend(void*, uint32_t destination, uint32_t payload) {
    return mesh_nic::try_send(destination, payload);
}

bool tryReceive(void*, RoutedWord* word) {
    return word != nullptr &&
           mesh_nic::try_receive_from(&word->source_tile, &word->payload);
}

bool trySendWords(
    void*,
    uint32_t destination,
    const uint32_t* words,
    uint32_t word_count
) {
    return mesh_nic::try_send_words(
        destination, words, word_count);
}

bool tryStartReceiveWords(
    void*,
    uint32_t source,
    uint32_t route_id,
    uint64_t logical_iteration,
    void* destination,
    uint32_t word_count
) {
    return mesh_nic::try_start_receive_words(
        source,
        route_id,
        logical_iteration,
        destination,
        word_count
    );
}

bool tryReceiveWordsCompletion(
    void*,
    uint32_t* source,
    uint32_t* route_id,
    uint64_t* logical_iteration
) {
    return mesh_nic::try_receive_words_completion(
        source, route_id, logical_iteration);
}

bool tryClaimReceiveWords(
    void*,
    uint32_t source,
    uint32_t route_id,
    uint64_t logical_iteration,
    uint32_t word_count
) {
    return mesh_nic::try_claim_receive_words(
        source, route_id, logical_iteration, word_count);
}

const RoutedWordTransport transport{
    nullptr,
    trySend,
    tryReceive,
    trySendWords,
    tryStartReceiveWords,
    tryReceiveWordsCompletion,
    tryClaimReceiveWords,
};

void emitTaskTrace(
    void*,
    TaskTraceEvent event,
    uint32_t task_id,
    ExecutionId execution_id
) {
    mesh_nic::trace_task(
        static_cast<uint32_t>(event), task_id, execution_id);
}

DeploymentTrace taskTrace{nullptr, emitTaskTrace};

[[maybe_unused]] int runSource() {
    const Task tasks[] = {{11, addOne, 1, 1}};
    const Route outgoing[] = {
        {7, 0, 11, 0, 1, 12, 0, 2, 1, kTensorBytes},
    };
    const ModelIO inputs[] = {{0, 0, 1, 0, kTensorBytes}};
    const int64_t dimensions[] = {kElementCount};
    const Resource resources[] = {
        {
            1,
            0,
            0,
            ResourceKind::ModelInput,
            ElementType::Float32,
            1,
            0,
            ResourceExternal,
            kTensorBytes,
            0,
        },
        {
            2,
            7,
            1,
            ResourceKind::RouteOutput,
            ElementType::Float32,
            1,
            0,
            ResourceWorkspace,
            kTensorBytes,
            0,
        },
    };
    const uint32_t binding_data[] = {0, 1};
    const TaskBinding bindings[] = {
        {11, 0, 1, 1, 1, 2, 0, 0},
    };
    TileABI abi{};
    abi.core_id = 0;
    abi.dispatch_tasks = {tasks, 1};
    abi.outgoing_routes = outgoing;
    abi.outgoing_route_count = 1;
    abi.model_inputs = inputs;
    abi.model_input_count = 1;
    abi.resources = resources;
    abi.resource_count = 2;
    abi.resource_dimensions = dimensions;
    abi.resource_dimension_count = 1;
    abi.workspace_size = kTensorBytes;
    abi.task_bindings = bindings;
    abi.task_binding_count = 1;
    abi.task_binding_data = binding_data;
    abi.task_binding_data_count = 2;

    alignas(64) float input[kElementCount];
    for (uint32_t index = 0; index < kElementCount; ++index) {
        input[index] = static_cast<float>(index);
    }
    mesh_nic::complete_memory_initialization();
    DeploymentRuntime runtime{abi, transport, nullptr, &taskTrace};
    if (!runtime.bindModelInput(0, &input)) {
        uart_puts("DEPLOYMENT_RUNTIME_TILE_0_FAIL\n");
        return 1;
    }
    while (!runtime.complete() && !runtime.failed()) {
        const DeploymentStep step = runtime.step();
        if (step == DeploymentStep::WaitForReceive) {
            mesh_nic::wait_for_receive();
        } else if (step == DeploymentStep::WaitForTransmit) {
            mesh_nic::wait_for_transmit();
        }
    }
    if (runtime.failed()) {
        uart_puts("DEPLOYMENT_RUNTIME_TILE_0_FAIL\n");
        return 1;
    }
    uart_puts("DEPLOYMENT_RUNTIME_TILE_0_PASS\n");
    return 0;
}

[[maybe_unused]] int runDestination() {
    const Task tasks[] = {{12, addOne, 1, 1}};
    const Route incoming[] = {
        {7, 0, 11, 0, 1, 12, 0, 2, 0, kTensorBytes},
    };
    const ModelIO outputs[] = {{0, 1, 3, 1, kTensorBytes}};
    const int64_t dimensions[] = {kElementCount};
    const Resource resources[] = {
        {
            2,
            7,
            0,
            ResourceKind::RouteInput,
            ElementType::Float32,
            1,
            0,
            ResourceWorkspace,
            kTensorBytes,
            0,
        },
        {
            3,
            0,
            1,
            ResourceKind::ModelOutput,
            ElementType::Float32,
            1,
            0,
            ResourceExternal,
            kTensorBytes,
            0,
        },
    };
    const uint32_t binding_data[] = {0, 1};
    const TaskBinding bindings[] = {
        {12, 0, 1, 1, 1, 2, 0, 0},
    };
    TileABI abi{};
    abi.core_id = 1;
    abi.dispatch_tasks = {tasks, 1};
    abi.incoming_routes = incoming;
    abi.incoming_route_count = 1;
    abi.model_outputs = outputs;
    abi.model_output_count = 1;
    abi.resources = resources;
    abi.resource_count = 2;
    abi.resource_dimensions = dimensions;
    abi.resource_dimension_count = 1;
    abi.workspace_size = kTensorBytes;
    abi.task_bindings = bindings;
    abi.task_binding_count = 1;
    abi.task_binding_data = binding_data;
    abi.task_binding_data_count = 2;

    alignas(64) float output[kElementCount] = {};
    mesh_nic::complete_memory_initialization();
    DeploymentRuntime runtime{abi, transport, nullptr, &taskTrace};
    if (!runtime.bindModelOutput(0, &output)) {
        uart_puts("DEPLOYMENT_RUNTIME_TILE_1_FAIL\n");
        return 1;
    }
    while (!runtime.complete() && !runtime.failed()) {
        const DeploymentStep step = runtime.step();
        if (step == DeploymentStep::WaitForReceive) {
            mesh_nic::wait_for_receive();
        } else if (step == DeploymentStep::WaitForTransmit) {
            mesh_nic::wait_for_transmit();
        }
    }
    if (runtime.failed()) {
        uart_puts("DEPLOYMENT_RUNTIME_TILE_1_FAIL\n");
        return 1;
    }
    for (uint32_t index = 0; index < kElementCount; ++index) {
        if (output[index] != static_cast<float>(index + 2U)) {
            uart_puts("DEPLOYMENT_RUNTIME_TILE_1_FAIL\n");
            return 1;
        }
    }
    uart_puts("DEPLOYMENT_RUNTIME_TILE_1_PASS words=300\n");
    return 0;
}

}  // namespace

extern "C" int tile_main() {
    if constexpr (MITTENS_TILE_ID == 0) {
        return runSource();
    }
    return runDestination();
}
