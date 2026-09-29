#pragma once
#include "../cycleProfile.h"
#include <sst/core/component.h>
#include <sst/core/link.h>
#include "commands.h"
#include <deque>
#include <fstream>
#include <map>
#include <set>

namespace TileComponents {
class AnalogArrays final : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(AnalogArrays, "tilecomponents", "AnalogArrays",
        SST_ELI_ELEMENT_VERSION(1,0,0), "Independent float32 MVM arrays", COMPONENT_CATEGORY_PROCESSOR)
    SST_ELI_DOCUMENT_PARAMS(
        {"cost_per_array_program_cycles", "Programming cycles charged according to array_program_delay_scope", "0"},
        {"array_program_delay_scope", "per_command or initial_full_array (one initial programming epoch)", "per_command"},
        {"cost_per_mvm_cycles", "Array MVM execution latency in cycles", NULL},
        {"arrays_per_tile", "Independent arrays per tile", NULL},
        {"array_rows", "Outputs per array", NULL},
        {"array_cols", "Inputs per array", NULL},
        {"riscv_vector_length_bits", "Architectural VLEN in bits per register: 128, 256, 512 or 1024", "256"},
        {"array_link_width", "Optional check of derived aggregate bytes/cycle; must equal VLEN/8", "VLEN/8"},
        {"array_inflight_bytes", "Bytes in the array-link transfer pipeline per array", NULL},
        {"array_pipeline_enabled", "Overlap one compute with register transfers using two output slots", "true"},
        {"array_link_duplex", "shared or independent input/output links", NULL})
    SST_ELI_DOCUMENT_PORTS({"commands", "Register transfers, MVM commands and completion responses", {"TileComponents.ArrayCommand"}})
    AnalogArrays(SST::ComponentId_t, SST::Params&);
    void finish() override;
private:
    struct Chunk {
        unsigned offset, bytes, linked = 0;
        std::uint64_t linkReady = 0;
        std::vector<std::uint8_t> data;
    };
    struct Array {
        struct Result {
            std::vector<float> values;
            std::vector<bool> delivered;
            std::size_t deliveredRows = 0;
            bool ready = false;
            std::uint64_t token = 0;
        };
        std::deque<ArrayCommand*> queue;
        ArrayCommand* active = nullptr;
        ArrayCommand* compute = nullptr;
        std::uint64_t computeReady = 0;
        std::vector<float> computeInput;
        std::deque<Result> results;
        std::uint64_t ready = 0;
        unsigned total = 0, issued = 0, complete = 0, buffered = 0;
        bool programming = false;
        bool inputCaptured = false;
        bool programDelayCharged = false, initialProgramClosed = false;
        bool programmed = false, loaded = false, computed = false;
        std::vector<float> weights, input, output;
        std::vector<bool> weightInitialized, inputInitialized;
        std::size_t initializedWeights = 0, initializedInputs = 0;
        // Allocated lazily only for deferred initial-epoch programming. The
        // bitmap is updated per admitted range, never copied per command.
        std::vector<bool> projectedWeightInitialized;
        std::size_t projectedInitializedWeights = 0;
        // Passive initial-program proof, populated only when requested.
        std::uint64_t observedProgramCommands = 0, observedProgramBytes = 0;
        std::uint64_t observedProgramFirstCycle = 0;
        std::vector<std::uint8_t> transfer;
        std::map<unsigned, Chunk> chunks;
    };
    bool tick(SST::Cycle_t);
    void command(SST::Event*);
    void start(unsigned, std::uint64_t);
    void beginTransfer(unsigned, std::uint64_t);
    void startPipeline(unsigned, std::uint64_t);
    void startCompute(unsigned, ArrayCommand*, std::uint64_t);
    void progressCompute(unsigned, std::uint64_t);
    void rejectQueued(unsigned, ArrayCommand*, std::uint64_t);
    void progress(unsigned, std::uint64_t);
    void link(std::uint64_t);
    void complete(unsigned, CommandStatus = CommandStatus::Complete);
    void reply(const ArrayCommand&, CommandStatus);
    void captureInput(unsigned);
    void initializeProgramProjection(Array&);
    bool projectedStoreResult(const Array&, std::uint64_t&);
    bool projectedNonpipelineOutput(const Array&) const;
    static bool initializeRange(std::vector<bool>&, std::size_t&, std::size_t, std::size_t);
    void record(const char*, unsigned, std::uint64_t, unsigned = 0);
    void recordCommand(const char*, unsigned, const ArrayCommand&, std::uint64_t, unsigned = 0);
    void recordTransfer(const char*, unsigned, unsigned, unsigned);
    void recordProgramming(const char*, unsigned, std::uint64_t);
    void recordInitialProgram(unsigned, std::uint64_t);
    SST::Link* commands_;
    SST::TimeConverter clock_;
    unsigned rows_, cols_, linkWidth_, inflightBytes_, cursor_ = 0;
    std::uint64_t programCost_, execCost_;
    bool independent_, pipeline_, initialProgram_;
    std::vector<Array> arrays_;
    std::set<std::uint64_t> tokens_;
    std::set<std::uint64_t> guaranteed_;
    std::map<std::uint64_t, std::uint64_t> storeResults_;
    std::uint64_t accepted_ = 0, completed_ = 0, rejected_ = 0, errors_ = 0;
    std::uint64_t readBytes_ = 0, writeBytes_ = 0, mvms_ = 0;
    unsigned peakBuffered_ = 0, peakActive_ = 0;
    unsigned peakPendingJobs_ = 0;
    std::uint64_t computeLoadOverlap_ = 0, computeStoreOverlap_ = 0, outputBackpressure_ = 0;
    std::uint64_t programDelayCharges_ = 0, programDelayCycles_ = 0, initialProgramCompletions_ = 0;
    std::ofstream trace_;
    CycleProfile cycleProfile_;
    std::ofstream transfers_;
    std::ofstream waits_;
    std::ofstream programmingTrace_;
    std::ofstream initialProgramTrace_;
};
}
