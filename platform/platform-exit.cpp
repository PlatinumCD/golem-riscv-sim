#include "platform.h"

namespace {

constexpr unsigned long kTestDeviceBase = 0x00100000UL;
constexpr unsigned int kPass = 0x5555U;
constexpr unsigned int kFail = 0x3333U;

volatile unsigned int* const test_device =
    reinterpret_cast<volatile unsigned int*>(kTestDeviceBase);

}  // namespace

extern "C" __attribute__((noreturn)) void platform_exit(unsigned long status) {
    const unsigned int value =
        status == 0 ? kPass
                    : ((static_cast<unsigned int>(status) & 0xffffU) << 16) |
                          kFail;

    *test_device = value;
    asm volatile("fence iorw, iorw" ::: "memory");

    for (;;) {
        asm volatile("wfi");
    }
}
