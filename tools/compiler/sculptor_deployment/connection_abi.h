#pragma once
#include "transfer_view.h"
#include <stddef.h>

namespace sculptor_deployment {
// Fixed deployment ABI, populated by MaterializeTileDeployment. It describes
// tensor transfers independently of the machine transport used to perform them.
struct Connection {
  int source, destination, sourcePort, destinationPort;
  int selectorCount, selectors[2], scopeCount, words, matchSendSelectors;
  uint64_t globalOffset, epochBytes, mailboxOffset;
  SourceRegion sourceRegion;
};
static_assert(sizeof(SourceRegion) == 64 && alignof(SourceRegion) == 8);
static_assert(sizeof(Connection) == 128 && alignof(Connection) == 8);
static_assert(offsetof(Connection, matchSendSelectors) == 36);
static_assert(offsetof(Connection, globalOffset) == 40);
static_assert(offsetof(Connection, epochBytes) == 48);
static_assert(offsetof(Connection, mailboxOffset) == 56);
static_assert(offsetof(Connection, sourceRegion) == 64);
static_assert(offsetof(SourceRegion, sizes) == 32);
} // namespace sculptor_deployment
