#include <stdint.h>

#include "golem/runtime/runtime.h"
#include "mesh-nic.h"
#include "platform.h"

#ifndef MITTENS_TILE_ID
#error "MITTENS_TILE_ID must identify this tile image"
#endif

namespace {

using namespace golem::runtime;

constexpr uint32_t kTileCount = 9;
constexpr uint32_t kTaskCount = 13;
constexpr uint32_t kVectorLength = 4;
constexpr ExecutionId kExecutionId = 0;

static_assert(MITTENS_TILE_ID >= 0);
static_assert(MITTENS_TILE_ID < kTileCount);
static_assert(sizeof(float) == sizeof(uint32_t));

struct VectorMemRef {
    float* allocated;
    float* aligned;
    int64_t offset;
    int64_t size;
    int64_t stride;
};

struct Matrix {
    float elements[kVectorLength * kVectorLength];
};

template <uint32_t TaskId>
constexpr Matrix makeMatrix() {
    static_assert(TaskId < kTaskCount);

    Matrix matrix{};
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        matrix.elements[index * kVectorLength + index] = 1.0F;
    }

    constexpr uint32_t row = TaskId % kVectorLength;
    constexpr uint32_t column = (TaskId + 1U) % kVectorLength;
    constexpr uint32_t coefficient = 1U + TaskId / kVectorLength;
    matrix.elements[row * kVectorLength + column] =
        static_cast<float>(coefficient);
    return matrix;
}

template <uint32_t TaskId>
TaskStatus executeMatvec(
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
        inputs[0].rank != 1 || outputs[0].rank != 1) {
        return TaskStatus::Failure;
    }

    const auto* input =
        static_cast<const VectorMemRef*>(inputs[0].descriptor);
    auto* output = static_cast<VectorMemRef*>(outputs[0].descriptor);
    if (input->size != kVectorLength ||
        output->size != kVectorLength ||
        input->stride != 1 ||
        output->stride != 1) {
        return TaskStatus::Failure;
    }

    constexpr Matrix matrix = makeMatrix<TaskId>();
    for (uint32_t row = 0; row < kVectorLength; ++row) {
        float sum = 0.0F;
        for (uint32_t column = 0; column < kVectorLength; ++column) {
            const float value =
                input->aligned[
                    input->offset +
                    static_cast<int64_t>(column) * input->stride
                ];
            sum += matrix.elements[row * kVectorLength + column] * value;
        }
        output->aligned[
            output->offset + static_cast<int64_t>(row) * output->stride
        ] = sum;
    }

    return TaskStatus::Success;
}

#if defined(MITTENS_REVERSE_SECOND_PASS)
#if MITTENS_TILE_ID == 0
constexpr Task kTasks[] = {{0, executeMatvec<0>, 1, 1}};
#elif MITTENS_TILE_ID == 1
constexpr Task kTasks[] = {{1, executeMatvec<1>, 1, 1}};
#elif MITTENS_TILE_ID == 2
constexpr Task kTasks[] = {{2, executeMatvec<2>, 1, 1}};
#elif MITTENS_TILE_ID == 3
constexpr Task kTasks[] = {{3, executeMatvec<3>, 1, 1}};
#elif MITTENS_TILE_ID == 4
constexpr Task kTasks[] = {{4, executeMatvec<4>, 1, 1}};
#elif MITTENS_TILE_ID == 5
constexpr Task kTasks[] = {
    {5, executeMatvec<5>, 1, 1},
    {12, executeMatvec<12>, 1, 1},
};
#elif MITTENS_TILE_ID == 6
constexpr Task kTasks[] = {
    {6, executeMatvec<6>, 1, 1},
    {11, executeMatvec<11>, 1, 1},
};
#elif MITTENS_TILE_ID == 7
constexpr Task kTasks[] = {
    {7, executeMatvec<7>, 1, 1},
    {10, executeMatvec<10>, 1, 1},
};
#elif MITTENS_TILE_ID == 8
constexpr Task kTasks[] = {
    {8, executeMatvec<8>, 1, 1},
    {9, executeMatvec<9>, 1, 1},
};
#endif
#else
#if MITTENS_TILE_ID == 0
constexpr Task kTasks[] = {
    {0, executeMatvec<0>, 1, 1},
    {9, executeMatvec<9>, 1, 1},
};
#elif MITTENS_TILE_ID == 1
constexpr Task kTasks[] = {
    {1, executeMatvec<1>, 1, 1},
    {10, executeMatvec<10>, 1, 1},
};
#elif MITTENS_TILE_ID == 2
constexpr Task kTasks[] = {
    {2, executeMatvec<2>, 1, 1},
    {11, executeMatvec<11>, 1, 1},
};
#elif MITTENS_TILE_ID == 3
constexpr Task kTasks[] = {
    {3, executeMatvec<3>, 1, 1},
    {12, executeMatvec<12>, 1, 1},
};
#elif MITTENS_TILE_ID == 4
constexpr Task kTasks[] = {{4, executeMatvec<4>, 1, 1}};
#elif MITTENS_TILE_ID == 5
constexpr Task kTasks[] = {{5, executeMatvec<5>, 1, 1}};
#elif MITTENS_TILE_ID == 6
constexpr Task kTasks[] = {{6, executeMatvec<6>, 1, 1}};
#elif MITTENS_TILE_ID == 7
constexpr Task kTasks[] = {{7, executeMatvec<7>, 1, 1}};
#elif MITTENS_TILE_ID == 8
constexpr Task kTasks[] = {{8, executeMatvec<8>, 1, 1}};
#endif
#endif

