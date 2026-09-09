#include "sst_config.h"

#include "epochBarrierController.h"

#include <algorithm>
#include <limits>
#include <string>

namespace SST {
namespace Mittens {

EpochBarrierController::EpochBarrierController(
    SST::ComponentId_t id, SST::Params& params) :
    SST::Component(id),
    output_(
        "mittens-epoch-barrier: ",
        params.find<int>("verbose", 0),
        0,
        SST::Output::STDOUT),
    tileCount_(params.find<std::uint32_t>("tile_count", 1)),
    epochCount_(params.find<std::uint32_t>("epoch_count", 1)),
    releaseCycles_(params.find<std::uint64_t>("release_cycles", 1)),
    stopAfterReleases_(
        params.find<std::uint32_t>("stop_after_releases", 0))
{
    if (tileCount_ == 0 || epochCount_ == 0 || releaseCycles_ == 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "invalid epoch barrier configuration: tiles=%u epochs=%u release_cycles=%llu\n",
            static_cast<unsigned>(tileCount_),
            static_cast<unsigned>(epochCount_),
            static_cast<unsigned long long>(releaseCycles_));
    }
    if (stopAfterReleases_ > epochCount_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "epoch barrier prefix stop exceeds epoch count: stop_after_releases=%u epoch_count=%u\n",
            static_cast<unsigned>(stopAfterReleases_),
            static_cast<unsigned>(epochCount_));
    }
    if (stopAfterReleases_ != 0) {
        registerAsPrimaryComponent();
        primaryComponentDoNotEndSim();
    }

    std::vector<std::uint32_t> activeTileIds;
    params.find_array("active_tiles", activeTileIds);
    if (activeTileIds.empty()) {
        activeTileIds.resize(tileCount_);
        for (std::uint32_t tile = 0; tile < tileCount_; ++tile) {
            activeTileIds[tile] = tile;
        }
    }
    std::sort(activeTileIds.begin(), activeTileIds.end());
    if (std::adjacent_find(
            activeTileIds.begin(), activeTileIds.end()) !=
        activeTileIds.end()) {
        output_.fatal(
            CALL_INFO, -1,
            "epoch barrier active tile list contains a duplicate\n");
    }

    links_.resize(tileCount_, nullptr);
    activeTiles_.resize(tileCount_, false);
    arrivedTiles_.resize(tileCount_, false);
    for (const std::uint32_t tile : activeTileIds) {
        if (tile >= tileCount_) {
            output_.fatal(
                CALL_INFO,
                -1,
                "epoch barrier active tile %u is outside tile_count=%u\n",
                static_cast<unsigned>(tile),
                static_cast<unsigned>(tileCount_));
        }
        activeTiles_[tile] = true;
        ++activeTileCount_;
    }
    if (activeTileCount_ == 0) {
        output_.fatal(
            CALL_INFO, -1,
            "epoch barrier requires at least one active tile\n");
    }

    for (std::uint32_t tile = 0; tile < tileCount_; ++tile) {
        const std::string port = "barrier" + std::to_string(tile);
        links_[tile] = configureLink(
            port,
            new SST::Event::Handler<
                EpochBarrierController,
                &EpochBarrierController::handleEvent>(this));
    }

    arrivalStatistic_ = registerStatistic<std::uint64_t>("arrivals");
    idleArrivalStatistic_ =
        registerStatistic<std::uint64_t>("idle_arrivals");
    releaseStatistic_ = registerStatistic<std::uint64_t>("releases");
    waitStatistic_ =
        registerStatistic<std::uint64_t>("barrier_wait_cycles");

    clockTimeBase_ = getTimeConverter(
        params.find<std::string>("clock", "1GHz"));
    clockHandler_ = new SST::Clock::Handler<
        EpochBarrierController,
        &EpochBarrierController::clockTick>(this);
    registerClock(clockTimeBase_, clockHandler_);
    clockRegistered_ = true;
}

void EpochBarrierController::setup()
{
    for (std::uint32_t tile = 0; tile < tileCount_; ++tile) {
        if (activeTiles_[tile] && links_[tile] == nullptr) {
            output_.fatal(
                CALL_INFO,
                -1,
                "epoch barrier active tile %u has no controller link\n",
                static_cast<unsigned>(tile));
        }
        if (!activeTiles_[tile] && links_[tile] != nullptr) {
            output_.fatal(
                CALL_INFO,
                -1,
                "epoch barrier inactive tile %u has an unexpected controller link\n",
                static_cast<unsigned>(tile));
        }
    }
}

