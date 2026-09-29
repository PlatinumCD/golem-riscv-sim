// source_new execution adapter for a deployment contained in one physical tile.
// The compiler still creates every computation and transfer descriptor.
// External tensors live in an embedded, writable image in local SPM; this
// adapter only copies those compiler-defined regions. It implements neither MVM
// nor model arithmetic, and deliberately rejects any tile-to-tile
// communication.
#include "connection_abi.h"
#include <stdint.h>

using sculptor_deployment::Connection;
using sculptor_deployment::View;

extern "C" {
extern const int logicalTile, physicalTile, physicalCount, connectionCount;
extern const Connection connections[];
extern const int sculptor_test_epochs;
void sculptor_tile_seed();
void sculptor_tile_initialize();
void sculptor_tile_execute(int64_t epoch);
void sculptor_tile_report(int64_t epoch);

// Provided by the host payload assembly and linker script respectively. The
// image includes all external output reservations, not just initialized inputs.
extern unsigned char sculptor_single_tile_payload[];
extern unsigned char sculptor_single_tile_payload_end[];
extern const uint64_t sculptor_single_tile_payload_global_offset;
extern unsigned char __sculptor_spm_begin[], __sculptor_spm_end[];
volatile uint32_t sculptor_single_tile_error = 0;
}

