#include "platform.h"

extern "C" int tile_main() {
    uart_puts("Golem: single tile booted\n");
    return 0;
}
