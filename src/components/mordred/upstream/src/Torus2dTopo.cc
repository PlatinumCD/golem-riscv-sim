//
// Torus2dTopo.cc
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
#include "Torus2dTopo.h"

using namespace SST::Mordred;

TorusTopo::TorusTopo(
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
  output               = new Output( "TorusTopo [" + getName() + ":@p:@t]:", verbosity, 0, Output::STDOUT );

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

  halfXDim = xDim / 2;
  halfYDim = yDim / 2;

  output->verbose(
    CALL_INFO,
    MORDRED_VERBOSE_MIN,
    0,
    "TorusTopo constructed; rtr_id=%" PRIu32 ", num_ports=%" PRIu32 ", local_ports=%" PRIu32 "\n",
    rtrId,
    numPorts,
    numLocalPorts
  );
}

void TorusTopo::init( uint32_t phase ) {
  output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0, "TorusTopo::init(%" PRIu32 ")\n", phase );
  output->flush();
  if( phase != 3 )
    return;
  // Verify that every rtr-rtr port is connected
  for( uint32_t i = 0; i < ( numPorts - numLocalPorts ); ++i ) {
    if( perPortConnectedRtr->at( i ) == UINT32_MAX )
      output->fatal( CALL_INFO, -1, "TorusTopo: port=%" PRIu32 " unconnected\n", i );
  }
}

void TorusTopo::setup() {
  for (uint32_t i = 0; i < numPorts; ++i) {
    output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0,
                     "perPortConnectedRtr[%" PRIu32 "]=%" PRIu32 "\n",
                     i, perPortConnectedRtr->at(i) );
  }
}

int32_t TorusTopo::getEndpointId( uint32_t portnum ) {
  if( portnum < TORUSNET_PORTS_PER_ROUTER )
    return -1;
  uint32_t base_id  = rtrId * numLocalPorts;
  uint32_t local_id = portnum - TORUSNET_PORTS_PER_ROUTER;
  return static_cast<int32_t>( base_id + local_id );
}

uint32_t TorusTopo::routePacket( uint32_t dest ) {
  uint32_t dest_rtr_id = dest / numLocalPorts;

  // Not stopping here
  if( dest_rtr_id != rtrId ) {
    uint32_t dest_x = dest_rtr_id % xDim;
    uint32_t dest_y = dest_rtr_id / xDim;

    // Currently just going along x until we hit the proper y
    // then we'll route along the y.
    // Routing logic from merlin
    PortDirE outport;
    if( dest_x != xId ) {
      // do X routing
      int32_t dx_neg = (int32_t) xId - (int32_t) dest_x;
      if( dx_neg < 0 )
        dx_neg += (int32_t) xDim;
      int32_t dx_pos = (int32_t) dest_x - (int32_t) xId;
      if( dx_pos < 0 )
        dx_pos += (int32_t) xDim;

      if( dx_pos <= dx_neg )
        outport = PortDirE::EAST;
      else
        outport = PortDirE::WEST;
    } else {
      // do Y routing
      int32_t dy_neg = (int32_t) yId - (int32_t) dest_y;
      if( dy_neg < 0 )
        dy_neg += (int32_t) yDim;
      int32_t dy_pos = (int32_t) dest_y - (int32_t) yId;
      if( dy_pos < 0 )
        dy_pos += (int32_t) yDim;

      if( dy_pos <= dy_neg )
        outport = PortDirE::NORTH;
      else
        outport = PortDirE::SOUTH;
    }
    output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0,
                     "Routing: dest_rtr_id=%" PRIu32 ", outport=%" PRIu32 "\n",
                     dest_rtr_id, (uint32_t)outport);
    output->flush();
    return outport;
  }

  // Local packet, just find the dest port
  uint32_t dest_port = dest - endptZeroId;  // this should be the number of the local port
  dest_port += TORUSNET_PORTS_PER_ROUTER;   // add to account for router ports
  if( dest_port >= numPorts )
    output->fatal(
      CALL_INFO, -1, "Error! Invalid destination for packet; numPorts=%" PRIu32 ", dest_port=%" PRIu32 "\n", numPorts, dest_port
    );
  output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0,
                   "Local packet; dest_port=%" PRIu32 "\n",
                   dest_port);
  output->flush();
  return dest_port;
}

void TorusTopo::routeUntimedBroadcastPacket(
  uint32_t receive_port_id, MordredInitEvent* init_ev, std::vector<Event*>& output_events
) {
  // Send to all connected endpoints except sender
  for( uint32_t i = TORUSNET_PORTS_PER_ROUTER; i < numPorts; ++i ) {
    if( i == receive_port_id )
      continue;  // always false if from another router
    if( perPortConnectedRtr->at( i ) == UINT32_MAX )
      continue;  // active endpts are set to UINT32_MAX - 1
    output_events.at( i ) = init_ev->clone();
  }

  // Broadcast received from an endpoint
  if( receive_port_id >= TORUSNET_PORTS_PER_ROUTER ) {
    for( uint32_t i = 0; i < TORUSNET_PORTS_PER_ROUTER; ++i ) {
      sendBroadcast( i, init_ev, output_events );
    }
    return;
  }

  // Received from another router
  if( receive_port_id == NORTH ) {
    sendBroadcast( SOUTH, init_ev, output_events );
  } else if( receive_port_id == SOUTH ) {
    sendBroadcast( NORTH, init_ev, output_events );
  } else {
    if( receive_port_id == WEST ) {
      sendBroadcast( EAST, init_ev, output_events );
    } else if( receive_port_id == EAST ) {
      sendBroadcast( WEST, init_ev, output_events );
    }
    sendBroadcast( NORTH, init_ev, output_events );
    sendBroadcast( SOUTH, init_ev, output_events );
  }
}

void TorusTopo::sendBroadcast( uint32_t dir, MordredInitEvent* init_ev, std::vector<Event*>& output_events ) {
  switch( dir ) {
  case NORTH:
    if( yId != ( yDim - 1 ) )
      output_events.at( NORTH ) = init_ev->clone();
    break;
  case SOUTH:
    if( yId != 0 )
      output_events.at( SOUTH ) = init_ev->clone();
    break;
  case EAST:
    if( xId != ( xDim - 1 ) )
      output_events.at( EAST ) = init_ev->clone();
    break;
  case WEST:
    if( xId != 0 )
      output_events.at( WEST ) = init_ev->clone();
    break;
  default: output->fatal( CALL_INFO, -1, "Unexpected direction=%" PRIu32 "\n", dir );
  }
}
