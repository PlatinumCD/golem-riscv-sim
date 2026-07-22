#pragma once

extern "C" void uart_putc(char value);
extern "C" void uart_puts(const char* value);
extern "C" void platform_exit(unsigned long status) __attribute__((noreturn));
