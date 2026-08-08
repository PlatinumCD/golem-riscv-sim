#include <stdint.h>

#include "mesh-nic.h"

namespace {

constexpr uint32_t kBaselineTask = 0;
constexpr uint32_t kScalarTask = 1;
constexpr uint32_t kVectorTask = 2;
constexpr uint32_t kScalarInstructions = 1024;
constexpr uint32_t kVectorInstructions = 1027;

alignas(32) uint32_t vectorOutput[8] = {};

void start(uint32_t taskId) {
    mesh_nic::trace_task(
        mesh_nic::kTaskTraceStart, taskId, 0);
}

void finish(uint32_t taskId) {
    mesh_nic::trace_task(
        mesh_nic::kTaskTraceFinish, taskId, 0);
}

}  // namespace

extern "C" int tile_main() {
    start(kBaselineTask);
    asm volatile("" ::: "memory");
    finish(kBaselineTask);

    start(kScalarTask);
    asm volatile(
        ".rept 1024\n\t"
        "addi zero, zero, 0\n\t"
        ".endr"
        :
        :
        : "memory");
    finish(kScalarTask);

    start(kVectorTask);
    asm volatile(
        "vsetivli zero, 8, e32, m1, ta, ma\n\t"
        "vmv.v.i v8, 1\n\t"
        ".rept 1024\n\t"
        "vadd.vx v8, v8, zero\n\t"
        ".endr\n\t"
        "vse32.v v8, (%0)"
        :
        : "r"(vectorOutput)
        : "v8", "memory");
    finish(kVectorTask);

    static_assert(kScalarInstructions == 1024);
    static_assert(kVectorInstructions == 1027);
    return vectorOutput[0] == 1 ? 0 : 1;
}
