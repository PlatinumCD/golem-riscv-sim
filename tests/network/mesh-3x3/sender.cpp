#include "mesh-nic.h"
#include "platform.h"

#include <stdint.h>

namespace {

constexpr uint32_t kFirstWorkerTile = 1;
constexpr uint32_t kReceiverTile = 8;
constexpr uint32_t kFloatThreePointTwoFive = 0x40500000U;
constexpr uint32_t kAcknowledgment = 0xacce5501U;
constexpr uint32_t kShutdown = 0x53544f50U;

} // namespace

extern "C" int tile_main() {
    uart_puts("[tile 0 at (0,0)] sending float32 3.25 to tile 8 at (2,2)\n");
    mesh_nic::send(kReceiverTile, kFloatThreePointTwoFive);

    uart_puts("[tile 0] waiting for the four-hop acknowledgment\n");
    const uint32_t response = mesh_nic::receive();
    if (response != kAcknowledgment) {
        uart_puts("[tile 0] ERROR: invalid acknowledgment\n");
        return 1;
    }

    uart_puts("[tile 0] acknowledgment received; stopping mesh workers\n");
    for (uint32_t destination = kFirstWorkerTile;
         destination <= kReceiverTile;
         ++destination) {
        mesh_nic::send(destination, kShutdown);
    }

    return 0;
}
