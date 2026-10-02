/* Non-speculative scalar LSU. Payload values are published only
 * after SST's timed service and ordered retirement, never at QEMU admission. */
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "hw/misc/mittens_sync.h"

bool mittens_sync_register_segment_safe(CPUState *cpu);

typedef struct ScalarPending {
    bool valid, write, fp, sign;
    uint32_t reg, bytes;
    uint64_t token, pc, address;
} ScalarPending;

static ScalarPending pending[MITTENS_SYNC_SLQ_CAPACITY];
static uint64_t next_token, fetch_mask, current_pc;

static uint64_t xreg(uint32_t r) { return r ? UINT64_C(1) << r : 0; }
static uint64_t freg(uint32_t r) { return UINT64_C(1) << (r + 32); }

static uint64_t pending_mask(void)
{
    uint64_t mask = 0;
    for (uint32_t i = 0; i < MITTENS_SYNC_SLQ_CAPACITY; ++i)
        if (pending[i].valid) mask |= UINT64_C(1) << i;
    return mask;
}

static void apply_completions(void)
{
    if (!mittens_sync_slq_enabled()) return;
    CPURISCVState *env = &RISCV_CPU(current_cpu)->env;
    MittensSyncScalarQueue *q = mittens_sync_slq_queue();
    for (uint32_t i = 0; i < q->depth; ++i) {
        MittensSyncScalarSlot *s = &q->slots[i];
        ScalarPending *p = &pending[i];
        if (mittens_sync_load_acquire(&s->state) != MITTENS_SYNC_LSQ_COMPLETE) continue;
        if (!p->valid || p->token != s->token || p->pc != s->program_counter ||
            p->address != s->address || p->bytes != s->size || s->element_bytes != p->bytes ||
            p->write != s->write || s->reserved ||
            s->destination != (p->write ? UINT32_MAX : p->reg + 32 * p->fp)) {
            mittens_sync_lsq_error("invalid scalar completion identity/metadata");
            return;
        }
        if (!p->write) {
            uint64_t value = 0;
            for (uint32_t b = 0; b < p->bytes; ++b) value |= (uint64_t)s->data[b] << (8*b);
            if (p->fp) {
                if (p->bytes == 4) value |= UINT64_C(0xffffffff00000000);
                env->fpr[p->reg] = value;
            } else if (p->reg) {
                env->gpr[p->reg] = p->sign && p->bytes < 8 ?
                    sextract64(value, 0, p->bytes * 8) : value;
            }
        }
        p->valid = false;
        mittens_sync_store_release(&s->state, MITTENS_SYNC_LSQ_FREE);
    }
}

/* Memory opcodes are also used by the joint vector/analog scoreboard. Actual
 * address safety is checked at the scalar helper before any admission. */
bool mittens_slq_memory_instruction(uint32_t i, uint32_t length)
{
    if (length == 4) {
        uint32_t op = i & 127, width = (i >> 12) & 7;
        return (op == 0x03 && width <= 6) || (op == 0x23 && width <= 3) ||
               ((op == 0x07 || op == 0x27) && (width == 2 || width == 3));
    }
    if (length == 2) {
        uint32_t q = i & 3, f = i >> 13;
        return (q == 0 || q == 2) && (f == 1 || f == 2 || f == 3 || f >= 5);
    }
    return false;
}

/* Conservative read/write register sets protect RAW and WAW. Stores captured
 * their values, so later writes to their source registers do not stall. An
 * unrecognized instruction drains the scalar queue before it can execute. */
