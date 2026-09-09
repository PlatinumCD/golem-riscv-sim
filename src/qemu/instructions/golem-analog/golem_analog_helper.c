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

bool mittens_sync_memory_initialization_active(void);
void mittens_sync_yield_memory(
    uint64_t physical_address,
    uint32_t size,
    bool write,
    uint64_t program_counter,
    uint64_t return_address);

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
    uint32_t matrix_byte_count = 0;
    uint32_t valid_rows = 0;
    uint32_t valid_columns = 0;
    uint32_t index;

    switch (operation) {
    case MITTENS_ANALOG_OPERATION_SET_MATRIX:
        command.operand0 = rs1;
        command.operand1 = (uint32_t)rs2;
        valid_rows =
            ((uint32_t)rs2 >> MITTENS_ANALOG_SET_MATRIX_ROWS_SHIFT) &
            MITTENS_ANALOG_SET_MATRIX_SHAPE_MASK;
        valid_columns =
            ((uint32_t)rs2 >> MITTENS_ANALOG_SET_MATRIX_COLUMNS_SHIFT) &
            MITTENS_ANALOG_SET_MATRIX_SHAPE_MASK;
        if ((valid_rows == 0) != (valid_columns == 0) ||
            valid_rows > array_rows || valid_columns > array_columns) {
            return MITTENS_ANALOG_STATUS_INVALID_PAYLOAD;
        }
        matrix_word_count =
            (uint64_t)array_rows * (uint64_t)array_columns;
        if (matrix_word_count > UINT32_MAX) {
            return MITTENS_ANALOG_STATUS_INVALID_PAYLOAD;
        }
        if (valid_rows != 0) {
            matrix_word_count =
                (uint64_t)valid_rows * (uint64_t)valid_columns;
            command.reserved =
                MITTENS_ANALOG_COMMAND_FLAG_COMPACT_SET_MATRIX;
        }
        if (matrix_word_count > UINT32_MAX / sizeof(uint32_t)) {
            return MITTENS_ANALOG_STATUS_INVALID_PAYLOAD;
        }
        input_word_count = (uint32_t)matrix_word_count;
        matrix_byte_count = input_word_count * sizeof(uint32_t);
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
        if (operation == MITTENS_ANALOG_OPERATION_SET_MATRIX &&
            mittens_sync_memory_initialization_active()) {
            if (cpu_memory_rw_debug(
                    env_cpu(env),
                    rs1,
                    input_words,
                    matrix_byte_count,
                    false) != 0) {
                return MITTENS_ANALOG_STATUS_INVALID_PAYLOAD;
            }
            for (index = 0; index < input_word_count; ++index) {
                input_words[index] = le32_to_cpu(input_words[index]);
            }
            /*
             * The debug copy is the host implementation of one architectural
             * SetMatrix snapshot. Charge its exact bytes through the normal
             * aggregate initialization accounting instead of generating one
             * QEMU/SST rendezvous per scalar word.
             */
            mittens_sync_yield_memory(
                rs1, matrix_byte_count, false, 0, 0);
        } else {
            for (index = 0; index < input_word_count; ++index) {
                input_words[index] = cpu_ldl_data_ra(
                    env, rs1 + (target_ulong)index * sizeof(uint32_t),
                    GETPC());
            }
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
