//
// MordredNicBase.cc
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#include <cmath>

#include "MordredNicBase.h"

using namespace SST::Mordred;

MordredNicBase::MordredNicBase( ComponentId_t cid, Params& params, int vns, const char* class_name )
    : SimpleNetwork( cid ) {
  const auto verbosity = params.find<uint32_t>( "verbose", MORDRED_VERBOSE_MED );
  output = new SST::Output( std::string( class_name ) + "[" + getName() + ":@p:@t]: ", verbosity, 0, Output::STDOUT );

  inbufSize = params.find<UnitAlgebra>( "input_buf_size", "1kiB" );
  if( !inbufSize.hasUnits( "b" ) && !inbufSize.hasUnits( "B" ) )
    output->fatal( CALL_INFO, -1, "input_buf_size must be in bits or bytes: %s\n", inbufSize.toStringBestSI().c_str() );
  if( inbufSize.hasUnits( "B" ) )
    inbufSize *= UnitAlgebra( "8b/B" );

  outbufSize = params.find<UnitAlgebra>( "output_buf_size", "1kiB" );
  if( !outbufSize.hasUnits( "b" ) && !outbufSize.hasUnits( "B" ) )
    output->fatal( CALL_INFO, -1, "output_buf_size must be in bits or bytes: %s\n", outbufSize.toStringBestSI().c_str() );
  if( outbufSize.hasUnits( "B" ) )
    outbufSize *= UnitAlgebra( "8b/B" );

  const auto clock_freq = params.find<std::string>( "clock", "1GHz" );
  registerClock( clock_freq, new Clock::Handler2<MordredNicBase, &MordredNicBase::clockTick>( this ) );

  UnitAlgebra ua_cf( clock_freq );
  bw = ua_cf * UnitAlgebra( "1b" );

  statPacketsRecv       = registerStatistic<uint64_t>( "packets_recv" );
  statAvgNocLatency     = registerStatistic<double>( "average_noc_latency" );
  statAvgFlitsPerPacket = registerStatistic<double>( "average_packet_size" );
}

