#include <stdint.h>

#include "golem/runtime/runtime.h"
#include "mesh-nic.h"
#include "platform.h"

#ifndef MITTENS_TILE_ID
#error "MITTENS_TILE_ID must be defined"
#endif

#ifndef MITTENS_FANOUT
#error "MITTENS_FANOUT must be defined"
#endif

namespace {

using namespace golem::runtime;

constexpr uint32_t kElementCount = 512;
constexpr uint32_t kFanout = MITTENS_FANOUT;
constexpr uint32_t kSourceTaskId = 11;
constexpr uint32_t kFirstDestinationTaskId = 100;
constexpr uint32_t kFirstRouteId = 1000;
constexpr uint64_t kTensorBytes =
    static_cast<uint64_t>(kElementCount) * sizeof(float);

static_assert(kFanout >= 1 && kFanout <= 24);

constexpr uint32_t sourceTaskId(uint32_t index)
{
#if defined(MITTENS_FANOUT_INDEPENDENT_TASKS)
    return kSourceTaskId + index;
#else
    (void)index;
    return kSourceTaskId;
#endif
}

struct VectorMemRefDescriptor {
    void* allocated;
    void* aligned;
    int64_t offset;
    int64_t sizes[1];
    int64_t strides[1];
};

TaskStatus copyVector(
    const Tensor* inputs,
    uint32_t inputCount,
    Tensor* outputs,
    uint32_t outputCount)
{
    if (inputs == nullptr || outputs == nullptr ||
        inputCount != 1 || outputCount != 1) {
        return TaskStatus::Failure;
    }
    const auto* input =
        static_cast<const VectorMemRefDescriptor*>(
            inputs[0].descriptor);
    auto* output =
        static_cast<VectorMemRefDescriptor*>(
            outputs[0].descriptor);
    if (input == nullptr || output == nullptr ||
        input->aligned == nullptr || output->aligned == nullptr) {
        return TaskStatus::Failure;
    }
    const auto* source =
        static_cast<const float*>(input->aligned);
    auto* destination = static_cast<float*>(output->aligned);
    for (uint32_t index = 0; index < kElementCount; ++index) {
        destination[index] = source[index];
    }
    return TaskStatus::Success;
}

TaskStatus consumeVector(
    const Tensor* inputs,
    uint32_t inputCount,
    Tensor*,
    uint32_t outputCount)
{
    if (inputs == nullptr || inputCount != 1 || outputCount != 0) {
        return TaskStatus::Failure;
    }
    const auto* input =
        static_cast<const VectorMemRefDescriptor*>(
            inputs[0].descriptor);
    if (input == nullptr || input->aligned == nullptr ||
        input->sizes[0] != kElementCount) {
        return TaskStatus::Failure;
    }
    const auto* values =
        static_cast<const float*>(input->aligned);
    return values[0] == 1.0F &&
                   values[kElementCount - 1] ==
                       static_cast<float>(kElementCount)
               ? TaskStatus::Success
               : TaskStatus::Failure;
}

bool trySend(void*, uint32_t destination, uint32_t payload)
{
    return mesh_nic::try_send(destination, payload);
}

bool tryReceive(void*, RoutedWord* word)
{
    return word != nullptr &&
           mesh_nic::try_receive_from(
               &word->source_tile, &word->payload);
}

bool trySendWords(
    void*,
    uint32_t destination,
    const uint32_t* words,
    uint32_t wordCount)
{
    return mesh_nic::try_send_words(
        destination, words, wordCount);
}

bool tryStartReceiveWords(
    void*,
    uint32_t source,
    uint32_t routeId,
    void* destination,
    uint32_t wordCount)
{
    return mesh_nic::try_start_receive_words(
        source, routeId, destination, wordCount);
}

bool tryReceiveWordsCompletion(
    void*,
    uint32_t* source,
    uint32_t* routeId)
{
    return mesh_nic::try_receive_words_completion(
        source, routeId);
}

uint64_t readCycle(void*)
{
    uint64_t value = 0;
    __asm__ volatile("rdcycle %0" : "=r"(value));
    return value;
}

void emitTaskTrace(
    void*,
    TaskTraceEvent event,
    uint32_t taskId,
    ExecutionId executionId)
{
    mesh_nic::trace_task(
        static_cast<uint32_t>(event),
        taskId,
        executionId);
}

const RoutedWordTransport kTransport{
    nullptr,
    trySend,
    tryReceive,
    trySendWords,
    tryStartReceiveWords,
    tryReceiveWordsCompletion,
};

void printUnsigned(uint64_t value)
{
    char digits[20];
    uint32_t count = 0;
    do {
        digits[count++] =
            static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    while (count != 0) {
        uart_putc(digits[--count]);
    }
}

void printProfile(const DeploymentProfile& profile)
{
    uart_puts(" blocked_steps=");
    printUnsigned(profile.transmit_blocked_steps);
    uart_puts(" blocked_cycles=");
    printUnsigned(profile.transmit_blocked_cycles);
    uart_puts(" transmit_steps=");
    printUnsigned(profile.transmit_steps);
    uart_puts(" transmit_cycles=");
    printUnsigned(profile.transmit_cycles);
}

[[maybe_unused]] int runSource()
{
    const Task tasks[] = {
        {kSourceTaskId, copyVector, 1, 1},
    };
    Route outgoing[kFanout]{};
    for (uint32_t index = 0; index < kFanout; ++index) {
        outgoing[index] = {
            kFirstRouteId + index,
            0,
            kSourceTaskId,
            0,
            1,
            kFirstDestinationTaskId + index,
            0,
            10000 + index,
            1,
            kTensorBytes,
        };
    }
    const ModelIO inputs[] = {
        {0, 0, 1, 0, kTensorBytes},
    };
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
            10000,
            kFirstRouteId,
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
    const uint32_t bindingData[] = {0, 1};
    const TaskBinding bindings[] = {
        {kSourceTaskId, 0, 1, 1, 1, 2, 0, 0},
    };
    const TileABI abi{
        0,
        nullptr,
        0,
        {tasks, 1},
        nullptr,
        0,
        outgoing,
        kFanout,
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
        bindingData,
        2,
    };

    alignas(64) float modelInput[kElementCount];
    for (uint32_t index = 0; index < kElementCount; ++index) {
        modelInput[index] = static_cast<float>(index + 1);
    }
    DeploymentProfile profile{nullptr, readCycle};
    DeploymentTrace trace{nullptr, emitTaskTrace};
    DeploymentRuntime runtime{abi, kTransport, &profile, &trace};
    if (!runtime.bindModelInput(0, modelInput)) {
        uart_puts("FANOUT_SOURCE_FAIL\n");
        return 1;
    }
    mesh_nic::complete_memory_initialization();
    while (!runtime.complete() && !runtime.failed()) {
        const DeploymentStep step = runtime.step();
        if (step == DeploymentStep::WaitForReceive) {
            mesh_nic::wait_for_receive();
        } else if (step == DeploymentStep::WaitForTransmit) {
            mesh_nic::wait_for_transmit();
        }
    }
    if (runtime.failed()) {
        uart_puts("FANOUT_SOURCE_FAIL\n");
        return 2;
    }
    uart_puts("FANOUT_SOURCE_PASS fanout=");
    printUnsigned(kFanout);
    printProfile(profile);
    uart_putc('\n');
    return 0;
}

#if defined(MITTENS_FANOUT_INDEPENDENT_TASKS)
[[maybe_unused]] int runIndependentSource()
{
    Task tasks[kFanout]{};
    Route outgoing[kFanout]{};
    Resource resources[kFanout + 1]{};
    TaskBinding bindings[kFanout]{};
    uint32_t bindingData[2 * kFanout]{};
    const ModelIO inputs[] = {
        {0, 0, 1, 0, kTensorBytes},
    };
    const int64_t dimensions[] = {kElementCount};
    resources[0] = {
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
    };
    for (uint32_t index = 0; index < kFanout; ++index) {
        const uint32_t taskId = sourceTaskId(index);
        const uint32_t routeId = kFirstRouteId + index;
        tasks[index] = {taskId, copyVector, 1, 1};
        outgoing[index] = {
            routeId,
            0,
            taskId,
            0,
            1,
            kFirstDestinationTaskId + index,
            0,
            10000 + index,
            1 + index,
            kTensorBytes,
        };
        resources[1 + index] = {
            10000 + index,
            routeId,
            1 + index,
            ResourceKind::RouteOutput,
            ElementType::Float32,
            1,
            0,
            ResourceWorkspace,
            kTensorBytes,
            static_cast<uint64_t>(index) * kTensorBytes,
        };
        bindingData[2 * index] = 0;
        bindingData[2 * index + 1] = 1 + index;
        bindings[index] = {
            taskId,
            2 * index,
            1,
            2 * index + 1,
            1,
            2 * kFanout,
            0,
            0,
        };
    }
    const TileABI abi{
        0,
        nullptr,
        0,
        {tasks, kFanout},
        nullptr,
        0,
        outgoing,
        kFanout,
        inputs,
        1,
        nullptr,
        0,
        resources,
        kFanout + 1,
        dimensions,
        1,
        kFanout * kTensorBytes,
        bindings,
        kFanout,
        bindingData,
        2 * kFanout,
    };

    alignas(64) float modelInput[kElementCount];
    for (uint32_t index = 0; index < kElementCount; ++index) {
        modelInput[index] = static_cast<float>(index + 1);
    }
    DeploymentProfile profile{nullptr, readCycle};
    DeploymentTrace trace{nullptr, emitTaskTrace};
    DeploymentRuntime runtime{
        abi,
        kTransport,
        &profile,
        &trace,
#if defined(MITTENS_RUNTIME_ASYNC_TRANSMIT)
        DeploymentTransmitPolicy::OverlapReadyTasks
#else
        DeploymentTransmitPolicy::Blocking
#endif
    };
    if (!runtime.bindModelInput(0, modelInput)) {
        uart_puts("FANOUT_SOURCE_FAIL\n");
        return 1;
    }
    mesh_nic::complete_memory_initialization();
    while (!runtime.complete() && !runtime.failed()) {
        const DeploymentStep step = runtime.step();
        if (step == DeploymentStep::WaitForReceive) {
            mesh_nic::wait_for_receive();
        } else if (step == DeploymentStep::WaitForTransmit) {
            mesh_nic::wait_for_transmit();
        }
    }
    if (runtime.failed()) {
        uart_puts("FANOUT_SOURCE_FAIL\n");
        return 2;
    }
    uart_puts("FANOUT_SOURCE_PASS fanout=");
    printUnsigned(kFanout);
    printProfile(profile);
    uart_putc('\n');
    return 0;
}
#endif

[[maybe_unused]] int runDestination()
{
    Task tasks[kFanout]{};
    Route incoming[kFanout]{};
    Resource resources[kFanout]{};
    TaskBinding bindings[kFanout]{};
    uint32_t bindingData[kFanout]{};
    const int64_t dimensions[] = {kElementCount};
    for (uint32_t index = 0; index < kFanout; ++index) {
        const uint32_t taskId =
            kFirstDestinationTaskId + index;
        const uint32_t routeId = kFirstRouteId + index;
        tasks[index] = {taskId, consumeVector, 1, 0};
        incoming[index] = {
            routeId,
            0,
            sourceTaskId(index),
            0,
            1,
            taskId,
            0,
            10000 + index,
            index,
            kTensorBytes,
        };
        resources[index] = {
            10000 + index,
            routeId,
            index,
            ResourceKind::RouteInput,
            ElementType::Float32,
            1,
            0,
            ResourceWorkspace,
            kTensorBytes,
            static_cast<uint64_t>(index) * kTensorBytes,
        };
        bindingData[index] = index;
        bindings[index] = {
            taskId,
            index,
            1,
            kFanout,
            0,
            kFanout,
            0,
            0,
        };
    }
    const TileABI abi{
        1,
        nullptr,
        0,
        {tasks, kFanout},
        incoming,
        kFanout,
        nullptr,
        0,
        nullptr,
        0,
        nullptr,
        0,
        resources,
        kFanout,
        dimensions,
        1,
        kFanout * kTensorBytes,
        bindings,
        kFanout,
        bindingData,
        kFanout,
    };

    DeploymentProfile profile{nullptr, readCycle};
    DeploymentTrace trace{nullptr, emitTaskTrace};
    DeploymentRuntime runtime{abi, kTransport, &profile, &trace};
    if (!runtime.initialize()) {
        uart_puts("FANOUT_DESTINATION_FAIL\n");
        return 3;
    }
    mesh_nic::complete_memory_initialization();
    while (!runtime.complete() && !runtime.failed()) {
        const DeploymentStep step = runtime.step();
        if (step == DeploymentStep::WaitForReceive) {
            mesh_nic::wait_for_receive();
        } else if (step == DeploymentStep::WaitForTransmit) {
            mesh_nic::wait_for_transmit();
        }
    }
    if (runtime.failed()) {
        uart_puts("FANOUT_DESTINATION_FAIL\n");
        return 4;
    }
    uart_puts("FANOUT_DESTINATION_PASS fanout=");
    printUnsigned(kFanout);
    printProfile(profile);
    uart_putc('\n');
    return 0;
}

} // namespace

extern "C" int tile_main()
{
    if constexpr (MITTENS_TILE_ID == 0) {
#if defined(MITTENS_FANOUT_INDEPENDENT_TASKS)
        return runIndependentSource();
#else
        return runSource();
#endif
    }
    return runDestination();
}
