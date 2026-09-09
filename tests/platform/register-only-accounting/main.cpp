#include <stdint.h>

#include "mesh-nic.h"

namespace {

constexpr uint32_t kTaskId = 1;
constexpr uint32_t kIterations = 10000;

}  // namespace

extern "C" int tile_main() {
    // Set up the scalar and vector operands before the measured interval.
#if defined(REGISTER_ONLY_RVV)
    asm volatile(
        "li t0, 0x3f800000\n\t"
        "fmv.w.x fa0, t0\n\t"
        "vsetivli zero, 8, e32, m1, ta, ma\n\t"
        "vfmv.v.f v8, fa0"
        :
        :
        : "t0", "fa0", "v8", "memory");
#else
    asm volatile(
        "li t0, 0x3f800000\n\t"
        "fmv.w.x fa0, t0\n\t"
        "fsgnj.s ft0, fa0, fa0\n\t"
        "fsgnj.s ft1, fa0, fa0\n\t"
        "fsgnj.s ft2, fa0, fa0\n\t"
        "fsgnj.s ft3, fa0, fa0\n\t"
        "fsgnj.s ft4, fa0, fa0\n\t"
        "fsgnj.s ft5, fa0, fa0\n\t"
        "fsgnj.s ft6, fa0, fa0\n\t"
        "fsgnj.s ft7, fa0, fa0"
        :
        :
        : "t0", "fa0", "ft0", "ft1", "ft2", "ft3", "ft4", "ft5",
          "ft6", "ft7", "memory");
#endif

    mesh_nic::trace_task(mesh_nic::kTaskTraceStart, kTaskId, 0);

#if defined(REGISTER_ONLY_RVV)
    asm volatile(
        ".rept 10000\n\t"
        "vfadd.vf v8, v8, fa0\n\t"
        ".endr"
        :
        :
        : "v8", "fa0", "memory");
#else
    asm volatile(
        ".rept 10000\n\t"
        "fadd.s ft0, ft0, fa0\n\t"
        "fadd.s ft1, ft1, fa0\n\t"
        "fadd.s ft2, ft2, fa0\n\t"
        "fadd.s ft3, ft3, fa0\n\t"
        "fadd.s ft4, ft4, fa0\n\t"
        "fadd.s ft5, ft5, fa0\n\t"
        "fadd.s ft6, ft6, fa0\n\t"
        "fadd.s ft7, ft7, fa0\n\t"
        ".endr"
        :
        :
        : "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7",
          "fa0", "memory");
#endif

    mesh_nic::trace_task(mesh_nic::kTaskTraceFinish, kTaskId, 0);

    uint32_t consumed = 0;
#if defined(REGISTER_ONLY_RVV)
    asm volatile(
        "vfmv.f.s fa1, v8\n\t"
        "fmv.x.w %[value], fa1"
        : [value] "=r"(consumed)
        :
        : "v8", "fa1");
#else
    asm volatile(
        "fmv.x.w %[value], ft0"
        : [value] "=r"(consumed)
        :
        : "ft0");
#endif

    return consumed == UINT32_C(0xffffffff) ? 1 : 0;
}

static_assert(kIterations == 10000);
