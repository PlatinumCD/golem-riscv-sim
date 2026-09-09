#include "measurementWriter.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace SST::Mittens
{

MeasurementProvenance compiledMeasurementProvenance()
{
    MeasurementProvenance result;
#ifdef MITTENS_SOURCE_TREE
    result.sourceTree = MITTENS_SOURCE_TREE;
#endif
#ifdef MITTENS_IMPLEMENTATION_ID
    result.implementationId = MITTENS_IMPLEMENTATION_ID;
#endif
    return result;
}

namespace
{

const char* status(MeasurementAvailability availability)
{
    switch (availability)
    {
    case MeasurementAvailability::Available:
        return "available";
    case MeasurementAvailability::Disabled:
        return "disabled";
    case MeasurementAvailability::NotMeasured:
        return "not_measured";
    case MeasurementAvailability::Unknown:
        return "unknown";
    }
    throw std::logic_error("invalid measurement availability");
}

void quoted(std::ostream& out, const std::string& value)
{
    constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (unsigned char ch : value)
    {
        if (ch == '"' || ch == '\\')
            out << '\\' << static_cast<char>(ch);
        else if (ch < 0x20)
            out << "\\u00" << hex[ch >> 4] << hex[ch & 15];
        else
            out << static_cast<char>(ch);
    }
    out << '"';
}

template <class T> void optionalValue(std::ostream& out, const std::optional<T>& value)
{
    out << "{\"value\":";
    if (value)
        out << *value;
    else
        out << "null";
    out << ",\"status\":\"" << (value ? "available" : "unknown") << "\"}";
}

void optionalValue(std::ostream& out, const std::optional<std::string>& value)
{
    out << "{\"value\":";
    if (value)
        quoted(out, *value);
    else
        out << "null";
    out << ",\"status\":\"" << (value ? "available" : "unknown") << "\"}";
}

struct Row
{
    std::string name;
    SummaryMetric metric;
    const char* unit;
    const char* domain;
    const char* aggregation;
    const char* scope;
};

void writeTxObservations(std::ostream& out, const TxObservationSnapshot& tx)
{
    const auto window = tx.observedCycles.value();
    for (const auto* histogram : {&tx.readyCycles, &tx.activeCycles, &tx.directionCycles})
    {
        if (!*histogram)
            continue;
        if (!window)
            throw std::invalid_argument("TX histogram requires an available observation window");
        std::uint64_t sum = 0;
        for (auto cycles : **histogram)
        {
            // Bound against the remaining window before adding: overflow and
            // conservation violations both fail without wrapping uint64.
            if (cycles > *window - sum)
                throw std::invalid_argument("TX histogram exceeds observation window");
            sum += cycles;
        }
        if (sum != *window)
            throw std::invalid_argument("TX histogram does not cover observation window");
    }
    out << "{\"unit\":\"cycle\",\"domain\":\"cpu\","
           "\"aggregation\":\"observed_state_duration\","
           "\"known_scope\":\"Service-entry and final-report observations; duration belongs to the "
           "previously observed state. Not event-complete activity or true busy time. Histograms "
           "share a window and must not be added together.\","
           "\"observed_cycles\":{\"value\":";
    if (window)
        out << *window;
    else
        out << "null";
    out << ",\"status\":";
    quoted(out, status(tx.observedCycles.availability()));
    out << ",\"unit\":\"cycle\",\"domain\":\"cpu\","
           "\"aggregation\":\"observation_window_duration\","
           "\"known_scope\":\"Sum of CPU-domain intervals between TX observations; excludes time "
           "outside the observation window.\"},"
           "\"bucket_labels\":[0,1,2,3,\"4+\"]";
    const auto histogram = [&](const char* name,
                               const std::optional<std::array<std::uint64_t, 5>>& values,
                               const char* scope)
    {
        out << ',';
        quoted(out, name);
        out << ":{\"value\":";
        if (values)
        {
            out << '[';
            for (std::size_t i = 0; i < values->size(); ++i)
            {
                if (i)
                    out << ',';
                out << (*values)[i];
            }
            out << ']';
        }
        else
            out << "null";
        auto availability =
            values ? MeasurementAvailability::Available : tx.observedCycles.availability();
        if (!values && availability == MeasurementAvailability::Available)
            availability = MeasurementAvailability::Unknown;
        out << ",\"status\":";
        quoted(out, status(availability));
        out << ",\"unit\":\"cycle\",\"domain\":\"cpu\","
               "\"aggregation\":\"observed_state_duration\",\"known_scope\":";
        quoted(out, scope);
        out << '}';
    };
    histogram("ready_cycles", tx.readyCycles,
              "Ready burst count includes assigned bursts even with empty FIFOs; bucket 4 is "
              "saturated. Includes bucket zero omitted by the legacy printed ready histogram.");
    histogram(
        "active_cycles", tx.activeCycles,
        "Active lane means holding a burst, not necessarily transmitting; bucket 4 is saturated.");
    histogram("direction_cycles", tx.directionCycles,
              "Observed ready-burst first-hop direction count, including queued and assigned "
              "bursts; not router cardinal-link utilization or proof of independent throughput; "
              "bucket 4 is saturated.");
    out << '}';
}

std::vector<Row> rows(std::uint32_t tile, const SummarySnapshot& s,
                      const SummaryWriterDiagnostics& d)
{
    // This single ordered map drives BOTH formats; semantics are the legacy
    // source semantics documented in MEASUREMENT_CONTRACT.md, not new counters.
    std::vector<Row> result{
        {"tile_id", tile, "identifier", "none", "identity",
         "Tile identifier, not an additive counter."},
        {"finish_tick", s.finishTick, "tick", "sst", "timestamp",
         "Absolute summary observation timestamp; not elapsed execution duration."},
        {"instructions", s.instructions, "instruction", "guest", "count",
         "Accumulated guest instruction accounting."},
        {"vector_instructions", s.vectorInstructions, "instruction", "guest", "count",
         "Guest vector instruction count, not vector elements."},
        {"cpu_cycles", s.cpuCycles, "cycle", "cpu", "issue_cost_sum",
         "Accumulated guest instruction-issue cost; excludes device waits and is not elapsed "
         "execution."},
        {"synchronization_grants", s.synchronizationGrants, "grant", "none", "count",
         "CPU/QEMU synchronization grants."},
        {"synchronization_events", s.synchronizationEvents, "event", "none", "count",
         "CPU/QEMU synchronization events."},
        {"network_packets", s.networkPackets, "packet", "none", "count",
         "TX successful NIC acceptance only; not TX+RX progress totals."},
        {"network_words", s.networkWords, "word_32bit", "none", "count",
         "TX successful NIC acceptance; includes raw, protocol and header words, not just "
         "payload."},
        {"network_word_hops", s.networkWordHops, "word_32bit_hop", "none", "sum",
         "TX words times geometric mesh hops; not measured router activity."},
        {"network_transit_ticks", s.networkTransitTicks, "tick", "sst", "interval_sum",
         "RX per-packet TX acceptance to RX completion; includes NIC queueing, transport, endpoint "
         "admission/dequeue and serialization; not pure wire latency."},
        {"network_endpoint_queue_ticks", s.networkQueueTicks, "tick", "sst", "interval_sum",
         "TX controller-ready to acceptance per accepted packet; excludes earlier bridge "
         "residence, may include source DMA wait, repeats burst age across fragments; some "
         "multi-lane scalar paths omit accounting."},
        {"physical_global_dma_submitted", s.physicalGlobalDMASubmitted, "request", "none", "count",
         "Tile physical global DMA submissions; not logical transfers or teardown rendezvous."},
        {"physical_global_dma_completed", s.physicalGlobalDMACompleted, "request", "none", "count",
         "Tile physical global DMA controller completions; not guest retirement or a global drain "
         "proof."},
        {"analog_commands_submitted", s.analogCommandsSubmitted, "command", "none", "count",
         "Analog device submitted commands."},
        {"analog_commands_completed", s.analogCommandsCompleted, "command", "none", "count",
         "Analog device completed commands; not guest consumption."},
        {"analog_active_cycles", s.analogActiveCycles, "cycle", "analog", "advanced_device_cycles",
         "Legacy AnalogDevice::elapsedCycles; device advancement clock, not per-array "
         "compute-service sum or whole-run elapsed time."},
        {"analog_link_beats", s.analogLinkBeats, "beat", "analog", "count",
         "Analog device serviced link beats; beat width comes from resolved configuration, not a "
         "fixed byte count."},
        {"receive_dma_active_cycles", s.receiveDMAActiveCycles, "cycle", "cpu", "service_sum",
         "RX DMA service charged at authorization after timing/invalidation; excludes invalidation "
         "and guest-ack delay. Do not sum both RX trace phases."},
        {"transmit_dma_active_cycles", s.transmitDMAActiveCycles, "cycle", "cpu", "service_sum",
         "TX SPM-beat service charged at scheduling; includes setup/bank delay, excludes frontend "
         "queueing; already included in shared SPM service."},
        {"scratchpad_service_cycles", s.scratchpadServiceCycles, "cycle", "cpu", "service_sum",
         "Shared CPU/global DMA/RX/TX scheduled completion minus service start; includes setup, "
         "beats and bank delay, excludes preceding DMA frontend queueing; not busy-time union."},
        {"scratchpad_read_service_cycles", s.scratchpadReadServiceCycles, "cycle", "cpu",
         "service_sum",
         "Read share of shared SPM service; same scheduling boundary as "
         "scratchpad_service_cycles."},
        {"scratchpad_write_service_cycles", s.scratchpadWriteServiceCycles, "cycle", "cpu",
         "service_sum",
         "Write share of shared SPM service; same scheduling boundary as "
         "scratchpad_service_cycles."},
        {"scratchpad_bank_conflicts", s.scratchpadBankConflicts, "delayed_beat", "none", "count",
         "One count per delayed scheduled bank-port beat regardless of delay length; NOT "
         "bank-delay cycles."},
        {"scratchpad_queue_cycles", s.scratchpadQueueCycles, "cycle", "cpu", "interval_sum",
         "DMA service start minus caller-supplied current cycle; excludes pre-call queueing and "
         "bank delay; RX frontend ordering can hide earlier queueing."},
        {"scratchpad_cpu_requests", s.scratchpadCPURequests, "access", "none", "count",
         "Scheduled CPU accesses; compact runs contribute original access count."},
        {"scratchpad_dma_transfers", s.scratchpadDMATransfers, "schedule_call", "none", "count",
         "scheduleDMA calls across clients: TX FIFO-fill beat, RX burst, global DMA transfer; not "
         "uniform logical transfers."},
        {"scratchpad_dma_bytes", s.scratchpadDMABytes, "byte", "none", "sum",
         "Requested DMA bytes across all shared SPM DMA clients, charged at scheduling."},
        {"transmit_blocked_ticks", s.transmitBlockedTicks, "tick", "sst", "interval_sum",
         "Closed T1 spaceToSend failure episodes; incomplete coverage of following send rejection "
         "and T2/T4 backpressure."},
        {"transmit_blocked_events", s.transmitBlockedEvents, "episode", "none", "count",
         "Closed T1 spaceToSend failure episodes, not all TX backpressure."},
        {"transmit_blocked_retries", s.transmitBlockedRetries, "attempt", "none", "count",
         "T1 failed spaceToSend attempts in closed episodes; includes first failure."},
        {"transmit_maximum_queue_occupancy", s.transmitMaximumQueueOccupancy, "queue_entry", "none",
         "maximum",
         "Maximum queue occupancy sampled in closed T1 blocked episodes; not whole-run or "
         "multi-lane FIFO occupancy."},
        {"memory_maximum_outstanding_requests", s.maximumOutstandingMemoryRequests, "request",
         "none", "maximum", "Tile pending StandardMem request-map high-water mark at issue."},
        {"memory_maximum_outstanding_reads", s.maximumOutstandingMemoryReads, "request", "none",
         "maximum", "Tile outstanding CPU timing-load high-water mark at issue."},
        {"memory_maximum_store_buffer_occupancy", s.maximumStoreBufferOccupancy, "entry", "none",
         "maximum", "Outstanding timing writes at store issue, not architectural store latency."},
        {"memory_store_buffer_full_events", s.storeBufferFullEvents, "event", "none", "count",
         "Store issues reaching configured buffer capacity; not blocked duration."},
        {"memory_vector_request_groups", s.vectorMemoryRequestGroups, "group", "none", "count",
         "Issued multi-request groups from the same dynamic vector memory instruction."},
        {"memory_vector_group_requests", s.vectorMemoryGroupRequests, "request", "none", "count",
         "Timing requests issued in vector groups, not bytes."},
        {"memory_scalar_request_groups", s.scalarMemoryRequestGroups, "group", "none", "count",
         "Issued independent scalar memory request groups."},
        {"memory_scalar_group_requests", s.scalarMemoryGroupRequests, "request", "none", "count",
         "Timing requests issued in scalar groups, not bytes."},
        {"profile_clock_regressions", d.clockRegressions, "event", "none", "count",
         "Writer-clamped reversed wait/TX-block trace intervals; only recorded when tracing is "
         "enabled."},
        {"progress_snapshots", d.progressSnapshots, "snapshot", "none", "count",
         "Progress snapshots written by this profile."},
        {"progress_watchdog_events", d.progressWatchdogEvents, "snapshot", "none", "count",
         "Written progress snapshots whose kind is watchdog."},
        {"progress_counter_flushes", d.progressCounterFlushes, "checkpoint", "none", "count",
         "Progress snapshot flush checkpoints, not model events."},
    };
    for (const auto& stop : s.stopCounts)
    {
        auto name = stop.reason;
        std::replace(name.begin(), name.end(), '-', '_');
        result.push_back({"stop_" + name, stop.metric, "event", "none", "count",
                          "CPU synchronization stop-reason accounting; includes replayed batch "
                          "steps, not necessarily distinct guest bridge traps."});
    }
    for (const auto& wait : s.waitTicks)
        result.push_back({"wait_" + wait.reason + "_ticks", wait.metric, "tick", "sst",
                          "interval_sum",
                          "CPU profiled wait intervals for this reason; not instruction-issue cost "
                          "or device service. Availability supplied by caller."});
    return result;
}

void writeFile(const std::string& path, const std::string& contents)
{
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open())
        throw std::runtime_error("could not open measurement summary '" + path + "'");
    out << contents;
    out.close();
    if (!out)
        throw std::runtime_error("could not write measurement summary '" + path + "'");
}

} // namespace

