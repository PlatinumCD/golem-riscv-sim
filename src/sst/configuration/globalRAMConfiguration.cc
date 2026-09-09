#include "sst_config.h"
#include "globalRAMConfiguration.h"
#include "resolvedConfiguration.h"
#include <sst/core/params.h>
#include <sst/core/output.h>
#include <algorithm>

namespace SST::Mittens
{
GlobalRAMConfiguration GlobalRAMConfiguration::read(SST::Params& params)
{
    GlobalRAMConfiguration config{
        params.find<int>("verbose", 0),
        params.find<std::uint64_t>("capacity_bytes", UINT64_C(34359738368)),
        params.find<std::uint32_t>("tile_count", 1),
        params.find<std::uint32_t>("channels", 1),
        params.find<std::uint32_t>("queue_depth", 16),
        params.find<std::uint32_t>("per_tile_queue_depth", 8),
        params.find<std::uint64_t>("setup_cycles", 8),
        params.find<std::uint32_t>("bytes_per_cycle", 32),
        params.find<std::uint32_t>("burst_bytes", 64),
        params.find<std::uint64_t>("fixed_latency_cycles", 2),
        params.find<std::uint32_t>("maximum_request_bytes", UINT32_MAX),
        params.find<std::uint32_t>("demand_write_burst", 0),
        params.find<std::uint32_t>("read_priority_burst", 0),
        params.find<std::uint32_t>("reserved_read_channels", 0),
        params.find<std::uint64_t>("progress_snapshot_interval_ms", 10000),
        params.find<std::string>("dependency_mode", "bulk_barrier"),
        params.find<std::string>("profile_output_directory", ""),
        params.find<std::string>("clock", "1GHz"),
        {},
    };
    params.find_array("active_tiles", config.active_tiles);
    if (config.active_tiles.empty())
    {
        config.active_tiles.resize(config.tile_count);
        for (std::uint32_t tile = 0; tile < config.tile_count; ++tile)
            config.active_tiles[tile] = tile;
    }
    std::sort(config.active_tiles.begin(), config.active_tiles.end());
    return config;
}

void GlobalRAMConfiguration::validate(SST::Output& output_) const
{
    if (capacity_bytes == 0 || tile_count == 0 || channels == 0 || queue_depth == 0 ||
        per_tile_queue_depth == 0 || bytes_per_cycle == 0 || burst_bytes == 0 ||
        maximum_request_bytes == 0)
    {
        output_.fatal(CALL_INFO, -1, "invalid global RAM configuration\n");
    }
    if (reserved_read_channels >= channels)
    {
        output_.fatal(CALL_INFO, -1,
                      "reserved global RAM read channels must leave at least "
                      "one write channel\n");
    }

    if (dependency_mode != "bulk_barrier" && dependency_mode != "exact_dependencies")
        output_.fatal(CALL_INFO, -1, "invalid global RAM dependency_mode: %s\n",
                      dependency_mode.c_str());
    if (std::adjacent_find(active_tiles.begin(), active_tiles.end()) != active_tiles.end())
        output_.fatal(CALL_INFO, -1, "global RAM active tile list contains a duplicate\n");
    if (active_tiles.empty())
        output_.fatal(CALL_INFO, -1, "global RAM requires at least one active tile\n");
    for (const auto tile : active_tiles)
        if (tile >= tile_count)
            output_.fatal(CALL_INFO, -1, "global RAM active tile %u is outside tile_count=%u\n",
                          tile, tile_count);
}

void GlobalRAMConfiguration::emit() const
{
    emitResolvedConfiguration(
        "global-ram", 0,
        [&](ConfigurationWriter& out)
        {
            out.add("verbose", "measurement", verbose);
            out.add("capacity_bytes", "hardware", capacity_bytes);
            out.add("tile_count", "hardware", tile_count);
            out.add("channels", "hardware", channels);
            out.add("queue_depth", "hardware", queue_depth);
            out.add("per_tile_queue_depth", "hardware", per_tile_queue_depth);
            out.add("setup_cycles", "hardware", setup_cycles);
            out.add("bytes_per_cycle", "hardware", bytes_per_cycle);
            out.add("burst_bytes", "hardware", burst_bytes);
            out.add("fixed_latency_cycles", "hardware", fixed_latency_cycles);
            out.add("maximum_request_bytes", "hardware", maximum_request_bytes);
            out.add("demand_write_burst", "hardware", demand_write_burst);
            out.add("read_priority_burst", "hardware", read_priority_burst);
            out.add("reserved_read_channels", "hardware", reserved_read_channels);
            out.add("progress_snapshot_interval_ms", "measurement", progress_snapshot_interval_ms);
            out.add("dependency_mode", "hardware", dependency_mode);
            out.add("profile_output_directory", "measurement", profile_output_directory);
            out.add("clock", "hardware", clock);
            out.add("active_tiles", "hardware", active_tiles);
        });
}
} // namespace SST::Mittens
