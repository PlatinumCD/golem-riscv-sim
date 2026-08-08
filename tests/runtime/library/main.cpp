#include <stdint.h>

#include "golem/runtime/runtime.h"
#include "platform.h"

namespace {

using namespace golem::runtime;

struct ScalarMemRef {
    float* allocated;
    float* aligned;
    int64_t offset;
};

TaskStatus addOne(
    const Tensor* inputs,
    uint32_t input_count,
    Tensor* outputs,
    uint32_t output_count
) {
    if (inputs == nullptr || outputs == nullptr ||
        input_count != 1 || output_count != 1 ||
        !validTensor(inputs[0]) || !validTensor(outputs[0]) ||
        inputs[0].element_type != ElementType::Float32 ||
        outputs[0].element_type != ElementType::Float32 ||
        inputs[0].rank != 0 || outputs[0].rank != 0) {
        return TaskStatus::Failure;
    }

    const auto* input =
        static_cast<const ScalarMemRef*>(inputs[0].descriptor);
    auto* output = static_cast<ScalarMemRef*>(outputs[0].descriptor);
    output->aligned[output->offset] =
        input->aligned[input->offset] + 1.0F;
    return TaskStatus::Success;
}

bool runProof() {
    const Task tasks[] = {{11, addOne, 1, 1}};
    const TaskRegistry registry{tasks, 1};
    if (!registry.valid() || registry.find(11) == nullptr ||
        registry.find(12) != nullptr) {
        return false;
    }

    float input_value = 4.5F;
    float output_value = 0.0F;
    ScalarMemRef input_descriptor{
        &input_value,
        &input_value,
        0,
    };
    ScalarMemRef output_descriptor{
        &output_value,
        &output_value,
        0,
    };
    Tensor inputs[] = {
        {ElementType::Float32, 0, &input_descriptor},
    };
    Tensor outputs[] = {
        {ElementType::Float32, 0, &output_descriptor},
    };

    TaskInstancePool pool;
    pool.initialize();
    TaskInstance* instance = pool.acquire(
        0,
        tasks[0].id,
        inputs,
        outputs
    );
    if (instance == nullptr) {
        return false;
    }

    instance->received_input_count = 1;
    instance->state = TaskInstanceState::Ready;

    ReadyQueue ready;
    ready.initialize();
    if (!ready.push(pool.indexOf(instance))) {
        return false;
    }

    uint32_t instance_index = kTaskInstanceCapacity;
    if (!ready.pop(&instance_index) ||
        pool.get(instance_index) != instance) {
        return false;
    }

    instance->state = TaskInstanceState::Running;
    if (tasks[0].execute(
            instance->inputs,
            tasks[0].input_count,
            instance->outputs,
            tasks[0].output_count
        ) != TaskStatus::Success ||
        output_value != 5.5F) {
        return false;
    }

    instance->state = TaskInstanceState::Complete;
    return pool.release(instance) && ready.empty();
}

}  // namespace

extern "C" int tile_main() {
    if (!runProof()) {
        uart_puts("Golem runtime library: FAIL\n");
        return 1;
    }

    uart_puts("Golem runtime library: PASS\n");
    return 0;
}
