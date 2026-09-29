//
// FlatButterflyTopo.cc
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
#include "FlatButterflyTopo.h"

using namespace SST::Mordred;

FlatButterflyTopo::FlatButterflyTopo(
  ComponentId_t          id,
  Params&                params,
  uint32_t               rtr_id,
  uint32_t               num_ports,
  uint32_t               num_local_ports,
  std::vector<uint32_t>* connected_ports
)
  : TopologyAPI( id ), rtrId( rtr_id ), numPorts( num_ports ), numLocalPorts( num_local_ports ),
    numRtrPorts( num_ports - num_local_ports ), perPortConnectedRtr( connected_ports ) {
  const auto verbosity = params.find<uint32_t>( "verbose", MORDRED_VERBOSE_MED );
  output               = new Output( "FlatButterflyTopo [" + getName() + ":@p:@t]:", verbosity, 0, Output::STDOUT );

  k                    = params.find<uint32_t>( "k", UINT32_MAX );
  if( k == UINT32_MAX ) {
    output->fatal( CALL_INFO, -1, "FlatButterflyTopo requires k to be specified\n" );
  }

  n = params.find<uint32_t>( "n", UINT32_MAX );
  if( n == UINT32_MAX ) {
    output->fatal( CALL_INFO, -1, "FlatButterflyTopo requires n to be specified\n" );
  }

  uint32_t base_endpt = rtrId * k;
  myAddress           = convertBase( base_endpt );

  connectedRtrsByBase.resize( numRtrPorts );

  output->verbose(
    CALL_INFO,
    MORDRED_VERBOSE_MIN,
    0,
    "FlatButterflyTopo constructed; rtr_id=%" PRIu32 ", base_endpt=%" PRIu32 ", num_ports=%" PRIu32 ", local_ports=%" PRIu32 "\n",
    rtrId,
    base_endpt,
    numPorts,
    numLocalPorts
  );

  for ( uint32_t i = 0; i < n; i++ ) {
    output->verbose( CALL_INFO, MORDRED_VERBOSE_ALL, 0, "myAddress[%" PRIu32 "] = %" PRIu32 "\n", i, myAddress.at( i ) );
  }
}

void FlatButterflyTopo::init( uint32_t phase ) {
  if( phase != 3 )
    return;
  // Compute the n-digit radix-k addresses for our neighboring routers
  // Need these for the routing function
  for( uint32_t i = 0; i < numRtrPorts; ++i ) {
    if( perPortConnectedRtr->at( i ) == UINT32_MAX )
      continue;
    uint32_t base_endpt         = perPortConnectedRtr->at( i ) * k;
    connectedRtrsByBase.at( i ) = convertBase( base_endpt );
  }
}

std::vector<uint32_t> FlatButterflyTopo::convertBase( uint32_t num ) {
  // base to convert into is always k
  // output vector length is n
  std::vector<uint32_t> result( n, 0 );
  uint32_t              idx = 0;
  while( num > 0 ) {
    if( idx >= n )
      output->fatal( CALL_INFO, -1, "fail idx=%u, n=%u\n", idx, n );
    result.at( idx ) = num % k;
    num /= k;
    idx++;
  }
  return result;
}

int32_t FlatButterflyTopo::getEndpointId( uint32_t portnum ) {
  if( portnum < numRtrPorts )
    return -1;
  uint32_t base_id  = rtrId * numLocalPorts;
  uint32_t local_id = portnum - numRtrPorts;
  return static_cast<int32_t>( base_id + local_id );
}

