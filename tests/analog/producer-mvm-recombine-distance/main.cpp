#include <stdint.h>

#include "mesh-nic.h"
#include "platform.h"

#ifndef MITTENS_TILE_ID
#error "MITTENS_TILE_ID must be defined"
#endif
#ifndef MITTENS_PRODUCER_TILE
#error "MITTENS_PRODUCER_TILE must be defined"
#endif
#ifndef MITTENS_MVM_TILE
#error "MITTENS_MVM_TILE must be defined"
#endif
#ifndef MITTENS_RECOMBINE_TILE
#error "MITTENS_RECOMBINE_TILE must be defined"
#endif

namespace {

constexpr uint32_t kTileId = MITTENS_TILE_ID;
constexpr uint32_t kProducerTile = MITTENS_PRODUCER_TILE;
constexpr uint32_t kMvmTile = MITTENS_MVM_TILE;
constexpr uint32_t kRecombineTile = MITTENS_RECOMBINE_TILE;

constexpr uint32_t kArrayCount = 2;
constexpr uint32_t kArrayRows = 256;
constexpr uint32_t kArrayColumns = 512;
constexpr uint32_t kMatrixWords = kArrayRows * kArrayColumns;
constexpr uint32_t kFrameMagic = UINT32_C(0x474f4c4d);
constexpr uint32_t kFrameHeaderWords = 7;
constexpr uint32_t kActivationRoute = 100;
constexpr uint32_t kPartial0Route = 200;
constexpr uint32_t kPartial1Route = 201;
constexpr uint32_t kReadyWord = UINT32_C(0x504d5259);
constexpr uint32_t kCompleteWord = UINT32_C(0x504d5243);
constexpr uint32_t kProducerTask = 1000;
constexpr uint32_t kMvmTask = 2000;
constexpr uint32_t kRecombineTask = 3000;

static_assert(kProducerTile < 81);
static_assert(kMvmTile < 81);
static_assert(kRecombineTile < 81);

alignas(64) float matrices[kArrayCount][kMatrixWords] = {};
alignas(64) float activation[kArrayColumns] = {};
alignas(64) float partials[kArrayCount][kArrayRows] = {};
alignas(64) float result[kArrayRows] = {};
alignas(64) float warmOutput[kArrayCount][kArrayRows] = {};

void printUnsigned(uint32_t value) {
    char digits[10];
    uint32_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) {
        uart_putc(digits[--count]);
    }
}

bool analogSuccess(unsigned long status) {
    return status == 0;
}

unsigned long setMatrix(const float* matrix, uint32_t arrayId) {
    unsigned long status;
    asm volatile(
        "mvm.set %0, %1, %2"
        : "=r"(status)
        : "r"(matrix), "r"(static_cast<unsigned long>(arrayId))
        : "memory");
    return status;
}

unsigned long loadVector(const float* input, uint32_t arrayId) {
    unsigned long status;
    asm volatile(
        "mvm.l %0, %1, %2"
        : "=r"(status)
        : "r"(input), "r"(static_cast<unsigned long>(arrayId))
        : "memory");
    return status;
}

unsigned long executeMvm(uint32_t arrayId) {
    unsigned long status;
    const unsigned long id = arrayId;
    asm volatile(
        "mvm %0, %1, %1"
        : "=r"(status)
        : "r"(id)
        : "memory");
    return status;
}

unsigned long storeVector(float* output, uint32_t arrayId) {
    unsigned long status;
    asm volatile(
        "mvm.s %0, %1, %2"
        : "=r"(status)
        : "r"(output), "r"(static_cast<unsigned long>(arrayId))
        : "memory");
    return status;
}

[[maybe_unused]] void sendWordBlocking(
    uint32_t destination,
    uint32_t word
) {
    while (!mesh_nic::try_send(destination, word)) {
        mesh_nic::wait_for_transmit();
    }
}

[[maybe_unused]] void sendWordsBlocking(
    uint32_t destination,
    const uint32_t* words,
    uint32_t wordCount
) {
    while (!mesh_nic::try_send_words(destination, words, wordCount)) {
        mesh_nic::wait_for_transmit();
    }
}

