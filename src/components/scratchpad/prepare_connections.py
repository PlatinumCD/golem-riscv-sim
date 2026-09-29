"""Add passive observations to a build-local copy of the pinned SST Bus."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]

def replace(text, before, after):
    assert text.count(before) == 1, before
    return text.replace(before, after)


def connections(output):
    upstream = ROOT / 'third_party/sst-elements/src/sst/elements/memHierarchy'
    header = (upstream / 'bus.h').read_text()
    source = (upstream / 'bus.cc').read_text()
    header = replace(header, '#include <queue>', '#include <queue>\n#include "cycleProfile.h"')
    header = replace(header, 'private:', '''private:
    TileComponents::CycleProfile profile_;
    void profileEvent(SST::Event*, const char*);
public:
    void finish() override { profile_.flush(); }
private:''')
    source = replace(source, '    configureParameters(params);',
                     '    profile_.open(getName());\n    configureParameters(params);')
    source = replace(source, '    eventQueue_.push(ev);', '''    eventQueue_.push(ev);
    profileEvent(ev, "enqueue");
    profile_.record(getCurrentSimTimeNano(),"state","pending_messages",0,eventQueue_.size());''')
    source = replace(source, '        SST::Event* event = eventQueue_.front();',
                     '        SST::Event* event = eventQueue_.front();\n        profileEvent(event, "forward");')
    source = replace(source, '        eventQueue_.pop();', '''        eventQueue_.pop();
        profile_.record(getCurrentSimTimeNano(),"state","pending_messages",0,eventQueue_.size());''')
    source += '''
void SST::MemHierarchy::Bus::profileEvent(SST::Event* ev, const char* kind) {
    if (!profile_) return;
    auto* event=static_cast<MemEventBase*>(ev);
    const auto& source=event->getSrc();
    const char* resource=source.find("router_spm")!=std::string::npos ? "router_message" :
        source.find("riscv")!=std::string::npos ? "cpu_message" : "response_message";
    profile_.record(getCurrentSimTimeNano(),kind,resource,0,
        static_cast<int>(event->getCmd()),event->getID().first,event->getID().second);
}
'''
    header = re.sub(r'\bBus\b', 'ProfiledSpmConnections', header)
    source = re.sub(r'\bBus\b', 'ProfiledSpmConnections', source)
    header = replace(header, 'ProfiledSpmConnections, "memHierarchy", "ProfiledSpmConnections",',
                     'ProfiledSpmConnections, "tilecomponents", "ProfiledSpmConnections",')
    header = header.replace('SST_MEMHIERARCHY_BUS_H', 'TILE_PROFILED_SPM_CONNECTIONS_H')
    source = replace(source, '#include "bus.h"', '#include "profiledBus.h"')
    output.mkdir(parents=True, exist_ok=True)
    # The generic component build includes the scratchpad directory.
    header = header.replace('#include "cycleProfile.h"', '#include "../cycleProfile.h"')
    (output / 'profiledBus.h').write_text(header)
    (output / 'profiledBus.cc').write_text(source)
    return output / 'profiledBus.cc', [upstream / 'bus.h', upstream / 'bus.cc']
