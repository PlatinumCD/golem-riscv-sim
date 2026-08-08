#include "mesh-nic.h"
#include "platform.h"

#include <stdint.h>

namespace {

constexpr uint32_t kReceiverTile = 1;
constexpr uint32_t kFloatThreePointTwoFive = 0x40500000U;
constexpr uint32_t kAcknowledgment = 0xacce5501U;

} // namespace

extern "C" int tile_main() {
    uart_puts("[tile 0] sending float32 3.25 to tile 1\n");
    mesh_nic::send(kReceiverTile, kFloatThreePointTwoFive);

    uart_puts("[tile 0] waiting for acknowledgment\n");
    const uint32_t response = mesh_nic::receive();
    if (response != kAcknowledgment) {
        uart_puts("[tile 0] ERROR: invalid acknowledgment\n");
        return 1;
    }

    uart_puts("[tile 0] acknowledgment received; done\n");
    return 0;
}
