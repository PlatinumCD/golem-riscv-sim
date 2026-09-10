#include "network/rx/receiveQueueOrder.h"
#include <cassert>
#include <deque>

struct Burst { int frame, part; bool softwareVisible; };
int main()
{
    using SST::Mittens::receivePayloadInsertionPoint;
    auto match = [](const Burst& b) { return b.frame == 1; };
    // Another source's header precedes two private prefix pieces.
    // The final suffix must not jump ahead of either prefix.
    std::deque<Burst> q{{2,0,true},{1,1,false},{3,0,true},{1,2,false}};
    q.insert(receivePayloadInsertionPoint(q,match),{1,3,false});
    int expected=1;
    for (const auto& b:q) if (match(b)) assert(b.part==expected++);
    assert(expected==4);
    // Non-streaming header -> payload remains intact.
    q={{1,0,true},{2,0,true}};
    q.insert(receivePayloadInsertionPoint(q,match),{1,1,false});
    assert(q[1].frame==1 && q[1].part==1 && q[2].frame==2);
    // Already-published prefix: retain payload-before-next-header admission.
    q={{2,1,false},{3,0,true}};
    q.insert(receivePayloadInsertionPoint(q,match),{1,2,false});
    assert(q[1].frame==1 && q[2].frame==3);
    q.clear();
    assert(receivePayloadInsertionPoint(q,match)==q.end());
}
