//
// RtrPortControlBase.cc
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#include <cinttypes>
#include <string>

#include "sst_config.h"
#include "MordredEvents.h"
#include "RtrPortControlBase.h"
#include "TopologyAPI.h"

using namespace SST::Mordred;

RtrPortControlBase::RtrPortControlBase(
  ComponentId_t       id,
  Params&             params,
  TopologyAPI*        topology,
  RtrOwnedSharedObjs* rtr_shared_objs,
  uint32_t            rtr_num,
  uint32_t            port_num,
  const char*         class_name
) : RtrPortControlAPI( id ),
    topo( topology ),
    rtrId( rtr_num ),
    portId( port_num ),
    rtrSharedObjs( rtr_shared_objs ) {
  const auto verbosity = params.find<uint32_t>( "verbose", MORDRED_VERBOSE_MED );
  output               = new Output(
    std::string( class_name ) + "[[" + std::to_string( rtrId ) + "." + std::to_string( portId ) + "]:@p:@t]: ",
    verbosity, 0, Output::STDOUT
  );

  if( rtrSharedObjs == nullptr )
    output->fatal( CALL_INFO_LONG, 1, "%s: rtr_shared_objs must not be null\n", class_name );

  numVns = rtrSharedObjs->needVcAlloc.size();
  numVcs = rtrSharedObjs->needVcAlloc.at( 0 ).size();

  auto flit_size_ua = params.find<UnitAlgebra>( "flit_size", "32b" );
  if( !flit_size_ua.hasUnits( "b" ) && !flit_size_ua.hasUnits( "B" ) )
    output->fatal(
      CALL_INFO, -1, "%s: flit_size must be in bits (b) or bytes (B): %s\n",
      class_name, flit_size_ua.toStringBestSI().c_str()
    );
  if( flit_size_ua.hasUnits( "B" ) )
    flit_size_ua *= UnitAlgebra( "8b" );
  flitSize = static_cast<uint32_t>( flit_size_ua.getRoundedValue() );

  bool found       = false;
  auto buf_size_ua = params.find<UnitAlgebra>( "input_buf_size", found );
  if( !found )
    output->fatal( CALL_INFO_LONG, 1, "%s: input_buf_size must be specified\n", class_name );
  if( !buf_size_ua.hasUnits( "b" ) && !buf_size_ua.hasUnits( "B" ) )
    output->fatal(
      CALL_INFO, -1, "%s: input_buf_size must be in bits (b) or bytes (B): %s\n",
      class_name, buf_size_ua.toStringBestSI().c_str()
    );
  if( buf_size_ua.hasUnits( "B" ) )
    buf_size_ua *= UnitAlgebra( "8b/B" );
  inBufSize = static_cast<uint32_t>( buf_size_ua.getRoundedValue() );
  if( flitSize > inBufSize )
    output->fatal(
      CALL_INFO, 1, "Invalid configuration; flit_size=%" PRIu32 "b > input_buf_size=%" PRIu32 "b\n",
      flitSize, inBufSize
    );

  buf_size_ua = params.find<UnitAlgebra>( "output_buf_size", found );
  if( !found )
    output->fatal( CALL_INFO_LONG, 1, "%s: output_buf_size must be specified\n", class_name );
  if( !buf_size_ua.hasUnits( "b" ) && !buf_size_ua.hasUnits( "B" ) )
    output->fatal(
      CALL_INFO, -1, "%s: output_buf_size must be in bits (b) or bytes (B): %s\n",
      class_name, buf_size_ua.toStringBestSI().c_str()
    );
  if( buf_size_ua.hasUnits( "B" ) )
    buf_size_ua *= UnitAlgebra( "8b/B" );
  outBufSize = static_cast<uint32_t>( buf_size_ua.getRoundedValue() );
  if( flitSize > outBufSize )
    output->fatal(
      CALL_INFO, 1, "Invalid configuration; flit_size=%" PRIu32 "b > output_buf_size=%" PRIu32 "b\n",
      flitSize, outBufSize
    );

  allocateBuffers();
}

