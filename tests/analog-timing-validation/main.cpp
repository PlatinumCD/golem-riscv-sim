#include <stdint.h>

#include "platform.h"

#ifndef MITTENS_ANALOG_TIMING_ARRAY_COUNT
#define MITTENS_ANALOG_TIMING_ARRAY_COUNT 1
#endif

namespace {

constexpr uint32_t kArrayCount = MITTENS_ANALOG_TIMING_ARRAY_COUNT;
constexpr uint32_t kArrayRows = 9;
constexpr uint32_t kArrayColumns = 9;
constexpr uint32_t kMatrixWords = kArrayRows * kArrayColumns;

static_assert(kArrayCount == 1 || kArrayCount == 2);

alignas(64) float matrices[kArrayCount][kMatrixWords] = {};
alignas(64) float inputs[kArrayCount][kArrayColumns] = {};
alignas(64) float outputs[kArrayCount][kArrayRows] = {};

unsigned long setMatrix(const float* matrix, uint32_t arrayId) {
    unsigned long status;
    asm volatile(
        "mvm.set %0, %1, %2"
        : "=r"(status)
        : "r"(matrix), "r"(static_cast<unsigned long>(arrayId))
        : "memory");
    return status;
}

unsigned long loadVector(const float* input, uint32_t arrayId) {
    unsigned long status;
    asm volatile(
        "mvm.l %0, %1, %2"
        : "=r"(status)
        : "r"(input), "r"(static_cast<unsigned long>(arrayId))
        : "memory");
    return status;
}

unsigned long compute(uint32_t arrayId) {
    unsigned long status;
    const unsigned long id = arrayId;
    asm volatile(
        "mvm %0, %1, %1"
        : "=r"(status)
        : "r"(id)
        : "memory");
    return status;
}

unsigned long storeVector(float* output, uint32_t arrayId) {
    unsigned long status;
    asm volatile(
        "mvm.s %0, %1, %2"
        : "=r"(status)
        : "r"(output), "r"(static_cast<unsigned long>(arrayId))
        : "memory");
    return status;
}

bool success(unsigned long status) {
    return status == 0;
}

bool close(float actual, float expected) {
    const float difference = actual - expected;
    return difference == difference &&
           difference >= -1.0e-5F &&
           difference <= 1.0e-5F;
}

void initializeData() {
    for (uint32_t arrayId = 0; arrayId < kArrayCount; ++arrayId) {
        for (uint32_t row = 0; row < kArrayRows; ++row) {
            for (uint32_t column = 0; column < kArrayColumns; ++column) {
                matrices[arrayId][row * kArrayColumns + column] =
                    arrayId == 0 ? 1.0F : (row == column ? 1.0F : 0.0F);
            }
        }
        for (uint32_t column = 0; column < kArrayColumns; ++column) {
            inputs[arrayId][column] = 1.0F;
        }
    }
}

bool outputsAreCorrect() {
    for (uint32_t arrayId = 0; arrayId < kArrayCount; ++arrayId) {
        const float expected = arrayId == 0 ? 9.0F : 1.0F;
        for (uint32_t row = 0; row < kArrayRows; ++row) {
            if (!close(outputs[arrayId][row], expected)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

extern "C" int tile_main() {
    initializeData();

    /*
     * Submit corresponding operations to every array before advancing to
     * the next stage. The non-store instructions return after fd 41/fd 43
     * acceptance, allowing independent array queues to overlap. Stores are
     * completion waits and make the numerical result visible to the guest.
     */
    for (uint32_t arrayId = 0; arrayId < kArrayCount; ++arrayId) {
        if (!success(setMatrix(matrices[arrayId], arrayId))) {
            uart_puts("ANALOG_TIMING_FAIL: set\n");
            return 1;
        }
    }
    for (uint32_t arrayId = 0; arrayId < kArrayCount; ++arrayId) {
        if (!success(loadVector(inputs[arrayId], arrayId))) {
            uart_puts("ANALOG_TIMING_FAIL: load\n");
            return 2;
        }
    }
    for (uint32_t arrayId = 0; arrayId < kArrayCount; ++arrayId) {
        if (!success(compute(arrayId))) {
            uart_puts("ANALOG_TIMING_FAIL: compute\n");
            return 3;
        }
    }
    for (uint32_t arrayId = 0; arrayId < kArrayCount; ++arrayId) {
        if (!success(storeVector(outputs[arrayId], arrayId))) {
            uart_puts("ANALOG_TIMING_FAIL: store\n");
            return 4;
        }
    }

    if (!outputsAreCorrect()) {
        uart_puts("ANALOG_TIMING_FAIL: numerical output\n");
        return 5;
    }

#if MITTENS_ANALOG_TIMING_ARRAY_COUNT == 1
    uart_puts("ANALOG_TIMING_SINGLE_PASS\n");
#else
    uart_puts("ANALOG_TIMING_DUAL_PASS\n");
#endif
    return 0;
}
