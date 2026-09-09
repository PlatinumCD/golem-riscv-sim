#include "../memory/globalRAMReadiness.h"

#include <cstdlib>
#include <iostream>

namespace {

using SST::Mittens::GlobalRAMCommittedRanges;

[[noreturn]] void fail(const char* message)
{
    std::cerr << "global RAM readiness test failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void expect(bool condition, const char* message)
{
    if (!condition) {
        fail(message);
    }
}

} // namespace

int main()
{
    using PublishResult = GlobalRAMCommittedRanges::PublishResult;

    GlobalRAMCommittedRanges ranges;
    expect(!ranges.covers(100, 101), "empty set covered a byte");
    expect(
        ranges.publish(100, 200) == PublishResult::Published,
        "first publication failed");
    expect(ranges.size() == 1, "first publication did not form one interval");
    expect(ranges.covers(100, 200), "exact interval was not covered");
    expect(ranges.covers(125, 175), "contained interval was not covered");
    expect(!ranges.covers(99, 200), "left-partial interval was covered");
    expect(!ranges.covers(100, 201), "right-partial interval was covered");

    expect(
        ranges.publish(200, 250) == PublishResult::Published,
        "right-adjacent publication failed");
    expect(
        ranges.publish(50, 100) == PublishResult::Published,
        "left-adjacent publication failed");
    expect(
        ranges.size() == 1 && ranges.covers(50, 250),
        "adjacent publications did not coalesce");

    expect(
        ranges.publish(50, 250) == PublishResult::Overlap,
        "duplicate publication was accepted");
    expect(
        ranges.publish(75, 80) == PublishResult::Overlap,
        "contained overlap was accepted");
    expect(
        ranges.publish(25, 75) == PublishResult::Overlap,
        "left overlap was accepted");
    expect(
        ranges.publish(225, 275) == PublishResult::Overlap,
        "right overlap was accepted");
    expect(
        ranges.size() == 1 && ranges.covers(50, 250),
        "rejected overlap mutated committed state");

    expect(
        ranges.publish(300, 400) == PublishResult::Published,
        "disjoint publication failed");
    expect(
        ranges.size() == 2 && !ranges.covers(50, 400),
        "coverage crossed an uncommitted gap");
    expect(
        ranges.publish(250, 300) == PublishResult::Published,
        "bridge publication failed");
    expect(
        ranges.size() == 1 && ranges.covers(50, 400),
        "bridge publication did not coalesce both neighbors");
    std::uint64_t committedBegin = 0;
    std::uint64_t committedEnd = 0;
    expect(
        ranges.coveringRange(250, &committedBegin, &committedEnd) &&
            committedBegin == 50 && committedEnd == 400,
        "covering committed interval was not recovered");
    expect(
        !ranges.coveringRange(400, nullptr, nullptr),
        "half-open interval covered its end point");

    expect(
        ranges.publish(400, 400) == PublishResult::Overlap,
        "empty publication was accepted");
    expect(!ranges.covers(400, 400), "empty query was covered");

    GlobalRAMCommittedRanges otherExecution;
    expect(
        !otherExecution.covers(50, 400),
        "committed coverage leaked across execution state");

    std::cout << "global RAM readiness: PASS\n";
    return EXIT_SUCCESS;
}
