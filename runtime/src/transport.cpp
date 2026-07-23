#include "golem/runtime/transport.h"

namespace golem::runtime {

bool WordTransport::valid() const noexcept {
    return try_send != nullptr && try_receive != nullptr;
}

bool WordTransport::trySend(
    uint32_t destination_tile,
    uint32_t word
) const noexcept {
    return try_send != nullptr &&
           try_send(context, destination_tile, word);
}

bool WordTransport::tryReceive(uint32_t* word) const noexcept {
    return try_receive != nullptr &&
           word != nullptr &&
           try_receive(context, word);
}

}  // namespace golem::runtime
