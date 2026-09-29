//
// VcAllocAPI.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_VCALLOCAPI_H
#define MORDRED_VCALLOCAPI_H

// Standard headers
#include <cstdint>

// Local SST header
#include "sst_config.h"

// Other local headers
#include "MordredEvents.h"
#include "RtrPortControlAPI.h"

/**
 * This API is for the virtual channel allocation that happens within the router; there is not
 * a direct analog for this in merlin.hr_router
 */
namespace SST::Mordred {

class VcAllocAPI : public SubComponent {
public:
  SST_ELI_REGISTER_SUBCOMPONENT_API(
    SST::Mordred::VcAllocAPI, uint32_t, uint32_t, uint32_t, uint32_t
  );  // rtr_id, num_ports, num_vns, num_vcs

  /// VcAllocAPI: constructor
  VcAllocAPI( ComponentId_t id ) : SubComponent( id ) {}

  /// VcAllocAPI: default constructor for serialization
  VcAllocAPI() : SubComponent() {}

  /// ArbAPI: default destructor
  ~VcAllocAPI() override                                                                                             = default;

  /// Main arbitration function
  virtual void arbitrate( std::vector<RtrPortControlAPI*>& ports, std::vector<RtrOwnedSharedObjs>& rtr_shared_objs ) = 0;

};  // class VcAllocAPI

}  // namespace SST::Mordred

#endif  //MORDRED_VCALLOCAPI_H
