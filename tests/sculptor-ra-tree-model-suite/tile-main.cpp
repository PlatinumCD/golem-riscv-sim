#include <stdint.h>

#include "golem/runtime/runtime.h"
#include "mesh-nic.h"
#include "platform.h"

namespace {

using namespace golem::runtime;

constexpr uint32_t kMaxModelIO = 8;
constexpr uint32_t kBufferFloats = 64U * 1024U;

alignas(64) float model_inputs[kMaxModelIO][kBufferFloats];
alignas(64) float model_outputs[kMaxModelIO][kBufferFloats];

bool trySendWord(void*, uint32_t destination, uint32_t word) {
    return mesh_nic::try_send(destination, word);
}

bool tryReceiveWord(void*, RoutedWord* word) {
    return word != nullptr &&
           mesh_nic::try_receive_from(&word->source_tile, &word->payload);
}

bool trySendWords(void*, uint32_t destination, const uint32_t* words,
                  uint32_t word_count) {
    return mesh_nic::try_send_words(destination, words, word_count);
}

bool tryStartReceiveWords(void*, uint32_t source, uint32_t route_id,
                          void* destination, uint32_t word_count) {
    return mesh_nic::try_start_receive_words(
        source, route_id, destination, word_count);
}

bool tryReceiveWordsCompletion(void*, uint32_t* source, uint32_t* route_id) {
    return mesh_nic::try_receive_words_completion(source, route_id);
}

const RoutedWordTransport kTransport{
    nullptr,
    trySendWord,
    tryReceiveWord,
    trySendWords,
    tryStartReceiveWords,
    tryReceiveWordsCompletion,
};

}  // namespace

extern "C" int tile_main() {
    const TileABI abi = linkedTileABI();
    if (abi.model_input_count > kMaxModelIO ||
        abi.model_output_count > kMaxModelIO) {
        uart_puts("SCULPTOR_RA_SIM_ERROR model I/O limit\n");
        return 1;
    }

    for (uint32_t index = 0; index < kMaxModelIO; ++index) {
        for (uint32_t value = 0; value < kBufferFloats; ++value) {
            model_inputs[index][value] = 1.0F;
            model_outputs[index][value] = 0.0F;
        }
    }

    DeploymentRuntime runtime{abi, kTransport};
    if (!runtime.initialize()) {
        uart_puts("SCULPTOR_RA_SIM_ERROR initialize\n");
        return 2;
    }
    for (uint32_t index = 0; index < abi.model_input_count; ++index) {
        if (!runtime.bindModelInput(
                abi.model_inputs[index].model_index, model_inputs[index])) {
            uart_puts("SCULPTOR_RA_SIM_ERROR bind input\n");
            return 3;
        }
    }
    for (uint32_t index = 0; index < abi.model_output_count; ++index) {
        if (!runtime.bindModelOutput(
                abi.model_outputs[index].model_index, model_outputs[index])) {
            uart_puts("SCULPTOR_RA_SIM_ERROR bind output\n");
            return 4;
        }
    }
    if (!runtime.boot()) {
        uart_puts("SCULPTOR_RA_SIM_ERROR boot\n");
        return 5;
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
        uart_puts("SCULPTOR_RA_SIM_ERROR execute\n");
        return 6;
    }
    uart_puts("SCULPTOR_RA_SIM_PASS\n");
    return 0;
}