void RtrPortControlBase::allocateBuffers() {
  const auto total_credits = outBufSize / flitSize;

  inStateVec.resize( numVns );
  outStateVec.resize( numVns );
  statLinkRecvFlitCnt.resize( numVns );
  statLinkSentFlitCnt.resize( numVns );
  statLinkSentPacketCnt.resize( numVns );

  for( uint32_t i = 0; i < numVns; i++ ) {
    inStateVec.at( i ).resize( numVcs );
    outStateVec.at( i ).resize( numVcs );
    statLinkRecvFlitCnt.at( i ).resize( numVcs, nullptr );
    statLinkSentFlitCnt.at( i ).resize( numVcs, nullptr );
    statLinkSentPacketCnt.at( i ).resize( numVcs, nullptr );
  }

  for( uint32_t i = 0; i < numVns; i++ ) {
    for( uint32_t j = 0; j < numVcs; j++ ) {
      inStateVec.at( i ).at( j ).reset();
      outStateVec.at( i ).at( j ).reset( static_cast<int16_t>( total_credits ) );
    }
  }

  for( uint32_t i = 0; i < numVns; i++ ) {
    for( uint32_t j = 0; j < numVcs; j++ ) {
      const std::string str =
        std::to_string( rtrId ) + "_" + std::to_string( portId ) + "_" + std::to_string( i ) + "_" + std::to_string( j );
      statLinkRecvFlitCnt.at( i ).at( j )   = registerStatistic<uint64_t>( "recv_flit_cnt", str.c_str() );
      statLinkSentFlitCnt.at( i ).at( j )   = registerStatistic<uint64_t>( "sent_flit_cnt", str.c_str() );
      statLinkSentPacketCnt.at( i ).at( j ) = registerStatistic<uint64_t>( "sent_packet_cnt", str.c_str() );
    }
  }
  statLinkOutputStalledCnt = registerStatistic<uint64_t>( "output_stalls" );
}

void RtrPortControlBase::init( unsigned int phase ) {
  transportInit( phase );

  switch( phase ) {
  case 0: {
    auto* ev    = new MordredInitEvent();
    ev->command = MordredInitEvent::REPORT_ROUTER;
    transportSendUntimedData( ev );

    ev          = new MordredInitEvent();
    ev->command = MordredInitEvent::ROUTER_ID;
    ev->value   = rtrId;
    transportSendUntimedData( ev );

    ev          = new MordredInitEvent();
    ev->command = MordredInitEvent::PORT_NUM;
    ev->value   = portId;
    transportSendUntimedData( ev );
    break;
  }

  case 1: {
    auto* raw = transportRecvUntimedData();
    if( raw == nullptr )
      output->fatal( CALL_INFO, -1, "Unable to recv init event in phase 1\n" );

    auto* init_ev = static_cast<MordredInitEvent*>( raw );
    if( init_ev->command == MordredInitEvent::REPORT_ROUTER ) {
      connectionType = ROUTER;
    } else if( init_ev->command == MordredInitEvent::REPORT_ENDPOINT ) {
      connectionType = ENDPOINT;
      connectedRtrId = UINT32_MAX - 1;
    } else {
      output->fatal( CALL_INFO, -1, "Unexpected command=%d in phase 1\n", (int) init_ev->command );
    }
    delete raw;

    if( connectionType == ROUTER ) {
      auto* ev_id    = getInitEvent( MordredInitEvent::ROUTER_ID );
      connectedRtrId = ev_id->value;
      delete ev_id;

      auto* ev_port   = getInitEvent( MordredInitEvent::PORT_NUM );
      connectedPortId = ev_port->value;
      output->verbose(
        CALL_INFO, MORDRED_VERBOSE_MED, 0, "Received init from [Rtr.Port]=[%" PRIu32 ".%" PRIu32 "]\n",
        connectedRtrId, connectedPortId
      );
      delete ev_port;
    } else {
      auto* ev    = new MordredInitEvent();
      ev->command = MordredInitEvent::NUM_VNS;
      ev->value   = numVns;
      transportSendUntimedData( ev );

      ev          = new MordredInitEvent();
      ev->command = MordredInitEvent::NUM_VCS;
      ev->value   = numVcs;
      transportSendUntimedData( ev );

      ev          = new MordredInitEvent();
      ev->command = MordredInitEvent::FLIT_WIDTH;
      ev->value   = flitSize;
      transportSendUntimedData( ev );

      output->verbose( CALL_INFO, MORDRED_VERBOSE_MED, 0, "Sent VNS/VCS/flit_size to endpoint\n" );
    }
    break;
  }

  case 2: {
    if( connectionType != ENDPOINT )
      break;
    auto* ev    = new MordredInitEvent();
    ev->command = MordredInitEvent::ENDPOINT_ID;
    ev->value   = (uint32_t) topo->getEndpointId( portId );
    transportSendUntimedData( ev );
    break;
  }

  case 3: {
    const auto total_credits = static_cast<int32_t>( inBufSize / flitSize );
    const uint32_t max_vc    = ( connectionType == ROUTER ) ? numVcs : 1;
    for( uint32_t i = 0; i < numVns; i++ ) {
      for( uint32_t j = 0; j < max_vc; j++ ) {
        transportSendUntimedData( new MordredCreditEvent( i, j, total_credits ) );
      }
    }
    break;
  }

  default: {
    SST::Event* ev = nullptr;
    while( ( ev = transportRecvUntimedData() ) != nullptr ) {
      auto* base_ev = static_cast<baseMordredEvent*>( ev );
      if( base_ev->getType() == baseMordredEvent::CREDIT ) {
        auto* credit_ev = static_cast<MordredCreditEvent*>( ev );
        outStateVec.at( credit_ev->vn ).at( credit_ev->vc ).destCredits += credit_ev->credits;
        output->verbose(
          CALL_INFO, MORDRED_VERBOSE_MED, 0, "Received credit vn=%d, vc=%d, credits=%d\n",
          credit_ev->vn, credit_ev->vc, credit_ev->credits
        );
        delete ev;
      } else if( base_ev->getType() == baseMordredEvent::PACKET ) {
        initEvents.push( ev );
      } else {
        output->verbose( CALL_INFO, MORDRED_VERBOSE_MED, 0, "Unexpected event type=%d in init\n", (int) base_ev->getType() );
        delete ev;
      }
    }
    break;
  }
  }
  output->verbose( CALL_INFO, MORDRED_VERBOSE_MED, 0, "END init phase=%" PRIu32 "\n", phase );
  output->flush();
}

