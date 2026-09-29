//
// XbarArbRR.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

// Changing this file up quite a bit - it was originally doing both VC and switch alloc, but
// now it's just going to be for switch allocation

#ifndef MORDRED_XBARARBRR_H
#define MORDRED_XBARARBRR_H

// Standard headers
#include <cstdint>

// Local SST header
#include "sst_config.h"

// Other local headers
#include "MordredEvents.h"
#include "XbarArbAPI.h"

namespace SST::Mordred {

class XbarArbRR : public XbarArbAPI {
public:
  SST_ELI_REGISTER_SUBCOMPONENT(
    XbarArbRR,
    "mordred",
    "xbarArbRR",
    SST_ELI_ELEMENT_VERSION( 0, 1, 0 ),
    "Round robin arbitration for the crossbar switch within the router",
    SST::Mordred::XbarArbAPI
  )

  // TODO: Use or delete this parameter - just auto set to 5 now in constructor
  SST_ELI_DOCUMENT_PARAMS( { "verbose", "Sets the output verbosity", "5" }, )  // currently unused

  SST_ELI_DOCUMENT_PORTS()

  SST_ELI_DOCUMENT_STATISTICS()

  XbarArbRR( ComponentId_t id, Params& params, uint32_t rtr_id, uint32_t num_ports, uint32_t num_vns, uint32_t num_vcs );

  ~XbarArbRR() override { delete output; }

  void arbitrate( std::vector<RtrPortControlAPI*>& ports, std::vector<RtrOwnedSharedObjs>& rtr_shared_objs ) final;

  /// default constructor
  XbarArbRR() : XbarArbAPI() {}

  /// serialization
  void serialize_order( SST::Core::Serialization::serializer& ser ) override {
    SST_SER( output );
    SST_SER( rtrId );
    SST_SER( numPorts );
    SST_SER( numVns );
    SST_SER( numVcs );
    SST_SER( recv_rr_port );
    SST_SER( send_rr_port );
    SST_SER( send_rr_vn );
    SST_SER( send_rr_vc );
    SST_SER( sending_vn );
    SST_SER( sending_vc );
  }

  /// serialization implementations
  ImplementSerializable( SST::Mordred::XbarArbRR );

private:
  Output*  output;
  uint32_t rtrId;
  uint32_t numPorts{ UINT32_MAX };
  uint32_t numVns{ UINT32_MAX };
  uint32_t numVcs{ UINT32_MAX };

  uint32_t recv_rr_port{ 0 };  // use to track rr start for receiving ports
  uint32_t send_rr_port{ 0 };  // use to track rr start for sending ports
  uint32_t send_rr_vn{ 0 };
  uint32_t send_rr_vc{ 0 };
  uint32_t sending_vn;
  uint32_t sending_vc;

  void resetSendingVnVc() { sending_vn = sending_vc = UINT32_MAX; }

  bool findSendableFlit( uint32_t rcvportnum, RtrPortControlAPI*& sendport, RtrOwnedSharedObjs& shared_obj );
};

}  // namespace SST::Mordred

#endif  //MORDRED_XBARARBRR_H
