#include "mesh-nic.h"
#include "platform.h"

#include <stdint.h>

#ifndef MITTENS_TILE_ID
#error "MITTENS_TILE_ID must identify this worker image"
#endif

#define MITTENS_STRINGIFY_IMPL(value) #value
#define MITTENS_STRINGIFY(value) MITTENS_STRINGIFY_IMPL(value)

namespace {

constexpr uint32_t kShutdown = 0x53544f50U;

static_assert(MITTENS_TILE_ID >= 1);
static_assert(MITTENS_TILE_ID <= 7);

} // namespace

extern "C" int tile_main() {
    if (mesh_nic::receive() != kShutdown) {
        uart_puts("[tile " MITTENS_STRINGIFY(MITTENS_TILE_ID)
                  "] ERROR: invalid shutdown message\n");
        return 1;
    }
    return 0;
}
