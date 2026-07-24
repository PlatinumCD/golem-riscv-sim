#include "sharedAnalogMemoryBridge.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

using SST::Mittens::AnalogBridgeSubmission;
using SST::Mittens::AnalogBridgeToken;
using SST::Mittens::SharedAnalogMemoryBridge;

namespace {

AnalogBridgeToken publish(
    MittensAnalogBridgeHeader* mapping,
    std::uint32_t arrayId,
    MittensAnalogCommand command,
    const std::vector<std::uint32_t>& input)
{
    MittensAnalogBridgeChannel* const channel =
        mittens_analog_channel(mapping, arrayId);
    const std::uint32_t sequence =
        mittens_analog_load_relaxed(&channel->write_index);
    MittensAnalogBridgeSlot* const slot =
        mittens_analog_slot(mapping, arrayId, sequence);

    assert(mittens_analog_load_acquire(&slot->state) ==
           MITTENS_ANALOG_SLOT_FREE);
    assert(input.size() <= mapping->words_per_slot);

    slot->sequence = sequence;
    slot->command = command;
    slot->input_word_count =
        static_cast<std::uint32_t>(input.size());
    slot->output_word_count = 0;
    slot->status = MITTENS_ANALOG_STATUS_SUCCESS;
    std::copy(
        input.begin(),
        input.end(),
        mittens_analog_slot_words(slot));
    mittens_analog_store_release(
        &slot->state, MITTENS_ANALOG_SLOT_SUBMITTED);
    mittens_analog_store_release(
        &channel->write_index, sequence + UINT32_C(1));
    return AnalogBridgeToken{arrayId, sequence};
}

void release(
    MittensAnalogBridgeHeader* mapping,
    const AnalogBridgeToken& token)
{
    MittensAnalogBridgeSlot* const slot =
        mittens_analog_slot(
            mapping, token.arrayId, token.sequence);
    assert(mittens_analog_load_acquire(&slot->state) ==
           MITTENS_ANALOG_SLOT_COMPLETED);
    mittens_analog_store_release(
        &slot->state, MITTENS_ANALOG_SLOT_FREE);
}

} // namespace

int main()
{
    SharedAnalogMemoryBridge bridge;
    bridge.create(7, 2, 2, 3);

    assert(bridge.open());
    assert(bridge.arrayCount() == 2);
    assert(bridge.protocolError() ==
           MITTENS_ANALOG_BRIDGE_ERROR_NONE);
    assert(
        bridge.mappingSize() ==
        mittens_analog_bridge_size(2, 2, 3));

    void* const address = mmap(
        nullptr,
        bridge.mappingSize(),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        bridge.fileDescriptor(),
        0);
    assert(address != MAP_FAILED);
    auto* const mapping =
        static_cast<MittensAnalogBridgeHeader*>(address);

    assert(mapping->magic == MITTENS_ANALOG_BRIDGE_MAGIC);
    assert(mapping->version == MITTENS_ANALOG_BRIDGE_VERSION);
    assert(mapping->structure_size == bridge.mappingSize());
    assert(mapping->array_count == 2);
    assert(mapping->array_rows == 2);
    assert(mapping->array_columns == 3);
    assert(mapping->queue_capacity ==
           MITTENS_ANALOG_QUEUE_CAPACITY);
    assert(mapping->words_per_slot == 6);
    assert(mapping->link_width_bits ==
           MITTENS_ANALOG_LINK_WIDTH_BITS);
    assert(mapping->tile_id == 7);

    const MittensAnalogCommand load{
        MITTENS_ANALOG_OPERATION_LOAD_VECTOR,
        0,
        UINT64_C(0x80001000),
        0,
    };
    const AnalogBridgeToken loadToken =
        publish(mapping, 0, load, {1, 2, 3});

    std::optional<AnalogBridgeSubmission> submission =
        bridge.nextSubmission(0);
    assert(submission.has_value());
    assert(submission->token.arrayId == 0);
    assert(submission->token.sequence == 0);
    assert(submission->command.operation ==
           MITTENS_ANALOG_OPERATION_LOAD_VECTOR);
    assert(
        submission->inputWords ==
        std::vector<std::uint32_t>({1, 2, 3}));
    assert(!bridge.nextSubmission(1).has_value());

    bridge.markAccepted(loadToken);
    const MittensAnalogBridgeSlot* slot =
        mittens_analog_slot_const(mapping, 0, 0);
    assert(mittens_analog_load_acquire(&slot->state) ==
           MITTENS_ANALOG_SLOT_ACCEPTED);

    bridge.complete(
        loadToken,
        MITTENS_ANALOG_STATUS_SUCCESS,
        {10, 11});
    assert(mittens_analog_load_acquire(&slot->state) ==
           MITTENS_ANALOG_SLOT_COMPLETED);
    assert(slot->status == MITTENS_ANALOG_STATUS_SUCCESS);
    assert(slot->output_word_count == 2);
    assert(mittens_analog_slot_words_const(slot)[0] == 10);
    assert(mittens_analog_slot_words_const(slot)[1] == 11);
    release(mapping, loadToken);

    // Separate array channels can publish and be accepted independently.
    const MittensAnalogCommand compute0{
        MITTENS_ANALOG_OPERATION_COMPUTE, 0, 0, 0};
    const MittensAnalogCommand compute1{
        MITTENS_ANALOG_OPERATION_COMPUTE, 0, 1, 0};
    const AnalogBridgeToken token0 =
        publish(mapping, 0, compute0, {});
    const AnalogBridgeToken token1 =
        publish(mapping, 1, compute1, {});
    assert(bridge.nextSubmission(0).has_value());
    assert(bridge.nextSubmission(1).has_value());
    bridge.markAccepted(token0);
    bridge.markAccepted(token1);
    bridge.complete(token0, MITTENS_ANALOG_STATUS_SUCCESS, {});
    bridge.complete(token1, MITTENS_ANALOG_STATUS_SUCCESS, {});
    release(mapping, token0);
    release(mapping, token1);

    assert(bridge.protocolError() ==
           MITTENS_ANALOG_BRIDGE_ERROR_NONE);
    assert(munmap(address, bridge.mappingSize()) == 0);
    bridge.close();
    assert(!bridge.open());
    return 0;
}
