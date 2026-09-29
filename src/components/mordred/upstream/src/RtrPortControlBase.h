//
// RtrPortControlBase.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_RTRPORTCONTROLBASE_H
#define MORDRED_RTRPORTCONTROLBASE_H

#include "sst_config.h"
#include "MordredEvents.h"
#include "RtrPortControlAPI.h"

namespace SST::Mordred {

/**
 * Abstract base class shared by RtrPortControl and RtrPortControlPC.
 *
 * Holds all data members and implements all logic independent of the physical
 * transport.  Derived classes supply four pure-virtual transport primitives
 * (send flit, send credit, send untimed, recv untimed), an optional space-to-
 * send gate (defaults to true for raw-link), and optional lifecycle hooks.
 */
class RtrPortControlBase : public RtrPortControlAPI {

public:
  RtrPortControlBase(
    ComponentId_t       id,
    Params&             params,
    TopologyAPI*        topology,
    RtrOwnedSharedObjs* rtr_shared_objs,
    uint32_t            rtr_num,
    uint32_t            port_num,
    const char*         class_name
  );

  ~RtrPortControlBase() override { delete output; }

  // SST lifecycle
  void init( unsigned int phase ) override;
  void setup() override;
  void complete( unsigned int phase ) override;
  void finish() override { /* empty */ }

  // RtrPortControlAPI untimed interface
  void   sendUntimedData( Event* ev ) override;
  Event* recvUntimedData() override;

  // Inline API overrides — identical in both derived classes
  PortConnectionE getConnectionType() override { return connectionType; }
  uint32_t        getPortId() override { return portId; }

  void ClockTick( Cycle_t cycle ) override;

  void validateVnVc( uint32_t vn, uint32_t vc ) {
    if( ( vn == UINT32_MAX ) || ( vc == UINT32_MAX ) )
      output->fatal( CALL_INFO, -1, "Invalid vn=%u or vc=%u\n", vn, vc );
    if( vn >= numVns )
      output->fatal( CALL_INFO, -1, "Invalid vn=%u\n", vn );
    if( vc >= numVcs )
      output->fatal( CALL_INFO, -1, "Invalid vc=%u\n", vc );
  }

  std::pair<uint32_t, uint32_t> getSwitchSendVnVc() override {
    return { switch_alloc_sendfrom_vn, switch_alloc_sendfrom_vc };
  }
  std::pair<uint32_t, uint32_t> getSwitchRecvVnVc() override {
    return { switch_alloc_rcvto_vn, switch_alloc_rcvto_vc };
  }

  uint32_t getDestPort( uint32_t vn, uint32_t vc ) override {
    validateVnVc( vn, vc );
    return inStateVec.at( vn ).at( vc ).outPort;
  }
  OutVcStateE getOutputState( uint32_t vn, uint32_t vc ) override {
    validateVnVc( vn, vc );
    return outStateVec.at( vn ).at( vc ).outVcState;
  }
  void inUnitSetDestVc( uint32_t vn, uint32_t input_vc, uint32_t dest_vc ) override {
    validateVnVc( vn, input_vc );
    inStateVec.at( vn ).at( input_vc ).outVn = vn;
    inStateVec.at( vn ).at( input_vc ).outVc = dest_vc;
  }
  void outUnitSetSrc( uint32_t port, uint32_t vn, uint32_t src_vc, uint32_t output_vc ) override {
    validateVnVc( vn, output_vc );
    outStateVec.at( vn ).at( output_vc ).outVcState = OUT_BUSY;
    outStateVec.at( vn ).at( output_vc ).inPort     = port;
    outStateVec.at( vn ).at( output_vc ).inVn       = vn;
    outStateVec.at( vn ).at( output_vc ).inVc       = src_vc;
  }

  bool isSendAllocatedToSwitch() override { return switch_alloc_sendto_port != UINT32_MAX; }
  void sendAllocateToSwitch( uint32_t port, uint32_t vn, uint32_t vc ) override {
    validateVnVc( vn, vc );
    if( port != inStateVec.at( vn ).at( vc ).outPort )
      output->fatal(
        CALL_INFO, -1, "Port mismatch in=%" PRIu32 ", state=%" PRIu32 "\n", port, inStateVec.at( vn ).at( vc ).outPort
      );
    switch_alloc_sendto_port = port;
    switch_alloc_sendfrom_vn = vn;
    switch_alloc_sendfrom_vc = vc;
  }
  void resetSwitchSendAllocation() override {
    validateVnVc( switch_alloc_sendfrom_vn, switch_alloc_sendfrom_vc );
    inStateVec.at( switch_alloc_sendfrom_vn ).at( switch_alloc_sendfrom_vc ).inVcState = IN_IDLE;
    switch_alloc_sendto_port = switch_alloc_sendfrom_vn = switch_alloc_sendfrom_vc = UINT32_MAX;
  }

  bool isRecvAllocatedFromSwitch() override { return switch_alloc_rcvto_vn != UINT32_MAX; }
  void recvAllocateFromSwitch( uint32_t sending_port, uint32_t vn, uint32_t vc ) override {
    validateVnVc( vn, vc );
    if( sending_port != outStateVec.at( vn ).at( vc ).inPort )
      output->fatal(
        CALL_INFO, -1, "Port mismatch in=%" PRIu32 ", state=%" PRIu32 "\n", sending_port, outStateVec.at( vn ).at( vc ).inPort
      );
    switch_alloc_rcvfrom_port = sending_port;
    switch_alloc_rcvto_vn     = vn;
    switch_alloc_rcvto_vc     = vc;
  }
  void resetSwitchRecvAllocation() override {
    validateVnVc( switch_alloc_rcvto_vn, switch_alloc_rcvto_vc );
    outStateVec.at( switch_alloc_rcvto_vn ).at( switch_alloc_rcvto_vc ).outVcState = OUT_IDLE;
    switch_alloc_rcvfrom_port = switch_alloc_rcvto_vn = switch_alloc_rcvto_vc = UINT32_MAX;
  }