namespace {
enum Error : uint32_t {
  Topology = 1,
  Descriptor = 2,
  Epoch = 3,
  Port = 4,
  PayloadBounds = 5,
  ViewBounds = 6,
  ReceiveBinding = 7,
  SendBinding = 8,
  Region = 9,
  AnalogStatus = 10,
};

[[noreturn]] void fail(Error error) {
  sculptor_single_tile_error = error;
  // SiFive finisher is the same termination interface as the source_new guest
  // startup. This is not the old mesh, DMA, or analog MMIO interface.
  *reinterpret_cast<volatile uint32_t *>(uintptr_t(0x100000)) =
      (uint32_t(error) << 16) | 0x3333;
  for (;;) {
  }
}
void require(bool condition, Error error) {
  if (!condition)
    fail(error);
}

uint64_t payloadBytes() {
  uintptr_t begin = reinterpret_cast<uintptr_t>(sculptor_single_tile_payload);
  uintptr_t end = reinterpret_cast<uintptr_t>(sculptor_single_tile_payload_end);
  require(end >= begin && !(begin % alignof(float)), PayloadBounds);
  return end - begin;
}

void checkEpoch(int64_t epoch) {
  require(epoch >= 0 && epoch < sculptor_test_epochs, Epoch);
}

// Check multiplication and addition before forming an image pointer. Offsets
// retain the compiler's shared-image address space even though storage is
// local.
float *payload(uint64_t offset, uint64_t stride, int64_t epoch,
               uint64_t words) {
  checkEpoch(epoch);
  require(!stride || uint64_t(epoch) <= (UINT64_MAX - offset) / stride,
          PayloadBounds);
  offset += uint64_t(epoch) * stride;
  require(offset >= sculptor_single_tile_payload_global_offset, PayloadBounds);
  offset -= sculptor_single_tile_payload_global_offset;
  uint64_t size = payloadBytes();
  require(offset <= size && words > 0 && words <= (size - offset) / 4 &&
              !(offset % alignof(float)),
          PayloadBounds);
  return reinterpret_cast<float *>(sculptor_single_tile_payload + offset);
}

int64_t checkView(View<float> view) {
  int64_t count = 1, last = 0;
  for (int d = 0; d < 4; ++d) {
    int64_t size = view.sizes[d], stride = view.strides[d];
    require(size > 0 && stride >= 0 && count <= INT64_MAX / size, ViewBounds);
    count *= size;
    require(!stride || size - 1 <= (INT64_MAX - last) / stride, ViewBounds);
    last += (size - 1) * stride;
  }
  uintptr_t begin = reinterpret_cast<uintptr_t>(__sculptor_spm_begin);
  uintptr_t end = reinterpret_cast<uintptr_t>(__sculptor_spm_end);
  uintptr_t address = reinterpret_cast<uintptr_t>(view.data);
  require(end >= begin && address >= begin && address < end &&
              !(address % alignof(float)) &&
              uint64_t(last) < (end - address) / 4,
          ViewBounds);
  return count;
}

View<float> flat(float *data, int64_t count) {
  return {data, {count, 1, 1, 1}, {1, 1, 1, 1}};
}

// Independent source/destination cursors support rank changes and strided
// output views without assuming that equal element counts imply equal shapes.
struct Cursor {
  View<float> view;
  float *pointer;
  int64_t indices[4] = {};
  explicit Cursor(View<float> v) : view(v), pointer(v.data) {}
  void next() {
    for (int d = 3; d >= 0; --d) {
      if (++indices[d] < view.sizes[d]) {
        pointer += view.strides[d];
        return;
      }
      indices[d] = 0;
      pointer -= (view.sizes[d] - 1) * view.strides[d];
    }
  }
};

void copy(View<float> source, View<float> destination) {
  int64_t count = checkView(source);
  require(count == checkView(destination), ViewBounds);
  Cursor src(source), dst(destination);
  for (int64_t i = 0; i < count; ++i) {
    *dst.pointer = *src.pointer;
    if (i + 1 < count) {
      src.next();
      dst.next();
    }
  }
}

bool selected(const Connection &c, const int64_t *keys) {
  for (int i = 0; i < c.scopeCount; ++i)
    require(keys[i] >= 0 && uint64_t(keys[i]) <= UINT32_MAX, Descriptor);
  for (int i = 0; i < c.selectorCount; ++i)
    if (keys[c.scopeCount + i] != c.selectors[i])
      return false;
  return true;
}

void validateDeployment() {
  require(logicalTile == 0 && physicalTile == 0 && physicalCount == 1,
          Topology);
  require(connectionCount >= 0 && sculptor_test_epochs > 0, Descriptor);
  auto begin = reinterpret_cast<uintptr_t>(sculptor_single_tile_payload);
  auto spmBegin = reinterpret_cast<uintptr_t>(__sculptor_spm_begin);
  auto spmEnd = reinterpret_cast<uintptr_t>(__sculptor_spm_end);
  require(begin >= spmBegin && begin <= spmEnd &&
              payloadBytes() <= spmEnd - begin,
          PayloadBounds);
  for (int i = 0; i < connectionCount; ++i) {
    const auto &c = connections[i];
    require((c.source == -1 && c.destination == 0) ||
                (c.source == 0 && c.destination == -1),
            Topology);
    require(c.words > 0 && c.selectorCount >= 0 && c.selectorCount <= 2 &&
                c.scopeCount >= 0 && c.scopeCount <= 1 &&
                (c.matchSendSelectors == 0 || c.matchSendSelectors == 1),
            Descriptor);
    require(c.source < 0 || c.sourcePort >= 0, Descriptor);
    require(c.destination < 0 || c.destinationPort >= 0, Descriptor);
    for (int k = 0; k < c.selectorCount; ++k)
      require(c.selectors[k] >= 0, Descriptor);
    int64_t regionWords = 1;
    for (int d = 0; d < 4; ++d) {
      int64_t size = c.sourceRegion.sizes[d];
      require(c.sourceRegion.offsets[d] >= 0 && size > 0 &&
                  regionWords <= INT64_MAX / size,
              Region);
      regionWords *= size;
    }
    require(regionWords == c.words, Region);
    // Check the final epoch as well as the first before any generated code
    // runs.
    payload(c.globalOffset, c.epochBytes, 0, c.words);
    payload(c.globalOffset, c.epochBytes, sculptor_test_epochs - 1, c.words);
  }
}
} // namespace

