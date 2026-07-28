#include <stdint.h>

#include "golem/runtime/runtime.h"
#include "mesh-nic.h"
#include "platform.h"

#ifndef GOLEM_DEPLOYMENT_CORE_ID
#error "GOLEM_DEPLOYMENT_CORE_ID must select the isolated core"
#endif

static_assert(
    GOLEM_DEPLOYMENT_CORE_ID == 0 || GOLEM_DEPLOYMENT_CORE_ID == 1
);

namespace {

using namespace golem::runtime;

constexpr uint32_t kRouteId = 0;
constexpr uint32_t kReadyWord = UINT32_C(0x52454144);
constexpr uint32_t kDoneWord = UINT32_C(0x444f4e45);
constexpr float kTolerance = 1.0e-4F;

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

bool close(float actual, float expected) {
    const float difference = actual - expected;
    return difference == difference &&
           difference >= -kTolerance &&
           difference <= kTolerance;
}

[[maybe_unused]] void sendControl(uint32_t destination, uint32_t word) {
    mesh_nic::send(destination, word);
}

[[maybe_unused]] bool receiveControl(uint32_t expected) {
    return mesh_nic::receive() == expected;
}

const WordTransport kTransport{
    nullptr,
    trySendWord,
    tryReceiveWord,
};

}  // namespace

extern "C" int tile_main() {
#if GOLEM_DEPLOYMENT_CORE_ID == 0
    uart_puts("[core 0] runtime entered\n");
#else
    uart_puts("[core 1] runtime entered\n");
#endif
    const TileABI abi = linkedTileABI();
    BasicTileRuntime runtime{abi, kTransport};

    if (!runtime.valid()) {
        uart_puts("[sculptor runtime] ERROR: invalid generated tile ABI\n");
        return 1;
    }
    if (abi.core_id != GOLEM_DEPLOYMENT_CORE_ID) {
        uart_puts("[sculptor runtime] ERROR: wrong generated core ID\n");
        return 2;
    }
#if GOLEM_DEPLOYMENT_CORE_ID == 0
    uart_puts("[core 0] generated ABI valid; running boot table\n");
#else
    uart_puts("[core 1] generated ABI valid; running boot table\n");
#endif
    if (!runtime.boot()) {
        uart_puts("[sculptor runtime] ERROR: matrix setup failed\n");
        return 3;
    }

#if GOLEM_DEPLOYMENT_CORE_ID == 0
    if (abi.boot_task_count != 1 ||
        abi.dispatch_tasks.task_count != 1 ||
        abi.model_input_count != 1 ||
        abi.incoming_route_count != 0 ||
        abi.outgoing_route_count != 1) {
        uart_puts("[core 0] ERROR: unexpected generated tables\n");
        return 4;
    }

    // Do not inject the model input until core 1 has installed its own matrix.
    if (!receiveControl(kReadyWord)) {
        uart_puts("[core 0] ERROR: invalid READY signal\n");
        return 5;
    }

    alignas(64) float input_storage[4] = {
        1.0F,
        2.0F,
        3.0F,
        4.0F,
    };
    alignas(64) float activation_storage[3]{};
    RankedMemRef2DF32 input_descriptor{
        input_storage,
        input_storage,
        0,
        {1, 4},
        {4, 1},
    };
    RankedMemRef2DF32 activation_descriptor{
        activation_storage,
        activation_storage,
        0,
        {1, 3},
        {3, 1},
    };
    Tensor inputs[] = {
        {ElementType::Float32, 2, &input_descriptor},
    };
    Tensor outputs[] = {
        {ElementType::Float32, 2, &activation_descriptor},
    };

    const uint32_t task_id = abi.dispatch_tasks.tasks[0].id;
    if (!runtime.execute(task_id, inputs, 1, outputs, 1) ||
        !close(activation_storage[0], 2.0F) ||
        !close(activation_storage[1], 4.0F) ||
        !close(activation_storage[2], 6.0F)) {
        uart_puts("[core 0] ERROR: first compiled layer failed\n");
        return 6;
    }
    if (!runtime.sendRoute(
            kRouteId,
            activation_storage,
            sizeof(activation_storage)
        )) {
        uart_puts("[core 0] ERROR: route transmission failed\n");
        return 7;
    }

    uart_puts("[core 0] task 1 produced [2,4,6] and routed 3 words\n");
    if (!receiveControl(kDoneWord)) {
        uart_puts("[core 0] ERROR: invalid DONE signal\n");
        return 8;
    }
    uart_puts("SCULPTOR_BASIC_RUNTIME_CORE_0_PASS\n");
#else
    if (abi.boot_task_count != 1 ||
        abi.dispatch_tasks.task_count != 1 ||
        abi.model_output_count != 1 ||
        abi.incoming_route_count != 1 ||
        abi.outgoing_route_count != 0) {
        uart_puts("[core 1] ERROR: unexpected generated tables\n");
        return 4;
    }

    sendControl(0, kReadyWord);

    alignas(64) float activation_storage[3]{};
    alignas(64) float output_storage[2]{};
    if (!runtime.receiveRoute(
            kRouteId,
            activation_storage,
            sizeof(activation_storage)
        )) {
        uart_puts("[core 1] ERROR: route reception failed\n");
        return 5;
    }

    RankedMemRef2DF32 activation_descriptor{
        activation_storage,
        activation_storage,
        0,
        {1, 3},
        {3, 1},
    };
    RankedMemRef2DF32 output_descriptor{
        output_storage,
        output_storage,
        0,
        {1, 2},
        {2, 1},
    };
    Tensor inputs[] = {
        {ElementType::Float32, 2, &activation_descriptor},
    };
    Tensor outputs[] = {
        {ElementType::Float32, 2, &output_descriptor},
    };

    const uint32_t task_id = abi.dispatch_tasks.tasks[0].id;
    if (!runtime.execute(task_id, inputs, 1, outputs, 1) ||
        !close(output_storage[0], 12.0F) ||
        !close(output_storage[1], 4.0F)) {
        uart_puts("[core 1] ERROR: second compiled layer failed\n");
        return 6;
    }

    uart_puts("[core 1] task 3 produced model output [12,4]\n");
    uart_puts("SCULPTOR_BASIC_RUNTIME_CORE_1_PASS\n");
    sendControl(0, kDoneWord);
#endif

    return 0;
}