void MordredNicBase::init( uint32_t phase ) {
  transportInit( phase );

  output->verbose( CALL_INFO, MORDRED_VERBOSE_MED, 0, "START init phase=%" PRIu32 "\n", phase );

  switch( phase ) {
  case 0: {
    auto* ev    = new MordredInitEvent();
    ev->command = MordredInitEvent::REPORT_ENDPOINT;
    ev->value   = UINT32_MAX;
    transportSendUntimedData( ev );
    break;
  }

  case 1: {
    auto* init_ev = getInitEvent( MordredInitEvent::REPORT_ROUTER );
    delete init_ev;

    init_ev = getInitEvent( MordredInitEvent::ROUTER_ID );
    rtrId   = init_ev->value;
    delete init_ev;

    init_ev = getInitEvent( MordredInitEvent::PORT_NUM );
    rtrPort = init_ev->value;
    delete init_ev;

    output->verbose(
      CALL_INFO, MORDRED_VERBOSE_MED, 0,
      "Phase 1: [Rtr.Port]=[%" PRIu32 ".%" PRIu32 "]\n", rtrId, rtrPort
    );
    break;
  }

  case 2: {
    auto* init_ev = getInitEvent( MordredInitEvent::NUM_VNS );
    numVns        = init_ev->value;
    delete init_ev;

    init_ev = getInitEvent( MordredInitEvent::NUM_VCS );
    numVcs  = init_ev->value;
    delete init_ev;
    transportValidateVcWidth( numVcs );

    init_ev  = getInitEvent( MordredInitEvent::FLIT_WIDTH );
    flitSize = init_ev->value;
    bw *= UnitAlgebra( std::to_string( flitSize ) );
    delete init_ev;

    output->verbose(
      CALL_INFO, MORDRED_VERBOSE_MED, 0,
      "Phase 2: numVNs=%" PRIu32 ", numVCs=%" PRIu32 ", flit_width=%" PRIu32 "\n",
      numVns, numVcs, flitSize
    );
    resizeVectors();
    break;
  }

  case 3: {
    auto* init_ev = getInitEvent( MordredInitEvent::ENDPOINT_ID );
    netID         = static_cast<nid_t>( init_ev->value );
    initialized   = true;
    output->verbose( CALL_INFO, MORDRED_VERBOSE_MED, 0,
                     "Phase 3: endpoint id = %" PRId64 "\n", netID );
    delete init_ev;

    const auto credits = static_cast<int32_t>( inbufSize.getRoundedValue() / flitSize );
    if( credits == 0 )
      output->fatal(
        CALL_INFO, -1, "flit_size=%" PRIu32 "b > input_buf_size=%" PRId64 "b\n",
        flitSize, inbufSize.getRoundedValue()
      );
    for( uint32_t i = 0; i < numVns; i++ )
      transportSendUntimedData( new MordredCreditEvent( i, 0, credits ) );
    break;
  }

  default: {
    SST::Event* ev = nullptr;
    while( ( ev = transportRecvUntimedData() ) != nullptr ) {
      auto* base_ev = static_cast<baseMordredEvent*>( ev );
      if( base_ev->getType() == baseMordredEvent::CREDIT ) {
        auto* credit_ev = static_cast<MordredCreditEvent*>( ev );
        rtrCredits.at( credit_ev->vn ) += credit_ev->credits;
        output->verbose(
          CALL_INFO, MORDRED_VERBOSE_MED, 0,
          "Received credit vn=%" PRIu32 ", credits=%" PRId32 "; cur=%" PRId32 "\n",
          credit_ev->vn, credit_ev->credits, rtrCredits.at( credit_ev->vn )
        );
        delete ev;
      } else if( base_ev->getType() == baseMordredEvent::PACKET ) {
        initEvents.push( static_cast<MordredInitEvent*>( ev ) );
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

void MordredNicBase::setup() {
  transportSetup();
}

void MordredNicBase::complete( uint32_t phase ) {
  transportComplete( phase );

  SST::Event* ev = nullptr;
  while( ( ev = transportRecvUntimedData() ) != nullptr ) {
    auto* base_ev = static_cast<baseMordredEvent*>( ev );
    if( base_ev->getType() == baseMordredEvent::CREDIT ) {
      auto* credit_ev = static_cast<MordredCreditEvent*>( ev );
      rtrCredits.at( credit_ev->vn ) += credit_ev->credits;
      delete ev;
    } else if( base_ev->getType() == baseMordredEvent::PACKET ) {
      initEvents.push( static_cast<MordredInitEvent*>( ev ) );
    } else {
      output->verbose( CALL_INFO, MORDRED_VERBOSE_MED, 0,
                       "Unexpected event type=%d in complete\n",
                       (int) base_ev->getType() );
      delete ev;
    }
  }
}

void MordredNicBase::finish() {
  const double avg_ticks = totalPackets > 0 ? (double) totalNocLatency / (double) totalPackets : -1.0;
  statPacketsRecv->addData( totalPackets );
  statAvgNocLatency->addData( avg_ticks );
  const double avg_flits = totalPackets > 0 ? (double) totalNumFlits / (double) totalPackets : 0.0;
  statAvgFlitsPerPacket->addData( avg_flits );
}

void MordredNicBase::sendUntimedData( Request* req ) {
  transportSendUntimedData( new MordredInitEvent( req ) );
}

SST::Interfaces::SimpleNetwork::Request* MordredNicBase::recvUntimedData() {
  if( initEvents.empty() )
    return nullptr;
  auto* ev  = initEvents.front();
  initEvents.pop();
  auto* req = ev->req;
  delete ev;
  return req;
}

bool MordredNicBase::send( Request* req, int vn ) {
  const auto u_vn = static_cast<uint32_t>( vn );
  if( numVns <= u_vn )
    output->fatal( CALL_INFO, -1, "send: vn=%d is invalid (numVns=%" PRIu32 ")\n", vn, numVns );

  const int32_t num_flits = calcNumFlits( req->size_in_bits );
  if( outbufCredits.at( u_vn ) < num_flits )
    return false;
  outbufCredits.at( u_vn ) -= num_flits;

#if 0
  req->setTraceType( Request::FULL );
  int trace_id = (int)((netID * 1000) + (nid_t)packetId);
  req->setTraceID( trace_id );
#endif

  const auto u_num_flits = static_cast<uint32_t>( num_flits );
  // req* is borrowed by all flits — it is not owned by any flit. Ownership
  // transfers to the receiver: on TAIL arrival processIncomingEvent() moves req
  // into inBuf, and recv() returns it to the caller who is responsible for deletion.
  const uint64_t created_cycle = getCurrentSimCycle();
  auto*          head          = new MordredFlit( req, MordredFlit::HEAD, packetId, 0 );
  head->pkt_created_cycle      = created_cycle;
  outBuf.at( u_vn ).push( head );
  for( uint32_t i = 1; i < u_num_flits - 1; i++ )
    outBuf.at( u_vn ).push( new MordredFlit( req, MordredFlit::BODY, packetId, i ) );
  auto* tail              = new MordredFlit( req, MordredFlit::TAIL, packetId++, u_num_flits - 1 );
  tail->pkt_created_cycle = created_cycle;
  outBuf.at( u_vn ).push( tail );

  if( req->getTraceType() != Request::NONE )
    output->output(
      "TRACE(%d): %" PRIu64 " ns send called on %s\n",
      req->getTraceID(), getCurrentSimTimeNano(), getName().c_str()
    );
  return true;
}

SST::Interfaces::SimpleNetwork::Request* MordredNicBase::recv( int vn ) {
  const auto u_vn = static_cast<uint32_t>( vn );
  if( numVns <= u_vn )
    output->fatal( CALL_INFO, -1, "recv: vn=%d is invalid\n", vn );
  if( inBuf.at( u_vn ).empty() )
    return nullptr;

  auto* req = inBuf.at( u_vn ).front();
  inBuf.at( u_vn ).pop();

  if( req->getTraceType() != Request::NONE )
    output->output(
      "TRACE(%d): %" PRIu64 " ns recv called on %s\n",
      req->getTraceID(), getCurrentSimTimeNano(), getName().c_str()
    );

  if( req->dest != netID )
    output->fatal(
      CALL_INFO, -1, "Packet with dest=%" PRId64 " received by netID=%" PRId64 "\n", req->dest, netID
    );
  return req;
}

bool MordredNicBase::spaceToSend( int vn, int num_bits ) {
  const auto u_vn = static_cast<uint32_t>( vn );
  if( numVns <= u_vn )
    output->fatal( CALL_INFO, -1, "spaceToSend: vn=%d is invalid\n", vn );
  return outbufCredits.at( u_vn ) >= calcNumFlits( static_cast<uint32_t>( num_bits ) );
}

bool MordredNicBase::requestToReceive( int vn ) {
  const auto u_vn = static_cast<uint32_t>( vn );
  if( numVns <= u_vn )
    output->fatal( CALL_INFO, -1, "requestToReceive: vn=%d is invalid\n", vn );
  return !inBuf.at( u_vn ).empty();
}

bool MordredNicBase::clockTick( Cycle_t cycle ) {
  bool sent = false;

  for( uint32_t vn = 0; vn < numVns; vn++ ) {
    if( !outBuf.at( vn ).empty() && rtrCredits.at( vn ) > 0 ) {
      auto* flit = outBuf.at( vn ).front();
      outBuf.at( vn ).pop();

      if( flit->ftype == MordredFlit::HEAD ) {
        headInjectCycle         = getCurrentSimCycle();
        // Also stamp the HEAD object itself (not just the local scalar): when a
        // physical channel coalesces a whole packet into one wire transfer, the
        // receive-side FlitFactory reuses this exact HEAD object and derives
        // synthesized BODY/TAIL timestamps from its fields (see
        // RtrPortControlPC::transportSetup() / MordredNicPC::transportSetup()).
        flit->head_inject_cycle = headInjectCycle;
        if( flit->req->getTraceType() == Request::FULL )
          output->output(
            "TRACE(%d): %" PRIu64 " ns put head flit on link %s\n",
            flit->req->getTraceID(), getCurrentSimTimeNano(), getName().c_str()
          );
      } else if( flit->ftype == MordredFlit::TAIL ) {
        flit->head_inject_cycle = headInjectCycle;
        headInjectCycle         = UINT64_MAX;
        if( flit->req->getTraceType() == Request::FULL )
          output->output(
            "TRACE(%d): %" PRIu64 " ns put tail flit on link %s\n",
            flit->req->getTraceID(), getCurrentSimTimeNano(), getName().c_str()
          );
        if( sendFunctor ) {
          const bool keep = ( *sendFunctor )( static_cast<int>( vn ) );
          if( !keep )
            sendFunctor = nullptr;
        }
      }

      // Capture debug info before transportSendFlit() — the physical layer
      // takes ownership of the flit and may delete it immediately (e.g. UCIe
      // serializes and discards BODY/TAIL flits on-the-spot).
      std::string flit_dbg = flit->pktIdStr();

      transportSendFlit( flit, vn );
      sent = true;
      rtrCredits.at( vn )--;
      outbufCredits.at( vn )++;

      output->verbose(
        CALL_INFO, MORDRED_VERBOSE_HIGH, 0, "Sent flit %s at cycle=%" PRIu64 "; rtrCredits=%" PRId32 "\n",
        flit_dbg.c_str(), cycle, rtrCredits.at( vn )
      );
      output->flush();
    }
  }

  if( sent )
    return false;

  for( uint32_t vn = 0; vn < numVns; vn++ ) {
    if( inReturnCredits.at( vn ) > 0 ) {
      transportSendCredit( new MordredCreditEvent( vn, 0, inReturnCredits.at( vn ) ), vn );
      inReturnCredits.at( vn ) = 0;
      break;
    }
  }
  return false;
}

void MordredNicBase::processIncomingEvent( SST::Event* ev ) {
  auto* bev = static_cast<baseMordredEvent*>( ev );
  switch( bev->getType() ) {

  case baseMordredEvent::CREDIT: {
    auto* credit = static_cast<MordredCreditEvent*>( bev );
    if( credit->vn >= numVns )
      output->fatal( CALL_INFO, -1, "Unsupported vn=%u\n", credit->vn );
    rtrCredits.at( credit->vn ) += credit->credits;
    output->verbose(
      CALL_INFO, MORDRED_VERBOSE_HIGH, 0,
      "Received %" PRId32 " credits for vn=%" PRIu32 ", cur=%" PRId32 "\n",
      credit->credits, credit->vn, rtrCredits.at( credit->vn )
    );
    delete bev;
    break;
  }

  case baseMordredEvent::FLIT: {
    auto* flit = static_cast<MordredFlit*>( bev );
    auto* req  = flit->getRequest();
    if( !flit || !req )
      output->fatal( CALL_INFO, -1, "Null flit or request\n" );

    if( flit->ftype == MordredFlit::HEAD && req->getTraceType() == Request::FULL )
      output->output(
        "TRACE(%d): %" PRIu64 " ns received head flit from link %s\n",
        flit->req->getTraceID(), getCurrentSimTimeNano(), getName().c_str()
      );

    if( flit->ftype == MordredFlit::TAIL ) {
      if( flit->vn >= numVns )
        output->fatal( CALL_INFO, -1, "Unsupported vn=%u\n", flit->vn );
      if( req->getTraceType() == Request::FULL )
        output->output(
          "TRACE(%d): %" PRIu64 " ns received tail flit from link %s\n",
          flit->req->getTraceID(), getCurrentSimTimeNano(), getName().c_str()
        );
      inBuf.at( flit->vn ).push( req );  // ownership of req transfers to inBuf; caller of recv() must delete
      const uint64_t noc_latency = getCurrentSimCycle() - flit->head_inject_cycle;
      totalNocLatency += static_cast<uint64_t>( std::ceil( noc_latency / 1000.0 ) );
      totalPackets++;
      totalNumFlits += ( flit->flit_id + 1 );
      if( receiveFunctor ) {
        const bool keep = ( *receiveFunctor )( static_cast<int>( flit->vn ) );
        if( !keep )
          receiveFunctor = nullptr;
      }
    }

    inReturnCredits.at( flit->vn )++;
    delete flit;
    break;
  }

  default:
    output->fatal( CALL_INFO, -1, "processIncomingEvent: unknown event type=%d\n", (int) bev->getType() );
  }
}

void MordredNicBase::resizeVectors() {
  if( numVns == 0 )
    output->fatal( CALL_INFO, -1, "MordredNicBase::resizeVectors: numVns is 0\n" );

  inBuf.resize( numVns );
  outBuf.resize( numVns );

  const auto credits = static_cast<int32_t>( outbufSize.getRoundedValue() / flitSize );
  rtrCredits.resize( numVns, 0 );
  outbufCredits.resize( numVns, credits );
  inReturnCredits.resize( numVns, 0 );
}

MordredInitEvent* MordredNicBase::getInitEvent( MordredInitEvent::Commands cmd ) {
  auto* ev = static_cast<MordredInitEvent*>( transportRecvUntimedData() );
  if( !ev )
    output->fatal( CALL_INFO, -1, "Unable to recv init event (cmd=%d)\n", (int) cmd );
  if( ev->getType() != baseMordredEvent::INITIALIZATION )
    output->fatal( CALL_INFO, -1, "getInitEvent: unexpected event type=%d\n", (int) ev->getType() );
  if( ev->command != cmd )
    output->fatal(
      CALL_INFO, -1, "getInitEvent: unexpected command %d, expected %d\n", (int) ev->command, (int) cmd
    );
  return ev;
}

int32_t MordredNicBase::calcNumFlits( uint32_t num_bits ) {
  auto num_flits = static_cast<int32_t>( std::ceil( (float) num_bits / (float) flitSize ) );
  if( num_flits < 2 )
    num_flits = 2;
  return num_flits;
}

void MordredNicBase::serialize_order( SST::Core::Serialization::serializer& ser ) {
  SST_SER( output );
  SST_SER( netID );
  SST_SER( rtrId );
  SST_SER( rtrPort );
  SST_SER( initialized );
  SST_SER( numVns );
  SST_SER( numVcs );
  SST_SER( flitSize );
  SST_SER( packetId );
  SST_SER( headInjectCycle );
  SST_SER( bw );
  SST_SER( inbufSize );
  SST_SER( outbufSize );
  SST_SER( initEvents );
  SST_SER( inBuf );
  SST_SER( outBuf );
  SST_SER( rtrCredits );
  SST_SER( outbufCredits );
  SST_SER( totalNocLatency );
  SST_SER( totalPackets );
  SST_SER( totalNumFlits );
  SST_SER( statPacketsRecv );
  SST_SER( statAvgNocLatency );
  SST_SER( statAvgFlitsPerPacket );
}
