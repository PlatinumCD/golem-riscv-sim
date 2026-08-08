#include "platform.h"

extern "C" int tile_main() {
    uart_puts("Golem Platform v0.1: single tile booted\n");
    return 0;
}
