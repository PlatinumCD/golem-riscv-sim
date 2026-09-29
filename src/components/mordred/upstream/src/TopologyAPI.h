//
// TopologyAPI.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_TOPOLOGYAPI_H
#define MORDRED_TOPOLOGYAPI_H

// Standard headers
#include <cstdint>
#include <vector>

// Local SST header
#include "sst_config.h"

/*
 * In Merlin, the Topology API is in router.h; the API generally maintains a set of routing
 * functions and an enum tracking what each port of the router is connected to;
 * a subset of the routing functions is processing and handling untimed functions
 */

namespace SST::Mordred {

class MordredInitEvent;

class TopologyAPI : public SubComponent {
public:
  SST_ELI_REGISTER_SUBCOMPONENT_API( SST::Mordred::TopologyAPI, ComponentId_t, uint32_t, uint32_t, std::vector<uint32_t>* )

  /// TopologyAPI: constructor
  TopologyAPI( ComponentId_t id ) : SubComponent( id ) {}

  /// TopologyAPI: constructor
  TopologyAPI() : SubComponent() {}

  /// TopologyAPI: default destructor
  ~TopologyAPI() override                           = default;

  /// Computed endpoint ID
  virtual int32_t getEndpointId( uint32_t portnum ) = 0;

  /// Get the output port for a packet
  virtual uint32_t routePacket( uint32_t dest )     = 0;

  /// Do routing for untimed broadcast packets
  virtual void
    routeUntimedBroadcastPacket( uint32_t receive_port_id, MordredInitEvent* ev, std::vector<Event*>& output_events ) = 0;

  /// Return true if the given output port uses a wrap-around link (i.e. crosses
  /// the ring boundary in this router's torus dimension).  Default returns false
  /// so non-torus topologies (mesh, butterfly, …) are unaffected.
  virtual bool isWrapAroundOutput( uint32_t output_port ) const { return false; }

};  // class TopologyAPI

}  // namespace SST::Mordred

#endif  //MORDRED_TOPOLOGYAPI_H
