//
// Torus2dTopo.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_TORUS2DTOPO_H
#define MORDRED_TORUS2DTOPO_H

// Standard headers
#include <cstdint>

// Local SST header
#include "sst_config.h"

// Local headers
#include "MordredEvents.h"
#include "TopologyAPI.h"

namespace SST::Mordred {

class TorusTopo : public TopologyAPI {

public:
  // register with the SST Core
  SST_ELI_REGISTER_SUBCOMPONENT(
    TorusTopo,
    "mordred",    // component library
    "torusTopo",  // component name
    SST_ELI_ELEMENT_VERSION( 0, 0, 1 ),
    "2D Torus Topology for NoC Router",
    SST::Mordred::TopologyAPI
  )

  // register the parameters
  SST_ELI_DOCUMENT_PARAMS(
    { "verbose", "Sets the output verbsoity", "5" },
    { "xDim", "Number of points in the X dimension", "1" },
    { "yDim", "Number of points in the Y dimension", "1" }
  )

  // register the ports
  SST_ELI_DOCUMENT_PORTS()

  // register the subcomponent slots
  SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS()

  /// TORUS2DTOPO: constructor
  TorusTopo(
    ComponentId_t          id,
    Params&                params,
    uint32_t               rtr_id,
    uint32_t               num_ports,
    uint32_t               num_local_ports,
    std::vector<uint32_t>* connected_ports
  );

  /// TORUS2DTOPO: destructor
  ~TorusTopo() override { delete output; }

  // Lifecycle functions
  void init( uint32_t phase ) final;
  void setup() final;

  int32_t getEndpointId( uint32_t portnum ) override;

  /// Get the output port for a packet
  uint32_t routePacket( uint32_t dest ) final;

  /// Return true when this router's output_port crosses the torus wrap boundary.
  /// Used by VcAllocRR to assign VC1 to wrap links, keeping VC0 deadlock-free.
  bool isWrapAroundOutput( uint32_t output_port ) const override {
    switch( output_port ) {
    case EAST:  return xId == xDim - 1;
    case WEST:  return xId == 0;
    case NORTH: return yId == yDim - 1;
    case SOUTH: return yId == 0;
    default:    return false;  // local ports
    }
  }

  /// Do routing for untimed packets; this has to handle broadcast messages
  void routeUntimedBroadcastPacket( uint32_t receive_port_id, MordredInitEvent* init_ev, std::vector<Event*>& output_events ) final;

  /// default constructor
  TorusTopo() : TopologyAPI() {}

  /// serialization
  void serialize_order( SST::Core::Serialization::serializer& ser ) override {
    SST_SER( output );
    SST_SER( rtrId );
    SST_SER( numPorts );
    SST_SER( numLocalPorts );
    SST_SER( xId );
    SST_SER( yId );
    SST_SER( yDim );
    SST_SER( halfXDim );
    SST_SER( halfYDim );
    SST_SER( perPortConnectedRtr );
  }

  /// serialization implementations
  ImplementSerializable( SST::Mordred::TorusTopo );

private:
  Output* output;

  uint32_t rtrId;
  uint32_t endptZeroId;
  uint32_t numPorts;
  uint32_t numLocalPorts;

  // Torus parameters
  static constexpr uint32_t TORUSNET_PORTS_PER_ROUTER = 4;
  uint32_t                  xId{ UINT32_MAX };
  uint32_t                  yId{ UINT32_MAX };
  uint32_t                  xDim{ UINT32_MAX };
  uint32_t                  yDim{ UINT32_MAX };
  // these will be floor(Dim/2)
  uint32_t halfXDim{ UINT32_MAX };
  uint32_t halfYDim{ UINT32_MAX };

  // Port mapping
  enum PortDirE : uint32_t { NORTH = 0, EAST = 1, SOUTH = 2, WEST = 3 };

  std::vector<uint32_t>* perPortConnectedRtr;  // Unused for this topology, but printed in setup.

  void sendBroadcast( uint32_t dir, MordredInitEvent* init_ev, std::vector<Event*>& output_events );
};

}  // namespace SST::Mordred

#endif  //MORDRED_TORUS2DTOPO_H
