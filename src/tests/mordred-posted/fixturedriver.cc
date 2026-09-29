#include <sst/core/sst_config.h>
#include <sst/core/component.h>
#include <sst/core/link.h>
#include "../../components/mordred/spmRequest.h"
#include <array>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

namespace TileComponents {
class MordredPostedTest final : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(MordredPostedTest,"tilecomponents","MordredPostedTest",
        SST_ELI_ELEMENT_VERSION(1,0,0),"Posted-write admission, commit and credit regression",COMPONENT_CATEGORY_PROCESSOR)
    SST_ELI_DOCUMENT_PARAMS({"sparse","Use sparse permitted bank stripes","0"},
        {"posted_enabled","Destination advertises posted slots","1"},
        {"arrival_connected","Destination has an arrival consumer","1"})
    SST_ELI_DOCUMENT_PORTS(
        {"requests0","Source requests",{"TileComponents.MordredSpmRequest"}},
        {"requests1","Destination local readback requests",{"TileComponents.MordredSpmRequest"}},
        {"arrivals0","Source arrival notifications",{"TileComponents.MordredSpmRequest"}},
        {"arrivals1","Destination arrival notifications",{"TileComponents.MordredSpmRequest"}})
    MordredPostedTest(SST::ComponentId_t id,SST::Params& p) : Component(id),
        sparse_(p.find<bool>("sparse",false)), enabled_(p.find<bool>("posted_enabled",true)),
        arrivals_(p.find<bool>("arrival_connected",true)) {
        clock_=registerClock("1GHz",new SST::Clock::Handler<MordredPostedTest,&MordredPostedTest::tick>(this));
        for (unsigned i=0;i<2;++i) {
            links_[i]=configureLink("requests"+std::to_string(i),clock_,
                new SST::Event::Handler<MordredPostedTest,&MordredPostedTest::response>(this));
            arrivalLinks_[i]=configureLink("arrivals"+std::to_string(i),clock_,
                new SST::Event::Handler<MordredPostedTest,&MordredPostedTest::response>(this));
            check(links_[i]!=nullptr,"missing requests port");
        }
        check((arrivalLinks_[1]!=nullptr)==arrivals_,"arrival fixture mismatch");
        const auto* output=std::getenv("TILE_COMPONENT_OUTPUT");check(output,"missing trace directory");
        trace_.open(std::string(output)+"/fixture.csv");check(bool(trace_),"cannot open fixture trace");
        trace_ << "event,cycle,id,label,source,destination,address,bytes,write,posted,arrival,status,data_hex\n";
        registerAsPrimaryComponent();primaryComponentDoNotEndSim();holding_=true;
    }
    void finish() override {
        check(done_ && pending_.empty() && accepted_.size()==(streaming()?13u:0u),"incomplete fixture");
        check(committed_.size()==accepted_.size(),"missing destination commit");
        check(checkedBytes_==(streaming()?13*bytes():0)+8,"incorrect readback count");
        check(busy_==(streaming()?1u:0u),"missing request-window Busy rejection");
        if (streaming()) check(releasedBeforeCommit_,"caller lifetime was not released with writes in flight");
        std::cout << "MORDRED_POSTED_TEST {\"passed\":true,\"accepted\":" << accepted_.size()
            << ",\"committed\":" << committed_.size() << ",\"busy\":" << busy_
            << ",\"rejections\":" << rejected_ << ",\"checked_bytes\":" << checkedBytes_
            << ",\"released_before_last_commit\":" << (releasedBeforeCommit_?"true":"false")
            << ",\"end_cycle\":" << now() << "}\n";
    }
private:
    using Packet=MordredSpmRequest;
    struct Expected { Packet packet; unsigned status; std::string label; std::vector<std::uint8_t> data; };
    SST::TimeConverter clock_;
    std::array<SST::Link*,2> links_{},arrivalLinks_{};
    bool sparse_,enabled_,arrivals_,holding_=false,done_=false,releasedBeforeCommit_=false;
    unsigned stage_=0,busy_=0,rejected_=0,checkedBytes_=0,nextRead_=0;
    bool legacyArrived_=false;
    std::map<std::uint64_t,Expected> pending_;
    std::map<std::uint64_t,std::uint64_t> accepted_,committed_;
    std::ofstream trace_;
    bool streaming() const { return enabled_ && arrivals_; }
    unsigned bytes() const { return sparse_?8:256; }
    unsigned address(unsigned index) const { return sparse_?0x108+16*index:0x100+256*index; }
    std::uint64_t now() const { return getCurrentSimTime(clock_); }
    void check(bool yes,const char* message) const { if (!yes) fatal(CALL_INFO,-1,"posted fixture stage%u: %s\n",stage_,message); }
    static std::vector<std::uint8_t> payload(unsigned index,unsigned size) {
        std::vector<std::uint8_t> result(size);
        for (unsigned j=0;j<size;++j) result[j]=(37*index+11*j+3)%251;
        return result;
    }
    void trace(const char* event,const Packet& p,const std::string& label) {
        std::ostringstream hex;
        for (auto byte:p.data) hex << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
        trace_ << event << ',' << now() << ',' << p.requestId << ',' << label << ',' << p.sourceTile << ','
            << p.destinationTile << ',' << p.address << ',' << p.bytes << ',' << p.write << ',' << p.posted << ','
            << p.arrival << ',' << p.status << ',' << hex.str() << '\n';
    }
    void issue(std::uint64_t id,unsigned source,unsigned target,unsigned offset,unsigned size,bool write,bool posted,
               unsigned status,const std::string& label,std::vector<std::uint8_t> data={},std::vector<std::uint8_t> expected={}) {
        check(!pending_.count(id),"duplicate live fixture ID");
        auto* p=new Packet;p->requestId=id;p->sourceTile=source;p->destinationTile=target;
        p->address=offset;p->bytes=size;p->write=write;p->posted=posted;p->data=std::move(data);
        pending_.emplace(id,Expected{*p,status,label,std::move(expected)});
        trace("request",*p,label);links_[source]->send(p);
    }
    void posted(unsigned index,unsigned expected=Packet::Accepted) {
        issue(100+index,0,1,address(index),bytes(),true,true,expected,"posted",payload(index,bytes()));
    }
    bool tick(SST::Cycle_t) {
        check(now()<100000,"progress timeout");
        if (done_) return true;
        if (stage_==0) {
            issue(1,0,1,0xf08,8,true,false,Packet::Success,"legacy_write",payload(31,8));++stage_;
        } else if (stage_==1 && pending_.empty() && (!arrivals_ || legacyArrived_)) {
            issue(2,0,1,address(0),bytes(),false,true,Packet::Malformed,"posted_read");
            issue(3,0,0,address(0),bytes(),true,true,Packet::Malformed,"posted_self",payload(0,bytes()));
            issue(4,0,1,address(0),bytes(),true,true,Packet::Malformed,"short_payload",{});
            issue(0,0,1,address(0),bytes(),true,true,Packet::Malformed,"zero_id",payload(0,bytes()));
            if (streaming()) {
                issue(5,0,1,4094,4,true,true,Packet::OutOfRange,"range",payload(0,4));
                if (sparse_) {
                    issue(6,0,1,0x100,4,true,true,Packet::ForbiddenBank,"forbidden",payload(0,4));
                    issue(7,0,1,0x10c,8,true,true,Packet::ForbiddenBank,"forbidden_tail",payload(0,8));
                }
            } else issue(7,0,1,address(0),bytes(),true,true,Packet::Malformed,"unsupported_peer",payload(0,bytes()));
            ++stage_;
        } else if (stage_==2 && pending_.empty()) {
            if (!streaming()) { stage_=7;return false; }
            for (unsigned i=0;i<5;++i) posted(i,i==4?Packet::Busy:Packet::Accepted);
            ++stage_;
        } else if (stage_==3 && pending_.empty()) { posted(4);++stage_; }
        else if (stage_==4 && pending_.empty()) { for (unsigned i=5;i<9;++i) posted(i);++stage_; }
        else if (stage_==5 && pending_.empty()) { for (unsigned i=9;i<13;++i) posted(i);++stage_; }
        else if (stage_==6 && pending_.empty()) {
            check(accepted_.size()==13 && committed_.size()<13,"expected final accepted write still in flight");
            releasedBeforeCommit_=true;primaryComponentOKToEndSim();holding_=false;++stage_;
        } else if (stage_==7 && committed_.size()==accepted_.size()) {
            if (!holding_) { primaryComponentDoNotEndSim();holding_=true; }
            issue(2000,0,1,0xf08,8,false,false,Packet::Success,"legacy_read",{},payload(31,8));
            ++stage_;
        } else if (stage_==8 && pending_.empty()) {
            if (streaming() && nextRead_<13) {
                const auto index=nextRead_++;
                issue(1000+index,1,1,address(index),bytes(),false,false,Packet::Success,"committed_read",{},payload(index,bytes()));
            } else if (streaming()) { posted(12,Packet::Malformed);++stage_; }
            else ++stage_;
        } else if (stage_==9 && pending_.empty()) {
            done_=true;primaryComponentOKToEndSim();holding_=false;return true;
        }
        return false;
    }
    void response(SST::Event* event) {
        auto* p=dynamic_cast<Packet*>(event);check(p && p->response,"invalid response");
        if (p->arrival) {
            check(arrivals_ && p->sourceTile==0 && p->destinationTile==1 && p->write && p->status==Packet::Success && p->data.empty(),"bad arrival");
            if (p->posted) {
                check(p->requestId>=100 && p->requestId<113 && accepted_.count(p->requestId) && !committed_.count(p->requestId),"unexpected posted arrival");
                const auto index=p->requestId-100;
                check(p->address==address(index) && p->bytes==bytes() && now()>accepted_.at(p->requestId),"arrival identity or early commit");
                committed_[p->requestId]=now();
            } else { check(p->requestId==1 && p->address==0xf08 && p->bytes==8 && !legacyArrived_,"legacy arrival mismatch");legacyArrived_=true; }
            trace("arrival",*p,p->posted?"posted":"legacy_write");
        } else {
            check(pending_.count(p->requestId),"duplicate/unexpected source response");
            const auto expected=pending_.at(p->requestId);const auto& original=expected.packet;
            check(p->sourceTile==original.sourceTile && p->destinationTile==original.destinationTile &&
                p->address==original.address && p->bytes==original.bytes && p->write==original.write &&
                p->posted==original.posted && p->status==expected.status && p->data==expected.data,"response identity/status/payload");
            if (p->status==Packet::Accepted) { check(!accepted_.count(p->requestId),"duplicate acceptance");accepted_[p->requestId]=now(); }
            else if (p->status!=Packet::Success) { ++rejected_;busy_+=p->status==Packet::Busy; }
            checkedBytes_+=p->data.size();trace("response",*p,expected.label);pending_.erase(p->requestId);
        }
        delete p;
    }
};
}
