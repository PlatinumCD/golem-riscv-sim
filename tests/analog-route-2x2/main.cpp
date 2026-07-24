#include <stdint.h>

#include "mesh-nic.h"
#include "platform.h"

#ifndef MITTENS_TILE_ID
#error "MITTENS_TILE_ID must identify this tile image"
#endif
#ifndef MITTENS_ROUTE_POSITION
#error "MITTENS_ROUTE_POSITION must identify this tile's route position"
#endif
#ifndef MITTENS_NEXT_TILE
#error "MITTENS_NEXT_TILE must identify the next route tile"
#endif
#ifndef MITTENS_ROUTE_LABEL
#error "MITTENS_ROUTE_LABEL must identify the tested route"
#endif

static_assert(MITTENS_TILE_ID >= 0);
static_assert(MITTENS_TILE_ID < 4);
static_assert(MITTENS_ROUTE_POSITION >= 0);
static_assert(MITTENS_ROUTE_POSITION < 4);

namespace {

constexpr uint32_t kArrayId = 0;
constexpr uint32_t kVectorLength = 4;
constexpr float kTolerance = 1.0e-4F;

constexpr float kMatrix[16] = {
    0.5F, 0.0F, 0.0F, 0.0F,
    0.0F, 0.5F, 0.0F, 0.0F,
    0.0F, 0.0F, 0.5F, 0.0F,
    0.0F, 0.0F, 0.0F, 0.5F,
};

constexpr float kInitialInput[4] = {
    8.0F, 4.0F, 2.0F, 1.0F,
};

constexpr float kFinalOutput[4] = {
    0.5F, 0.25F, 0.125F, 0.0625F,
};

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

[[maybe_unused]] bool equalVector(
    const float* actual,
    const float* expected
) {
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        if (!close(actual[index], expected[index])) {
            return false;
        }
    }
    return true;
}

bool outputIsHalfInput() {
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        if (!close(output[index], input[index] * 0.5F)) {
            return false;
        }
    }
    return true;
}

[[maybe_unused]] void copyVector(
    float* destination,
    const float* source
) {
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        destination[index] = source[index];
    }
}

[[maybe_unused]] void sendVector(
    uint32_t destination,
    const float* vector
) {
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        mesh_nic::send(
            destination,
            __builtin_bit_cast(uint32_t, vector[index]));
    }
}

[[maybe_unused]] void receiveVector(float* vector) {
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
    if constexpr (MITTENS_ROUTE_POSITION == 0) {
        copyVector(input, kInitialInput);
    } else {
        receiveVector(input);
    }

    if (!runMatvec() || !outputIsHalfInput()) {
        uart_puts("ANALOG_ROUTE_ERROR\n");
        return 1;
    }

    if constexpr (MITTENS_ROUTE_POSITION < 3) {
        sendVector(MITTENS_NEXT_TILE, output);
    } else {
        if (!equalVector(output, kFinalOutput)) {
            uart_puts("ANALOG_ROUTE_ERROR\n");
            return 2;
        }
        uart_puts("ANALOG_ROUTE_" MITTENS_ROUTE_LABEL "_PASS\n");
    }

    return 0;
}
