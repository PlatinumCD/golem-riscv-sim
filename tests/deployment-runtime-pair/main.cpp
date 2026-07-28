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

struct VectorMemRefDescriptor {
    void* allocated;
    void* aligned;
    int64_t offset;
    int64_t sizes[1];
    int64_t strides[1];
};

TaskStatus addOne(
    const Tensor* inputs,
    uint32_t input_count,
    Tensor* outputs,
    uint32_t output_count
) {
    if (inputs == nullptr ||
        outputs == nullptr ||
        input_count != 1 ||
        output_count != 1 ||
        inputs[0].rank != 1 ||
        outputs[0].rank != 1) {
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
    void* destination,
    uint32_t word_count
) {
    return mesh_nic::try_start_receive_words(
        source,
        route_id,
        destination,
        word_count
    );
}

bool tryReceiveWordsCompletion(
    void*,
    uint32_t* source,
    uint32_t* route_id
) {
    return mesh_nic::try_receive_words_completion(
        source, route_id);
}

const RoutedWordTransport transport{
    nullptr,
    trySend,
    tryReceive,
    trySendWords,
    tryStartReceiveWords,
    tryReceiveWordsCompletion,
};

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
    const TileABI abi{
        0,
        nullptr,
        0,
        {tasks, 1},
        nullptr,
        0,
        outgoing,
        1,
        inputs,
        1,
        nullptr,
        0,
        resources,
        2,
        dimensions,
        1,
        kTensorBytes,
        bindings,
        1,
        binding_data,
        2,
    };

    alignas(64) float input[kElementCount];
    for (uint32_t index = 0; index < kElementCount; ++index) {
        input[index] = static_cast<float>(index);
    }
    DeploymentRuntime runtime{abi, transport};
    if (!runtime.bindModelInput(0, &input)) {
        uart_puts("DEPLOYMENT_RUNTIME_TILE_0_FAIL\n");
        return 1;
    }
    while (!runtime.complete() && !runtime.failed()) {
        if (runtime.step() == DeploymentStep::WaitForReceive) {
            mesh_nic::wait_for_receive();
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
    const TileABI abi{
        1,
        nullptr,
        0,
        {tasks, 1},
        incoming,
        1,
        nullptr,
        0,
        nullptr,
        0,
        outputs,
        1,
        resources,
        2,
        dimensions,
        1,
        kTensorBytes,
        bindings,
        1,
        binding_data,
        2,
    };

    alignas(64) float output[kElementCount] = {};
    DeploymentRuntime runtime{abi, transport};
    if (!runtime.bindModelOutput(0, &output)) {
        uart_puts("DEPLOYMENT_RUNTIME_TILE_1_FAIL\n");
        return 1;
    }
    while (!runtime.complete() && !runtime.failed()) {
        if (runtime.step() == DeploymentStep::WaitForReceive) {
            mesh_nic::wait_for_receive();
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
