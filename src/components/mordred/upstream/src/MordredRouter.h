//
// MordredRouter.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

/**
 * Components of booksim with multiple implementations:
 * - Network (topology) - mostly python to do the linkage; but router has to be aware for initialization
 * - Router - basic implementation - probably want this to be pretty generic/flexible
 *    - Routing function - impacted by topology; include pipeline model - initialization(?)
 *    - Buffer/port interface - may differ on input and output (and virtual channels)
 *    - Allocator - VC and internal xbar
 *      - Arbiter - manage who gets what and when
 *    - Credit tracker - how managed; paper shows as flexible, but would have to dig through code to compare
 */

#ifndef MORDRED_MORDREDROUTER_H
#define MORDRED_MORDREDROUTER_H

// Standard headers
#include <cstdint>
#include <vector>

// Local SST headers
#include "RtrPortControlAPI.h"
#ifdef MORDRED_ENABLE_PHYS_CHANNEL
#include "RtrPortControlPC.h"
#endif
#include "TopologyAPI.h"
#include "VcAllocAPI.h"
#include "XbarArbAPI.h"
#include "sst_config.h"

// TODO: Configure verbosity control (use constants in MordredEvents)

namespace SST::Mordred {

class MordredRouter : public SST::Component {

public:
  SST_ELI_REGISTER_COMPONENT(
    MordredRouter,
    "mordred",
    "mordred_router",
    SST_ELI_ELEMENT_VERSION( 0, 1, 0 ),
    "Simple NoC router component",
    COMPONENT_CATEGORY_NETWORK
  )

  /*
   * For now, the {input,output}_buf_size parameters are handed down to the ports and the ports
   * will do some math and distribute credits equally across each of the VN,VC buffers.
   */

  SST_ELI_DOCUMENT_PARAMS(
    { "verbose", "Sets the output verbosity", "5" },
    { "id", "ID of the router", nullptr },
    { "clock", "Clock frequency of the router", "1GHz" },
    { "num_ports", "Number of ports on the router", "3" },
    { "num_local_ports", "Number of local ports", nullptr },
    { "num_vns", "Number of virtual networks", "1" },
    { "num_vcs", "Number of virtual channels.", "1" },
    { "flit_size", "Flit size specified in either b or B (can include SI prefix).", "32b"
    },  //passed down to RtrPortControlAPI instances
    { "input_buf_size", "Size of per-VC input buffers specified in b or B (can include SI prefix).", nullptr },
    { "output_buf_size", "Size of per-VC output buffer specified in b or B (can include SI prefix).", nullptr },
    // {"link_bw",            "Bandwidth of the links specified in either b/s or B/s (can include SI prefix)."},
    // {"port_priority_equal","Set to true to have all port have equal priority (usually endpoint ports have higher priority).","false"},
    // {"use_dense_map",      "Set to true to have a dense network id map instead of the sparse map normally used.","false"},
    // {"network_inspectors", "Comma separated list of network inspectors to put on output ports.", ""},
  )

  SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
    { "topology", "Topology and routing subcomponent", "SST::Mordred::TopologyAPI" },
    { "portcontrol",
      "PortControl blocks; user-configurable (e.g. rtrPortControl, rtrPortControlPC) or loaded anonymously as rtrPortControl when "
      "a direct link is present",
      "SST::Mordred::RtrPortControlAPI" },
    { "vc_alloc", "VC allocator", "SST::Mordred::VcAllocAPI" },
    { "arbiter", "Arbitration scheme/model", "SST::Mordred::ArbAPI" },
  )

  SST_ELI_DOCUMENT_PORTS(
    // TODO: Add message types as appropriate
    { "port%(portnum)d", "Port id.", { "basicMordredEvent" } },
  )

  SST_ELI_DOCUMENT_STATISTICS(
    { "xbar_idle", "For each receiving port, num cycles with an idle crossbar", "unitless", 3 },
    { "xbar_blocked", "For each receiving port, num cycles crossbar blocked", "unitless", 3 },
    { "flit_unavailable", "Port does not have the flit to send thru the crossbar", "unitless", 3 }
  )

  MordredRouter( ComponentId_t cid, Params& params );
  ~MordredRouter();

  /// SST Required
  void init( uint32_t phase ) override;
  void setup() override;
  void complete( uint32_t phase ) override;
  void finish() override;

  // Clock Handler
  bool clockTick( Cycle_t cycle );

  /// default constructor
  MordredRouter() : SST::Component() {}

  /// serialization
  void serialize_order( SST::Core::Serialization::serializer& ser ) override {
    SST_SER( output );
    SST_SER( id );
    SST_SER( timeConverter );
    SST_SER( numPorts );
    SST_SER( numLocalPorts );
    SST_SER( numVns );
    SST_SER( numVcs );
    SST_SER( topology );
    SST_SER( arbiter );
    SST_SER( vcAlloc );
    SST_SER( portsVec );
    SST_SER( perPortConnectedRtr );
    SST_SER( perPortSharedObjs );
    SST_SER( untimedInitEventsQ );
    SST_SER( statPerPortXbarIdle );
    SST_SER( statPerPortXbarBlocked );
    SST_SER( statPerPortFlitUnavailable );
  }

  /// serialization implementations
  ImplementSerializable( SST::Mordred::MordredRouter );

private:
  Output        output;
  uint32_t      id;
  TimeConverter timeConverter;
  uint32_t      numPorts{};
  uint32_t      numLocalPorts{};
  uint32_t      numVns{};
  uint32_t      numVcs{};

  // Major components
  TopologyAPI* topology{ nullptr };
  XbarArbAPI*  arbiter{ nullptr };
  VcAllocAPI*  vcAlloc{ nullptr };
  // If a port is unconnected, we push_back a nullptr for that port index
  std::vector<RtrPortControlAPI*> portsVec;

  // Values of UINT32_MAX represent either 1) unconnected ports or 2) local ports
  std::vector<uint32_t> perPortConnectedRtr;  // shared with topology

  // Shared between components
  std::vector<RtrOwnedSharedObjs> perPortSharedObjs;

  // For untimed receives/sends
  std::queue<Event*> untimedInitEventsQ;

  // Stats
  std::vector<Statistic<uint64_t>*> statPerPortXbarIdle;
  std::vector<Statistic<uint64_t>*> statPerPortXbarBlocked;
  std::vector<Statistic<uint64_t>*> statPerPortFlitUnavailable;

};  // MordredRouter

}  // namespace SST::Mordred

#endif  //MORDRED_MORDREDROUTER_H