void RtrPortControlBase::setup() {
  transportSetup();
}

void RtrPortControlBase::complete( unsigned int phase ) {
  transportComplete( phase );

  SST::Event* ev = nullptr;
  while( ( ev = transportRecvUntimedData() ) != nullptr ) {
    auto* base_ev = static_cast<baseMordredEvent*>( ev );
    if( base_ev->getType() == baseMordredEvent::CREDIT ) {
      auto* credit_ev = static_cast<MordredCreditEvent*>( ev );
      outStateVec.at( credit_ev->vn ).at( credit_ev->vc ).destCredits += credit_ev->credits;
      delete ev;
    } else if( base_ev->getType() == baseMordredEvent::PACKET ) {
      initEvents.push( ev );
    } else {
      output->verbose( CALL_INFO, MORDRED_VERBOSE_MED, 0, "Unexpected event type=%d in complete\n", (int) base_ev->getType() );
      delete ev;
    }
  }
}

void RtrPortControlBase::sendUntimedData( Event* ev ) {
  transportSendUntimedData( ev );
}

SST::Event* RtrPortControlBase::recvUntimedData() {
  if( initEvents.empty() )
    return nullptr;
  auto* ev = initEvents.front();
  initEvents.pop();
  return ev;
}

void RtrPortControlBase::ClockTick( Cycle_t cycle ) {
  // Input pipeline: IN_IDLE → ROUTING → WAIT_VC → IN_ACTIVE
  for( uint32_t vn = 0; vn < numVns; vn++ ) {
    for( uint32_t vc = 0; vc < numVcs; vc++ ) {
      if( inStateVec.at( vn ).at( vc ).inVcState == IN_IDLE ) {
        if( rtrSharedObjs->needVcAlloc.at( vn ).at( vc ) != nullptr )
          output->fatal( CALL_INFO, -1, "Expected nullptr in IN_IDLE\n" );
        if( !inStateVec.at( vn ).at( vc ).inBuf.empty() ) {
          auto* flit = inStateVec.at( vn ).at( vc ).inBuf.front();
          if( flit->ftype != MordredFlit::HEAD )
            output->fatal( CALL_INFO, -1, "Expected head flit\n" );
          inStateVec.at( vn ).at( vc ).inVcState = ROUTING;
          inStateVec.at( vn ).at( vc ).outPort   = topo->routePacket( (uint32_t) flit->req->dest );
        }
      } else if( inStateVec.at( vn ).at( vc ).inVcState == ROUTING ) {
        auto* flit                                   = inStateVec.at( vn ).at( vc ).inBuf.front();
        inStateVec.at( vn ).at( vc ).inVcState       = WAIT_VC;
        rtrSharedObjs->needVcAlloc.at( vn ).at( vc ) = flit;
      } else if( ( inStateVec.at( vn ).at( vc ).inVcState == WAIT_VC ) && ( inStateVec.at( vn ).at( vc ).outVc != UINT32_MAX ) ) {
        auto* flit                                       = inStateVec.at( vn ).at( vc ).inBuf.front();
        inStateVec.at( vn ).at( vc ).inVcState           = IN_ACTIVE;
        rtrSharedObjs->needSwitchAlloc.at( vn ).at( vc ) = flit;
      }
    }
  }

  // Output: send one flit per tick, round-robin across VN/VC
  bool sent = false;
  for( uint32_t i = 0, vn = flit_vn_rr; i < numVns && !sent; i++, vn = ( vn != ( numVns - 1 ) ) ? vn + 1 : 0 ) {
    for( uint32_t j = 0, vc = flit_vc_rr; j < numVcs; j++, vc = ( vc != ( numVcs - 1 ) ) ? vc + 1 : 0 ) {
      auto& out = outStateVec.at( vn ).at( vc );
      // Under flit coalescing, only the HEAD actually starts a new physical-channel
      // transfer (and so is the only one that needs to wait for wire-level space);
      // BODY/TAIL are free no-ops on the wire (see transportSendFlit()) and must not
      // be gated behind a check they don't need — doing so would spuriously stall
      // them behind an unrelated, already-in-flight transfer's credit usage.
      if( !out.outBuf.empty() && out.destCredits > 0 &&
          ( out.outBuf.front()->ftype != MordredFlit::HEAD || transportSpaceToSend( vn, vc ) ) ) {
        auto* flit = out.outBuf.front();
        out.outBuf.pop();
        out.outBufCredits++;
        out.destCredits--;

        if( flit->ftype == MordredFlit::HEAD &&
            flit->getRequest()->getTraceType() == Interfaces::SimpleNetwork::Request::FULL ) {
          output->output(
            "TRACE(%d): %" PRIu64 " ns sending head flit on link %s.%u\n",
            flit->req->getTraceID(), getCurrentSimTimeNano(), getName().c_str(), portId
          );
        }
        if( flit->ftype == MordredFlit::TAIL ) {
          statLinkSentFlitCnt.at( vn ).at( vc )->addData( flit->flit_id + 1 );
          statLinkSentPacketCnt.at( vn ).at( vc )->addData( 1 );
          if( flit->getRequest()->getTraceType() == Interfaces::SimpleNetwork::Request::FULL ) {
            output->output(
              "TRACE(%d): %" PRIu64 " ns sending tail flit on link %s.%u\n",
              flit->req->getTraceID(), getCurrentSimTimeNano(), getName().c_str(), portId
            );
          }
        }

        output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0,
                         "Flit %s going to transport layer\n",
                         flit->pktIdStr().c_str() );
        transportSendFlit( flit, vn );
        sent = true;
        break;
      }
    }
  }
  flit_vc_rr = ( flit_vc_rr + 1 ) % numVcs;
  flit_vn_rr = ( flit_vn_rr + 1 ) % numVns;

  // Return accumulated inBuf credits unconditionally every tick.
  // With transports where spaceToSend always returns true (e.g. UCIe),
  // the original !sent guard suppressed credit return whenever the output
  // was active, causing credit starvation under load.
  returnCredit();
}

