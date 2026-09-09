#include "mesh-nic.h"
#include "platform.h"

#include <stdint.h>

#if defined(MITTENS_READY_SET_ANALOG_FIRST)
namespace {

alignas(64) float matrix[1] = {1.0F};

unsigned long setMatrix() {
    unsigned long status;
    const unsigned long array_id = 0;
    asm volatile(
        "mvm.set %0, %1, %2"
        : "=r"(status)
        : "r"(matrix), "r"(array_id)
        : "memory");
    return status;
}

}  // namespace
#endif

extern "C" int tile_main() {
#if defined(MITTENS_READY_SET_ANALOG_FIRST)
    // The analog slot is tile-private.  QEMU publishes it and stops at the
    // submit boundary; only the SST event thread may accept or execute it.
    if (setMatrix() != 0) {
        uart_puts("MITTENS_READY_SET_ERROR analog submit\n");
        return 2;
    }
#endif

    // All execution after the first captured event is ordinary synchronized
    // QEMU work.
    mesh_nic::complete_memory_initialization();

    volatile uint64_t checksum = 0;
    for (uint64_t iteration = 1; iteration <= 32768; ++iteration) {
        checksum = checksum + iteration;
    }
    if (checksum != UINT64_C(536887296)) {
        uart_puts("MITTENS_READY_SET_ERROR checksum\n");
        return 1;
    }

    uart_puts("MITTENS_READY_SET_PASS\n");
    return 0;
}
