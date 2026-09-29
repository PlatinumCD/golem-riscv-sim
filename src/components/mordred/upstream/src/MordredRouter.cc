//
// MordredRouter.cc
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#include <cinttypes>
#include <cstdint>
#include <string>

#include "MordredEvents.h"
#include "MordredRouter.h"
#include "RtrPortControlAPI.h"

using namespace SST;
using namespace SST::Mordred;

MordredRouter::MordredRouter( ComponentId_t cid, Params& params ) : Component( cid ) {

  auto Verbosity = params.find<uint32_t>( "verbose", MORDRED_VERBOSE_MED );
  // Initialize the output handler
  output.init( "MordredRouter[" + getName() + ":@p:@t]: ", Verbosity, 0, SST::Output::STDOUT );

  id = params.find<uint32_t>( "id", UINT32_MAX );
  if( id == UINT32_MAX ) {
    output.fatal( CALL_INFO, -1, "MordredRouter requires id to be specified\n" );
  }

  auto clockFreq = params.find<std::string>( "clock", "1GHz" );
  timeConverter  = registerClock( clockFreq, new Clock::Handler2<MordredRouter, &MordredRouter::clockTick>( this ) );

  numPorts       = params.find<uint32_t>( "num_ports", 3 );
  numLocalPorts  = params.find<uint32_t>( "num_local_ports", UINT32_MAX );
  if( numLocalPorts == UINT32_MAX ) {
    output.fatal( CALL_INFO, -1, "MordredRouter requires num_local_ports to be specified\n" );
  }
  numVns = params.find<uint32_t>( "num_vns", 1 );
  numVcs = params.find<uint32_t>( "num_vcs", 1 );

  // Leaving here as a sanity check - assumes more than one router in the system
  if( numPorts <= numLocalPorts )
    output.fatal( CALL_INFO, -1, "num_ports must be greater than num_local_ports\n" );

  // Load subcomponents
  topology =
    loadUserSubComponent<TopologyAPI>( "topology", ComponentInfo::SHARE_NONE, id, numPorts, numLocalPorts, &perPortConnectedRtr );
  if( !topology )
    output.fatal( CALL_INFO, -1, "Couldn't load topology\n" );

  perPortSharedObjs.resize( numPorts );
  // Configure local/endpt ports.
  // Preference: user-provided portcontrol subcomponent at each slot index
  // (e.g. rtrPortControlPC for SimpleNetwork-backed ports).
  // Fallback: anonymous rtrPortControl when a direct link named "portN" is present.
  // An unconnected port produces a nullptr entry.
  SubComponentSlotInfo* port_ctrl_slot = getSubComponentSlotInfo( "portcontrol" );

  for( uint32_t i = 0; i < numPorts; i++ ) {
    std::string        linkname = "port" + std::to_string( i );
    RtrPortControlAPI* pc       = nullptr;

    if( port_ctrl_slot && port_ctrl_slot->isPopulated( static_cast<int>( i ) ) ) {
      // User-configured portcontrol at this index (params come from Python slot config)
      perPortSharedObjs.at( i ).allocateVecs( numVns, numVcs );
      pc = port_ctrl_slot->create<RtrPortControlAPI>(
        static_cast<int>( i ), ComponentInfo::SHARE_PORTS | ComponentInfo::INSERT_STATS, topology, &perPortSharedObjs[i], id, i
      );
    } else if( isPortConnected( linkname ) ) {
      // Legacy/default: direct link with anonymous rtrPortControl
      perPortSharedObjs.at( i ).allocateVecs( numVns, numVcs );
      pc = loadAnonymousSubComponent<RtrPortControlAPI>(
        "mordred.rtrPortControl",
        "portcontrol",
        static_cast<int>( i ),
        ComponentInfo::SHARE_PORTS | ComponentInfo::INSERT_STATS,
        params,
        topology,
        &perPortSharedObjs[i],
        id,
        i
      );
    } else {
      output.verbose( CALL_INFO, MORDRED_VERBOSE_MED, 0, "Port %u (%s) unconnected\n", i, linkname.c_str() );
    }

    portsVec.push_back( pc );
  }
  delete port_ctrl_slot;

  arbiter = loadAnonymousSubComponent<XbarArbAPI>(
    "mordred.xbarArbRR", "arbiter", 0, ComponentInfo::SHARE_NONE, params, id, numPorts, numVns, numVcs
  );
  if( arbiter == nullptr ) {
    output.fatal( CALL_INFO, -1, "arbiter is a nullptr\n" );
  }

  vcAlloc = loadAnonymousSubComponent<VcAllocAPI>(
    "mordred.VcAllocRR", "vcAlloc", 0, ComponentInfo::SHARE_NONE, params, id, numPorts, numVns, numVcs
  );
  if( vcAlloc == nullptr ) {
    output.fatal( CALL_INFO, -1, "vcAlloc is a nullptr\n" );
  }

  // Register Stats
  for( uint32_t i = 0; i < numPorts; i++ ) {
    std::string portstr = "port" + std::to_string( i );
    statPerPortXbarIdle.push_back( registerStatistic<uint64_t>( "xbar_idle", portstr.c_str() ) );
    statPerPortXbarBlocked.push_back( registerStatistic<uint64_t>( "xbar_blocked", portstr.c_str() ) );
    statPerPortFlitUnavailable.push_back( registerStatistic<uint64_t>( "flit_unavailable", portstr.c_str() ) );
  }

  output.verbose(
    CALL_INFO,
    MORDRED_VERBOSE_MIN,
    0,
    "Constructor complete for %s. local_ports=%" PRIu32 "; rtr_ports=%" PRIu32 "\n",
    getName().c_str(),
    numLocalPorts,
    numPorts - numLocalPorts
  );
  output.flush();
}

