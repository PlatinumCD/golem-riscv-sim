//
// MordredNicBase.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_MORDREDNICBASE_H
#define MORDRED_MORDREDNICBASE_H

#include <cinttypes>
#include <queue>
#include <vector>

#include "sst_config.h"
#include "MordredEvents.h"

namespace SST::Mordred {

/**
 * Abstract base class shared by MordredNIC and MordredNicPC.
 *
 * Holds all data members and implements all logic independent of the physical
 * transport.  Derived classes supply four pure-virtual transport primitives
 * (send flit, send credit, send untimed, recv untimed) and optional lifecycle
 * hooks (transportInit / transportSetup / transportComplete).
 */
class MordredNicBase : public Interfaces::SimpleNetwork {

public:
  MordredNicBase( ComponentId_t cid, Params& params, int vns, const char* class_name );
  ~MordredNicBase() override { delete output; }

  // SST lifecycle
  void init( uint32_t phase ) override;
  void setup() override;
  void complete( uint32_t phase ) override;
  void finish() override;

  // SimpleNetwork outer interface
  void     sendUntimedData( Request* req ) override;
  Request* recvUntimedData() override;
  bool     send( Request* req, int vn ) override;
  Request* recv( int vn ) override;
  bool     spaceToSend( int vn, int num_bits ) override;
  bool     requestToReceive( int vn ) override;

  void setNotifyOnReceive( HandlerBase* functor ) override { receiveFunctor = functor; }
  void setNotifyOnSend( HandlerBase* functor ) override { sendFunctor = functor; }

  bool               isNetworkInitialized() const override { return initialized; }
  nid_t              getEndpointID() const override { return netID; }
  const UnitAlgebra& getLinkBW() const override { return bw; }

  bool clockTick( Cycle_t cycle );

  // Default constructor for serialization
  MordredNicBase() : Interfaces::SimpleNetwork() {}

  void serialize_order( SST::Core::Serialization::serializer& ser ) override;

protected:
  // ---- Transport abstraction (derived class must supply these four) ----

  virtual void        transportSendFlit( MordredFlit* flit, uint32_t vn )           = 0;
  virtual void        transportSendCredit( MordredCreditEvent* credit, uint32_t vn ) = 0;
  virtual void        transportSendUntimedData( SST::Event* ev )                     = 0;
  virtual SST::Event* transportRecvUntimedData()                                     = 0;

  // Lifecycle hooks — default no-ops; NicPC overrides to forward to physChannel
  virtual void transportInit( uint32_t phase ) {}
  virtual void transportSetup() {}
  virtual void transportComplete( uint32_t phase ) {}
  virtual void transportFinish() {}

  // Called once init() phase 2 learns the router's authoritative num_vcs (see
  // below). Default no-op; MordredNicPC overrides it to fail loudly if its own
  // (config-time, construction-fixed) VC-address widening factor doesn't match
  // what the router actually uses — a mismatch there would otherwise silently
  // collapse to an unwidened link and reintroduce the deadlock this exists to
  // prevent (see RtrPortControlPC::extVn()).
  virtual void transportValidateVcWidth( uint32_t /*router_num_vcs*/ ) {}

  // Shared incoming-packet dispatch; call from derived event handler / functor
  void processIncomingEvent( SST::Event* ev );

  // ---- Shared data members ----

  Output*  output{};
  nid_t    netID{ -1 };
  uint32_t rtrId{ UINT32_MAX };
  uint32_t rtrPort{ UINT32_MAX };
  bool     initialized{ false };
  uint32_t numVns{ 0 };
  uint32_t numVcs{ UINT32_MAX };
  uint32_t flitSize{};
  uint64_t packetId{};
  uint64_t headInjectCycle{ UINT64_MAX };

  UnitAlgebra bw;

  HandlerBase* sendFunctor{ nullptr };
  HandlerBase* receiveFunctor{ nullptr };

  UnitAlgebra inbufSize;
  UnitAlgebra outbufSize;

  std::queue<MordredInitEvent*> initEvents;

  std::vector<std::queue<Request*>>     inBuf;
  std::vector<std::queue<MordredFlit*>> outBuf;

  std::vector<int32_t> rtrCredits;
  std::vector<int32_t> outbufCredits;
  std::vector<int32_t> inReturnCredits;

  uint64_t             totalNocLatency{ 0 };
  uint64_t             totalPackets{ 0 };
  uint64_t             totalNumFlits{ 0 };
  Statistic<uint64_t>* statPacketsRecv{};
  Statistic<double>*   statAvgNocLatency{};
  Statistic<double>*   statAvgFlitsPerPacket{};

private:
  void              resizeVectors();
  MordredInitEvent* getInitEvent( MordredInitEvent::Commands cmd );
  int32_t           calcNumFlits( uint32_t num_bits );
};

}  // namespace SST::Mordred

#endif  // MORDRED_MORDREDNICBASE_H
