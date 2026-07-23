#include <array>
#include <cstdint>
#include <cstdio>

#include "golem/runtime/runtime.h"

namespace {

using namespace golem::runtime;

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(                                                       \
                stderr,                                                         \
                "%s:%d: check failed: %s\n",                                   \
                __FILE__,                                                       \
                __LINE__,                                                       \
                #condition                                                      \
            );                                                                  \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

TaskStatus executeNoop(
    const Tensor*,
    uint32_t,
    Tensor*,
    uint32_t
) {
    return TaskStatus::Success;
}

void testTensor() {
    float scalar = 1.0F;
    Tensor tensor{ElementType::Float32, 0, &scalar};

    CHECK(elementSizeBytes(ElementType::Int8) == 1);
    CHECK(elementSizeBytes(ElementType::UInt16) == 2);
    CHECK(elementSizeBytes(ElementType::Float32) == 4);
    CHECK(elementSizeBytes(ElementType::Invalid) == 0);
    CHECK(validTensor(tensor));

    tensor.rank = 3;
    CHECK(!validTensor(tensor));
    tensor.rank = 0;
    tensor.descriptor = nullptr;
    CHECK(!validTensor(tensor));
}

void testTaskRegistry() {
    const Task tasks[] = {
        {2, executeNoop, 1, 1},
        {7, executeNoop, 2, 1},
        {42, executeNoop, 1, 2},
    };
    const TaskRegistry registry{tasks, 3};

    CHECK(registry.valid());
    CHECK(registry.find(2) == &tasks[0]);
    CHECK(registry.find(7) == &tasks[1]);
    CHECK(registry.find(42) == &tasks[2]);
    CHECK(registry.find(0) == nullptr);
    CHECK(registry.find(8) == nullptr);

    const Task duplicate[] = {
        {2, executeNoop, 1, 1},
        {2, executeNoop, 1, 1},
    };
    CHECK(!(TaskRegistry{duplicate, 2}.valid()));

    const Task unsorted[] = {
        {9, executeNoop, 1, 1},
        {3, executeNoop, 1, 1},
    };
    CHECK(!(TaskRegistry{unsorted, 2}.valid()));

    const Task no_execute[] = {{1, nullptr, 0, 0}};
    CHECK(!(TaskRegistry{no_execute, 1}.valid()));
    CHECK(!(TaskRegistry{nullptr, 1}.valid()));
    CHECK((TaskRegistry{nullptr, 1}.find(1) == nullptr));
    CHECK((TaskRegistry{nullptr, 0}.valid()));
}

void testTaskInstancePool() {
    TaskInstancePool pool;
    pool.initialize();

    std::array<Tensor, kTaskInstanceCapacity> inputs{};
    std::array<Tensor, kTaskInstanceCapacity> outputs{};
    std::array<TaskInstance*, kTaskInstanceCapacity> instances{};

    for (uint32_t index = 0; index < kTaskInstanceCapacity; ++index) {
        CHECK(pool.get(index) != nullptr);
        CHECK(pool.get(index)->state == TaskInstanceState::Free);
        instances[index] = pool.acquire(
            static_cast<ExecutionId>(index / 2U),
            100U + index,
            &inputs[index],
            &outputs[index]
        );
        CHECK(instances[index] != nullptr);
        CHECK(pool.indexOf(instances[index]) == index);
        CHECK(instances[index]->state == TaskInstanceState::WaitingForInputs);
    }

    CHECK(pool.get(kTaskInstanceCapacity) == nullptr);
    CHECK(pool.acquire(99, 99, nullptr, nullptr) == nullptr);
    CHECK(pool.acquire(0, 100, nullptr, nullptr) == nullptr);
    CHECK(pool.find(3, 106) == instances[6]);
    CHECK(pool.find(100, 106) == nullptr);
    CHECK(!pool.release(instances[0]));

    instances[0]->state = TaskInstanceState::Complete;
    CHECK(pool.release(instances[0]));
    CHECK(instances[0]->state == TaskInstanceState::Free);
    CHECK(pool.find(0, 100) == nullptr);

    TaskInstance* replacement = pool.acquire(77, 900, nullptr, nullptr);
    CHECK(replacement == instances[0]);
    replacement->state = TaskInstanceState::Failed;
    CHECK(pool.release(replacement));
    CHECK(!pool.release(nullptr));
}

void testReadyQueue() {
    ReadyQueue queue;
    queue.initialize();

    CHECK(queue.empty());
    CHECK(!queue.full());
    CHECK(queue.size() == 0);

    uint32_t value = 0;
    CHECK(!queue.pop(&value));
    CHECK(!queue.pop(nullptr));
    CHECK(!queue.push(kTaskInstanceCapacity));

    for (uint32_t index = 0; index < kTaskInstanceCapacity; ++index) {
        CHECK(queue.push(index));
    }
    CHECK(queue.full());
    CHECK(queue.size() == kTaskInstanceCapacity);
    CHECK(!queue.push(0));

    for (uint32_t index = 0; index < 8; ++index) {
        CHECK(queue.pop(&value));
        CHECK(value == index);
    }
    for (uint32_t index = 0; index < 8; ++index) {
        CHECK(queue.push(index));
    }
    for (uint32_t index = 8; index < kTaskInstanceCapacity; ++index) {
        CHECK(queue.pop(&value));
        CHECK(value == index);
    }
    for (uint32_t index = 0; index < 8; ++index) {
        CHECK(queue.pop(&value));
        CHECK(value == index);
    }
    CHECK(queue.empty());
}

struct FakeTransport {
    uint32_t destination;
    uint32_t sent_word;
    uint32_t received_word;
    bool accept_send;
    bool have_receive;
};

bool fakeSend(void* context, uint32_t destination, uint32_t word) {
    auto* fake = static_cast<FakeTransport*>(context);
    if (!fake->accept_send) {
        return false;
    }
    fake->destination = destination;
    fake->sent_word = word;
    return true;
}

bool fakeReceive(void* context, uint32_t* word) {
    auto* fake = static_cast<FakeTransport*>(context);
    if (!fake->have_receive) {
        return false;
    }
    *word = fake->received_word;
    fake->have_receive = false;
    return true;
}

void testTransport() {
    FakeTransport fake{0, 0, UINT32_C(0xdeadbeef), false, true};
    WordTransport transport{&fake, fakeSend, fakeReceive};

    CHECK(transport.valid());
    CHECK(!transport.trySend(8, 9));
    fake.accept_send = true;
    CHECK(transport.trySend(8, 9));
    CHECK(fake.destination == 8);
    CHECK(fake.sent_word == 9);

    uint32_t word = 0;
    CHECK(transport.tryReceive(&word));
    CHECK(word == UINT32_C(0xdeadbeef));
    CHECK(!transport.tryReceive(&word));
    CHECK(!transport.tryReceive(nullptr));

    const WordTransport invalid{nullptr, nullptr, nullptr};
    CHECK(!invalid.valid());
    CHECK(!invalid.trySend(0, 0));
    CHECK(!invalid.tryReceive(&word));
}

}  // namespace

int main() {
    testTensor();
    testTaskRegistry();
    testTaskInstancePool();
    testReadyQueue();
    testTransport();

    if (failures != 0) {
        std::fprintf(stderr, "%d runtime checks failed\n", failures);
        return 1;
    }

    std::puts("runtime unit tests: PASS");
    return 0;
}
