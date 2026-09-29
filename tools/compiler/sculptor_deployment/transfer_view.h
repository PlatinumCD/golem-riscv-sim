#pragma once
#include <stdint.h>

namespace sculptor_deployment {
// Hardware-independent memory descriptions. data already includes the source
// memref's base offset. Region coordinates are relative to that view.
template <class T> struct View {
  T *data;
  int64_t sizes[4];
  int64_t strides[4];
  T &at(int64_t a = 0, int64_t b = 0, int64_t c = 0, int64_t d = 0) const {
    return data[a * strides[0] + b * strides[1] + c * strides[2] +
                d * strides[3]];
  }
  int64_t count() const { return sizes[0] * sizes[1] * sizes[2] * sizes[3]; }
  T &flat(int64_t n) const {
    int64_t offset = 0;
    for (int d = 3; d >= 0; --d) {
      offset += (n % sizes[d]) * strides[d];
      n /= sizes[d];
    }
    return data[offset];
  }
};
struct SourceRegion {
  int64_t offsets[4];
  int64_t sizes[4];
};

// Validate before pointer arithmetic. Current deployed views have nonnegative
// strides; reject unsupported layouts explicitly. No copying occurs here.
template <class T>
inline bool selectRegion(View<T> source, const SourceRegion &region,
                         int64_t elements, View<T> &payload) {
  int64_t start = 0, last = 0, count = 1;
  for (int d = 0; d < 4; ++d) {
    auto offset = region.offsets[d], size = region.sizes[d];
    auto stride = source.strides[d];
    if (source.sizes[d] <= 0 || size <= 0 || size > source.sizes[d] ||
        offset < 0 || offset > source.sizes[d] - size || stride < 0 ||
        count > INT64_MAX / size)
      return false;
    count *= size;
    int64_t end = offset + size - 1;
    if (stride && end > (INT64_MAX - last) / stride)
      return false;
    last += end * stride;
    start += offset * stride;
  }
  if (count != elements || uint64_t(last) > uint64_t(INT64_MAX) / sizeof(T))
    return false;
  payload = source;
  payload.data += start;
  for (int d = 0; d < 4; ++d)
    payload.sizes[d] = region.sizes[d];
  return true;
}
} // namespace sculptor_deployment
