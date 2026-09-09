#include "../network/injectionOrder.h"

#include <cassert>
#include <cstdio>
#include <stdexcept>

using SST::Mittens::InjectionOrder;

int main()
{
    InjectionOrder order;
    order.enqueue(13, 0, 7); // Header.
    order.enqueue(13, 0, 128); // Payload remains queued/in flight.
    assert(!order.accepts(13, 1));
    assert(order.accepts(13, 0));
    order.returnedCredits(0, 7);
    assert(!order.accepts(13, 1)); // Header sent is not frame completion.
    order.returnedCredits(0, 127);
    assert(!order.accepts(13, 1)); // The final old flit still owns the ordering frontier.

    // Four destinations can use all four lanes concurrently. Never impose a
    // source-wide drain or hash directions onto fixed lanes.
    order.enqueue(7, 1, 32);
    order.enqueue(11, 2, 32);
    order.enqueue(17, 3, 32);
    assert(order.accepts(19, 1));
    order.enqueue(19, 1, 32);
    order.returnedCredits(0, 1);
    assert(order.accepts(13, 2)); // Safe migration after source-router forwarding.
    order.enqueue(13, 2, 8);
    order.returnedCredits(2, 32); // Credit belongs to 11, not the new 13 packet.
    assert(!order.accepts(13, 0));
    order.returnedCredits(2, 8);
    assert(order.accepts(13, 0));
    order.returnedCredits(1, 64); // A credit burst can span destinations.
    order.returnedCredits(3, 32);
    assert(order.empty());

    // Interleaving destinations on the same lane must retain all outstanding
    // pieces of each destination, including pieces behind another packet.
    order.enqueue(1, 0, 2);
    order.enqueue(2, 0, 3);
    order.enqueue(1, 0, 4);
    order.returnedCredits(0, 2);
    assert(!order.accepts(1, 1));
    try { order.enqueue(1, 1, 1); assert(false); }
    catch (const std::logic_error&) {}
    try { order.returnedCredits(0, 8); assert(false); }
    catch (const std::logic_error&) {}
    order.returnedCredits(0, 7);
    assert(order.empty());
    std::puts("injection ordering: migration, concurrent lanes, credit bursts and drain PASS");
}
