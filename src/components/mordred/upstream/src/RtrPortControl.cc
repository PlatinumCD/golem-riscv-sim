//
// RtrPortControl.cc
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#include "RtrPortControl.h"

using namespace SST::Mordred;

RtrPortControl::RtrPortControl(
  ComponentId_t id, Params& params, TopologyAPI* topology,
  RtrOwnedSharedObjs* rtr_shared_objs, uint32_t rtr_num, uint32_t port_num
) : RtrPortControlBase( id, params, topology, rtr_shared_objs, rtr_num, port_num, "RtrPortControl" ) {
  const std::string pname = "port" + std::to_string( port_num );
  link = configureLink( pname, new Event::Handler2<RtrPortControl, &RtrPortControl::inHandler>( this ) );
  if( !link )
    output->fatal( CALL_INFO, -1, "Unable to configure link %s\n", pname.c_str() );

  output->verbose(
    CALL_INFO, MORDRED_VERBOSE_MIN, 0,
    "Constructor complete; [Rtr.Port]=[%" PRIu32 ".%" PRIu32 "], inbuf=%" PRIu32 "b, outbuf=%" PRIu32 "b\n",
    rtrId, portId, inBufSize, outBufSize
  );
}
