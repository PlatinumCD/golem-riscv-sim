//
// MordredNIC.cc
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#include "MordredNIC.h"

using namespace SST::Mordred;

MordredNIC::MordredNIC( ComponentId_t cid, Params& params, int vns )
    : MordredNicBase( cid, params, vns, "MordredNIC" ) {
  link = configureLink( "port", new Event::Handler2<MordredNIC, &MordredNIC::handleIncomingPacket>( this ) );
  if( !link )
    output->fatal( CALL_INFO, -1, "Failed to initialize link\n" );

  output->verbose( CALL_INFO, MORDRED_VERBOSE_MIN, 0, "MordredNIC constructed\n" );
  output->flush();
}
