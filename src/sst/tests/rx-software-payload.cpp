#include <stdint.h>
#include "mesh-nic.h"
#include "platform.h"
namespace {
constexpr uint32_t kDeploymentFrameMagic = UINT32_C(0x474f4c4d);
constexpr uint32_t kSoftwarePayloadWords = 1022;
constexpr uint32_t kSoftwarePayloadFrames = 2;
constexpr uint32_t kSoftwarePayloadRouteBase = 9000;
uint64_t readCycle()
{
    uint64_t value;
    __asm__ volatile("rdcycle %0" : "=r"(value));
    return value;
}
[[maybe_unused]] void delayReceiver() {
    const uint64_t start = readCycle();
    while (readCycle() - start < 500000) {}
}
void printUnsigned(uint64_t value)
{
    char digits[20];
    uint32_t count = 0;
    do {
        digits[count++] =
            static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    while (count != 0) {
        uart_putc(digits[--count]);
    }
}

[[maybe_unused]] uint32_t softwarePayloadWord(
    uint32_t frame,
    uint32_t word)
{
    return UINT32_C(0xa5000000) ^ (frame << 20U) ^ word;
}

[[maybe_unused]] void sendWordsBlocking(
    uint32_t destination,
    const uint32_t* words,
    uint32_t wordCount)
{
    while (!mesh_nic::try_send_words(destination, words, wordCount)) {
        mesh_nic::wait_for_transmit();
    }
}

[[maybe_unused]] uint32_t receiveWordBlocking(uint32_t* source)
{
    uint32_t payload = 0;
    while (!mesh_nic::try_receive_from(source, &payload)) {
        mesh_nic::wait_for_receive();
    }
    return payload;
}

int runSoftwarePayload()
{
    mesh_nic::complete_memory_initialization();
    if constexpr (MITTENS_TILE_ID == 0) {
        alignas(64) uint32_t payload[kSoftwarePayloadWords];
        for (uint32_t frame = 0;
             frame < kSoftwarePayloadFrames;
             ++frame) {
            const uint64_t executionId = UINT64_C(0x100000000) + frame;
            const uint64_t logicalIteration = frame;
            alignas(64) const uint32_t header[] = {
                kDeploymentFrameMagic,
                kSoftwarePayloadRouteBase + frame,
                static_cast<uint32_t>(executionId),
                static_cast<uint32_t>(executionId >> 32U),
                static_cast<uint32_t>(logicalIteration),
                static_cast<uint32_t>(logicalIteration >> 32U),
                kSoftwarePayloadWords,
            };
            for (uint32_t word = 0;
                 word < kSoftwarePayloadWords;
                 ++word) {
                payload[word] = softwarePayloadWord(frame, word);
            }
            sendWordsBlocking(1, header, 7);
            sendWordsBlocking(1, payload, kSoftwarePayloadWords);
        }
        uart_puts("SOFTWARE_PAYLOAD_SOURCE_PASS frames=");
        printUnsigned(kSoftwarePayloadFrames);
        uart_putc('\n');
        return 0;
    }

    if constexpr (MITTENS_TILE_ID == 1) {
        delayReceiver();
        for (uint32_t frame = 0;
             frame < kSoftwarePayloadFrames;
             ++frame) {
            const uint64_t executionId = UINT64_C(0x100000000) + frame;
            const uint64_t logicalIteration = frame;
            const uint32_t expectedHeader[] = {
                kDeploymentFrameMagic,
                kSoftwarePayloadRouteBase + frame,
                static_cast<uint32_t>(executionId),
                static_cast<uint32_t>(executionId >> 32U),
                static_cast<uint32_t>(logicalIteration),
                static_cast<uint32_t>(logicalIteration >> 32U),
                kSoftwarePayloadWords,
            };
            for (uint32_t word = 0; word < 7; ++word) {
                uint32_t source = UINT32_MAX;
                if (receiveWordBlocking(&source) != expectedHeader[word] ||
                    source != 0) {
                    uart_puts("SOFTWARE_PAYLOAD_DESTINATION_FAIL header\n");
                    return 1;
                }
            }
            if (!mesh_nic::try_claim_receive_words(
                    0,
                    kSoftwarePayloadRouteBase + frame,
                    logicalIteration,
                    kSoftwarePayloadWords)) {
                uart_puts(
                    "SOFTWARE_PAYLOAD_DESTINATION_FAIL claim\n");
                return 3;
            }
            for (uint32_t word = 0;
                 word < kSoftwarePayloadWords;
                 ++word) {
                uint32_t source = UINT32_MAX;
                if (receiveWordBlocking(&source) !=
                        softwarePayloadWord(frame, word) ||
                    source != 0) {
                    uart_puts("SOFTWARE_PAYLOAD_DESTINATION_FAIL payload\n");
                    return 2;
                }
            }
        }
        uart_puts("SOFTWARE_PAYLOAD_DESTINATION_PASS frames=");
        printUnsigned(kSoftwarePayloadFrames);
        uart_puts(" words=");
        printUnsigned(
            static_cast<uint64_t>(kSoftwarePayloadFrames) *
            kSoftwarePayloadWords);
        uart_putc('\n');
        return 0;
    }
    return 3;
}
}
extern "C" int tile_main() { return runSoftwarePayload(); }
