//
// MordredEvents.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_MORDREDEVENTS_H
#define MORDRED_MORDREDEVENTS_H

// Standard headers
#include <cinttypes>
#include <utility>

// Local SST header
#include "sst_config.h"

/**
 * For merlin, here is the "standard" approach for packet types from endpoint to endpoint:
 * - start with an endpoint event that creates a SimpleNetwork::Request; the endpoint event
 *    is stored into the SimpleNetwork::Request as the payload
 * - SimpleNetwork::Request is passed into the NIC (linkControl) and the NIC creates a wrapping event,
 *    RtrEvent.  The RtrEvent is transmitted on the SST::Link from the endpt NIC to the router
 * - The router then creates yet another wrapping event (internal_router_event) to use while the
 *    packet traverses the network (this transition happens in topology->proecess_input() ).
 * - The router removes the outer shell (internal_router_event) when sending the packet to the endpt
 *    NIC (thus the endpt NIC gets a RtrEvent)
 * - Endpt NIC removes the RtrEvent shell and returns a SimpleNetwork::Request to the endpt
 * - Endpt will remove the SimpleNetwork::Request shell and finally, we have the original endpoint event
 */

/**
 * In comparison, what I've done here (for now anyway) is the following:
 * - The simpleTestEvent is the endpoint event; the TestEP is the endpoint and it creates the
 *    SimpleNetwork::Request wrapper for the simpleTestEvent. The request destination is the router
 *    number/ID of the desired endpoint
 * - The MordredNIC receives the SimpleNetwork::Request and creates a series of MordredFlits that
 *    wraps the SimpleNetwork::Request (all MordredFlits contain a copy of the request pointer).
 *    The MordredFlit is transmitted on the SST::Link to the router
 * - The network passes around the MordredFlit until we hit the desired endpoint router; the MordredFlit
 *    is sent to the endpt NIC.
 * - The MordredNIC removes the MordredFlit wrapper and returns the SimpleNetwork::Request to the endpt
 *    once the TAIL flit is received
 * - Endpt code (TestEP) removes the SimpleNetwork::Request shell and prints out the message stored in
 *    the simpleTestEvent.
 *  - Only the HEAD and TAIL flits really need the SimpleNetwork::Request
 */

namespace SST::Mordred {

// Use for setting up output masks - not really using at the moment, but may
// be desirable in the long run
constexpr uint32_t DEBUG_CONSTRUCTORS = ( 1UL << 0 );
constexpr uint32_t DEBUG_INIT_PHASE   = ( 1UL << 1 );

// -- Verbosity Constants
#define MORDRED_VERBOSE_NONE  0
#define MORDRED_VERBOSE_MIN   1
#define MORDRED_VERBOSE_LOW   2
#define MORDRED_VERBOSE_MED   5
#define MORDRED_VERBOSE_HIGH  8
#define MORDRED_VERBOSE_ALL   10

// TODO: Add a mask for debugging credits

// This is a very simple event being sent by the TestEP.
class simpleTestEvent final : public Event {
public:
  simpleTestEvent() : Event() { /* empty */ }

  simpleTestEvent( std::string str_ ) : str( std::move( str_ ) ) { /* empty */ }

  void serialize_order( Core::Serialization::serializer& ser ) override {
    Event::serialize_order( ser );
    SST_SER( str );
  }

  ImplementSerializable( SST::Mordred::simpleTestEvent );

public:
  std::string str;
};

// Base event sent around the NoC.
class baseMordredEvent : public Event {
public:
  enum MordredEventType { FLIT, CREDIT, PACKET, INITIALIZATION };

  baseMordredEvent( MordredEventType type_ ) : type( type_ ) { /* empty */ }

  baseMordredEvent() : SST::Event() {}

  MordredEventType getType() { return type; }

  void serialize_order( Core::Serialization::serializer& ser ) override {
    Event::serialize_order( ser );
    SST_SER( type );
  }

  ImplementSerializable( SST::Mordred::baseMordredEvent );

private:
  MordredEventType type;
};

// Used to initialize the network
class MordredInitEvent final : public baseMordredEvent {
public:
  enum Commands {
    REPORT_ENDPOINT,
    REPORT_ROUTER,
    ROUTER_ID,
    PORT_NUM,
    ENDPOINT_ID,
    NUM_VNS,
    NUM_VCS,
    FLIT_WIDTH,
    EP_PACKET,
    NUM_COMMANDS
  };

  MordredInitEvent() : baseMordredEvent( INITIALIZATION ) {}

  MordredInitEvent( Interfaces::SimpleNetwork::Request* r )
    : baseMordredEvent( PACKET ), command( Commands::EP_PACKET ), value( 0 ), req( r ) {}

  MordredInitEvent* clone( void ) override {
    MordredInitEvent* ret = new MordredInitEvent( *this );
    if( this->req != nullptr ) {
      ret->req = this->req->clone();
    }
    return ret;
  }

  void serialize_order( Core::Serialization::serializer& ser ) override {
    baseMordredEvent::serialize_order( ser );
    SST_SER( command );
    SST_SER( ua_value );
    SST_SER( req );
  }

  ImplementSerializable( SST::Mordred::MordredInitEvent );

public:
  Commands                            command;
  uint32_t                            value;
  UnitAlgebra                         ua_value;
  Interfaces::SimpleNetwork::Request* req{ nullptr };
};

// This is intended to be the basic Flit running around the NoC.
class MordredFlit final : public baseMordredEvent {
public:
  enum FlitTypeE { HEAD, BODY, TAIL, EMPTY, NUM_TYPES };