std::uint64_t EpochBarrierController::currentClockCycle()
{
    return static_cast<std::uint64_t>(
        getNextClockCycle(clockTimeBase_) - 1U);
}

void EpochBarrierController::handleEvent(SST::Event* rawEvent)
{
    auto* event = dynamic_cast<EpochBarrierEvent*>(rawEvent);
    if (event == nullptr) {
        delete rawEvent;
        output_.fatal(
            CALL_INFO, -1,
            "epoch barrier received an event with the wrong type\n");
    }

    const std::uint32_t tile = event->tileId();
    const std::uint32_t epoch = event->completedEpoch();
    const EpochBarrierMessage message = event->message();
    const EpochBarrierContribution contribution = event->contribution();
    delete event;

    if (message != EpochBarrierMessage::Arrive) {
        output_.fatal(
            CALL_INFO,
            -1,
            "epoch barrier controller received non-arrival: tile=%u epoch=%u message=%u\n",
            static_cast<unsigned>(tile),
            static_cast<unsigned>(epoch),
            static_cast<unsigned>(message));
    }
    if (tile >= tileCount_ || !activeTiles_[tile]) {
        output_.fatal(
            CALL_INFO,
            -1,
            "epoch barrier arrival from inactive tile: tile=%u epoch=%u current_epoch=%u\n",
            static_cast<unsigned>(tile),
            static_cast<unsigned>(epoch),
            static_cast<unsigned>(currentEpoch_));
    }
    if (finished_ || currentEpoch_ >= epochCount_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "epoch barrier arrival after final release: tile=%u epoch=%u epoch_count=%u\n",
            static_cast<unsigned>(tile),
            static_cast<unsigned>(epoch),
            static_cast<unsigned>(epochCount_));
    }
    if (epoch < currentEpoch_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "epoch barrier stale arrival: tile=%u epoch=%u current_epoch=%u\n",
            static_cast<unsigned>(tile),
            static_cast<unsigned>(epoch),
            static_cast<unsigned>(currentEpoch_));
    }
    if (epoch > currentEpoch_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "epoch barrier future arrival: tile=%u epoch=%u current_epoch=%u\n",
            static_cast<unsigned>(tile),
            static_cast<unsigned>(epoch),
            static_cast<unsigned>(currentEpoch_));
    }
    if (arrivedTiles_[tile]) {
        output_.fatal(
            CALL_INFO,
            -1,
            "epoch barrier duplicate arrival: tile=%u epoch=%u current_epoch=%u\n",
            static_cast<unsigned>(tile),
            static_cast<unsigned>(epoch),
            static_cast<unsigned>(currentEpoch_));
    }
    if (contribution != EpochBarrierContribution::WorkComplete &&
        contribution != EpochBarrierContribution::Idle) {
        output_.fatal(
            CALL_INFO,
            -1,
            "epoch barrier invalid contribution: tile=%u epoch=%u contribution=%u\n",
            static_cast<unsigned>(tile),
            static_cast<unsigned>(epoch),
            static_cast<unsigned>(contribution));
    }

    const std::uint64_t now = currentClockCycle();
    if (arrivalCount_ == 0) {
        firstArrivalCycle_ = now;
    }
    arrivedTiles_[tile] = true;
    ++arrivalCount_;
    arrivalStatistic_->addData(1);
    if (contribution == EpochBarrierContribution::Idle) {
        ++idleArrivalCount_;
        idleArrivalStatistic_->addData(1);
    }

    output_.verbose(
        CALL_INFO,
        2,
        0,
        "arrival tile=%u epoch=%u contribution=%u (%u/%u) cycle=%llu\n",
        static_cast<unsigned>(tile),
        static_cast<unsigned>(epoch),
        static_cast<unsigned>(contribution),
        static_cast<unsigned>(arrivalCount_),
        static_cast<unsigned>(activeTileCount_),
        static_cast<unsigned long long>(now));

    if (arrivalCount_ == activeTileCount_) {
        if (releasePending_ || releaseCycles_ > UINT64_MAX - now) {
            output_.fatal(
                CALL_INFO,
                -1,
                "epoch barrier release scheduling overflow: epoch=%u cycle=%llu release_cycles=%llu\n",
                static_cast<unsigned>(currentEpoch_),
                static_cast<unsigned long long>(now),
                static_cast<unsigned long long>(releaseCycles_));
        }
        releaseCycle_ = now + releaseCycles_;
        releasePending_ = true;
        ensureClock();
    }
}

