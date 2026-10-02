/* Deferred SPM vector accesses and token-owned analog register transfers. */
#include "qemu/osdep.h"
#include "cpu.h"
#include "vector_internals.h"
#include "qemu/bswap.h"
#include "hw/misc/mittens_sync.h"
#include "mittens/FetchSegment.h"

bool mittens_sync_register_segment_safe(CPUState *cpu);

typedef struct MittensPendingVectorMemory {
    bool valid, write, tail_ones;
    uint64_t token;
    uint8_t *destination;
    uint32_t reg, registers, elements, element_bytes, total_elements;
} MittensPendingVectorMemory;

/* One CPU exists in this QEMU process. No host pointers cross the bridge. */
static MittensPendingVectorMemory pending[64];
static uint64_t next_token, fetch_wait_mask, current_instruction_pc;
static uint32_t current_instruction, current_instruction_length;

typedef struct MittensPendingVectorAnalog {
    bool valid, output, source_pinned;
    uint64_t token, program_counter, array_id, element_offset;
    uint32_t operation, elements, reg, registers;
    uint32_t *destination;
} MittensPendingVectorAnalog;

static MittensPendingVectorAnalog analog_pending[MITTENS_SYNC_ASQ_CAPACITY];
static uint64_t analog_next_token, analog_fetch_wait_mask;
static bool analog_fetch_drain;


static uint64_t register_element(const void *base, uint32_t index, uint32_t bytes)
{
    switch (bytes) {
    case 1: return ((const uint8_t *)base)[H1(index)];
    case 2: return ((const uint16_t *)base)[H2(index)];
    case 4: return ((const uint32_t *)base)[H4(index)];
    case 8: return ((const uint64_t *)base)[H8(index)];
    default: g_assert_not_reached();
    }
}

static void set_register_element(void *base, uint32_t index, uint32_t bytes,
                                 uint64_t value)
{
    switch (bytes) {
    case 1: ((uint8_t *)base)[H1(index)] = value; break;
    case 2: ((uint16_t *)base)[H2(index)] = value; break;
    case 4: ((uint32_t *)base)[H4(index)] = value; break;
    case 8: ((uint64_t *)base)[H8(index)] = value; break;
    default: g_assert_not_reached();
    }
}

static void apply_lsq_completions(void)
{
    MittensSyncLoadStoreQueue *queue = mittens_sync_lsq_queue();
    if (!queue) {
        return;
    }
    for (uint32_t i = 0; i < queue->depth; ++i) {
        MittensSyncLoadStoreSlot *slot = &queue->slots[i];
        MittensPendingVectorMemory *local = &pending[i];
        if (mittens_sync_load_acquire(&slot->state) != MITTENS_SYNC_LSQ_COMPLETE) {
            continue;
        }
        if (!local->valid || slot->token != local->token ||
            slot->size != local->elements * local->element_bytes ||
            slot->write != local->write || slot->element_bytes != local->element_bytes ||
            slot->destination != (local->write ? UINT32_MAX : local->reg)) {
            mittens_sync_lsq_error("invalid deferred vector completion");
            return;
        }
        if (!local->write) {
            for (uint32_t element = 0; element < local->elements; ++element) {
                uint64_t value = 0;
                for (uint32_t byte = 0; byte < local->element_bytes; ++byte) {
                    value |= (uint64_t)slot->data[element * local->element_bytes + byte] << (8 * byte);
                }
                set_register_element(local->destination, element, local->element_bytes, value);
            }
            if (local->tail_ones) {
                for (uint32_t element = local->elements; element < local->total_elements; ++element) {
                    set_register_element(local->destination, element, local->element_bytes, UINT64_MAX);
                }
            }
        }
        local->valid = false;
        mittens_sync_store_release(&slot->state, MITTENS_SYNC_LSQ_FREE);
    }
}

static bool analog_metadata_matches(const MittensSyncAnalogQueueSlot *slot,
                                    const MittensPendingVectorAnalog *local)
{
    return local->valid && slot->token == local->token &&
           slot->program_counter == local->program_counter &&
           slot->array_id == local->array_id &&
           slot->element_offset == local->element_offset &&
           slot->operation == local->operation &&
           slot->element_count == local->elements &&
           slot->vector_register == local->reg &&
           slot->register_mask == local->registers && slot->reserved == 0;
}

