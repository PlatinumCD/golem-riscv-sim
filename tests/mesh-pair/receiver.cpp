#include "mesh-nic.h"
#include "platform.h"

#include <stdint.h>

namespace {

constexpr uint32_t kSenderTile = 0;
constexpr uint32_t kFloatThreePointTwoFive = 0x40500000U;
constexpr uint32_t kAcknowledgment = 0xacce5501U;

} // namespace

extern "C" int tile_main() {
    uart_puts("[tile 1] waiting for a mesh packet\n");

    const uint32_t payload = mesh_nic::receive();
    if (payload != kFloatThreePointTwoFive) {
        uart_puts("[tile 1] ERROR: payload is not float32 3.25\n");
        return 1;
    }

    uart_puts("[tile 1] received float32 3.25 from tile 0\n");
    uart_puts("[tile 1] sending acknowledgment\n");
    mesh_nic::send(kSenderTile, kAcknowledgment);
    return 0;
}
