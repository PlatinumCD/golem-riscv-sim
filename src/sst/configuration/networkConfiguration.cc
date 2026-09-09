#include "sst_config.h"
#include "networkConfiguration.h"
#include "resolvedConfiguration.h"
#include <sst/core/params.h>
#include <sst/core/output.h>

namespace SST::Mittens
{
RouterConfiguration RouterConfiguration::read(SST::Params& params)
{
    return {
        params.find<std::uint32_t>("id"),
        params.find<std::uint32_t>("mesh_width"),
        params.find<std::uint32_t>("mesh_height"),
        params.find<std::uint32_t>("input_buffer_flits", 32),
        params.find<std::uint32_t>("pipeline_cycles", 3),
        params.find<std::uint32_t>("link_width_bits", 32),
        params.find<bool>("packet_burst_coalescing", false),
        params.find<std::uint32_t>("tx_streams", 1),
        params.find<std::uint32_t>("rx_streams", 1),
        params.find<int>("verbose", 0),
        params.find<std::string>("clock", "1GHz"),
    };
}

void RouterConfiguration::validate(SST::Output& output_) const
{
    if (rx_streams != 1 && rx_streams != 2 && rx_streams != 4)
        output_.fatal(CALL_INFO, -1, "rx_streams must be 1, 2, or 4\n");
    if (rx_streams > 1 && packet_burst_coalescing)
        output_.fatal(CALL_INFO, -1, "multi-RX requires flit-level routing\n");
    const std::uint32_t linkWidth = link_width_bits;
    const std::uint64_t meshSize = static_cast<std::uint64_t>(mesh_width) * mesh_height;
    if (mesh_width == 0 || mesh_height == 0 || meshSize > UINT32_MAX || id >= meshSize)
    {
        output_.fatal(CALL_INFO, -1, "router %u has invalid %ux%u mesh coordinates\n",
                      static_cast<unsigned>(id), static_cast<unsigned>(mesh_width),
                      static_cast<unsigned>(mesh_height));
    }
    if (linkWidth == 0 || linkWidth % 32 != 0)
    {
        output_.fatal(CALL_INFO, -1,
                      "router %u link_width_bits must be a positive multiple of 32\n",
                      static_cast<unsigned>(id));
    }
    if (input_buffer_flits == 0 || pipeline_cycles == 0 ||
        (tx_streams != 1 && tx_streams != 2 && tx_streams != 4))
    {
        output_.fatal(
            CALL_INFO, -1,
            "router %u requires positive buffer/pipeline sizes and tx_streams in {1,2,4}\n",
            static_cast<unsigned>(id));
    }
}

void RouterConfiguration::emit() const
{
    emitResolvedConfiguration("router", id,
                              [&](ConfigurationWriter& out)
                              {
                                  out.add("id", "hardware", id);
                                  out.add("mesh_width", "hardware", mesh_width);
                                  out.add("mesh_height", "hardware", mesh_height);
                                  out.add("input_buffer_flits", "hardware", input_buffer_flits);
                                  out.add("pipeline_cycles", "hardware", pipeline_cycles);
                                  out.add("link_width_bits", "hardware", link_width_bits);
                                  out.add("packet_burst_coalescing", "hardware",
                                          packet_burst_coalescing);
                                  out.add("tx_streams", "hardware", tx_streams);
            out.add("rx_streams", "hardware", rx_streams);
                                  out.add("verbose", "measurement", verbose);
                                  out.add("clock", "hardware", clock);
                              });
}

NetworkInterfaceConfiguration NetworkInterfaceConfiguration::read(SST::Params& params)
{
    return {
        params.find<std::uint32_t>("endpoint_id"),
        params.find<std::uint32_t>("network_size"),
        params.find<std::uint32_t>("mesh_width", 0),
        params.find<std::uint32_t>("mesh_height", 0),
        params.find<std::uint32_t>("tx_streams", 1),
        params.find<std::uint32_t>("rx_streams", 1),
        params.find<std::uint32_t>("link_width_bits", 32),
        params.find<std::uint32_t>("router_buffer_flits", 32),
        params.find<std::uint32_t>("injection_buffer_flits", 64),
        params.find<bool>("packet_burst_coalescing", false),
        params.find<int>("verbose", 0),
        params.find<std::string>("clock", "1GHz"),
    };
}

void NetworkInterfaceConfiguration::validate(SST::Output& output_, int virtualNetworks) const
{
    if (rx_streams != 1 && rx_streams != 2 && rx_streams != 4)
        output_.fatal(CALL_INFO, -1, "rx_streams must be 1, 2, or 4\n");
    if (rx_streams > 1 && packet_burst_coalescing)
        output_.fatal(CALL_INFO, -1, "multi-RX requires flit-level routing\n");
    const std::uint32_t linkWidth = link_width_bits;
    if (virtualNetworks != 1)
    {
        output_.fatal(CALL_INFO, -1, "endpoint %u supports exactly one virtual network\n",
                      static_cast<unsigned>(endpoint_id));
    }
    if (network_size == 0 || endpoint_id >= network_size)
    {
        output_.fatal(CALL_INFO, -1, "endpoint %u is outside network size %u\n",
                      static_cast<unsigned>(endpoint_id), static_cast<unsigned>(network_size));
    }
    if (linkWidth == 0 || linkWidth % 32 != 0 || router_buffer_flits == 0 ||
        injection_buffer_flits == 0)
    {
        output_.fatal(CALL_INFO, -1, "endpoint %u has invalid link or buffer parameters\n",
                      static_cast<unsigned>(endpoint_id));
    }
    const bool meshDimensionsProvided = mesh_width != 0 || mesh_height != 0;
    if ((tx_streams != 1 && tx_streams != 2 && tx_streams != 4) ||
        (meshDimensionsProvided &&
         (mesh_width == 0 || mesh_height == 0 ||
          static_cast<std::uint64_t>(mesh_width) * mesh_height != network_size)))
    {
        output_.fatal(CALL_INFO, -1,
                      "endpoint %u requires tx_streams in {1,2,4} and, when "
                      "provided, valid mesh dimensions\n",
                      static_cast<unsigned>(endpoint_id));
    }
}

void NetworkInterfaceConfiguration::emit() const
{
    emitResolvedConfiguration(
        "nic", endpoint_id,
        [&](ConfigurationWriter& out)
        {
            out.add("endpoint_id", "hardware", endpoint_id);
            out.add("network_size", "hardware", network_size);
            out.add("mesh_width", "hardware", mesh_width);
            out.add("mesh_height", "hardware", mesh_height);
            out.add("tx_streams", "hardware", tx_streams);
            out.add("rx_streams", "hardware", rx_streams);
            out.add("link_width_bits", "hardware", link_width_bits);
            out.add("router_buffer_flits", "hardware", router_buffer_flits);
            out.add("injection_buffer_flits", "hardware", injection_buffer_flits);
            out.add("packet_burst_coalescing", "hardware", packet_burst_coalescing);
            out.add("verbose", "measurement", verbose);
            out.add("clock", "hardware", clock);
        });
}

} // namespace SST::Mittens
