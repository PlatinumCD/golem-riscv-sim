//
// GatewayTopology.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_GATEWAYTOPOLOGY_H
#define MORDRED_GATEWAYTOPOLOGY_H

// Standard headers
#include <cstdint>

// Local SST header
#include "sst_config.h"

// Local headers
#include "MordredEvents.h"
#include "TopologyAPI.h"

namespace SST::Mordred {

/**
 * GatewayTopology — decorates any TopologyAPI implementation (Mesh, Torus2d,
 * Torus3d, FlatButterfly, ...) with cross-domain gateway redirection.
 *
 * Two or more independently-addressed topology domains (e.g. meshes, tori,
 * or a mix) can be joined -- not just pairwise, but in any graph among
 * domains (a triangle of three domains each linked to the other two, a
 * star, a chain, ...) -- if every router in each domain loads
 * GatewayTopology as its "topology" subcomponent, wrapping that domain's
 * real topology in the "inner_topology" slot. Each domain owns a contiguous
 * slice [id_base, id_base+local_range_size) of one shared global id space.
 * A destination outside that slice is "foreign"; the four parallel
 * "remote_*" lists describe, for each OTHER domain directly reachable from
 * here, its own slice [remote_id_bases[i], +remote_range_sizes[i]) and
 * which LOCAL router (remote_gateway_rtr_ids[i]) + port
 * (remote_gateway_ports[i]) is the gateway to it. A foreign destination is
 * matched against these ranges and redirected toward that entry's gateway
 * router; that router alone forwards it out its own extra, dedicated port
 * unchanged (no address translation — the dest field is already globally
 * meaningful).
 *
 * GatewayTopology must be loaded on EVERY router in the domain, not just the
 * gateway routers — MordredRouter calls topology->routePacket() at every
 * hop, including the router where a foreign-destined packet is first
 * injected, so every router needs the full remote-route list to know how to
 * steer such traffic toward whichever gateway serves it (which may be a
 * different router than the one serving some OTHER remote domain). Only a
 * router named as some entry's gateway_rtr_id gets an extra, additive port
 * beyond its inner topology's normal direction set for that entry (never a
 * repurposed cardinal/wraparound port — several inner topologies, e.g.
 * Torus2dTopo and Torus3dTopo, fatal in init() if any of their own
 * router-router ports is left unconnected); a router named as the gateway
 * for more than one remote entry gets one extra port per DISTINCT physical
 * port those entries name, as the trailing contiguous block of port
 * indices -- multiple entries sharing the same gateway_rtr_id AND
 * gateway_port (e.g. two remote domains reachable via the same physical
 * link, see below) are deduplicated to that one port, not double-counted.
 *
 * This also gives multi-hop transit for free, with no address translation:
 * a domain doesn't need a direct link to every domain it needs to reach --
 * it just needs a remote_* entry (or several, aggregated or not) whose
 * range covers that domain's ids and whose gateway_rtr_id/gateway_port
 * point at whichever neighbor is actually directly linked. E.g. in a chain
 * A-B-C, A can have entries for both B's range and C's range that both name
 * A's B-facing gateway; once such a packet crosses into B, B's own
 * (accurate, direct) entry for C forwards it onward -- B needs no awareness
 * that the packet actually originated at A rather than one of its own local
 * endpoints, since routing is purely dest-based at every hop.
 *
 * Broadcast relay is multi-hop, mirroring unicast routing: a broadcast is
 * relayed out every one of a router's gateway ports except the one it just
 * arrived from, so it propagates through the whole domain graph -- a
 * chain's far leaf gets it via the middle domain, same as unicast transit.
 * The one thing that needs care is a domain graph with a CYCLE (a triangle
 * is exactly one): domain C there can be entered externally via two
 * different physical routers (its two separate gateway routers, one per
 * edge), each its own GatewayTopology instance with no other shared state,
 * so per-router bookkeeping alone can't recognize the second arrival as a
 * duplicate of a broadcast this domain already fully flooded. This is
 * handled by a domain-wide (not per-router) dedup keyed by id_base, gated
 * ONLY on genuine external entries (arrival via one of a router's own
 * gateway ports) -- ordinary hops within an already-started intra-domain
 * flood (arriving via a cardinal/local port) are never gated, since the
 * inner topology's own flood algorithm already guarantees each of ITS
 * routers sees a given flood at most once. See routeUntimedBroadcastPacket()
 * and the sSeenBroadcastOrigins comment in the .cc for the full mechanism,
 * including known limitations (not rank-safe, not checkpointed).
 */
class GatewayTopology : public TopologyAPI {

public:
  // register with the SST Core
  SST_ELI_REGISTER_SUBCOMPONENT(
    GatewayTopology,
    "mordred",           // component library
    "GatewayTopology",   // component name
    SST_ELI_ELEMENT_VERSION( 0, 0, 1 ),
    "Cross-domain gateway decorator for any TopologyAPI implementation",
    SST::Mordred::TopologyAPI
  )

