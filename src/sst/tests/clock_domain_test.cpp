#include "../execution/clockDomain.h"
#include <cassert>
#include <iostream>
#include <type_traits>

using namespace SST::Mittens::Timing;

int main() {
    constexpr Clock<Cpu> cpu{1000};
    constexpr Clock<ReceiveDMA> dma{2000};
    static_assert(!std::is_convertible_v<Cycles<Cpu>, Cycles<ReceiveDMA>>);
    static_assert(cpu.ticks({2}).value == 2000);
    static_assert(cpu.floor({1999}).value == 1);
    static_assert(cpu.ceil({1999}).value == 2);
    static_assert(cpu.ceil(dma.ticks({46})).value == 92);
    static_assert(dma.ceil(cpu.ticks({3})).value == 2);
    static_assert(cpu.after({100}, {3}).value == 3100);
    static_assert(ceilDivide(UINT64_MAX, 2) == UINT64_MAX / 2 + 1);
    for (std::uint64_t sourceFactor = 1; sourceFactor <= 17; ++sourceFactor)
        for (std::uint64_t targetFactor = 1; targetFactor <= 17; ++targetFactor)
            for (std::uint64_t cycle = 0; cycle < 100; ++cycle) {
                const Clock<Cpu> source{sourceFactor};
                const Clock<ReceiveDMA> target{targetFactor};
                const Ticks actual = source.ticks({cycle});
                const Ticks delivered = target.ticks(target.ceil(actual));
                assert(delivered.value >= actual.value);
                assert(delivered.value - actual.value < targetFactor);
            }
    int rejected = 0;
    try { (void)Clock<Cpu>{0}; } catch (const std::invalid_argument&) { ++rejected; }
    try { (void)cpu.ticks({UINT64_MAX}); } catch (const std::overflow_error&) { ++rejected; }
    try { (void)cpu.after({UINT64_MAX}, {1}); } catch (const std::overflow_error&) { ++rejected; }
    try { (void)ceilDivide(1, 0); } catch (const std::invalid_argument&) { ++rejected; }
    assert(rejected == 4);
    std::cout << "clock domains and rounding: PASS\n";
}
