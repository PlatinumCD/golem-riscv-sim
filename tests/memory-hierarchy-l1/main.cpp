#include "mesh-nic.h"
#include "platform.h"

#include <stdint.h>

namespace {

volatile uint32_t probe_words[16] = {
    1, 2, 3, 4, 5, 6, 7, 8,
    9, 10, 11, 12, 13, 14, 15, 16,
};

}  // namespace

extern "C" int tile_main() {
    uint32_t sum = 0;

    for (uint32_t index = 0; index < 16; ++index) {
        probe_words[index] += 1;
        sum += probe_words[index];
    }
    mesh_nic::complete_memory_initialization();
    for (uint32_t index = 0; index < 16; ++index) {
        sum += probe_words[index];
    }

    if (sum != 304) {
        uart_puts("MEMORY_HIERARCHY_L1_FAIL\n");
        return 1;
    }

    uart_puts("MEMORY_HIERARCHY_L1_PASS\n");
    return 0;
}
