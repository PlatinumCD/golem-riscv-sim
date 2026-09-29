#ifndef HW_MISC_MITTENS_SYNC_H
#define HW_MISC_MITTENS_SYNC_H

#include "hw/qdev-core.h"
#include "mittens/SyncTileBridge.h"

#include <stdbool.h>
#include <stdint.h>

#define TYPE_MITTENS_SYNC "mittens-sync"

DeviceState *mittens_sync_create(void);

void helper_mittens_sync_vector_instruction(void);
void helper_mittens_sync_memory_instruction(
    uint32_t source_register_mask,
    uint32_t destination_register_mask,
    uint32_t instruction_length,
    uint64_t program_counter);

bool mittens_sync_available(void);
bool mittens_sync_vector_analog_enabled(void);
uint32_t mittens_sync_vector_analog(
    uint32_t operation, uint64_t array_id, uint64_t element_offset,
    uint32_t element_count, uint8_t *data);
bool mittens_sync_memory_timing_enabled(void);
bool mittens_sync_instruction_fetch_timing_enabled(void);
bool mittens_sync_scratchpad_contains(uint64_t address, uint64_t byte_count);
/* A page-local RVV beat is authorized before any element touches memory.
 * The target TLB wrapper performs a nonfaulting, non-dirtying RAM probe. */
bool mittens_sync_vector_memory_enabled(void);
bool mittens_sync_vector_memory_probe(
    void *cpu_env, uint64_t address, uint32_t size, uint32_t element_size,
    bool write, uintptr_t retaddr);
bool mittens_sync_vector_memory_begin(
    const void *host, uint64_t physical_address, uint32_t size,
    uint32_t element_size, bool write, uint64_t program_counter,
    uint64_t return_address);
void mittens_sync_vector_memory_finish(void);
/* Deferred vector-memory queue. The target owns register snapshots and applies
 * completion payloads only at explicit QEMU instruction/trap boundaries. */
bool mittens_sync_lsq_enabled(void);
MittensSyncLoadStoreQueue *mittens_sync_lsq_queue(void);
bool mittens_sync_scratchpad_host_matches(const void *host, uint64_t address,
                                         uint32_t size);
bool mittens_sync_lsq_memory_probe(void *cpu_env, uint64_t address,
                                   uint32_t size, uint32_t element_size,
                                   bool write, uintptr_t retaddr,
                                   uint64_t *physical_address);
void mittens_sync_lsq_submit(uint32_t slot);
void mittens_sync_lsq_wait(uint64_t mask, bool any);
void mittens_sync_lsq_error(const char *message);
/* Separate analog transfer queue. Depth zero retains synchronous operation;
 * depth one is still asynchronous and independent of the memory LSQ depth. */
bool mittens_sync_asq_enabled(void);
MittensSyncAnalogQueue *mittens_sync_asq_queue(void);
void mittens_sync_asq_submit(uint32_t slot);
void mittens_sync_asq_wait(uint64_t mask, bool any);
bool mittens_asq_vector_analog(void *cpu_env, uint32_t operation,
                              uint32_t vector_register, uint64_t array_id,
                              uint64_t element_offset, uint32_t element_count,
                              uint32_t *status);
void mittens_lsq_before_instruction(uint64_t pc, uint32_t instruction, uint32_t length);
void mittens_lsq_after_instruction_fetch(void);
void mittens_lsq_drain(void);
bool mittens_lsq_vector_memory(void *destination, uint64_t base, void *cpu_env,
                               uint32_t desc, uint32_t log2_element_size,
                               uint32_t elements, bool write, bool whole, uintptr_t retaddr);
int64_t mittens_sync_wait_for_grant(int64_t qemu_budget);
void mittens_sync_begin_quantum(int64_t instruction_budget);
void mittens_sync_account_icount(
    int64_t executed,
    int64_t remaining);
void mittens_sync_quantum_end(void);
void mittens_sync_guest_exit(void);
void mittens_sync_yield_nic(uint32_t reason);
void mittens_sync_yield_nic_transmit(
    uint32_t reason,
    bool burst);
void mittens_sync_yield_receive_dma(
    uint32_t source,
    uint32_t route_id,
    uint64_t logical_iteration,
    uint64_t destination,
    uint32_t word_count);
void mittens_sync_yield_receive_software_claim(
    uint32_t source,
    uint32_t route_id,
    uint64_t logical_iteration,
    uint32_t word_count);
void mittens_sync_yield_task(
    uint32_t reason,
    uint32_t task_id,
    uint64_t execution_id);
void mittens_sync_yield_epoch(
    uint32_t epoch_id,
    uint32_t contribution);
void mittens_sync_yield_analog(
    uint32_t reason,
    uint32_t array_id,
    uint64_t analog_sequence,
    bool wait_for_completion);
void mittens_sync_yield_memory(
    uint64_t physical_address,
    uint32_t size,
    bool write,
    uint64_t program_counter,
    uint64_t return_address);
void mittens_sync_yield_memory_atomic(
    uint64_t physical_address,
    uint32_t size,
    uint64_t program_counter,
    uint64_t return_address);
void helper_mittens_sync_memory_fence(void);
void helper_mittens_sync_instruction_fetch(uint64_t program_counter,
                                           uint32_t instruction_length,
                                           uint32_t instruction);
void helper_mittens_sync_instruction_fence(void);

#endif
