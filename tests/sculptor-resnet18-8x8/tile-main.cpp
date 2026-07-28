#include <stddef.h>
#include <stdint.h>

#include "golem/runtime/runtime.h"
#include "mesh-nic.h"
#include "platform.h"

namespace {

using namespace golem::runtime;

constexpr uint32_t kInputElementCount = 3U * 224U * 224U;
constexpr uint32_t kOutputElementCount = 1000;

alignas(64) float model_input[kInputElementCount];
alignas(64) float model_output[kOutputElementCount];

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

#if defined(MITTENS_RESNET18_RUNTIME_PROFILE)
uint64_t readCycle(void*) {
    uint64_t cycle;
    asm volatile("rdcycle %0" : "=r"(cycle));
    return cycle;
}
#endif

#if defined(MITTENS_RESNET18_TASK_TRACE)
void emitTaskTrace(
    void*,
    TaskTraceEvent event,
    uint32_t task_id,
    ExecutionId execution_id
) {
    const uint32_t marker =
        event == TaskTraceEvent::Start
            ? mesh_nic::kTaskTraceStart
            : mesh_nic::kTaskTraceFinish;
    mesh_nic::trace_task(marker, task_id, execution_id);
}
#endif

void printUnsigned(uint64_t value) {
    char digits[20];
    uint32_t count = 0;
    do {
        digits[count++] =
            static_cast<char>('0' + value % UINT64_C(10));
        value /= UINT64_C(10);
    } while (value != 0);
    while (count != 0) {
        uart_putc(digits[--count]);
    }
}

void printRuntimeFailure(uint32_t core_id, DeploymentError error) {
    uart_puts("RESNET18_TILE_FAIL core=");
    printUnsigned(core_id);
    uart_puts(" error=");
    printUnsigned(static_cast<uint32_t>(error));
    uart_putc('\n');
}

void initializeModelInput() {
    constexpr float denominator =
        static_cast<float>(kInputElementCount - 1U);
    for (uint32_t index = 0; index < kInputElementCount; ++index) {
        model_input[index] =
            -1.0F + 2.0F * static_cast<float>(index) / denominator;
    }
}

uint32_t topOne() {
    uint32_t result = 0;
    for (uint32_t index = 1; index < kOutputElementCount; ++index) {
        if (model_output[index] > model_output[result]) {
            result = index;
        }
    }
    return result;
}

}  // namespace

