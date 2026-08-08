#include <stdint.h>

#include "platform.h"

namespace {

constexpr uint32_t kArray0 = 0;
constexpr uint32_t kVectorLength = 4;
constexpr float kTolerance = 1.0e-5F;

alignas(64) float matrix0[16] = {
    0.5F,  0.25F,  0.0F,   0.0F,
    0.0F,  0.5F,  -0.25F,  0.0F,
    0.0F,  0.0F,   0.5F,   0.25F,
    0.25F, 0.0F,   0.0F,   0.5F,
};

alignas(64) float input0[kVectorLength] = {
    0.5F, -0.25F, 0.125F, -0.5F,
};
alignas(64) float output0[kVectorLength] = {};

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

bool close(float actual, float expected) {
    const float difference = actual - expected;
    return difference == difference &&
           difference >= -kTolerance &&
           difference <= kTolerance;
}

bool equalVector(const float* actual, const float* expected) {
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        if (!close(actual[index], expected[index])) {
            return false;
        }
    }
    return true;
}

void printOperationResult(const char* operation, bool passed) {
    uart_puts(operation);
    uart_puts(passed ? ": PASS\n" : ": FAIL\n");
}

bool requireSuccess(const char* operation, unsigned long status) {
    const bool passed = status == 0;
    printOperationResult(operation, passed);
    return passed;
}

}  // namespace

extern "C" int tile_main() {
    constexpr float expected0[kVectorLength] = {
        0.1875F, -0.15625F, -0.0625F, -0.125F,
    };

    if (!requireSuccess("mvm.set array 0", setMatrix(matrix0, kArray0))) {
        return 1;
    }
    if (!requireSuccess("mvm.l array 0", loadVector(input0, kArray0))) {
        return 2;
    }
    if (!requireSuccess("mvm array 0", compute(kArray0))) {
        return 3;
    }
    if (!requireSuccess(
            "mvm.s array 0 status",
            storeVector(output0, kArray0))) {
        return 4;
    }

    const bool firstOutputPassed = equalVector(output0, expected0);
    printOperationResult(
        "mvm.s array 0 output [0.1875,-0.15625,-0.0625,-0.125]",
        firstOutputPassed);
    if (!firstOutputPassed) {
        return 5;
    }

    uart_puts("ANALOG_OPS_PASS: matrix-vector output validated\n");
    return 0;
}