  // register the parameters
  SST_ELI_DOCUMENT_PARAMS(
    { "verbose", "Sets the output verbosity", "5" },
    { "id_base", "Offset of this domain's slice in the shared global id space", "0" },
    { "local_range_size", "Total endpoints this domain's inner topology owns "
                           "(e.g. xDim*yDim*num_local_ports)", nullptr },
    { "remote_id_bases", "Comma-separated id_base of each directly-reachable remote domain", "" },
    { "remote_range_sizes", "Comma-separated local_range_size of each directly-reachable remote domain "
                             "(parallel to remote_id_bases)", "" },
    { "remote_gateway_rtr_ids", "Comma-separated LOCAL rtr_id of the gateway router to each remote domain "
                                 "(parallel to remote_id_bases)", "" },
    { "remote_gateway_ports", "Comma-separated port index the gateway router uses for each remote domain's "
                               "link (parallel to remote_id_bases; only meaningful on the router named by "
                               "the matching remote_gateway_rtr_ids entry)", "" }
  )

  // register the ports
  SST_ELI_DOCUMENT_PORTS()

  // register the subcomponent slots
  SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
    { "inner_topology", "The real topology being wrapped", "SST::Mordred::TopologyAPI" }
  )

  /// GatewayTopology: constructor
  GatewayTopology(
    ComponentId_t          id,
    Params&                params,
    uint32_t               rtr_id,
    uint32_t               num_ports,
    uint32_t               num_local_ports,
    std::vector<uint32_t>* connected_ports
  );

  /// GatewayTopology: destructor
  ~GatewayTopology() override { delete output; }

  int32_t getEndpointId( uint32_t portnum ) override;

  /// Get the output port for a packet
  uint32_t routePacket( uint32_t dest ) final;

  /// Do routing for untimed broadcast packets. Delegates to the inner
  /// topology for intra-domain flooding; on a gateway router, additionally
  /// relays every broadcast out each of its gateway ports -- except the one
  /// it was just received FROM, so it never bounces back (see the class
  /// doc comment for why this assumes a fully-connected domain graph).
  void routeUntimedBroadcastPacket( uint32_t receive_port_id, MordredInitEvent* ev, std::vector<Event*>& output_events ) final;

  /// Pass through to the inner topology; a gateway port is never a wraparound link.
  bool isWrapAroundOutput( uint32_t output_port ) const override;

  /// default constructor
  GatewayTopology() : SST::Mordred::TopologyAPI() {}

  /// One directly-reachable remote domain and how to reach it from here.
  struct RemoteRoute {
    uint32_t idBase;
    uint32_t rangeSize;
    uint32_t gatewayRtrId;  // LOCAL rtr_id (within THIS domain) of the gateway to this remote domain
    uint32_t gatewayPort;   // meaningful only on the router named by gatewayRtrId

    bool contains( uint32_t dest ) const { return ( dest >= idBase ) && ( ( dest - idBase ) < rangeSize ); }

    void serialize_order( SST::Core::Serialization::serializer& ser ) {
      SST_SER( idBase );
      SST_SER( rangeSize );
      SST_SER( gatewayRtrId );
      SST_SER( gatewayPort );
    }
  };

  /// serialization
  void serialize_order( SST::Core::Serialization::serializer& ser ) override {
    SST_SER( output );
    SST_SER( inner );
    SST_SER( rtrId );
    SST_SER( numLocalPorts );
    SST_SER( idBase );
    SST_SER( localRangeSize );
    SST_SER( remoteRoutes );
    SST_SER( myGatewayPorts );
    SST_SER( perPortConnectedRtr );
  }

  /// serialization implementations
  ImplementSerializable( SST::Mordred::GatewayTopology );

private:
  Output* output;

  TopologyAPI* inner{ nullptr };

  uint32_t rtrId;
  uint32_t numLocalPorts;

  uint32_t idBase;
  uint32_t localRangeSize;

  std::vector<RemoteRoute> remoteRoutes;

  // Distinct physical ports this router itself hosts (deduplicated -- see
  // the class doc comment on multiple remote entries sharing one physical
  // link), computed once in the constructor and reused by getEndpointId()
  // and routeUntimedBroadcastPacket() instead of re-deriving from
  // remoteRoutes (and re-hitting the same dedup question) each time.
  std::vector<uint32_t> myGatewayPorts;

  // Needed to check whether a gateway port is actually wired before relaying
  // a broadcast out it (see routeUntimedBroadcastPacket()).
  std::vector<uint32_t>* perPortConnectedRtr;
};

}  // namespace SST::Mordred

#endif  //MORDRED_GATEWAYTOPOLOGY_H
