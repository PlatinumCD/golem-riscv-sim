//
// RtrPortControlPC.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_RTRPORTCONTROLPC_H
#define MORDRED_RTRPORTCONTROLPC_H

#include "RtrPortControlBase.h"
#include "PhysChannelAPI.h"
#include <deque>

namespace SST::Mordred {

/**
 * RtrPortControlPC — router port controller backed by a PhysChannelAPI subcomponent.
 *
 * The inner PhysChannelAPI can be any matching implementation:
 *   prydwen.genericPhysChannel — for tests
 *   prydwen.uciePhysChannel    — for production UCIe physical links
 */
class RtrPortControlPC : public RtrPortControlBase {
public:
  SST_ELI_REGISTER_SUBCOMPONENT(
    RtrPortControlPC,
    "mordred",
    "rtrPortControlPC",
    SST_ELI_ELEMENT_VERSION( 0, 1, 0 ),
    "SimpleNetwork-backed port control for the Mordred router",
    SST::Mordred::RtrPortControlAPI
  )

  SST_ELI_DOCUMENT_PARAMS()

  SST_ELI_DOCUMENT_PORTS()

  SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
    { "port_iface", "PhysChannelAPI subcomponent that manages the physical port link",
      "SST::Prydwen::PhysChannelAPI" }
  )

  SST_ELI_DOCUMENT_STATISTICS(
    { "recv_flit_cnt", "Number of flits received on the link", "unitless", 3 },
    { "sent_flit_cnt", "Number of flits sent on the link", "unitless", 3 },
    { "sent_packet_cnt", "Number of packets sent on the link", "unitless", 3 },
    { "output_stalls", "Number of cycles stalled on output", "unitless", 3 }
  )

  RtrPortControlPC(
    ComponentId_t       id,
    Params&             params,
    TopologyAPI*        topology,
    RtrOwnedSharedObjs* rtr_shared_objs,
    uint32_t            rtr_num,
    uint32_t            port_num
  );

  ~RtrPortControlPC() override = default;

  // Inner-channel receive notification callback
  bool onReceive( int sn_vn );

  // Override ClockTick to drip one pending flit per tick. The HEAD of a packet
  // triggers a coalesced UCIe transfer (see transportSendFlit() below); once all
  // physical fragments are reassembled on the far side, the FlitFactory
  // synthesizes every Mordred flit of that packet at once ("bulk delivery" —
  // see UCIePhysChannel's class doc). Without this queue, all of those flits
  // would land in the same tick and be processed immediately, saturating inBuf
  // and violating the router's one-flit-per-cycle input assumption; this drains
  // them at native link-equivalent timing instead.
  void ClockTick( Cycle_t cycle ) override;


  RtrPortControlPC() : RtrPortControlBase() {}

  void serialize_order( SST::Core::Serialization::serializer& ser ) override {
    RtrPortControlBase::serialize_order( ser );
    SST_SER( physChannel );
    SST_SER( pendingFlits_ );
  }

  ImplementSerializable( SST::Mordred::RtrPortControlPC );

protected:
  // Maps a Mordred (vn, vc) pair onto a single UCIe ext_vn. UCIePhysChannel's
  // wire-level VN space is otherwise addressed by vn alone (transportSendFlit()
  // et al. only carry vn), so every VC sharing a VN would share one physical
  // send queue and one credit pool — harmless when each flit was its own atomic
  // wire message, but a deadlock hazard once whole packets are coalesced into
  // one uninterruptible burst (a VC-dependent deadlock-avoidance scheme, e.g. an
  // escape VC on a wraparound torus, needs its VCs to have independent forward
  // progress on the wire). Widening the address space here — and symmetrically
  // in MordredNicPC, see its transportValidateVcWidth() — restores that
  // independence: each (vn,vc) pair gets its own UCIePhysChannel queue/credits.
  uint32_t extVn( uint32_t vn, uint32_t vc ) const { return vn * numVcs + vc; }

  void transportSendFlit( MordredFlit* flit, uint32_t vn ) override {
    // Coalesce a whole packet into one UCIe wire transfer instead of sending
    // every Mordred flit as its own atomic physical message. The HEAD carries
    // the true packet size (all flits of a packet share the same req*, so
    // size_in_bits is available from any of them); UCIePhysChannel::send()
    // fragments it into ceil(bytes/flit_payload_bytes) physical FLITs and
    // registers m_count = ceil(packet_size_bits/network_flit_size) for the
    // FlitFactory (below, in transportSetup()) to resynthesize on the far side.
    // BODY/TAIL are then free no-ops on the wire — desc.is_first=false makes
    // UCIePhysChannel::send() discard them immediately (see its doc comment) —
    // since the far side already reconstructs the full flit sequence from the
    // HEAD's fragments.
    Prydwen::PhysChannelFlitDescriptor desc;
    desc.flit      = flit;
    desc.req       = flit->req;
    desc.packet_id = flit->packet_id;
    if( flit->ftype == MordredFlit::HEAD ) {
      desc.packet_size_bits = static_cast<uint32_t>( flit->req->size_in_bits );
      desc.is_first         = true;
      desc.is_last          = false;
    } else {
      desc.packet_size_bits = 0;
      desc.is_first         = false;
      desc.is_last          = ( flit->ftype == MordredFlit::TAIL );
    }
    physChannel->send( desc, static_cast<int>( extVn( vn, flit->cur_vc ) ) );
  }
  void transportSendCredit( MordredCreditEvent* credit, uint32_t vn ) override {
    Prydwen::PhysChannelFlitDescriptor desc;
    desc.flit             = credit;
    desc.req              = nullptr;
    desc.packet_id        = 0;
    desc.packet_size_bits = 0;
    desc.is_first         = true;
    desc.is_last          = true;
    physChannel->send( desc, static_cast<int>( extVn( vn, credit->vc ) ) );
  }
  void        transportSendUntimedData( SST::Event* ev ) override { physChannel->sendUntimedData( ev ); }
  SST::Event* transportRecvUntimedData() override { return physChannel->recvUntimedData(); }

