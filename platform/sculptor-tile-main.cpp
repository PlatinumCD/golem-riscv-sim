#include <stddef.h>
#include <stdint.h>

#include "golem/runtime/runtime.h"
#include "mesh-nic.h"
#include "platform.h"

namespace {

using namespace golem::runtime;

constexpr uint32_t kMaxModelIO = 8;

extern "C" void* malloc(size_t size);

template <typename Element>
void fillElements(void* data, size_t count, Element value) {
    auto* elements = static_cast<Element*>(data);
    for (size_t index = 0; index < count; ++index) {
        elements[index] = value;
    }
}

bool initializeModelInput(
    const TileABI& abi,
    const ModelIO& input,
    void* data
) {
    const Resource* resource = abi.findResource(input.local_slot);
    if (resource == nullptr ||
        resource->kind != ResourceKind::ModelInput ||
        resource->byte_size != input.byte_size) {
        return false;
    }

    const uint32_t element_size = elementSizeBytes(resource->element_type);
    if (element_size == 0 ||
        input.byte_size == 0 ||
        input.byte_size % element_size != 0 ||
        input.byte_size / element_size > SIZE_MAX) {
        return false;
    }
    const size_t element_count =
        static_cast<size_t>(input.byte_size / element_size);

    switch (resource->element_type) {
        case ElementType::Float32:
            fillElements(data, element_count, 1.0F);
            return true;
        case ElementType::Int8:
            fillElements(data, element_count, static_cast<int8_t>(1));
            return true;
        case ElementType::UInt8:
            fillElements(data, element_count, static_cast<uint8_t>(1));
            return true;
        case ElementType::Int16:
            fillElements(data, element_count, static_cast<int16_t>(1));
            return true;
        case ElementType::UInt16:
            fillElements(data, element_count, static_cast<uint16_t>(1));
            return true;
        case ElementType::Int32:
            fillElements(data, element_count, static_cast<int32_t>(1));
            return true;
        case ElementType::UInt32:
            fillElements(data, element_count, static_cast<uint32_t>(1));
            return true;
        case ElementType::Invalid:
            return false;
    }

    return false;
}

void* allocateModelIO(
    const TileABI& abi,
    const ModelIO& model_io,
    ResourceKind expected_kind
) {
    const Resource* resource = abi.findResource(model_io.local_slot);
    if (resource == nullptr ||
        resource->kind != expected_kind ||
        resource->byte_size != model_io.byte_size ||
        model_io.owner_core != abi.core_id ||
        model_io.byte_size == 0 ||
        model_io.byte_size > SIZE_MAX) {
        return nullptr;
    }
    return malloc(static_cast<size_t>(model_io.byte_size));
}

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

#if defined(GOLEM_ENABLE_TASK_TRACE)
void emitTaskTrace(
    void*,
    TaskTraceEvent event,
    uint32_t task_id,
    ExecutionId execution_id
) {
    mesh_nic::trace_task(
        static_cast<uint32_t>(event), task_id, execution_id);
}

DeploymentTrace kTaskTrace{nullptr, emitTaskTrace};
#endif

}  // namespace

extern "C" int tile_main() {
    const TileABI abi = linkedTileABI();
    if (abi.model_input_count > kMaxModelIO ||
        abi.model_output_count > kMaxModelIO) {
        uart_puts("SCULPTOR_RA_SIM_ERROR model I/O limit\n");
        return 1;
    }

    void* model_inputs[kMaxModelIO]{};
    void* model_outputs[kMaxModelIO]{};

#if defined(GOLEM_ENABLE_TASK_TRACE)
    DeploymentRuntime runtime{abi, kTransport, nullptr, &kTaskTrace};
#else
    DeploymentRuntime runtime{abi, kTransport};
#endif
    if (!runtime.initialize()) {
        uart_puts("SCULPTOR_RA_SIM_ERROR initialize\n");
        return 2;
    }
    for (uint32_t index = 0; index < abi.model_input_count; ++index) {
        const ModelIO& input = abi.model_inputs[index];
        model_inputs[index] =
            allocateModelIO(abi, input, ResourceKind::ModelInput);
        if (model_inputs[index] == nullptr ||
            !initializeModelInput(abi, input, model_inputs[index])) {
            uart_puts("SCULPTOR_RA_SIM_ERROR allocate input\n");
            return 3;
        }
        if (!runtime.bindModelInput(
                input.model_index, model_inputs[index])) {
            uart_puts("SCULPTOR_RA_SIM_ERROR bind input\n");
            return 4;
        }
    }
    for (uint32_t index = 0; index < abi.model_output_count; ++index) {
        const ModelIO& output = abi.model_outputs[index];
        model_outputs[index] =
            allocateModelIO(abi, output, ResourceKind::ModelOutput);
        if (model_outputs[index] == nullptr) {
            uart_puts("SCULPTOR_RA_SIM_ERROR allocate output\n");
            return 5;
        }
        if (!runtime.bindModelOutput(
                output.model_index, model_outputs[index])) {
            uart_puts("SCULPTOR_RA_SIM_ERROR bind output\n");
            return 6;
        }
    }
    if (!runtime.boot()) {
        uart_puts("SCULPTOR_RA_SIM_ERROR boot\n");
        return 7;
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
        return 8;
    }
    uart_puts("SCULPTOR_RA_SIM_PASS\n");
    return 0;
}
