#pragma once

#include <sst/core/component.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/link.h>
#include "cpuExecutionLedger.h"
#include "qemuProcess.h"
#include "scratchpadBootImage.h"
#include "externalCommit.h"
#include "instructionCache.h"
#include "sharedSyncMemoryBridge.h"
#include "../analog-arrays/commands.h"
#include "../mordred/networkCommand.h"
#include <fstream>
#include <map>
#include <memory>

namespace TileComponents {

// A single RV64 core. QEMU supplies architectural execution;
// An instruction cache fronts StandardMem; data accesses remain uncached.
class RiscvQemu final : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(RiscvQemu, "tilecomponents", "RiscvQemu",
        SST_ELI_ELEMENT_VERSION(1,0,0), "RISC-V QEMU core connected to banked SPM",
        COMPONENT_CATEGORY_PROCESSOR)
    SST_ELI_DOCUMENT_PARAMS(
        {"qemu", "Path to the source_new shared-SPM QEMU executable", ""},
        {"elf", "Bare-metal RV64 ELF linked entirely into SPM", ""},
        {"memory_file", "Shared SPM backing file, also used by the controller", ""},
        {"spm_capacity_bytes", "SPM size in bytes", "2097152"},
        {"spm_request_bytes", "SPM transport fragment size; ordering uses exact byte ranges", "32"},
        {"instruction_budget", "Instructions per QEMU synchronization grant", "256"},
        {"issue_width", "Maximum scalar instructions issued per CPU cycle", "1"},
        {"load_store_queue_depth", "Outstanding RVV memory beats, 1 retains the blocking baseline; 1..64", "1"},
        {"scalar_load_store_queue_depth", "Outstanding scalar memory operations; zero disables; 0..64", "8"},
        {"analog_command_queue_depth", "Asynchronous vector analog commands; zero disables; 0..16", "4"},
        {"analog_command_queue_bytes", "Maximum active analog payload bytes; 1024..16384", "16384"},
        {"instruction_cache_enabled", "Enable the scratchpad-backed instruction cache", "true"},
        {"instruction_cache_bytes", "Instruction cache capacity", "8192"},
        {"instruction_cache_line_bytes", "Instruction cache line size", "64"},
        {"instruction_cache_ways", "Instruction cache LRU associativity", "2"},
        {"instruction_cache_hit_cycles", "Lookup cycles including one prepaid issue cycle", "1"},
        {"array_pipeline_enabled", "Resume MVM at validated compute start; rd=0 reports started instead of completed", "true"},
        {"riscv_vector_enabled", "Enable RVV 1.0", "true"},
        {"riscv_vector_length_bits", "RVV VLEN in bits", "256"},
        {"riscv_vector_element_bits", "RVV ELEN in bits", "64"},
        {"serial_output", "Optional absolute path for QEMU serial output", ""},
        {"host_timeout_seconds", "Wall-time limit for one QEMU rendezvous", "30"})
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"qemu_memory", "Uncached SPM timing requests", "SST::Interfaces::StandardMem"})
    SST_ELI_DOCUMENT_PORTS(
        {"network_commands", "Guest network command/completion control path", {"TileComponents.NetworkCommand"}},
        {"external_commit", "Release SPM byte ranges after the QEMU access commits", {"TileComponents.ExternalCommit"}},
        {"analog_commands", "Vector transfers and MVM start/completion responses", {"TileComponents.ArrayCommand"}})

    RiscvQemu(SST::ComponentId_t id, SST::Params& params);
    ~RiscvQemu() override;
    void init(unsigned phase) override;
    void setup() override;
    void finish() override;
    void emergencyShutdown() override;

