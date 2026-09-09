#ifndef SST_MITTENS_SUMMARY_SNAPSHOT_H
#define SST_MITTENS_SUMMARY_SNAPSHOT_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace SST::Mittens
{

enum class MeasurementAvailability
{
    Available,
    Disabled,
    NotMeasured,
    Unknown
};

// A supplied integer is an explicitly measured value (including zero).
// Default construction is unknown, never a measured zero. The compatibility
// payload is for CSV only: legacy callers cannot establish resource availability.
class SummaryMetric final
{
  public:
    SummaryMetric() = default;
    SummaryMetric(std::uint64_t value)
        : value_(value), availability_(MeasurementAvailability::Available)
    {
    }
    static SummaryMetric disabled()
    {
        return missing(MeasurementAvailability::Disabled);
    }
    static SummaryMetric notMeasured(std::uint64_t legacyPayload = 0)
    {
        SummaryMetric result = missing(MeasurementAvailability::NotMeasured);
        result.value_ = legacyPayload;
        return result;
    }
    static SummaryMetric legacyUnknown(std::uint64_t value)
    {
        SummaryMetric result;
        result.value_ = value;
        return result;
    }
    std::optional<std::uint64_t> value() const noexcept
    {
        return availability_ == MeasurementAvailability::Available
                   ? std::optional<std::uint64_t>{value_}
                   : std::nullopt;
    }
    MeasurementAvailability availability() const noexcept
    {
        return availability_;
    }
    std::uint64_t legacyValue() const noexcept
    {
        return value_;
    }

  private:
    static SummaryMetric missing(MeasurementAvailability availability)
    {
        SummaryMetric result;
        result.availability_ = availability;
        return result;
    }
    std::uint64_t value_ = 0;
    MeasurementAvailability availability_ = MeasurementAvailability::Unknown;
};

struct MeasurementProvenance final
{
    std::optional<std::string> sourceTree;
    std::optional<std::string> implementationId;
    // References, not identities inferred from paths/environment. The full
    // binary/toolchain identity remains the responsibility of the run manifest.
    std::optional<std::string> buildId;
    std::optional<std::string> runManifestReference;
    std::optional<std::string> resolvedConfigReference;
};

// Uses build definitions MITTENS_SOURCE_TREE / MITTENS_IMPLEMENTATION_ID.
// Standalone builds leave them unknown. Never consults environment variables.
MeasurementProvenance compiledMeasurementProvenance();

struct MeasurementTimebase final
{
    // Exact SST timebase text, e.g. "1ps". No implicit seconds conversion.
    std::optional<std::string> sstTimebase;
    // Exact resolved SST converter factors; absent is unknown, zero is invalid.
    std::optional<std::uint64_t> cpuTicksPerCycle;
    std::optional<std::uint64_t> rxTicksPerCycle;
    std::optional<std::uint64_t> analogTicksPerCycle;
    std::optional<std::uint64_t> nicTicksPerCycle;
    std::optional<std::uint64_t> routerTicksPerCycle;
    std::optional<std::uint64_t> globalRAMTicksPerCycle;
};

struct NamedSummaryMetric final
{
    std::string reason;
    SummaryMetric metric;
};

// Caller copies these observations from TxController, including bucket zero.
// Bucket 4 is saturated (4 or more), matching the controller's min(count, 4).
// Each supplied histogram must sum exactly to the available observedCycles.
// Missing histograms are unknown, or inherit disabled/not_measured from the
// window. They are never inferred from the other histograms or printed reports.
struct TxObservationSnapshot final
{
    SummaryMetric observedCycles;
    std::optional<std::array<std::uint64_t, 5>> readyCycles;
    std::optional<std::array<std::uint64_t, 5>> activeCycles;
    std::optional<std::array<std::uint64_t, 5>> directionCycles;
};

// An owned value snapshot assembled by the caller from resource-owned
// snapshots. This type and the writer neither poll nor mutate controllers.
struct SummarySnapshot final
{
    MeasurementProvenance provenance = compiledMeasurementProvenance();
    MeasurementTimebase timebase;
    TxObservationSnapshot transmitObservations;
    SummaryMetric finishTick;
    SummaryMetric instructions;
    SummaryMetric vectorInstructions;
    SummaryMetric cpuCycles;
    SummaryMetric synchronizationGrants;
    SummaryMetric synchronizationEvents;
    SummaryMetric networkPackets;
    SummaryMetric networkWords;
    SummaryMetric networkWordHops;
    SummaryMetric networkTransitTicks;
    SummaryMetric networkQueueTicks;
    SummaryMetric physicalGlobalDMASubmitted;
    SummaryMetric physicalGlobalDMACompleted;
    SummaryMetric analogCommandsSubmitted;
    SummaryMetric analogCommandsCompleted;
    SummaryMetric analogActiveCycles;
    SummaryMetric analogLinkBeats;
    SummaryMetric receiveDMAActiveCycles;
    SummaryMetric transmitDMAActiveCycles;
    SummaryMetric scratchpadServiceCycles;
    SummaryMetric scratchpadReadServiceCycles;
    SummaryMetric scratchpadWriteServiceCycles;
    SummaryMetric scratchpadBankConflicts;
    SummaryMetric scratchpadQueueCycles;
    SummaryMetric scratchpadCPURequests;
    SummaryMetric scratchpadDMATransfers;
    SummaryMetric scratchpadDMABytes;
    SummaryMetric transmitBlockedTicks;
    SummaryMetric transmitBlockedEvents;
    SummaryMetric transmitBlockedRetries;
    SummaryMetric transmitMaximumQueueOccupancy;
    SummaryMetric maximumOutstandingMemoryRequests;
    SummaryMetric maximumOutstandingMemoryReads;
    SummaryMetric maximumStoreBufferOccupancy;
    SummaryMetric storeBufferFullEvents;
    SummaryMetric vectorMemoryRequestGroups;
    SummaryMetric vectorMemoryGroupRequests;
    SummaryMetric scalarMemoryRequestGroups;
    SummaryMetric scalarMemoryGroupRequests;
    // Order and duplicates are preserved. Stop names alone replace '-' with
    // '_', matching the legacy CSV. Wait names keep their original spelling.
    std::vector<NamedSummaryMetric> stopCounts;
    std::vector<NamedSummaryMetric> waitTicks;
};

// These are writer-owned diagnostics, not new model/resource counters.
struct SummaryWriterDiagnostics final
{
    SummaryMetric clockRegressions;
    SummaryMetric progressSnapshots;
    SummaryMetric progressWatchdogEvents;
    SummaryMetric progressCounterFlushes;
};

} // namespace SST::Mittens
#endif
