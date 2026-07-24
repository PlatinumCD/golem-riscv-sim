#include <stdint.h>

#include "mesh-nic.h"
#include "platform.h"

#ifndef MITTENS_TILE_ID
#error "MITTENS_TILE_ID must identify this tile image"
#endif

static_assert(MITTENS_TILE_ID >= 0);
static_assert(MITTENS_TILE_ID < 4);

namespace {

constexpr uint32_t kArray0 = 0;
constexpr uint32_t kArray1 = 1;
constexpr uint32_t kVectorLength = 4;
constexpr float kTolerance = 1.0e-4F;

#if MITTENS_TILE_ID == 0
constexpr float kMatrix0[16] = {
    0.5F,  0.25F,  0.0F,   0.0F,
    0.0F,  0.5F,  -0.25F,  0.0F,
    0.0F,  0.0F,   0.5F,   0.25F,
    0.25F, 0.0F,   0.0F,   0.5F,
};
constexpr float kMatrix1[16] = {
    0.5F, 0.0F, 0.0F, 0.0F,
    0.0F, 0.5F, 0.0F, 0.0F,
    0.0F, 0.0F, 0.5F, 0.0F,
    0.0F, 0.0F, 0.0F, 0.5F,
};
constexpr float kFirstExpectedOutput[4] = {
    0.1875F, -0.15625F, -0.0625F, -0.125F,
};
constexpr float kSecondExpectedInput[4] = {
    -0.015625F, 0.0234375F, -0.01953125F, -0.0078125F,
};
constexpr float kSecondExpectedOutput[4] = {
    -0.0078125F, 0.01171875F, -0.009765625F, -0.00390625F,
};
constexpr char kFirstMessage[] =
    "[tile 0 array 0] analog output "
    "[0.1875,-0.15625,-0.0625,-0.125]; sending to tile 1\n";
constexpr char kSecondMessage[] =
    "[tile 0 array 1] final analog output "
    "[-0.0078125,0.01171875,-0.009765625,-0.00390625]"
    "; pipeline complete\n";
#elif MITTENS_TILE_ID == 1
constexpr float kMatrix0[16] = {
    0.5F, 0.0F, 0.0F, 0.0F,
    0.0F, 0.5F, 0.0F, 0.0F,
    0.0F, 0.0F, 0.5F, 0.0F,
    0.0F, 0.0F, 0.0F, 0.5F,
};
constexpr float kMatrix1[16] = {
    0.0F, 0.0F, 0.0F, 1.0F,
    0.0F, 0.0F, 1.0F, 0.0F,
    0.0F, 1.0F, 0.0F, 0.0F,
    1.0F, 0.0F, 0.0F, 0.0F,
};
constexpr float kFirstExpectedInput[4] = {
    0.1875F, -0.15625F, -0.0625F, -0.125F,
};
constexpr float kFirstExpectedOutput[4] = {
    0.09375F, -0.078125F, -0.03125F, -0.0625F,
};
constexpr float kSecondExpectedInput[4] = {
    -0.0078125F, -0.01953125F, 0.0234375F, -0.015625F,
};
constexpr float kSecondExpectedOutput[4] = {
    -0.015625F, 0.0234375F, -0.01953125F, -0.0078125F,
};
constexpr char kFirstMessage[] =
    "[tile 1 array 0] analog output "
    "[0.09375,-0.078125,-0.03125,-0.0625]; sending to tile 3\n";
constexpr char kSecondMessage[] =
    "[tile 1 array 1] analog output "
    "[-0.015625,0.0234375,-0.01953125,-0.0078125]"
    "; sending to tile 0\n";
#elif MITTENS_TILE_ID == 3
constexpr float kMatrix0[16] = {
    0.0F, 0.0F, 0.0F, 1.0F,
    0.0F, 0.0F, 1.0F, 0.0F,
    0.0F, 1.0F, 0.0F, 0.0F,
    1.0F, 0.0F, 0.0F, 0.0F,
};
constexpr float kMatrix1[16] = {
    0.5F, 0.0F, 0.0F, 0.0F,
    0.0F, 0.5F, 0.0F, 0.0F,
    0.0F, 0.0F, 0.5F, 0.0F,
    0.0F, 0.0F, 0.0F, 0.5F,
};
constexpr float kFirstExpectedInput[4] = {
    0.09375F, -0.078125F, -0.03125F, -0.0625F,
};
constexpr float kFirstExpectedOutput[4] = {
    -0.0625F, -0.03125F, -0.078125F, 0.09375F,
};
constexpr float kSecondExpectedInput[4] = {
    -0.015625F, -0.0390625F, 0.046875F, -0.03125F,
};
constexpr float kSecondExpectedOutput[4] = {
    -0.0078125F, -0.01953125F, 0.0234375F, -0.015625F,
};
constexpr char kFirstMessage[] =
    "[tile 3 array 0] analog output "
    "[-0.0625,-0.03125,-0.078125,0.09375]; sending to tile 2\n";
constexpr char kSecondMessage[] =
    "[tile 3 array 1] analog output "
    "[-0.0078125,-0.01953125,0.0234375,-0.015625]"
    "; sending to tile 1\n";
#else
constexpr float kMatrix0[16] = {
    0.5F, 0.0F, 0.0F, 0.0F,
    0.0F, 0.5F, 0.0F, 0.0F,
    0.0F, 0.0F, 0.5F, 0.0F,
    0.0F, 0.0F, 0.0F, 0.5F,
};
constexpr float kMatrix1[16] = {
    0.0F, 1.0F, 0.0F, 0.0F,
    0.0F, 0.0F, 1.0F, 0.0F,
    0.0F, 0.0F, 0.0F, 1.0F,
    1.0F, 0.0F, 0.0F, 0.0F,
};
constexpr float kFirstExpectedInput[4] = {
    -0.0625F, -0.03125F, -0.078125F, 0.09375F,
};
constexpr float kFirstExpectedOutput[4] = {
    -0.03125F, -0.015625F, -0.0390625F, 0.046875F,
};
constexpr float kSecondExpectedInput[4] = {
    -0.03125F, -0.015625F, -0.0390625F, 0.046875F,
};
constexpr float kSecondExpectedOutput[4] = {
    -0.015625F, -0.0390625F, 0.046875F, -0.03125F,
};
constexpr char kFirstMessage[] =
    "[tile 2 array 0] analog output "
    "[-0.03125,-0.015625,-0.0390625,0.046875]"
    "; loading tile 2 array 1\n";
constexpr char kSecondMessage[] =
    "[tile 2 array 1] analog output "
    "[-0.015625,-0.0390625,0.046875,-0.03125]"
    "; sending to tile 3\n";
#endif

alignas(64) float input[kVectorLength] = {};
alignas(64) float output[kVectorLength] = {};

unsigned long setMatrix(
    const float* matrix,
    uint32_t arrayId
) {
    unsigned long status;
    asm volatile(
        "mvm.set %0, %1, %2"
        : "=r"(status)
        : "r"(matrix), "r"(static_cast<unsigned long>(arrayId))
        : "memory");
    return status;
}

unsigned long loadVector(
    const float* vector,
    uint32_t arrayId
) {
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

unsigned long storeVector(
    float* vector,
    uint32_t arrayId
) {
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

[[maybe_unused]] void copyVector(
    float* destination,
    const float* source
) {
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        destination[index] = source[index];
    }
}

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

bool runStage(
    uint32_t arrayId,
    const float* matrix,
    const float* expectedOutput,
    const char* message
) {
    if (setMatrix(matrix, arrayId) != 0 ||
        loadVector(input, arrayId) != 0 ||
        compute(arrayId) != 0 ||
        storeVector(output, arrayId) != 0 ||
        !equalVector(output, expectedOutput)) {
        return false;
    }

    uart_puts(message);
    return true;
}

bool requireInput(const float* expected) {
    if (equalVector(input, expected)) {
        return true;
    }
    uart_puts("[dual-array mesh] ERROR: input vector is incorrect\n");
    return false;
}

}  // namespace

extern "C" int tile_main() {
#if MITTENS_TILE_ID == 0
    constexpr float kInitialInput[4] = {
        0.5F, -0.25F, 0.125F, -0.5F,
    };

    copyVector(input, kInitialInput);
    if (!runStage(
            kArray0,
            kMatrix0,
            kFirstExpectedOutput,
            kFirstMessage)) {
        uart_puts("[tile 0 array 0] ERROR\n");
        return 1;
    }
    sendVector(1, output);

    receiveVector(input);
    if (!requireInput(kSecondExpectedInput) ||
        !runStage(
            kArray1,
            kMatrix1,
            kSecondExpectedOutput,
            kSecondMessage)) {
        uart_puts("[tile 0 array 1] ERROR\n");
        return 2;
    }
    return 0;
#elif MITTENS_TILE_ID == 1
    receiveVector(input);
    if (!requireInput(kFirstExpectedInput) ||
        !runStage(
            kArray0,
            kMatrix0,
            kFirstExpectedOutput,
            kFirstMessage)) {
        uart_puts("[tile 1 array 0] ERROR\n");
        return 1;
    }
    sendVector(3, output);

    receiveVector(input);
    if (!requireInput(kSecondExpectedInput) ||
        !runStage(
            kArray1,
            kMatrix1,
            kSecondExpectedOutput,
            kSecondMessage)) {
        uart_puts("[tile 1 array 1] ERROR\n");
        return 2;
    }
    sendVector(0, output);
    return 0;
#elif MITTENS_TILE_ID == 3
    receiveVector(input);
    if (!requireInput(kFirstExpectedInput) ||
        !runStage(
            kArray0,
            kMatrix0,
            kFirstExpectedOutput,
            kFirstMessage)) {
        uart_puts("[tile 3 array 0] ERROR\n");
        return 1;
    }
    sendVector(2, output);

    receiveVector(input);
    if (!requireInput(kSecondExpectedInput) ||
        !runStage(
            kArray1,
            kMatrix1,
            kSecondExpectedOutput,
            kSecondMessage)) {
        uart_puts("[tile 3 array 1] ERROR\n");
        return 2;
    }
    sendVector(1, output);
    return 0;
#else
    receiveVector(input);
    if (!requireInput(kFirstExpectedInput) ||
        !runStage(
            kArray0,
            kMatrix0,
            kFirstExpectedOutput,
            kFirstMessage)) {
        uart_puts("[tile 2 array 0] ERROR\n");
        return 1;
    }

    copyVector(input, output);
    if (!requireInput(kSecondExpectedInput) ||
        !runStage(
            kArray1,
            kMatrix1,
            kSecondExpectedOutput,
            kSecondMessage)) {
        uart_puts("[tile 2 array 1] ERROR\n");
        return 2;
    }
    sendVector(3, output);
    return 0;
#endif
}