constexpr uint32_t kLocalTaskCount =
    sizeof(kTasks) / sizeof(kTasks[0]);
const TaskRegistry kRegistry{kTasks, kLocalTaskCount};

constexpr uint32_t tileForTask(uint32_t task_id) {
    if (task_id < kTileCount) {
        return task_id;
    }
#if defined(MITTENS_REVERSE_SECOND_PASS)
    return 17U - task_id;
#else
    return task_id - 9U;
#endif
}

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

void sendWord(uint32_t destination_tile, uint32_t word) {
    while (!kTransport.trySend(destination_tile, word)) {
    }
}

uint32_t receiveWord() {
    uint32_t word = 0;
    while (!kTransport.tryReceive(&word)) {
    }
    return word;
}

void sendVector(uint32_t destination_tile, const float* vector) {
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        sendWord(
            destination_tile,
            __builtin_bit_cast(uint32_t, vector[index])
        );
    }
}

void receiveVector(float* vector) {
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        vector[index] = __builtin_bit_cast(float, receiveWord());
    }
}

void uartPutUnsigned(uint32_t value) {
    char digits[10];
    uint32_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10U);
        value /= 10U;
    } while (value != 0);

    while (count != 0) {
        uart_putc(digits[--count]);
    }
}

void uartPutVector(const float* vector) {
    uart_putc('[');
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        if (index != 0) {
            uart_putc(',');
        }
        uartPutUnsigned(static_cast<uint32_t>(vector[index]));
    }
    uart_putc(']');
}

