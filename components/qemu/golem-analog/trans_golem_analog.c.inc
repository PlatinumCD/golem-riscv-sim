/*
 * RISC-V Golem analog custom-instruction translation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "mittens/AnalogTileBridge.h"

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
    return trans_golem_analog(
        ctx, a, MITTENS_ANALOG_OPERATION_SET_MATRIX);
}

static bool trans_mvm_load(DisasContext *ctx, arg_r *a)
{
    return trans_golem_analog(
        ctx, a, MITTENS_ANALOG_OPERATION_LOAD_VECTOR);
}

static bool trans_mvm_compute(DisasContext *ctx, arg_r *a)
{
    return trans_golem_analog(
        ctx, a, MITTENS_ANALOG_OPERATION_COMPUTE);
}

static bool trans_mvm_store(DisasContext *ctx, arg_r *a)
{
    return trans_golem_analog(
        ctx, a, MITTENS_ANALOG_OPERATION_STORE_VECTOR);
}

static bool trans_mvm_move(DisasContext *ctx, arg_r *a)
{
    return trans_golem_analog(
        ctx, a, MITTENS_ANALOG_OPERATION_MOVE_VECTOR);
}