[[maybe_unused]] bool receiveWordFrom(
    uint32_t expectedSource,
    uint32_t expectedWord
) {
    uint32_t source = 0;
    const uint32_t word = mesh_nic::receive_from(&source);
    return source == expectedSource && word == expectedWord;
}

[[maybe_unused]] void sendFrame(
    uint32_t destination,
    uint32_t routeId,
    const float* data,
    uint32_t wordCount
) {
    const uint32_t header[kFrameHeaderWords] = {
        kFrameMagic,
        routeId,
        0,
        0,
        UINT32_MAX,
        UINT32_MAX,
        wordCount,
    };
    sendWordsBlocking(destination, header, kFrameHeaderWords);
    sendWordsBlocking(
        destination,
        reinterpret_cast<const uint32_t*>(data),
        wordCount
    );
}

[[maybe_unused]] bool receiveFrame(
    uint32_t expectedSource,
    uint32_t expectedRoute,
    float* destination,
    uint32_t wordCount
) {
    uint32_t header[kFrameHeaderWords] = {};
    for (uint32_t index = 0; index < kFrameHeaderWords; ++index) {
        uint32_t source = 0;
        header[index] = mesh_nic::receive_from(&source);
        if (source != expectedSource) {
            return false;
        }
    }
    if (header[0] != kFrameMagic ||
        header[1] != expectedRoute ||
        header[2] != 0 ||
        header[3] != 0 ||
        header[4] != UINT32_MAX ||
        header[5] != UINT32_MAX ||
        header[6] != wordCount) {
        return false;
    }

    while (!mesh_nic::try_start_receive_words(
        expectedSource,
        expectedRoute,
        UINT64_MAX,
        destination,
        wordCount
    )) {
        mesh_nic::wait_for_receive();
    }

    while (true) {
        uint32_t source = 0;
        uint32_t route = 0;
        uint64_t logicalIteration = 0;
        if (mesh_nic::try_receive_words_completion(
                &source, &route, &logicalIteration)) {
            return source == expectedSource &&
                   route == expectedRoute &&
                   logicalIteration == UINT64_MAX;
        }
        mesh_nic::wait_for_receive();
    }
}

void traceStart(uint32_t taskId) {
    mesh_nic::trace_task(mesh_nic::kTaskTraceStart, taskId, 0);
}

void traceFinish(uint32_t taskId) {
    mesh_nic::trace_task(mesh_nic::kTaskTraceFinish, taskId, 0);
}

[[maybe_unused]] bool prepareArrays() {
    for (uint32_t row = 0; row < kArrayRows; ++row) {
        matrices[0][row * kArrayColumns + row] = 1.0F;
        matrices[1][
            row * kArrayColumns + kArrayRows + row
        ] = 1.0F;
    }

    for (uint32_t arrayId = 0; arrayId < kArrayCount; ++arrayId) {
        if (!analogSuccess(setMatrix(matrices[arrayId], arrayId))) {
            return false;
        }
    }

    /*
     * Drain both array queues before announcing readiness. This deliberately
     * keeps matrix programming and the warm-up operation outside the three
     * measured task intervals.
     */
    for (uint32_t arrayId = 0; arrayId < kArrayCount; ++arrayId) {
        if (!analogSuccess(loadVector(activation, arrayId)) ||
            !analogSuccess(executeMvm(arrayId))) {
            return false;
        }
    }
    for (uint32_t arrayId = 0; arrayId < kArrayCount; ++arrayId) {
        if (!analogSuccess(storeVector(warmOutput[arrayId], arrayId))) {
            return false;
        }
    }
    return true;
}

[[maybe_unused]] void produceActivation() {
    for (uint32_t index = 0; index < kArrayColumns; ++index) {
        activation[index] =
            static_cast<float>((index % 17U) + 1U);
    }
}

