#include "analog/analogDevice.h"
#include "analog/nativeAnalogBackend.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

using SST::Mittens::AnalogCompletion;
using SST::Mittens::AnalogDevice;
using SST::Mittens::NativeAnalogBackend;

namespace {

std::uint32_t floatWord(float value)
{
    std::uint32_t word = 0;
    static_assert(sizeof(word) == sizeof(value));
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

float wordFloat(std::uint32_t word)
{
    float value = 0.0F;
    static_assert(sizeof(word) == sizeof(value));
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

MittensAnalogCommand command(
    std::uint32_t operation,
    std::uint64_t operand0,
    std::uint64_t operand1)
{
    return MittensAnalogCommand{operation, 0, operand0, operand1};
}

void tick(AnalogDevice& device, std::size_t cycles)
{
    for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
        device.tick();
    }
}

AnalogCompletion requireCompletion(
    AnalogDevice& device,
    std::uint64_t ticket,
    std::uint32_t operation,
    std::uint32_t arrayId)
{
    assert(device.completionReadyForTicket(ticket));
    std::optional<AnalogCompletion> completion =
        device.takeCompletionForTicket(ticket);
    assert(completion.has_value());
    assert(completion->ticket == ticket);
    assert(completion->operation == operation);
    assert(completion->arrayId == arrayId);
    return std::move(*completion);
}

void requireSuccess(
    AnalogDevice& device,
    std::uint64_t ticket,
    std::uint32_t operation,
    std::uint32_t arrayId)
{
    const AnalogCompletion completion =
        requireCompletion(device, ticket, operation, arrayId);
    assert(completion.response.status ==
           MITTENS_ANALOG_STATUS_SUCCESS);
    assert(completion.outputWords.empty());
}

void requireOutput(
    AnalogDevice& device,
    std::uint64_t ticket,
    std::uint32_t arrayId,
    float expectedValue)
{
    const AnalogCompletion completion = requireCompletion(
        device,
        ticket,
        MITTENS_ANALOG_OPERATION_STORE_VECTOR,
        arrayId);
    assert(completion.response.status ==
           MITTENS_ANALOG_STATUS_SUCCESS);
    assert(completion.outputWords.size() == 9);
    for (const std::uint32_t word : completion.outputWords) {
        assert(wordFloat(word) == expectedValue);
    }
}

} // namespace

int main()
{
    AnalogDevice device(
        7,
        4,
        std::make_unique<NativeAnalogBackend>(2, 9, 9));

    assert(device.tileId() == 7);
    assert(device.arrayCount() == 2);
    assert(device.queueDepth() == 4);
    assert(AnalogDevice::linkCyclesForWords(0) == 0);
    assert(AnalogDevice::linkCyclesForWords(8) == 1);
    assert(AnalogDevice::linkCyclesForWords(9) == 2);
    assert(AnalogDevice::linkCyclesForWords(18) == 3);
    assert(AnalogDevice::linkCyclesForWords(81) == 11);

    std::vector<std::uint32_t> matrix0(81, floatWord(1.0F));
    std::vector<std::uint32_t> matrix1(81, floatWord(0.0F));
    for (std::size_t index = 0; index < 9; ++index) {
        matrix1[index * 9 + index] = floatWord(1.0F);
    }
    const std::vector<std::uint32_t> input(9, floatWord(1.0F));

    const std::uint64_t set0 = device.submit(
        command(MITTENS_ANALOG_OPERATION_SET_MATRIX, 0x80001000, 0),
        matrix0);
    const std::uint64_t load0 = device.submit(
        command(MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 0x80002000, 0),
        input);
    const std::uint64_t compute0 = device.submit(
        command(MITTENS_ANALOG_OPERATION_COMPUTE, 0, 0));
    const std::uint64_t store0 = device.submit(
        command(MITTENS_ANALOG_OPERATION_STORE_VECTOR, 0x80003000, 0));

    const std::uint64_t set1 = device.submit(
        command(MITTENS_ANALOG_OPERATION_SET_MATRIX, 0x80004000, 1),
        matrix1);
    const std::uint64_t load1 = device.submit(
        command(MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 0x80005000, 1),
        input);
    const std::uint64_t compute1 = device.submit(
        command(MITTENS_ANALOG_OPERATION_COMPUTE, 1, 1));
    const std::uint64_t store1 = device.submit(
        command(MITTENS_ANALOG_OPERATION_STORE_VECTOR, 0x80006000, 1));

    assert(device.queuedCommands(0) == 4);
    assert(device.queuedCommands(1) == 4);
    assert(!device.canSubmit(
        command(MITTENS_ANALOG_OPERATION_COMPUTE, 0, 0)));

    bool queueFullRejected = false;
    try {
        (void)device.submit(
            command(MITTENS_ANALOG_OPERATION_COMPUTE, 0, 0));
    } catch (const std::logic_error&) {
        queueFullRejected = true;
    }
    assert(queueFullRejected);

    // Both 81-word matrices use their own links and finish in 11 cycles,
    // rather than serializing into 22 cycles.
    tick(device, 10);
    assert(!device.completionReadyForTicket(set0));
    assert(!device.completionReadyForTicket(set1));
    device.tick();
    requireSuccess(
        device, set0, MITTENS_ANALOG_OPERATION_SET_MATRIX, 0);
    requireSuccess(
        device, set1, MITTENS_ANALOG_OPERATION_SET_MATRIX, 1);

    // Both nine-word input vectors overlap and finish in two cycles.
    device.tick();
    assert(!device.completionReadyForTicket(load0));
    assert(!device.completionReadyForTicket(load1));
    device.tick();
    requireSuccess(
        device, load0, MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 0);
    requireSuccess(
        device, load1, MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 1);

    // Independent four-cycle compute intervals overlap.
    tick(device, 3);
    assert(!device.completionReadyForTicket(compute0));
    assert(!device.completionReadyForTicket(compute1));
    device.tick();
    requireSuccess(
        device, compute0, MITTENS_ANALOG_OPERATION_COMPUTE, 0);
    requireSuccess(
        device, compute1, MITTENS_ANALOG_OPERATION_COMPUTE, 1);

    // StoreVector is now timed in the reverse direction. Both nine-word
    // outputs need two cycles and use their independent array links.
    device.tick();
    assert(!device.completionReadyForTicket(store0));
    assert(!device.completionReadyForTicket(store1));
    device.tick();
    requireOutput(device, store0, 0, 9.0F);
    requireOutput(device, store1, 1, 1.0F);
    assert(device.elapsedCycles() == 19);
    assert(!device.busy());

    // MoveVector reserves both links and transfers nine words in two cycles.
    const std::uint64_t move01 = device.submit(
        command(MITTENS_ANALOG_OPERATION_MOVE_VECTOR, 0, 1));
    device.tick();
    assert(!device.completionReadyForTicket(move01));
    device.tick();
    requireSuccess(
        device, move01, MITTENS_ANALOG_OPERATION_MOVE_VECTOR, 0);

    const std::uint64_t recompute1 = device.submit(
        command(MITTENS_ANALOG_OPERATION_COMPUTE, 1, 1));
    tick(device, 4);
    requireSuccess(
        device, recompute1, MITTENS_ANALOG_OPERATION_COMPUTE, 1);
    const std::uint64_t movedStore1 = device.submit(
        command(MITTENS_ANALOG_OPERATION_STORE_VECTOR, 0x80007000, 1));
    tick(device, 2);
    requireOutput(device, movedStore1, 1, 9.0F);
    assert(device.elapsedCycles() == 27);

    // Opposite directions on different array links progress concurrently.
    const std::uint64_t reverse0 = device.submit(
        command(MITTENS_ANALOG_OPERATION_STORE_VECTOR, 0x80008000, 0));
    const std::uint64_t forward1 = device.submit(
        command(MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 0x80009000, 1),
        input);
    device.tick();
    assert(!device.completionReadyForTicket(reverse0));
    assert(!device.completionReadyForTicket(forward1));
    device.tick();
    requireOutput(device, reverse0, 0, 9.0F);
    requireSuccess(
        device, forward1, MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 1);
    assert(device.elapsedCycles() == 29);

    // The same array link is half-duplex: its queued input does not advance
    // during the two cycles consumed by the preceding output.
    const std::uint64_t reverseAgain0 = device.submit(
        command(MITTENS_ANALOG_OPERATION_STORE_VECTOR, 0x8000a000, 0));
    const std::uint64_t forward0 = device.submit(
        command(MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 0x8000b000, 0),
        input);
    tick(device, 2);
    requireOutput(device, reverseAgain0, 0, 9.0F);
    assert(!device.completionReadyForTicket(forward0));
    device.tick();
    assert(!device.completionReadyForTicket(forward0));
    device.tick();
    requireSuccess(
        device, forward0, MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 0);
    assert(device.elapsedCycles() == 33);

    const std::uint64_t invalidArray = device.submit(
        command(MITTENS_ANALOG_OPERATION_COMPUTE, 99, 0));
    const AnalogCompletion invalid = requireCompletion(
        device,
        invalidArray,
        MITTENS_ANALOG_OPERATION_COMPUTE,
        99);
    assert(invalid.response.status ==
           MITTENS_ANALOG_STATUS_INVALID_ARRAY);

    const std::uint64_t invalidPayload = device.submit(
        command(MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 0, 0),
        {floatWord(1.0F)});
    const AnalogCompletion badPayload = requireCompletion(
        device,
        invalidPayload,
        MITTENS_ANALOG_OPERATION_LOAD_VECTOR,
        0);
    assert(badPayload.response.status ==
           MITTENS_ANALOG_STATUS_INVALID_PAYLOAD);

    assert(!device.busy());
    assert(!device.requiresTick());
    return 0;
}
