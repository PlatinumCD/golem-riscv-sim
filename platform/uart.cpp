#include "platform.h"

namespace {

constexpr unsigned long kUartBase = 0x10000000UL;
constexpr unsigned long kTransmit = 0;
constexpr unsigned long kLineStatus = 5;
constexpr unsigned char kTransmitEmpty = 1U << 5;

volatile unsigned char* const uart =
    reinterpret_cast<volatile unsigned char*>(kUartBase);

void uart_putc_raw(char value) {
    while ((uart[kLineStatus] & kTransmitEmpty) == 0) {
    }
    uart[kTransmit] = static_cast<unsigned char>(value);
}
}  // namespace

extern "C" void uart_putc(char value) {
    if (value == '\n') {
        uart_putc_raw('\r');
    }
    uart_putc_raw(value);
}

extern "C" void uart_puts(const char* value) {
    while (*value != '\0') {
        uart_putc(*value++);
    }
}
