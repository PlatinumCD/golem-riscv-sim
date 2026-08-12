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

#ifndef MITTENS_FANOUT_ELEMENT_COUNT
#define MITTENS_FANOUT_ELEMENT_COUNT 512
#endif

#ifndef MITTENS_FANOUT_SOURCE_COUNT
#define MITTENS_FANOUT_SOURCE_COUNT 1
#endif

#ifndef MITTENS_FANOUT_DESTINATION_TILE
#define MITTENS_FANOUT_DESTINATION_TILE 1
#endif

#ifndef MITTENS_FANOUT_WAVES
#define MITTENS_FANOUT_WAVES 1
#endif

namespace {

using namespace golem::runtime;

constexpr uint32_t kElementCount = MITTENS_FANOUT_ELEMENT_COUNT;
constexpr uint32_t kFanout = MITTENS_FANOUT;
constexpr uint32_t kSourceCount = MITTENS_FANOUT_SOURCE_COUNT;
constexpr uint32_t kDestinationTile = MITTENS_FANOUT_DESTINATION_TILE;
[[maybe_unused]] constexpr uint32_t kWaves = MITTENS_FANOUT_WAVES;
constexpr uint32_t kSourceTaskId = 11;
constexpr uint32_t kFirstDestinationTaskId = 100;
constexpr uint32_t kFirstRouteId = 1000;
constexpr uint64_t kTensorBytes =
    static_cast<uint64_t>(kElementCount) * sizeof(float);

static_assert(kFanout >= 1 && kFanout <= 24);
static_assert(kSourceCount >= 1);
#if !defined(MITTENS_FANOUT_BIDIRECTIONAL)
static_assert(kDestinationTile >= kSourceCount);
#endif

constexpr uint32_t sourceTaskId(
    uint32_t source,
    uint32_t index)
{
    const uint32_t base = kSourceTaskId + source * kFanout;
#if defined(MITTENS_FANOUT_INDEPENDENT_TASKS)
    return base + index;
#else
    (void)index;
    return base;
#endif
}

constexpr uint32_t routeId(uint32_t source, uint32_t index)
{
    return kFirstRouteId + source * kFanout + index;
}

constexpr uint32_t destinationTaskId(
    uint32_t source,
    uint32_t index)
{
    return kFirstDestinationTaskId + source * kFanout + index;
}

constexpr uint32_t routeResourceId(
    uint32_t source,
    uint32_t index)
{
#if defined(MITTENS_FANOUT_SHARED_RESOURCE_ID)
    (void)index;
    return 10000 + source;
#else
    return 10000 + source * kFanout + index;
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
        {sourceTaskId(MITTENS_TILE_ID, 0), copyVector, 1, 1},
    };
    Route outgoing[kFanout]{};
    for (uint32_t index = 0; index < kFanout; ++index) {
        outgoing[index] = {
            routeId(MITTENS_TILE_ID, index),
            MITTENS_TILE_ID,
            sourceTaskId(MITTENS_TILE_ID, 0),
            0,
            kDestinationTile,
            destinationTaskId(MITTENS_TILE_ID, index),
            0,
            routeResourceId(MITTENS_TILE_ID, index),
            1,
            kTensorBytes,
        };
    }
    const ModelIO inputs[] = {
        {0, MITTENS_TILE_ID, 1, 0, kTensorBytes},
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
            routeResourceId(MITTENS_TILE_ID, 0),
            routeId(MITTENS_TILE_ID, 0),
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
        {sourceTaskId(MITTENS_TILE_ID, 0), 0, 1, 1, 1, 2, 0, 0},
    };
    const TileABI abi{
        MITTENS_TILE_ID,
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
        const uint32_t taskId = sourceTaskId(MITTENS_TILE_ID, index);
        const uint32_t currentRouteId = routeId(MITTENS_TILE_ID, index);
        tasks[index] = {taskId, copyVector, 1, 1};
        outgoing[index] = {
            currentRouteId,
            MITTENS_TILE_ID,
            taskId,
            0,
            kDestinationTile,
            destinationTaskId(MITTENS_TILE_ID, index),
            0,
            routeResourceId(MITTENS_TILE_ID, index),
            1 + index,
            kTensorBytes,
        };
        resources[1 + index] = {
            routeResourceId(MITTENS_TILE_ID, index),
            currentRouteId,
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
        MITTENS_TILE_ID,
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
    constexpr uint32_t kIncomingCount = kSourceCount * kFanout;
    Task tasks[kIncomingCount]{};
    Route incoming[kIncomingCount]{};
    Resource resources[kIncomingCount]{};
    TaskBinding bindings[kIncomingCount]{};
    uint32_t bindingData[kIncomingCount]{};
    const int64_t dimensions[] = {kElementCount};
    for (uint32_t flat = 0; flat < kIncomingCount; ++flat) {
        const uint32_t source = flat / kFanout;
        const uint32_t index = flat % kFanout;
        const uint32_t taskId = destinationTaskId(source, index);
        const uint32_t currentRouteId = routeId(source, index);
        tasks[flat] = {taskId, consumeVector, 1, 0};
        incoming[flat] = {
            currentRouteId,
            source,
            sourceTaskId(source, index),
            0,
            kDestinationTile,
            taskId,
            0,
            routeResourceId(source, index),
            flat,
            kTensorBytes,
        };
        resources[flat] = {
            routeResourceId(source, index),
            currentRouteId,
            flat,
            ResourceKind::RouteInput,
            ElementType::Float32,
            1,
            0,
            ResourceWorkspace,
            kTensorBytes,
            static_cast<uint64_t>(flat) * kTensorBytes,
        };
        bindingData[flat] = flat;
        bindings[flat] = {
            taskId,
            flat,
            1,
            kIncomingCount,
            0,
            kIncomingCount,
            0,
            0,
        };
    }
    const TileABI abi{
        kDestinationTile,
        nullptr,
        0,
        {tasks, kIncomingCount},
        incoming,
        kIncomingCount,
        nullptr,
        0,
        nullptr,
        0,
        nullptr,
        0,
        resources,
        kIncomingCount,
        dimensions,
        1,
        kIncomingCount * kTensorBytes,
        bindings,
        kIncomingCount,
        bindingData,
        kIncomingCount,
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

#if defined(MITTENS_FANOUT_BIDIRECTIONAL)
int runBidirectional()
{
    static_assert(kSourceCount >= 2 && kSourceCount % 2 == 0);
    static_assert(kWaves >= 1);
    constexpr uint32_t kRouteCount = kWaves * kFanout;
    constexpr uint32_t kTaskCount = kWaves + kRouteCount;
    constexpr uint32_t kResourceCount = 1 + kWaves + kRouteCount;
    constexpr uint32_t kBindingDataCount = 2 * kWaves + kRouteCount;
    const uint32_t tile = MITTENS_TILE_ID;
    const uint32_t peer = kSourceCount - 1 - tile;
    const auto bidirectionalSourceTaskId = [](uint32_t source, uint32_t wave) {
        return UINT32_C(10000) + source * kWaves + wave;
    };
    const auto bidirectionalRouteId = [](uint32_t source,
                                         uint32_t wave,
                                         uint32_t index) {
        return UINT32_C(1000000) +
               (source * kWaves + wave) * kFanout + index;
    };
    const auto bidirectionalDestinationTaskId = [](uint32_t source,
                                                   uint32_t wave,
                                                   uint32_t index) {
        return UINT32_C(100000) +
               (source * kWaves + wave) * kFanout + index;
    };
    const auto bidirectionalResourceId = [](uint32_t source,
                                            uint32_t wave) {
        return UINT32_C(300000) + source * kWaves + wave;
    };
    Task tasks[kTaskCount]{};
    Route outgoing[kRouteCount]{};
    Route incoming[kRouteCount]{};
    Resource resources[kResourceCount]{};
    TaskBinding bindings[kTaskCount]{};
    uint32_t bindingData[kBindingDataCount]{};
    const ModelIO inputs[] = {
        {0, tile, 1, 0, kTensorBytes},
    };
    const int64_t dimensions[] = {kElementCount};
    resources[0] = {
        1, 0, 0, ResourceKind::ModelInput, ElementType::Float32,
        1, 0, ResourceExternal, kTensorBytes, 0,
    };
    for (uint32_t wave = 0; wave < kWaves; ++wave) {
        const uint32_t taskId = bidirectionalSourceTaskId(tile, wave);
        const uint32_t peerTaskId =
            bidirectionalSourceTaskId(peer, wave);
        const uint32_t outputSlot = 1 + wave;
        tasks[wave] = {taskId, copyVector, 1, 1};
        resources[outputSlot] = {
            bidirectionalResourceId(tile, wave),
            bidirectionalRouteId(tile, wave, 0),
            outputSlot,
            ResourceKind::RouteOutput,
            ElementType::Float32,
            1,
            0,
            ResourceWorkspace,
            kTensorBytes,
            static_cast<uint64_t>(wave) * kTensorBytes,
        };
        bindingData[2 * wave] = 0;
        bindingData[2 * wave + 1] = outputSlot;
        bindings[wave] = {
            taskId,
            2 * wave,
            1,
            2 * wave + 1,
            1,
            kBindingDataCount,
            0,
            0,
        };
        for (uint32_t index = 0; index < kFanout; ++index) {
            const uint32_t routeIndex = wave * kFanout + index;
            const uint32_t inputSlot = 1 + kWaves + routeIndex;
            const uint32_t destinationTask =
                bidirectionalDestinationTaskId(peer, wave, index);
            outgoing[routeIndex] = {
                bidirectionalRouteId(tile, wave, index),
                tile,
                taskId,
                0,
                peer,
                bidirectionalDestinationTaskId(tile, wave, index),
                0,
                bidirectionalResourceId(tile, wave),
                outputSlot,
                kTensorBytes,
            };
            incoming[routeIndex] = {
                bidirectionalRouteId(peer, wave, index),
                peer,
                peerTaskId,
                0,
                tile,
                destinationTask,
                0,
                bidirectionalResourceId(peer, wave),
                inputSlot,
                kTensorBytes,
            };
            resources[inputSlot] = {
                bidirectionalResourceId(peer, wave),
                bidirectionalRouteId(peer, wave, index),
                inputSlot,
                ResourceKind::RouteInput,
                ElementType::Float32,
                1,
                0,
                ResourceWorkspace,
                kTensorBytes,
                static_cast<uint64_t>(kWaves + routeIndex) *
                    kTensorBytes,
            };
            const uint32_t taskIndex = kWaves + routeIndex;
            tasks[taskIndex] = {destinationTask, consumeVector, 1, 0};
            const uint32_t bindingOffset = 2 * kWaves + routeIndex;
            bindingData[bindingOffset] = inputSlot;
            bindings[taskIndex] = {
                destinationTask,
                bindingOffset,
                1,
                kBindingDataCount,
                0,
                kBindingDataCount,
                0,
                0,
            };
        }
    }
    const TileABI abi{
        tile,
        nullptr,
        0,
        {tasks, kTaskCount},
        incoming,
        kRouteCount,
        outgoing,
        kRouteCount,
        inputs,
        1,
        nullptr,
        0,
        resources,
        kResourceCount,
        dimensions,
        1,
        static_cast<uint64_t>(kWaves + kRouteCount) * kTensorBytes,
        bindings,
        kTaskCount,
        bindingData,
        kBindingDataCount,
    };

    alignas(64) float modelInput[kElementCount];
    for (uint32_t index = 0; index < kElementCount; ++index) {
        modelInput[index] = static_cast<float>(index + 1);
    }
    DeploymentTrace trace{nullptr, emitTaskTrace};
    DeploymentRuntime runtime{abi, kTransport, nullptr, &trace};
    if (!runtime.bindModelInput(0, modelInput)) {
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
        return 2;
    }
    uart_puts("FANOUT_BIDIRECTIONAL_PASS tile=");
    printUnsigned(tile);
    uart_puts(" waves=");
    printUnsigned(kWaves);
    uart_putc('\n');
    return 0;
}
#endif

} // namespace

extern "C" int tile_main()
{
#if defined(MITTENS_FANOUT_BIDIRECTIONAL)
    return runBidirectional();
#else
    if constexpr (MITTENS_TILE_ID < kSourceCount) {
#if defined(MITTENS_FANOUT_INDEPENDENT_TASKS)
        return runIndependentSource();
#else
        return runSource();
#endif
    }
    if constexpr (MITTENS_TILE_ID == kDestinationTile) {
        return runDestination();
    }
    return 1;
#endif
}
