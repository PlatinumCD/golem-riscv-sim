#include "mesh-nic.h"
#include "platform.h"

#include <stdint.h>

#ifndef MITTENS_TILE_ID
#error "MITTENS_TILE_ID must identify this worker image"
#endif

#ifndef MITTENS_NEXT_TILE
#error "MITTENS_NEXT_TILE must identify the successor tile"
#endif

#ifndef MITTENS_EXPECTED_INPUT
#error "MITTENS_EXPECTED_INPUT must contain the expected float32 bits"
#endif

#ifndef MITTENS_EXPECTED_OUTPUT
#error "MITTENS_EXPECTED_OUTPUT must contain the expected float32 bits"
#endif

namespace {

static_assert(MITTENS_TILE_ID >= 1);
static_assert(MITTENS_TILE_ID <= 8);
static_assert(MITTENS_NEXT_TILE >= 0);
static_assert(MITTENS_NEXT_TILE <= 8);
static_assert(sizeof(float) == sizeof(uint32_t));

void uart_put_unsigned(uint32_t value) {
    char digits[10];
    uint32_t count = 0;

    do {
        digits[count++] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);

    while (count != 0U) {
        uart_putc(digits[--count]);
    }
}

} // namespace

extern "C" int tile_main() {
    const uint32_t incoming = mesh_nic::receive();
    if (incoming != MITTENS_EXPECTED_INPUT) {
        uart_puts("[pipeline worker] ERROR: unexpected input value\n");
        return 1;
    }

    float value = __builtin_bit_cast(float, incoming);
    volatile float tile_id = static_cast<float>(MITTENS_TILE_ID);
    value += tile_id;
    const uint32_t outgoing = __builtin_bit_cast(uint32_t, value);

    if (outgoing != MITTENS_EXPECTED_OUTPUT) {
        uart_puts("[pipeline worker] ERROR: unexpected output value\n");
        return 1;
    }

    uart_puts("[tile ");
    uart_put_unsigned(MITTENS_TILE_ID);
    uart_puts("] added ID ");
    uart_put_unsigned(MITTENS_TILE_ID);
    uart_puts(": result ");
    uart_put_unsigned(static_cast<uint32_t>(value));
    uart_puts("; forwarding to tile ");
    uart_put_unsigned(MITTENS_NEXT_TILE);
    uart_putc('\n');

    mesh_nic::send(MITTENS_NEXT_TILE, outgoing);
    return 0;
}
