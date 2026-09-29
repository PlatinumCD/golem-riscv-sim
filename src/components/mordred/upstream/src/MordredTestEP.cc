//
// MordredTestEP.cc
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#include "MordredTestEP.h"

using namespace SST;
using namespace SST::Mordred;

MordredTestEP::MordredTestEP( ComponentId_t id, Params& params )
    : SST::Component( id ) {

  const int verbose = params.find<int>( "verbose", 0 );
  output.init( "MordredTestEP[@p:@l]: ", static_cast<uint32_t>( verbose ), 0u, Output::STDOUT );

  ep_id      = static_cast<SST::Interfaces::SimpleNetwork::nid_t>( params.find<int64_t>( "id", 0 ) );
  num_peers  = params.find<int64_t>( "num_peers", 1 );
  num_vns    = params.find<int32_t>( "num_vns", 1 );
  num_messages = params.find<int64_t>( "num_messages", 10 );

  auto msg_ua = params.find<UnitAlgebra>( "message_size", UnitAlgebra( "64b" ) );
  if( msg_ua.hasUnits( "B" ) )
    msg_ua *= UnitAlgebra( "8b" );
  msg_size_bits = static_cast<int32_t>( msg_ua.getRoundedValue() );

  link_control = loadUserSubComponent<SST::Interfaces::SimpleNetwork>(
    "networkIF", ComponentInfo::SHARE_NONE, num_vns
  );
  if( !link_control )
    output.fatal( CALL_INFO, -1, "MordredTestEP: no subcomponent in slot 'networkIF'\n" );

  link_control->setNotifyOnReceive(
    new SST::Interfaces::SimpleNetwork::Handler2<MordredTestEP, &MordredTestEP::onReceive>( this )
  );

  const auto clk_str = params.find<std::string>( "clock", "1GHz" );
  registerClock( clk_str, new Clock::Handler2<MordredTestEP, &MordredTestEP::clockTick>( this ) );

  primaryComponentDoNotEndSim();

  output.verbose( CALL_INFO, 1, 0,
    "Constructed: id=%" PRId64 ", num_peers=%" PRId64 ", num_vns=%d, "
    "num_messages=%" PRId64 ", msg_size=%db\n",
    static_cast<int64_t>( ep_id ), num_peers, num_vns, num_messages, msg_size_bits );
}

void MordredTestEP::init( uint32_t phase ) {
  link_control->init( phase );
}

void MordredTestEP::setup() {
  link_control->setup();
  ep_id = link_control->getEndpointID();
  output.verbose( CALL_INFO, 1, 0,
    "Setup complete: ep_id=%" PRId64 "\n", static_cast<int64_t>( ep_id ) );
}

void MordredTestEP::complete( uint32_t phase ) {
  link_control->complete( phase );
}

bool MordredTestEP::clockTick( Cycle_t /*cycle*/ ) {
  // Drain all pending receives across every VN
  for( int vn = 0; vn < num_vns; vn++ ) {
    SST::Interfaces::SimpleNetwork::Request* req;
    while( ( req = link_control->recv( vn ) ) != nullptr ) {
      received++;
      output.verbose( CALL_INFO, 2, 0,
        "Received message %" PRId64 " from %" PRId64 " on VN %d\n",
        received, req->src, vn );
      delete req;
    }
  }

  // Try to send the next message
  if( sent < num_messages ) {
    const int vn = static_cast<int>( sent % num_vns );
    if( link_control->spaceToSend( vn, msg_size_bits ) ) {
      auto* req          = new SST::Interfaces::SimpleNetwork::Request();
      req->dest          = ( ep_id + sent + 1 ) % num_peers;
      req->src           = ep_id;
      req->size_in_bits  = static_cast<size_t>( msg_size_bits );
      req->vn            = vn;
      req->allow_adaptive = false;
      link_control->send( req, vn );
      sent++;
      output.verbose( CALL_INFO, 2, 0,
        "Sent message %" PRId64 " to %" PRId64 " on VN %d\n",
        sent, req->dest, vn );
    }
  }

  if( !done && sent >= num_messages ) {
    done = true;
    primaryComponentOKToEndSim();
    output.verbose( CALL_INFO, 1, 0,
      "All %" PRId64 " messages sent; OKToEndSim\n", num_messages );
  }

  return false;
}

bool MordredTestEP::onReceive( int /*vn*/ ) {
  // Actual drain happens in clockTick; nothing to do here
  return true;
}

void MordredTestEP::finish() {
  output.verbose( CALL_INFO, 1, 0,
    "Finish: sent=%" PRId64 ", received=%" PRId64 "\n", sent, received );
}
