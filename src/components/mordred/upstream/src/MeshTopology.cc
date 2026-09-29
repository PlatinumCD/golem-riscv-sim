//
// MeshTopology.cc
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

// Standard headers
#include <cinttypes>
#include <cstdint>

// Local SST header
#include "sst_config.h"

// Local headers
#include "MeshTopology.h"

using namespace SST::Mordred;

MeshTopology::MeshTopology(
  ComponentId_t          id,
  Params&                params,
  uint32_t               rtr_id,
  uint32_t               num_ports,
  uint32_t               num_local_ports,
  std::vector<uint32_t>* connected_ports
)
  : TopologyAPI( id ), rtrId( rtr_id ), endptZeroId( rtr_id * num_local_ports ), numPorts( num_ports ),
    numLocalPorts( num_local_ports ), perPortConnectedRtr( connected_ports ) {
  const auto verbosity = params.find<uint32_t>( "verbose", MORDRED_VERBOSE_MED );
  output               = new Output( "MeshTopology [" + getName() + ":@p:@t]:", verbosity, 0, Output::STDOUT );

  // Process and validate input parameters
  xDim                 = params.find<uint32_t>( "xDim", 1 );
  yDim                 = params.find<uint32_t>( "yDim", 1 );

  xId                  = rtrId % xDim;
  yId                  = rtrId / xDim;

  if( ( xId >= xDim ) || ( yId >= yDim ) ) {
    output->fatal(
      CALL_INFO, -1, "xId=%" PRId32 " >= xSize=%" PRId32 " OR yId=%" PRId32 " >= ySize=%" PRId32 "\n", xId, xDim, yId, yDim
    );
  }

  output->verbose(
    CALL_INFO,
    MORDRED_VERBOSE_MIN,
    0,
    "MeshTopology constructed; rtr_id=%" PRIu32 ", num_ports=%" PRIu32 ", local_ports=%" PRIu32 "\n",
    rtrId,
    numPorts,
    numLocalPorts
  );
}

void MeshTopology::setup() {
  for( uint32_t i = 0; i < numPorts; ++i ) {
    output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0,
                     "perPortConnectedRtr[%" PRIu32 "]=%" PRIu32 "\n",
                     i, perPortConnectedRtr->at( i ) );
  }
}

int32_t MeshTopology::getEndpointId( uint32_t portnum ) {
  if( portnum < 4 )
    return -1;
  uint32_t base_id  = rtrId * numLocalPorts;
  uint32_t local_id = portnum - 4;
  return static_cast<int32_t>( base_id + local_id );
}

uint32_t MeshTopology::routePacket( uint32_t dest ) {
  uint32_t dest_rtr_id = dest / numLocalPorts;

  // Not stopping here
  if( dest_rtr_id != rtrId ) {
    uint32_t dest_x = dest_rtr_id % xDim;
    uint32_t dest_y = dest_rtr_id / xDim;

    output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0,
                     "Routing: dest=%" PRIu32 ", dest_rtr_id=%" PRIu32 ", dest_x=%" PRIu32 ", dest_y=%" PRIu32 "\n",
                     dest, dest_rtr_id, dest_x, dest_y );
    output->flush();

    // Currently just going along x until we hit the proper y
    // then we'll route along the y.
    if( dest_x < xId )
      return PortDirE::WEST;
    if( dest_x > xId )
      return PortDirE::EAST;
    if( dest_y < yId )
      return PortDirE::SOUTH;
    if( dest_y > yId )
      return PortDirE::NORTH;
  }

  uint32_t dest_port = dest - endptZeroId;  // this should be the number of the local port
  dest_port += MESHNET_PORTS_PER_ROUTER;    // add to account for router ports
  if( dest_port >= numPorts )
    output->fatal(
      CALL_INFO, -1, "Error! Invalid destination for packet; numPorts=%" PRIu32 ", dest_port=%" PRIu32 "\n", numPorts, dest_port
    );
  return dest_port;
}

void MeshTopology::routeUntimedBroadcastPacket(
  uint32_t receive_port_id, MordredInitEvent* init_ev, std::vector<Event*>& output_events
) {
  // Send to all connected endpoints except sender
  for( uint32_t i = MESHNET_PORTS_PER_ROUTER; i < numPorts; ++i ) {
    if( i == receive_port_id )
      continue;  // always false if from another router
    if( perPortConnectedRtr->at( i ) == UINT32_MAX )
      continue;  // active endpts are set to UINT32_MAX - 1
    output_events.at( i ) = init_ev->clone();
  }

  // Broadcast received from an endpoint
  if( receive_port_id >= MESHNET_PORTS_PER_ROUTER ) {
    // Send to all other routers
    for( uint32_t i = 0; i < MESHNET_PORTS_PER_ROUTER; ++i ) {
      if( perPortConnectedRtr->at( i ) != UINT32_MAX ) {
        output_events.at( i ) = init_ev->clone();
      }
    }
    return;
  }

  // Received from another router
  switch( receive_port_id ) {
  case NORTH:  // continue sending south
    if( perPortConnectedRtr->at( SOUTH ) != UINT32_MAX ) {
      output_events.at( SOUTH ) = init_ev->clone();
    }
    break;
  case SOUTH:  // continue sending north
    if( perPortConnectedRtr->at( NORTH ) != UINT32_MAX ) {
      output_events.at( NORTH ) = init_ev->clone();
    }
    break;
  case WEST: [[fallthrough]];
  case EAST:
    // Send N, S, and same direction
    for( uint32_t i = 0; i < MESHNET_PORTS_PER_ROUTER; ++i ) {
      // Skip unconnected or sending ports
      if( ( perPortConnectedRtr->at( i ) == UINT32_MAX ) || ( i == receive_port_id ) )
        continue;
      output_events.at( i ) = init_ev->clone();
    }
    break;
  default: output->fatal( CALL_INFO, -1, "Unexpected receive_port_id=%" PRIu32 "\n", receive_port_id );
  }
}