static void apply_analog_completions(void)
{
    MittensSyncAnalogQueue *queue = mittens_sync_asq_queue();
    if (!queue || !mittens_sync_asq_enabled()) {
        return;
    }
    for (uint32_t i = 0; i < queue->depth; ++i) {
        MittensSyncAnalogQueueSlot *slot = &queue->slots[i];
        MittensPendingVectorAnalog *local = &analog_pending[i];
        const uint32_t state = mittens_sync_load_acquire(&slot->state);
        if (!local->valid) {
            if (state != MITTENS_SYNC_ASQ_FREE) {
                mittens_sync_lsq_error("unowned deferred analog slot");
            }
            continue;
        }
        if (!analog_metadata_matches(slot, local) ||
            (state != MITTENS_SYNC_ASQ_INFLIGHT &&
             state != MITTENS_SYNC_ASQ_COMPLETE)) {
            /* ERROR is legal only as a synchronous SUBMIT result. SST must
             * guarantee success before allowing the issuing helper to return. */
            mittens_sync_lsq_error("invalid or late deferred analog completion");
            return;
        }
        const uint32_t captured = mittens_sync_load_acquire(&slot->source_captured);
        if (captured > 1) {
            mittens_sync_lsq_error("invalid deferred analog source-capture flag");
            return;
        }
        if (captured && !local->output) {
            local->source_pinned = false;
        }
        if (state != MITTENS_SYNC_ASQ_COMPLETE) {
            continue;
        }
        if (slot->status != 0 || (!local->output && !captured)) {
            mittens_sync_lsq_error("deferred analog completion lacks payload/capture");
            return;
        }
        if (local->output) {
            /* Only active FP32 elements change; fractional/ordinary tails keep
             * their prior architectural values independently of current vtype. */
            for (uint32_t element = 0; element < local->elements; ++element) {
                local->destination[H4(element)] =
                    ldl_le_p(slot->data + element * sizeof(uint32_t));
            }
        }
        local->valid = false;
        local->source_pinned = false;
        mittens_sync_store_release(&slot->state, MITTENS_SYNC_ASQ_FREE);
    }
}

static void apply_completions(void)
{
    apply_lsq_completions();
    apply_analog_completions();
}

static uint64_t analog_pending_mask(bool full_completion)
{
    uint64_t mask = 0;
    for (uint32_t i = 0; i < MITTENS_SYNC_ASQ_CAPACITY; ++i) {
        const MittensPendingVectorAnalog *local = &analog_pending[i];
        if (local->valid &&
            (full_completion || local->output || local->source_pinned)) {
            mask |= UINT64_C(1) << i;
        }
    }
    return mask;
}

static uint64_t pending_mask(void)
{
    uint64_t mask = 0;
    for (uint32_t i = 0; i < 64; ++i) {
        if (pending[i].valid) {
            mask |= UINT64_C(1) << i;
        }
    }
    return mask;
}

void mittens_lsq_drain(void)
{
    mittens_slq_drain();
    if (!mittens_sync_lsq_enabled() && !mittens_sync_asq_enabled()) {
        return;
    }
    apply_completions();
    const uint64_t mask = pending_mask();
    if (mask) {
        mittens_sync_lsq_wait(mask, false);
        apply_completions();
        if (pending_mask()) {
            mittens_sync_lsq_error("deferred vector drain resumed before completion");
        }
    }
    const uint64_t analog_mask = analog_pending_mask(true);
    if (analog_mask) {
        mittens_sync_asq_wait(analog_mask, false);
        apply_completions();
        if (analog_pending_mask(true)) {
            mittens_sync_lsq_error("deferred analog drain resumed before completion");
        }
    }
}

/* Whole-register memory ignores vtype/VL, including vill. Ordinary memory
 * and ALU forms below use integer LMUL, with exact physical-group hazards. */
