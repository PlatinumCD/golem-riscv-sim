#pragma once
#include <cstdint>
#include <string>
namespace SST
{
class Params;
class Output;
} // namespace SST
namespace SST::Mittens
{
struct RouterConfiguration
{
    std::uint32_t id;
    std::uint32_t mesh_width;
    std::uint32_t mesh_height;
    std::uint32_t input_buffer_flits;
    std::uint32_t pipeline_cycles;
    std::uint32_t link_width_bits;
    bool packet_burst_coalescing;
    std::uint32_t tx_streams;
    std::uint32_t rx_streams;
    int verbose;
    std::string clock;
    static RouterConfiguration read(SST::Params& params);
    void validate(SST::Output& output_) const;
    void emit() const;
};
struct NetworkInterfaceConfiguration
{
    std::uint32_t endpoint_id;
    std::uint32_t network_size;
    std::uint32_t mesh_width;
    std::uint32_t mesh_height;
    std::uint32_t tx_streams;
    std::uint32_t rx_streams;
    std::uint32_t link_width_bits;
    std::uint32_t router_buffer_flits;
    std::uint32_t injection_buffer_flits;
    bool packet_burst_coalescing;
    int verbose;
    std::string clock;
    static NetworkInterfaceConfiguration read(SST::Params& params);
    void validate(SST::Output& output_, int virtualNetworks) const;
    void emit() const;
};
} // namespace SST::Mittens