void RtrPortControlBase::processIncomingEvent( SST::Event* ev ) {
  auto* bev = static_cast<baseMordredEvent*>( ev );
  if( !bev )
    output->fatal( CALL_INFO, -1, "Null event in processIncomingEvent\n" );

  switch( bev->getType() ) {
  case baseMordredEvent::CREDIT: {
    auto* credit = static_cast<MordredCreditEvent*>( bev );
    validateVnVc( credit->vn, credit->vc );
    outStateVec.at( credit->vn ).at( credit->vc ).destCredits += credit->credits;
    delete bev;
    break;
  }
  case baseMordredEvent::FLIT: {
    auto* flit = static_cast<MordredFlit*>( bev );
    validateVnVc( flit->vn, flit->cur_vc );
    inStateVec.at( flit->vn ).at( flit->cur_vc ).inBuf.push( flit );
    if( flit->ftype == MordredFlit::HEAD &&
        flit->getRequest()->getTraceType() != Interfaces::SimpleNetwork::Request::NONE ) {
      output->output(
        "TRACE(%d): %" PRIu64 " ns received head flit from link %s.%u\n",
        flit->req->getTraceID(), getCurrentSimTimeNano(), getName().c_str(), portId
      );
    }
    if( flit->ftype == MordredFlit::TAIL ) {
      statLinkRecvFlitCnt.at( flit->vn ).at( flit->cur_vc )->addData( flit->flit_id + 1 );
      if( flit->getRequest()->getTraceType() != Interfaces::SimpleNetwork::Request::NONE )
        output->output(
          "TRACE(%d): %" PRIu64 " ns received tail flit from link %s.%u\n",
          flit->req->getTraceID(), getCurrentSimTimeNano(), getName().c_str(), portId
        );
    }
    break;
  }
  default: output->fatal( CALL_INFO, -1, "processIncomingEvent: unknown event type=%d\n", (int) bev->getType() );
  }
}

