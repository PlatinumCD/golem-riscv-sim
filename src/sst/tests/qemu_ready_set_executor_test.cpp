#include "../execution/qemuReadySetExecutor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

using SST::Mittens::QemuReadySetExecutionError;
using SST::Mittens::QemuReadySetExecutor;
using SST::Mittens::QemuAsyncCaptureExecutor;
using SST::Mittens::QemuSyncEvent;
using SST::Mittens::SharedSyncMemoryBridge;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QemuSyncEvent makeEvent(
    std::uint64_t epoch,
    std::uint64_t sequence,
    std::uint64_t instructions)
{
    QemuSyncEvent event{};
    event.grantEpoch = epoch;
    event.eventSequence = sequence;
    event.instructionsExecuted = instructions;
    event.vectorInstructionsExecuted = instructions / 4;
    event.stopReason = MITTENS_SYNC_STOP_QUANTUM_END;
    return event;
}

void wakeFutex(std::uint32_t* state)
{
    (void)syscall(
        SYS_futex,
        state,
        FUTEX_WAKE,
        std::numeric_limits<int>::max(),
        nullptr,
        nullptr,
        0);
}

void waitWhile(std::uint32_t* state, std::uint32_t expected)
{
    while (mittens_sync_load_acquire(state) == expected) {
        (void)syscall(
            SYS_futex,
            state,
            FUTEX_WAIT,
            expected,
            nullptr,
            nullptr,
            0);
    }
}

struct TaskSpec {
    std::uint32_t tile;
    std::uint64_t sequence;
    std::uint64_t instructions;
    std::uint64_t deliveryOffset;
    std::chrono::milliseconds hostDelay;
};

std::vector<QemuReadySetExecutor::Completion> runBatch(
    std::size_t workers,
    const std::vector<TaskSpec>& specs,
    std::vector<std::uint32_t>* commits)
{
    constexpr std::uint64_t frontier = 1000;
    const std::thread::id owner = std::this_thread::get_id();
    QemuReadySetExecutor executor(workers);
    executor.begin(frontier, specs.size());
    for (const TaskSpec& spec : specs) {
        executor.submit({
            frontier,
            spec.tile,
            1,
            [spec, owner]() {
                require(
                    std::this_thread::get_id() != owner,
                    "capture ran on the owner thread");
                std::this_thread::sleep_for(spec.hostDelay);
                return makeEvent(
                    1, spec.sequence, spec.instructions);
            },
            [owner](const QemuSyncEvent&) {
                require(
                    std::this_thread::get_id() == owner,
                    "validation ran on a worker thread");
            },
            [spec](const QemuSyncEvent&) {
                return frontier + spec.deliveryOffset;
            },
            [owner, commits, tile = spec.tile](const QemuSyncEvent&) {
                require(
                    std::this_thread::get_id() == owner,
                    "commit ran on a worker thread");
                commits->push_back(tile);
            },
        });
    }

    auto completions = executor.collect();
    for (const auto& completion : completions) {
        completion.commit(completion.event);
    }
    return completions;
}

void testOneWorkerIdentity()
{
    const std::vector<TaskSpec> specs{
        {7, 4, 400, 40, std::chrono::milliseconds(0)},
        {2, 3, 300, 10, std::chrono::milliseconds(0)},
        {5, 2, 200, 10, std::chrono::milliseconds(0)},
        {1, 1, 100, 30, std::chrono::milliseconds(0)},
    };
    std::vector<std::uint32_t> serialCommits;
    std::vector<std::uint32_t> parallelCommits;
    const auto serial = runBatch(1, specs, &serialCommits);
    const auto parallel = runBatch(3, specs, &parallelCommits);

    require(serial.size() == specs.size(), "serial result size changed");
    require(parallel.size() == serial.size(), "parallel result size changed");
    for (std::size_t index = 0; index < serial.size(); ++index) {
        const auto serialKey = std::make_tuple(
            serial[index].modeledDeliveryTick,
            serial[index].tileId,
            serial[index].grantEpoch,
            serial[index].eventSequence,
            serial[index].event.instructionsExecuted);
        const auto parallelKey = std::make_tuple(
            parallel[index].modeledDeliveryTick,
            parallel[index].tileId,
            parallel[index].grantEpoch,
            parallel[index].eventSequence,
            parallel[index].event.instructionsExecuted);
        require(serialKey == parallelKey,
                "one-worker and multi-worker results differ");
    }
    const std::vector<std::uint32_t> expected{2, 5, 1, 7};
    require(serialCommits == expected,
            "one-worker commit order is not the modeled reference");
    require(parallelCommits == expected,
            "parallel commit order differs from the reference");
}

