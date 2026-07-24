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
#include "exec/cpu_ldst.h"
#include "exec/helper-proto.h"
#include "hw/misc/mittens_analog.h"

#include "mittens/AnalogTileBridge.h"

target_ulong HELPER(golem_analog)(
    CPURISCVState *env,
    uint32_t operation,
    target_ulong rs1,
    target_ulong rs2)
{
    MittensAnalogCommand command = {
        .operation = operation,
        .reserved = 0,
        .operand0 = 0,
        .operand1 = 0,
    };
    g_autofree uint32_t *input_words = NULL;
    g_autofree uint32_t *output_words = NULL;
    uint32_t input_word_count = 0;
    uint32_t output_word_count = 0;
    uint32_t output_capacity = 0;
    uint32_t array_rows = mittens_analog_array_rows();
    uint32_t array_columns = mittens_analog_array_columns();
    bool wait_for_completion = false;
    uint64_t status;
    uint64_t matrix_word_count;
    uint32_t index;

    switch (operation) {
    case MITTENS_ANALOG_OPERATION_SET_MATRIX:
        command.operand0 = rs1;
        command.operand1 = (uint32_t)rs2;
        matrix_word_count =
            (uint64_t)array_rows * (uint64_t)array_columns;
        if (matrix_word_count > UINT32_MAX) {
            return MITTENS_ANALOG_STATUS_INVALID_PAYLOAD;
        }
        input_word_count = (uint32_t)matrix_word_count;
        break;
    case MITTENS_ANALOG_OPERATION_LOAD_VECTOR:
        command.operand0 = rs1;
        command.operand1 = (uint32_t)rs2;
        input_word_count = array_columns;
        break;
    case MITTENS_ANALOG_OPERATION_COMPUTE:
        command.operand0 = (uint32_t)rs1;
        break;
    case MITTENS_ANALOG_OPERATION_STORE_VECTOR:
        command.operand0 = rs1;
        command.operand1 = (uint32_t)rs2;
        output_capacity = array_rows;
        wait_for_completion = true;
        break;
    case MITTENS_ANALOG_OPERATION_MOVE_VECTOR:
        command.operand0 = (uint32_t)rs1;
        command.operand1 = (uint32_t)rs2;
        break;
    default:
        return MITTENS_ANALOG_STATUS_INVALID_OPERATION;
    }

    if (input_word_count != 0) {
        input_words = g_new(uint32_t, input_word_count);
        for (index = 0; index < input_word_count; ++index) {
            input_words[index] = cpu_ldl_data_ra(
                env, rs1 + (target_ulong)index * sizeof(uint32_t),
                GETPC());
        }
    }
    if (output_capacity != 0) {
        output_words = g_new0(uint32_t, output_capacity);
    }

    status = mittens_analog_submit(
        env_cpu(env),
        &command,
        input_words,
        input_word_count,
        wait_for_completion,
        output_words,
        output_capacity,
        &output_word_count);

    if (status == MITTENS_ANALOG_STATUS_SUCCESS &&
        operation == MITTENS_ANALOG_OPERATION_STORE_VECTOR) {
        if (output_word_count != output_capacity) {
            return MITTENS_ANALOG_STATUS_INVALID_PAYLOAD;
        }
        for (index = 0; index < output_word_count; ++index) {
            cpu_stl_data_ra(
                env,
                rs1 + (target_ulong)index * sizeof(uint32_t),
                output_words[index],
                GETPC());
        }
    }

    return status;
}
