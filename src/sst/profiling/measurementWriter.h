#ifndef SST_MITTENS_MEASUREMENT_WRITER_H
#define SST_MITTENS_MEASUREMENT_WRITER_H

#include "summarySnapshot.h"

namespace SST::Mittens
{

// File-only writer used by both PerformanceProfile summary overloads.
class MeasurementWriter final
{
  public:
    static constexpr unsigned schemaVersion = 1;
    // Empty directory disables output, like PerformanceProfile. Writes the
    // legacy tile-N-summary.csv and versioned tile-N-summary.json sidecar.
    // CSV cannot express availability: missing values retain compatibility
    // zero (or legacyUnknown's payload). JSON is authoritative for availability.
    static void writeSummary(std::uint32_t tileId, const std::string& outputDirectory,
                             const SummarySnapshot& snapshot,
                             const SummaryWriterDiagnostics& diagnostics = {});
};

} // namespace SST::Mittens
#endif