void testSerialTieOrderPreserved()
{
    const std::vector<TaskSpec> specs{
        {9, 1, 100, 10, std::chrono::milliseconds(4)},
        {2, 2, 100, 10, std::chrono::milliseconds(0)},
        {7, 3, 100, 10, std::chrono::milliseconds(2)},
    };
    std::vector<std::uint32_t> serialCommits;
    std::vector<std::uint32_t> parallelCommits;
    const auto serial = runBatch(1, specs, &serialCommits);
    const auto parallel = runBatch(3, specs, &parallelCommits);
    const std::vector<std::uint32_t> expected{9, 2, 7};
    require(serialCommits == expected,
            "one-worker equal-tick order changed from submission order");
    require(parallelCommits == expected,
            "host completion order changed an equal-tick serial tie");
    require(serial.size() == parallel.size(),
            "equal-tick identity result size changed");
    for (std::size_t index = 0; index < serial.size(); ++index) {
        require(
            serial[index].submissionSequence ==
                parallel[index].submissionSequence &&
            serial[index].tileId == parallel[index].tileId,
            "one-worker and multi-worker equal-tick identities differ");
    }
}

void testMultiWorkerConcurrency()
{
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        std::size_t active = 0;
        std::size_t maximum = 0;
        bool release = false;
    } gate;

    QemuReadySetExecutor executor(2);
    executor.begin(0, 6);
    for (std::uint32_t tile = 0; tile < 6; ++tile) {
        executor.submit({
            0,
            tile,
            1,
            [&gate, tile]() {
                std::unique_lock<std::mutex> lock(gate.mutex);
                ++gate.active;
                gate.maximum = std::max(gate.maximum, gate.active);
                if (gate.active == 2) {
                    gate.release = true;
                    gate.changed.notify_all();
                }
                gate.changed.wait_for(
                    lock,
                    std::chrono::seconds(2),
                    [&gate]() { return gate.release; });
                lock.unlock();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
                lock.lock();
                --gate.active;
                lock.unlock();
                return makeEvent(1, tile + 1, 10);
            },
            [](const QemuSyncEvent&) {},
            [](const QemuSyncEvent&) { return UINT64_C(10); },
            [](const QemuSyncEvent&) {},
        });
    }
    const auto completions = executor.collect();
    require(completions.size() == 6, "concurrency batch lost a task");
    require(gate.maximum == 2,
            "two-worker batch was not concurrent and bounded at two");
}

void testRepeatedDeterministicOrdering()
{
    QemuReadySetExecutor executor(4);
    std::vector<std::tuple<std::uint64_t, std::uint32_t,
                           std::uint64_t, std::uint64_t>> reference;

    for (std::uint32_t round = 0; round < 24; ++round) {
        constexpr std::uint64_t frontier = 700;
        executor.begin(frontier, 8);
        for (std::uint32_t tile = 0; tile < 8; ++tile) {
            executor.submit({
                frontier,
                tile,
                11,
                [round, tile]() {
                    const auto delay = std::chrono::milliseconds(
                        (round * 5 + tile * 7) % 4);
                    std::this_thread::sleep_for(delay);
                    return makeEvent(11, 100 - tile, 50 + tile);
                },
                [](const QemuSyncEvent&) {},
                [tile](const QemuSyncEvent&) {
                    return UINT64_C(700) + (tile % 3);
                },
                [](const QemuSyncEvent&) {},
            });
        }
        const auto completions = executor.collect();
        std::vector<std::tuple<std::uint64_t, std::uint32_t,
                               std::uint64_t, std::uint64_t>> observed;
        for (const auto& completion : completions) {
            observed.emplace_back(
                completion.modeledDeliveryTick,
                completion.tileId,
                completion.grantEpoch,
                completion.eventSequence);
        }
        if (round == 0) {
            reference = observed;
        } else {
            require(observed == reference,
                    "host completion order changed deterministic output");
        }
    }
}