[[maybe_unused]] bool runMvm() {
    /*
     * Preserve the intended streaming order: load/execute one array, then
     * load/execute the other, followed by the two stores.
     */
    for (uint32_t arrayId = 0; arrayId < kArrayCount; ++arrayId) {
        if (!analogSuccess(loadVector(activation, arrayId)) ||
            !analogSuccess(executeMvm(arrayId))) {
            return false;
        }
    }
    for (uint32_t arrayId = 0; arrayId < kArrayCount; ++arrayId) {
        if (!analogSuccess(storeVector(partials[arrayId], arrayId))) {
            return false;
        }
    }
    return true;
}

[[maybe_unused]] void recombine() {
    for (uint32_t index = 0; index < kArrayRows; ++index) {
        result[index] = partials[0][index] + partials[1][index];
    }
}

[[maybe_unused]] bool resultIsCorrect() {
    for (uint32_t index = 0; index < kArrayRows; ++index) {
        const float expected =
            static_cast<float>((index % 17U) + 1U) +
            static_cast<float>(
                ((kArrayRows + index) % 17U) + 1U
            );
        if (result[index] != expected) {
            return false;
        }
    }
    return true;
}

int fail(const char* reason, int code) {
    uart_puts("PMR_FAIL tile=");
    printUnsigned(kTileId);
    uart_puts(" reason=");
    uart_puts(reason);
    uart_putc('\n');
    return code;
}

}  // namespace

extern "C" int tile_main() {
    if constexpr (kTileId == kMvmTile) {
        if (!prepareArrays()) {
            return fail("array-setup", 1);
        }
    }
    mesh_nic::complete_memory_initialization();

    if constexpr (kProducerTile != kMvmTile) {
        if constexpr (kTileId == kMvmTile) {
            sendWordBlocking(kProducerTile, kReadyWord);
        }
        if constexpr (kTileId == kProducerTile) {
            if (!receiveWordFrom(kMvmTile, kReadyWord)) {
                return fail("ready", 2);
            }
        }
    }

    if constexpr (kTileId == kProducerTile) {
        traceStart(kProducerTask);
        produceActivation();
        traceFinish(kProducerTask);
        if constexpr (kProducerTile != kMvmTile) {
            sendFrame(
                kMvmTile,
                kActivationRoute,
                activation,
                kArrayColumns
            );
        }
    }

    if constexpr (kTileId == kMvmTile) {
        if constexpr (kProducerTile != kMvmTile) {
            if (!receiveFrame(
                kProducerTile,
                kActivationRoute,
                activation,
                kArrayColumns
            )) {
                return fail("activation-receive", 3);
            }
        }

        traceStart(kMvmTask);
        if (!runMvm()) {
            return fail("mvm", 4);
        }
        traceFinish(kMvmTask);

        if constexpr (kMvmTile != kRecombineTile) {
            sendFrame(
                kRecombineTile,
                kPartial0Route,
                partials[0],
                kArrayRows
            );
            sendFrame(
                kRecombineTile,
                kPartial1Route,
                partials[1],
                kArrayRows
            );
        }
    }

    if constexpr (kTileId == kRecombineTile) {
        if constexpr (kMvmTile != kRecombineTile) {
            if (!receiveFrame(
                    kMvmTile,
                    kPartial0Route,
                    partials[0],
                    kArrayRows
                ) ||
                !receiveFrame(
                    kMvmTile,
                    kPartial1Route,
                    partials[1],
                    kArrayRows
                )) {
                return fail("partial-receive", 5);
            }
        }

        traceStart(kRecombineTask);
        recombine();
        traceFinish(kRecombineTask);
        if (!resultIsCorrect()) {
            return fail("numerical-result", 6);
        }
        if constexpr (kRecombineTile != kProducerTile) {
            sendWordBlocking(kProducerTile, kCompleteWord);
        }
    }

    if constexpr (kTileId == kProducerTile) {
        if constexpr (kRecombineTile != kProducerTile) {
            if (!receiveWordFrom(kRecombineTile, kCompleteWord)) {
                return fail("completion", 7);
            }
        }
        uart_puts("PMR_PASS producer=");
        printUnsigned(kProducerTile);
        uart_puts(" mvm=");
        printUnsigned(kMvmTile);
        uart_puts(" recombine=");
        printUnsigned(kRecombineTile);
        uart_putc('\n');
    }
    return 0;
}
