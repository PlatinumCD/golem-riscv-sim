#pragma once
#include "../mordred/spmEndpoint.h"

namespace TileComponents {
// DRAM-backed NIU endpoint. The Python tile composition adds a control CPU,
// control SPM and an SST DRAM controller; no analog array is needed here.
class DramTile final : public MordredSpmEndpoint {
public:
    SST_ELI_REGISTER_COMPONENT(DramTile, "tilecomponents", "DramTile",
        SST_ELI_ELEMENT_VERSION(1,0,0), "DRAM Tile: guest-controlled whole-message weight distribution", COMPONENT_CATEGORY_MEMORY)
    static const std::vector<SST::ElementInfoParam>& ELI_getParams() {
        static const auto parameters=[] {
            auto result=MordredSpmEndpoint::ELI_getParams();
            result.insert(result.end(), {
                {"dram_base", "Guest descriptor payload address base (not CPU-mapped)", "4294967296"},
                {"dram_capacity_bytes", "DRAM payload address extent", "67108864"},
                {"dram_request_bytes", "DRAM request boundary and maximum read size", "64"}});
            return result;
        }();
        return parameters;
    }
    static const std::vector<SST::ElementInfoSubComponentSlot>& ELI_getSubComponentSlots() {
        static const auto slots=[] {
            auto result=MordredSpmEndpoint::ELI_getSubComponentSlots();
            result.push_back({"dram_memory", "Payload reads through a timed DRAM controller", "SST::Interfaces::StandardMem"});
            return result;
        }();
        return slots;
    }
    DramTile(SST::ComponentId_t, SST::Params&);
    void init(unsigned) override;
    void setup() override;
    void complete(unsigned) override;
    void finish() override;
protected:
    std::uint64_t sourceBase() const override { return base_; }
    bool payloadUsesSpm() const override { return false; }
    int sourceRange(std::uint64_t, std::uint64_t) const override;
    std::uint32_t memoryBoundary(const Packet&) const override;
    void sendMemory(const Packet&, Memory::Request*) override;
private:
    Memory* dram_=nullptr;
    std::uint64_t base_, capacity_;
    std::uint32_t requestBytes_;
    struct Read { std::uint64_t address, bytes, cycle; };
    std::map<Memory::Request::id_t, Read> reads_;
    std::uint64_t requests_=0, bytes_=0, peak_=0, latency_=0, busy_=0, busyStart_=0;
    std::ofstream trace_;
    CycleProfile profile_;
    void response(Memory::Request*);
    void record(const char*, Memory::Request::id_t, const Read&);
};
}
