//
// MordredNIC.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_MORDREDNIC_H
#define MORDRED_MORDREDNIC_H

#include "MordredNicBase.h"

namespace SST::Mordred {

/**
 * MordredNIC — SST SimpleNetwork NIC backed by a direct SST::Link to the router.
 */
class MordredNIC : public MordredNicBase {

public:
  SST_ELI_REGISTER_SUBCOMPONENT(
    MordredNIC,
    "mordred",
    "mordredNIC",
    SST_ELI_ELEMENT_VERSION( 0, 1, 0 ),
    "Interface for connecting to the Mordred NOC",
    SST::Interfaces::SimpleNetwork
  )

  SST_ELI_DOCUMENT_PARAMS(
    { "verbose", "Sets the output verbosity", "5" },
    { "clock", "Clock frequency of this interface", "1GHz" },
    { "input_buf_size", "Size of input buffers specified in b or B (can include SI prefix).", "1kiB" },
    { "output_buf_size", "Size of output buffers specified in b or B (can include SI prefix).", "1kiB" },
  )

  SST_ELI_DOCUMENT_PORTS(
    { "port", "Port that connects to a Mordred router.", { "untimedMordredEvent", "basicMordredEvent" } },
  )

  SST_ELI_DOCUMENT_STATISTICS(
    { "packets_recv", "Number of packets received", "unitless", 3 },
    { "average_noc_latency", "Average latency (in clocks) of each packet", "unitless", 3 },
    { "average_packet_size", "Average packet size in number of flits", "unitless", 3 }
  )

  MordredNIC( ComponentId_t cid, Params& params, int vns );
  ~MordredNIC() override = default;

  MordredNIC() : MordredNicBase() {}

  void serialize_order( SST::Core::Serialization::serializer& ser ) override {
    MordredNicBase::serialize_order( ser );
    SST_SER( link );
  }

  ImplementSerializable( SST::Mordred::MordredNIC );

protected:
  void        transportSendFlit( MordredFlit* flit, uint32_t vn ) override { link->send( flit ); }
  void        transportSendCredit( MordredCreditEvent* credit, uint32_t vn ) override { link->send( credit ); }
  void        transportSendUntimedData( SST::Event* ev ) override { link->sendUntimedData( ev ); }
  SST::Event* transportRecvUntimedData() override { return link->recvUntimedData(); }

private:
  void handleIncomingPacket( SST::Event* ev ) { processIncomingEvent( ev ); }

  Link* link{};
};

}  // namespace SST::Mordred

#endif  // MORDRED_MORDREDNIC_H