static bool vector_enabled(CPURISCVState *env)
{
    return riscv_has_ext(env, RVV) && (env->mstatus & MSTATUS_VS);
}

static bool simple_vector_configuration(CPURISCVState *env)
{
    const uint32_t sew = FIELD_EX64(env->vtype, VTYPE, VSEW);
    const uint32_t lmul = FIELD_EX64(env->vtype, VTYPE, VLMUL);
    return vector_enabled(env) && !env->vill && env->vstart == 0 &&
           lmul <= 3 && sew <= 3 && (8u << sew) <= riscv_cpu_cfg(env)->elen &&
           env->vl <= (riscv_cpu_cfg(env)->vlen / (8u << sew)) * (1u << lmul);
}

static uint32_t group_mask(uint32_t reg, uint32_t count)
{
    if (!count || count > 8 || reg % count || reg + count > 32) {
        return 0;
    }
    return ((UINT32_C(1) << count) - 1) << reg;
}

typedef struct MittensVectorMemoryInstruction {
    bool write, whole;
    uint32_t reg, registers, count, log2_element_size;
} MittensVectorMemoryInstruction;

static bool eligible_instruction(CPURISCVState *env, uint32_t instruction,
                                 uint32_t length, MittensVectorMemoryInstruction *op)
{
    const uint32_t opcode = instruction & 127;
    const uint32_t width = (instruction >> 12) & 7;
    const uint32_t eew = width == 0 ? 0 : width >= 5 ? width - 4 : UINT32_MAX;
    const uint32_t mode = (instruction >> 20) & 31;
    const uint32_t nf = (instruction >> 29) + 1;
    if (length != 4 || (opcode != 0x07 && opcode != 0x27) || eew == UINT32_MAX ||
        ((instruction >> 26) & 7) != 0 || !(instruction & (1u << 25)) ||
        !vector_enabled(env) || env->vstart != 0) {
        return false;
    }
    *op = (MittensVectorMemoryInstruction) {
        .write = opcode == 0x27, .whole = mode == 8,
        .reg = (instruction >> 7) & 31, .log2_element_size = eew,
    };
    if (op->whole) {
        if ((nf != 1 && nf != 2 && nf != 4 && nf != 8) ||
            (op->write && eew != 0)) {
            return false;
        }
        op->count = nf;
    } else {
        if (mode != 0 || nf != 1 || !simple_vector_configuration(env) ||
            FIELD_EX64(env->vtype, VTYPE, VSEW) != eew) {
            return false;
        }
        op->count = 1u << FIELD_EX64(env->vtype, VTYPE, VLMUL);
    }
    op->registers = group_mask(op->reg, op->count);
    return op->registers != 0;
}

/* Only explicitly decoded, legal non-widening integer operations proceed
 * selectively. A pending load protects each physical destination from RAW/WAW;
 * tails and masked destinations are conservatively treated as read/write. */
