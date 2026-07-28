#include "platform.h"

namespace {

struct RankedMemRef1DF32 {
    float* allocated;
    float* aligned;
    long offset;
    long sizes[1];
    long strides[1];
};

static_assert(sizeof(long) == 8);

RankedMemRef1DF32 make_memref(float* data, long size) {
    return {
        .allocated = data,
        .aligned = data,
        .offset = 0,
        .sizes = {size},
        .strides = {1},
    };
}

}  // namespace

extern "C" void _mlir_ciface_model_forward(
    RankedMemRef1DF32* input,
    RankedMemRef1DF32* output);

extern "C" int tile_main() {
    float input[4] = {1.0F, 2.0F, 3.0F, 4.0F};
    float output[2] = {};
    const float expected[2] = {12.0F, 4.0F};

    auto input_memref = make_memref(input, 4);
    auto output_memref = make_memref(output, 2);

    uart_puts("PyTorch single-core RISC-V: executing two linear layers\n");
    _mlir_ciface_model_forward(&input_memref, &output_memref);

    for (unsigned int index = 0; index < 2; ++index) {
        if (output[index] != expected[index]) {
            uart_puts("PYTORCH_SINGLE_CORE_FAIL: incorrect output\n");
            return 1;
        }
    }

    uart_puts("output [12, 4]\n");
    uart_puts("PYTORCH_SINGLE_CORE_PASS\n");
    return 0;
}
