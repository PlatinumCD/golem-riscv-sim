//
// MordredTestEP.h
//
// Copyright (C) 2025-2026 Tactical Computing Laboratories, LLC
// All Rights Reserved
// contact@tactcomplabs.com
//
// See LICENSE in the top level directory for licensing details
//

#ifndef MORDRED_MORDREDTESTEP_H
#define MORDRED_MORDREDTESTEP_H

#include <cstdint>
#include <string>

#include "sst_config.h"
#include <sst/core/component.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/output.h>
#include <sst/core/params.h>
#include <sst/core/timeConverter.h>
#include <sst/core/unitAlgebra.h>

namespace SST::Mordred {

/**
 * MordredTestEP — multi-VN test endpoint for Mordred.
 *
 * A minimal traffic-generator component that loads any SST::Interfaces::SimpleNetwork
 * subcomponent with a configurable num_vns.  This makes it possible to test
 * UCIe num_stacks > 1, which requires total_vns > 1 — something merlin.test_nic
 * and merlin.trafficgen cannot provide (both hardcode num_vns=1 when loading
 * their networkIF subcomponent).
 *
 * Each endpoint sends num_messages messages, rotating through VNs 0..num_vns-1
 * and cycling through destinations so that traffic is spread across the network.
 * The simulation ends when every endpoint has finished sending.
 */
class MordredTestEP : public SST::Component {

public:
  SST_ELI_REGISTER_COMPONENT(
    MordredTestEP,
    "mordred",
    "mordredTestEP",
    SST_ELI_ELEMENT_VERSION( 0, 1, 0 ),
    "Multi-VN test endpoint for Mordred — use when num_vns > 1 is required",
    COMPONENT_CATEGORY_NETWORK
  )

  SST_ELI_DOCUMENT_PARAMS(
    { "id",           "Endpoint ID (must match the network interface assignment)", "0" },
    { "num_peers",    "Total number of endpoints in the network",                  "1" },
    { "num_vns",      "Number of virtual networks to request from the networkIF",  "1" },
    { "num_messages", "Total messages this endpoint will send",                   "10" },
    { "message_size", "Size of each message (UnitAlgebra; bits or bytes)",        "64b" },
    { "clock",        "Clock frequency",                                         "1GHz" },
    { "verbose",      "Verbosity level (0=off, 1=lifecycle, 2=per-message)",      "0" },
  )

  SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
    { "networkIF",
      "Network interface subcomponent — accepts any SST::Interfaces::SimpleNetwork "
      "implementation (e.g. mordred.mordredNIC, mordred.mordredNicPC)",
      "SST::Interfaces::SimpleNetwork" }
  )

  MordredTestEP( ComponentId_t id, Params& params );
  ~MordredTestEP() override = default;

  void init( uint32_t phase ) override;
  void setup() override;
  void complete( uint32_t phase ) override;
  void finish() override;

private:
  bool clockTick( Cycle_t cycle );
  bool onReceive( int vn );

  SST::Output                        output;
  SST::Interfaces::SimpleNetwork*    link_control{ nullptr };

  SST::Interfaces::SimpleNetwork::nid_t ep_id{ 0 };
  int64_t  num_peers{ 1 };
  int32_t  num_vns{ 1 };
  int64_t  num_messages{ 10 };
  int32_t  msg_size_bits{ 64 };

  int64_t  sent{ 0 };
  int64_t  received{ 0 };
  bool     done{ false };
};

}  // namespace SST::Mordred

#endif  // MORDRED_MORDREDTESTEP_H
