#include "instructionTiming.h"
#include "include/mittens/SyncTileBridge.h"
#include <cassert>
#include <iostream>
using namespace TileComponents::Riscv;
static unsigned r(unsigned op, unsigned rd, unsigned rs1, unsigned rs2, unsigned fn = 0, unsigned high = 0) {
    return op | rd << 7 | fn << 12 | rs1 << 15 | rs2 << 20 | high << 25;
}
int main() {
    auto add = decodeInstructionTiming(r(0x33, 5, 10, 11), 4, 0);
    auto independent = decodeInstructionTiming(r(0x33, 6, 12, 13), 4, 0);
    auto dependent = decodeInstructionTiming(r(0x33, 7, 5, 13), 4, 0);
    IssueConfiguration c; c.width = 2;
    InstructionIssue dual(c);
    assert(dual.issue(add, 0) == 1);
    assert(dual.earliest(dependent, 0).cycle == 1);
    assert(dual.earliest(independent, 0).cycle == 0);
    dual.issue(independent, 0);
    assert(dual.earliest(add, 0).cycle == 1);
    assert(dual.issueCycles() == 1 && dual.instructions() == 2 && dual.peakWidth() == 2);
    c.units[unsigned(IssueUnit::Integer)].latency = 3;
    c.units[unsigned(IssueUnit::Integer)].count = 1;
    InstructionIssue pipelined(c);
    assert(pipelined.issue(add, 0) == 3);
    assert(pipelined.earliest(independent, 0).cycle == 1);
    assert(pipelined.issue(independent, 1) == 4);
    assert(pipelined.earliest(dependent, 1).cycle == 3);
    assert(pipelined.earliest(add, 1).cycle == 3); // WAW, even without a RAW.
    c.units[unsigned(IssueUnit::Integer)].interval = 3;
    InstructionIssue occupied(c);
    occupied.issue(add, 0);
    assert(occupied.earliest(independent, 0).cycle == 3);
    auto zero = decodeInstructionTiming(r(0x33, 0, 0, 0), 4, 0);
    assert(zero.reads.none() && zero.writes.none());
    auto memory = decodeInstructionTiming(r(0x03, 5, 10, 0, 3), 4, 0);
    auto store = decodeInstructionTiming(r(0x23, 0, 10, 5, 3), 4, 0);
    assert(memory.unit == IssueUnit::Memory && memory.reads[10] && memory.writes[5]);
    assert(store.reads[5] && store.reads[10] && store.writes.none());
    auto fp = decodeInstructionTiming(r(0x53, 4, 1, 2), 4, 0);
    assert(fp.reads[33] && fp.reads[34] && fp.writes[36] && !fp.writes[4]);
    auto cvt = decodeInstructionTiming(r(0x53, 4, 1, 0, 0, 0x60), 4, 0);
    assert(cvt.reads[33] && cvt.writes[4] && !cvt.writes[36]);
    auto vec = decodeInstructionTiming(r(0x57, 8, 16, 24, 0, 1), 4, 2); // LMUL=4, vm=1.
    for (unsigned n = 0; n < 4; ++n) {
        assert(vec.writes[64+8+n] && vec.reads[64+16+n] && vec.reads[64+24+n]);
    }
    assert(vec.reads[96] && !vec.reads[64]);
    auto widen = decodeInstructionTiming(r(0x57, 8, 8, 20, 1, 0x25), 4, 1);
    auto narrow = decodeInstructionTiming(r(0x57, 8, 16, 20, 1, 0x25), 4, 1);
    assert(widen.writes[75] && widen.reads[85] && !widen.reads[86]);
    assert(narrow.reads[87] && narrow.writes[73] && !narrow.writes[74]);
    auto gather = decodeInstructionTiming(r(0x57, 8, 16, 24, 0, 0x1d), 4, 2);
    assert(gather.reads[64+23]); // EEW=16 indices span eight registers at e8,m4.
    auto vload = decodeInstructionTiming(r(0x07, 8, 10, 0, 6, 1), 4, 2 | (2 << 3));
    for (unsigned n = 0; n < 4; ++n) assert(vload.writes[64+8+n]);
    auto compressed = decodeInstructionTiming(0x0285, 2, 0); // c.addi t0,1
    assert(compressed.reads[5] && compressed.writes[5]);
    auto fence = decodeInstructionTiming(0x0000100f, 4, 0);
    assert(fence.serialize && fence.endsFetchBlock);
    assert(pipelined.earliest(fence, 1).cycle == 4);
    for (unsigned width : {0u, 5u}) {
        c.width = width;
        bool rejected = false;
        try { InstructionIssue bad(c); } catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    std::cout << "PASS instruction issue policy: width, RAW, WAW, units, pipelines, register classes, serialization\n";
}
