#include "mesh-nic.h"
#include "platform.h"

#include <stdint.h>

namespace {

constexpr uint32_t kSenderTile = 0;
constexpr uint32_t kFloatThreePointTwoFive = 0x40500000U;
constexpr uint32_t kAcknowledgment = 0xacce5501U;
constexpr uint32_t kShutdown = 0x53544f50U;

} // namespace

extern "C" int tile_main() {
    uart_puts("[tile 8 at (2,2)] waiting for tile 0\n");

    const uint32_t payload = mesh_nic::receive();
    if (payload != kFloatThreePointTwoFive) {
        uart_puts("[tile 8] ERROR: payload is not float32 3.25\n");
        return 1;
    }

    uart_puts("[tile 8] received float32 3.25 after four hops\n");
    mesh_nic::send(kSenderTile, kAcknowledgment);

    if (mesh_nic::receive() != kShutdown) {
        uart_puts("[tile 8] ERROR: invalid shutdown message\n");
        return 1;
    }

    return 0;
}
