#include <stddef.h>
#include <stdint.h>

#include "golem/runtime/runtime.h"
#include "mesh-nic.h"
#include "platform.h"

namespace {

using namespace golem::runtime;

#ifndef MITTENS_GPT2_SEQUENCE_LENGTH
#define MITTENS_GPT2_SEQUENCE_LENGTH 4
#endif

#ifndef MITTENS_GPT2_HIDDEN_SIZE
#define MITTENS_GPT2_HIDDEN_SIZE 768
#endif

static_assert(MITTENS_GPT2_SEQUENCE_LENGTH > 0);
static_assert(MITTENS_GPT2_HIDDEN_SIZE > 0);
constexpr uint32_t kElementCount =
    1U * MITTENS_GPT2_SEQUENCE_LENGTH * MITTENS_GPT2_HIDDEN_SIZE;
#if defined(MITTENS_GPT2_ASYNC_TRANSMIT)
constexpr DeploymentTransmitPolicy kTransmitPolicy =
    DeploymentTransmitPolicy::OverlapReadyTasks;
#else
constexpr DeploymentTransmitPolicy kTransmitPolicy =
    DeploymentTransmitPolicy::Blocking;
#endif

alignas(64) float model_input[kElementCount];
alignas(64) float model_output[kElementCount];

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
    return mesh_nic::try_send_words(destination, words, word_count);
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
    return mesh_nic::try_receive_words_completion(source, route_id);
}

#if defined(MITTENS_GPT2_TASK_TRACE)
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
    uart_puts("GPT2_TILE_FAIL core=");
    printUnsigned(core_id);
    uart_puts(" error=");
    printUnsigned(static_cast<uint32_t>(error));
}

void initializeModelInput() {
    for (uint32_t index = 0; index < kElementCount; ++index) {
        model_input[index] =
            static_cast<float>(index + 1U) / 100.0F;
    }
}

uint32_t floatBits(float value) {
    union {
        float value;
        uint32_t bits;
    } conversion{value};
    return conversion.bits;
}

bool isFinite(float value) {
    return (floatBits(value) & UINT32_C(0x7f800000)) !=
           UINT32_C(0x7f800000);
}

}  // namespace

extern "C" int tile_main() {
    const TileABI abi = linkedTileABI();
    const ScratchpadABI scratchpad_abi = linkedScratchpadABI();
    if (!scratchpad_abi.valid(MITTENS_GPT2_SCRATCHPAD_BYTES)) {
        printRuntimeFailure(abi.core_id, DeploymentError::InvalidABI);
        return 8;
    }
    const RoutedWordTransport transport{
        nullptr,
        trySend,
        tryReceive,
        trySendWords,
        tryStartReceiveWords,
        tryReceiveWordsCompletion,
    };
#if defined(MITTENS_GPT2_TASK_TRACE)
    DeploymentTrace trace{nullptr, emitTaskTrace};
    DeploymentRuntime runtime{
        abi, transport, nullptr, &trace, kTransmitPolicy};
#else
    DeploymentRuntime runtime{
        abi, transport, nullptr, nullptr, kTransmitPolicy};
#endif

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
    mesh_nic::complete_memory_initialization();

#if !defined(MITTENS_GPT2_DUMP_OUTPUT_BITS)
    uart_puts("GPT2_TILE_READY core=");
    printUnsigned(abi.core_id);
    uart_putc('\n');
#endif

    while (!runtime.complete() && !runtime.failed()) {
        const DeploymentStep step = runtime.step();
        if (step == DeploymentStep::WaitForReceive) {
            mesh_nic::wait_for_receive();
        } else if (step == DeploymentStep::WaitForTransmit) {
            mesh_nic::wait_for_transmit();
        }
    }
    if (runtime.failed()) {
        printRuntimeFailure(abi.core_id, runtime.error());
        if (runtime.error() == DeploymentError::InvalidFrame) {
            const InvalidFrameDiagnostic& diagnostic =
                runtime.invalidFrameDiagnostic();
            uart_puts(" frame_reason=");
            printUnsigned(
                static_cast<uint32_t>(diagnostic.reason));
            uart_puts(" source=");
            printUnsigned(diagnostic.source_tile);
            uart_puts(" route=");
            printUnsigned(diagnostic.route_id);
            uart_puts(" phase=");
            printUnsigned(diagnostic.receive_phase);
            uart_puts(" expected=");
            printUnsigned(diagnostic.expected);
            uart_puts(" actual=");
            printUnsigned(diagnostic.actual);
        }
        uart_putc('\n');
        return 6;
    }

    if (abi.model_output_count != 0) {
        uint32_t finite_count = 0;
        float checksum = 0.0F;
        for (float value : model_output) {
            if (isFinite(value)) {
                ++finite_count;
            }
            checksum += value;
        }
        uart_puts("GPT2_OUTPUT elements=");
        printUnsigned(kElementCount);
        uart_puts(" finite=");
        printUnsigned(finite_count);
        uart_puts(" first_bits=");
        printUnsigned(floatBits(model_output[0]));
        uart_puts(" checksum_bits=");
        printUnsigned(floatBits(checksum));
        uart_putc('\n');
#if defined(MITTENS_GPT2_DUMP_OUTPUT_BITS)
        uart_puts("GPT2_OUTPUT_BITS ");
        for (uint32_t index = 0; index < kElementCount; ++index) {
            if (index != 0) {
                uart_putc(',');
            }
            printUnsigned(floatBits(model_output[index]));
        }
        uart_putc('\n');
#endif
        if (finite_count != kElementCount) {
            return 7;
        }
    }

#if !defined(MITTENS_GPT2_DUMP_OUTPUT_BITS)
    uart_puts("GPT2_TILE_PASS core=");
    printUnsigned(abi.core_id);
    uart_putc('\n');
#endif
    return 0;
}
