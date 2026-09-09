#include "../analog/analogDevice.h"
#include "../analog/nativeAnalogBackend.h"
#include "../analog/timingAnalogBackend.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

using SST::Mittens::AnalogCompletion;
using SST::Mittens::AnalogDevice;
using SST::Mittens::AnalogTraceEvent;
using SST::Mittens::AnalogTracePhase;
using SST::Mittens::NativeAnalogBackend;
using SST::Mittens::TimingAnalogBackend;

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

MittensAnalogCommand compactSetMatrixCommand(
    std::uint64_t operand0,
    std::uint32_t arrayId,
    std::uint32_t rows,
    std::uint32_t columns)
{
    const std::uint64_t operand1 =
        (static_cast<std::uint64_t>(columns) <<
         MITTENS_ANALOG_SET_MATRIX_COLUMNS_SHIFT) |
        (static_cast<std::uint64_t>(rows) <<
         MITTENS_ANALOG_SET_MATRIX_ROWS_SHIFT) |
        arrayId;
    return MittensAnalogCommand{
        MITTENS_ANALOG_OPERATION_SET_MATRIX,
        MITTENS_ANALOG_COMMAND_FLAG_COMPACT_SET_MATRIX,
        operand0,
        operand1,
    };
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

void requireActiveShapeService(
    std::uint32_t validRows,
    std::uint32_t validColumns)
{
    constexpr std::uint32_t kPhysicalRows = 1024;
    constexpr std::uint32_t kPhysicalColumns = 512;
    constexpr std::uint64_t kComputeCycles = 100;

    AnalogDevice device(
        9,
        kComputeCycles,
        std::make_unique<NativeAnalogBackend>(
            1, kPhysicalRows, kPhysicalColumns));
    assert(device.submittedCommandCount() == 0);
    assert(device.completedCommandCount() == 0);
    assert(device.maximumOutstandingCommandCount() == 0);

    const std::size_t matrixWords =
        static_cast<std::size_t>(validRows) * validColumns;
    const std::uint64_t setTicket = device.submit(
        compactSetMatrixCommand(
            0x80000000, 0, validRows, validColumns),
        std::vector<std::uint32_t>(matrixWords, floatWord(1.0F)));
    assert(device.submittedCommandCount() == 1);
    assert(device.completedCommandCount() == 0);
    assert(device.maximumOutstandingCommandCount() == 1);
    const std::uint64_t expectedSetBeats =
        AnalogDevice::linkCyclesForWords(matrixWords);
    tick(device, expectedSetBeats);
    requireSuccess(
        device,
        setTicket,
        MITTENS_ANALOG_OPERATION_SET_MATRIX,
        0);
    assert(device.linkBeats() == expectedSetBeats);

    std::vector<std::uint32_t> input(
        kPhysicalColumns, floatWord(0.0F));
    std::fill_n(input.begin(), validColumns, floatWord(1.0F));
    const std::uint64_t serviceStart = device.elapsedCycles();
    const std::uint64_t linkStart = device.linkBeats();

    const std::uint64_t loadTicket = device.submit(
        command(MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 0x80001000, 0),
        input);
    const std::uint64_t expectedLoadBeats =
        AnalogDevice::linkCyclesForWords(validColumns);
    tick(device, expectedLoadBeats);
    requireSuccess(
        device,
        loadTicket,
        MITTENS_ANALOG_OPERATION_LOAD_VECTOR,
        0);

    const std::uint64_t computeTicket = device.submit(
        command(MITTENS_ANALOG_OPERATION_COMPUTE, 0, 0));
    tick(device, kComputeCycles);
    requireSuccess(
        device,
        computeTicket,
        MITTENS_ANALOG_OPERATION_COMPUTE,
        0);

    const std::uint64_t storeTicket = device.submit(
        command(MITTENS_ANALOG_OPERATION_STORE_VECTOR, 0x80002000, 0));
    const std::uint64_t expectedStoreBeats =
        AnalogDevice::linkCyclesForWords(validRows);
    tick(device, expectedStoreBeats);
    const AnalogCompletion output = requireCompletion(
        device,
        storeTicket,
        MITTENS_ANALOG_OPERATION_STORE_VECTOR,
        0);
    assert(output.response.status == MITTENS_ANALOG_STATUS_SUCCESS);
    assert(output.outputWords.size() == kPhysicalRows);
    for (std::uint32_t row = 0; row < validRows; ++row) {
        assert(wordFloat(output.outputWords[row]) ==
               static_cast<float>(validColumns));
    }
    for (std::uint32_t row = validRows; row < kPhysicalRows; ++row) {
        assert(wordFloat(output.outputWords[row]) == 0.0F);
    }

    assert(device.linkBeats() - linkStart ==
           expectedLoadBeats + expectedStoreBeats);
    assert(device.elapsedCycles() - serviceStart ==
           expectedLoadBeats + kComputeCycles + expectedStoreBeats);
    assert(device.submittedCommandCount() == 4);
    assert(device.completedCommandCount() == 4);
}

} // namespace