static bool simple_vector_alu(CPURISCVState *env, uint32_t instruction,
                              uint32_t length, uint32_t *registers)
{
    const uint32_t funct6 = instruction >> 26;
    const uint32_t funct3 = (instruction >> 12) & 7;
    const uint32_t vd = (instruction >> 7) & 31;
    const uint32_t vs1 = (instruction >> 15) & 31;
    const uint32_t vs2 = (instruction >> 20) & 31;
    const bool vm = (instruction >> 25) & 1;
    if (length != 4 || (instruction & 127) != 0x57 ||
        !simple_vector_configuration(env)) {
        return false;
    }
    const uint32_t count = 1u << FIELD_EX64(env->vtype, VTYPE, VLMUL);
    uint32_t touched, source;
    if (funct6 == 0x10 && vm && funct3 == 2 && vs1 == 0) {
        *registers = UINT32_C(1) << vs2; /* vmv.x.s: vd is a scalar. */
        return true;
    }
    if (funct6 == 0x10 && vm && funct3 == 6 && vs2 == 0) {
        *registers = UINT32_C(1) << vd; /* vmv.s.x ignores LMUL. */
        return true;
    }
    if (funct6 == 0 && funct3 == 2) { /* vredsum.vs */
        source = group_mask(vs2, count);
        if (!source) return false;
        *registers = source | (UINT32_C(1) << vd) | (UINT32_C(1) << vs1) |
                     (vm ? 0 : 1);
        return true;
    }
    touched = group_mask(vd, count);
    if (!touched) return false;
    if (funct6 == 0x17 && vm && vs2 == 0 &&
        (funct3 == 0 || funct3 == 3 || funct3 == 4)) {
        /* vmv.v.v, vmv.v.i and vmv.v.x. */
        if (funct3 == 0) {
            source = group_mask(vs1, count);
            if (!source) return false;
            touched |= source;
        }
    } else if (funct6 == 0 && (funct3 == 0 || funct3 == 3 || funct3 == 4)) {
        /* vadd.vv, vadd.vi and vadd.vx. */
        if (!vm && vd == 0) return false;
        source = group_mask(vs2, count);
        if (!source) return false;
        touched |= source;
        if (funct3 == 0) {
            source = group_mask(vs1, count);
            if (!source) return false;
            touched |= source;
        }
        if (!vm) touched |= 1;
    } else if ((funct6 == 0x0e || funct6 == 0x0f) &&
               (funct3 == 3 || funct3 == 4)) {
        /* vslideup/down.vi/vx; slideup requires disjoint groups. */
        source = group_mask(vs2, count);
        if (!source || (!vm && vd == 0) ||
            (funct6 == 0x0e && (source & touched))) return false;
        touched |= source | (vm ? 0 : 1);
    } else if (funct6 == 0x14 && vs2 == 0 && vs1 == 17 && funct3 == 2) {
        /* vid.v */
        if (!vm && vd == 0) return false;
        if (!vm) touched |= 1;
    } else {
        return false;
    }
    *registers = touched;
    return true;
}

static bool independent_vector_configuration(CPURISCVState *env,
                                              uint32_t instruction, uint32_t length)
{
    /* Completion metadata is captured at memory issue, so changing vtype/VL
     * cannot reinterpret an older load or its tail. Match QEMU's vset decoder. */
    return length == 4 && (instruction & 0x707f) == 0x7057 &&
           vector_enabled(env) && riscv_cpu_cfg(env)->ext_zve32f &&
           (!(instruction >> 31) || instruction >> 30 == 3 ||
            instruction >> 25 == 0x40);
}

static bool independent_scalar(CPURISCVState *env, uint32_t instruction,
                               uint32_t length)
{
    if (length == 4) {
        const uint32_t opcode = instruction & 127;
        const uint32_t funct3 = (instruction >> 12) & 7;
        if ((instruction >> 25) == 1 &&
            (opcode == 0x33 || (opcode == 0x3b && (funct3 == 0 || funct3 >= 4)))) {
            /* RV64 M/Zmmul arithmetic has no memory or vector operands.
             * Fetch-segment batching deliberately excludes multiply/divide;
             * that restriction is not a memory-ordering dependency. Keep
             * ordinary instruction timing and QEMU execution, and only admit
             * legal encodings with the required extension enabled. */
            return riscv_has_ext(env, RVM) ||
                   (riscv_cpu_cfg(env)->ext_zmmul && funct3 < 4);
        }
        return mittens_fetch_segment_instruction(instruction) || opcode == 0x6f ||
               (opcode == 0x67 && funct3 == 0) ||
               (opcode == 0x63 && (funct3 <= 1 || funct3 >= 4));
    }
    if (length == 2) {
        if (!riscv_has_ext(env, RVC) && !riscv_cpu_cfg(env)->ext_zca) {
            return false;
        }
        const uint32_t quadrant = instruction & 3;
        const uint32_t funct3 = instruction >> 13;
        const uint32_t rd = (instruction >> 7) & 31;
        if (quadrant == 1) {
            if (funct3 == 4) {
                /* Base C integer ALU: SRLI/SRAI/ANDI, SUB/XOR/OR/AND,
                 * SUBW/ADDW. The outer guard requires RV64. Bit-12-set
                 * neighbors other than the two word operations retain the
                 * conservative drain, including reserved and Zcb forms. */
                return ((instruction >> 10) & 3) != 3 ||
                       !(instruction & 0x1000) || ((instruction >> 5) & 3) <= 1;
            }
            /* ADDI/NOP, ADDIW, LI, J, BEQZ and BNEZ. */
            return funct3 == 0 || (funct3 == 1 && rd != 0) ||
                   funct3 == 2 || funct3 >= 5;
        }
        if (quadrant == 2) {
            if (funct3 == 0) {
                return rd != 0; /* SLLI */
            }
            if (funct3 == 4) {
                /* MV/ADD/JR/JALR; exclude EBREAK and reserved rd=0 forms. */
                return rd != 0;
            }
        }
    }
    return false;
}

