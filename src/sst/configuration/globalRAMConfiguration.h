#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace SST
{
class Params;
class Output;
} // namespace SST
namespace SST::Mittens
{
struct GlobalRAMConfiguration
{
    int verbose;
    std::uint64_t capacity_bytes;
    std::uint32_t tile_count;
    std::uint32_t channels;
    std::uint32_t queue_depth;
    std::uint32_t per_tile_queue_depth;
    std::uint64_t setup_cycles;
    std::uint32_t bytes_per_cycle;
    std::uint32_t burst_bytes;
    std::uint64_t fixed_latency_cycles;
    std::uint32_t maximum_request_bytes;
    std::uint32_t demand_write_burst;
    std::uint32_t read_priority_burst;
    std::uint32_t reserved_read_channels;
    std::uint64_t progress_snapshot_interval_ms;
    std::string dependency_mode;
    std::string profile_output_directory;
    std::string clock;
    std::vector<std::uint32_t> active_tiles;
    static GlobalRAMConfiguration read(SST::Params& params);
    void validate(SST::Output& output_) const;
    void emit() const;
};
} // namespace SST::Mittens