void MeasurementWriter::writeSummary(std::uint32_t tileId, const std::string& outputDirectory,
                                     const SummarySnapshot& snapshot,
                                     const SummaryWriterDiagnostics& diagnostics)
{
    if (outputDirectory.empty())
        return;
    const auto checkCompletions = [](const SummaryMetric& submitted, const SummaryMetric& completed)
    {
        if (submitted.value() && completed.value() && *completed.value() > *submitted.value())
            throw std::logic_error("tile summary completion counters exceed submissions");
    };
    checkCompletions(snapshot.physicalGlobalDMASubmitted, snapshot.physicalGlobalDMACompleted);
    checkCompletions(snapshot.analogCommandsSubmitted, snapshot.analogCommandsCompleted);
    const auto& p = snapshot.provenance;
    const auto& t = snapshot.timebase;
    for (const auto* text : {&p.sourceTree, &p.implementationId, &p.buildId,
                             &p.runManifestReference, &p.resolvedConfigReference, &t.sstTimebase})
        if (*text && (*text)->empty())
            throw std::invalid_argument("empty measurement metadata; omit unknown values");
    for (const auto* factor :
         {&t.cpuTicksPerCycle, &t.rxTicksPerCycle, &t.analogTicksPerCycle, &t.nicTicksPerCycle,
          &t.routerTicksPerCycle, &t.globalRAMTicksPerCycle})
        if (*factor && **factor == 0)
            throw std::invalid_argument("zero measurement timebase factor");

    std::ostringstream csv, json;
    csv.imbue(std::locale::classic());
    json.imbue(std::locale::classic());
    csv << "metric,value\n";
    json << "{\n\"schema\":\"mittens.summary\",\"schema_version\":" << schemaVersion
         << ",\n\"contract\":\"src/sst/profiling/"
            "MEASUREMENT_CONTRACT.md\",\n\"metadata\":{\"source_tree\":";
    optionalValue(json, p.sourceTree);
    json << ",\"implementation_id\":";
    optionalValue(json, p.implementationId);
    json << ",\"build_id\":";
    optionalValue(json, p.buildId);
    json << ",\"run_manifest_reference\":";
    optionalValue(json, p.runManifestReference);
    json << ",\"resolved_config_reference\":";
    optionalValue(json, p.resolvedConfigReference);
    json << "},\n\"timebase\":{\"sst_timebase\":";
    optionalValue(json, t.sstTimebase);
    json << ",\"factor_unit\":\"sst_ticks_per_domain_cycle\",\"domain_factors\":{\"cpu\":";
    optionalValue(json, t.cpuTicksPerCycle);
    json << ",\"rx\":";
    optionalValue(json, t.rxTicksPerCycle);
    json << ",\"analog\":";
    optionalValue(json, t.analogTicksPerCycle);
    json << ",\"nic\":";
    optionalValue(json, t.nicTicksPerCycle);
    json << ",\"router\":";
    optionalValue(json, t.routerTicksPerCycle);
    json << ",\"global_ram\":";
    optionalValue(json, t.globalRAMTicksPerCycle);
    json << "}},\n\"legacy_csv_policy\":\"Unavailable metrics use compatibility zero or an "
            "explicitly supplied legacy payload in CSV only; JSON null/status is authoritative.\","
            "\n\"known_scope\":\"Legacy Tile summary plus optional caller-supplied TX observation "
            "histograms; no new hardware counters. Service sums may overlap and cannot be added to "
            "obtain elapsed time. NIC/router statistics, whole-run busy unions and bank-delay "
            "cycles are not supplied by this schema.\","
            "\n\"tx_observations\":";
    writeTxObservations(json, snapshot.transmitObservations);
    json << ",\n\"metrics\":[\n";
    bool first = true;
    for (const auto& row : rows(tileId, snapshot, diagnostics))
    {
        // Existing CSV reason names are tokens. Reject delimiters instead of
        // silently changing the legacy format or emitting an ambiguous row.
        if (row.name.find_first_of(",\r\n\"") != std::string::npos ||
            row.name.find('\0') != std::string::npos)
            throw std::invalid_argument("summary reason is not a legacy CSV token");
        csv << row.name << ',' << row.metric.legacyValue() << '\n';
        if (!first)
            json << ",\n";
        first = false;
        json << "{\"name\":";
        quoted(json, row.name);
        json << ",\"value\":";
        if (row.metric.value())
            json << *row.metric.value();
        else
            json << "null";
        json << ",\"status\":";
        quoted(json, status(row.metric.availability()));
        json << ",\"unit\":";
        quoted(json, row.unit);
        json << ",\"domain\":";
        quoted(json, row.domain);
        json << ",\"aggregation\":";
        quoted(json, row.aggregation);
        json << ",\"known_scope\":";
        quoted(json, row.scope);
        json << '}';
    }
    json << "\n]}\n";
    // Validate and render before touching either destination. This is not a
    // transactional two-file commit; run manifests govern artifact completeness.
    std::filesystem::create_directories(outputDirectory);
    const auto prefix = outputDirectory + "/tile-" + std::to_string(tileId) + "-summary";
    writeFile(prefix + ".csv", csv.str());
    writeFile(prefix + ".json", json.str());
}

} // namespace SST::Mittens
