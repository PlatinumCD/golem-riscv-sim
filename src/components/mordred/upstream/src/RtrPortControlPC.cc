//
// RtrPortControlPC.cc
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#include "RtrPortControlPC.h"

using namespace SST;
using namespace SST::Mordred;

RtrPortControlPC::RtrPortControlPC(
  ComponentId_t id, Params& params, TopologyAPI* topology,
  RtrOwnedSharedObjs* rtr_shared_objs, uint32_t rtr_num, uint32_t port_num
) : RtrPortControlBase( id, params, topology, rtr_shared_objs, rtr_num, port_num, "RtrPortControlPC" ) {
  // numVns * numVcs: give every (vn,vc) pair its own independent UCIe queue and
  // credit pool (see extVn() in RtrPortControlPC.h) instead of sharing one per vn.
  physChannel = loadUserSubComponent<Prydwen::PhysChannelAPI>(
    "port_iface", ComponentInfo::SHARE_PORTS | ComponentInfo::INSERT_STATS, static_cast<int>( numVns * numVcs )
  );
  if( !physChannel )
    output->fatal( CALL_INFO, -1, "RtrPortControlPC: no PhysChannelAPI subcomponent in slot 'port_iface'\n" );

  physChannel->setNotifyOnReceive(
    new Prydwen::PhysChannelAPI::Handler2<RtrPortControlPC, &RtrPortControlPC::onReceive>( this )
  );

  output->verbose(
    CALL_INFO, MORDRED_VERBOSE_MIN, 0,
    "Constructor complete; [Rtr.Port]=[%" PRIu32 ".%" PRIu32 "], inbuf=%" PRIu32 "b, outbuf=%" PRIu32 "b\n",
    rtrId, portId, inBufSize, outBufSize
  );
}

bool RtrPortControlPC::onReceive( int sn_vn ) {
  SST::Event* ev = nullptr;
  while( ( ev = physChannel->recv( sn_vn ) ) != nullptr ) {
    // Credits are control events that must be processed immediately so that
    // upstream credit counts stay accurate regardless of flit draining pace.
    auto* base = dynamic_cast<baseMordredEvent*>( ev );
    if( base && base->getType() == baseMordredEvent::CREDIT ) {
      processIncomingEvent( ev );
    } else {
      output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0,
                         "Flit %s received\n",
                         ((MordredFlit*)base)->pktIdStr().c_str() );
      pendingFlits_.push_back( ev );
    }
  }
  return true;
}

void RtrPortControlPC::ClockTick( Cycle_t cycle ) {
  // Drip one pending flit per tick to match native link timing (one flit/cycle).
  if( !pendingFlits_.empty() ) {
    SST::Event* ev = pendingFlits_.front();
    pendingFlits_.pop_front();
    processIncomingEvent( ev );
  }
  RtrPortControlBase::ClockTick( cycle );
}
