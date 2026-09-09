#include "sst_config.h"

#include "epochBarrierProbe.h"

#include <algorithm>
#include <string>

namespace SST {
namespace Mittens {

EpochBarrierProbe::EpochBarrierProbe(
    SST::ComponentId_t id, SST::Params& params) :
    SST::Component(id),
    output_(
        "mittens-epoch-probe: ",
        params.find<int>("verbose", 0),
        0,
        SST::Output::STDOUT),
    tileId_(params.find<std::uint32_t>("tile_id", 0)),
    epochCount_(params.find<std::uint32_t>("epoch_count", 1)),
    invalidMode_(params.find<std::string>("invalid_mode", "none"))
{
    params.find_array("arrival_delays", arrivalDelays_);
    params.find_array("idle_epochs", idleEpochs_);
    if (epochCount_ == 0 || arrivalDelays_.size() != epochCount_ ||
        std::any_of(
            arrivalDelays_.begin(), arrivalDelays_.end(),
            [](std::uint64_t delay) { return delay == 0; })) {
        output_.fatal(
            CALL_INFO,
            -1,
            "invalid epoch barrier probe schedule: tile=%u epochs=%u delays=%zu\n",
            static_cast<unsigned>(tileId_),
            static_cast<unsigned>(epochCount_),
            arrivalDelays_.size());
    }
    std::sort(idleEpochs_.begin(), idleEpochs_.end());
    if (std::adjacent_find(idleEpochs_.begin(), idleEpochs_.end()) !=
            idleEpochs_.end() ||
        (!idleEpochs_.empty() && idleEpochs_.back() >= epochCount_)) {
        output_.fatal(
            CALL_INFO,
            -1,
            "invalid idle epoch list for tile=%u epoch_count=%u\n",
            static_cast<unsigned>(tileId_),
            static_cast<unsigned>(epochCount_));
    }
    if (invalidMode_ != "none" && invalidMode_ != "duplicate" &&
        invalidMode_ != "future" && invalidMode_ != "stale") {
        output_.fatal(
            CALL_INFO,
            -1,
            "invalid epoch barrier probe mode: %s\n",
            invalidMode_.c_str());
    }
    if (invalidMode_ == "stale" && epochCount_ < 2) {
        output_.fatal(
            CALL_INFO, -1,
            "stale epoch barrier probe requires at least two epochs\n");
    }

    barrierLink_ = configureLink(
        "barrier",
        new SST::Event::Handler<
            EpochBarrierProbe,
            &EpochBarrierProbe::handleEvent>(this));
    if (barrierLink_ == nullptr) {
        output_.fatal(
            CALL_INFO, -1,
            "epoch barrier probe tile=%u has no controller link\n",
            static_cast<unsigned>(tileId_));
    }

    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
    clockTimeBase_ = registerClock(
        params.find<std::string>("clock", "1GHz"),
        new SST::Clock::Handler<
            EpochBarrierProbe,
            &EpochBarrierProbe::clockTick>(this));
    nextArrivalCycle_ = arrivalDelays_.front();
}

bool EpochBarrierProbe::idleEpoch(std::uint32_t epoch) const
{
    return std::binary_search(idleEpochs_.begin(), idleEpochs_.end(), epoch);
}

bool EpochBarrierProbe::clockTick(SST::Cycle_t cycle)
{
    const std::uint64_t now = static_cast<std::uint64_t>(cycle);
    if (!finished_ && !arrivalSent_ && now >= nextArrivalCycle_) {
        sendArrival(now);
    }
    return finished_;
}

void EpochBarrierProbe::sendArrival(std::uint64_t cycle)
{
    const EpochBarrierContribution contribution = idleEpoch(currentEpoch_)
        ? EpochBarrierContribution::Idle
        : EpochBarrierContribution::WorkComplete;
    std::uint32_t transmittedEpoch = currentEpoch_;
    if (invalidMode_ == "future" && currentEpoch_ == 0) {
        transmittedEpoch = 1;
    } else if (invalidMode_ == "stale" && currentEpoch_ == 1) {
        transmittedEpoch = 0;
    }

    barrierLink_->send(new EpochBarrierEvent(
        tileId_,
        transmittedEpoch,
        EpochBarrierMessage::Arrive,
        contribution));
    if (invalidMode_ == "duplicate" && currentEpoch_ == 0) {
        barrierLink_->send(new EpochBarrierEvent(
            tileId_,
            transmittedEpoch,
            EpochBarrierMessage::Arrive,
            contribution));
    }
    arrivalCycle_ = cycle;
    arrivalSent_ = true;
}

void EpochBarrierProbe::handleEvent(SST::Event* rawEvent)
{
    auto* event = dynamic_cast<EpochBarrierEvent*>(rawEvent);
    if (event == nullptr) {
        delete rawEvent;
        output_.fatal(
            CALL_INFO, -1,
            "epoch barrier probe received an event with the wrong type\n");
    }

    const std::uint32_t tile = event->tileId();
    const std::uint32_t completedEpoch = event->completedEpoch();
    const std::uint32_t releasedEpoch = event->releasedEpoch();
    const EpochBarrierMessage message = event->message();
    const EpochBarrierContribution contribution = event->contribution();
    delete event;

    const bool prefixStop = message == EpochBarrierMessage::PrefixStop;
    if ((message != EpochBarrierMessage::Release && !prefixStop) ||
        contribution != EpochBarrierContribution::None ||
        tile != tileId_ || !arrivalSent_ ||
        completedEpoch != currentEpoch_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "epoch barrier probe received invalid release: tile=%u expected_tile=%u completed_epoch=%u expected_epoch=%u message=%u contribution=%u arrival_sent=%u\n",
            static_cast<unsigned>(tile),
            static_cast<unsigned>(tileId_),
            static_cast<unsigned>(completedEpoch),
            static_cast<unsigned>(currentEpoch_),
            static_cast<unsigned>(message),
            static_cast<unsigned>(contribution),
            arrivalSent_ ? 1U : 0U);
    }

    const std::uint64_t releaseCycle = static_cast<std::uint64_t>(
        getNextClockCycle(clockTimeBase_) - 1U);
    output_.output(
        "MITTENS_EPOCH_BARRIER_PROBE tile=%u completed_epoch=%u released_epoch=%u contribution=%s arrival_cycle=%llu release_cycle=%llu sst_rank=%u sst_thread=%u\n",
        static_cast<unsigned>(tileId_),
        static_cast<unsigned>(completedEpoch),
        static_cast<unsigned>(releasedEpoch),
        idleEpoch(currentEpoch_) ? "idle" : "work",
        static_cast<unsigned long long>(arrivalCycle_),
        static_cast<unsigned long long>(releaseCycle),
        static_cast<unsigned>(getRank().rank),
        static_cast<unsigned>(getRank().thread));

    ++currentEpoch_;
    arrivalSent_ = false;
    arrivalCycle_ = 0;
    if (prefixStop) {
        finished_ = true;
        primaryComponentOKToEndSim();
        return;
    }
    if (currentEpoch_ == epochCount_) {
        finished_ = true;
        primaryComponentOKToEndSim();
        return;
    }
    nextArrivalCycle_ = releaseCycle + arrivalDelays_[currentEpoch_];
}

} // namespace Mittens
} // namespace SST
