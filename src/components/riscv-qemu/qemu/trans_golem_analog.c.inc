/*
 * RISC-V Golem analog custom-instruction translation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "mittens/SyncTileBridge.h"

static bool trans_golem_analog(
    DisasContext *ctx,
    arg_r *a,
    uint32_t operation)
{
    TCGv status;

    REQUIRE_64BIT(ctx);
    translator_io_start(&ctx->base);

    status = tcg_temp_new();
    gen_helper_golem_analog(
        status,
        tcg_env,
        tcg_constant_i32(operation),
        get_gpr(ctx, a->rs1, EXT_NONE),
        get_gpr(ctx, a->rs2, EXT_NONE));
    gen_set_gpr(ctx, a->rd, status);
    return true;
}

static bool trans_mvm_set(DisasContext *ctx, arg_r *a)
{
    /* Legacy memory-to-array and array-to-array transfers are unsupported
     * in source_new. Reject before any helper can access guest memory. */
    return false;
}

static bool trans_mvm_load(DisasContext *ctx, arg_r *a)
{
    return false;
}

static bool trans_mvm_compute(DisasContext *ctx, arg_r *a)
{
    return trans_golem_analog(
        ctx, a, MITTENS_SYNC_VECTOR_ANALOG_EXECUTE);
}

static bool trans_mvm_store(DisasContext *ctx, arg_r *a)
{
    return false;
}

static bool trans_mvm_move(DisasContext *ctx, arg_r *a)
{
    return false;
}

static bool trans_golem_vector_analog(DisasContext *ctx, arg_r *a,
                                     uint32_t operation)
{
    const uint32_t group_registers = 1U << MAX(ctx->lmul, 0);

    REQUIRE_64BIT(ctx);
    if (!has_ext(ctx, RVV) || ctx->mstatus_vs == EXT_STATUS_DISABLED ||
        ctx->vill || ctx->sew != MO_32 || !ctx->vstart_eq_zero ||
        ctx->lmul < -3 || ctx->lmul > 3 ||
        a->rd % group_registers != 0 || a->rd + group_registers > 32) {
        return false;
    }
    translator_io_start(&ctx->base);
    gen_helper_mittens_sync_vector_instruction();
    gen_helper_golem_vector_analog(
        tcg_env, tcg_constant_i32(operation), tcg_constant_i32(a->rd),
        get_gpr(ctx, a->rs1, EXT_NONE), get_gpr(ctx, a->rs2, EXT_NONE));
    if (operation == MITTENS_SYNC_VECTOR_ANALOG_STORE) {
        mark_vs_dirty(ctx);
    }
    return true;
}

static bool trans_mvm_vset(DisasContext *ctx, arg_r *a)
{
    return trans_golem_vector_analog(ctx, a, MITTENS_SYNC_VECTOR_ANALOG_PROGRAM);
}

static bool trans_mvm_vl(DisasContext *ctx, arg_r *a)
{
    return trans_golem_vector_analog(ctx, a, MITTENS_SYNC_VECTOR_ANALOG_LOAD);
}

static bool trans_mvm_vs(DisasContext *ctx, arg_r *a)
{
    return trans_golem_vector_analog(ctx, a, MITTENS_SYNC_VECTOR_ANALOG_STORE);
}