bool executeTask(
    const Task& task,
    const float* input_values,
    float* output_values,
    TaskInstancePool* pool,
    ReadyQueue* ready
) {
    if (task.input_count != 1 || task.output_count != 1) {
        uart_puts("[matvec] ERROR: task is not registered on this tile\n");
        return false;
    }

    float input_storage[kVectorLength]{};
    float output_storage[kVectorLength]{};
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        input_storage[index] = input_values[index];
    }

    VectorMemRef input_descriptor{
        input_storage,
        input_storage,
        0,
        kVectorLength,
        1,
    };
    VectorMemRef output_descriptor{
        output_storage,
        output_storage,
        0,
        kVectorLength,
        1,
    };
    Tensor inputs[] = {
        {ElementType::Float32, 1, &input_descriptor},
    };
    Tensor outputs[] = {
        {ElementType::Float32, 1, &output_descriptor},
    };

    TaskInstance* instance = pool->acquire(
        kExecutionId,
        task.id,
        inputs,
        outputs
    );
    if (instance == nullptr) {
        uart_puts("[matvec] ERROR: no task-instance slot\n");
        return false;
    }

    instance->received_input_count = 1;
    instance->state = TaskInstanceState::Ready;
    const uint32_t instance_index = pool->indexOf(instance);
    if (!ready->push(instance_index)) {
        uart_puts("[matvec] ERROR: ready queue is full\n");
        return false;
    }

    uint32_t ready_index = kTaskInstanceCapacity;
    if (!ready->pop(&ready_index) || ready_index != instance_index) {
        uart_puts("[matvec] ERROR: invalid ready-queue entry\n");
        return false;
    }

    instance->state = TaskInstanceState::Running;
    if (task.execute(
            instance->inputs,
            task.input_count,
            instance->outputs,
            task.output_count
        ) != TaskStatus::Success) {
        instance->state = TaskInstanceState::Failed;
        uart_puts("[matvec] ERROR: task execution failed\n");
        return false;
    }
    instance->state = TaskInstanceState::ForwardingOutputs;

    uart_puts("[tile ");
    uartPutUnsigned(MITTENS_TILE_ID);
    uart_puts("] executed task ");
    uartPutUnsigned(task.id);
    uart_puts(": ");
    uartPutVector(input_storage);
    uart_puts(" -> ");
    uartPutVector(output_storage);
    uart_putc('\n');

    for (uint32_t index = 0; index < kVectorLength; ++index) {
        output_values[index] = output_storage[index];
    }

    instance->state = TaskInstanceState::Complete;
    return pool->release(instance);
}

[[maybe_unused]] bool finalVectorMatches(const float* vector) {
    constexpr float expected[kVectorLength] = {
        398.0F,
        82.0F,
        120.0F,
        243.0F,
    };
    for (uint32_t index = 0; index < kVectorLength; ++index) {
        if (vector[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

}  // namespace

extern "C" int tile_main() {
    if (!kRegistry.valid() || !kTransport.valid()) {
        uart_puts("[matvec] ERROR: invalid tile runtime configuration\n");
        return 1;
    }

    TaskInstancePool pool;
    ReadyQueue ready;
    pool.initialize();
    ready.initialize();

    float current_vector[kVectorLength]{};
    bool have_local_vector = false;
    if constexpr (MITTENS_TILE_ID == 0) {
        constexpr float initial[kVectorLength] = {
            1.0F,
            2.0F,
            3.0F,
            4.0F,
        };
        for (uint32_t index = 0; index < kVectorLength; ++index) {
            current_vector[index] = initial[index];
        }
        have_local_vector = true;
    }

    uint32_t next_local_task = 0;
    while (next_local_task < kLocalTaskCount) {
        if (!have_local_vector) {
            receiveVector(current_vector);
        }

        float output_vector[kVectorLength]{};
        if (!executeTask(
                kTasks[next_local_task],
                current_vector,
                output_vector,
                &pool,
                &ready
            )) {
            return 1;
        }

        const Task& completed_task = kTasks[next_local_task];
        ++next_local_task;

        const bool terminal = completed_task.id + 1U == kTaskCount;
        const uint32_t next_tile =
            terminal ? 0U : tileForTask(completed_task.id + 1U);
        const bool next_task_is_local =
            !terminal &&
            next_local_task < kLocalTaskCount &&
            kTasks[next_local_task].id == completed_task.id + 1U &&
            next_tile == MITTENS_TILE_ID;

        if (next_task_is_local) {
            for (uint32_t index = 0; index < kVectorLength; ++index) {
                current_vector[index] = output_vector[index];
            }
            have_local_vector = true;
        } else {
            sendVector(next_tile, output_vector);
            have_local_vector = false;
        }
    }

    if constexpr (MITTENS_TILE_ID == 0) {
        float final_vector[kVectorLength]{};
        receiveVector(final_vector);
        if (!finalVectorMatches(final_vector)) {
            uart_puts("[matvec] ERROR: final vector mismatch\n");
            return 1;
        }

        uart_puts(
            "[tile 0] all 13 tasks complete; final vector "
            "[398,82,120,243]\n"
        );
    }

    return 0;
}
