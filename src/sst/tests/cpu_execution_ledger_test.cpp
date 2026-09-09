#include "../execution/cpuExecutionLedger.h"
#include <cassert>
#include <iostream>
#include <tuple>
using SST::Mittens::CpuExecutionLedger;
static auto state(const CpuExecutionLedger& l) {
    const auto s=l.snapshot();
    return std::make_tuple(s.epoch,s.budget,s.baseline.instructions,s.baseline.vectors,
        s.region.instructions,s.region.vectors,s.total.instructions,s.total.vectors,
        s.regionCycles,s.totalCycles);
}
template<class F> static void rejects(CpuExecutionLedger& l,F fn) {
    const auto before=state(l); bool threw=false;
    try { fn(); } catch(const std::exception&) { threw=true; }
    assert(threw && before==state(l));
}
int main() {
    for (const auto width : {1U,2U,4U}) {
        // Exhaust all scalar/vector mixes in small regions and all two-way
        // transport splits. Expected occupancy is independently calculated.
        for (unsigned total=0;total<=25;++total) for(unsigned vectors=0;vectors<=total;++vectors)
            for(unsigned cut=0;cut<=total;++cut) {
                CpuExecutionLedger l(width); l.beginGrant(1,100);
                const auto firstVectors=std::min(vectors,cut);
                auto first=l.accountTo({cut,firstVectors},false).cycles.value;
                auto before=state(l);
                auto preview=l.previewTo({total,vectors}); assert(state(l)==before);
                auto last=l.accountTo({total,vectors},true).cycles.value;
                const auto expected=std::max((total+width-1)/width,vectors);
                assert(first+last==expected && last==preview.cycles.value);
                assert(l.snapshot().regionCycles==0 && l.snapshot().totalCycles==expected);
                assert(l.snapshot().total.instructions==total && l.snapshot().total.vectors==vectors);
            }
        CpuExecutionLedger l(width); l.beginGrant(1,100);
        l.accountTo({3,1},false); l.beginGrant(2,100);
        l.accountTo({2,1},true);
        assert(l.snapshot().totalCycles==std::max(5U/width+(5U%width!=0),2U));
        auto before=l.snapshot().totalCycles;
        l.accountTo({2,1},true); assert(l.snapshot().totalCycles==before);
        l.accountTo({3,1},true); assert(l.snapshot().totalCycles==before+1);
    }
    CpuExecutionLedger l(4); l.beginGrant(7,20); l.accountTo({8,2},false);
    rejects(l,[&]{l.validateCaptured(8,{9,2});});
    rejects(l,[&]{l.validateCaptured(7,{21,2});});
    rejects(l,[&]{l.accountTo({7,2},false);});
    rejects(l,[&]{l.accountTo({9,1},false);});
    rejects(l,[&]{l.accountTo({9,10},false);});
    rejects(l,[&]{l.accountTo({9,4},false);}); // valid cumulative V<=I, invalid delta
    rejects(l,[&]{l.previewTo({9,4});});
    rejects(l,[&]{l.beginGrant(8,0);});
    CpuExecutionLedger overflow(1); overflow.beginGrant(1,UINT64_MAX);
    overflow.accountTo({UINT64_MAX,UINT64_MAX},true);
    overflow.beginGrant(2,100);
    rejects(overflow,[&]{overflow.accountTo({1,1},true);});
    rejects(overflow,[&]{overflow.previewTo({1,1});});
    CpuExecutionLedger region(4); region.beginGrant(1,UINT64_MAX);
    region.accountTo({UINT64_MAX,0},false); region.beginGrant(2,100);
    rejects(region,[&]{region.accountTo({1,0},false);});
    std::cout << "CPU ledger: exhaustive mixes/splits, epochs, boundaries, atomic rejection PASS\n";
}