int main()
{
    // Production first-convolution shape: 27 activation columns and 64
    // output rows use 4 + 100 + 8 = 112 cycles rather than the padded
    // 64 + 100 + 128 = 292-cycle physical service.
    requireActiveShapeService(64, 27);
    // Beat-boundary and tail cases prove independent ceil(valid / 8)
    // accounting in both link directions.
    requireActiveShapeService(1, 1);
    requireActiveShapeService(65, 9);
    // A fully valid physical array retains the original padded service.
    requireActiveShapeService(1024, 512);

    NativeAnalogBackend paddedBackend(1, 4, 5);
    std::vector<float> paddedMatrix(20, 0.0F);
    paddedMatrix[0] = 1.0F;
    paddedMatrix[1] = 2.0F;
    paddedMatrix[2] = 3.0F;
    paddedMatrix[5] = 4.0F;
    paddedMatrix[6] = 5.0F;
    paddedMatrix[7] = 6.0F;
    paddedBackend.setMatrix(0, paddedMatrix);
    paddedBackend.loadVector(0, {1.0F, 1.0F, 1.0F, 7.0F, 9.0F});
    paddedBackend.compute(0);
    const std::vector<float> paddedOutput = paddedBackend.output(0);
    assert((paddedOutput == std::vector<float>{6.0F, 15.0F, 0.0F, 0.0F}));
    assert(paddedBackend.lastComputeMultiplyCount(0) == 6);

    AnalogDevice compactDevice(
        8,
        4,
        std::make_unique<NativeAnalogBackend>(1, 4, 5));
    const std::vector<std::uint32_t> compactMatrix = {
        floatWord(1.0F), floatWord(2.0F), floatWord(3.0F),
        floatWord(4.0F), floatWord(5.0F), floatWord(6.0F),
    };
    const std::uint64_t compactSet = compactDevice.submit(
        compactSetMatrixCommand(0x80000000, 0, 2, 3),
        compactMatrix);
    compactDevice.tick();
    requireSuccess(
        compactDevice,
        compactSet,
        MITTENS_ANALOG_OPERATION_SET_MATRIX,
        0);
    assert(compactDevice.linkBeats() == 1);

    const std::uint64_t compactLoad = compactDevice.submit(
        command(MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 0x80001000, 0),
        {
            floatWord(1.0F), floatWord(1.0F), floatWord(1.0F),
            floatWord(0.0F), floatWord(0.0F),
        });
    compactDevice.tick();
    requireSuccess(
        compactDevice,
        compactLoad,
        MITTENS_ANALOG_OPERATION_LOAD_VECTOR,
        0);
    const std::uint64_t compactCompute = compactDevice.submit(
        command(MITTENS_ANALOG_OPERATION_COMPUTE, 0, 0));
    tick(compactDevice, 4);
    requireSuccess(
        compactDevice,
        compactCompute,
        MITTENS_ANALOG_OPERATION_COMPUTE,
        0);
    const std::uint64_t compactStore = compactDevice.submit(
        command(MITTENS_ANALOG_OPERATION_STORE_VECTOR, 0x80002000, 0));
    compactDevice.tick();
    const AnalogCompletion compactOutput = requireCompletion(
        compactDevice,
        compactStore,
        MITTENS_ANALOG_OPERATION_STORE_VECTOR,
        0);
    assert(compactOutput.response.status ==
           MITTENS_ANALOG_STATUS_SUCCESS);
    assert(compactOutput.outputWords.size() == 4);
    assert(wordFloat(compactOutput.outputWords[0]) == 6.0F);
    assert(wordFloat(compactOutput.outputWords[1]) == 15.0F);
    assert(wordFloat(compactOutput.outputWords[2]) == 0.0F);
    assert(wordFloat(compactOutput.outputWords[3]) == 0.0F);
    assert(compactDevice.linkBeats() == 3);
    assert(compactDevice.elapsedCycles() == 7);

    const std::uint64_t compactOneByOne = compactDevice.submit(
        compactSetMatrixCommand(0, 0, 1, 1),
        {floatWord(-7.0F)});
    compactDevice.tick();
    requireSuccess(
        compactDevice,
        compactOneByOne,
        MITTENS_ANALOG_OPERATION_SET_MATRIX,
        0);
    assert(compactDevice.linkBeats() == 4);

    // Active-width service may omit only the exact +0.0 suffix inserted by
    // the compiler. Reject a malformed padded input before it consumes any
    // modeled link service or changes backend state.
    const std::uint64_t malformedLinkStart = compactDevice.linkBeats();
    const std::uint64_t malformedCycleStart = compactDevice.elapsedCycles();
    const std::uint64_t malformedPaddedLoad = compactDevice.submit(
        command(MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 0, 0),
        {
            floatWord(1.0F), floatWord(7.0F), floatWord(0.0F),
            floatWord(0.0F), floatWord(0.0F),
        });
    const AnalogCompletion malformedPaddedCompletion = requireCompletion(
        compactDevice,
        malformedPaddedLoad,
        MITTENS_ANALOG_OPERATION_LOAD_VECTOR,
        0);
    assert(malformedPaddedCompletion.response.status ==
           MITTENS_ANALOG_STATUS_INVALID_PAYLOAD);
    assert(compactDevice.linkBeats() == malformedLinkStart);
    assert(compactDevice.elapsedCycles() == malformedCycleStart);

    const std::uint64_t compactOddBoundary = compactDevice.submit(
        compactSetMatrixCommand(0, 0, 3, 5),
        std::vector<std::uint32_t>(15, floatWord(2.0F)));
    tick(compactDevice, 2);
    requireSuccess(
        compactDevice,
        compactOddBoundary,
        MITTENS_ANALOG_OPERATION_SET_MATRIX,
        0);
    assert(compactDevice.linkBeats() == 6);

    const std::uint64_t compactFullBoundary = compactDevice.submit(
        compactSetMatrixCommand(0, 0, 4, 5),
        std::vector<std::uint32_t>(20, floatWord(3.0F)));
    tick(compactDevice, 3);
    requireSuccess(
        compactDevice,
        compactFullBoundary,
        MITTENS_ANALOG_OPERATION_SET_MATRIX,
        0);
    assert(compactDevice.linkBeats() == 9);

    const auto requireInvalidPayload =
        [](AnalogDevice& target,
           const MittensAnalogCommand& invalidCommand,
           std::vector<std::uint32_t> words) {
            const std::uint64_t ticket =
                target.submit(invalidCommand, std::move(words));
            const AnalogCompletion completion = requireCompletion(
                target,
                ticket,
                invalidCommand.operation,
                mittens_analog_set_matrix_array_id(&invalidCommand));
            assert(completion.response.status ==
                   MITTENS_ANALOG_STATUS_INVALID_PAYLOAD);
        };
    requireInvalidPayload(
        compactDevice,
        compactSetMatrixCommand(0, 0, 0, 3),
        {});
    requireInvalidPayload(
        compactDevice,
        compactSetMatrixCommand(0, 0, 2, 0),
        {});
    requireInvalidPayload(
        compactDevice,
        compactSetMatrixCommand(0, 0, 5, 3),
        std::vector<std::uint32_t>(15));
    requireInvalidPayload(
        compactDevice,
        compactSetMatrixCommand(0, 0, 2, 6),
        std::vector<std::uint32_t>(12));
    requireInvalidPayload(
        compactDevice,
        compactSetMatrixCommand(0, 0, 2, 3),
        std::vector<std::uint32_t>(5));
    MittensAnalogCommand unknownFlag =
        compactSetMatrixCommand(0, 0, 2, 3);
    unknownFlag.reserved |= UINT32_C(1) << 31;
    requireInvalidPayload(
        compactDevice, unknownFlag, compactMatrix);
    MittensAnalogCommand nonCanonicalOperand =
        compactSetMatrixCommand(0, 0, 2, 3);
    nonCanonicalOperand.operand1 |= UINT64_C(1) << 32;
    requireInvalidPayload(
        compactDevice, nonCanonicalOperand, compactMatrix);
    MittensAnalogCommand flagOnCompute =
        command(MITTENS_ANALOG_OPERATION_COMPUTE, 0, 0);
    flagOnCompute.reserved =
        MITTENS_ANALOG_COMMAND_FLAG_COMPACT_SET_MATRIX;
    requireInvalidPayload(compactDevice, flagOnCompute, {});

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
    assert(device.maximumOutstandingCommandCount() == 8);
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

    // Both 81-word matrices contend for one shared 256-bit link. Round-robin
    // arbitration gives each array one eight-word beat every other cycle.
    assert(device.cyclesUntilNextTransition() == 21);
    device.advance(20);
    assert(!device.completionReadyForTicket(set0));
    assert(!device.completionReadyForTicket(set1));
    device.tick();
    requireSuccess(
        device, set0, MITTENS_ANALOG_OPERATION_SET_MATRIX, 0);
    assert(!device.completionReadyForTicket(set1));
    device.tick();
    requireSuccess(
        device, set1, MITTENS_ANALOG_OPERATION_SET_MATRIX, 1);

    // The two nine-word inputs require four shared-link beats in total.
    device.tick();
    assert(!device.completionReadyForTicket(load0));
    assert(!device.completionReadyForTicket(load1));
    device.tick();
    assert(!device.completionReadyForTicket(load0));
    assert(!device.completionReadyForTicket(load1));
    device.tick();
    requireSuccess(
        device, load0, MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 0);
    assert(!device.completionReadyForTicket(load1));
    device.tick();
    requireSuccess(
        device, load1, MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 1);

    // Independent four-cycle compute intervals overlap.
    assert(device.cyclesUntilNextTransition() == 3);
    device.advance(3);
    requireSuccess(
        device, compute0, MITTENS_ANALOG_OPERATION_COMPUTE, 0);
    assert(!device.completionReadyForTicket(compute1));
    device.tick();
    requireSuccess(
        device, compute1, MITTENS_ANALOG_OPERATION_COMPUTE, 1);

    // Store 0 used the link while compute 1 consumed its final cycle. Both
    // stores still share one link and consume four beats in total.
    device.tick();
    assert(!device.completionReadyForTicket(store0));
    assert(!device.completionReadyForTicket(store1));
    device.tick();
    requireOutput(device, store0, 0, 9.0F);
    assert(!device.completionReadyForTicket(store1));
    device.tick();
    requireOutput(device, store1, 1, 1.0F);
    assert(device.elapsedCycles() == 33);
    assert(device.linkBeats() == 30);
    assert(!device.busy());

    // MoveVector traverses the shared link in both directions: source array
    // to tile, then tile to destination array.
    const std::uint64_t move01 = device.submit(
        command(MITTENS_ANALOG_OPERATION_MOVE_VECTOR, 0, 1));
    tick(device, 3);
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
    assert(device.elapsedCycles() == 43);
    assert(device.linkBeats() == 36);

    // Opposite directions contend for the same half-duplex tile link.
    const std::uint64_t reverse0 = device.submit(
        command(MITTENS_ANALOG_OPERATION_STORE_VECTOR, 0x80008000, 0));
    const std::uint64_t forward1 = device.submit(
        command(MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 0x80009000, 1),
        input);
    device.tick();
    assert(!device.completionReadyForTicket(reverse0));
    assert(!device.completionReadyForTicket(forward1));
    device.tick();
    assert(!device.completionReadyForTicket(reverse0));
    assert(!device.completionReadyForTicket(forward1));
    device.tick();
    requireOutput(device, reverse0, 0, 9.0F);
    assert(!device.completionReadyForTicket(forward1));
    device.tick();
    requireSuccess(
        device, forward1, MITTENS_ANALOG_OPERATION_LOAD_VECTOR, 1);
    assert(device.elapsedCycles() == 47);
    assert(device.linkBeats() == 40);

    // The shared link is half-duplex: the queued input does not advance
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
    assert(device.elapsedCycles() == 51);
    assert(device.linkBeats() == 44);

    device.setTraceEnabled(true);
    const std::uint64_t tracedCompute = device.submit(
        command(MITTENS_ANALOG_OPERATION_COMPUTE, 0, 0));
    tick(device, 4);
    requireSuccess(
        device,
        tracedCompute,
        MITTENS_ANALOG_OPERATION_COMPUTE,
        0);
    const AnalogTracePhase expectedPhases[] = {
        AnalogTracePhase::Submitted,
        AnalogTracePhase::ComputeStart,
        AnalogTracePhase::ComputeFinish,
        AnalogTracePhase::Complete,
    };
    for (const AnalogTracePhase expected : expectedPhases) {
        const std::optional<AnalogTraceEvent> event =
            device.takeTraceEvent();
        assert(event.has_value());
        assert(event->ticket == tracedCompute);
        assert(event->operation ==
               MITTENS_ANALOG_OPERATION_COMPUTE);
        assert(event->arrayId == 0);
        assert(event->phase == expected);
    }
    assert(!device.takeTraceEvent().has_value());

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

    // Timing-only execution preserves the analog state contract while
    // deliberately substituting zero data for numerical-output-skipped runs.
    TimingAnalogBackend timingBackend(1, 4, 5);
    timingBackend.setMatrix(0, std::vector<float>(20, 3.0F));
    timingBackend.loadVector(0, std::vector<float>(5, 2.0F));
    timingBackend.compute(0);
    assert((timingBackend.output(0) == std::vector<float>(4, 0.0F)));
    return 0;
}