/* Decode legal analog register use before fetch. Source transfers only read
 * their group; output transfers write it. Scalar mvm has no vector operand.
 * Invalid configurations retain the conservative joint-queue drain. */
static bool analog_registers(CPURISCVState *env, uint32_t instruction,
                             uint32_t length, uint32_t *reads, uint32_t *writes)
{
    const uint32_t operation = instruction >> 25;
    if (length != 4 || (instruction & 0x707f) != 0x700b ||
        !mittens_sync_vector_analog_enabled()) {
        return false;
    }
    if (operation == 3) { /* mvm: scalar array selector, no vector operands. */
        *reads = *writes = 0;
        return true;
    }
    if (operation < 6 || operation > 8) {
        return false;
    }
    const int32_t lmul = sextract32(FIELD_EX64(env->vtype, VTYPE, VLMUL), 0, 3);
    if (!vector_enabled(env) || env->vill || env->vstart != 0 ||
        FIELD_EX64(env->vtype, VTYPE, VSEW) != 2 || lmul < -3 || lmul > 3 ||
        riscv_cpu_cfg(env)->vlen > RV_VLEN_MAX ||
        env->vl > vext_get_vlmax(env_archcpu(env), env->vtype) ||
        env->vl > MITTENS_SYNC_VECTOR_ANALOG_BYTES / sizeof(uint32_t)) {
        return false;
    }
    const uint32_t registers = group_mask((instruction >> 7) & 31, 1u << MAX(lmul, 0));
    *reads = operation == 8 ? 0 : registers;
    *writes = operation == 8 ? registers : 0;
    return registers != 0;
}