private:
    using Memory = SST::Interfaces::StandardMem;
    Memory* memory_ = nullptr;
    SST::Link* wake_ = nullptr;
    SST::Link* cacheWake_ = nullptr;
    SST::Link* commit_ = nullptr;
    SST::Link* analog_ = nullptr;
    SST::Link* network_ = nullptr;
    bool networkPending_ = false;
    std::uint64_t networkCommands_ = 0, networkCycles_ = 0, networkStart_ = 0;
    std::ofstream networkTrace_;
    SST::TimeConverter clock_;
    Riscv::QemuConfiguration config_{};
    Riscv::SharedSyncMemoryBridge bridge_;
    Riscv::QemuProcess process_;
    Riscv::CpuExecutionLedger ledger_;
    Riscv::ScratchpadBootImage boot_;
    std::optional<Riscv::QemuSyncEvent> event_;
    std::map<Memory::Request::id_t, bool> pending_;
    std::vector<ExternalCommit::Range> accessRanges_;
    struct LoadStoreEntry {
        std::uint64_t token, address, pc;
        std::uint32_t slot, bytes;
        bool write, serviced = false;
        std::size_t pending = 0;
        std::vector<ExternalCommit::Range> ranges;
        std::vector<std::uint8_t> data;
    };
    std::map<std::uint64_t, LoadStoreEntry> loadStoreEntries_;
    std::map<Memory::Request::id_t, std::uint64_t> loadStoreRequests_;
    std::uint32_t loadStoreDepth_ = 1;
    std::uint64_t lastLoadStoreToken_ = 0;
    std::uint64_t lsqEnqueued_ = 0, lsqCompleted_ = 0, lsqPeak_ = 0;
    std::uint64_t lsqRegisterStalls_ = 0, lsqFullStalls_ = 0, lsqDrainStalls_ = 0;
    std::uint64_t lsqStallCycles_ = 0, lsqWaitStart_ = 0, lsqWaitMask_ = 0;
    std::uint64_t lsqWaitPc_ = 0;
    std::uint32_t lsqWaitStopReason_ = 0;
    std::string lsqWaitReason_;
    bool lsqWaiting_ = false, lsqWaitAny_ = false;
    std::ofstream loadStoreTrace_, waitTrace_;
    std::map<std::uint64_t, LoadStoreEntry> scalarEntries_;
    std::map<Memory::Request::id_t, std::uint64_t> scalarRequests_;
    std::uint32_t scalarDepth_ = 0;
    std::uint64_t lastScalarToken_ = 0;
    std::uint64_t slqEnqueued_ = 0, slqCompleted_ = 0, slqPeak_ = 0;
    std::uint64_t slqRegisterStalls_ = 0, slqFullStalls_ = 0, slqDrainStalls_ = 0;
    std::uint64_t slqStallCycles_ = 0, slqWaitStart_ = 0, slqWaitMask_ = 0;
    std::uint64_t slqWaitPc_ = 0;
    std::uint32_t slqWaitStopReason_ = 0;
    std::string slqWaitReason_;
    bool slqWaiting_ = false, slqWaitAny_ = false;
    std::ofstream scalarTrace_, scalarWaitTrace_;
    std::uint64_t observationStartTask_ = 0;
    bool observationTaskPending_ = false;
    std::uint8_t* backingBytes_ = nullptr;
    std::uint64_t budget_, capacity_, requestBytes_, timeoutSeconds_;
    std::uint64_t reads_ = 0, writes_ = 0, fetches_ = 0, requests_ = 0, completions_ = 0;
    std::uint64_t vectorMemoryBeats_ = 0, vectorReadBytes_ = 0, vectorWriteBytes_ = 0;
    std::uint64_t grants_ = 0, stops_ = 0, endCycle_ = 0;
    std::uint64_t instructionBytes_ = 0, prepaidIssue_ = 0;
    std::uint64_t cacheStart_ = 0, cacheLine_ = 0, cacheLastLine_ = 0;
    std::unique_ptr<Riscv::InstructionCache> instructionCache_;
    bool cacheFillPending_ = false;
    std::uint64_t analogCommands_ = 0, analogReadBytes_ = 0, analogWriteBytes_ = 0;
    struct AnalogEntry {
        std::uint64_t token, queueToken, pc, offset;
        std::uint32_t slot, array, count, registers;
        Operation operation;
        std::vector<std::uint8_t> data;
        bool accepted = false, guaranteed = false, captured = false;
    };
    std::map<std::uint64_t, AnalogEntry> analogEntries_;
    std::uint32_t analogDepth_ = 0;
    std::uint64_t analogByteCapacity_ = 16384, analogActiveBytes_ = 0;
    std::uint64_t lastAnalogQueueToken_ = 0, analogAdmissionToken_ = 0;
    std::uint64_t asqEnqueued_ = 0, asqCompleted_ = 0, asqPeak_ = 0, asqPeakBytes_ = 0;
    std::uint64_t asqBusy_ = 0, asqGuaranteed_ = 0, asqFallback_ = 0, asqStallCycles_ = 0;
    std::uint64_t asqWaitStart_ = 0, asqWaitMask_ = 0, asqWaitPc_ = 0;
    std::uint32_t asqWaitReason_ = 0, asqWaitStop_ = 0;
    bool asqWaiting_ = false, asqWaitAny_ = false;
    std::ofstream analogQueueTrace_, analogWaitTrace_;
    std::ofstream taskTrace_;
    std::ofstream cacheTrace_;
    std::ofstream memoryTrace_;
    bool analogPending_ = false, analogAccepted_ = false;
    bool arrayPipelineEnabled_ = true, executionDrainWaiting_ = false;
    // Execute has no range or payload. Keep token/array identity independently
    // of event_: QEMU can move to another instruction before completion.
    std::map<std::uint64_t, std::uint32_t> startedExecutions_;
    int backingFd_ = -1;
    bool started_ = false, done_ = false, fetchedInGrant_ = false;

    void step(SST::Event* event);
    void grant();
    void capture();
    void resume();
    void dispatch();
    void access(bool fetch);
    void issueMemory(std::uint64_t address, std::uint64_t size, bool write);
    void releaseAccessRanges();
    void instructionFetch();
    void cacheLookup(SST::Event* event);
    void advanceInstructionFetch();
    void traceCache(const char* event, std::uint64_t address, std::uint64_t bytes);
    void traceMemory(const char* event);
    void memoryResponse(Memory::Request* response);
    bool loadStoreBarrier();
    bool loadStoreWaitSatisfied(std::uint64_t mask, bool any) const;
    void enqueueLoadStore();
    void loadStoreResponse(Memory::Request* response);
    void retireLoadStores();
    void traceLoadStore(const char* kind, const LoadStoreEntry* entry = nullptr,
                        const char* reason = "");
    void traceLoadStoreMemory(const char* kind, const LoadStoreEntry& entry);
    bool scalarBarrier();
    bool scalarWaitSatisfied(std::uint64_t mask, bool any) const;
    void enqueueScalar();
    void scalarResponse(Memory::Request* response);
    void retireScalars();
    void traceScalar(const char* kind, const LoadStoreEntry* entry = nullptr,
                        const char* reason = "");
    void traceScalarMemory(const char* kind, const LoadStoreEntry& entry);
    bool analogQueueBarrier();
    bool analogQueueWaitSatisfied(std::uint64_t mask, bool any, std::uint32_t reason) const;
    void enqueueAnalog();
    void queuedAnalogResponse(const ArrayCommand& answer);
    void wakeAnalogQueue();
    void traceAnalogQueue(const char* kind, const AnalogEntry* entry = nullptr);
    void analogCommand();
    void networkCommand();
    void networkResponse(SST::Event* response);
    void analogResponse(SST::Event* response);
    void traceTask();
    void guestExit();
    [[noreturn]] void fail(const std::string& message);
};
}
