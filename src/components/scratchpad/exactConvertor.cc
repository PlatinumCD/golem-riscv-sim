#include <sst/core/sst_config.h>
#include <sst/elements/memHierarchy/membackend/simpleMemScratchBackendConvertor.h>
#include <sst/elements/memHierarchy/membackend/memBackend.h>

namespace TileComponents {
// Preserve actual addresses and tail sizes; the upstream converter rounds to
// backend width and uses line base addresses. Requests are bounded by one line.
class ExactConvertor final : public SST::MemHierarchy::SimpleMemScratchBackendConvertor {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(ExactConvertor, "tilecomponents", "ExactConvertor",
        SST_ELI_ELEMENT_VERSION(1,0,0), "Exact-size local scratchpad requests",
        SST::MemHierarchy::ScratchBackendConvertor)
    ExactConvertor(SST::ComponentId_t id, SST::Params& p)
        : SimpleMemScratchBackendConvertor(id, p) {
        const auto width = p.find<unsigned>("request_width", 64);
        if (!width || m_backendRequestWidth != width ||
            static_cast<SST::MemHierarchy::SimpleMemBackend*>(m_backend)->getRequestWidth() != width)
            fatal(CALL_INFO, -1, "SPM converter and backend request sizes must agree\n");
    }
    bool issue(MemReq* request) override {
        const auto* event = request->getMemEvent();
        if (!event->getSize() || event->getSize() > m_backendRequestWidth || request->processed() ||
            event->getAddr() % m_backendRequestWidth + event->getSize() > m_backendRequestWidth)
            fatal(CALL_INFO, -1, "request must fit one configured SPM line\n");
        return static_cast<SST::MemHierarchy::SimpleMemBackend*>(m_backend)->issueRequest(
            request->id(), event->getAddr(), request->isWrite(), event->getSize());
    }
};
}
