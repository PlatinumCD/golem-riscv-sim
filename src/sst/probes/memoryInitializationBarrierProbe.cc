#include "sst_config.h"

#include "memoryInitializationBarrierProbe.h"

namespace SST {
namespace Mittens {

MemoryInitializationBarrierProbe::MemoryInitializationBarrierProbe(
    SST::ComponentId_t id, SST::Params& params) :
    SST::Component(id),
    output_(
        "mittens-memory-init-probe: ",
        params.find<int>("verbose", 0),
        0,
        SST::Output::STDOUT),
    tileId_(params.find<std::uint32_t>("tile_id", 0)),
    arrivalDelay_(params.find<std::uint64_t>("arrival_delay", 1)),
    invalidMode_(params.find<std::string>("invalid_mode", "none"))
{
    if (arrivalDelay_ == 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "memory initialization barrier probe requires a positive delay\n");
    }
    if (invalidMode_ != "none" && invalidMode_ != "duplicate") {
        output_.fatal(
            CALL_INFO,
            -1,
            "invalid memory initialization barrier probe mode: %s\n",
            invalidMode_.c_str());
    }

    barrierLink_ = configureLink(
        "barrier",
        new SST::Event::Handler<
            MemoryInitializationBarrierProbe,
            &MemoryInitializationBarrierProbe::handleEvent>(this));
    if (barrierLink_ == nullptr) {
        output_.fatal(
            CALL_INFO,
            -1,
            "memory initialization barrier probe tile %u has no controller link\n",
            static_cast<unsigned>(tileId_));
    }

    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
    clockTimeBase_ = registerClock(
        params.find<std::string>("clock", "1GHz"),
        new SST::Clock::Handler<
            MemoryInitializationBarrierProbe,
            &MemoryInitializationBarrierProbe::clockTick>(this));
}

bool MemoryInitializationBarrierProbe::clockTick(SST::Cycle_t cycle)
{
    if (!arrivalSent_ && cycle >= arrivalDelay_) {
        arrivalCycle_ = static_cast<std::uint64_t>(cycle);
        barrierLink_->send(new MemoryInitializationBarrierEvent(
            tileId_, MemoryInitializationBarrierMessage::Arrive));
        if (invalidMode_ == "duplicate") {
            barrierLink_->send(new MemoryInitializationBarrierEvent(
                tileId_, MemoryInitializationBarrierMessage::Arrive));
        }
        arrivalSent_ = true;
    }
    return released_;
}

void MemoryInitializationBarrierProbe::handleEvent(SST::Event* rawEvent)
{
    auto* event =
        dynamic_cast<MemoryInitializationBarrierEvent*>(rawEvent);
    if (event == nullptr) {
        delete rawEvent;
        output_.fatal(
            CALL_INFO,
            -1,
            "memory initialization barrier probe received the wrong event type\n");
    }

    const std::uint32_t tile = event->tileId();
    const MemoryInitializationBarrierMessage message = event->message();
    delete event;
    if (!arrivalSent_ || released_ || tile != tileId_ ||
        message != MemoryInitializationBarrierMessage::Release) {
        output_.fatal(
            CALL_INFO,
            -1,
            "memory initialization barrier probe received an invalid release: expected_tile=%u tile=%u arrival_sent=%u released=%u message=%u\n",
            static_cast<unsigned>(tileId_),
            static_cast<unsigned>(tile),
            arrivalSent_ ? 1U : 0U,
            released_ ? 1U : 0U,
            static_cast<unsigned>(message));
    }

    const RankInfo rank = getRank();
    const std::uint64_t releaseCycle = static_cast<std::uint64_t>(
        getNextClockCycle(clockTimeBase_) - 1U);
    output_.output(
        "MITTENS_MEMORY_INIT_BARRIER_PROBE tile=%u arrival_cycle=%llu release_cycle=%llu sst_rank=%u sst_thread=%u\n",
        static_cast<unsigned>(tileId_),
        static_cast<unsigned long long>(arrivalCycle_),
        static_cast<unsigned long long>(releaseCycle),
        static_cast<unsigned>(rank.rank),
        static_cast<unsigned>(rank.thread));
    released_ = true;
    primaryComponentOKToEndSim();
}

} // namespace Mittens
} // namespace SST
