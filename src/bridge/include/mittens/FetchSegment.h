#ifndef MITTENS_FETCH_SEGMENT_H
#define MITTENS_FETCH_SEGMENT_H
#include <stdint.h>

/* Deliberately narrow RV64I whitelist: no memory, control flow, CSRs, traps,
 * floating point, vector state, or variable-latency multiply/divide. */
static inline int mittens_fetch_segment_instruction(uint32_t instruction)
{
    const uint32_t opcode = instruction & 127;
    const uint32_t funct3 = (instruction >> 12) & 7;
    const uint32_t funct7 = instruction >> 25;
    if (opcode == 0x37 || opcode == 0x17) return 1; /* LUI, AUIPC */
    if (opcode == 0x13) {
        if (funct3 == 1) return (instruction >> 26) == 0;
        if (funct3 == 5) return (instruction >> 26) == 0 || (instruction >> 26) == 0x10;
        return 1;
    }
    if (opcode == 0x33)
        return funct7 == 0 || (funct7 == 0x20 && (funct3 == 0 || funct3 == 5));
    if (opcode == 0x1b)
        return funct3 == 0 || (funct3 == 1 && funct7 == 0) ||
               (funct3 == 5 && (funct7 == 0 || funct7 == 0x20));
    if (opcode == 0x3b)
        return (funct7 == 0 && (funct3 == 0 || funct3 == 1 || funct3 == 5)) ||
               (funct7 == 0x20 && (funct3 == 0 || funct3 == 5));
    return 0;
}
#endif