uint32_t FlatButterflyTopo::routePacket( uint32_t dest ) {
  uint32_t dest_port              = UINT32_MAX;

  std::vector<uint32_t> dest_addr = convertBase( dest );
  if( dest_addr.size() != myAddress.size() )
    output->fatal( CALL_INFO, -1, "Invalid dest_addr.size=%zu; myAddr.size=%zu\n", dest_addr.size(), myAddress.size() );

  // First, we want to determine if this dest_addr is local
  // If so, we return local output port
  if( isLocalAddr( dest_addr ) ) {
    dest_port = numRtrPorts + dest_addr[0];
    output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0,
                     "Found local delivery; dest=%" PRIu32 ", outport=%" PRIu32 "\n",
                     dest, dest_port);
    output->flush();
    return dest_port;
  }

  // Now, we need to find a valid output port for this packet
  // Want to prioritize the closest "distance" where distance is the number of differing digits
  std::vector distances( numRtrPorts, UINT32_MAX );
  for( uint32_t i = 0; i < numRtrPorts; i++ ) {
    if( perPortConnectedRtr->at( i ) == UINT32_MAX )
      continue;
    distances.at( i ) = calcDist( i, dest_addr );
    output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0, "Distance[%" PRIu32 "] = %" PRIu32 "\n", i, distances.at(i) );
    output->flush();
  }

  // At this point, the distances vector has how many digits differ between the destination endpt and
  // each of our neighboring routers;
  // the lower the distance, the fewer hops in the NOC to be made (distance of 0 means that destRtr is the final dest)
  // Since distances is a short vector, let's just increment to find a winner
  // This is horrific for load balance, etc, but it will at least send packets to their destination
  bool     found    = false;
  uint32_t cur_dist = 0;
  while( !found ) {
    for( uint32_t i = 0; i < numRtrPorts; i++ ) {
      if( cur_dist == distances.at( i ) ) {
        found     = true;
        dest_port = i;
      }
    }
    cur_dist++;
    if( cur_dist > n + 1 )  // I'm sure this could be in the while condition, but my brain is failing me today
      break;
  }

  if( !found )
    output->fatal( CALL_INFO, -1, "Could not find a winner\n" );

  if( dest_port >= numRtrPorts )
    output->fatal(
      CALL_INFO,
      -1,
      "Error! Invalid destination for packet; numRtrPorts=%" PRIu32 ", dest_port=%" PRIu32 "\n",
      numRtrPorts,
      dest_port
    );
  return dest_port;
}

void FlatButterflyTopo::routeUntimedBroadcastPacket(
  uint32_t receive_port_id, MordredInitEvent* init_ev, std::vector<Event*>& output_events
) {

#if 0
  /* This if-endif block works for k-ary, n-flys of n=2 (namely the 4-ary, 2-fly); fails for the k=2,n=4 test.
   For the 2-ary, 4-fly (with 8 routers), rtr0 is only connected to routers 1,2,4
   and getting to rtr 7 would take at least 2 hops.

   Since Google's AI summary suggests that this topology isn't meant for broadcasts, so we're going to leave this
   exercise for a later day.
  */

  // Send to all connected endpoints except sender
  for ( uint32_t i = numRtrPorts; i < numPorts; ++i ) {
    if (i == receive_port_id ) continue; // always false if from another router
    output_events.at(i) = init_ev->clone();
  }

  // if from an endpoint, send to everyone except myself
  if ( receive_port_id >= numRtrPorts ) {
    for ( uint32_t i = 0; i < numRtrPorts; i++ ) {
      output_events.at(i) = init_ev->clone();
    }
  }
#endif
  output->fatal( CALL_INFO, -1, "INIT_BROADCAST_ADDR destination for untimed messages is not supported for this topology\n" );

#if 0  // Debugging code
  if ( rtrId == 0 || rtrId == 2 ) {
    for ( uint32_t i = 0; i < numPorts; i++ ) {
      if ( output_events.at(i) != nullptr ) {
        output->output( CALL_INFO, "Send broadcast on router port=%u\n", i );
      }
    }
  }
#endif
}

bool FlatButterflyTopo::isLocalAddr( std::vector<uint32_t>& dest_addr ) {
  for( uint32_t i = n - 1; i > 0; i-- ) {
    if( dest_addr.at( i ) != myAddress.at( i ) )
      return false;
  }
  return true;
}

uint32_t FlatButterflyTopo::calcDist( uint32_t idx, std::vector<uint32_t>& dest_addr ) {
  uint32_t dist = 0;
  if( idx >= connectedRtrsByBase.size() )
    output->fatal( CALL_INFO, -1, "Error! Invalid idx=%u, size=%zu\n", idx, connectedRtrsByBase.size() );
  for( uint32_t i = n - 1; i > 0; i-- ) {
    if( dest_addr.at( i ) != connectedRtrsByBase.at( idx ).at( i ) )
      dist++;
  }
  return dist;
}