MordredRouter::~MordredRouter() {
  for( auto& port : portsVec )
    delete port;
  delete vcAlloc;
  delete arbiter;
  delete topology;
}

void MordredRouter::init( uint32_t phase ) {
  output.verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0, "MordredRouter::init(%" PRIu32 ")\n", phase );
  output.flush();

  topology->init( phase );
  vcAlloc->init( phase );
  arbiter->init( phase );

  if( phase == 2 ) {
    // This block has to be done during the init phase
    // certain topologies will need this vector to be completed before
    // we try routing untimed messages from endpoints in later
    // init phases
    perPortConnectedRtr.resize( numPorts, UINT32_MAX );
    for( uint32_t i = 0; i < numPorts; i++ ) {
      if( portsVec.at( i ) != nullptr )
        perPortConnectedRtr.at( i ) = portsVec.at( i )->getConnectedRtrId();
    }
  }

  for( auto& port : portsVec ) {
    if( port == nullptr )
      continue;
    port->init( phase );
    /// Once we've passed the network initialized phase, some endpoint setups will want to do some
    /// type of initialization themselves.  So we need to grab the untimed packets from each port
    /// (which are in port.initEvents), route them, and send towards their final destinations
    if( phase >= 4 ) {
      Event* ev = port->recvUntimedData();
      while( ev != nullptr ) {
        output.verbose( CALL_INFO, MORDRED_VERBOSE_MED, 0, "Received untimed data packet\n" );
        auto init_ev = static_cast<MordredInitEvent*>( ev );
        if( init_ev->req->dest == Interfaces::SimpleNetwork::INIT_BROADCAST_ADDR ) {
          std::vector<Event*> out_events( numPorts, nullptr );
          topology->routeUntimedBroadcastPacket( port->getPortId(), init_ev, out_events );
          for( uint32_t i = 0; i < numPorts; i++ ) {
            if( out_events.at( i ) != nullptr )
              portsVec.at( i )->sendUntimedData( out_events.at( i ) );
          }
        } else {
          auto dest_port = topology->routePacket( init_ev->req->dest );
          output.verbose(
            CALL_INFO,
            MORDRED_VERBOSE_MED,
            0,
            "Determined route of untimed data packet; dest=%" PRId64 ", dest_port=%u\n",
            init_ev->req->dest,
            dest_port
          );
          portsVec.at( dest_port )->sendUntimedData( ev );
        }
        ev = port->recvUntimedData();
      }
    }
  }
}

void MordredRouter::setup() {
  output.verbose(CALL_INFO, MORDRED_VERBOSE_HIGH, 0, "MordredRouter::setup\n");
  output.flush();

  topology->setup();
  vcAlloc->setup();
  arbiter->setup();
  for( auto& port : portsVec )
    if( port != nullptr )
      port->setup();
}