void mittens_lsq_before_instruction(uint64_t pc, uint32_t instruction, uint32_t length)
{
    const bool lsq_enabled = mittens_sync_lsq_enabled();
    const bool asq_enabled = mittens_sync_asq_enabled();
    if (!lsq_enabled && !asq_enabled) {
        return;
    }
    CPURISCVState *env = &RISCV_CPU(current_cpu)->env;
    MittensSyncLoadStoreQueue *queue = mittens_sync_lsq_queue();
    MittensSyncAnalogQueue *analog = mittens_sync_asq_queue();
    MittensVectorMemoryInstruction op;
    uint32_t reads = 0, writes = 0, touched;
    bool decoded = false, drain = false;
    apply_completions();
    current_instruction = instruction;
    current_instruction_length = length;
    current_instruction_pc = pc;
    fetch_wait_mask = analog_fetch_wait_mask = 0;
    if (!mittens_sync_register_segment_safe(current_cpu)) {
        drain = true;
    } else if (eligible_instruction(env, instruction, length, &op)) {
        reads = op.write ? op.registers : 0;
        writes = op.write ? 0 : op.registers;
        decoded = true;
    } else if (simple_vector_alu(env, instruction, length, &touched)) {
        /* Conservatively classify all touched ALU registers as read/write.
         * This may wait on read/read source pins but never misses a WAR. */
        reads = writes = touched;
        decoded = true;
    } else if (analog_registers(env, instruction, length, &reads, &writes)) {
        decoded = true;
    } else if (length == 4 && (instruction & 127) == 0x2b &&
               (((instruction >> 25) == 0 && ((instruction >> 12) & 7) <= 4) ||
                ((instruction >> 25) == 1 && (((instruction >> 12) & 7) == 1 ||
                                              ((instruction >> 12) & 7) == 4)))) {
        /* Scalar network control. SST orders prior stores for send;
         * recv/wait preserve unrelated vector and analog progress. */
    } else if (mittens_sync_slq_enabled() && mittens_slq_memory_instruction(instruction, length)) {
        /* Scalar requests use their own queue and the same SPM range ordering;
         * they do not touch vector registers. Unsafe accesses drain at fallback. */
    } else if (!independent_scalar(env, instruction, length) &&
               !independent_vector_configuration(env, instruction, length)) {
        drain = true;
    }
    if (drain) {
        fetch_wait_mask = pending_mask();
        analog_fetch_wait_mask = analog_pending_mask(true);
    } else if (decoded) {
        for (uint32_t i = 0; i < MITTENS_SYNC_LSQ_CAPACITY; ++i) {
            /* Memory stores captured their source at issue; pending loads
             * protect the complete physical destination including tails. */
            if (pending[i].valid && !pending[i].write &&
                ((reads | writes) & pending[i].registers)) {
                fetch_wait_mask |= UINT64_C(1) << i;
            }
        }
        for (uint32_t i = 0; i < MITTENS_SYNC_ASQ_CAPACITY; ++i) {
            const MittensPendingVectorAnalog *local = &analog_pending[i];
            if (!local->valid) continue;
            if ((local->output && ((reads | writes) & local->registers)) ||
                (local->source_pinned && (writes & local->registers))) {
                analog_fetch_wait_mask |= UINT64_C(1) << i;
            }
        }
    }
    queue->wait_mask = fetch_wait_mask;
    queue->wait_reason = !fetch_wait_mask ? MITTENS_SYNC_LSQ_WAIT_NONE :
                        drain ? MITTENS_SYNC_LSQ_WAIT_DRAIN : MITTENS_SYNC_LSQ_WAIT_REGISTER;
    analog_fetch_drain = drain;
    analog->wait_mask = analog_fetch_wait_mask;
    analog->wait_reason = !analog_fetch_wait_mask ? MITTENS_SYNC_ASQ_WAIT_NONE :
                         drain ? MITTENS_SYNC_ASQ_WAIT_DRAIN : MITTENS_SYNC_ASQ_WAIT_REGISTER;
}

void mittens_lsq_after_instruction_fetch(void)
{
    if (!mittens_sync_lsq_enabled() && !mittens_sync_asq_enabled()) {
        return;
    }
    apply_completions();
    if (pending_mask() & fetch_wait_mask) {
        mittens_sync_lsq_error("instruction resumed with unresolved vector register dependency");
    }
    if (analog_pending_mask(analog_fetch_drain) & analog_fetch_wait_mask) {
        mittens_sync_lsq_error("instruction resumed with unresolved analog register dependency");
    }
}

