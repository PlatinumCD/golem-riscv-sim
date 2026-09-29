//
// VcAllocRR.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

// Notes:
// - This will not modify the VN of any flit that goes through this allocator
// - This allocator is just looking for an IDLE destination (output) VC; not checking
//   credits anywhere

#ifndef MORDRED_VCALLOCRR_H
#define MORDRED_VCALLOCRR_H

// Standard headers
#include <cstdint>

// Local SST header
#include "sst_config.h"

// Other local headers
#include "MordredEvents.h"
#include "VcAllocAPI.h"

namespace SST::Mordred {

class VcAllocRR : public VcAllocAPI {
public:
  SST_ELI_REGISTER_SUBCOMPONENT(
    VcAllocRR,
    "mordred",
    "VcAllocRR",
    SST_ELI_ELEMENT_VERSION( 0, 1, 0 ),
    "Round robin allocation for virtual channels",
    SST::Mordred::VcAllocAPI
  )

  // TODO: Use or delete this parameter - just auto set to 5 now in constructor
  SST_ELI_DOCUMENT_PARAMS( { "verbose", "Sets the output verbosity", "5" }, )  // currently unused

  SST_ELI_DOCUMENT_PORTS()

  SST_ELI_DOCUMENT_STATISTICS()

  VcAllocRR( ComponentId_t id, Params& params, uint32_t rtr_id, uint32_t num_ports, uint32_t num_vns, uint32_t num_vcs );

  ~VcAllocRR() override { delete output; }

  // Lifecycle functions
  void init( unsigned int phase ) final { /* empty */ }

  void setup() final { /* empty */ }

  void complete( unsigned int phase ) override { /* empty */ }

  void finish() override { /* empty */ }

  void arbitrate( std::vector<RtrPortControlAPI*>& ports, std::vector<RtrOwnedSharedObjs>& rtr_shared_objs ) final;

  /// default constructor
  VcAllocRR() : VcAllocAPI() {}

  /// serialization
  void serialize_order( SST::Core::Serialization::serializer& ser ) override {
    SST_SER( output );
    SST_SER( rtrId );
    SST_SER( numPorts );
    SST_SER( numVns );
    SST_SER( numVcs );
    SST_SER( rr_port );
    SST_SER( rr_vn );
    SST_SER( rr_vc );
    SST_SER( src_vn );
    SST_SER( src_vc );
    SST_SER( rr_dest_vc );
  }

  /// serialization implementations
  ImplementSerializable( SST::Mordred::VcAllocRR );

private:
  Output*  output;
  uint32_t rtrId;
  uint32_t numPorts{ UINT32_MAX };
  uint32_t numVns{ UINT32_MAX };
  uint32_t numVcs{ UINT32_MAX };

  uint32_t rr_port{ 0 };
  uint32_t rr_vn{ 0 };
  uint32_t rr_vc{ 0 };
  uint32_t src_vn;
  uint32_t src_vc;
  uint32_t rr_dest_vc{ 0 };

  void resetSrcVnVc() {
    src_vn = UINT32_MAX;
    src_vc = UINT32_MAX;
  }

  MordredFlit* findMappableFlit( RtrOwnedSharedObjs* obj );
  uint32_t     findDestVc( RtrPortControlAPI*& port, uint32_t in_port, uint32_t out_port ) const;
};

}  // namespace SST::Mordred

#endif  //MORDRED_VCALLOCRR_H