extern "C" {
// LLVM may recognize buffer initialization and copies as libc intrinsics even
// when the generated model has no explicit library calls. Supply only their
// byte-memory semantics for the freestanding tile; no model computation lives
// here. Compile this file with -ffreestanding -fno-builtin so these loops
// cannot be rewritten into calls to themselves.
void *memset(void *destination, int value, size_t bytes) {
  auto *output = static_cast<unsigned char *>(destination);
  for (size_t i = 0; i < bytes; ++i)
    output[i] = static_cast<unsigned char>(value);
  return destination;
}

void *memcpy(void *destination, const void *source, size_t bytes) {
  auto *output = static_cast<unsigned char *>(destination);
  auto *input = static_cast<const unsigned char *>(source);
  for (size_t i = 0; i < bytes; ++i)
    output[i] = input[i];
  return destination;
}

void *memmove(void *destination, const void *source, size_t bytes) {
  auto *output = static_cast<unsigned char *>(destination);
  auto *input = static_cast<const unsigned char *>(source);
  uintptr_t outAddress = reinterpret_cast<uintptr_t>(output);
  uintptr_t inAddress = reinterpret_cast<uintptr_t>(input);
  if (outAddress > inAddress && outAddress - inAddress < bytes) {
    for (size_t i = bytes; i > 0; --i)
      output[i - 1] = input[i - 1];
  } else {
    for (size_t i = 0; i < bytes; ++i)
      output[i] = input[i];
  }
  return destination;
}

void sculptor_rt_check_status(uint64_t status) {
  require(status == 0, AnalogStatus);
}

// All copies complete synchronously, so there are no outstanding source-buffer
// lifetimes or acknowledgements to drain on this single-tile transport.
void sculptor_rt_drain() {}

#define VIEW_ARGS                                                              \
  float *ptr, int64_t a, int64_t b, int64_t c, int64_t d, int64_t sa,          \
      int64_t sb, int64_t sc, int64_t sd
void sculptor_rt_receive(int64_t port, int64_t epoch, VIEW_ARGS, int64_t k0,
                         int64_t k1, int64_t k2) {
  require(port >= 0 && port <= INT32_MAX, Port);
  checkEpoch(epoch);
  const int64_t keys[] = {k0, k1, k2};
  View<float> destination{ptr, {a, b, c, d}, {sa, sb, sc, sd}};
  const Connection *match = nullptr;
  for (int i = 0; i < connectionCount; ++i) {
    const auto &entry = connections[i];
    if (entry.destination != physicalTile || entry.destinationPort != port ||
        !selected(entry, keys))
      continue;
    require(!match && entry.source == -1, ReceiveBinding);
    match = &entry;
  }
  require(match, ReceiveBinding);
  require(checkView(destination) == match->words, ViewBounds);
  copy(
      flat(payload(match->globalOffset, match->epochBytes, epoch, match->words),
           match->words),
      destination);
}

void sculptor_rt_send(int64_t port, int64_t epoch, VIEW_ARGS, int64_t k0,
                      int64_t k1, int64_t k2) {
  require(port >= 0 && port <= INT32_MAX, Port);
  checkEpoch(epoch);
  const int64_t keys[] = {k0, k1, k2};
  View<float> source{ptr, {a, b, c, d}, {sa, sb, sc, sd}};
  checkView(source);
  bool matched = false;
  for (int i = 0; i < connectionCount; ++i) {
    const auto &entry = connections[i];
    if (entry.source != physicalTile || entry.sourcePort != port ||
        (entry.matchSendSelectors && !selected(entry, keys)))
      continue;
    require(entry.destination == -1, SendBinding);
    View<float> region;
    require(sculptor_deployment::selectRegion(source, entry.sourceRegion,
                                              entry.words, region),
            Region);
    copy(region,
         flat(payload(entry.globalOffset, entry.epochBytes, epoch, entry.words),
              entry.words));
    matched = true;
  }
  require(matched, SendBinding);
}
#undef VIEW_ARGS

void sculptor_rt_seed(float *data, int64_t offset, int64_t count) {
  require(offset >= 0 && count > 0, PayloadBounds);
  copy(flat(data, count), flat(payload(offset, 0, 0, count), count));
}

void sculptor_rt_report(int64_t epoch, int64_t offset, int64_t stride,
                        int64_t count) {
  require(offset >= 0 && stride >= 0 && count > 0, PayloadBounds);
  // The SST harness reads this same bounded image after the guest finishes.
  // No UART, mesh transfer, or host-side tensor computation is needed here.
  payload(offset, stride, epoch, count);
}

int main() {
  validateDeployment();
  sculptor_tile_seed();
  sculptor_tile_initialize();
  for (int64_t epoch = 0; epoch < sculptor_test_epochs; ++epoch) {
    sculptor_tile_execute(epoch);
    sculptor_tile_report(epoch);
  }
  return 0;
}
}