  MordredFlit() : baseMordredEvent( FLIT ) { /* empty */ }

  MordredFlit( Interfaces::SimpleNetwork::Request* req_ )
    : baseMordredEvent( FLIT ), req( req_ ), vn( (uint32_t) req_->vn ) { /* empty */ }

  MordredFlit( Interfaces::SimpleNetwork::Request* req_, FlitTypeE ftype_, uint64_t pkt_id, uint32_t flit_id_ )
    : baseMordredEvent( FLIT ), ftype( ftype_ ), req( req_ ), vn( (uint32_t) req_->vn ), packet_id( pkt_id ),
      flit_id( flit_id_ ) { /* empty */ }

  Interfaces::SimpleNetwork::Request* getRequest() { return req; }

  /** START: Data members of the flit **/
  FlitTypeE ftype{ NUM_TYPES };

  // This is what the endpoint passes into the MordredNIC; within
  // this request is the data packet (SST::Event) that one endpoint
  // wants to pass to another endpoint
  // Not owned by the flit. All flits in a packet share the same req*. Ownership
  // transfers to inBuf on TAIL receipt; the caller of NicBase::recv() must delete.
  Interfaces::SimpleNetwork::Request* req;

  uint32_t vn{ 0 };
  uint32_t cur_vc{ 0 };  // Update when the flit is written into an output buffer
  uint64_t packet_id{};
  uint32_t flit_id{};
  // The values below are using getCurrentSimCycle() - this needs to be scaled by the clock frequency
  // to get the number of clock ticks

  uint64_t pkt_created_cycle{ UINT64_MAX };  // set when MordredNIC::send() is called

  // The MordredNIC stores the head time and then fills this for the TAIL; this allows us to just use
  // the data in the TAIL flit for doing "everything"
  uint64_t head_inject_cycle{ UINT64_MAX };  // set when the head flit is put onto the link.

  /** END: Data members of the flit **/

  std::string getFtypeStr() {
    switch( ftype ) {
    case HEAD: return "HEAD";
    case BODY: return "BODY";
    case TAIL: return "TAIL";
    case EMPTY: return "EMPTY";
    default: return "UNKNOWN";
    }
  }

  std::string pktIdStr() {
    std::string ss = "[Src.PktId.FlitId]=[" + std::to_string( req->src ) + "." + std::to_string( packet_id ) + "." +
                     std::to_string( flit_id ) + "]";
    return ss;
  }

  // Events must provide a serialization function that serializes
  // all data members of the event
  void serialize_order( SST::Core::Serialization::serializer& ser ) override {
    baseMordredEvent::serialize_order( ser );
    SST_SER( ftype );
    SST_SER( req );
    SST_SER( vn );
    SST_SER( cur_vc );
    SST_SER( packet_id );
    SST_SER( flit_id );
    SST_SER( pkt_created_cycle );
    SST_SER( head_inject_cycle );
  }

  // Register this event as serializable
  ImplementSerializable( SST::Mordred::MordredFlit );
};

// More-or-less borrowed from merlin's credit_event in router.h
class MordredCreditEvent : public baseMordredEvent {
public:
  uint32_t vn;
  uint32_t vc;
  int32_t  credits;

  MordredCreditEvent() : baseMordredEvent( CREDIT ) {}

  MordredCreditEvent( uint32_t vn_, uint32_t vc_, int32_t credits_ )
    : baseMordredEvent( CREDIT ), vn( vn_ ), vc( vc_ ), credits( credits_ ) { /* empty */ }

  void serialize_order( Core::Serialization::serializer& ser ) override {
    baseMordredEvent::serialize_order( ser );
    SST_SER( vn );
    SST_SER( vc );
    SST_SER( credits );
  }

  ImplementSerializable( SST::Mordred::MordredCreditEvent );
};

/**
 * So the naming here is probably horrible, but this is a collection of
 * data structures that the router owns but is shared between units within
 * the router.
 *
 * The router will create a vector of these objects (one element per port) and then
 * the data objects within this struct will be what the ports operate on/deal with.
 *
 */
struct RtrOwnedSharedObjs {
  // Stays false if this object is for a port that is unconnected/invalid
  bool isValid{ false };

  // Could these be booleans?
  std::vector<std::vector<MordredFlit*>> needVcAlloc;      // ports place into this, the VC allocator will clear entries
  std::vector<std::vector<MordredFlit*>> needSwitchAlloc;  // ports place into this, the switch allocator will clear entries

  void allocateVecs( uint32_t num_vns, uint32_t num_vcs ) {
    isValid = true;
    needVcAlloc.resize( num_vns );
    needSwitchAlloc.resize( num_vns );
    for( uint32_t i = 0; i < num_vns; i++ ) {
      needVcAlloc[i].resize( num_vcs, nullptr );
      needSwitchAlloc[i].resize( num_vcs, nullptr );
    }
  }

  // For serialization
  void serialize_order( SST::Core::Serialization::serializer& ser ) {
    SST_SER( isValid );
    SST_SER( needVcAlloc );
    SST_SER( needSwitchAlloc );
  }
};

}  // namespace SST::Mordred

#endif  //MORDRED_MORDREDEVENTS_H