  bool transportSpaceToSend( uint32_t vn, uint32_t vc ) override {
    return physChannel->spaceToSend( static_cast<int>( extVn( vn, vc ) ), static_cast<int>( flitSize ) );
  }

  void transportInit( uint32_t phase ) override { physChannel->init( phase ); }
  void transportSetup() override {
    // Inform the physical channel of Mordred's logical flit width so it can
    // compute m_count = ceil(packet_size_bits / network_flit_size) on send.
    if( flitSize > 0 )
      physChannel->setNetworkFlitSize( flitSize );

    // Register the factory that synthesizes M Mordred flits from the deserialized
    // HEAD flit on the receive side.
    physChannel->setFlitFactory(
      [this]( SST::Event* head_event, uint32_t m_count, int /*ext_vn*/ ) -> std::vector<SST::Event*> {
        auto* hf = dynamic_cast<MordredFlit*>( head_event );
        if( !hf )
          output->fatal( CALL_INFO, -1, "RtrPortControlPC FlitFactory: head_event is not a MordredFlit\n" );
        auto*                    req = hf->req;
        std::vector<SST::Event*> result;
        result.reserve( m_count );
        result.push_back( hf );  // HEAD — already constructed correctly
        for( uint32_t i = 1; i < m_count - 1; i++ ) {
          auto* body              = new MordredFlit( req, MordredFlit::BODY, hf->packet_id, i );
          body->cur_vc            = hf->cur_vc;            // preserve VC assignment
          body->pkt_created_cycle = hf->pkt_created_cycle;
          body->head_inject_cycle = hf->head_inject_cycle;
          result.push_back( body );
        }
        auto* tail              = new MordredFlit( req, MordredFlit::TAIL, hf->packet_id, m_count - 1 );
        tail->cur_vc            = hf->cur_vc;              // preserve VC assignment
        tail->pkt_created_cycle = hf->pkt_created_cycle;
        tail->head_inject_cycle = hf->head_inject_cycle;
        result.push_back( tail );
        return result;
      }
    );

    physChannel->setup();
  }
  void transportComplete( uint32_t phase ) override { physChannel->complete( phase ); }

private:
  Prydwen::PhysChannelAPI* physChannel{};

  // Pending flit queue: a coalesced packet's Mordred flits are all synthesized by
  // the FlitFactory at once, the instant the last physical fragment is reassembled
  // ("bulk delivery" — see UCIePhysChannel's class doc), but the router's input
  // state machine expects flits to arrive one per clock tick (matching native
  // link timing). We queue incoming events here and inject one per ClockTick to
  // match native link behaviour and avoid inBuf saturation leading to deadlock.
  // Credit events bypass this queue and are processed immediately.
  //
  // UCIe's own wire-level credit return is scoped only to its own adapter
  // buffer occupancy — see UCIePhysChannel.h's "Credit scope" doc — so it
  // cannot see, and does not protect, this queue. That protection has to
  // come from a separate credit loop with visibility into this resource,
  // which is exactly what output_buf_size/destCredits below provides.
  //
  // IMPORTANT — this queue adds real, packet-size-dependent latency to every
  // Mordred-level credit round trip, and `output_buf_size` must be sized with
  // that in mind:
  //
  // Before coalescing, each Mordred flit crossed the wire as its own message, so
  // arrivals were already paced by wire transit — this queue was rarely more
  // than empty. Under coalescing, all M flits of one packet are bulk-delivered
  // here simultaneously the moment the (now single, efficient) physical transfer
  // completes, but they still drain at one per router cycle: flit k of that
  // packet now waits k-1 extra cycles just to be dripped, before the input state
  // machine can even see it, let alone service it and return the credit that
  // lets the sender dequeue its next flit. So coalescing trades wire efficiency
  // for O(M) extra cycles of credit-return latency at every hop.
  //
  // A sender with only enough `output_buf_size` for the old per-flit-atomic
  // model (as little as one credit — sufficient before, because wire transit was
  // the rate limiter and always outpaced this queue) has no slack to absorb that
  // added latency and can stall completely under load; this was observed as a
  // real, reproducible deadlock on a 5x5 torus (`torus5x5_2vc_uciePhysChannel`)
  // that only cleared once `output_buf_size` was given real headroom (confirmed
  // by bisection: bumping the *Mordred* buffer alone fixed it; bumping the UCIe
  // wire-level `credits_per_vn` alone did not — the bottleneck is this queue,
  // not physical-channel credit availability). Give `output_buf_size` — and
  // therefore destCredits — enough headroom to cover several packets' worth of
  // Mordred flits in flight, not just one, whenever this port's uciePhysChannel
  // has num_vns_per_stack (or num_vcs) that make coalescing this port's traffic
  // likely.
  std::deque<SST::Event*> pendingFlits_;
};

}  // namespace SST::Mordred

#endif  // MORDRED_RTRPORTCONTROLPC_H
