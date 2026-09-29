//
// GatewayTopology.cc
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

// Standard headers
#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Local SST header
#include "sst_config.h"

// Local headers
#include "GatewayTopology.h"

using namespace SST::Mordred;

namespace {

// Parse a comma-separated list of non-negative integers, e.g. "4,8,12".
// An empty string parses to an empty vector (no remote domains configured).
std::vector<uint32_t> parseUintList( const std::string& s ) {
  std::vector<uint32_t> result;
  std::istringstream    ss( s );
  std::string           tok;
  while( std::getline( ss, tok, ',' ) ) {
    auto first = tok.find_first_not_of( " \t" );
    if( first == std::string::npos )
      continue;
    tok = tok.substr( first, tok.find_last_not_of( " \t" ) - first + 1 );
    result.push_back( static_cast<uint32_t>( std::stoul( tok ) ) );
  }
  return result;
}

// Domain-wide (NOT per-router) broadcast-origin dedup, keyed by id_base --
// every GatewayTopology instance sharing the same id_base belongs to the
// same domain and shares the same entry. This is what makes multi-hop
// relay safe when a domain has more than one gateway router (e.g. each of
// a triangle's three domains has two, one per edge): a domain can be
// entered externally via two different physical routers, each running its
// own separate GatewayTopology C++ object with no other shared state, so
// per-router tracking alone can't recognize the second arrival as a
// duplicate of one already fully flooded through this domain by the first.
//
// Deliberately a plain static map rather than an SST SharedObject: this
// codebase has no existing pattern for state shared across ComponentS
// (RtrOwnedSharedObjs in MordredEvents.h is scoped to ports within ONE
// router). Known limitations from that choice: (1) not rank-safe -- a
// domain split across MPI ranks would need real cross-rank shared state
// instead; (2) not checkpointed, matching mordred_router's own
// "Checkpointable: false" today; (3) not thread-safe, matching the rest of
// this single-threaded-per-rank test suite.
std::unordered_map<uint32_t, std::unordered_set<int64_t>> sSeenBroadcastOrigins;

}  // namespace

