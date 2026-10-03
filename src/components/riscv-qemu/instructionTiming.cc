#include "instructionTiming.h"

namespace TileComponents::Riscv {
namespace {
void x(RegisterSet& set, unsigned r) { if (r) set.set(r); }
void f(RegisterSet& set, unsigned r) { set.set(32 + r); }
void v(RegisterSet& set, unsigned r, unsigned count = 1) {
    // Illegal encodings are handled by QEMU; conservatively reserve the file.
    if (!count || count > 32 || r + count > 32) { r = 0; count = 32; }
    for (unsigned n = 0; n < count; ++n) set.set(64 + r + n);
}
InstructionTiming serial() {
    InstructionTiming d; d.reads.set(); d.writes.set(); d.reads.reset(0); d.writes.reset(0); return d;
}
}

InstructionTiming decodeInstructionTiming(std::uint32_t i, unsigned bytes, std::uint64_t vtype) {
    InstructionTiming d;
    d.unit = IssueUnit::Integer; d.serialize = false; d.endsFetchBlock = false;
    const unsigned rd = (i >> 7) & 31, rs1 = (i >> 15) & 31, rs2 = (i >> 20) & 31;
    const unsigned fn = (i >> 12) & 7, op = i & 127;
    auto load = [&](unsigned base, unsigned target, bool fp) {
        d.unit = IssueUnit::Memory; x(d.reads, base);
        if (fp) f(d.writes, target); else x(d.writes, target);
    };
    auto store = [&](unsigned base, unsigned source, bool fp) {
        d.unit = IssueUnit::Memory; x(d.reads, base);
        if (fp) f(d.reads, source); else x(d.reads, source);
    };
    if (bytes == 2) {
        const unsigned q = i & 3, fnc = (i >> 13) & 7;
        const unsigned a = 8 + ((i >> 7) & 7), b = 8 + ((i >> 2) & 7), r2 = (i >> 2) & 31;
        if (q == 0) {
            if (fnc == 0) { x(d.reads, 2); x(d.writes, b); }
            else if (fnc >= 1 && fnc <= 3) load(a, b, fnc == 1);
            else if (fnc >= 5) store(a, b, fnc == 5);
            else return serial();
        } else if (q == 1) {
            if (fnc <= 1) { x(d.reads, rd); x(d.writes, rd); }
            else if (fnc == 2) x(d.writes, rd);
            else if (fnc == 3) { if (rd == 2) x(d.reads, 2); x(d.writes, rd); }
            else if (fnc == 4) {
                x(d.reads, a); x(d.writes, a);
                if (((i >> 10) & 3) == 3) x(d.reads, b);
            } else { if (fnc >= 6) x(d.reads, a); d.endsFetchBlock = true; }
        } else if (q == 2) {
            if (fnc == 0) { x(d.reads, rd); x(d.writes, rd); }
            else if (fnc >= 1 && fnc <= 3) load(2, rd, fnc == 1);
            else if (fnc >= 5) store(2, r2, fnc == 5);
            else if (fnc == 4) {
                if (!rd) return serial();
                if (!r2) { x(d.reads, rd); if (i & 0x1000) x(d.writes, 1); d.endsFetchBlock = true; }
                else { x(d.reads, r2); if (i & 0x1000) x(d.reads, rd); x(d.writes, rd); }
            } else return serial();
        } else return serial();
        return d;
    }
    if (bytes != 4 || (i & 3) != 3) return serial();
    switch (op) {
    case 0x03: load(rs1, rd, false); break;
    case 0x23: store(rs1, rs2, false); break;
    case 0x13: case 0x1b: x(d.reads, rs1); x(d.writes, rd); break;
    case 0x33: case 0x3b:
        x(d.reads, rs1); x(d.reads, rs2); x(d.writes, rd);
        if ((i >> 25) == 1) d.unit = fn >= 4 ? IssueUnit::Divide : IssueUnit::Multiply;
        break;
    case 0x37: case 0x17: x(d.writes, rd); break;
    case 0x6f: x(d.writes, rd); d.endsFetchBlock = true; break;
    case 0x67: x(d.reads, rs1); x(d.writes, rd); d.endsFetchBlock = true; break;
    case 0x63: x(d.reads, rs1); x(d.reads, rs2); d.endsFetchBlock = true; break;
    case 0x43: case 0x47: case 0x4b: case 0x4f:
        d.unit = IssueUnit::Floating;
        f(d.reads, rs1); f(d.reads, rs2); f(d.reads, i >> 27); f(d.writes, rd); break;
    case 0x53: {
        d.unit = IssueUnit::Floating;
        const unsigned funct = i >> 25, family = funct & ~3u;
        if (family == 0x60 || family == 0x70) { f(d.reads, rs1); x(d.writes, rd); }
        else if (family == 0x68 || family == 0x78) { x(d.reads, rs1); f(d.writes, rd); }
        else if (family == 0x50) { f(d.reads, rs1); f(d.reads, rs2); x(d.writes, rd); }
        else {
            f(d.reads, rs1); f(d.writes, rd);
            if (family != 0x2c && family != 0x20) f(d.reads, rs2);
        }
        if (family == 0x0c || family == 0x2c) d.unit = IssueUnit::Divide;
        break;
    }
    case 0x07: case 0x27: {
        if (fn == 2 || fn == 3) {
            if (op == 0x07) load(rs1, rd, true); else store(rs1, rs2, true);
            break;
        }
        if (fn != 0 && fn < 5) return serial();
        d.unit = IssueUnit::Memory; x(d.reads, rs1); d.reads.set(96);
        const int lm = (vtype & 7) < 4 ? int(vtype & 7) : int(vtype & 7) - 8;
        const unsigned sew = 8u << ((vtype >> 3) & 7), eew = fn == 0 ? 8 : 8u << (fn - 4);
        const unsigned numerator = 1u << std::max(lm, 0), denominator = 1u << std::max(-lm, 0);
        auto group = [&](unsigned width) { return std::max(1u, (numerator * width + denominator * sew - 1) / (denominator * sew)); };
        const unsigned mode = (i >> 26) & 3, fields = (i >> 29) + 1;
        unsigned registers = (mode & 1 ? group(sew) : group(eew)) * fields;
        if (mode == 0 && rs2 == 8) registers = fields; // Whole-register transfers.
        if (mode == 0 && rs2 == 11) registers = 1; // Mask load/store.
        if (mode == 2) x(d.reads, rs2);
        if (mode & 1) v(d.reads, rs2, group(eew));
        if (!(i & (1u << 25))) v(d.reads, 0);
        v(op == 0x07 ? d.writes : d.reads, rd, registers);
        break;
    }
    case 0x57: {
        if (fn == 7) {
            x(d.writes, rd); d.writes.set(96);
            if ((i >> 30) != 3) x(d.reads, rs1);
            if ((i >> 25) == 0x40) x(d.reads, rs2);
            break;
        }
        d.unit = IssueUnit::Vector; d.reads.set(96);
        const unsigned lm = vtype & 7, group = lm < 4 ? 1u << lm : 1;
        const unsigned funct = i >> 26;
        if (funct == 0x10 && fn == 2 && (rs1 == 0 || rs1 == 16 || rs1 == 17)) {
            v(d.reads, rs2); x(d.writes, rd); break;
        }
        if (funct == 0x10 && fn == 1 && rs1 == 0) { v(d.reads, rs2); f(d.writes, rd); break; }
        if (funct == 0x27 && fn == 3) { v(d.reads, rs2, rs1 + 1); v(d.writes, rd, rs1 + 1); break; }
        if (funct == 0x12 && fn == 1) {
            // Unary FP/integer conversions encode a suboperation, not vs1.
            // Narrowing reads a double-width group; widening writes one.
            v(d.reads, rs2, group * (rs1 >= 16 ? 2 : 1));
            v(d.writes, rd, group * (rs1 >= 8 && rs1 < 16 ? 2 : 1));
            if (!(i & (1u << 25))) v(d.reads, 0);
            break;
        }
        if (fn == 0 || fn == 1 || fn == 2) {
            const unsigned sew = 8u << ((vtype >> 3) & 7);
            const unsigned indexGroup = fn == 0 && funct == 0x0e ?
                group * std::max(1u, 16u / sew) : group; // vrgatherei16.vv
            v(d.reads, rs1, indexGroup);
        }
        else if (fn == 4 || fn == 6) x(d.reads, rs1);
        else if (fn == 5) f(d.reads, rs1);
        // Supersets for widening/narrowing forms preserve readiness across LMUL
        // groups without relying on the active VL or assuming tail values dead.
        const bool narrow = (fn == 0 || fn == 3 || fn == 4) && funct >= 0x2c && funct <= 0x2f;
        const bool wide = funct >= 0x30;
        v(d.reads, rs2, group * (narrow || (wide && (funct & 4)) ? 2 : 1));
        v(d.writes, rd, group * (wide ? 2 : 1));
        if (!(i & (1u << 25))) v(d.reads, 0);
        break;
    }
    case 0x0b: {
        if (fn != 7) return serial();
        d.unit = IssueUnit::Control; d.endsFetchBlock = true;
        x(d.reads, rs1); x(d.reads, rs2);
        const unsigned operation = i >> 25, lm = vtype & 7, group = lm < 4 ? 1u << lm : 1;
        if (operation >= 6 && operation <= 8) {
            d.reads.set(96); v(operation == 8 ? d.writes : d.reads, rd, group);
        } else if (operation == 3 || operation == 9) x(d.writes, rd);
        else return serial();
        break;
    }
    case 0x2b:
        d.unit = IssueUnit::Control; d.endsFetchBlock = true;
        x(d.reads, rs1); x(d.reads, rs2); x(d.writes, rd); break;
    default: return serial(); // CSRs, fences, atomics, traps and unsupported classes.
    }
    return d;
}
}