void MordredRouter::complete( uint32_t phase ) {
  output.verbose(CALL_INFO, MORDRED_VERBOSE_HIGH, 0, "MordredRouter::complete(%" PRIu32 ")\n", phase);
  output.flush();

  topology->complete( phase );
  vcAlloc->complete( phase );
  arbiter->complete( phase );
  for( auto& port : portsVec ) {
    if( port == nullptr )
      continue;

    port->complete( phase );
    while( true ) {
      Event* ev = port->recvUntimedData();
      if( ev == nullptr )
        break;  // jump out of while loop
      auto init_ev = static_cast<MordredInitEvent*>( ev );
      if( init_ev->req->dest == Interfaces::SimpleNetwork::INIT_BROADCAST_ADDR ) {
        std::vector<Event*> out_events( numPorts, nullptr );
        topology->routeUntimedBroadcastPacket( port->getPortId(), init_ev, out_events );
        for( uint32_t i = 0; i < numPorts; i++ ) {
          if( out_events.at( i ) != nullptr )
            portsVec.at( i )->sendUntimedData( out_events.at( i ) );
        }
      } else {
        auto dest_port = topology->routePacket( init_ev->req->dest );
        portsVec.at( dest_port )->sendUntimedData( ev );
      }
    }
  }
}

void MordredRouter::finish() {
  output.verbose(CALL_INFO, MORDRED_VERBOSE_HIGH, 0, "MordredRouter::finish\n");
  output.flush();
  topology->finish();
  vcAlloc->finish();
  arbiter->finish();
  for( auto& port : portsVec )
    if( port != nullptr )
      port->finish();
}

bool MordredRouter::clockTick( Cycle_t cycle ) {
  // May want/need to look at how we want to time/order ticking the ports and running the crossbar/arbitration here

  // For all router ports, see if we can receive a flit through the crossbar
  // portsVec.at(i) is the receiving port in this loop
  for( uint32_t i = 0; i < numPorts; i++ ) {
    if( portsVec.at( i ) == nullptr )
      continue;

    auto sending_port = portsVec.at( i )->getSendingPort();  //ensures we have a sender and a credit available
    if( sending_port == UINT32_MAX ) {
      statPerPortXbarIdle.at( i )->addData( 1 );
      continue;
    }
    if( sending_port == ( UINT32_MAX - 1 ) ) {
      statPerPortXbarBlocked.at( i )->addData( 1 );
      continue;
    }

    // Get the flit from the sender -- multiple checks there for invalid/null concerns
    auto flit = portsVec.at( sending_port )->getInBufFlit();
    if( flit == nullptr ) {
      // can happen if there was a delay due to a lack of credits
      statPerPortFlitUnavailable.at( sending_port )->addData( 1 );
      continue;
    }

    // Give the flit to the receiver
    portsVec.at( i )->recvOutBufFlit( flit );

    // Do tracing
    if( ( flit->ftype == MordredFlit::HEAD ) &&
        ( flit->getRequest()->getTraceType() == Interfaces::SimpleNetwork::Request::FULL ) ) {
      std::pair<uint32_t, uint32_t> src_vn_vc, dest_vn_vc;
      src_vn_vc  = portsVec.at( sending_port )->getSwitchSendVnVc();
      dest_vn_vc = portsVec.at( i )->getSwitchRecvVnVc();
      output.output(
        "TRACE(%d): %" PRIu64 " ns at %s: Move head flit from port.vn.vc = %u.%u.%u to %u.%u.%u\n",
        flit->getRequest()->getTraceID(),
        getCurrentSimTimeNano(),
        getName().c_str(),
        sending_port,
        src_vn_vc.first,
        src_vn_vc.second,
        i,
        dest_vn_vc.first,
        dest_vn_vc.second
      );
    }

    if( flit->ftype == MordredFlit::TAIL ) {
      // These MUST be reset prior to calling the resetSwitch{Send,Recv}Allocation functions in a clockTick
      // as the next two lines rely on the values that are reset when calling resetSwitch{Send,Recv}Allocation
      portsVec.at( sending_port )->resetPerVcDest();
      portsVec.at( i )->resetPerVcSrc();
      // The switch allocation could be done more frequently than on a packet basis
      portsVec.at( sending_port )->resetSwitchSendAllocation();
      portsVec.at( i )->resetSwitchRecvAllocation();
      output.verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0, "Tail flit %s observed\n", flit->pktIdStr().c_str() );
    }
  }

  output.verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0, "Cycle=%" PRIu64 "\n", cycle );
  output.flush();
  arbiter->arbitrate( portsVec, perPortSharedObjs );

  vcAlloc->arbitrate( portsVec, perPortSharedObjs );

  // Let the port do its work
  for( auto& port : portsVec ) {
    if( port == nullptr )
      continue;
    port->ClockTick( cycle );
  }

  return false;
}