GatewayTopology::GatewayTopology(
  ComponentId_t          id,
  Params&                params,
  uint32_t               rtr_id,
  uint32_t               num_ports,
  uint32_t               num_local_ports,
  std::vector<uint32_t>* connected_ports
)
  : TopologyAPI( id ), rtrId( rtr_id ), numLocalPorts( num_local_ports ), perPortConnectedRtr( connected_ports ) {
  const auto verbosity = params.find<uint32_t>( "verbose", MORDRED_VERBOSE_MED );
  output               = new Output( "GatewayTopology [" + getName() + ":@p:@t]:", verbosity, 0, Output::STDOUT );

  idBase = params.find<uint32_t>( "id_base", 0 );

  localRangeSize = params.find<uint32_t>( "local_range_size", UINT32_MAX );
  if( localRangeSize == UINT32_MAX )
    output->fatal( CALL_INFO, -1, "GatewayTopology requires local_range_size to be specified\n" );

  auto remote_id_bases        = parseUintList( params.find<std::string>( "remote_id_bases", "" ) );
  auto remote_range_sizes     = parseUintList( params.find<std::string>( "remote_range_sizes", "" ) );
  auto remote_gateway_rtr_ids = parseUintList( params.find<std::string>( "remote_gateway_rtr_ids", "" ) );
  auto remote_gateway_ports   = parseUintList( params.find<std::string>( "remote_gateway_ports", "" ) );

  if( ( remote_id_bases.size() != remote_range_sizes.size() ) || ( remote_id_bases.size() != remote_gateway_rtr_ids.size() )
      || ( remote_id_bases.size() != remote_gateway_ports.size() ) )
    output->fatal(
      CALL_INFO, -1,
      "GatewayTopology: remote_id_bases (%zu), remote_range_sizes (%zu), remote_gateway_rtr_ids (%zu), and "
      "remote_gateway_ports (%zu) must all be the same length\n",
      remote_id_bases.size(), remote_range_sizes.size(), remote_gateway_rtr_ids.size(), remote_gateway_ports.size()
    );

  remoteRoutes.reserve( remote_id_bases.size() );
  for( size_t i = 0; i < remote_id_bases.size(); i++ )
    remoteRoutes.push_back( RemoteRoute{ remote_id_bases[i], remote_range_sizes[i], remote_gateway_rtr_ids[i], remote_gateway_ports[i] } );

  // Not fatal: an intentionally isolated domain (no cross-domain links at
  // all) is a legitimate configuration, and none of the four remote_*
  // params have a meaningful required value to enforce the way
  // local_range_size does. But an EMPTY remote_id_bases is also exactly
  // what a forgotten/mis-wired remotes list looks like, and unlike a
  // missing required param, nothing catches that at construction time --
  // routePacket() only finds out the hard way, via its generic "no
  // matching remote entry" fatal, whenever something eventually tries to
  // reach a domain this router doesn't know about (which may be
  // immediately, much later, or never, depending on traffic patterns).
  // output() (not verbose()) so this is visible regardless of verbosity.
  if( remoteRoutes.empty() )
    output->output(
      CALL_INFO,
      "WARNING: GatewayTopology on rtr_id=%" PRIu32 " (id_base=%" PRIu32 ") has no remote_* entries configured -- "
      "every destination outside [%" PRIu32 ", %" PRIu32 ") will fatal in routePacket() the first time something "
      "tries to reach it. Fine for an intentionally isolated domain; otherwise check that "
      "remote_id_bases/remote_range_sizes/remote_gateway_rtr_ids/remote_gateway_ports were actually populated.\n",
      rtrId, idBase, idBase, idBase + localRangeSize
    );

  // Distinct physical ports this router itself hosts. Several remote routes
  // can name the same gateway_rtr_id/gateway_port -- e.g. in a chain A-B-C,
  // A has separate entries for B's range and C's range, but both point at
  // the one physical link to B -- so dedupe by port value, not raw entry
  // count, or a shared port would get counted twice and corrupt
  // inner_num_ports below. The inner topology only ever needs to know about
  // its own direction set plus local ports -- it stays completely unaware
  // it is being wrapped, and never sees (or routes to) these extra ports.
  for( const auto& r : remoteRoutes )
    if( r.gatewayRtrId == rtrId )
      myGatewayPorts.push_back( r.gatewayPort );
  std::sort( myGatewayPorts.begin(), myGatewayPorts.end() );
  myGatewayPorts.erase( std::unique( myGatewayPorts.begin(), myGatewayPorts.end() ), myGatewayPorts.end() );

  const uint32_t inner_num_ports = num_ports - static_cast<uint32_t>( myGatewayPorts.size() );

  // routeUntimedBroadcastPacket() relies on a gateway port being outside the
  // range the inner topology knows about (see its use of a gateway port as
  // an out-of-range sentinel) -- enforce that myGatewayPorts is exactly the
  // trailing contiguous block [inner_num_ports, num_ports), rather than let
  // a misconfigured port silently corrupt the inner topology's own
  // local-port bookkeeping.
  for( size_t i = 0; i < myGatewayPorts.size(); i++ ) {
    if( myGatewayPorts[i] != inner_num_ports + i )
      output->fatal(
        CALL_INFO, -1,
        "GatewayTopology: gateway ports on rtr_id=%" PRIu32 " must be the trailing contiguous block of port "
        "indices [%" PRIu32 ", %" PRIu32 ") -- they must be extra, additive ports, never repurposed "
        "cardinal/local ones (got port=%" PRIu32 " at position %zu)\n",
        rtrId, inner_num_ports, num_ports, myGatewayPorts[i], i
      );
  }

  inner = loadUserSubComponent<TopologyAPI>(
    "inner_topology", ComponentInfo::SHARE_NONE, rtr_id, inner_num_ports, num_local_ports, connected_ports
  );
  if( !inner )
    output->fatal( CALL_INFO, -1, "GatewayTopology: no inner_topology subcomponent in slot 'inner_topology'\n" );

  output->verbose(
    CALL_INFO, MORDRED_VERBOSE_MIN, 0,
    "GatewayTopology constructed; rtr_id=%" PRIu32 ", id_base=%" PRIu32 ", local_range_size=%" PRIu32 ", "
    "remote_routes=%zu, my_gateway_ports=%zu\n",
    rtrId, idBase, localRangeSize, remoteRoutes.size(), myGatewayPorts.size()
  );
}

int32_t GatewayTopology::getEndpointId( uint32_t portnum ) {
  for( const auto p : myGatewayPorts )
    if( p == portnum )
      return -1;  // not a local endpoint, same convention as any router-facing port

  // This is the value RtrPortControlBase hands to a connected NIC as its
  // network address during init (see RtrPortControlBase.cc's use of
  // topo->getEndpointId()), which the NIC then uses for both its own "src"
  // on send and to validate incoming "dest" on receive. It has to be
  // expressed in the same global id space as everything else routePacket()
  // deals with, so -- like local_dest in routePacket() -- the inner
  // topology's local id needs id_base added back before returning it.
  const int32_t local_id = inner->getEndpointId( portnum );
  if( local_id < 0 )
    return local_id;  // not a local endpoint (cardinal port, etc.) -- pass through
  return static_cast<int32_t>( idBase ) + local_id;
}

