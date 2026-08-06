#ifndef HW_MISC_MITTENS_SYNC_H
#define HW_MISC_MITTENS_SYNC_H

#include "hw/qdev-core.h"
#include "mittens/SyncTileBridge.h"

#include <stdbool.h>
#include <stdint.h>

#define TYPE_MITTENS_SYNC "mittens-sync"

DeviceState *mittens_sync_create(void);

void helper_mittens_sync_vector_instruction(void);

bool mittens_sync_available(void);
bool mittens_sync_memory_timing_enabled(void);
void mittens_sync_memory_init_complete(void);
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
    uint64_t destination,
    uint32_t word_count);
void mittens_sync_yield_task(
    uint32_t reason,
    uint32_t task_id,
    uint64_t execution_id);
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

#endif