void testFailClosedAndRecover()
{
    QemuReadySetExecutor executor(3);
    std::vector<std::uint32_t> commits;
    executor.begin(10, 3);
    for (const std::uint32_t tile : {9U, 2U, 5U}) {
        executor.submit({
            10,
            tile,
            3,
            [tile]() -> QemuSyncEvent {
                if (tile == 9 || tile == 2) {
                    throw std::runtime_error(
                        "synthetic bridge failure");
                }
                return makeEvent(3, tile, 4);
            },
            [](const QemuSyncEvent&) {},
            [](const QemuSyncEvent&) { return UINT64_C(20); },
            [&commits, tile](const QemuSyncEvent&) {
                commits.push_back(tile);
            },
        });
    }

    bool failed = false;
    try {
        const auto ignored = executor.collect();
        (void)ignored;
    } catch (const QemuReadySetExecutionError& error) {
        failed = true;
        require(error.tileId() == 2,
                "capture errors were not reported deterministically");
        require(std::string(error.what()).find("synthetic bridge failure") !=
                    std::string::npos,
                "capture error lost its cause");
    }
    require(failed, "capture error did not fail the batch");
    require(commits.empty(), "failed batch partially committed");

    executor.begin(30, 1);
    executor.submit({
        30,
        4,
        4,
        []() { return makeEvent(4, 1, 1); },
        [](const QemuSyncEvent&) {},
        [](const QemuSyncEvent&) { return UINT64_C(31); },
        [&commits](const QemuSyncEvent&) { commits.push_back(4); },
    });
    const auto recovered = executor.collect();
    require(recovered.size() == 1,
            "executor did not recover after a failed batch");
    recovered.front().commit(recovered.front().event);
    require(commits == std::vector<std::uint32_t>{4},
            "recovered batch did not commit exactly once");
}

void testAmbiguityRejection()
{
    QemuReadySetExecutor executor(1);
    executor.begin(100, 1);
    bool failed = false;
    try {
        executor.submit({
            101,
            0,
            1,
            []() { return makeEvent(1, 1, 1); },
            [](const QemuSyncEvent&) {},
            [](const QemuSyncEvent&) { return UINT64_C(102); },
            [](const QemuSyncEvent&) {},
        });
    } catch (const QemuReadySetExecutionError&) {
        failed = true;
    }
    require(failed, "mixed modeled-time frontiers were accepted");
    executor.discard();
    require(!executor.active(), "discard left an ambiguous batch active");

    executor.begin(200, 1);
    executor.submit({
        200,
        0,
        2,
        []() { return makeEvent(2, 1, 1); },
        [](const QemuSyncEvent&) {},
        [](const QemuSyncEvent&) { return UINT64_C(201); },
        [](const QemuSyncEvent&) {},
    });
    require(executor.collect().size() == 1,
            "discarded executor could not start a clean batch");
}

void testValidationFailureDoesNotCommit()
{
    QemuReadySetExecutor executor(2);
    std::vector<std::uint32_t> commits;
    executor.begin(50, 2);
    for (std::uint32_t tile = 0; tile < 2; ++tile) {
        executor.submit({
            50,
            tile,
            6,
            [tile]() { return makeEvent(6, tile + 1, 10); },
            [tile](const QemuSyncEvent&) {
                if (tile == 1) {
                    throw std::runtime_error(
                        "synthetic eligibility failure");
                }
            },
            [](const QemuSyncEvent&) { return UINT64_C(60); },
            [&commits, tile](const QemuSyncEvent&) {
                commits.push_back(tile);
            },
        });
    }

    bool failed = false;
    try {
        const auto ignored = executor.collect();
        (void)ignored;
    } catch (const QemuReadySetExecutionError& error) {
        failed = true;
        require(error.tileId() == 1,
                "validation error identified the wrong tile");
        require(std::string(error.what()).find("eligibility failure") !=
                    std::string::npos,
                "validation error lost its cause");
    }
    require(failed, "validation error did not fail the batch");
    require(commits.empty(), "validation failure partially committed");
}

