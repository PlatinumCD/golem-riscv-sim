#include "../../bridge/include/mittens/NICTileBridge.h"

#include <array>
#include <cassert>
#include <cstdint>

int main()
{
    MittensBridgeShared bridge{};
    bridge.magic = MITTENS_BRIDGE_MAGIC;
    bridge.version = MITTENS_BRIDGE_VERSION;
    bridge.structure_size = sizeof(bridge);
    bridge.queue_capacity = MITTENS_BRIDGE_QUEUE_CAPACITY;

    std::array<std::uint32_t, MITTENS_BRIDGE_BURST_WORD_CAPACITY>
        large{};
    for (std::uint32_t index = 0; index < large.size(); ++index) {
        large[index] = UINT32_C(0x10000000) + index;
    }
    assert(mittens_bridge_rx_burst_push(
        &bridge,
        7,
        large.data(),
        static_cast<std::uint32_t>(large.size())));

    const MittensBridgeRxBurst* burst = nullptr;
    assert(mittens_bridge_rx_burst_peek(&bridge, &burst));
    assert(burst != nullptr);
    assert(burst->source == 7);
    assert(burst->word_count == large.size());
    assert(burst->words[0] == large.front());
    assert(burst->words[large.size() - 1] == large.back());
    assert(mittens_bridge_rx_burst_consume(&bridge));
    assert(!mittens_bridge_rx_burst_valid(&bridge));

    const std::array<std::uint32_t, 3> small{
        UINT32_C(0xa),
        UINT32_C(0xb),
        UINT32_C(0xc),
    };
    assert(mittens_bridge_tx_burst_push(
        &bridge,
        11,
        UINT64_C(0x90001000),
        small.data(),
        static_cast<std::uint32_t>(small.size())));
    MittensBridgeTxBurst txBurst{};
    assert(mittens_bridge_tx_burst_pop(&bridge, &txBurst));
    assert(txBurst.destination == 11);
    assert(txBurst.source_address == UINT64_C(0x90001000));
    assert(txBurst.word_count == small.size());
    assert(txBurst.words[2] == small[2]);

    assert(mittens_bridge_rx_burst_push(
        &bridge,
        9,
        small.data(),
        static_cast<std::uint32_t>(small.size())));

    MittensBridgeRxPacket word{};
    assert(mittens_bridge_rx_burst_pop_word(&bridge, &word));
    assert(word.source == 9);
    assert(word.payload == small[0]);
    burst = nullptr;
    assert(!mittens_bridge_rx_burst_peek(&bridge, &burst));
    assert(!mittens_bridge_rx_burst_consume(&bridge));
    assert(mittens_bridge_rx_burst_pop_word(&bridge, &word));
    assert(word.payload == small[1]);
    assert(mittens_bridge_rx_burst_pop_word(&bridge, &word));
    assert(word.payload == small[2]);
    assert(!mittens_bridge_rx_burst_valid(&bridge));

    bridge.rx_dma_timing_enabled = 1;
    assert(mittens_bridge_rx_dma_timing_enabled(&bridge));
    assert(!mittens_bridge_rx_dma_authorization_available(&bridge));
    for (std::uint32_t index = 0;
         index < MITTENS_BRIDGE_BURST_QUEUE_CAPACITY;
         ++index) {
        assert(mittens_bridge_rx_dma_authorize(&bridge));
    }
    assert(!mittens_bridge_rx_dma_authorize(&bridge));
    for (std::uint32_t index = 0;
         index < MITTENS_BRIDGE_BURST_QUEUE_CAPACITY;
         ++index) {
        assert(mittens_bridge_rx_dma_authorization_available(&bridge));
        assert(mittens_bridge_rx_dma_consume_authorization(&bridge));
    }
    assert(!mittens_bridge_rx_dma_authorization_available(&bridge));
    assert(!mittens_bridge_rx_dma_consume_authorization(&bridge));

    assert(!mittens_bridge_rx_dma_completion_available(&bridge));
    for (std::uint32_t index = 0;
         index < MITTENS_BRIDGE_RX_DMA_COMPLETION_CAPACITY;
         ++index) {
        assert(mittens_bridge_rx_dma_publish_completion(&bridge));
    }
    assert(!mittens_bridge_rx_dma_publish_completion(&bridge));
    for (std::uint32_t index = 0;
         index < MITTENS_BRIDGE_RX_DMA_COMPLETION_CAPACITY;
         ++index) {
        assert(mittens_bridge_rx_dma_completion_available(&bridge));
        assert(mittens_bridge_rx_dma_consume_completion(&bridge));
    }
    assert(!mittens_bridge_rx_dma_completion_available(&bridge));
    assert(!mittens_bridge_rx_dma_consume_completion(&bridge));
    return 0;
}
