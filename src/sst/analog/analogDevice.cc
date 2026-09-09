#include "analogDevice.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace SST {
namespace Mittens {

AnalogDevice::AnalogDevice(
    std::uint32_t tileId,
    std::uint64_t computeLatencyCycles,
    std::unique_ptr<AnalogBackend> backend,
    std::size_t queueDepth) :
    tileId_(tileId),
    computeLatencyCycles_(computeLatencyCycles),
    backend_(std::move(backend)),
    queueDepth_(queueDepth)
{
    if (backend_ == nullptr) {
        throw std::invalid_argument("analog device requires a backend");
    }
    if (backend_->arrayCount() == 0) {
        throw std::invalid_argument(
            "analog device requires at least one array");
    }
    if (computeLatencyCycles_ == 0) {
        throw std::invalid_argument(
            "analog compute latency must be at least one cycle");
    }
    if (queueDepth_ == 0) {
        throw std::invalid_argument(
            "analog command queue depth must be at least one");
    }

    channels_.resize(backend_->arrayCount());
    for (ArrayChannel& channel : channels_) {
        channel.validRows = backend_->arrayRows();
        channel.validColumns = backend_->arrayColumns();
    }
}

bool AnalogDevice::canSubmit(
    const MittensAnalogCommand& command) const noexcept
{
    if (!supportedOperation(command.operation)) {
        return true;
    }

    std::vector<std::uint32_t> arrays;
    std::uint32_t primaryArray = InvalidArrayId;
    if (!routeCommand(command, arrays, primaryArray)) {
        return true;
    }

    for (const std::uint32_t arrayId : arrays) {
        if (channels_[arrayId].requests.size() >= queueDepth_) {
            return false;
        }
    }
    return true;
}

std::uint64_t AnalogDevice::submit(
    const MittensAnalogCommand& command,
    std::vector<std::uint32_t> inputWords)
{
    const std::uint64_t ticket = nextTicket_++;

    if (!supportedOperation(command.operation)) {
        recordSubmittedCommand();
        completeImmediately(
            command,
            ticket,
            InvalidArrayId,
            MITTENS_ANALOG_STATUS_INVALID_OPERATION);
        return ticket;
    }

    std::vector<std::uint32_t> arrays;
    std::uint32_t primaryArray = InvalidArrayId;
    if (!routeCommand(command, arrays, primaryArray)) {
        recordSubmittedCommand();
        completeImmediately(
            command,
            ticket,
            primaryArray,
            MITTENS_ANALOG_STATUS_INVALID_ARRAY);
        return ticket;
    }

    const std::optional<std::uint64_t> payloadError =
        validatePayload(command, inputWords);
    if (payloadError.has_value()) {
        recordSubmittedCommand();
        completeImmediately(
            command, ticket, primaryArray, *payloadError);
        return ticket;
    }

    if (!canSubmit(command)) {
        throw std::logic_error(
            "selected analog array command queue is full");
    }
    recordSubmittedCommand();

    auto request = std::make_shared<Request>();
    request->ticket = ticket;
    request->command = command;
    request->primaryArray = primaryArray;
    request->arrays = std::move(arrays);
    request->transferWords = std::move(inputWords);
    recordTrace(request, AnalogTracePhase::Submitted);

    for (const std::uint32_t arrayId : request->arrays) {
        channels_[arrayId].requests.push_back(request);
    }

    startReadyRequests();
    return ticket;
}

void AnalogDevice::tick()
{
    startReadyRequests();

    std::vector<std::shared_ptr<Request>> active;
    active.reserve(channels_.size());
    for (const ArrayChannel& channel : channels_) {
        if (channel.requests.empty()) {
            continue;
        }
        const std::shared_ptr<Request>& request = channel.requests.front();
        if (request->phase == Phase::Queued ||
            request->phase == Phase::Complete) {
            continue;
        }
        const auto found = std::find_if(
            active.begin(),
            active.end(),
            [&request](const std::shared_ptr<Request>& candidate) {
                return candidate.get() == request.get();
            });
        if (found == active.end()) {
            active.push_back(request);
        }
    }

    if (active.empty()) {
        return;
    }

    ++elapsedCycles_;

    /*
     * Compute engines are array-local and advance concurrently. All data
     * movement shares one tile-wide, half-duplex 256-bit link, so exactly
     * one transfer request may consume one beat during this tick.
     */
    for (const std::shared_ptr<Request>& request : active) {
        if (request->phase == Phase::Computing) {
            advanceRequest(request);
        }
    }

    std::shared_ptr<Request> linkWinner;
    std::uint32_t bestDistance =
        std::numeric_limits<std::uint32_t>::max();
    for (const std::shared_ptr<Request>& request : active) {
        if (!usesLink(request)) {
            continue;
        }
        const std::uint32_t arrayId = linkArray(request);
        const std::uint32_t distance =
            (arrayId + static_cast<std::uint32_t>(channels_.size()) -
             nextLinkArray_) %
            static_cast<std::uint32_t>(channels_.size());
        if (linkWinner == nullptr || distance < bestDistance ||
            (distance == bestDistance &&
             request->ticket < linkWinner->ticket)) {
            linkWinner = request;
            bestDistance = distance;
        }
    }
    if (linkWinner != nullptr) {
        const std::uint32_t arrayId = linkArray(linkWinner);
        advanceRequest(linkWinner);
        ++linkBeats_;
        nextLinkArray_ =
            (arrayId + 1U) %
            static_cast<std::uint32_t>(channels_.size());
    }

    startReadyRequests();
}

void AnalogDevice::advance(std::uint64_t cycles)
{
    if (cycles == 0) {
        throw std::invalid_argument(
            "analog device advance requires at least one cycle");
    }
    const std::uint64_t horizon = cyclesUntilNextTransition();
    if (cycles > horizon) {
        throw std::logic_error(
            "analog device advance crossed an observable transition");
    }
    for (std::uint64_t cycle = 0; cycle < cycles; ++cycle) {
        tick();
    }
}

std::uint64_t AnalogDevice::cyclesUntilNextTransition() const
{
    std::vector<std::shared_ptr<Request>> active;
    active.reserve(channels_.size());
    for (const ArrayChannel& channel : channels_) {
        if (channel.requests.empty()) {
            continue;
        }
        const std::shared_ptr<Request>& request = channel.requests.front();
        if (request->phase == Phase::Queued ||
            request->phase == Phase::Complete) {
            continue;
        }
        const auto found = std::find_if(
            active.begin(),
            active.end(),
            [&request](const std::shared_ptr<Request>& candidate) {
                return candidate.get() == request.get();
            });
        if (found == active.end()) {
            active.push_back(request);
        }
    }

    std::uint64_t horizon = std::numeric_limits<std::uint64_t>::max();
    for (const std::shared_ptr<Request>& request : active) {
        if (request->phase == Phase::Computing) {
            if (request->remainingComputeCycles == 0) {
                throw std::logic_error(
                    "active analog compute has no remaining cycles");
            }
            horizon = std::min(
                horizon, request->remainingComputeCycles);
        }
    }

    // At most one active request for an array can win the shared link.  The
    // lower-ticket request wins the existing arbitration tie until it
    // completes, so requests sharing a link array are deliberately omitted
    // from this stable arbitration round.
    std::vector<std::shared_ptr<Request>> linkRequests;
    linkRequests.reserve(active.size());
    for (const std::shared_ptr<Request>& request : active) {
        if (!usesLink(request)) {
            continue;
        }
        const std::uint32_t arrayId = linkArray(request);
        auto sameArray = std::find_if(
            linkRequests.begin(),
            linkRequests.end(),
            [this, arrayId](const std::shared_ptr<Request>& candidate) {
                return linkArray(candidate) == arrayId;
            });
        if (sameArray == linkRequests.end()) {
            linkRequests.push_back(request);
        } else if (request->ticket < (*sameArray)->ticket) {
            *sameArray = request;
        }
    }

    std::sort(
        linkRequests.begin(),
        linkRequests.end(),
        [this](const std::shared_ptr<Request>& left,
               const std::shared_ptr<Request>& right) {
            const std::uint32_t leftArray = linkArray(left);
            const std::uint32_t rightArray = linkArray(right);
            const std::uint32_t count =
                static_cast<std::uint32_t>(channels_.size());
            const std::uint32_t leftDistance =
                (leftArray + count - nextLinkArray_) % count;
            const std::uint32_t rightDistance =
                (rightArray + count - nextLinkArray_) % count;
            if (leftDistance != rightDistance) {
                return leftDistance < rightDistance;
            }
            return left->ticket < right->ticket;
        });

    const std::uint64_t contenders = linkRequests.size();
    for (std::size_t position = 0;
         position < linkRequests.size(); ++position) {
        const std::uint64_t beats =
            remainingLinkBeats(linkRequests[position]);
        if (beats == 0) {
            throw std::logic_error(
                "active analog transfer has no remaining beats");
        }
        if (beats - 1 >
            (std::numeric_limits<std::uint64_t>::max() -
             (position + 1)) /
                contenders) {
            throw std::overflow_error(
                "analog transition horizon overflowed");
        }
        const std::uint64_t completionCycle =
            (beats - 1) * contenders + position + 1;
        horizon = std::min(horizon, completionCycle);
    }

    if (horizon == std::numeric_limits<std::uint64_t>::max()) {
        if (busy()) {
            throw std::logic_error(
                "busy analog device has no active transition");
        }
        throw std::logic_error(
            "idle analog device has no next transition");
    }
    return horizon;
}

bool AnalogDevice::busy() const noexcept
{
    return std::any_of(
        channels_.begin(),
        channels_.end(),
        [](const ArrayChannel& channel) {
            return !channel.requests.empty();
        });
}

bool AnalogDevice::busy(std::uint32_t arrayId) const noexcept
{
    return arrayId < channels_.size() &&
           !channels_[arrayId].requests.empty();
}

bool AnalogDevice::requiresTick() const noexcept
{
    return busy();
}

std::size_t AnalogDevice::queuedCommands(std::uint32_t arrayId) const
{
    if (arrayId >= channels_.size()) {
        throw std::out_of_range("analog array ID is invalid");
    }
    return channels_[arrayId].requests.size();
}

bool AnalogDevice::completionReady() const noexcept
{
    return !completions_.empty();
}

bool AnalogDevice::completionReady(std::uint32_t arrayId) const noexcept
{
    return std::any_of(
        completions_.begin(),
        completions_.end(),
        [arrayId](const AnalogCompletion& completion) {
            return completion.arrayId == arrayId;
        });
}

bool AnalogDevice::completionReadyForTicket(
    std::uint64_t ticket) const noexcept
{
    return std::any_of(
        completions_.begin(),
        completions_.end(),
        [ticket](const AnalogCompletion& completion) {
            return completion.ticket == ticket;
        });
}

std::optional<AnalogCompletion> AnalogDevice::takeCompletion()
{
    if (completions_.empty()) {
        return std::nullopt;
    }

    AnalogCompletion completion = std::move(completions_.front());
    completions_.pop_front();
    return completion;
}

std::optional<AnalogCompletion> AnalogDevice::takeCompletion(
    std::uint32_t arrayId)
{
    const auto found = std::find_if(
        completions_.begin(),
        completions_.end(),
        [arrayId](const AnalogCompletion& completion) {
            return completion.arrayId == arrayId;
        });
    if (found == completions_.end()) {
        return std::nullopt;
    }

    AnalogCompletion completion = std::move(*found);
    completions_.erase(found);
    return completion;
}

std::optional<AnalogCompletion> AnalogDevice::takeCompletionForTicket(
    std::uint64_t ticket)
{
    const auto found = std::find_if(
        completions_.begin(),
        completions_.end(),
        [ticket](const AnalogCompletion& completion) {
            return completion.ticket == ticket;
        });
    if (found == completions_.end()) {
        return std::nullopt;
    }

    AnalogCompletion completion = std::move(*found);
    completions_.erase(found);
    return completion;
}

void AnalogDevice::setTraceEnabled(bool enabled) noexcept
{
    traceEnabled_ = enabled;
    if (!traceEnabled_) {
        traceEvents_.clear();
    }
}

std::optional<AnalogTraceEvent> AnalogDevice::takeTraceEvent()
{
    if (traceEvents_.empty()) {
        return std::nullopt;
    }
    AnalogTraceEvent event = traceEvents_.front();
    traceEvents_.pop_front();
    return event;
}

std::size_t AnalogDevice::arrayCount() const noexcept
{
    return backend_->arrayCount();
}

std::uint64_t
AnalogDevice::linkCyclesForWords(std::size_t wordCount) noexcept
{
    return (wordCount + MITTENS_ANALOG_WORDS_PER_BEAT - 1) /
           MITTENS_ANALOG_WORDS_PER_BEAT;
}

bool AnalogDevice::supportedOperation(
    std::uint32_t operation) const noexcept
{
    switch (operation) {
    case MITTENS_ANALOG_OPERATION_SET_MATRIX:
    case MITTENS_ANALOG_OPERATION_LOAD_VECTOR:
    case MITTENS_ANALOG_OPERATION_COMPUTE:
    case MITTENS_ANALOG_OPERATION_STORE_VECTOR:
    case MITTENS_ANALOG_OPERATION_MOVE_VECTOR:
        return true;
    default:
        return false;
    }
}

bool AnalogDevice::routeCommand(
    const MittensAnalogCommand& command,
    std::vector<std::uint32_t>& arrays,
    std::uint32_t& primaryArray) const noexcept
{
    std::uint64_t first = 0;
    std::optional<std::uint64_t> second;

    switch (command.operation) {
    case MITTENS_ANALOG_OPERATION_SET_MATRIX:
        if ((command.reserved &
             MITTENS_ANALOG_COMMAND_FLAG_COMPACT_SET_MATRIX) != 0) {
            first = mittens_analog_set_matrix_array_id(&command);
            break;
        }
        first = command.operand1;
        break;
    case MITTENS_ANALOG_OPERATION_LOAD_VECTOR:
    case MITTENS_ANALOG_OPERATION_STORE_VECTOR:
        first = command.operand1;
        break;
    case MITTENS_ANALOG_OPERATION_COMPUTE:
        first = command.operand0;
        break;
    case MITTENS_ANALOG_OPERATION_MOVE_VECTOR:
        first = command.operand0;
        second = command.operand1;
        break;
    default:
        return false;
    }

    if (first <= std::numeric_limits<std::uint32_t>::max()) {
        primaryArray = static_cast<std::uint32_t>(first);
    }
    if (!validArray(first) ||
        (second.has_value() && !validArray(*second))) {
        return false;
    }

    arrays.push_back(static_cast<std::uint32_t>(first));
    if (second.has_value() && *second != first) {
        arrays.push_back(static_cast<std::uint32_t>(*second));
    }
    return true;
}

bool AnalogDevice::validArray(std::uint64_t arrayId) const noexcept
{
    return arrayId < backend_->arrayCount();
}

std::optional<std::uint64_t> AnalogDevice::validatePayload(
    const MittensAnalogCommand& command,
    const std::vector<std::uint32_t>& inputWords) const noexcept
{
    std::size_t expectedWords = 0;
    const std::uint32_t knownFlags =
        MITTENS_ANALOG_COMMAND_FLAG_COMPACT_SET_MATRIX;

    if ((command.reserved & ~knownFlags) != 0 ||
        (command.operation != MITTENS_ANALOG_OPERATION_SET_MATRIX &&
         command.reserved != 0)) {
        return MITTENS_ANALOG_STATUS_INVALID_PAYLOAD;
    }

    switch (command.operation) {
    case MITTENS_ANALOG_OPERATION_SET_MATRIX:
        if ((command.reserved &
             MITTENS_ANALOG_COMMAND_FLAG_COMPACT_SET_MATRIX) != 0) {
            const std::uint32_t rows =
                mittens_analog_set_matrix_valid_rows(&command);
            const std::uint32_t columns =
                mittens_analog_set_matrix_valid_columns(&command);
            if (rows == 0 || columns == 0 ||
                rows > backend_->arrayRows() ||
                columns > backend_->arrayColumns() ||
                command.operand1 > UINT32_MAX) {
                return MITTENS_ANALOG_STATUS_INVALID_PAYLOAD;
            }
            expectedWords =
                static_cast<std::size_t>(rows) * columns;
        } else {
            expectedWords =
                static_cast<std::size_t>(backend_->arrayRows()) *
                backend_->arrayColumns();
        }
        break;
    case MITTENS_ANALOG_OPERATION_LOAD_VECTOR:
        expectedWords = backend_->arrayColumns();
        break;
    case MITTENS_ANALOG_OPERATION_COMPUTE:
    case MITTENS_ANALOG_OPERATION_STORE_VECTOR:
    case MITTENS_ANALOG_OPERATION_MOVE_VECTOR:
        expectedWords = 0;
        break;
    default:
        return MITTENS_ANALOG_STATUS_INVALID_OPERATION;
    }

    if (inputWords.size() != expectedWords) {
        return MITTENS_ANALOG_STATUS_INVALID_PAYLOAD;
    }
    return std::nullopt;
}

bool AnalogDevice::requestIsReady(
    const std::shared_ptr<Request>& request) const
{
    return std::all_of(
        request->arrays.begin(),
        request->arrays.end(),
        [this, &request](std::uint32_t arrayId) {
            const ArrayChannel& channel = channels_[arrayId];
            return !channel.requests.empty() &&
                   channel.requests.front().get() == request.get();
        });
}

void AnalogDevice::startReadyRequests()
{
    bool changed = false;
    do {
        changed = false;
        std::vector<std::shared_ptr<Request>> candidates;
        candidates.reserve(channels_.size());

        for (const ArrayChannel& channel : channels_) {
            if (channel.requests.empty()) {
                continue;
            }
            const std::shared_ptr<Request>& request =
                channel.requests.front();
            if (request->phase != Phase::Queued ||
                !requestIsReady(request)) {
                continue;
            }
            const auto found = std::find_if(
                candidates.begin(),
                candidates.end(),
                [&request](const std::shared_ptr<Request>& candidate) {
                    return candidate.get() == request.get();
                });
            if (found == candidates.end()) {
                candidates.push_back(request);
            }
        }

        for (const std::shared_ptr<Request>& request : candidates) {
            if (request->phase == Phase::Queued &&
                requestIsReady(request)) {
                startRequest(request);
                changed = true;
            }
        }
    } while (changed);
}

void AnalogDevice::startRequest(
    const std::shared_ptr<Request>& request)
{
    request->transferredWords = 0;
    request->linkWordCount = request->transferWords.size();

    switch (request->command.operation) {
    case MITTENS_ANALOG_OPERATION_SET_MATRIX:
        request->phase = Phase::TransferringInput;
        recordTrace(request, AnalogTracePhase::InputTransferStart);
        return;

    case MITTENS_ANALOG_OPERATION_LOAD_VECTOR: {
        const ArrayChannel& channel = channels_[request->primaryArray];
        if (channel.validColumns == 0 ||
            channel.validColumns > request->transferWords.size()) {
            finishRequest(request, MITTENS_ANALOG_STATUS_INVALID_PAYLOAD);
            return;
        }

        // A compact SetMatrix command is the compiler proof that columns
        // beyond validColumns are physical padding.  Fail closed unless that
        // untransferred suffix is the exact arithmetic +0.0 inserted by the
        // lowering; this prevents active-shape timing from hiding data that
        // could change IEEE-754 results (notably NaN/Inf multiplied by zero).
        if (!std::all_of(
                request->transferWords.begin() + channel.validColumns,
                request->transferWords.end(),
                [](std::uint32_t word) { return word == 0; })) {
            finishRequest(request, MITTENS_ANALOG_STATUS_INVALID_PAYLOAD);
            return;
        }

        request->linkWordCount = channel.validColumns;
        request->phase = Phase::TransferringInput;
        recordTrace(request, AnalogTracePhase::InputTransferStart);
        return;
    }

    case MITTENS_ANALOG_OPERATION_COMPUTE:
        try {
            backend_->compute(request->primaryArray);
        } catch (const std::exception&) {
            finishRequest(
                request, MITTENS_ANALOG_STATUS_BACKEND_ERROR);
            return;
        }
        request->remainingComputeCycles = computeLatencyCycles_;
        request->phase = Phase::Computing;
        recordTrace(request, AnalogTracePhase::ComputeStart);
        return;

    case MITTENS_ANALOG_OPERATION_STORE_VECTOR:
        try {
            request->transferWords = encodeFloatWords(
                backend_->output(request->primaryArray));
        } catch (const std::exception&) {
            finishRequest(
                request, MITTENS_ANALOG_STATUS_BACKEND_ERROR);
            return;
        }
        request->linkWordCount =
            channels_[request->primaryArray].validRows;
        if (request->linkWordCount == 0 ||
            request->linkWordCount > request->transferWords.size()) {
            finishRequest(request, MITTENS_ANALOG_STATUS_BACKEND_ERROR);
            return;
        }
        request->phase = Phase::TransferringOutput;
        recordTrace(request, AnalogTracePhase::OutputTransferStart);
        return;

    case MITTENS_ANALOG_OPERATION_MOVE_VECTOR:
        try {
            request->transferWords = encodeFloatWords(
                backend_->output(request->primaryArray));
        } catch (const std::exception&) {
            finishRequest(
                request, MITTENS_ANALOG_STATUS_BACKEND_ERROR);
            return;
        }
        // MoveVector retains the physical-width contract.  A destination may
        // have a different programmed valid shape, so active-width movement
        // requires a separate compiler proof and is not part of A2.
        request->linkWordCount = request->transferWords.size();
        request->phase = Phase::MovingOutput;
        recordTrace(request, AnalogTracePhase::MoveOutputStart);
        return;

    default:
        finishRequest(
            request, MITTENS_ANALOG_STATUS_INVALID_OPERATION);
        return;
    }
}

void AnalogDevice::advanceRequest(
    const std::shared_ptr<Request>& request)
{
    switch (request->phase) {
    case Phase::TransferringInput:
    case Phase::TransferringOutput:
    case Phase::MovingOutput:
    case Phase::MovingInput: {
        const std::size_t remaining =
            request->linkWordCount - request->transferredWords;
        request->transferredWords += std::min<std::size_t>(
            remaining, MITTENS_ANALOG_WORDS_PER_BEAT);

        if (request->transferredWords !=
            request->linkWordCount) {
            return;
        }

        if (request->phase == Phase::TransferringInput) {
            recordTrace(request, AnalogTracePhase::InputTransferFinish);
            finishInputTransfer(request);
        } else if (request->phase == Phase::MovingOutput) {
            recordTrace(request, AnalogTracePhase::MoveOutputFinish);
            request->transferredWords = 0;
            request->linkWordCount = request->transferWords.size();
            request->phase = Phase::MovingInput;
            recordTrace(request, AnalogTracePhase::MoveInputStart);
        } else if (request->phase == Phase::MovingInput) {
            recordTrace(request, AnalogTracePhase::MoveInputFinish);
            finishMoveTransfer(request);
        } else {
            recordTrace(request, AnalogTracePhase::OutputTransferFinish);
            finishRequest(
                request,
                MITTENS_ANALOG_STATUS_SUCCESS,
                std::move(request->transferWords));
        }
        return;
    }

    case Phase::Computing:
        --request->remainingComputeCycles;
        if (request->remainingComputeCycles == 0) {
            recordTrace(request, AnalogTracePhase::ComputeFinish);
            finishRequest(
                request, MITTENS_ANALOG_STATUS_SUCCESS);
        }
        return;

    case Phase::Queued:
    case Phase::Complete:
        return;
    }
}

bool AnalogDevice::usesLink(
    const std::shared_ptr<Request>& request) const noexcept
{
    return request->phase == Phase::TransferringInput ||
           request->phase == Phase::TransferringOutput ||
           request->phase == Phase::MovingOutput ||
           request->phase == Phase::MovingInput;
}

std::uint64_t AnalogDevice::remainingLinkBeats(
    const std::shared_ptr<Request>& request) const
{
    if (!usesLink(request) ||
        request->transferredWords > request->linkWordCount) {
        throw std::logic_error(
            "analog request has invalid link progress");
    }
    return linkCyclesForWords(
        request->linkWordCount - request->transferredWords);
}

std::uint32_t AnalogDevice::linkArray(
    const std::shared_ptr<Request>& request) const noexcept
{
    if (request->phase == Phase::MovingInput) {
        return static_cast<std::uint32_t>(request->command.operand1);
    }
    return request->primaryArray;
}

void AnalogDevice::finishInputTransfer(
    const std::shared_ptr<Request>& request)
{
    try {
        if (request->command.operation ==
            MITTENS_ANALOG_OPERATION_SET_MATRIX) {
            std::vector<std::uint32_t> matrixWords;
            if ((request->command.reserved &
                 MITTENS_ANALOG_COMMAND_FLAG_COMPACT_SET_MATRIX) != 0) {
                const std::uint32_t rows =
                    mittens_analog_set_matrix_valid_rows(
                        &request->command);
                const std::uint32_t columns =
                    mittens_analog_set_matrix_valid_columns(
                        &request->command);
                matrixWords.assign(
                    static_cast<std::size_t>(backend_->arrayRows()) *
                        backend_->arrayColumns(),
                    0);
                for (std::uint32_t row = 0; row < rows; ++row) {
                    std::copy_n(
                        request->transferWords.begin() +
                            static_cast<std::size_t>(row) * columns,
                        columns,
                        matrixWords.begin() +
                            static_cast<std::size_t>(row) *
                                backend_->arrayColumns());
                }
            } else {
                matrixWords = request->transferWords;
            }
            const std::vector<float> values =
                decodeFloatWords(matrixWords);
            backend_->setMatrix(request->primaryArray, values);
            ArrayChannel& channel = channels_[request->primaryArray];
            if ((request->command.reserved &
                 MITTENS_ANALOG_COMMAND_FLAG_COMPACT_SET_MATRIX) != 0) {
                channel.validRows =
                    mittens_analog_set_matrix_valid_rows(
                        &request->command);
                channel.validColumns =
                    mittens_analog_set_matrix_valid_columns(
                        &request->command);
            } else {
                channel.validRows = backend_->arrayRows();
                channel.validColumns = backend_->arrayColumns();
            }
        } else {
            const std::vector<float> values =
                decodeFloatWords(request->transferWords);
            backend_->loadVector(request->primaryArray, values);
        }
        finishRequest(request, MITTENS_ANALOG_STATUS_SUCCESS);
    } catch (const std::exception&) {
        finishRequest(request, MITTENS_ANALOG_STATUS_BACKEND_ERROR);
    }
}

void AnalogDevice::finishMoveTransfer(
    const std::shared_ptr<Request>& request)
{
    try {
        backend_->loadVector(
            static_cast<std::uint32_t>(request->command.operand1),
            decodeFloatWords(request->transferWords));
        finishRequest(request, MITTENS_ANALOG_STATUS_SUCCESS);
    } catch (const std::exception&) {
        finishRequest(request, MITTENS_ANALOG_STATUS_BACKEND_ERROR);
    }
}

void AnalogDevice::finishRequest(
    const std::shared_ptr<Request>& request,
    std::uint64_t status,
    std::vector<std::uint32_t> outputWords)
{
    completions_.push_back(AnalogCompletion{
        MittensAnalogResponse{status},
        request->ticket,
        request->command.operation,
        request->primaryArray,
        std::move(outputWords),
    });
    recordCompletedCommand();

    for (const std::uint32_t arrayId : request->arrays) {
        ArrayChannel& channel = channels_[arrayId];
        if (channel.requests.empty() ||
            channel.requests.front().get() != request.get()) {
            throw std::logic_error(
                "analog array queue ordering invariant was violated");
        }
        channel.requests.pop_front();
    }
    request->phase = Phase::Complete;
    recordTrace(request, AnalogTracePhase::Complete);
}

void AnalogDevice::completeImmediately(
    const MittensAnalogCommand& command,
    std::uint64_t ticket,
    std::uint32_t arrayId,
    std::uint64_t status)
{
    recordTrace(
        ticket, command.operation, arrayId, AnalogTracePhase::Submitted);
    completions_.push_back(AnalogCompletion{
        MittensAnalogResponse{status},
        ticket,
        command.operation,
        arrayId,
        {},
    });
    recordCompletedCommand();
    recordTrace(
        ticket, command.operation, arrayId, AnalogTracePhase::Complete);
}

void AnalogDevice::recordSubmittedCommand()
{
    if (submittedCommandCount_ ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("analog submitted-command counter overflow");
    }
    ++submittedCommandCount_;
    maximumOutstandingCommandCount_ = std::max(
        maximumOutstandingCommandCount_,
        submittedCommandCount_ - completedCommandCount_);
}

void AnalogDevice::recordCompletedCommand()
{
    if (completedCommandCount_ == submittedCommandCount_) {
        throw std::logic_error(
            "analog completed-command counter exceeds submissions");
    }
    ++completedCommandCount_;
}

void AnalogDevice::recordTrace(
    const std::shared_ptr<Request>& request,
    AnalogTracePhase phase)
{
    recordTrace(
        request->ticket,
        request->command.operation,
        request->primaryArray,
        phase);
}

void AnalogDevice::recordTrace(
    std::uint64_t ticket,
    std::uint32_t operation,
    std::uint32_t arrayId,
    AnalogTracePhase phase)
{
    if (!traceEnabled_) {
        return;
    }
    traceEvents_.push_back(AnalogTraceEvent{
        ticket,
        operation,
        arrayId,
        phase,
        elapsedCycles_,
    });
}

std::vector<float> AnalogDevice::decodeFloatWords(
    const std::vector<std::uint32_t>& words) const
{
    std::vector<float> values(words.size());
    for (std::size_t index = 0; index < words.size(); ++index) {
        static_assert(sizeof(values[index]) == sizeof(words[index]));
        std::memcpy(&values[index], &words[index], sizeof(values[index]));
    }
    return values;
}

std::vector<std::uint32_t> AnalogDevice::encodeFloatWords(
    const std::vector<float>& values) const
{
    std::vector<std::uint32_t> words(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        static_assert(sizeof(values[index]) == sizeof(words[index]));
        std::memcpy(&words[index], &values[index], sizeof(words[index]));
    }
    return words;
}

} // namespace Mittens
} // namespace SST
