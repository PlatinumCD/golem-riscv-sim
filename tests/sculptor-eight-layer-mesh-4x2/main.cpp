#include <stdint.h>

#include "golem/runtime/runtime.h"
#include "mesh-nic.h"
#include "platform.h"

#ifndef GOLEM_DEPLOYMENT_CORE_ID
#error "GOLEM_DEPLOYMENT_CORE_ID must select one of the eight isolated cores"
#endif

static_assert(
    GOLEM_DEPLOYMENT_CORE_ID >= 0 && GOLEM_DEPLOYMENT_CORE_ID < 8
);

namespace {

using namespace golem::runtime;

constexpr uint32_t kReadyBase = UINT32_C(0x52440000);
[[maybe_unused]] constexpr uint32_t kDoneWord = UINT32_C(0x444f4e45);
constexpr uint32_t kVectorLength = 4;
constexpr float kTolerance = 1.0e-4F;

#if GOLEM_DEPLOYMENT_CORE_ID == 0
constexpr float kExpectedOutput[kVectorLength] = {
    2.0F,
    3.0F,
    4.0F,
    5.0F,
};
#elif GOLEM_DEPLOYMENT_CORE_ID == 1
constexpr float kExpectedOutput[kVectorLength] = {
    4.0F,
    7.0F,
    10.0F,
    13.0F,
};
#elif GOLEM_DEPLOYMENT_CORE_ID == 2
constexpr float kExpectedOutput[kVectorLength] = {
    12.0F,
    9.0F,
    6.0F,
    3.0F,
};
#elif GOLEM_DEPLOYMENT_CORE_ID == 3
constexpr float kExpectedOutput[kVectorLength] = {
    21.0F,
    10.0F,
    20.0F,
    15.0F,
};
#elif GOLEM_DEPLOYMENT_CORE_ID == 7
constexpr float kExpectedOutput[kVectorLength] = {
    22.0F,
    12.0F,
    23.0F,
    19.0F,
};
#elif GOLEM_DEPLOYMENT_CORE_ID == 6
constexpr float kExpectedOutput[kVectorLength] = {
    22.0F,
    24.0F,
    23.0F,
    38.0F,
};
#elif GOLEM_DEPLOYMENT_CORE_ID == 5
constexpr float kExpectedOutput[kVectorLength] = {
    20.0F,
    20.0F,
    36.0F,
    21.0F,
};
#else
constexpr float kExpectedOutput[kVectorLength] = {
    56.0F,
    42.0F,
    42.0F,
    60.0F,
};
#endif

struct RankedMemRef2DF32 {
    float* allocated;
    float* aligned;
    int64_t offset;
    int64_t sizes[2];
    int64_t strides[2];
};

bool trySendWord(void*, uint32_t destination_tile, uint32_t word) {
    return mesh_nic::try_send(destination_tile, word);
}

bool tryReceiveWord(void*, uint32_t* word) {
    return mesh_nic::try_receive(word);
}

const WordTransport kTransport{
    nullptr,
    trySendWord,
    tryReceiveWord,
};

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

bool executeLocalTask(
    const BasicTileRuntime& runtime,
    const TileABI& abi,
    float* input,
    float* output
) {
    RankedMemRef2DF32 input_descriptor{
        input,
        input,
        0,
        {1, kVectorLength},
        {kVectorLength, 1},
    };
    RankedMemRef2DF32 output_descriptor{
        output,
        output,
        0,
        {1, kVectorLength},
        {kVectorLength, 1},
    };
    Tensor inputs[] = {
        {ElementType::Float32, 2, &input_descriptor},
    };
    Tensor outputs[] = {
        {ElementType::Float32, 2, &output_descriptor},
    };

    return abi.dispatch_tasks.task_count == 1 &&
           runtime.execute(
               abi.dispatch_tasks.tasks[0].id,
               inputs,
               1,
               outputs,
               1
           ) &&
           equalVector(output, kExpectedOutput);
}

#if GOLEM_DEPLOYMENT_CORE_ID == 0
bool waitForWorkersReady() {
    bool ready[8] = {
        true,
        false,
        false,
        false,
        false,
        false,
        false,
        false,
    };
    uint32_t ready_count = 1;

    while (ready_count != 8) {
        const uint32_t word = mesh_nic::receive();
        const uint32_t core_id = word & UINT32_C(0x0000ffff);
        if ((word & UINT32_C(0xffff0000)) != kReadyBase ||
            core_id == 0 ||
            core_id >= 8 ||
            ready[core_id]) {
            return false;
        }
        ready[core_id] = true;
        ++ready_count;
    }
    return true;
}
#endif

}  // namespace

