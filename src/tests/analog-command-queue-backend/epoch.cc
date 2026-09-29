#include <sst/core/sst_config.h>
#include <sst/core/component.h>
#include <sst/core/link.h>
#include "commands.h"
#include <map>
#include <set>
#include <cstring>
#include <iostream>
namespace TileComponents {
class EpochProbe final:public SST::Component {
public:
 SST_ELI_REGISTER_COMPONENT(EpochProbe,"tilecomponents","EpochProbe",SST_ELI_ELEMENT_VERSION(1,0,0),"deferred epoch admission",COMPONENT_CATEGORY_PROCESSOR)
 SST_ELI_DOCUMENT_PARAMS({"deferred","enable deferred transfers","false"})
 SST_ELI_DOCUMENT_PORTS({"commands","commands",{"TileComponents.ArrayCommand"}})
 EpochProbe(SST::ComponentId_t id,SST::Params& p):Component(id),deferred(p.find<bool>("deferred",false)){
  registerClock("1GHz",new SST::Clock::Handler<EpochProbe,&EpochProbe::tick>(this));
  link=configureLink("commands","1ns",new SST::Event::Handler<EpochProbe,&EpochProbe::response>(this));
  registerAsPrimaryComponent();primaryComponentDoNotEndSim();
 }
 void finish()override{check(done,"not done");std::cout<<"EPOCH_RESULT {\"passed\":true,\"deferred\":"<<deferred<<",\"captured\":"<<capture.size()<<"}\n";}
private:
 bool deferred,launched=false,done=false,busy=false,error=false;SST::Link*link;
 std::set<unsigned> accepted,complete;std::map<unsigned,std::uint64_t>capture,completed;
 void check(bool yes,const char*msg){if(!yes)fatal(CALL_INFO,-1,"epoch: %s\n",msg);}
 void send(unsigned token,unsigned offset,std::initializer_list<float>values){
  auto*c=new ArrayCommand;c->operation=Operation::Program;c->token=token;c->elementOffset=offset;c->elementCount=values.size();c->deferred=deferred;
  for(float f:values){std::uint32_t bits;std::memcpy(&bits,&f,4);for(unsigned b=0;b<4;++b)c->payload.push_back(bits>>(8*b));}link->send(c);
 }
 bool tick(SST::Cycle_t c){check(c<10000,"timeout");if(!launched){launched=true;send(1,0,{1});send(2,0,{1});send(3,1,{0});send(4,2,{0,1});send(5,0,{1});}return done;}
 void response(SST::Event*event){auto*r=dynamic_cast<ArrayCommand*>(event);check(r&&r->deferred==deferred,"bad reply");unsigned t=r->token;
  if(r->status==CommandStatus::Busy){check(t==5&&!busy,"bad Busy");busy=true;}
  else if(r->status==CommandStatus::Accepted||r->status==CommandStatus::Guaranteed){check(accepted.insert(t).second,"duplicate admission");check(r->status==(deferred?CommandStatus::Guaranteed:CommandStatus::Accepted),"bad guarantee");check(!deferred||t<=4,"late epoch Guaranteed");}
  else if(r->status==CommandStatus::Captured){check(deferred&&t<=4&&accepted.count(t)&&capture.emplace(t,r->cycle).second,"bad Captured");}
  else if(r->status==CommandStatus::Error){check(t==5&&busy&&!error,"unexpected Error");check(deferred?!accepted.count(t):accepted.count(t),"wrong error admission");error=true;}
  else if(r->status==CommandStatus::Complete){check(t<=4&&accepted.count(t)&&complete.insert(t).second,"bad completion");completed[t]=r->cycle;if(deferred)check(capture.count(t)&&r->cycle-capture[t]==(t==4?7:0),"capture/delay ordering");if(t==1){check(busy,"capacity not enforced");send(5,0,{1});}}
  else check(false,"bad status");
  if(complete.size()==4&&error){check(capture.size()==(deferred?4:0),"capture count");done=true;primaryComponentOKToEndSim();}delete r;
 }
};
class NonpipelineProbe final:public SST::Component {
public:
 SST_ELI_REGISTER_COMPONENT(NonpipelineProbe,"tilecomponents","NonpipelineProbe",SST_ELI_ELEMENT_VERSION(1,0,0),"deferred nonpipeline admission",COMPONENT_CATEGORY_PROCESSOR)
 SST_ELI_DOCUMENT_PARAMS({"deferred","enable deferred transfers","false"})
 SST_ELI_DOCUMENT_PORTS({"commands","commands",{"TileComponents.ArrayCommand"}})
 NonpipelineProbe(SST::ComponentId_t id,SST::Params&p):Component(id),deferred(p.find<bool>("deferred",false)){
  registerClock("1GHz",new SST::Clock::Handler<NonpipelineProbe,&NonpipelineProbe::tick>(this));
  link=configureLink("commands","1ns",new SST::Event::Handler<NonpipelineProbe,&NonpipelineProbe::response>(this));registerAsPrimaryComponent();primaryComponentDoNotEndSim();
 }
 void finish()override{check(done&&complete.size()==7&&error&&capture.size()==(deferred?3:0),"incomplete");std::cout<<"NONPIPELINE_RESULT {\"passed\":true,\"deferred\":"<<deferred<<"}\n";}
private:
 bool deferred,launched=false,done=false,error=false;SST::Link*link;std::set<unsigned>accepted,complete,capture;
 void check(bool yes,const char*msg){if(!yes)fatal(CALL_INFO,-1,"nonpipeline: %s\n",msg);}
 static std::vector<std::uint8_t>encode(std::initializer_list<float>values){std::vector<std::uint8_t>b;for(float f:values){std::uint32_t bits;std::memcpy(&bits,&f,4);for(unsigned j=0;j<4;++j)b.push_back(bits>>(8*j));}return b;}
 void send(unsigned token,Operation op,unsigned offset=0,unsigned count=0,std::initializer_list<float>values={}){auto*c=new ArrayCommand;c->operation=op;c->token=token;c->elementOffset=offset;c->elementCount=count;c->payload=encode(values);c->deferred=deferred&&op!=Operation::Execute;link->send(c);}
 bool tick(SST::Cycle_t c){check(c<10000,"timeout");if(!launched){launched=true;send(1,Operation::Program,0,4,{1,0,0,1});}return done;}
 void response(SST::Event*e){auto*r=dynamic_cast<ArrayCommand*>(e);check(r,"bad reply");unsigned t=r->token;
  if(r->status==CommandStatus::Accepted||r->status==CommandStatus::Guaranteed){check(accepted.insert(t).second,"duplicate admission");bool guaranteed=deferred&&t!=3&&t!=6&&t!=7;check(r->status==(guaranteed?CommandStatus::Guaranteed:CommandStatus::Accepted),"wrong guarantee");if(t==3)send(4,Operation::Store,0,1);if(t==4){send(5,Operation::Load,0,2,{3,4});send(6,Operation::Store,1,1);}if(t==7)send(8,Operation::Store,0,2);}
  else if(r->status==CommandStatus::Captured){check(deferred&&(t==1||t==2||t==5)&&accepted.count(t)&&capture.insert(t).second,"bad Captured");}
  else if(r->status==CommandStatus::Error){check(t==6&&accepted.count(6)&&!error,"bad Error");error=true;send(7,Operation::Execute);}
  else if(r->status==CommandStatus::Complete){check(accepted.count(t)&&complete.insert(t).second,"bad completion");if(t==1)send(2,Operation::Load,0,2,{1,2});if(t==2)send(3,Operation::Execute);if(t==4)check(r->payload==encode({1}),"wrong first output");if(t==8){check(r->payload==encode({3,4}),"wrong second output");done=true;primaryComponentOKToEndSim();}}
  else check(false,"bad status");
  delete r;
 }
};
}
namespace TileComponents {
class CaptureProbe final:public SST::Component {
public:
 SST_ELI_REGISTER_COMPONENT(CaptureProbe,"tilecomponents","CaptureProbe",SST_ELI_ELEMENT_VERSION(1,0,0),"timed input capture",COMPONENT_CATEGORY_PROCESSOR)
 SST_ELI_DOCUMENT_PARAMS({"deferred","enable deferred transfers","false"})
 SST_ELI_DOCUMENT_PORTS({"commands","commands",{"TileComponents.ArrayCommand"}})
 CaptureProbe(SST::ComponentId_t id,SST::Params&p):Component(id),deferred(p.find<bool>("deferred",false)){
  registerClock("1GHz",new SST::Clock::Handler<CaptureProbe,&CaptureProbe::tick>(this));link=configureLink("commands","1ns",new SST::Event::Handler<CaptureProbe,&CaptureProbe::response>(this));registerAsPrimaryComponent();primaryComponentDoNotEndSim();
 }
 void finish()override{check(done&&captured.size()==(deferred?3:0),"not done");std::cout<<"CAPTURE_RESULT {\"passed\":true,\"capture_1\":"<<captured[1]<<",\"capture_2\":"<<captured[2]<<"}\n";}
private:
 bool deferred,launched=false,done=false;SST::Link*link;std::set<unsigned>accepted,complete;std::map<unsigned,std::uint64_t>captured;
 void check(bool yes,const char*msg){if(!yes)fatal(CALL_INFO,-1,"capture: %s\n",msg);}
 void send(unsigned token,Operation op,unsigned array,unsigned count){auto*c=new ArrayCommand;c->token=token;c->operation=op;c->array=array;c->elementCount=count;c->deferred=deferred;if(op==Operation::Program)c->payload.assign(count*4,0);link->send(c);}
 bool tick(SST::Cycle_t c){check(c<10000,"timeout");if(!launched){launched=true;send(1,Operation::Program,0,64);send(2,Operation::Program,1,64);}return done;}
 void response(SST::Event*e){auto*r=dynamic_cast<ArrayCommand*>(e);check(r,"bad response");unsigned t=r->token;
  if(r->status==CommandStatus::Accepted||r->status==CommandStatus::Guaranteed){check(accepted.insert(t).second&&r->status==(deferred?CommandStatus::Guaranteed:CommandStatus::Accepted),"bad admission");}
  else if(r->status==CommandStatus::Captured){check(deferred&&t<=3&&accepted.count(t)&&captured.emplace(t,r->cycle).second,"bad capture");if(t<=2)check(r->cycle>=16,"instant capture");}
  else if(r->status==CommandStatus::Complete){check(accepted.count(t)&&complete.insert(t).second,"bad completion");if(deferred&&t<=3)check(captured.count(t)&&r->cycle-captured[t]==(t<=2?7:0),"capture/delay mismatch");if(t==1){send(3,Operation::Load,0,0);send(4,Operation::Store,0,0);}if(complete.size()==4){done=true;primaryComponentOKToEndSim();}}
  else check(false,"bad status");
  delete r;
 }
};
}
