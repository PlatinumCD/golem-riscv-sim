/*
 * RISC-V Golem analog custom-instruction helpers
 *
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "hw/misc/mittens_sync.h"
#include "qemu/bswap.h"
#include "vector_internals.h"

target_ulong HELPER(golem_network)(CPURISCVState *env, uint32_t operation,
                                   target_ulong first, target_ulong second)
{
    if (!mittens_sync_vector_analog_enabled()) {
        riscv_raise_exception(env, RISCV_EXCP_ILLEGAL_INST, GETPC());
    }
    return mittens_sync_network(operation, first, second);
}

target_ulong HELPER(golem_analog)(
    CPURISCVState *env,
    uint32_t operation,
    target_ulong rs1,
    target_ulong rs2)
{
    /* Both scalar commands return status. Configure supplies packed active
     * extents, not a memory address; data transfers remain RVV-only. */
    if ((operation != MITTENS_SYNC_VECTOR_ANALOG_EXECUTE &&
         operation != MITTENS_SYNC_VECTOR_ANALOG_CONFIGURE) ||
        !mittens_sync_vector_analog_enabled()) {
        riscv_raise_exception(env, RISCV_EXCP_ILLEGAL_INST, GETPC());
    }
    if (operation == MITTENS_SYNC_VECTOR_ANALOG_CONFIGURE) {
        return mittens_sync_vector_analog(operation, rs1, rs2, 0, NULL);
    }
    return mittens_sync_vector_analog(
        MITTENS_SYNC_VECTOR_ANALOG_EXECUTE, (uint32_t)rs1, 0, 0, NULL);
}

void HELPER(golem_vector_analog)(
    CPURISCVState *env, uint32_t operation, uint32_t vector_register,
    target_ulong array_id, target_ulong element_offset)
{
    uint8_t data[MITTENS_SYNC_VECTOR_ANALOG_BYTES];
    const uint32_t vlen = riscv_cpu_cfg(env)->vlen;
    const int32_t lmul = sextract32(FIELD_EX64(env->vtype, VTYPE, VLMUL), 0, 3);
    const uint32_t group_registers = 1U << MAX(lmul, 0);
    uint32_t element_count, vlmax, index;
    uint32_t asynchronous_status = MITTENS_SYNC_VECTOR_ANALOG_PENDING;
    uint32_t *words;

    if (!riscv_has_ext(env, RVV) || env->vill || env->vstart != 0 ||
        FIELD_EX64(env->vtype, VTYPE, VSEW) != 2 ||
        lmul < -3 || lmul > 3 || vector_register >= 32 ||
        vector_register % group_registers != 0 ||
        vector_register + group_registers > 32 ||
        vlen > RV_VLEN_MAX || !mittens_sync_vector_analog_enabled() ||
        (operation != MITTENS_SYNC_VECTOR_ANALOG_PROGRAM &&
         operation != MITTENS_SYNC_VECTOR_ANALOG_LOAD &&
         operation != MITTENS_SYNC_VECTOR_ANALOG_STORE)) {
        riscv_raise_exception(env, RISCV_EXCP_ILLEGAL_INST, GETPC());
    }
    vlmax = vext_get_vlmax(env_archcpu(env), env->vtype);
    if (env->vl > vlmax ||
        env->vl > MITTENS_SYNC_VECTOR_ANALOG_BYTES / sizeof(uint32_t)) {
        riscv_raise_exception(env, RISCV_EXCP_ILLEGAL_INST, GETPC());
    }
    element_count = env->vl;
    if (mittens_asq_vector_analog(env, operation, vector_register, array_id,
                                  element_offset, element_count, &asynchronous_status)) {
        if (asynchronous_status != 0) {
            riscv_raise_exception(env, RISCV_EXCP_ILLEGAL_INST, GETPC());
        }
        return;
    }

    /* QEMU packs architectural registers using configured VLEN, while
     * elements within each host-endian 64-bit word use H4 addressing. */
    words = (uint32_t *)((uint8_t *)env->vreg + vector_register * vlen / 8);
    if (operation != MITTENS_SYNC_VECTOR_ANALOG_STORE) {
        for (index = 0; index < element_count; ++index) {
            stl_le_p(data + index * sizeof(uint32_t), words[H4(index)]);
        }
    }
    if (mittens_sync_vector_analog(operation, array_id, element_offset,
                                  element_count, data) != 0) {
        riscv_raise_exception(env, RISCV_EXCP_ILLEGAL_INST, GETPC());
    }
    if (operation == MITTENS_SYNC_VECTOR_ANALOG_STORE) {
        for (index = 0; index < element_count; ++index) {
            words[H4(index)] = ldl_le_p(data + index * sizeof(uint32_t));
        }
    }
    /* Only active elements are copied, preserving both ordinary tails and
     * the unused portion of a fractional register regardless of vta/vma. */
}
