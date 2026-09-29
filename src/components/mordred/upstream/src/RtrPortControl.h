//
// RtrPortControl.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_RTRPORTCONTROL_H
#define MORDRED_RTRPORTCONTROL_H

#include "RtrPortControlBase.h"

namespace SST::Mordred {

/**
 * RtrPortControl — router port controller backed by a direct SST::Link.
 */
class RtrPortControl : public RtrPortControlBase {
public:
  SST_ELI_REGISTER_SUBCOMPONENT(
    RtrPortControl,
    "mordred",
    "rtrPortControl",
    SST_ELI_ELEMENT_VERSION( 0, 1, 0 ),
    "Manage a port on the Mordred router",
    SST::Mordred::RtrPortControlAPI
  )

  SST_ELI_DOCUMENT_PARAMS()

  SST_ELI_DOCUMENT_PORTS()

  SST_ELI_DOCUMENT_STATISTICS(
    { "recv_flit_cnt", "Number of flits received on the link", "unitless", 3 },
    { "sent_flit_cnt", "Number of flits sent on the link", "unitless", 3 },
    { "sent_packet_cnt", "Number of packets sent on the link", "unitless", 3 },
    { "output_stalls", "Number of cycles stalled on sending output", "unitless", 3 }
  )

  RtrPortControl(
    ComponentId_t       id,
    Params&             params,
    TopologyAPI*        topology,
    RtrOwnedSharedObjs* rtr_shared_objs,
    uint32_t            rtr_num,
    uint32_t            port_num
  );

  ~RtrPortControl() override = default;

  RtrPortControl() : RtrPortControlBase() {}

  void serialize_order( SST::Core::Serialization::serializer& ser ) override {
    RtrPortControlBase::serialize_order( ser );
    SST_SER( link );
    SST_SER( param_link_bw );
    SST_SER( param_flit_size );
  }

  ImplementSerializable( SST::Mordred::RtrPortControl );

protected:
  void        transportSendFlit( MordredFlit* flit, uint32_t vn ) override { link->send( flit ); }
  void        transportSendCredit( MordredCreditEvent* credit, uint32_t vn ) override { link->send( credit ); }
  void        transportSendUntimedData( SST::Event* ev ) override { link->sendUntimedData( ev ); }
  SST::Event* transportRecvUntimedData() override { return link->recvUntimedData(); }

private:
  void inHandler( SST::Event* ev ) { processIncomingEvent( ev ); }

  Link*       link{};
  UnitAlgebra param_link_bw;
  UnitAlgebra param_flit_size;
};

}  // namespace SST::Mordred

#endif  // MORDRED_RTRPORTCONTROL_H