/* Called only after the analog helper's architectural legality checks. */
bool mittens_asq_vector_analog(void *cpu_env, uint32_t operation,
                              uint32_t vector_register, uint64_t array_id,
                              uint64_t element_offset, uint32_t element_count,
                              uint32_t *status)
{
    if (!mittens_sync_asq_enabled()) {
        return false;
    }
    CPURISCVState *env = cpu_env;
    if (!mittens_sync_register_segment_safe(env_cpu(env))) {
        mittens_lsq_drain();
        return false;
    }
    MittensSyncAnalogQueue *queue = mittens_sync_asq_queue();
    const int32_t lmul = sextract32(FIELD_EX64(env->vtype, VTYPE, VLMUL), 0, 3);
    const uint32_t registers = group_mask(vector_register, 1u << MAX(lmul, 0));
    const uint32_t vector_bytes = riscv_cpu_cfg(env)->vlen / 8;
    uint32_t *words = (uint32_t *)((uint8_t *)env->vreg + vector_register * vector_bytes);
    uint32_t index;
    apply_completions();
    for (;;) {
        for (index = 0; index < queue->depth; ++index) {
            if (mittens_sync_load_acquire(&queue->slots[index].state) == MITTENS_SYNC_ASQ_FREE) {
                break;
            }
        }
        if (index < queue->depth) break;
        mittens_sync_asq_wait(analog_pending_mask(true), true);
        apply_completions();
    }
    MittensSyncAnalogQueueSlot *slot = &queue->slots[index];
    MittensPendingVectorAnalog *local = &analog_pending[index];
    if (local->valid || !registers || ++analog_next_token == 0) {
        mittens_sync_lsq_error("invalid deferred analog slot reuse/token");
        return true;
    }
    *local = (MittensPendingVectorAnalog) {
        .valid = true, .output = operation == MITTENS_SYNC_VECTOR_ANALOG_STORE,
        .source_pinned = operation != MITTENS_SYNC_VECTOR_ANALOG_STORE,
        .token = analog_next_token, .program_counter = current_instruction_pc,
        .array_id = array_id, .element_offset = element_offset,
        .operation = operation, .elements = element_count, .reg = vector_register,
        .registers = registers, .destination = words,
    };
    slot->token = local->token;
    slot->program_counter = local->program_counter;
    slot->array_id = array_id;
    slot->element_offset = element_offset;
    slot->operation = operation;
    slot->element_count = element_count;
    slot->vector_register = vector_register;
    slot->register_mask = registers;
    slot->status = MITTENS_SYNC_VECTOR_ANALOG_PENDING;
    slot->source_captured = 0;
    slot->reserved = 0;
    memset(slot->data, 0, sizeof(slot->data));
    if (!local->output) {
        for (uint32_t element = 0; element < element_count; ++element) {
            stl_le_p(slot->data + element * sizeof(uint32_t), words[H4(element)]);
        }
    }
    mittens_sync_store_release(&slot->state, MITTENS_SYNC_ASQ_SUBMITTED);
    mittens_sync_asq_submit(index);
    const uint32_t state = mittens_sync_load_acquire(&slot->state);
    if (!analog_metadata_matches(slot, local) ||
        (state != MITTENS_SYNC_ASQ_INFLIGHT && state != MITTENS_SYNC_ASQ_COMPLETE &&
         state != MITTENS_SYNC_ASQ_ERROR)) {
        mittens_sync_lsq_error("deferred analog submission resumed before guaranteed admission");
        return true;
    }
    if (state == MITTENS_SYNC_ASQ_ERROR) {
        if (slot->status == 0 || slot->status == MITTENS_SYNC_VECTOR_ANALOG_PENDING) {
            mittens_sync_lsq_error("deferred analog rejection has no architectural status");
            return true;
        }
        *status = slot->status;
        local->valid = false;
        local->source_pinned = false;
        mittens_sync_store_release(&slot->state, MITTENS_SYNC_ASQ_FREE);
        return true;
    }
    *status = 0;
    apply_completions();
    return true;
}

/* Admission is per physical-register beat, preserving the existing queue
 * capacity and transport ABI. The caller preflights the ENTIRE instruction
 * before submitting the first beat; no partial admission can precede a fault. */
