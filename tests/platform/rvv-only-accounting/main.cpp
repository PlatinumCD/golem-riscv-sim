#include <stdint.h>

#include "mesh-nic.h"

#ifndef RVV_COMPUTE_K
#error "RVV_COMPUTE_K must be supplied by the Makefile"
#endif

#define STRINGIFY_IMPL(value) #value
#define STRINGIFY(value) STRINGIFY_IMPL(value)

namespace {

constexpr uint32_t kTaskId = 1;
constexpr uint32_t kComputeK = RVV_COMPUTE_K;

}  // namespace

extern "C" int tile_main() {
    // Initialize the scalar operand and vector register before timing.
    const uint64_t increment = 1;
    asm volatile(
        "vsetivli zero, 8, e32, m1, ta, ma\n\t"
        "vmv.v.x v8, %[increment]"
        :
        : [increment] "r"(increment)
        : "v8", "memory");

    mesh_nic::trace_task(mesh_nic::kTaskTraceStart, kTaskId, 0);

    // This is the complete measured workload: exactly K vector adds.
    asm volatile(
        ".rept " STRINGIFY(RVV_COMPUTE_K) "\n\t"
        "vadd.vx v8, v8, %[increment]\n\t"
        ".endr"
        :
        : [increment] "r"(increment)
        : "v8", "memory");

    mesh_nic::trace_task(mesh_nic::kTaskTraceFinish, kTaskId, 0);

    // Consume the result after timing so the measured work is observable.
    uint32_t consumed = 0;
    asm volatile(
        "vmv.x.s %[value], v8"
        : [value] "=r"(consumed)
        :
        : "v8");
    return consumed == UINT32_C(0xffffffff) ? 1 : 0;
}

static_assert(kComputeK >= 32);
static_assert(kComputeK <= 1024);
