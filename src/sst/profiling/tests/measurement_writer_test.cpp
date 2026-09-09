#include "../measurementWriter.h"
#include "../performanceProfile.h"
#include "expected_summary_csv.h"

// Test-only dependency already vendored by SST core. Production writer has no
// JSON-library or SST dependency. Parse independently rather than self-testing
// with the writer's escaping/rendering routines.
#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace SST::Mittens;
using Json = nlohmann::json;

static_assert(std::is_same_v<decltype(&PerformanceProfile::writeSummary),
    void (PerformanceProfile::*)(const SummarySnapshot&)>,
    "PerformanceProfile summary API must remain named-only");

namespace {

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    assert(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

Json readSummary(const std::filesystem::path& dir)
{
    return Json::parse(readFile(dir / "tile-3-summary.json"));
}

const Json& metric(const Json& summary, const std::string& name)
{
    for (const auto& row : summary.at("metrics"))
        if (row.at("name") == name) return row;
    throw std::runtime_error("missing metric " + name);
}

template<class Action>
void rejects(Action action)
{
    bool rejected = false;
    try { action(); }
    catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
}

SummarySnapshot fixture()
{
    SummarySnapshot s;
    s.finishTick = UINT64_MAX;
    s.instructions = 2;
    s.vectorInstructions = 3;
    s.cpuCycles = 4;
    s.synchronizationGrants = 5;
    s.synchronizationEvents = 6;
    s.networkPackets = 7;
    s.networkWords = 8;
    s.networkWordHops = 9;
    s.networkTransitTicks = 10;
    s.networkQueueTicks = 11;
    s.physicalGlobalDMASubmitted = 120;
    s.physicalGlobalDMACompleted = 13;
    s.analogCommandsSubmitted = 140;
    s.analogCommandsCompleted = 15;
    s.analogActiveCycles = 16;
    s.analogLinkBeats = 17;
    s.receiveDMAActiveCycles = 18;
    s.transmitDMAActiveCycles = 19;
    s.scratchpadServiceCycles = 20;
    s.scratchpadReadServiceCycles = 21;
    s.scratchpadWriteServiceCycles = 22;
    s.scratchpadBankConflicts = 23;
    s.scratchpadQueueCycles = 24;
    s.scratchpadCPURequests = 25;
    s.scratchpadDMATransfers = 26;
    s.scratchpadDMABytes = 27;
    s.transmitBlockedTicks = 28;
    s.transmitBlockedEvents = 29;
    s.transmitBlockedRetries = 30;
    s.transmitMaximumQueueOccupancy = 31;
    s.maximumOutstandingMemoryRequests = 32;
    s.maximumOutstandingMemoryReads = 33;
    s.maximumStoreBufferOccupancy = 34;
    s.storeBufferFullEvents = 35;
    s.vectorMemoryRequestGroups = 36;
    s.vectorMemoryGroupRequests = 37;
    s.scalarMemoryRequestGroups = 38;
    s.scalarMemoryGroupRequests = 39;
    s.stopCounts = {{"none", 7}, {"nic-transmit", 8}, {"nic_transmit", 9}};
    s.waitTicks = {{"none", 10}, {"nic-transmit", 11}, {"path\\wait", 12}};
    s.timebase.sstTimebase = "1ps";
    s.timebase.cpuTicksPerCycle = 1000;
    s.timebase.rxTicksPerCycle = 2000;
    s.timebase.analogTicksPerCycle = 3000;
    s.timebase.nicTicksPerCycle = 4000;
    s.timebase.routerTicksPerCycle = 5000;
    s.timebase.globalRAMTicksPerCycle = 6000;
    return s;
}

// Catch accidental reliance on process-global locale for JSON/CSV numbers.
class GroupedNumbers final : public std::numpunct<char> {
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\3"; }
};

void checkSchemaAndEquality(const std::filesystem::path& root)
{
    const auto newDir = root / "named";
    auto snapshot = fixture();
    const std::string escaped = std::string("quote\" slash\\ newline\n return\r tab\t nul")
        + '\0' + '\1' + " utf8-\xc3\xa9";
    snapshot.provenance.buildId = escaped;
    snapshot.provenance.runManifestReference = escaped;
    snapshot.provenance.resolvedConfigReference = escaped;
    const auto previousLocale = std::locale();
    std::locale::global(std::locale(previousLocale, new GroupedNumbers));
    MeasurementWriter::writeSummary(3, newDir.string(), snapshot, {1, 1, 1, 1});
    std::locale::global(previousLocale);
    const auto csv = readFile(newDir / "tile-3-summary.csv");
    // This fixture predates integration/removal of the positional API.
    // Comparing the profile with the standalone writer alone is tautological.
    assert(csv == expectedSummaryCsv);

    PerformanceProfile named;
    const auto profileDir = root / "named-profile";
    named.configure(3, profileDir.string(), true);
    named.recordWait(100, 50, "reversed", 1);
    ProgressSnapshot progress;
    progress.kind = "watchdog";
    named.recordProgressSnapshot(progress);
    named.writeSummary(snapshot);
    assert(readFile(profileDir / "tile-3-summary.csv") == expectedSummaryCsv);
    assert(metric(readSummary(profileDir), "cpu_cycles").at("status") == "available");
    assert(metric(readSummary(profileDir), "profile_clock_regressions").at("value") == 1);
    const auto json = readSummary(newDir);
    assert(json.at("schema") == "mittens.summary");
    assert(json.at("schema_version") == 1);
    assert(json.at("contract") == "src/sst/profiling/MEASUREMENT_CONTRACT.md");
    assert(json.at("metadata").at("build_id").at("value") == escaped);
    assert(json.at("metadata").at("resolved_config_reference").at("value") == escaped);
    assert(json.at("metadata").at("run_manifest_reference").at("value") == escaped);
    assert(json.at("timebase").at("sst_timebase").at("value") == "1ps");
    const auto& factors = json.at("timebase").at("domain_factors");
    assert(factors.at("cpu").at("value") == 1000);
    assert(factors.at("rx").at("value") == 2000);
    assert(factors.at("analog").at("value") == 3000);
    assert(factors.at("nic").at("value") == 4000);
    assert(factors.at("router").at("value") == 5000);
    assert(factors.at("global_ram").at("value") == 6000);
    assert(metric(json, "finish_tick").at("value").get<std::uint64_t>() == UINT64_MAX);

    std::istringstream input(csv);
    std::string line;
    std::getline(input, line);
    assert(line == "metric,value");
    std::size_t index = 0;
    for (const auto& row : json.at("metrics")) {
        assert(row.size() == 7);
        assert(row.at("status") == "available");
        for (const char* key : {"unit", "domain", "aggregation", "known_scope"})
            assert(!row.at(key).get<std::string>().empty());
        assert(row.at("value").is_number_unsigned());
        assert(static_cast<bool>(std::getline(input, line)));
        const auto comma = line.find(',');
        assert(line.substr(0, comma) == row.at("name"));
        assert(std::stoull(line.substr(comma + 1)) == row.at("value").get<std::uint64_t>());
        ++index;
    }
    assert(index == 50); // 40 fixed rows, 4 diagnostics, 6 reason rows.
    assert(!std::getline(input, line));
    const auto& metrics = json.at("metrics");
    assert(metrics.at(45).at("name") == "stop_nic_transmit");
    assert(metrics.at(46).at("name") == "stop_nic_transmit");
    assert(metrics.at(45).at("value") != metrics.at(46).at("value"));
    assert(metric(json, "wait_nic-transmit_ticks").at("value") == 11);
    assert(metric(json, "wait_path\\wait_ticks").at("value") == 12);
    assert(metric(json, "cpu_cycles").at("aggregation") == "issue_cost_sum");
    assert(metric(json, "network_transit_ticks").at("domain") == "sst");
    assert(metric(json, "receive_dma_active_cycles").at("domain") == "cpu");
    assert(metric(json, "analog_active_cycles").at("domain") == "analog");
    assert(metric(json, "scratchpad_bank_conflicts").at("unit") == "delayed_beat");
    assert(metric(json, "scratchpad_service_cycles").at("aggregation") == "service_sum");
}

void checkAvailabilityAndProvenance(const std::filesystem::path& root)
{
    // Deliberately spoof environment names; defaults must come from compiled
    // macros only. Test both macro-defined and standalone builds (README).
    assert(setenv("MITTENS_IMPLEMENTATION_ID", "environment-spoof", 1) == 0);
    assert(setenv("MITTENS_SOURCE_TREE", "environment-spoof", 1) == 0);
    SummarySnapshot s;
#ifdef MITTENS_IMPLEMENTATION_ID
    assert(s.provenance.implementationId == MITTENS_IMPLEMENTATION_ID);
#else
    assert(!s.provenance.implementationId);
#endif
#ifdef MITTENS_SOURCE_TREE
    assert(s.provenance.sourceTree == MITTENS_SOURCE_TREE);
#else
    assert(!s.provenance.sourceTree);
#endif
    s.instructions = 0; // Explicitly measured zero.
    s.analogCommandsSubmitted = SummaryMetric::disabled();
    s.analogCommandsCompleted = SummaryMetric::disabled();
    s.analogActiveCycles = SummaryMetric::disabled();
    s.analogLinkBeats = SummaryMetric::disabled();
    s.scratchpadServiceCycles = SummaryMetric::notMeasured(456);
    s.networkWords = SummaryMetric::legacyUnknown(123);
    s.transmitBlockedTicks = SummaryMetric::notMeasured(28);
    s.stopCounts = {{"missing", {}}};
    s.waitTicks = {{"disabled", SummaryMetric::disabled()}};
    const auto dir = root / "unavailable";
    MeasurementWriter::writeSummary(3, dir.string(), s);
    const auto j = readSummary(dir);
    assert(metric(j, "instructions").at("value") == 0);
    assert(metric(j, "instructions").at("status") == "available");
    for (const char* name : {"analog_commands_submitted", "analog_commands_completed",
         "analog_active_cycles", "analog_link_beats", "wait_disabled_ticks"}) {
        assert(metric(j, name).at("value").is_null());
        assert(metric(j, name).at("status") == "disabled");
    }
    assert(metric(j, "scratchpad_service_cycles").at("status") == "not_measured");
    assert(metric(j, "scratchpad_service_cycles").at("value").is_null());
    for (const char* name : {"network_words", "cpu_cycles", "progress_snapshots", "stop_missing"}) {
        assert(metric(j, name).at("status") == "unknown");
        assert(metric(j, name).at("value").is_null());
    }
    assert(readFile(dir / "tile-3-summary.csv").find("network_words,123\n") != std::string::npos);
    assert(readFile(dir / "tile-3-summary.csv").find("scratchpad_service_cycles,456\n") != std::string::npos);
    assert(readFile(dir / "tile-3-summary.csv").find("transmit_blocked_ticks,28\n") != std::string::npos);
    assert(metric(j, "transmit_blocked_ticks").at("status") == "not_measured");
    assert(metric(j, "transmit_blocked_ticks").at("value").is_null());
    for (const char* field : {"build_id", "resolved_config_reference", "run_manifest_reference"}) {
        assert(j.at("metadata").at(field).at("value").is_null());
        assert(j.at("metadata").at(field).at("status") == "unknown");
    }
    const auto checkOptionalIdentity = [&](const char* key, const std::optional<std::string>& expected) {
        const auto& actual = j.at("metadata").at(key);
        if (expected) {
            assert(actual.at("value") == *expected);
            assert(actual.at("status") == "available");
        } else {
            assert(actual.at("value").is_null());
            assert(actual.at("status") == "unknown");
        }
    };
    checkOptionalIdentity("source_tree", s.provenance.sourceTree);
    checkOptionalIdentity("implementation_id", s.provenance.implementationId);
    for (const auto& factor : j.at("timebase").at("domain_factors")) {
        assert(factor.at("value").is_null());
        assert(factor.at("status") == "unknown");
    }
    for (const char* field : {"observed_cycles", "ready_cycles", "active_cycles", "direction_cycles"}) {
        assert(j.at("tx_observations").at(field).at("value").is_null());
        assert(j.at("tx_observations").at(field).at("status") == "unknown");
    }
    PerformanceProfile noTrace;
    noTrace.configure(3, (root / "no-trace").string(), false);
    noTrace.writeSummary(s);
    const auto noTraceJson = readSummary(root / "no-trace");
    assert(metric(noTraceJson, "profile_clock_regressions").at("status") == "not_measured");
    assert(metric(noTraceJson, "profile_clock_regressions").at("value").is_null());
    assert(metric(noTraceJson, "progress_snapshots").at("value") == 0);
}

void checkTxObservations(const std::filesystem::path& root)
{
    const auto dir = root / "tx-observations";
    auto s = fixture();
    auto& tx = s.transmitObservations;
    tx.observedCycles = 10;
    tx.readyCycles = std::array<std::uint64_t, 5>{0, 1, 2, 3, 4};
    tx.activeCycles = std::array<std::uint64_t, 5>{4, 3, 2, 1, 0};
    tx.directionCycles = std::array<std::uint64_t, 5>{1, 2, 3, 4, 0};
    PerformanceProfile profile;
    profile.configure(3, dir.string(), false);
    profile.writeSummary(s);
    const auto csv = readFile(dir / "tile-3-summary.csv");
    const auto validJson = readFile(dir / "tile-3-summary.json");
    const auto j = Json::parse(validJson).at("tx_observations");
    assert(j.at("unit") == "cycle");
    assert(j.at("domain") == "cpu");
    assert(j.at("aggregation") == "observed_state_duration");
    assert(j.at("bucket_labels") == Json::array({0, 1, 2, 3, "4+"}));
    assert(j.at("observed_cycles").at("value") == 10);
    assert(j.at("ready_cycles").at("value") == Json::array({0, 1, 2, 3, 4}));
    assert(j.at("active_cycles").at("value") == Json::array({4, 3, 2, 1, 0}));
    assert(j.at("direction_cycles").at("value") == Json::array({1, 2, 3, 4, 0}));
    for (const char* field : {"ready_cycles", "active_cycles", "direction_cycles"}) {
        assert(j.at(field).at("status") == "available");
        assert(j.at(field).at("domain") == "cpu");
    }
    const auto invalid = [&](const SummarySnapshot& candidate) {
        rejects([&] { profile.writeSummary(candidate); });
        assert(readFile(dir / "tile-3-summary.csv") == csv);
        assert(readFile(dir / "tile-3-summary.json") == validJson);
    };
    // Test each histogram independently, including deficit, excess and wrap.
    for (auto member : {&TxObservationSnapshot::readyCycles,
         &TxObservationSnapshot::activeCycles, &TxObservationSnapshot::directionCycles}) {
        auto bad = s;
        bad.transmitObservations.*member = std::array<std::uint64_t, 5>{0, 0, 0, 0, 9};
        invalid(bad);
        bad.transmitObservations.*member = std::array<std::uint64_t, 5>{0, 0, 0, 0, 11};
        invalid(bad);
        bad.transmitObservations.*member = std::array<std::uint64_t, 5>{UINT64_MAX, 11, 0, 0, 0};
        invalid(bad); // Would wrap to the apparent valid total 10.
    }
    for (const auto& window : {SummaryMetric{}, SummaryMetric::disabled(), SummaryMetric::notMeasured(10)}) {
        auto bad = s;
        bad.transmitObservations.observedCycles = window;
        invalid(bad);
    }
    // Optional histograms remain absent, not reconstructed from others.
    tx.activeCycles.reset();
    profile.writeSummary(s);
    assert(readSummary(dir).at("tx_observations").at("active_cycles").at("value").is_null());
    assert(readSummary(dir).at("tx_observations").at("active_cycles").at("status") == "unknown");
    assert(readFile(dir / "tile-3-summary.csv") == csv); // JSON only.
    tx = {};
    tx.observedCycles = SummaryMetric::disabled();
    profile.writeSummary(s);
    for (const char* field : {"observed_cycles", "ready_cycles", "active_cycles", "direction_cycles"})
        assert(readSummary(dir).at("tx_observations").at(field).at("status") == "disabled");
    tx.observedCycles = 0;
    tx.readyCycles = std::array<std::uint64_t, 5>{};
    profile.writeSummary(s); // Measured empty window is legitimate.
    assert(readSummary(dir).at("tx_observations").at("ready_cycles").at("status") == "available");
    tx.observedCycles = UINT64_MAX;
    tx.readyCycles = std::array<std::uint64_t, 5>{UINT64_MAX, 0, 0, 0, 0};
    profile.writeSummary(s);
    assert(readSummary(dir).at("tx_observations").at("observed_cycles").at("value").get<std::uint64_t>() == UINT64_MAX);
}

void checkValidation(const std::filesystem::path& root)
{
    PerformanceProfile profile;
    const auto profileDir = root / "profile-invalid";
    profile.configure(3, profileDir.string(), false);
    auto invalid = fixture();
    invalid.physicalGlobalDMACompleted = 121;
    rejects([&] { profile.writeSummary(invalid); });
    invalid = fixture();
    invalid.analogCommandsCompleted = 141;
    rejects([&] { profile.writeSummary(invalid); });
    assert(!std::filesystem::exists(profileDir / "tile-3-summary.csv"));
    assert(!std::filesystem::exists(profileDir / "tile-3-summary.json"));
    const auto dir = root / "validation";
    auto s = fixture();
    MeasurementWriter::writeSummary(3, dir.string(), s);
    const auto csv = readFile(dir / "tile-3-summary.csv");
    const auto json = readFile(dir / "tile-3-summary.json");
    const auto rejectUnchanged = [&](const SummarySnapshot& invalid) {
        rejects([&] { MeasurementWriter::writeSummary(3, dir.string(), invalid); });
        assert(readFile(dir / "tile-3-summary.csv") == csv);
        assert(readFile(dir / "tile-3-summary.json") == json);
    };
    s.physicalGlobalDMACompleted = 121;
    rejectUnchanged(s);
    s = fixture();
    s.analogCommandsCompleted = 141;
    rejectUnchanged(s);
    s = fixture();
    s.timebase.cpuTicksPerCycle = 0;
    rejectUnchanged(s);
    s = fixture();
    s.provenance.buildId = "";
    rejectUnchanged(s);
    for (const auto& reason : {std::string("comma,name"), std::string("line\nbreak"),
         std::string("quote\"name"), std::string("nul\0name", 8)}) {
        s = fixture();
        s.stopCounts.push_back({reason, 0});
        rejectUnchanged(s);
    }
    // Disabled output stays a no-op even for invalid snapshots.
    MeasurementWriter::writeSummary(3, "", s);
}

} // namespace

int main()
{
    char directoryTemplate[] = "/tmp/mittens-measurement-writer-XXXXXX";
    const char* directory = mkdtemp(directoryTemplate);
    assert(directory);
    const std::filesystem::path root(directory);
    checkSchemaAndEquality(root);
    checkAvailabilityAndProvenance(root);
    checkTxObservations(root);
    checkValidation(root);
    std::cout << "measurement writer: PASS; evidence " << root << '\n';
}
