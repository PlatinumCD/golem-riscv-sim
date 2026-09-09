#include <stdint.h>

#include "golem/runtime/runtime.h"
#include "materialized-model-inputs.h"
#include "../devices/platform.h"

namespace {

void printUnsigned(uint64_t value) {
    char digits[20];
    unsigned count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0) {
        uart_putc(digits[--count]);
    }
}

bool rejectSend(void*, uint32_t, uint32_t) {
    return false;
}

bool rejectReceive(void*, golem::runtime::RoutedWord*) {
    return false;
}

bool rejectGlobalDMASubmit(
    void*, golem::runtime::ExecutionId, uint32_t, uint64_t, uint64_t,
    uint64_t, uint32_t, golem::runtime::ScratchpadDMADirection, uint32_t
) {
    return false;
}

bool rejectGlobalDMAWait(void*, golem::runtime::ExecutionId, uint32_t) {
    return false;
}

struct PreflightEpochBarrier {
    uint32_t next_epoch = 0;
    uint32_t arrival_count = 0;
};

bool acceptPreflightEpoch(
    void* context, uint32_t completedEpoch,
    golem::runtime::EpochContribution
) {
    auto* barrier = static_cast<PreflightEpochBarrier*>(context);
    if (barrier == nullptr || completedEpoch != barrier->next_epoch) {
        return false;
    }
    ++barrier->next_epoch;
    ++barrier->arrival_count;
    return true;
}

const golem::runtime::RoutedWordTransport kPreflightTransport{
    nullptr,
    rejectSend,
    rejectReceive,
};

const golem::runtime::GlobalDMATransport kPreflightGlobalDMA{
    nullptr,
    rejectGlobalDMASubmit,
    rejectGlobalDMAWait,
};

}  // namespace

extern "C" int tile_main() {
    const golem::runtime::TileABI abi = golem::runtime::linkedTileABI();
    // Preflight deliberately pays for the exhaustive semantic proof outside
    // production SST timing. Production still validates the compiler's
    // canonical proof in modeled guest instructions via abi.valid().
    if (!abi.validExhaustive()) {
        uart_puts("SCULPTOR_RA_ABI_ERROR tile=");
        printUnsigned(abi.core_id);
        uart_puts(" stage=exhaustive_");
        printUnsigned(golem::runtime::tileABIValidationStage());
        uart_putc('\n');
        return 2;
    }
    if (!abi.valid()) {
        uart_puts("SCULPTOR_RA_ABI_ERROR tile=");
        printUnsigned(abi.core_id);
        uart_puts(" stage=");
        printUnsigned(golem::runtime::tileABIValidationStage());
        uart_putc('\n');
        return 2;
    }
    golem::runtime::HeapProfile heapProfile{};
    PreflightEpochBarrier epochBarrier{};
    const golem::runtime::EpochBarrierTransport preflightEpochTransport{
        &epochBarrier,
        acceptPreflightEpoch,
    };
    golem::runtime::DeploymentRuntime runtime{
        abi,
        kPreflightTransport,
        nullptr,
        nullptr,
        golem::runtime::DeploymentTransmitPolicy::Blocking,
        &heapProfile,
        kPreflightGlobalDMA,
        nullptr,
        preflightEpochTransport,
    };
    if (!runtime.initialize()) {
        uart_puts("SCULPTOR_RA_ABI_ERROR tile=");
        printUnsigned(abi.core_id);
        uart_puts(" stage=runtime_initialize code=");
        printUnsigned(static_cast<uint32_t>(runtime.error()));
        uart_puts(" heap_peak=");
        printUnsigned(heapProfile.peak_live_bytes);
        uart_puts(" failed_size=");
        printUnsigned(heapProfile.failed_allocation_size);
        uart_putc('\n');
        return 4;
    }
    if ((abi.abi_features & golem::runtime::TileABIEpochSchedule) != 0) {
        for (uint32_t epoch = 0; epoch < abi.epoch_count; ++epoch) {
            bool hasWork = false;
            for (uint32_t loop = 0; loop < abi.shard_loop_count; ++loop) {
                hasWork |= abi.shard_loops[loop].epoch_id == epoch;
            }
            const auto contribution =
                hasWork ? golem::runtime::EpochContribution::WorkComplete
                        : golem::runtime::EpochContribution::Idle;
            if (!acceptPreflightEpoch(&epochBarrier, epoch, contribution)) {
                uart_puts("SCULPTOR_RA_ABI_ERROR tile=");
                printUnsigned(abi.core_id);
                uart_puts(" stage=epoch_manifest epoch=");
                printUnsigned(epoch);
                uart_putc('\n');
                return 5;
            }
        }
        if (epochBarrier.arrival_count != abi.epoch_count) {
            uart_puts("SCULPTOR_RA_ABI_ERROR tile=");
            printUnsigned(abi.core_id);
            uart_puts(" stage=epoch_count expected=");
            printUnsigned(abi.epoch_count);
            uart_puts(" actual=");
            printUnsigned(epochBarrier.arrival_count);
            uart_putc('\n');
            return 5;
        }
    }
    golem::platform::MaterializedModelInputPlanStats seedPlan{};
    if (!golem::platform::validateMaterializedModelInputPlan(abi, &seedPlan)) {
        uart_puts("SCULPTOR_RA_ABI_ERROR tile=");
        printUnsigned(abi.core_id);
        uart_puts(" stage=materialized_seed_plan\n");
        return 3;
    }
    uart_puts("SCULPTOR_RA_ABI_SEED tile=");
    printUnsigned(abi.core_id);
    uart_puts(" descriptors=");
    printUnsigned(seedPlan.descriptor_count);
    uart_puts(" iterations=");
    printUnsigned(seedPlan.iteration_count);
    uart_puts(" transfers=");
    printUnsigned(seedPlan.transfer_count);
    uart_puts(" bytes=");
    printUnsigned(seedPlan.byte_count);
    uart_puts(" runtime_heap_bytes=");
    printUnsigned(heapProfile.peak_live_bytes);
    uart_puts(" receive_state_capacity=");
    printUnsigned(runtime.allocatedReceiveStateCapacity());
    uart_puts(" epoch_arrivals=");
    printUnsigned(epochBarrier.arrival_count);
    uart_putc('\n');
    uart_puts("SCULPTOR_RA_ABI_PASS tile=");
    printUnsigned(abi.core_id);
    uart_puts(" epoch_count=");
    printUnsigned(abi.epoch_count);
    uart_putc('\n');
    return 0;
}
