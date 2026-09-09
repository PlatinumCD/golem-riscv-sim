#ifndef HW_MISC_MITTENS_ANALOG_H
#define HW_MISC_MITTENS_ANALOG_H

#include "hw/core/cpu.h"
#include "hw/qdev-core.h"

#include "mittens/AnalogTileBridge.h"

#define TYPE_MITTENS_ANALOG "mittens-analog"

DeviceState *mittens_analog_create(void);

bool mittens_analog_available(void);
uint32_t mittens_analog_array_count(void);
uint32_t mittens_analog_array_rows(void);
uint32_t mittens_analog_array_columns(void);

uint64_t mittens_analog_submit(
    CPUState *cpu,
    const MittensAnalogCommand *command,
    const uint32_t *input_words,
    uint32_t input_word_count,
    bool wait_for_completion,
    uint32_t *output_words,
    uint32_t output_capacity,
    uint32_t *output_word_count);

#endif