bool EpochBarrierController::clockTick(SST::Cycle_t cycle)
{
    const std::uint64_t now = static_cast<std::uint64_t>(cycle);
    if (releasePending_ && now >= releaseCycle_) {
        releaseCurrentEpoch(now);
    }
    if (!releasePending_) {
        clockRegistered_ = false;
        return true;
    }
    return false;
}

void EpochBarrierController::ensureClock()
{
    if (clockRegistered_) {
        return;
    }
    reregisterClock(clockTimeBase_, clockHandler_);
    clockRegistered_ = true;
}

void EpochBarrierController::releaseCurrentEpoch(std::uint64_t cycle)
{
    if (!releasePending_ || arrivalCount_ != activeTileCount_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "epoch barrier impossible release transition: epoch=%u arrivals=%u/%u pending=%u\n",
            static_cast<unsigned>(currentEpoch_),
            static_cast<unsigned>(arrivalCount_),
            static_cast<unsigned>(activeTileCount_),
            releasePending_ ? 1U : 0U);
    }

    const bool prefixStop =
        stopAfterReleases_ != 0 &&
        currentEpoch_ + 1U == stopAfterReleases_;
    for (std::uint32_t tile = 0; tile < tileCount_; ++tile) {
        if (!activeTiles_[tile]) {
            continue;
        }
        if (links_[tile] == nullptr) {
            output_.fatal(
                CALL_INFO,
                -1,
                "epoch barrier release has no endpoint: tile=%u epoch=%u\n",
                static_cast<unsigned>(tile),
                static_cast<unsigned>(currentEpoch_));
        }
        links_[tile]->send(new EpochBarrierEvent(
            tile,
            currentEpoch_,
            prefixStop ? EpochBarrierMessage::PrefixStop
                       : EpochBarrierMessage::Release,
            EpochBarrierContribution::None));
    }

    releaseStatistic_->addData(1);
    waitStatistic_->addData(cycle - firstArrivalCycle_);
    output_.output(
        "MITTENS_EPOCH_BARRIER_RELEASE completed_epoch=%u released_epoch=%u arrivals=%u idle=%u first_arrival_cycle=%llu release_cycle=%llu\n",
        static_cast<unsigned>(currentEpoch_),
        static_cast<unsigned>(currentEpoch_ + 1U),
        static_cast<unsigned>(arrivalCount_),
        static_cast<unsigned>(idleArrivalCount_),
        static_cast<unsigned long long>(firstArrivalCycle_),
        static_cast<unsigned long long>(cycle));

    ++currentEpoch_;
    std::fill(arrivedTiles_.begin(), arrivedTiles_.end(), false);
    arrivalCount_ = 0;
    idleArrivalCount_ = 0;
    releasePending_ = false;
    firstArrivalCycle_ = 0;
    releaseCycle_ = 0;
    finished_ = currentEpoch_ == epochCount_;
    if (prefixStop) {
        prefixComplete_ = true;
        output_.output(
            "MITTENS_EPOCH_PREFIX_COMPLETE releases=%u epoch_count=%u stop_cycle=%llu\n",
            static_cast<unsigned>(currentEpoch_),
            static_cast<unsigned>(epochCount_),
            static_cast<unsigned long long>(cycle));
        primaryComponentOKToEndSim();
    }
}

void EpochBarrierController::finish()
{
    if (!finished_ && !prefixComplete_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "epoch barrier simulation ended incomplete: current_epoch=%u epoch_count=%u arrivals=%u/%u release_pending=%u\n",
            static_cast<unsigned>(currentEpoch_),
            static_cast<unsigned>(epochCount_),
            static_cast<unsigned>(arrivalCount_),
            static_cast<unsigned>(activeTileCount_),
            releasePending_ ? 1U : 0U);
    }
}

} // namespace Mittens
} // namespace SST