uint32_t GatewayTopology::routePacket( uint32_t dest ) {
  const bool local = ( dest >= idBase ) && ( ( dest - idBase ) < localRangeSize );

  if( local ) {
    // In range: the inner topology only ever knows LOCAL addressing --
    // strip this domain's id_base before delegating. (This is the offset
    // that made an earlier version of this code easy to get wrong: id_base=0
    // makes "local == global", which is exactly the case a first domain
    // often happens to use.)
    return inner->routePacket( dest - idBase );
  }

  for( const auto& r : remoteRoutes ) {
    if( !r.contains( dest ) )
      continue;

    if( r.gatewayRtrId == rtrId ) {
      output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0,
                       "Routing: dest=%" PRIu32 " belongs to remote domain [%" PRIu32 ",%" PRIu32
                       "); I am its gateway -> port=%" PRIu32 "\n",
                       dest, r.idBase, r.idBase + r.rangeSize, r.gatewayPort );
      return r.gatewayPort;  // hand off to the cross-domain link; dest is untouched
    }

    // Not that domain's gateway: pretend the destination IS the gateway
    // router's own first local endpoint, so the inner topology's own
    // routing algorithm -- whatever it is -- carries us there like any
    // other in-domain packet. This is a LOCAL address for the inner
    // topology's consumption -- no id_base offset here, same as local_dest
    // above.
    const uint32_t proxy_dest = r.gatewayRtrId * numLocalPorts;
    output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0,
                     "Routing: dest=%" PRIu32 " belongs to remote domain [%" PRIu32 ",%" PRIu32
                     "); redirecting toward gateway_rtr_id=%" PRIu32 " (proxy_dest=%" PRIu32 ")\n",
                     dest, r.idBase, r.idBase + r.rangeSize, r.gatewayRtrId, proxy_dest );
    return inner->routePacket( proxy_dest );
  }

  output->fatal( CALL_INFO, -1,
                 "GatewayTopology: dest=%" PRIu32 " is not local to this domain and does not match any "
                 "configured remote_id_bases/remote_range_sizes entry\n",
                 dest );
  return 0;  // unreachable
}

void GatewayTopology::routeUntimedBroadcastPacket(
  uint32_t receive_port_id, MordredInitEvent* init_ev, std::vector<Event*>& output_events
) {
  bool arrived_via_my_gateway = false;
  for( const auto p : myGatewayPorts ) {
    if( p == receive_port_id ) {
      arrived_via_my_gateway = true;
      break;
    }
  }

  if( arrived_via_my_gateway ) {
    // A genuine external entry into my domain from a neighboring domain --
    // NOT a hop within an already-started intra-domain flood (those arrive
    // via ordinary cardinal/local ports, never a gateway port). Domain-wide
    // dedup only needs to gate entry points: a domain graph with a cycle
    // (the triangle) can enter this same domain a second time, via a
    // different gateway router, after it's already been fully flooded --
    // recognize and drop that duplicate here rather than double-flood
    // locally or bounce it back out. See sSeenBroadcastOrigins's comment.
    const int64_t origin = init_ev->req->src;
    if( !sSeenBroadcastOrigins[idBase].insert( origin ).second )
      return;
  }

  // Flood this domain. If this arrived via one of my own gateway ports,
  // receive_port_id already equals that port's value, which -- by the
  // trailing-contiguous-block property enforced in the constructor -- is
  // guaranteed to be outside the inner topology's own port range. That
  // means the inner topology's "send to all local endpoints except sender"
  // exclusion never matches a real port (every real local endpoint still
  // gets the broadcast), while its "receive_port_id >= <cardinal-port-
  // count>" check still correctly treats this as an endpoint-style
  // injection -- so no special-casing is needed here versus a genuinely
  // local/cardinal arrival.
  inner->routeUntimedBroadcastPacket( receive_port_id, init_ev, output_events );

  // Relay out every one of my OWN gateway ports except the one this just
  // arrived from (if any), so the broadcast keeps propagating through the
  // domain graph -- e.g. reaching a chain's far leaf via the middle
  // domain, or every domain in a triangle. This fires at whichever
  // router(s) in this domain host a gateway port, wherever the intra-
  // domain flood happens to reach them (including the very entry router,
  // for its OTHER gateway ports if it has more than one) -- the dedup
  // check above is what makes this safe even when domains form a cycle.
  for( const auto p : myGatewayPorts ) {
    if( p == receive_port_id )
      continue;  // don't bounce it back out the way it just came in
    if( perPortConnectedRtr->at( p ) != UINT32_MAX )
      output_events.at( p ) = init_ev->clone();
  }
}

bool GatewayTopology::isWrapAroundOutput( uint32_t output_port ) const {
  return inner->isWrapAroundOutput( output_port );
}
