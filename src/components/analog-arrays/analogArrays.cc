#include <sst/core/sst_config.h>
#include "analogArrays.h"
#include "../fixed.h"
#include "../observations.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <limits>

namespace TileComponents {
static_assert(sizeof(float) == 4, "analog-array payloads require 32-bit float storage");

// QEMU register transfers use little-endian float32 bytes regardless of the
// simulator host. Preserve each bit pattern while crossing that boundary.
static void decodeVectorPayload(float* destination, const std::vector<std::uint8_t>& payload) {
    for (std::size_t i = 0; i < payload.size() / 4; ++i) {
        const auto* bytes = payload.data() + i * 4;
        const std::uint32_t bits = std::uint32_t(bytes[0]) |
            (std::uint32_t(bytes[1]) << 8) | (std::uint32_t(bytes[2]) << 16) |
            (std::uint32_t(bytes[3]) << 24);
        std::memcpy(destination + i, &bits, sizeof(bits));
    }
}

static void encodeVectorPayload(std::vector<std::uint8_t>& payload, const float* source) {
    for (std::size_t i = 0; i < payload.size() / 4; ++i) {
        std::uint32_t bits;
        std::memcpy(&bits, source + i, sizeof(bits));
        for (unsigned byte = 0; byte < 4; ++byte)
            payload[i * 4 + byte] = std::uint8_t(bits >> (byte * 8));
    }
}

AnalogArrays::AnalogArrays(SST::ComponentId_t id, SST::Params& p) : Component(id),
    rows_(p.find<unsigned>("array_rows")), cols_(p.find<unsigned>("array_cols")),
    linkWidth_(0),
    inflightBytes_(p.find<unsigned>("array_inflight_bytes")),
    programCost_(p.find<std::uint64_t>("cost_per_array_program_cycles", 0)),
    execCost_(p.find<std::uint64_t>("cost_per_mvm_cycles")),
    independent_(p.find<std::string>("array_link_duplex") == "independent"),
    pipeline_(p.find<bool>("array_pipeline_enabled", true)),
    initialProgram_(p.find<std::string>("array_program_delay_scope", "per_command") == "initial_full_array"),
    arrays_(p.find<unsigned>("arrays_per_tile")) {
    cycleProfile_.open(getName());
    // Both are required even though zero is a valid explicitly configured
    // execution cost. A stale parameter name must not silently select zero.
    if (!p.contains("cost_per_mvm_cycles") || !p.contains("arrays_per_tile"))
        fatal(CALL_INFO, -1, "cost_per_mvm_cycles and arrays_per_tile are required\n");
    const auto vlen = p.find<std::uint64_t>("riscv_vector_length_bits", 256);
    if (vlen != 128 && vlen != 256 && vlen != 512 && vlen != 1024)
        fatal(CALL_INFO, -1, "riscv_vector_length_bits must be 128, 256, 512 or 1024\n");
    linkWidth_ = static_cast<unsigned>(vlen / 8);
    if (p.find<std::uint64_t>("array_link_width", linkWidth_) != linkWidth_)
        fatal(CALL_INFO, -1, "array_link_width must equal riscv_vector_length_bits / 8 (%u bytes per cycle)\n", linkWidth_);
    const auto duplex = p.find<std::string>("array_link_duplex");
    const auto scope = p.find<std::string>("array_program_delay_scope", "per_command");
    if (scope != "per_command" && scope != "initial_full_array")
        fatal(CALL_INFO, -1, "array_program_delay_scope must be per_command or initial_full_array\n");
    if (!rows_ || !cols_ || arrays_.empty() || !linkWidth_ || !inflightBytes_ ||
        std::uint64_t(rows_) * cols_ > std::numeric_limits<std::uint32_t>::max() / 4 ||
        (duplex != "shared" && duplex != "independent"))
        fatal(CALL_INFO, -1, "invalid analog-array configuration\n");
    for (auto& a : arrays_) {
        a.weights.resize(std::size_t(rows_) * cols_);
        a.input.resize(cols_); a.output.resize(rows_);
        a.weightInitialized.resize(a.weights.size()); a.inputInitialized.resize(cols_);
    }
    clock_ = registerClock(Clock, new SST::Clock::Handler<AnalogArrays, &AnalogArrays::tick>(this));
    commands_ = configureLink("commands", Clock,
        new SST::Event::Handler<AnalogArrays, &AnalogArrays::command>(this));
    if (!commands_) fatal(CALL_INFO, -1, "arrays require a command connection\n");
    if (const auto dir = CycleProfile::traceDirectory(); !dir.empty()) {
        const auto stem = std::string(dir) + "/" + getName();
        // Preserve existing single-tile filenames while isolating named tiles.
        const auto detail = std::string(dir) + "/" + (getName() == "arrays" ? "array" : getName());
        trace_.open(stem + ".csv");
        if (!trace_) fatal(CALL_INFO, -1, "cannot open array trace\n");
        trace_ << "event,cycle,array,token,operation,bytes,buffered,element_offset,element_count\n";
        transfers_.open(detail + "-requests.csv");
        if (!transfers_) fatal(CALL_INFO, -1, "cannot open array transfer trace\n");
        transfers_ << "event,cycle,array,token,operation,bytes,buffered,element_offset,element_count,chunk_offset\n";
        waits_.open(detail + "-waits.csv");
        if (!waits_) fatal(CALL_INFO, -1, "cannot open array wait trace\n");
        waits_ << "cycle,array,token,operation,buffered,remaining\n";
        programmingTrace_.open(detail + "-programming.csv");
        if (!programmingTrace_) fatal(CALL_INFO, -1, "cannot open array programming trace\n");
        programmingTrace_ << "event,cycle,array,token,scope,element_offset,element_count,initialized_weights,total_weights,delay_cycles\n";
        if (std::getenv("TILE_COMPONENT_TRACE_START_TASK") ||
            std::getenv("TILE_COMPONENT_PROGRAM_PROOF")) {
            initialProgramTrace_.open(detail + "-initial-program.csv");
            if (!initialProgramTrace_) fatal(CALL_INFO, -1, "cannot open initial-program proof trace\n");
            initialProgramTrace_ << "array,first_program_start_cycle,cycle,final_token,scope,program_commands,delivered_bytes,initialized_weights,total_weights,delay_cycles,delay_charges,initial_completions,weights_fnv1a64\n";
        }
    }
}

void AnalogArrays::reply(const ArrayCommand& request, CommandStatus status) {
    auto* answer = new ArrayCommand;
    answer->operation = request.operation; answer->array = request.array;
    answer->token = request.token;
    answer->status = status; answer->cycle = getCurrentSimTime(clock_);
    answer->elementOffset = request.elementOffset; answer->elementCount = request.elementCount;
    answer->deferred = request.deferred;
    if (request.operation == Operation::Store && status == CommandStatus::Complete)
        answer->payload = request.payload;
    commands_->send(answer);
}

void AnalogArrays::initializeProgramProjection(Array& a) {
    if (!a.projectedWeightInitialized.empty()) return;
    a.projectedWeightInitialized = a.weightInitialized;
    a.projectedInitializedWeights = a.initializedWeights;
    const auto reserve = [&](const ArrayCommand* request) {
        if (request && request->operation == Operation::Program && request->elementCount)
            initializeRange(a.projectedWeightInitialized, a.projectedInitializedWeights,
                            request->elementOffset, request->elementCount);
    };
    reserve(a.active);
    for (const auto* request : a.queue) reserve(request);
}

bool AnalogArrays::projectedStoreResult(const Array& a, std::uint64_t& token) {
    // Only existing result slots are guaranteed, including a computing slot.
    // A queued Execute that has not reserved a result remains a blocking case.
    // Project stores in their actual FIFO order: they can pass a blocked
    // Execute, but cannot pass another Store. At most two row bitmaps are
    // copied here; the dense weight matrix is never involved.
    std::vector<std::vector<bool>> delivered;
    std::vector<std::size_t> counts;
    for (const auto& result : a.results) {
        delivered.push_back(result.delivered);
        counts.push_back(result.deliveredRows);
    }
    std::size_t current = 0;
    const auto reserve = [&](const ArrayCommand* request) {
        if (!request || request->operation != Operation::Store || !request->elementCount)
            return true;
        if (current >= a.results.size()) return false;
        const auto reserved = storeResults_.find(request->token);
        if (reserved != storeResults_.end() && reserved->second != a.results[current].token)
            fatal(CALL_INFO, -1, "guaranteed Store changed its projected result\n");
        if (initializeRange(delivered[current], counts[current],
                            request->elementOffset, request->elementCount))
            ++current;
        return true;
    };
    if (!reserve(a.active)) return false;
    for (const auto* request : a.queue) if (!reserve(request)) return false;
    if (current >= a.results.size()) return false;
    token = a.results[current].token;
    return true;
}

bool AnalogArrays::projectedNonpipelineOutput(const Array& a) const {
    bool valid = a.computed;
    const auto project = [&](const ArrayCommand* request, bool started) {
        if (!request) return;
        if (request->operation == Operation::Execute) {
            // Active Execute already passed readiness validation. A queued
            // Execute may still fault; do not speculate about its output.
            valid = started;
        } else if (request->elementCount &&
                   (request->operation == Operation::Program || request->operation == Operation::Load)) {
            valid = false;
        }
    };
    project(a.active, true);
    for (const auto* request : a.queue) project(request, false);
    return valid;
}

void AnalogArrays::command(SST::Event* event) {
    auto* request = dynamic_cast<ArrayCommand*>(event);
    if (!request) fatal(CALL_INFO, -1, "unknown array event\n");
    const auto op = request->operation;
    const std::uint64_t elements = op == Operation::Program ? std::uint64_t(rows_) * cols_ :
                                   op == Operation::Load ? cols_ : op == Operation::Store ? rows_ : 0;
    const std::uint64_t bytes = std::uint64_t(request->elementCount) * 4;
    const bool input = op == Operation::Program || op == Operation::Load;
    if (request->status != CommandStatus::Request || request->array >= arrays_.size() ||
        op > Operation::Store || tokens_.count(request->token) ||
        (request->deferred && op == Operation::Execute) ||
        request->elementOffset > elements || request->elementCount > elements - request->elementOffset ||
        bytes > std::numeric_limits<unsigned>::max() ||
        (input ? request->payload.size() != bytes : !request->payload.empty())) {
        ++errors_; reply(*request, CommandStatus::Error); delete request; return;
    }
    auto& a = arrays_[request->array];
    // A fresh array is the only epoch in initial_full_array mode. Close it
    // as soon as all weights arrive, including while its delay is outstanding.
    if (initialProgram_ && a.initialProgramClosed && op == Operation::Program && request->elementCount) {
        ++errors_; reply(*request, CommandStatus::Error); delete request; return;
    }
    // Reprogramming requires drained jobs. Reject before accepting instead of
    // blocking the single CPU before it can issue the Stores needed to drain.
    // Empty Program commands keep their existing no-op semantics.
    if (pipeline_ && op == Operation::Program && request->elementCount &&
        (!a.results.empty() || std::any_of(a.queue.begin(), a.queue.end(),
            [](const auto* queued) { return queued->operation == Operation::Execute; }))) {
        ++errors_; reply(*request, CommandStatus::Error); delete request; return;
    }
    if (a.queue.size() + (a.active != nullptr) + (a.compute != nullptr) >= ArrayQueueEntries) {
        ++rejected_; reply(*request, CommandStatus::Busy); delete request; return;
    }
    if (initialProgram_ && op == Operation::Program && request->elementCount) {
        if (request->deferred) initializeProgramProjection(a);
        if (!a.projectedWeightInitialized.empty()) {
            // A guaranteed command must not sit behind an admitted range that
            // will close the epoch. Legacy clients keep their old late-error
            // behavior, but their accepted ranges still affect projection.
            if (request->deferred && a.projectedInitializedWeights == a.weights.size()) {
                ++errors_; reply(*request, CommandStatus::Error); delete request; return;
            }
            initializeRange(a.projectedWeightInitialized, a.projectedInitializedWeights,
                            request->elementOffset, request->elementCount);
        }
    }
    bool guaranteed = request->deferred;
    if (guaranteed && op == Operation::Store && request->elementCount) {
        if (pipeline_) {
            std::uint64_t resultToken;
            guaranteed = projectedStoreResult(a, resultToken);
            if (guaranteed) storeResults_.emplace(request->token, resultToken);
        } else {
            guaranteed = projectedNonpipelineOutput(a);
        }
    }
    if (guaranteed) guaranteed_.insert(request->token);
    tokens_.insert(request->token); a.queue.push_back(request);
    ++accepted_; reply(*request, guaranteed ? CommandStatus::Guaranteed : CommandStatus::Accepted);
}

void AnalogArrays::start(unsigned index, std::uint64_t now) {
    auto& a = arrays_[index];
    if (a.active || a.queue.empty()) return;
    a.active = a.queue.front(); a.queue.pop_front();
    const auto op = a.active->operation;
    const bool emptyTransfer = op != Operation::Execute && !a.active->elementCount;
    if ((initialProgram_ && a.initialProgramClosed && op == Operation::Program && !emptyTransfer) ||
        (op == Operation::Execute && (!a.programmed || !a.loaded)) ||
        (op == Operation::Store && !emptyTransfer && !a.computed)) {
        complete(index, CommandStatus::Error); return;
    }
    beginTransfer(index, now);
}

void AnalogArrays::beginTransfer(unsigned index, std::uint64_t now) {
    auto& a = arrays_[index];
    const auto op = a.active->operation;
    const bool emptyTransfer = op != Operation::Execute && !a.active->elementCount;
    // Transfers are eligible immediately. Program's device delay begins only
    // after its bytes arrive; Execute's delay begins at command start.
    const auto cost = op == Operation::Execute ? execCost_ : 0;
    if (cost > UINT64_MAX - now) fatal(CALL_INFO, -1, "array deadline overflow\n");
    a.ready = now + cost;
    a.total = a.active->elementCount * 4;
    a.issued = a.complete = a.buffered = 0;
    a.programming = false;
    a.inputCaptured = false;
    a.programDelayCharged = false;
    if (op == Operation::Program && !emptyTransfer) a.programmed = false;
    if (initialProgramTrace_ && op == Operation::Program && !emptyTransfer &&
        !a.observedProgramCommands)
        a.observedProgramFirstCycle = now;
    a.transfer.assign(a.total, 0);
    if (op == Operation::Store && a.total) {
        const auto reserved = storeResults_.find(a.active->token);
        if (reserved != storeResults_.end() &&
            (a.results.empty() || a.results.front().token != reserved->second))
            fatal(CALL_INFO, -1, "guaranteed Store started against the wrong result\n");
        const auto& output = pipeline_ ? a.results.front().values : a.output;
        encodeVectorPayload(a.transfer, output.data() + a.active->elementOffset);
    }
    if (!emptyTransfer && (op == Operation::Load || op == Operation::Program)) a.computed = false;
    record("start", index, now);
}

void AnalogArrays::complete(unsigned index, CommandStatus status) {
    auto& a = arrays_[index];
    if (status != CommandStatus::Complete && guaranteed_.count(a.active->token))
        fatal(CALL_INFO, -1, "guaranteed analog command failed after admission\n");
    if (!a.chunks.empty() || a.buffered) fatal(CALL_INFO, -1, "array completed with live transfers\n");
    if (status == CommandStatus::Complete) ++completed_; else ++errors_;
    record(status == CommandStatus::Complete ? "complete" : "error", index, getCurrentSimTime(clock_));
    if (status == CommandStatus::Complete && a.active->operation == Operation::Store) {
        if (pipeline_ && a.active->elementCount) {
            auto& result = a.results.front();
            if (initializeRange(result.delivered, result.deliveredRows,
                                a.active->elementOffset, a.active->elementCount))
                a.results.pop_front();
        }
        a.active->payload = std::move(a.transfer);
    }
    reply(*a.active, status); tokens_.erase(a.active->token);
    guaranteed_.erase(a.active->token); storeResults_.erase(a.active->token);
    delete a.active; a.active = nullptr; a.transfer.clear();
}

void AnalogArrays::rejectQueued(unsigned index, ArrayCommand* request, std::uint64_t now) {
    if (guaranteed_.count(request->token))
        fatal(CALL_INFO, -1, "guaranteed analog command rejected after admission\n");
    ++errors_;
    recordCommand("error", index, *request, now);
    reply(*request, CommandStatus::Error); tokens_.erase(request->token);
    delete request;
}

void AnalogArrays::startCompute(unsigned index, ArrayCommand* request, std::uint64_t now) {
    auto& a = arrays_[index];
    if (a.compute || a.results.size() >= 2)
        fatal(CALL_INFO, -1, "pipeline compute started without a free compute/output slot\n");
    if (execCost_ > UINT64_MAX - now) fatal(CALL_INFO, -1, "array deadline overflow\n");
    // Program cannot run until every pending result has drained, so weights
    // remain stable. Inputs are captured before allowing later Loads to run.
    a.computeInput = a.input;
    a.results.push_back(Array::Result{std::vector<float>(rows_), std::vector<bool>(rows_)});
    a.results.back().token = request->token;
    a.compute = request;
    a.computeReady = now + execCost_;
    peakPendingJobs_ = std::max<unsigned>(peakPendingJobs_, a.results.size());
    recordCommand("start", index, *request, now);
    reply(*request, CommandStatus::Started);
}

void AnalogArrays::progressCompute(unsigned index, std::uint64_t now) {
    auto& a = arrays_[index];
    if (!a.compute || now < a.computeReady) return;
    auto& result = a.results.back();
    if (result.ready) fatal(CALL_INFO, -1, "pipeline compute lost its output reservation\n");
    for (unsigned row = 0; row < rows_; ++row) {
        double sum = 0;
        for (unsigned col = 0; col < cols_; ++col)
            sum += double(a.weights[std::size_t(row) * cols_ + col]) * a.computeInput[col];
        result.values[row] = static_cast<float>(sum);
    }
    result.ready = true;
    ++mvms_; ++completed_;
    recordCommand("complete", index, *a.compute, now);
    // Started commits the command: all user errors were checked beforehand.
    // Completion therefore always succeeds, including a zero-cycle execution.
    reply(*a.compute, CommandStatus::Complete); tokens_.erase(a.compute->token);
    delete a.compute; a.compute = nullptr; a.computeInput.clear();
}

void AnalogArrays::startPipeline(unsigned index, std::uint64_t now) {
    auto& a = arrays_[index];
    // At most four accepted commands exist. The loop can start a compute and
    // its following transfer together, without launching overlapping computes.
    for (unsigned attempt = 0; attempt < ArrayQueueEntries && !a.queue.empty(); ++attempt) {
        auto selected = a.queue.begin();
        const bool execute = (*selected)->operation == Operation::Execute;
        if (a.active) {
            // A preceding Load/Program must finish before the input snapshot.
            // A Store owns only an older output and may overlap a new compute.
            if (a.active->operation != Operation::Store || !execute) return;
            if (a.compute || a.results.size() >= 2) return;
        } else if (execute && (a.compute || a.results.size() >= 2)) {
            // Preserve Load/Execute ordering. Only Stores may bypass a blocked
            // Execute, so queued clients can release the oldest output slot.
            selected = std::find_if(std::next(selected), a.queue.end(),
                [](const auto* request) { return request->operation == Operation::Store; });
            if (selected == a.queue.end()) return;
        }
        auto* request = *selected;
        if (initialProgram_ && a.initialProgramClosed && request->operation == Operation::Program && request->elementCount) {
            a.queue.erase(selected); rejectQueued(index, request, now); continue;
        }
        if (request->operation == Operation::Execute) {
            a.queue.erase(selected);
            if (!a.programmed || !a.loaded) {
                rejectQueued(index, request, now); continue;
            }
            startCompute(index, request, now);
            continue;
        }
        if (a.active) return;
        if (request->operation == Operation::Store && request->elementCount) {
            if (a.results.empty()) {
                a.queue.erase(selected); rejectQueued(index, request, now); continue;
            }
            if (!a.results.front().ready) return;
        }
        if (request->operation == Operation::Program && request->elementCount && !a.results.empty()) {
            // Admission rejects this state; retain a defensive check before
            // touching weights if another command client violates ordering.
            a.queue.erase(selected); rejectQueued(index, request, now); continue;
        }
        a.active = request; a.queue.erase(selected);
        beginTransfer(index, now);
        return;
    }
}

void AnalogArrays::captureInput(unsigned index) {
    auto& a = arrays_[index];
    if (a.active->deferred && !a.inputCaptured &&
        (a.active->operation == Operation::Program || a.active->operation == Operation::Load)) {
        if (a.complete != a.total || !a.chunks.empty() || a.buffered)
            fatal(CALL_INFO, -1, "analog input captured before timed byte consumption\n");
        a.inputCaptured = true;
        reply(*a.active, CommandStatus::Captured);
    }
}

void AnalogArrays::progress(unsigned index, std::uint64_t now) {
    auto& a = arrays_[index];
    if (!a.active || now < a.ready) return;
    const auto op = a.active->operation;
    if (op == Operation::Execute) {
        for (unsigned row = 0; row < rows_; ++row) {
            double sum = 0;
            for (unsigned col = 0; col < cols_; ++col)
                sum += double(a.weights[std::size_t(row) * cols_ + col]) * a.input[col];
            a.output[row] = static_cast<float>(sum);
        }
        a.computed = true; ++mvms_; complete(index); return;
    }
    if (!a.total) { captureInput(index); complete(index); return; }
    const bool write = op == Operation::Store;
    for (auto it = a.chunks.begin(); it != a.chunks.end();) {
        auto& c = it->second;
        if (c.linked != c.bytes || c.linkReady > now) { ++it; continue; }
        if (!write) std::copy(c.data.begin(), c.data.end(), a.transfer.begin() + c.offset);
        a.complete += c.bytes; a.buffered -= c.bytes;
        recordTransfer("release", index, c.offset, c.bytes);
        it = a.chunks.erase(it);
    }
    while (a.issued < a.total && a.buffered < inflightBytes_) {
        const unsigned bytes = std::min(a.total - a.issued, inflightBytes_ - a.buffered);
        Chunk c{a.issued, bytes, 0, 0, {}};
        const auto& source = write ? a.transfer : a.active->payload;
        c.data.assign(source.begin() + a.issued, source.begin() + a.issued + bytes);
        a.chunks.emplace(a.issued, std::move(c));
        recordTransfer("allocate", index, a.issued, bytes);
        a.issued += bytes; a.buffered += bytes;
        peakBuffered_ = std::max(peakBuffered_, a.buffered);
    }
    if (RecordComponentObservations && a.issued < a.total && a.buffered == inflightBytes_ && waits_)
        waits_ << now << ',' << index << ',' << a.active->token << ','
               << unsigned(op) << ',' << a.buffered << ',' << a.total - a.issued << '\n';
    if (a.complete == a.total) {
        captureInput(index);
        if (op == Operation::Program) {
            if (!a.programming) {
                // Coverage describes received weights. The command still owns
                // its payload and programmed readiness remains false until its
                // selected delay finishes and the payload is committed below.
                const bool full = initializeRange(a.weightInitialized, a.initializedWeights,
                                                  a.active->elementOffset, a.total / 4);
                a.programDelayCharged = !initialProgram_ || full;
                const auto delay = a.programDelayCharged ? programCost_ : 0;
                if (delay > UINT64_MAX - now || delay > UINT64_MAX - programDelayCycles_)
                    fatal(CALL_INFO, -1, "array programming deadline overflow\n");
                a.ready = now + delay;
                a.programming = true;
                recordProgramming("delivered", index, now);
                if (a.programDelayCharged) {
                    if (initialProgram_) a.initialProgramClosed = true;
                    ++programDelayCharges_; programDelayCycles_ += delay;
                    recordProgramming("delay_start", index, now);
                }
            }
            if (now < a.ready) return;
            decodeVectorPayload(a.weights.data() + a.active->elementOffset, a.transfer);
            a.programmed = a.initializedWeights == a.weights.size();
            if (initialProgramTrace_) {
                ++a.observedProgramCommands;
                a.observedProgramBytes += a.total;
            }
            if (a.programDelayCharged) {
                if (initialProgram_) ++initialProgramCompletions_;
                recordProgramming("delay_complete", index, now);
            }
            if (initialProgram_ && a.programDelayCharged) recordInitialProgram(index, now);
        } else if (op == Operation::Load) {
            decodeVectorPayload(a.input.data() + a.active->elementOffset, a.transfer);
            a.loaded = initializeRange(a.inputInitialized, a.initializedInputs,
                                      a.active->elementOffset, a.total / 4);
        }
        complete(index);
    }
}

bool AnalogArrays::initializeRange(std::vector<bool>& coverage, std::size_t& initialized,
                                  std::size_t offset, std::size_t count) {
    for (std::size_t i = offset; i < offset + count; ++i) {
        if (!coverage[i]) { coverage[i] = true; ++initialized; }
    }
    return initialized == coverage.size();
}

void AnalogArrays::link(std::uint64_t now) {
    unsigned readBudget = linkWidth_, writeBudget = independent_ ? linkWidth_ : 0;
    for (unsigned n = 0; n < arrays_.size(); ++n) {
        const unsigned index = (cursor_ + n) % arrays_.size();
        auto& a = arrays_[index];
        if (!a.active || now < a.ready) continue;
        const bool write = a.active->operation == Operation::Store;
        auto& budget = independent_ && write ? writeBudget : readBudget;
        for (auto& [offset, c] : a.chunks) {
            if (!budget) break;
            if (c.linked == c.bytes) continue;
            const auto bytes = std::min(budget, c.bytes - c.linked);
            c.linked += bytes; budget -= bytes; c.linkReady = now + 1;
            (write ? writeBytes_ : readBytes_) += bytes;
            record(write ? "link_write" : "link_read", index, now, bytes);
        }
    }
    cursor_ = (cursor_ + 1) % arrays_.size();
}

bool AnalogArrays::tick(SST::Cycle_t) {
    const auto now = getCurrentSimTime(clock_);
    unsigned active = 0;
    for (unsigned i = 0; i < arrays_.size(); ++i) {
        auto& a = arrays_[i];
        if (!pipeline_) {
            start(i, now); progress(i, now); active += a.active != nullptr;
            continue;
        }
        progressCompute(i, now);
        progress(i, now);
        if (!a.queue.empty() && a.queue.front()->operation == Operation::Execute && a.results.size() == 2)
            ++outputBackpressure_;
        const bool transferWasActive = a.active != nullptr;
        startPipeline(i, now);
        if (!transferWasActive) progress(i, now);
        progressCompute(i, now);
        if (a.compute && a.active) {
            computeLoadOverlap_ += a.active->operation == Operation::Load;
            computeStoreOverlap_ += a.active->operation == Operation::Store;
        }
        active += a.active != nullptr || a.compute != nullptr;
    }
    peakActive_ = std::max(peakActive_, active);
    link(now);
    if (cycleProfile_) for (unsigned i=0;i<arrays_.size();++i) {
        const auto& a=arrays_[i];
        cycleProfile_.record(now,"state","command_queue",i,a.queue.size());
        cycleProfile_.record(now,"state","transfer_active",i,a.active!=nullptr, a.active ? a.active->token : 0);
        cycleProfile_.record(now,"state","transfer_buffer_bytes",i,a.buffered);
        cycleProfile_.record(now,"state","transfer_chunks",i,a.chunks.size());
        cycleProfile_.record(now,"state","compute_active",i,a.compute!=nullptr, a.compute ? a.compute->token : 0);
        cycleProfile_.record(now,"state","input_elements_initialized",i,a.initializedInputs);
        cycleProfile_.record(now,"state","result_slots",i,a.results.size());
        cycleProfile_.record(now,"state","ready_result_slots",i,
            std::count_if(a.results.begin(),a.results.end(),[](const auto& result){return result.ready;}));
    }
    return false;
}

void AnalogArrays::record(const char* event, unsigned i, std::uint64_t now, unsigned bytes) {
    recordCommand(event, i, *arrays_[i].active, now, bytes);
}

void AnalogArrays::recordCommand(const char* event, unsigned i, const ArrayCommand& command,
                                 std::uint64_t now, unsigned bytes) {
    if (!RecordComponentObservations) return;
    const auto& a = arrays_[i];
    if (trace_) trace_ << event << ',' << now << ',' << i << ',' << command.token << ','
        << unsigned(command.operation) << ',' << bytes << ',' << a.buffered << ','
        << command.elementOffset << ',' << command.elementCount << '\n';
}

void AnalogArrays::finish() {
    cycleProfile_.flush();
    if (!tokens_.empty()) fatal(CALL_INFO, -1, "arrays finished with live commands\n");
    if (trace_) trace_.flush();
    if (transfers_) transfers_.flush();
    if (waits_) waits_.flush();
    if (programmingTrace_) programmingTrace_.flush();
    if (initialProgramTrace_) initialProgramTrace_.flush();
    std::cout << "ARRAY_STATS {\"component\":" << std::quoted(getName())
        << ",\"accepted\":" << accepted_ << ",\"completed\":" << completed_
        << ",\"busy\":" << rejected_ << ",\"errors\":" << errors_ << ",\"mvms\":" << mvms_
        << ",\"link_read_bytes\":" << readBytes_ << ",\"link_write_bytes\":" << writeBytes_
        << ",\"peak_buffered_bytes_per_array\":" << peakBuffered_
        << ",\"peak_active_arrays\":" << peakActive_
        << ",\"pipeline_enabled\":" << (pipeline_ ? "true" : "false")
        << ",\"peak_pending_jobs_per_array\":" << peakPendingJobs_
        << ",\"compute_load_overlap_cycles\":" << computeLoadOverlap_
        << ",\"compute_store_overlap_cycles\":" << computeStoreOverlap_
        << ",\"output_backpressure_cycles\":" << outputBackpressure_
        << ",\"program_delay_scope\":\"" << (initialProgram_ ? "initial_full_array" : "per_command") << '"'
        << ",\"program_delay_charges\":" << programDelayCharges_
        << ",\"program_delay_cycles\":" << programDelayCycles_
        << ",\"initial_full_array_completions\":" << initialProgramCompletions_ << "}\n";
}

void AnalogArrays::recordInitialProgram(unsigned index, std::uint64_t now) {
    if (!initialProgramTrace_) return;
    const auto& a = arrays_[index];
    // Hash the committed resident matrix, not the incoming command payload.
    // Its byte order is explicitly LE float32 on every simulator host.
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const auto value : a.weights) {
        std::uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        for (unsigned byte = 0; byte < 4; ++byte) {
            hash ^= std::uint8_t(bits >> (8 * byte));
            hash *= UINT64_C(1099511628211);
        }
    }
    initialProgramTrace_ << index << ',' << a.observedProgramFirstCycle << ',' << now << ','
        << a.active->token << ",initial_full_array," << a.observedProgramCommands << ','
        << a.observedProgramBytes << ',' << a.initializedWeights << ',' << a.weights.size()
        << ',' << programCost_ << ",1,1," << hash << '\n';
}

void AnalogArrays::recordProgramming(const char* event, unsigned index, std::uint64_t now) {
    if (!RecordComponentObservations || !programmingTrace_) return;
    const auto& a = arrays_[index];
    programmingTrace_ << event << ',' << now << ',' << index << ',' << a.active->token << ','
        << (initialProgram_ ? "initial_full_array" : "per_command") << ','
        << a.active->elementOffset << ',' << a.active->elementCount << ','
        << a.initializedWeights << ',' << a.weights.size() << ',' << programCost_ << '\n';
}

void AnalogArrays::recordTransfer(const char* event, unsigned index, unsigned offset, unsigned bytes) {
    if (!RecordComponentObservations) return;
    const auto& a = arrays_[index];
    if (transfers_) transfers_ << event << ',' << getCurrentSimTime(clock_) << ',' << index
        << ',' << a.active->token << ',' << unsigned(a.active->operation)
        << ',' << bytes << ',' << a.buffered << ',' << a.active->elementOffset << ','
        << a.active->elementCount << ',' << offset << '\n';
}
}
