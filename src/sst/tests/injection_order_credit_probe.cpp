#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/output.h>
#include "network/packetEvent.h"
#include "network/wormholeEvents.h"

// A controllable source-router boundary: no guest, routing or polling retries.
// Return a paced credit burst after the NIC has emptied its first lane, then
// require a blocked same-destination lane migration to wake on the final credit.
class InjectionCreditProbe final : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(InjectionCreditProbe, "injection_credit_test", "probe",
        SST_ELI_ELEMENT_VERSION(1,0,0), "Deferred credit ordering regression",
        COMPONENT_CATEGORY_UNCATEGORIZED)
    SST_ELI_DOCUMENT_PORTS({"lane0", "Test router lane 0", {}},
                           {"lane1", "Test router lane 1", {}})
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS({"nic", "NIC under test",
                                        "SST::Interfaces::SimpleNetwork"})
    InjectionCreditProbe(SST::ComponentId_t id, SST::Params&) : SST::Component(id) {
        nic_ = loadUserSubComponent<SST::Interfaces::SimpleNetwork>(
            "nic", SST::ComponentInfo::SHARE_NONE, 1);
        if (!nic_) out_.fatal(CALL_INFO, -1, "missing NIC\n");
        for (unsigned lane=0; lane<2; ++lane)
            links_[lane] = configureLink("lane" + std::to_string(lane),
                new SST::Event::Handler<InjectionCreditProbe,
                    &InjectionCreditProbe::receive, int>(this, lane));
        nic_->setNotifyOnSend(new SST::Interfaces::SimpleNetwork::Handler<
            InjectionCreditProbe, &InjectionCreditProbe::ready>(this));
        registerClock("1GHz", new SST::Clock::Handler<InjectionCreditProbe,
                       &InjectionCreditProbe::clock>(this));
        registerAsPrimaryComponent(); primaryComponentDoNotEndSim();
    }
    void init(unsigned phase) override { nic_->init(phase); }
    void setup() override { nic_->setup(); }
    void finish() override { nic_->finish(); }
private:
    bool send(unsigned lane) {
        SST::Mittens::PacketEvent::Metadata metadata;
        metadata.injectionLane = lane;
        auto* request = new SST::Interfaces::SimpleNetwork::Request(1,0,256,true,true,
            new SST::Mittens::PacketEvent(std::vector<std::uint32_t>(8,1),metadata));
        if (nic_->send(request,0)) return true;
        delete request; return false;
    }
    bool ready(int) {
        if (waiting_ && send(1)) {
            const auto cycle = getCurrentSimTime("1ns");
            if (cycle < 28) out_.fatal(CALL_INFO,-1,"migration before final credit\n");
            waiting_ = false; migrated_ = true;
        }
        return true;
    }
    void receive(SST::Event* event, int lane) {
        auto* flit = dynamic_cast<SST::Mittens::WormholeFlitEvent*>(event);
        if (!flit) out_.fatal(CALL_INFO,-1,"expected flit\n");
        ++received_[lane]; delete flit->takeRequest(); delete flit;
        if (lane==1) links_[1]->send(new SST::Mittens::WormholeCreditEvent());
    }
    bool clock(SST::Cycle_t cycle) {
        if (cycle==1 && !send(0)) out_.fatal(CALL_INFO,-1,"initial send failed\n");
        if (cycle==12) {
            if (received_[0]!=8 || send(1))
                out_.fatal(CALL_INFO,-1,"old flits must retain lane ownership\n");
            waiting_=true;
        }
        if (cycle==20) links_[0]->send(new SST::Mittens::WormholeCreditBurstEvent(8,1));
        if (cycle==60) {
            if (!migrated_ || received_[1]!=8)
                out_.fatal(CALL_INFO,-1,"deferred credits failed to wake empty NIC\n");
            out_.output("injection credit wakeup: PASS\n");
            primaryComponentOKToEndSim(); return true;
        }
        return false;
    }
    SST::Output out_{"injection-credit-test: ",0,0,SST::Output::STDOUT};
    SST::Interfaces::SimpleNetwork* nic_{};
    SST::Link* links_[2]{};
    unsigned received_[2]{};
    bool waiting_=false, migrated_=false;
};