MordredFlit* RtrPortControlBase::getInBufFlit() {
  validateVnVc( switch_alloc_sendfrom_vn, switch_alloc_sendfrom_vc );
  auto& buf = inStateVec.at( switch_alloc_sendfrom_vn ).at( switch_alloc_sendfrom_vc ).inBuf;
  if( buf.empty() )
    return nullptr;
  auto* flit = buf.front();
  buf.pop();
  if( !flit )
    output->fatal( CALL_INFO, -1, "Invalid flit in getInBufFlit\n" );
  inStateVec.at( switch_alloc_sendfrom_vn ).at( switch_alloc_sendfrom_vc ).retCredits++;
  return flit;
}

void RtrPortControlBase::recvOutBufFlit( MordredFlit* flit ) {
  validateVnVc( switch_alloc_rcvto_vn, switch_alloc_rcvto_vc );
  flit->cur_vc = switch_alloc_rcvto_vc;
  auto& out    = outStateVec.at( switch_alloc_rcvto_vn ).at( switch_alloc_rcvto_vc );
  out.outBuf.push( flit );
  out.outBufCredits--;
  output->verbose( CALL_INFO, MORDRED_VERBOSE_HIGH, 0,
                           "Flit %s pushed to output buffer\n",
                           flit->pktIdStr().c_str() );
}

MordredInitEvent* RtrPortControlBase::getInitEvent( MordredInitEvent::Commands cmd ) {
  auto* ev = static_cast<MordredInitEvent*>( transportRecvUntimedData() );
  if( !ev )
    output->fatal( CALL_INFO, -1, "Unable to recv init event (cmd=%d)\n", (int) cmd );
  if( ev->getType() != baseMordredEvent::INITIALIZATION )
    output->fatal( CALL_INFO, -1, "getInitEvent: unexpected event type=%d\n", (int) ev->getType() );
  if( ev->command != cmd )
    output->fatal( CALL_INFO, -1, "getInitEvent: unexpected command %d, expected %d\n", (int) ev->command, (int) cmd );
  return ev;
}

bool RtrPortControlBase::isWrapAroundOutputPort( uint32_t output_port ) const {
  return topo->isWrapAroundOutput( output_port );
}

void RtrPortControlBase::returnCredit() {
  for( uint32_t i = 0, vn = credit_ret_vn_rr; i < numVns; i++, vn = ( vn != ( numVns - 1 ) ) ? vn + 1 : 0 ) {
    for( uint32_t j = 0, vc = credit_ret_vc_rr; j < numVcs; j++, vc = ( vc != ( numVcs - 1 ) ) ? vc + 1 : 0 ) {
      auto& in = inStateVec.at( vn ).at( vc );
      if( in.retCredits != 0 ) {
        transportSendCredit( new MordredCreditEvent( vn, vc, in.retCredits ), vn );
        in.retCredits = 0;
        break;
      }
    }
  }
  credit_ret_vc_rr = ( credit_ret_vc_rr + 1 ) % numVcs;
  credit_ret_vn_rr = ( credit_ret_vn_rr + 1 ) % numVns;
}

void RtrPortControlBase::serialize_order( SST::Core::Serialization::serializer& ser ) {
  SST_SER( output );
  SST_SER( topo );
  SST_SER( connectionType );
  SST_SER( rtrId );
  SST_SER( portId );
  SST_SER( connectedRtrId );
  SST_SER( connectedPortId );
  SST_SER( numVns );
  SST_SER( numVcs );
  SST_SER( flitSize );
  SST_SER( flit_vn_rr );
  SST_SER( flit_vc_rr );
  SST_SER( credit_ret_vn_rr );
  SST_SER( credit_ret_vc_rr );
  SST_SER( switch_alloc_sendto_port );
  SST_SER( switch_alloc_sendfrom_vn );
  SST_SER( switch_alloc_sendfrom_vc );
  SST_SER( switch_alloc_rcvfrom_port );
  SST_SER( switch_alloc_rcvto_vn );
  SST_SER( switch_alloc_rcvto_vc );
  SST_SER( inBufSize );
  SST_SER( outBufSize );
  SST_SER( initEvents );
  SST_SER( inStateVec );
  SST_SER( outStateVec );
  SST_SER( rtrSharedObjs );
  SST_SER( statLinkRecvFlitCnt );
  SST_SER( statLinkSentFlitCnt );
  SST_SER( statLinkSentPacketCnt );
  SST_SER( statLinkOutputStalledCnt );
}