void testRealBridgeCaptureOnWorkers()
{
    constexpr std::uint32_t tileCount = 6;
    std::vector<std::unique_ptr<SharedSyncMemoryBridge>> bridges;
    std::vector<MittensSyncBridge*> mappings;
    std::vector<std::thread> qemus;
    std::atomic<bool> guestStateValid{true};
    std::atomic<std::size_t> activeGuests{0};
    std::atomic<std::size_t> maximumActiveGuests{0};
    std::mutex firstPairMutex;
    std::condition_variable firstPairChanged;
    bool firstPairReleased = false;

    for (std::uint32_t tile = 0; tile < tileCount; ++tile) {
        auto bridge = std::make_unique<SharedSyncMemoryBridge>();
        bridge->create(tile);
        void* const address = mmap(
            nullptr,
            MITTENS_SYNC_BRIDGE_MAPPING_SIZE,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            bridge->fileDescriptor(),
            0);
        require(address != MAP_FAILED, "could not map real bridge fixture");
        mappings.push_back(static_cast<MittensSyncBridge*>(address));
        bridges.push_back(std::move(bridge));
    }

    for (std::uint32_t tile = 0; tile < tileCount; ++tile) {
        qemus.emplace_back([&, tile]() {
            MittensSyncBridge* const mapping = mappings[tile];
            waitWhile(&mapping->state, MITTENS_SYNC_STATE_IDLE);
            if (mittens_sync_load_acquire(&mapping->state) !=
                    MITTENS_SYNC_STATE_GRANTED ||
                mapping->instruction_budget != 100) {
                guestStateValid.store(false);
            }
            const std::size_t active = activeGuests.fetch_add(1) + 1;
            std::size_t maximum = maximumActiveGuests.load();
            while (active > maximum &&
                   !maximumActiveGuests.compare_exchange_weak(
                       maximum, active)) {
            }
            if (tile < 2) {
                std::unique_lock<std::mutex> lock(firstPairMutex);
                if (activeGuests.load() >= 2) {
                    firstPairReleased = true;
                    firstPairChanged.notify_all();
                }
                firstPairChanged.wait_for(
                    lock,
                    std::chrono::seconds(2),
                    [&firstPairReleased]() {
                        return firstPairReleased;
                    });
            }
            mittens_sync_store_release(
                &mapping->state, MITTENS_SYNC_STATE_RUNNING);
            wakeFutex(&mapping->state);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
            mapping->instructions_executed = 10 + tile;
            mapping->vector_instructions_executed = tile;
            mapping->stop_reason = MITTENS_SYNC_STOP_QUANTUM_END;
            mapping->event_flags = MITTENS_SYNC_EVENT_FLAG_NONE;
            mapping->event_sequence = 40 + tile;
            activeGuests.fetch_sub(1);
            mittens_sync_store_release(
                &mapping->state, MITTENS_SYNC_STATE_EVENT);
            wakeFutex(&mapping->state);
        });
    }

    std::vector<std::uint32_t> commits;
    QemuReadySetExecutor executor(2);
    executor.begin(500, tileCount);
    for (std::uint32_t tile = 0; tile < tileCount; ++tile) {
        executor.submit({
            500,
            tile,
            1,
            [&, tile]() {
                const std::uint64_t epoch = bridges[tile]->grant(100);
                require(epoch == 1, "real bridge grant epoch changed");
                for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
                    const auto event = bridges[tile]->waitForEvent(
                        std::chrono::milliseconds(10));
                    if (event.has_value()) {
                        return *event;
                    }
                }
                throw std::runtime_error("real bridge capture timed out");
            },
            [](const QemuSyncEvent& event) {
                require(
                    event.stopReason == MITTENS_SYNC_STOP_QUANTUM_END,
                    "real bridge returned the wrong stop reason");
            },
            [tile](const QemuSyncEvent&) {
                return UINT64_C(510) + (tile % 2);
            },
            [&commits, tile](const QemuSyncEvent&) {
                commits.push_back(tile);
            },
        });
    }
    const auto completions = executor.collect();
    for (const auto& completion : completions) {
        completion.commit(completion.event);
    }

    for (std::thread& qemu : qemus) {
        qemu.join();
    }
    for (MittensSyncBridge* mapping : mappings) {
        require(
            munmap(mapping, MITTENS_SYNC_BRIDGE_MAPPING_SIZE) == 0,
            "could not unmap real bridge fixture");
    }
    require(guestStateValid.load(), "real bridge grant state was invalid");
    require(maximumActiveGuests.load() == 2,
            "bounded executor woke more than two real QEMU fixtures");
    require(commits == std::vector<std::uint32_t>({0, 2, 4, 1, 3, 5}),
            "real bridge captures did not commit deterministically");
}

