#include <sst/core/sst_config.h>
#include <sst/core/component.h>
#include <sst/core/link.h>
#include "../../components/mordred/spmRequest.h"
#include <iostream>
#include <map>
#include <vector>

namespace TileComponents {
class MordredLocalTest final : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(MordredLocalTest,"tilecomponents","MordredLocalTest",
        SST_ELI_ELEMENT_VERSION(1,0,0),"Explicit local router-bank request regression",COMPONENT_CATEGORY_PROCESSOR)
    SST_ELI_DOCUMENT_PORTS({"requests","Explicit local transactions",{"TileComponents.MordredSpmRequest"}})
    MordredLocalTest(SST::ComponentId_t id,SST::Params&) : Component(id) {
        clock_=registerClock("1GHz",new SST::Clock::Handler<MordredLocalTest,&MordredLocalTest::tick>(this));
        requests_=configureLink("requests",clock_,new SST::Event::Handler<MordredLocalTest,&MordredLocalTest::response>(this));
        if (!requests_) fatal(CALL_INFO,-1,"missing local test requests port\n");
        registerAsPrimaryComponent(); primaryComponentDoNotEndSim();
    }
    void finish() override {
        if (!done_ || !pending_.empty() || responded_!=18 || checkedBytes_!=64 || errors_!=6)
            fatal(CALL_INFO,-1,"incomplete local router-bank test\n");
        std::cout << "MORDRED_LOCAL_TEST {\"passed\":true,\"responses\":" << responded_
            << ",\"checked_bytes\":" << checkedBytes_ << ",\"expected_errors\":" << errors_ << "}\n";
    }
private:
    struct Expected { unsigned status; std::uint64_t address; unsigned bytes; bool write; std::vector<std::uint8_t> data; };
    SST::TimeConverter clock_;
    SST::Link* requests_=nullptr;
    std::map<std::uint64_t,Expected> pending_;
    unsigned phase_=0, next_=0, responded_=0, checkedBytes_=0, errors_=0;
    bool done_=false;
    static std::vector<std::uint8_t> pattern(unsigned index) {
        std::vector<std::uint8_t> bytes(8);
        for (unsigned i=0;i<8;++i) bytes[i]=0x40+index*8+i;
        return bytes;
    }
    void issue(std::uint64_t address,unsigned bytes,bool write,unsigned status=0,
               std::vector<std::uint8_t> data={},std::vector<std::uint8_t> expected={}) {
        auto* request=new MordredSpmRequest;
        request->requestId=++next_; request->destinationTile=0; request->address=address;
        request->bytes=bytes; request->write=write; request->data=std::move(data);
        pending_.emplace(next_,Expected{status,address,bytes,write,std::move(expected)});
        requests_->send(request);
    }
    bool tick(SST::Cycle_t cycle) {
        if (cycle>10000) fatal(CALL_INFO,-1,"local request test progress timeout\n");
        if (!pending_.empty()) return false;
        if (phase_==0) {
            for (unsigned i=0;i<4;++i) issue(0x108+16*i,8,true,0,pattern(i));
            issue(0x148,8,true,MordredSpmRequest::Busy,std::vector<std::uint8_t>(8,0xa5));
        } else if (phase_==1 || phase_==3) {
            for (unsigned i=0;i<4;++i) issue(0x108+16*i,8,false,0,{},pattern(i));
        } else if (phase_==2) {
            issue(0x100,4,true,MordredSpmRequest::ForbiddenBank,std::vector<std::uint8_t>(4,0xa5));
            // Starts in permitted bank 3, then touches forbidden bank 0. The
            // permitted prefix must remain unchanged: validation precedes work.
            issue(0x10c,8,true,MordredSpmRequest::ForbiddenBank,std::vector<std::uint8_t>(8,0xa5));
            issue(4094,4,true,MordredSpmRequest::OutOfRange,std::vector<std::uint8_t>(4,0xa5));
            issue(4095,4,false,MordredSpmRequest::OutOfRange);
            issue(0x108,0,false,MordredSpmRequest::Malformed);
        } else {
            done_=true; primaryComponentOKToEndSim(); return true;
        }
        ++phase_; return false;
    }
    void response(SST::Event* event) {
        auto* value=dynamic_cast<MordredSpmRequest*>(event);
        if (!value || !value->response || !pending_.count(value->requestId))
            fatal(CALL_INFO,-1,"invalid or duplicate local response\n");
        const auto expected=pending_.at(value->requestId);
        if (value->sourceTile || value->destinationTile || value->address!=expected.address ||
            value->bytes!=expected.bytes || value->write!=expected.write || value->status!=expected.status ||
            value->data!=expected.data) fatal(CALL_INFO,-1,"local response identity/status/payload mismatch\n");
        ++responded_; checkedBytes_+=value->data.size(); errors_+=value->status!=0;
        pending_.erase(value->requestId); delete value;
    }
};
}
