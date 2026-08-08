#include "golem/runtime/scratchpad_abi.h"
#include "platform.h"
#include "scratchpad-dma.h"

#include <stdint.h>

namespace {

alignas(64) float input[8] = {1, 2, 3, 4, 5, 6, 7, 8};
alignas(64) float output[4]{};

bool runDMA(
    const golem::runtime::ScratchpadDMADescriptor& descriptor,
    const void* backing_source,
    void* backing_destination
) {
    using golem::runtime::ScratchpadDMADirection;
    using golem::platform::ScratchpadBase;
    auto* scratchpad = reinterpret_cast<void*>(
        ScratchpadBase + descriptor.scratchpad_offset);
    const bool inbound =
        descriptor.direction == ScratchpadDMADirection::BackingToScratchpad;
    golem::platform::scratchpadDMASubmit(
        inbound ? backing_source : scratchpad,
        inbound ? scratchpad : backing_destination,
        static_cast<uint32_t>(descriptor.byte_size),
        descriptor.completion_token_id,
        0,
        static_cast<golem::platform::ScratchpadDMADirection>(
            descriptor.direction));
    return golem::platform::scratchpadDMAWait(
        0, descriptor.completion_token_id);
}

}  // namespace

extern "C" int tile_main() {
    using namespace golem::runtime;
    const ScratchpadABI abi = linkedScratchpadABI();
    if (!abi.valid(256 * 1024) ||
        abi.required_bytes != 128 ||
        abi.descriptor_count != 2) {
        uart_puts("compiler scratchpad ABI invalid\n");
        return 1;
    }
    if (!runDMA(abi.descriptors[0], input, nullptr)) {
        uart_puts("compiler input DMA failed\n");
        return 1;
    }
    const auto* scratchpad_input = reinterpret_cast<const float*>(
        golem::platform::ScratchpadBase +
        abi.descriptors[0].scratchpad_offset);
    auto* scratchpad_output = reinterpret_cast<float*>(
        golem::platform::ScratchpadBase +
        abi.descriptors[1].scratchpad_offset);
    for (uint32_t index = 0; index < 4; ++index) {
        scratchpad_output[index] = scratchpad_input[index] + 1.0F;
    }
    if (!runDMA(abi.descriptors[1], nullptr, output)) {
        uart_puts("compiler output DMA failed\n");
        return 1;
    }
    for (uint32_t index = 0; index < 4; ++index) {
        if (output[index] != input[index] + 1.0F) {
            uart_puts("compiler scratchpad output mismatch\n");
            return 1;
        }
    }
    uart_puts("compiler scratchpad integration: PASS\n");
    return 0;
}
