#pragma once
#include <atomic>
#include <mutex>

namespace TileComponents {
// Single-process observation gate for test fixtures. Never used by scheduling,
// memory service, numerical execution or completion logic. Existing components
// trace their full lifetime unless a fixture explicitly narrows trace scope.
inline std::atomic<bool> RecordComponentObservations{true};
inline std::mutex ObservationOutputMutex;
}
