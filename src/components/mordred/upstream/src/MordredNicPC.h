//
// MordredNicPC.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_MORDREDNICPC_H
#define MORDRED_MORDREDNICPC_H

#include <cinttypes>

#include "MordredNicBase.h"
#include "PhysChannelAPI.h"

namespace SST::Mordred {

/**
 * Note (tdysart, 9-jul-2026):
 * Most of this file (along with its .cc as well as RtrPortControlPC.{h,cc} was developed by
 * Claude with guidance from me.  One of the things that has to happen when using the physical channel
 * is that we have to convert mordred network flits into physical channel flits (e.g, ucie),
 * especially if they are different sizes.  This is required since the mordred_router expects
 * to handle mordred flits and the ucie flits may or may not match that size).
 *
 * Work over the last couple of days has focused on getting the transmit side to properly coalesce
 * smaller mordred flits into the larger ucie flits (the receive side was pretty well complete
 * previously).  Claude claims to have this issue resolved.
 *
 * Overall, this code should still be reviewed by a set of human eyes.
 */

/**
 * MordredNicPC — SST SimpleNetwork NIC backed by a PhysChannelAPI subcomponent.
 *
 * The inner PhysChannelAPI can be any matching implementation:
 *   prydwen.genericPhysChannel — for tests (generic raw-link wrapper)
 *   prydwen.uciePhysChannel    — for production UCIe physical links
 *
 * Mordred's own credit protocol is unchanged.  The inner channel's flow
 * control and Mordred's MordredCreditEvent are complementary: the inner
 * channel protects the local TX buffer; Mordred credits protect the remote
 * router's input buffer.
 */


class MordredNicPC : public MordredNicBase {

public:
  SST_ELI_REGISTER_SUBCOMPONENT(
    MordredNicPC,
    "mordred",
    "mordredNicPC",
    SST_ELI_ELEMENT_VERSION( 0, 1, 0 ),
    "SimpleNetwork-backed Mordred NIC endpoint; uses inner PhysChannelAPI for physical link",
    SST::Interfaces::SimpleNetwork
  )

  SST_ELI_DOCUMENT_PARAMS(
    { "verbose", "Sets the output verbosity", "5" },
    { "clock", "Clock frequency of this interface", "1GHz" },
    { "input_buf_size", "Size of input buffers specified in b or B (can include SI prefix).", "1kiB" },
    { "output_buf_size", "Size of output buffers specified in b or B (can include SI prefix).", "1kiB" },
    { "num_vcs",
      "Number of VCs used by the connected router network. This endpoint has no VC of its "
      "own (VC assignment happens inside the router fabric) — it exists only so this "
      "endpoint's UCIe address-space widening (see RtrPortControlPC::extVn()) matches its "
      "connected router's local port symmetrically. Must equal the router network's actual "
      "num_vcs, or the link will silently fail to widen (default \"1\" == no widening, "
      "matches today's behavior).",
      "1" }
  )

  SST_ELI_DOCUMENT_PORTS( {
    "port",
    "Port that connects to a Mordred router (via inner PhysChannelAPI adapter).",
    { "untimedMordredEvent", "basicMordredEvent" }
  } )

  SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
    { "port_iface", "PhysChannelAPI subcomponent that manages the physical link to the router",
      "SST::Prydwen::PhysChannelAPI" }
  )

  SST_ELI_DOCUMENT_STATISTICS(
    { "packets_recv", "Number of packets received", "unitless", 3 },
    { "average_noc_latency", "Average latency (in clocks) of each packet", "unitless", 3 },
    { "average_packet_size", "Average packet size in number of flits", "unitless", 3 }
  )

  MordredNicPC( ComponentId_t cid, Params& params, int vns );
  ~MordredNicPC() override = default;

  // Inner-channel receive notification callback
  bool onReceive( int sn_vn );

  MordredNicPC() : MordredNicBase() {}

  void serialize_order( SST::Core::Serialization::serializer& ser ) override {
    MordredNicBase::serialize_order( ser );
    SST_SER( physChannel );
  }

  ImplementSerializable( SST::Mordred::MordredNicPC );

protected:
  // Mirrors RtrPortControlPC::extVn() — an endpoint never has a real VC of its
  // own (vc is always 0 here), but must widen its own UCIe address space by the
  // same factor as its connected router's local port, or the two ends negotiate
  // down to the unwidened count during UCIe's INIT/AGREE handshake (silently
  // undoing the fix on this link — see transportValidateVcWidth() below).
  uint32_t extVn( uint32_t vn ) const { return vn * numVcsForAddressing_; }

  void transportSendFlit( MordredFlit* flit, uint32_t vn ) override {
    // Same whole-packet coalescing as RtrPortControlPC — see that file for the
    // full rationale. HEAD carries the true packet size and triggers UCIe
    // fragmentation + FlitFactory registration; BODY/TAIL are free no-ops.
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
    physChannel->send( desc, static_cast<int>( extVn( vn ) ) );
  }
  void transportSendCredit( MordredCreditEvent* credit, uint32_t vn ) override {
    Prydwen::PhysChannelFlitDescriptor desc;
    desc.flit             = credit;
    desc.req              = nullptr;
    desc.packet_id        = 0;
    desc.packet_size_bits = 0;
    desc.is_first         = true;
    desc.is_last          = true;
    physChannel->send( desc, static_cast<int>( extVn( vn ) ) );
  }
  void        transportSendUntimedData( SST::Event* ev ) override { physChannel->sendUntimedData( ev ); }
  SST::Event* transportRecvUntimedData() override { return physChannel->recvUntimedData(); }

  // Fail loudly, not silently, on a misconfigured num_vcs (see the param doc above).
  void transportValidateVcWidth( uint32_t router_num_vcs ) override {
    if( router_num_vcs != numVcsForAddressing_ )
      output->fatal(
        CALL_INFO, -1,
        "MordredNicPC: connected router uses num_vcs=%" PRIu32 " but this endpoint was "
        "configured with num_vcs=%" PRIu32 " — they must match so both ends of the link "
        "widen their UCIe address space identically. Set this endpoint's 'num_vcs' param "
        "to the router network's actual num_vcs.\n",
        router_num_vcs, numVcsForAddressing_
      );
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
          output->fatal( CALL_INFO, -1, "MordredNicPC FlitFactory: head_event is not a MordredFlit\n" );
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
  void transportFinish() override { physChannel->finish(); }

private:
  Prydwen::PhysChannelAPI* physChannel{};
  uint32_t                 numVcsForAddressing_{ 1 };
};

}  // namespace SST::Mordred

#endif  // MORDRED_MORDREDNICPC_H
