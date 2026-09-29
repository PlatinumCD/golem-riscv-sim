//
// XbarArbRR.cc
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//
//

#include <cinttypes>
#include <string>

// Local SST config
#include "sst_config.h"

#include "MordredEvents.h"
#include "XbarArbRR.h"

using namespace SST::Mordred;

XbarArbRR::XbarArbRR( ComponentId_t id, Params& params, uint32_t rtr_id, uint32_t num_ports, uint32_t num_vns, uint32_t num_vcs )
  : XbarArbAPI( id ), rtrId( rtr_id ), numPorts( num_ports ), numVns( num_vns ), numVcs( num_vcs ) {
  const auto verbosity = params.find<uint32_t>( "verbose", MORDRED_VERBOSE_MED );
  output               = new Output( "ArbRR[[" + std::to_string( rtrId ) + "]:@p:@t]: ", verbosity, 0, Output::STDOUT );

  resetSendingVnVc();
}

// This code is nearly vomit inducing; using continues to reduce the nesting a little bit
void XbarArbRR::arbitrate( std::vector<RtrPortControlAPI*>& ports, std::vector<RtrOwnedSharedObjs>& rtr_shared_objs ) {
  resetSendingVnVc();
  for( uint32_t i = 0, rcvportnum = recv_rr_port; i < numPorts; ++i, rcvportnum = ( rcvportnum + 1 ) % numPorts ) {
    if( ports.at( rcvportnum ) == nullptr )
      continue;
    if( ports.at( rcvportnum )->isRecvAllocatedFromSwitch() )  // already receiving from someone
      continue;
    // rcvportnum is not actively receiving from the switch, so RR through the ports and find the next sender
    for( uint32_t j = 0, sendportnum = send_rr_port; j < numPorts; ++j, sendportnum = ( sendportnum + 1 ) % numPorts ) {
      //if ( sendportnum == rcvportnum ) // disallow sending back to self
      //  continue;
      if( ports.at( sendportnum ) == nullptr )  // skip invalid ports
        continue;
      if( ports.at( sendportnum )->isSendAllocatedToSwitch() )  // already sending to someone
        continue;
      if( findSendableFlit( rcvportnum, ports.at( sendportnum ), rtr_shared_objs.at( sendportnum ) ) ) {
        // We have a sendable flit, so notify/update send/recv ports; clear flit from shared struct
        ports.at( sendportnum )->sendAllocateToSwitch( rcvportnum, sending_vn, sending_vc );
        auto dest_vc = ports.at( sendportnum )->getDestVc( sending_vn, sending_vc );
        ports.at( rcvportnum )->recvAllocateFromSwitch( sendportnum, sending_vn, dest_vc );
        rtr_shared_objs.at( sendportnum ).needSwitchAlloc.at( sending_vn ).at( sending_vc ) = nullptr;
        output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0,
                         "SwitchArb flit [Port:VN:VC] from [%" PRIu32 ":%" PRIu32 ":%" PRIu32 "] to [%" PRIu32 ":%" PRIu32 ":%" PRIu32 "]\n",
                         sendportnum, sending_vn, sending_vc, rcvportnum, sending_vn, dest_vc);
        output->flush();
        resetSendingVnVc();
        break;  // found a matching sender, no need to do another one
      }
    }
  }
  // Update start values
  recv_rr_port = ( recv_rr_port + 1 ) % numPorts;
  send_rr_port = ( send_rr_port + 1 ) % numPorts;
  send_rr_vn   = ( send_rr_vn + 1 ) % numVns;
  send_rr_vc   = ( send_rr_vc + 1 ) % numVcs;
}

bool XbarArbRR::findSendableFlit( uint32_t rcvportnum, RtrPortControlAPI*& sendport, RtrOwnedSharedObjs& shared_obj ) {
  for( uint32_t i = 0, vn = send_rr_vn; i < numVns; ++i, vn = ( vn + 1 ) % numVns ) {
    for( uint32_t j = 0, vc = send_rr_vc; j < numVcs; ++j, vc = ( vc + 1 ) % numVcs ) {
      if( shared_obj.needSwitchAlloc.at( vn ).at( vc ) != nullptr ) {
        if( rcvportnum == sendport->getDestPort( vn, vc ) ) {
          // We have a winner!
          output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0,
                           "Flit %s wins the switch\n",
                           shared_obj.needSwitchAlloc.at(vn).at(vc)->pktIdStr().c_str() );
          output->flush();
          sending_vn = vn;
          sending_vc = vc;
          return true;
        }
      }
    }
  }
  return false;
}
