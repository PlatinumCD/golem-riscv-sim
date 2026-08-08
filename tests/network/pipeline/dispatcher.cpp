#include "mesh-nic.h"
#include "platform.h"

#include <stdint.h>

namespace {

constexpr uint32_t kFirstWorkerTile = 1;
constexpr uint32_t kInitialValue = 0x00000000U;
constexpr uint32_t kExpectedResult = 0x42100000U;

static_assert(sizeof(float) == sizeof(uint32_t));

} // namespace

extern "C" int tile_main() {
    float value = __builtin_bit_cast(float, kInitialValue);
    volatile float tile_id = 0.0F;
    value += tile_id;

    uart_puts("[tile 0] added ID 0: result 0; dispatching to tile 1\n");
    mesh_nic::send(kFirstWorkerTile, __builtin_bit_cast(uint32_t, value));

    uart_puts("[tile 0] waiting for the completed pipeline\n");
    if (mesh_nic::receive() != kExpectedResult) {
        uart_puts("[tile 0] ERROR: final result is not float32 36.0\n");
        return 1;
    }

    uart_puts("[tile 0] final result is float32 36.0; pipeline complete\n");
    return 0;
}