void testAsyncCaptureExecutor()
{
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        std::size_t active = 0;
        std::size_t maximum = 0;
        bool release = false;
    } gate;

    QemuAsyncCaptureExecutor executor(3);
    std::vector<std::future<QemuSyncEvent>> futures;
    for (std::uint32_t tile = 0; tile < 9; ++tile) {
        futures.push_back(executor.submit([&gate, tile]() {
            std::unique_lock<std::mutex> lock(gate.mutex);
            ++gate.active;
            gate.maximum = std::max(gate.maximum, gate.active);
            if (gate.active == 3) {
                gate.release = true;
                gate.changed.notify_all();
            }
            gate.changed.wait_for(
                lock,
                std::chrono::seconds(2),
                [&gate]() { return gate.release; });
            --gate.active;
            lock.unlock();
            return makeEvent(1, tile + 1, tile + 10);
        }));
    }
    for (std::uint32_t tile = 0; tile < futures.size(); ++tile) {
        const QemuSyncEvent event = futures[tile].get();
        require(event.eventSequence == tile + 1,
                "asynchronous capture future returned the wrong event");
    }
    const auto statistics = executor.statistics();
    require(gate.maximum == 3 && statistics.maximumConcurrent == 3,
            "asynchronous capture executor did not expose bounded concurrency");
    require(statistics.submitted == 9 && statistics.completed == 9,
            "asynchronous capture executor statistics did not reconcile");

    auto failure = executor.submit([]() -> QemuSyncEvent {
        throw std::runtime_error("synthetic asynchronous capture failure");
    });
    bool failed = false;
    try {
        (void)failure.get();
    } catch (const std::runtime_error& error) {
        failed = std::string(error.what()).find("synthetic") !=
            std::string::npos;
    }
    require(failed,
            "asynchronous capture executor lost a worker exception");
}

void testOwnerThreadEnforcement()
{
    QemuReadySetExecutor executor(1);
    executor.begin(0, 1);
    std::atomic<bool> rejected{false};
    std::thread intruder([&]() {
        try {
            executor.submit({
                0,
                0,
                1,
                []() { return makeEvent(1, 1, 1); },
                [](const QemuSyncEvent&) {},
                [](const QemuSyncEvent&) { return UINT64_C(1); },
                [](const QemuSyncEvent&) {},
            });
        } catch (const std::logic_error&) {
            rejected.store(true);
        }
    });
    intruder.join();
    require(rejected.load(), "non-owner submission was accepted");
    executor.discard();
}

void testDiscardDrainsInFlightCapture()
{
    QemuReadySetExecutor executor(2);
    std::atomic<bool> captureFinished{false};
    std::atomic<bool> committed{false};
    executor.begin(0, 2);
    executor.submit({
        0,
        0,
        1,
        [&captureFinished]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            captureFinished.store(true);
            return makeEvent(1, 1, 1);
        },
        [](const QemuSyncEvent&) {},
        [](const QemuSyncEvent&) { return UINT64_C(1); },
        [&committed](const QemuSyncEvent&) {
            committed.store(true);
        },
    });
    executor.discard();
    require(captureFinished.load(),
            "discard returned before an in-flight capture drained");
    require(!committed.load(), "discard committed an in-flight capture");
    require(!executor.active(), "discard left the drained batch active");
}

} // namespace

int main()
{
    try {
        testOneWorkerIdentity();
        testSerialTieOrderPreserved();
        testMultiWorkerConcurrency();
        testRepeatedDeterministicOrdering();
        testFailClosedAndRecover();
        testAmbiguityRejection();
        testValidationFailureDoesNotCommit();
        testRealBridgeCaptureOnWorkers();
        testAsyncCaptureExecutor();
        testOwnerThreadEnforcement();
        testDiscardDrainsInFlightCapture();
    } catch (const std::exception& error) {
        std::cerr << "qemu ready-set executor test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "qemu ready-set executor test passed\n";
    return EXIT_SUCCESS;
}
