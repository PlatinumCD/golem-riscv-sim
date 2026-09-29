//
// XbarArbAPI.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_XBARARBAPI_H
#define MORDRED_XBARARBAPI_H

// Standard headers
#include <cstdint>

// Local SST header
#include "sst_config.h"

// Other local headers
#include "MordredEvents.h"
#include "RtrPortControlAPI.h"

/**
 * This API is for the crossbar arbitration that happens within the router; in Merlin, this
 * is a pretty minimal API (XbarArbitration in router.h)
 */
namespace SST::Mordred {

class XbarArbAPI : public SubComponent {
public:
  SST_ELI_REGISTER_SUBCOMPONENT_API(
    SST::Mordred::XbarArbAPI, uint32_t, uint32_t, uint32_t, uint32_t
  );  // rtr_id, num_ports, num_vns, num_vcs

  /// XbarArbAPI: constructor
  XbarArbAPI( ComponentId_t id ) : SubComponent( id ) {}

  // XbarArbAPI: default constructor for serialization
  XbarArbAPI() : SubComponent() {}

  /// XbarArbAPI: default destructor
  ~XbarArbAPI() override                                                                                             = default;

  /// Main arbitration function
  virtual void arbitrate( std::vector<RtrPortControlAPI*>& ports, std::vector<RtrOwnedSharedObjs>& rtr_shared_objs ) = 0;

};  // class XbarArbAPI

}  // namespace SST::Mordred

#endif  //MORDRED_XBARARBAPI_H