extern "C" int tile_main() {
    const TileABI abi = linkedTileABI();
    BasicTileRuntime runtime{abi, kTransport};

    if (!runtime.valid() ||
        abi.core_id != GOLEM_DEPLOYMENT_CORE_ID ||
        abi.boot_task_count != 1 ||
        abi.dispatch_tasks.task_count != 1) {
        uart_puts("[eight-layer-4x2] ERROR: invalid generated tile ABI\n");
        return 1;
    }
    if (!runtime.boot()) {
        uart_puts("[eight-layer-4x2] ERROR: matrix setup failed\n");
        return 2;
    }

#if GOLEM_DEPLOYMENT_CORE_ID == 0
    if (abi.model_input_count != 1 ||
        abi.model_output_count != 0 ||
        abi.incoming_route_count != 0 ||
        abi.outgoing_route_count != 1 ||
        !waitForWorkersReady()) {
        uart_puts("[core 0] ERROR: invalid deployment or READY barrier\n");
        return 3;
    }
    uart_puts("[core 0] all eight tiles ready; dispatching model input\n");
#else
    if (abi.model_input_count != 0 ||
        abi.incoming_route_count != 1) {
        uart_puts("[eight-layer-4x2 worker] ERROR: invalid incoming deployment\n");
        return 3;
    }
    mesh_nic::send(
        0,
        kReadyBase | static_cast<uint32_t>(GOLEM_DEPLOYMENT_CORE_ID)
    );
#endif

    alignas(64) float input[kVectorLength]{};
    alignas(64) float output[kVectorLength]{};

#if GOLEM_DEPLOYMENT_CORE_ID == 0
    constexpr float kModelInput[kVectorLength] = {
        1.0F,
        2.0F,
        3.0F,
        4.0F,
    };
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        input[index] = kModelInput[index];
    }
#else
    if (!runtime.receiveRoute(
            abi.incoming_routes[0].id,
            input,
            sizeof(input)
        )) {
        uart_puts("[eight-layer-4x2 worker] ERROR: route reception failed\n");
        return 4;
    }
#endif

    if (!executeLocalTask(runtime, abi, input, output)) {
        uart_puts("[eight-layer-4x2] ERROR: compiled layer result mismatch\n");
        return 5;
    }

#if GOLEM_DEPLOYMENT_CORE_ID == 4
    if (abi.model_output_count != 1 ||
        abi.outgoing_route_count != 0) {
        uart_puts("[core 4] ERROR: invalid terminal deployment\n");
        return 6;
    }
    uart_puts("[core 4] final model output [56,42,42,60]\n");
    mesh_nic::send(0, kDoneWord);
#else
    if (abi.model_output_count != 0 ||
        abi.outgoing_route_count != 1 ||
        !runtime.sendRoute(
            abi.outgoing_routes[0].id,
            output,
            sizeof(output)
        )) {
        uart_puts("[eight-layer-4x2] ERROR: route transmission failed\n");
        return 6;
    }
#endif

#if GOLEM_DEPLOYMENT_CORE_ID == 0
    if (mesh_nic::receive() != kDoneWord) {
        uart_puts("[core 0] ERROR: invalid DONE signal\n");
        return 7;
    }
    uart_puts("SCULPTOR_EIGHT_LAYER_4X2_PASS\n");
#endif

    return 0;
}
