#include "platform.h"

extern "C" int tile_main() {
    uart_puts("Golem Platform v0: single tile booted\n");
    return 0;
}
