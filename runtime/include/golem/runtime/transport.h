#pragma once

#include <stdint.h>

namespace golem::runtime {

using TrySendWord = bool (*)(
    void* context,
    uint32_t destination_tile,
    uint32_t word
);

using TryReceiveWord = bool (*)(void* context, uint32_t* word);

struct WordTransport {
    void* context;
    TrySendWord try_send;
    TryReceiveWord try_receive;

    bool valid() const noexcept;
    bool trySend(uint32_t destination_tile, uint32_t word) const noexcept;
    bool tryReceive(uint32_t* word) const noexcept;
};

}  // namespace golem::runtime
