#include <stdint.h>

#include "mesh-nic.h"
#include "platform.h"

#ifndef MITTENS_TILE_ID
#error "MITTENS_TILE_ID must identify this tile image"
#endif

static_assert(MITTENS_TILE_ID >= 0);
static_assert(MITTENS_TILE_ID < 4);

namespace {

constexpr uint32_t kArrayId = 0;
constexpr uint32_t kVectorLength = 4;
constexpr float kTolerance = 1.0e-4F;

#if MITTENS_TILE_ID == 0
constexpr float kMatrix[16] = {
    0.5F,  0.25F,  0.0F,   0.0F,
    0.0F,  0.5F,  -0.25F,  0.0F,
    0.0F,  0.0F,   0.5F,   0.25F,
    0.25F, 0.0F,   0.0F,   0.5F,
};
constexpr float kExpectedOutput[4] = {
    0.1875F, -0.15625F, -0.0625F, -0.125F,
};
constexpr char kOutputMessage[] =
    "[tile 0] analog output [0.1875,-0.15625,-0.0625,-0.125]"
    "; sending to tile 1\n";
#elif MITTENS_TILE_ID == 1
constexpr float kMatrix[16] = {
    0.5F, 0.0F, 0.0F, 0.0F,
    0.0F, 0.5F, 0.0F, 0.0F,
    0.0F, 0.0F, 0.5F, 0.0F,
    0.0F, 0.0F, 0.0F, 0.5F,
};
constexpr float kExpectedInput[4] = {
    0.1875F, -0.15625F, -0.0625F, -0.125F,
};
constexpr float kExpectedOutput[4] = {
    0.09375F, -0.078125F, -0.03125F, -0.0625F,
};
constexpr uint32_t kNextTile = 3;
constexpr char kOutputMessage[] =
    "[tile 1] analog output [0.09375,-0.078125,-0.03125,-0.0625]"
    "; sending to tile 3\n";
#elif MITTENS_TILE_ID == 3
constexpr float kMatrix[16] = {
    0.0F, 0.0F, 0.0F, 1.0F,
    0.0F, 0.0F, 1.0F, 0.0F,
    0.0F, 1.0F, 0.0F, 0.0F,
    1.0F, 0.0F, 0.0F, 0.0F,
};
constexpr float kExpectedInput[4] = {
    0.09375F, -0.078125F, -0.03125F, -0.0625F,
};
constexpr float kExpectedOutput[4] = {
    -0.0625F, -0.03125F, -0.078125F, 0.09375F,
};
constexpr uint32_t kNextTile = 2;
constexpr char kOutputMessage[] =
    "[tile 3] analog output [-0.0625,-0.03125,-0.078125,0.09375]"
    "; sending to tile 2\n";
#else
constexpr float kMatrix[16] = {
    0.5F, 0.0F, 0.0F, 0.0F,
    0.0F, 0.5F, 0.0F, 0.0F,
    0.0F, 0.0F, 0.5F, 0.0F,
    0.0F, 0.0F, 0.0F, 0.5F,
};
constexpr float kExpectedInput[4] = {
    -0.0625F, -0.03125F, -0.078125F, 0.09375F,
};
constexpr float kExpectedOutput[4] = {
    -0.03125F, -0.015625F, -0.0390625F, 0.046875F,
};
constexpr uint32_t kNextTile = 0;
constexpr char kOutputMessage[] =
    "[tile 2] analog output [-0.03125,-0.015625,-0.0390625,0.046875]"
    "; returning to tile 0\n";
#endif

alignas(64) float input[kVectorLength] = {};
alignas(64) float output[kVectorLength] = {};

unsigned long setMatrix(const float* matrix, uint32_t arrayId) {
    unsigned long status;
    asm volatile(
        "mvm.set %0, %1, %2"
        : "=r"(status)
        : "r"(matrix), "r"(static_cast<unsigned long>(arrayId))
        : "memory");
    return status;
}

unsigned long loadVector(const float* vector, uint32_t arrayId) {
    unsigned long status;
    asm volatile(
        "mvm.l %0, %1, %2"
        : "=r"(status)
        : "r"(vector), "r"(static_cast<unsigned long>(arrayId))
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

unsigned long storeVector(float* vector, uint32_t arrayId) {
    unsigned long status;
    asm volatile(
        "mvm.s %0, %1, %2"
        : "=r"(status)
        : "r"(vector), "r"(static_cast<unsigned long>(arrayId))
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

#if MITTENS_TILE_ID == 0
void copyVector(float* destination, const float* source) {
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        destination[index] = source[index];
    }
}
#endif

void sendVector(uint32_t destination, const float* vector) {
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        mesh_nic::send(
            destination,
            __builtin_bit_cast(uint32_t, vector[index]));
    }
}

void receiveVector(float* vector) {
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        vector[index] =
            __builtin_bit_cast(float, mesh_nic::receive());
    }
}

bool runMatvec() {
    return setMatrix(kMatrix, kArrayId) == 0 &&
           loadVector(input, kArrayId) == 0 &&
           compute(kArrayId) == 0 &&
           storeVector(output, kArrayId) == 0;
}

}  // namespace

extern "C" int tile_main() {
#if MITTENS_TILE_ID == 0
    constexpr float kInitialInput[4] = {
        0.5F, -0.25F, 0.125F, -0.5F,
    };
    constexpr float kFinalOutput[4] = {
        -0.03125F, -0.015625F, -0.0390625F, 0.046875F,
    };

    copyVector(input, kInitialInput);
    if (!runMatvec() || !equalVector(output, kExpectedOutput)) {
        uart_puts("[tile 0] ERROR: local analog MVM failed\n");
        return 1;
    }

    uart_puts(kOutputMessage);
    sendVector(1, output);

    receiveVector(input);
    if (!equalVector(input, kFinalOutput)) {
        uart_puts("[tile 0] ERROR: final vector is incorrect\n");
        return 2;
    }

    uart_puts(
        "[tile 0] final vector "
        "[-0.03125,-0.015625,-0.0390625,0.046875]; pipeline complete\n");
    return 0;
#else
    receiveVector(input);
    if (!equalVector(input, kExpectedInput)) {
        uart_puts("[analog mesh worker] ERROR: input vector is incorrect\n");
        return 1;
    }
    if (!runMatvec() || !equalVector(output, kExpectedOutput)) {
        uart_puts("[analog mesh worker] ERROR: local analog MVM failed\n");
        return 2;
    }

    uart_puts(kOutputMessage);
    sendVector(kNextTile, output);
    return 0;
#endif
}