  uint32_t getDestVc( uint32_t vn, uint32_t vc ) override {
    validateVnVc( vn, vc );
    return inStateVec.at( vn ).at( vc ).outVc;
  }
  uint32_t getSendingPort() override {
    if( switch_alloc_rcvfrom_port == UINT32_MAX )
      return UINT32_MAX;
    validateVnVc( switch_alloc_rcvto_vn, switch_alloc_rcvto_vc );
    if( outStateVec.at( switch_alloc_rcvto_vn ).at( switch_alloc_rcvto_vc ).outBufCredits > 0 )
      return switch_alloc_rcvfrom_port;
    return UINT32_MAX - 1;
  }

  MordredFlit* getInBufFlit() override;
  void         recvOutBufFlit( MordredFlit* flit ) override;

  void resetPerVcDest() override {
    validateVnVc( switch_alloc_sendfrom_vn, switch_alloc_sendfrom_vc );
    inStateVec.at( switch_alloc_sendfrom_vn ).at( switch_alloc_sendfrom_vc ).outPort =
      inStateVec.at( switch_alloc_sendfrom_vn ).at( switch_alloc_sendfrom_vc ).outVn =
        inStateVec.at( switch_alloc_sendfrom_vn ).at( switch_alloc_sendfrom_vc ).outVc = UINT32_MAX;
  }
  void resetPerVcSrc() override {
    validateVnVc( switch_alloc_rcvto_vn, switch_alloc_rcvto_vc );
    outStateVec.at( switch_alloc_rcvto_vn ).at( switch_alloc_rcvto_vc ).inPort =
      outStateVec.at( switch_alloc_rcvto_vn ).at( switch_alloc_rcvto_vc ).inVn =
        outStateVec.at( switch_alloc_rcvto_vn ).at( switch_alloc_rcvto_vc ).inVc = UINT32_MAX;
  }

  uint32_t getConnectedRtrId() const override { return connectedRtrId; }

  bool isWrapAroundOutputPort( uint32_t output_port ) const override;

  // Default constructor for serialization
  RtrPortControlBase() : RtrPortControlAPI() {}

  void serialize_order( SST::Core::Serialization::serializer& ser ) override;

protected:
  // ---- Transport abstraction (derived class must supply these four) ----

  virtual void        transportSendFlit( MordredFlit* flit, uint32_t vn )            = 0;
  virtual void        transportSendCredit( MordredCreditEvent* credit, uint32_t vn ) = 0;
  virtual void        transportSendUntimedData( SST::Event* ev )                     = 0;
  virtual SST::Event* transportRecvUntimedData()                                     = 0;

  // Returns false to gate output sends; default true (raw link has no inner flow control).
  // Takes vc as well as vn so transports that address (vn,vc) pairs independently on the
  // wire (see RtrPortControlPC) can check space for the right underlying channel.
  virtual bool transportSpaceToSend( uint32_t vn, uint32_t vc ) { return true; }

  // Lifecycle hooks — default no-ops; RtrPortControlPC overrides to forward to physChannel
  virtual void transportInit( uint32_t phase ) {}
  virtual void transportSetup() {}
  virtual void transportComplete( uint32_t phase ) {}

  // Shared incoming-packet dispatch; call from derived event handler / functor
  void processIncomingEvent( SST::Event* ev );

  // ---- Shared data members ----

  Output*         output{};
  TopologyAPI*    topo{};
  PortConnectionE connectionType{ UNKNOWN };
  uint32_t        rtrId{};
  uint32_t        portId{};
  uint32_t        connectedRtrId{ UINT32_MAX };
  uint32_t        connectedPortId{ UINT32_MAX };
  uint32_t        numVns{ UINT32_MAX };
  uint32_t        numVcs{ UINT32_MAX };
  uint32_t        flitSize{};
  uint32_t        flit_vn_rr{};
  uint32_t        flit_vc_rr{};
  uint32_t        credit_ret_vn_rr{};
  uint32_t        credit_ret_vc_rr{};

  uint32_t switch_alloc_sendto_port{ UINT32_MAX };
  uint32_t switch_alloc_sendfrom_vn{ UINT32_MAX };
  uint32_t switch_alloc_sendfrom_vc{ UINT32_MAX };
  uint32_t switch_alloc_rcvfrom_port{ UINT32_MAX };
  uint32_t switch_alloc_rcvto_vn{ UINT32_MAX };
  uint32_t switch_alloc_rcvto_vc{ UINT32_MAX };

  uint32_t inBufSize{};
  uint32_t outBufSize{};

  std::queue<Event*> initEvents;

  std::vector<std::vector<perVcInState>>  inStateVec;
  std::vector<std::vector<perVcOutState>> outStateVec;

  RtrOwnedSharedObjs* rtrSharedObjs{};

  std::vector<std::vector<Statistic<uint64_t>*>> statLinkRecvFlitCnt;
  std::vector<std::vector<Statistic<uint64_t>*>> statLinkSentFlitCnt;
  std::vector<std::vector<Statistic<uint64_t>*>> statLinkSentPacketCnt;
  Statistic<uint64_t>*                           statLinkOutputStalledCnt{};

private:
  void              allocateBuffers();
  MordredInitEvent* getInitEvent( MordredInitEvent::Commands cmd );
  void              returnCredit();
};

}  // namespace SST::Mordred

#endif  // MORDRED_RTRPORTCONTROLBASE_H