static bool touched_registers(uint32_t i, uint32_t length, uint64_t *mask)
{
    uint32_t rd = (i >> 7) & 31, rs1 = (i >> 15) & 31, rs2 = (i >> 20) & 31;
    uint32_t f = (i >> 12) & 7;
    *mask = 0;
    if (length == 4) {
        switch (i & 127) {
        case 0x03: *mask = xreg(rs1) | xreg(rd); return true;
        case 0x23: *mask = xreg(rs1) | xreg(rs2); return true;
        case 0x13: case 0x1b: *mask = xreg(rs1) | xreg(rd); return true;
        case 0x33: case 0x3b: *mask = xreg(rs1) | xreg(rs2) | xreg(rd); return true;
        case 0x37: case 0x17: case 0x6f: *mask = xreg(rd); return true;
        case 0x67: *mask = xreg(rs1) | xreg(rd); return true;
        case 0x63: *mask = xreg(rs1) | xreg(rs2); return true;
        case 0x07: case 0x27:
            if (f == 2 || f == 3) {
                *mask = xreg(rs1) | freg((i & 127) == 0x07 ? rd : rs2);
            } else {
                /* Vector addresses are scalar; rs2 is a stride only in
                 * strided mode. Indexed vector addresses use vregs. */
                *mask = xreg(rs1);
                if (((i >> 26) & 3) == 2) *mask |= xreg(rs2);
            }
            return true;
        case 0x57:
            if (f == 7) {
                *mask = xreg(rd);
                if (i >> 30 != 3) *mask |= xreg(rs1);
                if (i >> 25 == 0x40) *mask |= xreg(rs2);
            } else {
                if (f == 4 || f == 6) *mask |= xreg(rs1);
                if (f == 5) *mask |= freg(rs1);
                if (i >> 26 == 0x10) {
                    if (f == 2 && (rs1 == 0 || rs1 == 16 || rs1 == 17))
                        *mask |= xreg(rd); /* vmv.x.s, vcpop.m, vfirst.m */
                    if (f == 1 && rs1 == 0) *mask |= freg(rd); /* vfmv.f.s */
                }
            }
            return true;
        case 0x53:
            /* Integer/FP conversions and moves may use either register file.
             * The superset avoids missed hazards without modeling FP latency. */
            *mask = xreg(rd) | xreg(rs1) | xreg(rs2) | freg(rd) | freg(rs1) | freg(rs2);
            return true;
        case 0x43: case 0x47: case 0x4b: case 0x4f:
            *mask = freg(rd) | freg(rs1) | freg(rs2) | freg(i >> 27); return true;
        case 0x0b:
            if (f != 7 || !mittens_sync_vector_analog_enabled()) return false;
            if (i >> 25 == 3) *mask = xreg(rd) | xreg(rs1) | xreg(rs2); /* mvm */
            else if (i >> 25 >= 6 && i >> 25 <= 8) *mask = xreg(rs1) | xreg(rs2);
            else return false;
            return true;
        case 0x2b:
            *mask = xreg(rd) | xreg(rs1) | xreg(rs2); return true;
        default: return false;
        }
    }
    if (length == 2) {
        uint32_t q = i & 3, fn = i >> 13;
        uint32_t a = 8 + ((i >> 7) & 7), b = 8 + ((i >> 2) & 7), r2 = (i >> 2) & 31;
        if (q == 0) {
            switch (fn) {
            case 0: *mask = xreg(2) | xreg(b); return true; /* addi4spn */
            case 1: case 5: *mask = xreg(a) | freg(b); return true;
            case 2: case 3: case 6: case 7: *mask = xreg(a) | xreg(b); return true;
            default: return false;
            }
        }
        if (q == 1) {
            if (fn <= 3) *mask = xreg(rd);
            else if (fn == 4) *mask = xreg(a) | (((i >> 10) & 3) == 3 ? xreg(b) : 0);
            else if (fn >= 6) *mask = xreg(a);
            return true;
        }
        if (q == 2) {
            switch (fn) {
            case 0: *mask = xreg(rd); return true;
            case 1: *mask = xreg(2) | freg(rd); return true;
            case 2: case 3: *mask = xreg(2) | xreg(rd); return true;
            case 4:
                if (rd == 0) return false; /* ebreak or reserved */
                *mask = xreg(rd) | xreg(r2) | ((i & 0x1000) && r2 == 0 ? xreg(1) : 0);
                return true;
            case 5: *mask = xreg(2) | freg(r2); return true;
            case 6: case 7: *mask = xreg(2) | xreg(r2); return true;
            default: return false;
            }
        }
    }
    return false;
}

