#include <stdint.h>

#include "platform.h"

namespace {

constexpr uint32_t kArray0 = 0;
constexpr uint32_t kArray1 = 1;

alignas(64) float matrix0[16] = {
    1.0F, 0.0F, 0.0F, 0.0F,
    0.0F, 1.0F, 0.0F, 0.0F,
    0.0F, 0.0F, 1.0F, 0.0F,
    0.0F, 0.0F, 0.0F, 1.0F,
};

alignas(64) float matrix1[16] = {
    0.5F, 0.0F, 0.0F, 0.0F,
    0.0F, 0.5F, 0.0F, 0.0F,
    0.0F, 0.0F, 0.5F, 0.0F,
    0.0F, 0.0F, 0.0F, 0.5F,
};

alignas(64) float input0[4] = {0.5F, -0.25F, 0.125F, -0.5F};
alignas(64) float input1[4] = {0.25F, 0.5F, -0.5F, 0.125F};
alignas(64) float output0[4] = {};
alignas(64) float output1[4] = {};
alignas(64) float movedOutput[4] = {};

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

unsigned long moveVector(uint32_t sourceId, uint32_t destinationId) {
    unsigned long status;
    asm volatile(
        "mvm.mv %0, %1, %2"
        : "=r"(status)
        : "r"(static_cast<unsigned long>(sourceId)),
          "r"(static_cast<unsigned long>(destinationId))
        : "memory");
    return status;
}

bool equalVector(const float* actual, const float* expected) {
    for (uint32_t index = 0; index < 4; ++index) {
        const float difference = actual[index] - expected[index];
        if (difference != difference ||
            difference < -1.0e-5F ||
            difference > 1.0e-5F) {
            return false;
        }
    }
    return true;
}

bool statusOk(unsigned long status, const char* operation) {
    if (status == 0) {
        return true;
    }
    uart_puts("ANALOG_INSTRUCTION_FAIL: ");
    uart_puts(operation);
    uart_puts("\n");
    return false;
}

}  // namespace

extern "C" int tile_main() {
    constexpr float expected0[4] = {0.5F, -0.25F, 0.125F, -0.5F};
    constexpr float expected1[4] = {0.125F, 0.25F, -0.25F, 0.0625F};
    constexpr float expectedMoved[4] = {0.25F, -0.125F, 0.0625F, -0.25F};

    /*
     * Issue work to both independent array queues before either blocking
     * store. Their transfers share one 256-bit tile link, while their
     * compute phases can still overlap.
     */
    if (!statusOk(setMatrix(matrix0, kArray0), "mvm.set array 0") ||
        !statusOk(setMatrix(matrix1, kArray1), "mvm.set array 1") ||
        !statusOk(loadVector(input0, kArray0), "mvm.l array 0") ||
        !statusOk(loadVector(input1, kArray1), "mvm.l array 1") ||
        !statusOk(compute(kArray0), "mvm array 0") ||
        !statusOk(compute(kArray1), "mvm array 1") ||
        !statusOk(storeVector(output0, kArray0), "mvm.s array 0") ||
        !statusOk(storeVector(output1, kArray1), "mvm.s array 1")) {
        return 1;
    }

    if (!equalVector(output0, expected0) ||
        !equalVector(output1, expected1)) {
        uart_puts("ANALOG_INSTRUCTION_FAIL: concurrent results\n");
        return 2;
    }

    /*
     * Exercise the fifth instruction by moving array 0's output directly
     * into array 1's input, then compute and read array 1 again.
     */
    if (!statusOk(
            moveVector(kArray0, kArray1), "mvm.mv array 0 to array 1") ||
        !statusOk(compute(kArray1), "mvm array 1 after move") ||
        !statusOk(
            storeVector(movedOutput, kArray1),
            "mvm.s array 1 after move")) {
        return 3;
    }

    if (!equalVector(movedOutput, expectedMoved)) {
        uart_puts("ANALOG_INSTRUCTION_FAIL: moved result\n");
        return 4;
    }

    uart_puts(
        "ANALOG_INSTRUCTION_PASS: two asynchronous arrays and all five "
        "instructions\n");
    return 0;
}
