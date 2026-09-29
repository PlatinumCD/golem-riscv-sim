//
// FlatButterflyTopo.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_FLATBUTTERFLYTOPO_H
#define MORDRED_FLATBUTTERFLYTOPO_H

// Standard headers
#include <cstdint>
#include <vector>

// Local SST header
#include "sst_config.h"

// Local headers
#include "MordredEvents.h"
#include "TopologyAPI.h"

namespace SST::Mordred {

class FlatButterflyTopo : public TopologyAPI {

public:
  // register with the SST Core
  SST_ELI_REGISTER_SUBCOMPONENT(
    FlatButterflyTopo,
    "mordred",             // component library
    "flattenedButterfly",  // component name
    SST_ELI_ELEMENT_VERSION( 0, 0, 1 ),
    "Flattened Butterfly Topology for NoC Router",
    SST::Mordred::TopologyAPI
  )

  // register the parameters
  SST_ELI_DOCUMENT_PARAMS(
    { "verbose", "Sets the output verbsoity", "5" },
    { "k", "k-ary value of the network (REQUIRED)", nullptr },
    { "n", "n-fly value of the network (REQUIRED)", nullptr }
  )

  // register the ports
  SST_ELI_DOCUMENT_PORTS()

  // register the subcomponent slots
  SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS()

  /// FlatButterflyTopo: constructor
  FlatButterflyTopo(
    ComponentId_t          id,
    Params&                params,
    uint32_t               rtr_id,
    uint32_t               num_ports,
    uint32_t               num_local_ports,
    std::vector<uint32_t>* connected_ports
  );

  /// FlatButterflyTopo: destructor
  ~FlatButterflyTopo() override { delete output; }

  /// FlatButterflyTopo: necessary lifecycle functions
  void init( uint32_t phase ) final;
  //void setup() final {};

  int32_t getEndpointId( uint32_t portnum ) override;

  /// Get the output port for a flit -- dest should be the destination endpoint
  uint32_t routePacket( uint32_t dest ) final;

  /// Do routing for untimed packets; this has to handle broadcast messages
  __attribute__( ( noreturn ) ) void
    routeUntimedBroadcastPacket( uint32_t receive_port_id, MordredInitEvent* init_ev, std::vector<Event*>& output_events ) final;

  /// default constructor
  FlatButterflyTopo() : SST::Mordred::TopologyAPI() {}

  /// serialization
  void serialize_order( SST::Core::Serialization::serializer& ser ) override {
    SST_SER( output );
    SST_SER( k );
    SST_SER( n );
    SST_SER( rtrId );
    SST_SER( numPorts );
    SST_SER( numLocalPorts );
    SST_SER( numRtrPorts );
    SST_SER( myAddress );
    SST_SER( perPortConnectedRtr );
    SST_SER( connectedRtrsByBase );
  }

  /// serialization implementations
  ImplementSerializable( SST::Mordred::FlatButterflyTopo );

private:
  Output* output;

  uint32_t              k{ UINT32_MAX };
  uint32_t              n{ UINT32_MAX };
  uint32_t              rtrId;
  uint32_t              numPorts;
  uint32_t              numLocalPorts;
  uint32_t              numRtrPorts;
  std::vector<uint32_t> myAddress;

  std::vector<uint32_t>*             perPortConnectedRtr;  //owned by MordredRouter
  std::vector<std::vector<uint32_t>> connectedRtrsByBase;  // sized to numRtrPorts

  // In the returned vector, index[0] has the least significant digit which is generally ignored by the
  // routing algorithm (in theory, it should be the ID of the local endpoint to use if this router holds our end
  // destination)
  std::vector<uint32_t> convertBase( uint32_t num );
  bool                  isLocalAddr( std::vector<uint32_t>& dest_addr );

  // For each connected router, how many hops from it to the destination router;
  // The destination router is 0 hops away from the desired endpoint
  uint32_t calcDist( uint32_t idx, std::vector<uint32_t>& dest_addr );
};

}  // namespace SST::Mordred

#endif  //MORDRED_FLATBUTTERFLYTOPO_H