void mittens_slq_before_instruction(uint64_t pc, uint32_t instruction, uint32_t length)
{
    if (!mittens_sync_slq_enabled()) return;
    apply_completions();
    current_pc = pc;
    fetch_mask = 0;
    uint64_t touched;
    bool drain = !mittens_sync_register_segment_safe(current_cpu) ||
                 !touched_registers(instruction, length, &touched);
    if (drain) fetch_mask = pending_mask();
    else for (uint32_t i = 0; i < MITTENS_SYNC_SLQ_CAPACITY; ++i) {
        ScalarPending *p = &pending[i];
        if (p->valid && !p->write &&
            (touched & (p->fp ? freg(p->reg) : xreg(p->reg)))) fetch_mask |= UINT64_C(1) << i;
    }
    MittensSyncScalarQueue *q = mittens_sync_slq_queue();
    q->wait_mask = fetch_mask;
    q->wait_reason = !fetch_mask ? MITTENS_SYNC_LSQ_WAIT_NONE :
        drain ? MITTENS_SYNC_LSQ_WAIT_DRAIN : MITTENS_SYNC_LSQ_WAIT_REGISTER;
}

void mittens_slq_after_instruction_fetch(void)
{
    if (!mittens_sync_slq_enabled()) return;
    apply_completions();
    if (pending_mask() & fetch_mask)
        mittens_sync_lsq_error("scalar destination used before timed completion");
}

void mittens_slq_drain(void)
{
    if (!mittens_sync_slq_enabled()) return;
    apply_completions();
    uint64_t mask = pending_mask();
    if (mask) {
        mittens_sync_slq_wait(mask, false);
        apply_completions();
        if (pending_mask()) mittens_sync_lsq_error("scalar drain resumed before completion");
    }
}

uint32_t helper_mittens_scalar_memory(CPURISCVState *env, target_ulong address,
                                    uint32_t metadata, target_ulong value,
                                    target_ulong pc)
{
    if (!mittens_sync_slq_enabled()) return 0;
    uint32_t reg = metadata & 31, bytes = 1u << ((metadata >> 8) & 3);
    bool sign = metadata & (1u << 10), write = metadata & (1u << 11), fp = metadata & (1u << 12);
    uint64_t physical;
    if (!mittens_sync_register_segment_safe(env_cpu(env)) || pc != current_pc ||
        (fp && (bytes < 4 || riscv_cpu_cfg(env)->ext_zfinx || riscv_cpu_cfg(env)->ext_zdinx)) ||
        !mittens_sync_lsq_memory_probe(env, address, bytes, bytes, write, GETPC(), &physical)) {
        mittens_lsq_drain();
        return 0;
    }
    apply_completions();
    MittensSyncScalarQueue *q = mittens_sync_slq_queue();
    uint32_t index;
    for (;;) {
        for (index = 0; index < q->depth; ++index)
            if (mittens_sync_load_acquire(&q->slots[index].state) == MITTENS_SYNC_LSQ_FREE) break;
        if (index < q->depth) break;
        mittens_sync_slq_wait(pending_mask(), true);
        apply_completions();
    }
    ScalarPending *p = &pending[index];
    MittensSyncScalarSlot *s = &q->slots[index];
    if (p->valid || ++next_token == 0) {
        mittens_sync_lsq_error("scalar slot reused before completion");
        return 0;
    }
    *p = (ScalarPending){ .valid=true, .write=write, .fp=fp, .sign=sign,
        .reg=reg, .bytes=bytes, .token=next_token, .pc=pc, .address=physical };
    s->token=next_token; s->program_counter=pc; s->address=physical;
    s->size=bytes; s->element_bytes=bytes; s->write=write;
    s->destination=write ? UINT32_MAX : reg+32*fp; s->reserved=0;
    if (write) for (uint32_t b=0;b<bytes;++b) s->data[b]=value>>(8*b);
    mittens_sync_store_release(&s->state, MITTENS_SYNC_LSQ_SUBMITTED);
    mittens_sync_slq_submit(index);
    uint32_t state=mittens_sync_load_acquire(&s->state);
    if (state != MITTENS_SYNC_LSQ_INFLIGHT && state != MITTENS_SYNC_LSQ_COMPLETE)
        mittens_sync_lsq_error("scalar submission resumed before admission");
    return 1;
}
