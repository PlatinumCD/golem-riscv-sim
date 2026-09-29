//
// Torus3dTopo.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_TORUS3DTOPO_H
#define MORDRED_TORUS3DTOPO_H

// Standard headers
#include <cstdint>

// Local SST header
#include "sst_config.h"

// Local headers
#include "MordredEvents.h"
#include "TopologyAPI.h"

namespace SST::Mordred {

class Torus3DTopo : public TopologyAPI {

public:
  // register with the SST Core
  SST_ELI_REGISTER_SUBCOMPONENT(
    Torus3DTopo,
    "mordred",      // component library
    "torus3DTopo",  // component name
    SST_ELI_ELEMENT_VERSION( 0, 0, 1 ),
    "3D Torus Topology for NoC Router",
    SST::Mordred::TopologyAPI
  )

  // register the parameters
  SST_ELI_DOCUMENT_PARAMS(
    { "verbose", "Sets the output verbsoity", "5" },
    { "xDim", "Number of points in the X dimension", "1" },
    { "yDim", "Number of points in the Y dimension", "1" },
    { "zDim", "Number of points in the Z dimension", "1" },
  )

  // register the ports
  SST_ELI_DOCUMENT_PORTS()

  // register the subcomponent slots
  SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS()

  /// TORUS2DTOPO: constructor
  Torus3DTopo(
    ComponentId_t          id,
    Params&                params,
    uint32_t               rtr_id,
    uint32_t               num_ports,
    uint32_t               num_local_ports,
    std::vector<uint32_t>* connected_ports
  );

  /// TORUS2DTOPO: destructor
  ~Torus3DTopo() override { delete output; }

  // Lifecycle functions
  void init( uint32_t phase ) final;
  void setup() final;

  int32_t getEndpointId( uint32_t portnum ) override;

  /// Get the output port for a packet
  uint32_t routePacket( uint32_t dest ) final;

  /// Do routing for untimed packets; this has to handle broadcast messages
  void routeUntimedBroadcastPacket( uint32_t receive_port_id, MordredInitEvent* init_ev, std::vector<Event*>& output_events ) final;

  /// default constructor
  Torus3DTopo() : TopologyAPI() {}

  /// serialization
  void serialize_order( SST::Core::Serialization::serializer& ser ) override {
    SST_SER( output );
    SST_SER( rtrId );
    SST_SER( numPorts );
    SST_SER( numLocalPorts );
    SST_SER( xId );
    SST_SER( yId );
    SST_SER( zId );
    SST_SER( xDim );
    SST_SER( yDim );
    SST_SER( zDim );
    SST_SER( halfXDim );
    SST_SER( halfYDim );
    SST_SER( halfZDim );
    SST_SER( perPortConnectedRtr );
  }

  /// serialization implementations
  ImplementSerializable( SST::Mordred::Torus3DTopo );

private:
  Output* output;

  uint32_t rtrId;
  uint32_t endptZeroId;
  uint32_t numPorts;
  uint32_t numLocalPorts;

  // Torus parameters
  static constexpr uint32_t TORUSNET_PORTS_PER_ROUTER = 6;
  uint32_t                  xId{ UINT32_MAX };
  uint32_t                  yId{ UINT32_MAX };
  uint32_t                  zId{ UINT32_MAX };
  uint32_t                  xDim{ UINT32_MAX };
  uint32_t                  yDim{ UINT32_MAX };
  uint32_t                  zDim{ UINT32_MAX };
  // these will be floor(Dim/2)
  uint32_t halfXDim{ UINT32_MAX };
  uint32_t halfYDim{ UINT32_MAX };
  uint32_t halfZDim{ UINT32_MAX };

  // Port mapping
  // North = PlusY, East = PlusX, South = MinusY, West = MinusX
  enum PortDirE : uint32_t { NORTH = 0, EAST = 1, SOUTH = 2, WEST = 3, PLUSZ = 4, MINUSZ = 5 };

  std::vector<uint32_t>* perPortConnectedRtr;  // Unused for this topology, but printed in setup.

  void sendBroadcast( uint32_t dir, MordredInitEvent* init_ev, std::vector<Event*>& output_events );
};

}  // namespace SST::Mordred

#endif  //MORDRED_TORUS3DTOPO_H
