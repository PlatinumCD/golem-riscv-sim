#pragma once
#include <cstdint>

namespace TileComponents {
// Implementation constants, not architecture parameters.
inline constexpr const char* Clock = "1GHz";
inline constexpr unsigned BackendQueueEntries = 64;
inline constexpr unsigned BankLatency = 1;
inline constexpr unsigned ArrayQueueEntries = 4;
}