static void submit_vector_beat(void *destination, uint64_t physical,
                               uint32_t reg, uint32_t registers, uint32_t elements,
                               uint32_t element_bytes, uint32_t total_elements,
                               bool write, bool tail_ones)
{
    apply_completions();
    MittensSyncLoadStoreQueue *queue = mittens_sync_lsq_queue();
    uint32_t index;
    for (;;) {
        for (index = 0; index < queue->depth; ++index) {
            if (mittens_sync_load_acquire(&queue->slots[index].state) == MITTENS_SYNC_LSQ_FREE) {
                break;
            }
        }
        if (index < queue->depth) {
            break;
        }
        mittens_sync_lsq_wait(pending_mask(), true);
        apply_completions();
    }
    MittensSyncLoadStoreSlot *slot = &queue->slots[index];
    MittensPendingVectorMemory *local = &pending[index];
    if (local->valid || ++next_token == 0) {
        mittens_sync_lsq_error("invalid deferred vector slot reuse/token");
        return;
    }
    *local = (MittensPendingVectorMemory) {
        .valid = true, .write = write, .tail_ones = tail_ones,
        .token = next_token, .destination = destination, .reg = reg,
        .registers = registers,
        .elements = elements, .element_bytes = element_bytes,
        .total_elements = total_elements,
    };
    slot->token = next_token;
    slot->address = physical;
    slot->program_counter = current_instruction_pc;
    slot->size = elements * element_bytes;
    slot->write = write;
    slot->destination = write ? UINT32_MAX : reg;
    slot->element_bytes = element_bytes;
    slot->reserved = 0;
    if (write) {
        for (uint32_t element = 0; element < elements; ++element) {
            const uint64_t value = register_element(destination, element, element_bytes);
            for (uint32_t byte = 0; byte < element_bytes; ++byte) {
                slot->data[element * element_bytes + byte] = value >> (8 * byte);
            }
        }
    }
    mittens_sync_store_release(&slot->state, MITTENS_SYNC_LSQ_SUBMITTED);
    mittens_sync_lsq_submit(index);
    const uint32_t state = mittens_sync_load_acquire(&slot->state);
    if (state != MITTENS_SYNC_LSQ_INFLIGHT && state != MITTENS_SYNC_LSQ_COMPLETE) {
        mittens_sync_lsq_error("deferred vector submission resumed before admission");
    }
}

bool mittens_lsq_vector_memory(void *destination, uint64_t base, void *cpu_env,
                               uint32_t desc, uint32_t log2_element_size,
                               uint32_t elements, bool write, bool whole,
                               uintptr_t retaddr)
{
    if (!mittens_sync_lsq_enabled()) {
        return false;
    }
    CPURISCVState *env = cpu_env;
    MittensVectorMemoryInstruction op;
    uint64_t physical;
    const uint32_t element_bytes = 1u << log2_element_size;
    const uint32_t vector_bytes = riscv_cpu_cfg(env)->vlen / 8;
    const uint64_t address = (base & ~env->cur_pmmask) | env->cur_pmbase;
    const uint64_t bytes = (uint64_t)elements * element_bytes;
    if (!mittens_sync_register_segment_safe(env_cpu(env)) ||
        !eligible_instruction(env, current_instruction, current_instruction_length, &op) ||
        op.write != write || op.whole != whole || op.log2_element_size != log2_element_size ||
        (whole ? (vext_nf(desc) != op.count || bytes != op.count * vector_bytes) :
                 (vext_nf(desc) != 1 || vext_lmul(desc) !=
                     FIELD_EX64(env->vtype, VTYPE, VLMUL) || elements != env->vl)) ||
        !elements || bytes > op.count * vector_bytes || vector_bytes > MITTENS_SYNC_LSQ_BYTES ||
        (((base + bytes - 1) & ~env->cur_pmmask) | env->cur_pmbase) != address + bytes - 1 ||
        destination != (uint8_t *)env->vreg + op.reg * vector_bytes ||
        !mittens_sync_lsq_memory_probe(env, address, bytes,
                                       element_bytes, write, retaddr, &physical)) {
        mittens_lsq_drain();
        return false;
    }
    const uint32_t per_register = vector_bytes / element_bytes;
    const bool tail_ones = !whole && !write && vext_vta(desc);
    for (uint32_t r = 0; r < op.count; ++r) {
        uint8_t *reg_destination = (uint8_t *)destination + r * vector_bytes;
        const uint32_t offset = r * per_register;
        if (offset >= elements) break;
        const uint32_t active = MIN(elements - offset, per_register);
        const bool last = offset + active == elements;
        const uint32_t registers = tail_ones && last ? op.count - r : 1;
        /* Attach all remaining agnostic tails to the last active load beat.
         * Tail-only registers stay protected until that completion, without
         * creating zero-byte memory requests or consuming extra queue slots. */
        const uint32_t touched = ((UINT32_C(1) << registers) - 1) << (op.reg + r);
        submit_vector_beat(reg_destination, physical + r * vector_bytes,
                           op.reg + r, touched, active, element_bytes,
                           registers * per_register, write, tail_ones);
    }
    env->vstart = 0;
    return true;
}