extern "C" int tile_main() {
#if defined(MITTENS_RESNET18_RUNTIME_PROFILE)
    const uint64_t start_cycle = readCycle(nullptr);
#endif
    const TileABI abi = linkedTileABI();
    const RoutedWordTransport transport{
        nullptr,
        trySend,
        tryReceive,
        trySendWords,
        tryStartReceiveWords,
        tryReceiveWordsCompletion,
    };
#if defined(MITTENS_RESNET18_RUNTIME_PROFILE)
    DeploymentProfile profile{nullptr, readCycle};
#endif
#if defined(MITTENS_RESNET18_TASK_TRACE)
    DeploymentTrace trace{nullptr, emitTaskTrace};
#endif
    DeploymentRuntime runtime{
        abi,
        transport,
#if defined(MITTENS_RESNET18_RUNTIME_PROFILE)
        &profile,
#else
        nullptr,
#endif
#if defined(MITTENS_RESNET18_TASK_TRACE)
        &trace
#else
        nullptr
#endif
    };

    if (!runtime.initialize()) {
        printRuntimeFailure(abi.core_id, runtime.error());
        return 1;
    }

    for (uint32_t index = 0; index < abi.model_input_count; ++index) {
        const ModelIO& input = abi.model_inputs[index];
        if (input.model_index != 0 ||
            input.byte_size != sizeof(model_input)) {
            printRuntimeFailure(
                abi.core_id,
                DeploymentError::InvalidModelIO
            );
            return 2;
        }
        initializeModelInput();
        if (!runtime.bindModelInput(0, model_input)) {
            printRuntimeFailure(abi.core_id, runtime.error());
            return 3;
        }
    }

    for (uint32_t index = 0; index < abi.model_output_count; ++index) {
        const ModelIO& output = abi.model_outputs[index];
        if (output.model_index != 0 ||
            output.byte_size != sizeof(model_output) ||
            !runtime.bindModelOutput(0, model_output)) {
            printRuntimeFailure(
                abi.core_id,
                DeploymentError::InvalidModelIO
            );
            return 4;
        }
    }

    if (!runtime.boot()) {
        printRuntimeFailure(abi.core_id, runtime.error());
        return 5;
    }

#if defined(MITTENS_RESNET18_RUNTIME_PROFILE)
    const uint64_t ready_cycle = readCycle(nullptr);
#endif
    uart_puts("RESNET18_TILE_READY core=");
    printUnsigned(abi.core_id);
    uart_putc('\n');

    while (!runtime.complete() && !runtime.failed()) {
        if (runtime.step() == DeploymentStep::WaitForReceive) {
            mesh_nic::wait_for_receive();
        }
    }
#if defined(MITTENS_RESNET18_RUNTIME_PROFILE)
    const uint64_t complete_cycle = readCycle(nullptr);
#endif
    if (runtime.failed()) {
        printRuntimeFailure(abi.core_id, runtime.error());
        return 6;
    }

#if defined(MITTENS_RESNET18_RUNTIME_PROFILE)
    const uint64_t accounted_runtime_cycles =
        profile.receive_cycles +
        profile.transmit_cycles +
        profile.transmit_blocked_cycles +
        profile.execute_cycles +
        profile.idle_cycles +
        profile.receive_wait_cycles +
        profile.complete_cycles +
        profile.failed_cycles;
    const uint64_t runtime_cycles = complete_cycle - ready_cycle;
    const uint64_t unattributed_runtime_cycles =
        accounted_runtime_cycles < runtime_cycles
            ? runtime_cycles - accounted_runtime_cycles
            : 0;
    uart_puts("RESNET18_CYCLE_PROFILE core=");
    printUnsigned(abi.core_id);
    uart_puts(" total=");
    printUnsigned(complete_cycle - start_cycle);
    uart_puts(" startup=");
    printUnsigned(ready_cycle - start_cycle);
    uart_puts(" runtime=");
    printUnsigned(runtime_cycles);
    uart_puts(" execute=");
    printUnsigned(profile.execute_cycles);
    uart_puts(" idle=");
    printUnsigned(profile.idle_cycles);
    uart_puts(" rx_wait=");
    printUnsigned(profile.receive_wait_cycles);
    uart_puts(" receive=");
    printUnsigned(profile.receive_cycles);
    uart_puts(" transmit=");
    printUnsigned(profile.transmit_cycles);
    uart_puts(" tx_blocked=");
    printUnsigned(profile.transmit_blocked_cycles);
    uart_puts(" unattributed=");
    printUnsigned(unattributed_runtime_cycles);
    uart_putc('\n');

    uart_puts("RESNET18_STEP_PROFILE core=");
    printUnsigned(abi.core_id);
    uart_puts(" execute=");
    printUnsigned(profile.execute_steps);
    uart_puts(" idle=");
    printUnsigned(profile.idle_steps);
    uart_puts(" rx_wait=");
    printUnsigned(profile.receive_wait_steps);
    uart_puts(" receive=");
    printUnsigned(profile.receive_steps);
    uart_puts(" transmit=");
    printUnsigned(profile.transmit_steps);
    uart_puts(" tx_blocked=");
    printUnsigned(profile.transmit_blocked_steps);
    uart_putc('\n');
#endif

    if (abi.model_output_count != 0) {
        const uint32_t top_one = topOne();
        uart_puts("RESNET18_OUTPUT top1=");
        printUnsigned(top_one);
        uart_putc('\n');
        if (top_one != 620) {
            return 7;
        }
    }

    uart_puts("RESNET18_TILE_PASS core=");
    printUnsigned(abi.core_id);
    uart_putc('\n');
    return 0;
}
