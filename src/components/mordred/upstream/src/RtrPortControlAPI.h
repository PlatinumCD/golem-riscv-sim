//
// RtrPortControlAPI.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_RTRPORTCONTROLAPI_H
#define MORDRED_RTRPORTCONTROLAPI_H

// Local SST config
#include <cstdint>

#include "sst_config.h"

#include "MordredEvents.h"

/*
 * In Merlin, the PortInterface API is in router.h; the purpose of the PortInterface is to
 * provide a set of functions for sending/receiving messages on one of the ports of the
 * router. This is separate from the linkControl (in Merlin) and MordredNIC (here) that
 * is for the endpoint connecting to the network.
 *
 * In Merlin::PortInterface, there is also the API for the output arbitration unit;
 * based on the output of sst-info, there are at least a couple of options for this
 * Default is merlin.arb.output.basic
 *
 * While the MordredNIC inherits from SimpleNetwork::Interface (as does Merlin::linkControl),
 * the Merlin::PortInterface and Merlin::PortControl are a subcomponent API and subcomponent
 * with no inheritance from SimpleNetwork.  That written, the PortInterface has many of
 * the same functions as a SimpleNetwork::Interface.
 *
 */

namespace SST::Mordred {

class InVcHeads;    // forward declaration
class TopologyAPI;  // forward declaration

// These are for the individual VCs
// Currently not using the {WAIT,NEED}_CREDITS states
enum InVcStateE { IN_IDLE, ROUTING, WAIT_VC, WAIT_CREDITS, IN_ACTIVE };  // for the port input side - match Dally book

enum OutVcStateE { OUT_IDLE, OUT_BUSY, NEED_CREDITS };  // for the port output side - mainly for xbar arb

/**
 * For now, I'm taking the following two structures pretty much directly
 * from the Dally interconnections book
 *
 * The buffers in these structs are not size limited; we'll use the number of credits
 * to manage the usage of the buffers
 *
 */
struct perVcInState {
  InVcStateE               inVcState;
  uint32_t                 outPort;
  uint32_t                 outVn;
  uint32_t                 outVc;
  int32_t                  retCredits;  // credits to return to sender as inBuf is emptied; init to zero
  std::queue<MordredFlit*> inBuf;

  void reset() {
    inVcState  = IN_IDLE;
    outPort    = UINT32_MAX;
    outVn      = UINT32_MAX;
    outVc      = UINT32_MAX;
    retCredits = 0;
  }

  void serialize_order( SST::Core::Serialization::serializer& ser ) {
    SST_SER( inVcState );
    SST_SER( outPort );
    SST_SER( outVn );
    SST_SER( outVc );
    SST_SER( retCredits );
    SST_SER( inBuf );
  }
};

struct perVcOutState {
  OutVcStateE outVcState;
  uint32_t    inPort;
  uint32_t    inVn;
  uint32_t    inVc;
  int32_t     outBufCredits;  // space available in outBuf; dec on write to buf, inc when flit put on link

  // (dec on send to dest, inc when credit packet comes from dest)
  int32_t
    destCredits;  // maintains available credits for myVn.myVc downstream; initialized to non-zero in init (dest sends a count)
  std::queue<MordredFlit*> outBuf;

  void reset( int32_t ob_creds ) {
    outVcState    = OUT_IDLE;
    inPort        = UINT32_MAX;
    inVn          = UINT32_MAX;
    inVc          = UINT32_MAX;
    outBufCredits = ob_creds;
    destCredits   = 0;
  }

  void serialize_order( SST::Core::Serialization::serializer& ser ) {
    SST_SER( outVcState );
    SST_SER( inPort );
    SST_SER( inVn );
    SST_SER( inVc );
    SST_SER( outBufCredits );
    SST_SER( destCredits );
    SST_SER( outBuf );
  }
};

class RtrPortControlAPI : public SubComponent {
public:
  SST_ELI_REGISTER_SUBCOMPONENT_API( SST::Mordred::RtrPortControlAPI, TopologyAPI*, RtrOwnedSharedObjs*, uint32_t, uint32_t )

  enum PortConnectionE { ENDPOINT, ROUTER, UNKNOWN, INVALID };

  /// RtrPortControlAPI: constructor
  RtrPortControlAPI( ComponentId_t id ) : SubComponent( id ) {}

  /// RtrPortControlAPI : constructor for serialization
  RtrPortControlAPI() : SubComponent() {}

  /// RtrPortControlAPI: default destructor
  ~RtrPortControlAPI() override                                                                        = default;

  /// Untimed recv/send
  virtual void   sendUntimedData( Event* ev )                                                          = 0;
  virtual Event* recvUntimedData()                                                                     = 0;

  virtual PortConnectionE getConnectionType()                                                          = 0;
  virtual uint32_t        getPortId()                                                                  = 0;

  // No separate clock - run off the router clock
  // NOTE: This may get broken up into separate update and execute functions; booksim approached their
  // model this way and described as update being the combinational logic and execute being the state
  // updates
  virtual void ClockTick( Cycle_t cycle )                                                              = 0;

  virtual uint32_t getConnectedRtrId() const                                                           = 0;

  /// True when this port's outgoing link is a wrap-around link in the torus.
  /// Delegates to the topology; default-returns false for non-torus topologies.
  virtual bool isWrapAroundOutputPort( uint32_t output_port ) const { return false; }

  virtual uint32_t getSendingPort()                                                                    = 0;

  virtual std::pair<uint32_t, uint32_t> getSwitchSendVnVc()                                            = 0;
  virtual std::pair<uint32_t, uint32_t> getSwitchRecvVnVc()                                            = 0;

  // VC Allocator functions
  virtual uint32_t    getDestPort( uint32_t vn, uint32_t vc )                                          = 0;
  virtual OutVcStateE getOutputState( uint32_t vn, uint32_t vc )                                       = 0;
  virtual void        inUnitSetDestVc( uint32_t vn, uint32_t input_vc, uint32_t dest_vc )              = 0;
  virtual void        outUnitSetSrc( uint32_t port, uint32_t vn, uint32_t src_vc, uint32_t output_vc ) = 0;

  // Switch allocation functions
  virtual bool     isSendAllocatedToSwitch()                                                           = 0;
  virtual void     sendAllocateToSwitch( uint32_t port, uint32_t vn, uint32_t vc )                     = 0;
  virtual void     resetSwitchSendAllocation()                                                         = 0;
  virtual uint32_t getDestVc( uint32_t vn, uint32_t vc )                                               = 0;

  virtual bool isRecvAllocatedFromSwitch()                                                             = 0;
  virtual void recvAllocateFromSwitch( uint32_t sending_port, uint32_t vn, uint32_t vc )               = 0;
  virtual void resetSwitchRecvAllocation()                                                             = 0;

  // Moving packets
  virtual MordredFlit* getInBufFlit()                                                                  = 0;
  virtual void         recvOutBufFlit( MordredFlit* flit )                                             = 0;

  // Reset input/output state when we see a tail flit - use the switch_alloc variables (at least for now)
  // to determine which input/output set to reset
  // These are not my favorite names
  virtual void resetPerVcDest()                                                                        = 0;
  virtual void resetPerVcSrc()                                                                         = 0;

};  // class RtrPortControlAPI

}  // namespace SST::Mordred

#endif  //MORDRED_RTRPORTCONTROLAPI_H
