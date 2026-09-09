#include "sst_config.h"

#include "memoryInitializationBarrierController.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <string>

namespace SST {
namespace Mittens {

MemoryInitializationBarrierController::
    MemoryInitializationBarrierController(
        SST::ComponentId_t id, SST::Params& params) :
    SST::Component(id),
    output_(
        "mittens-memory-init-barrier: ",
        params.find<int>("verbose", 0),
        0,
        SST::Output::STDOUT),
    tileCount_(params.find<std::uint32_t>("tile_count", 1)),
    releaseCycles_(params.find<std::uint64_t>("release_cycles", 1)),
    releaseTilesPerCycle_(
        params.find<std::uint32_t>("release_tiles_per_cycle", 1))
{
    if (tileCount_ == 0 || releaseCycles_ == 0 ||
        releaseTilesPerCycle_ == 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "invalid memory initialization barrier configuration: tiles=%u release_cycles=%llu release_tiles_per_cycle=%u\n",
            static_cast<unsigned>(tileCount_),
            static_cast<unsigned long long>(releaseCycles_),
            static_cast<unsigned>(releaseTilesPerCycle_));
    }
    try {
        performanceProfile_.configure(
            params.find<std::string>("profile_output_directory", ""));
    } catch (const std::exception& error) {
        output_.fatal(
            CALL_INFO,
            -1,
            "cannot configure memory initialization barrier profile: %s\n",
            error.what());
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
            CALL_INFO,
            -1,
            "memory initialization barrier active tile list contains a duplicate\n");
    }

    links_.resize(tileCount_, nullptr);
    activeTiles_.resize(tileCount_, false);
    arrivedTiles_.resize(tileCount_, false);
    arrivalCycles_.resize(tileCount_, UINT64_MAX);
    for (const std::uint32_t tile : activeTileIds) {
        if (tile >= tileCount_) {
            output_.fatal(
                CALL_INFO,
                -1,
                "memory initialization barrier active tile %u is outside tile_count=%u\n",
                static_cast<unsigned>(tile),
                static_cast<unsigned>(tileCount_));
        }
        activeTiles_[tile] = true;
        ++activeTileCount_;
    }
    if (activeTileCount_ == 0) {
        output_.fatal(
            CALL_INFO,
            -1,
            "memory initialization barrier requires at least one active tile\n");
    }
    activeTileIds_ = activeTileIds;

    for (std::uint32_t tile = 0; tile < tileCount_; ++tile) {
        const std::string port = "barrier" + std::to_string(tile);
        links_[tile] = configureLink(
            port,
            new SST::Event::Handler<
                MemoryInitializationBarrierController,
                &MemoryInitializationBarrierController::handleEvent>(this));
    }

    arrivalStatistic_ = registerStatistic<std::uint64_t>("arrivals");
    releaseStatistic_ = registerStatistic<std::uint64_t>("releases");
    waitStatistic_ =
        registerStatistic<std::uint64_t>("barrier_wait_cycles");
    tileWaitStatistic_ =
        registerStatistic<std::uint64_t>("tile_wait_cycles");

    clockTimeBase_ = getTimeConverter(
        params.find<std::string>("clock", "1GHz"));
    clockHandler_ = new SST::Clock::Handler<
        MemoryInitializationBarrierController,
        &MemoryInitializationBarrierController::clockTick>(this);
    registerClock(clockTimeBase_, clockHandler_);
    clockRegistered_ = true;
}

void MemoryInitializationBarrierController::setup()
{
    for (std::uint32_t tile = 0; tile < tileCount_; ++tile) {
        if (activeTiles_[tile] && links_[tile] == nullptr) {
            output_.fatal(
                CALL_INFO,
                -1,
                "memory initialization barrier active tile %u has no controller link\n",
                static_cast<unsigned>(tile));
        }
        if (!activeTiles_[tile] && links_[tile] != nullptr) {
            output_.fatal(
                CALL_INFO,
                -1,
                "memory initialization barrier inactive tile %u has an unexpected controller link\n",
                static_cast<unsigned>(tile));
        }
    }
}

std::uint64_t
MemoryInitializationBarrierController::currentClockCycle()
{
    return static_cast<std::uint64_t>(
        getNextClockCycle(clockTimeBase_) - 1U);
}

void MemoryInitializationBarrierController::handleEvent(
    SST::Event* rawEvent)
{
    auto* event =
        dynamic_cast<MemoryInitializationBarrierEvent*>(rawEvent);
    if (event == nullptr) {
        delete rawEvent;
        output_.fatal(
            CALL_INFO,
            -1,
            "memory initialization barrier received an event with the wrong type\n");
    }

    const std::uint32_t tile = event->tileId();
    const MemoryInitializationBarrierMessage message = event->message();
    delete event;

    if (message != MemoryInitializationBarrierMessage::Arrive) {
        output_.fatal(
            CALL_INFO,
            -1,
            "memory initialization barrier controller received non-arrival from tile %u\n",
            static_cast<unsigned>(tile));
    }
    if (tile >= tileCount_ || !activeTiles_[tile]) {
        output_.fatal(
            CALL_INFO,
            -1,
            "memory initialization barrier arrival from inactive tile %u\n",
            static_cast<unsigned>(tile));
    }
    if (released_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "memory initialization barrier arrival after release from tile %u\n",
            static_cast<unsigned>(tile));
    }
    if (arrivedTiles_[tile]) {
        output_.fatal(
            CALL_INFO,
            -1,
            "memory initialization barrier duplicate arrival from tile %u\n",
            static_cast<unsigned>(tile));
    }

    const std::uint64_t now = currentClockCycle();
    if (arrivalCount_ == 0) {
        firstArrivalCycle_ = now;
    }
    arrivedTiles_[tile] = true;
    arrivalCycles_[tile] = now;
    ++arrivalCount_;
    arrivalStatistic_->addData(1);

    output_.verbose(
        CALL_INFO,
        2,
        0,
        "arrival tile=%u (%u/%u) cycle=%llu\n",
        static_cast<unsigned>(tile),
        static_cast<unsigned>(arrivalCount_),
        static_cast<unsigned>(activeTileCount_),
        static_cast<unsigned long long>(now));

    if (arrivalCount_ == activeTileCount_) {
        if (releasePending_ || releaseCycles_ > UINT64_MAX - now) {
            output_.fatal(
                CALL_INFO,
                -1,
                "memory initialization barrier release scheduling overflow: cycle=%llu release_cycles=%llu\n",
                static_cast<unsigned long long>(now),
                static_cast<unsigned long long>(releaseCycles_));
        }
        releaseCycle_ = now + releaseCycles_;
        releasePending_ = true;
        ensureClock();
    }
}

bool MemoryInitializationBarrierController::clockTick(SST::Cycle_t cycle)
{
    const std::uint64_t now = static_cast<std::uint64_t>(cycle);
    if (releasePending_ && now >= releaseCycle_) {
        releaseBatch(now);
    }
    if (!releasePending_) {
        clockRegistered_ = false;
        return true;
    }
    return false;
}

void MemoryInitializationBarrierController::ensureClock()
{
    if (clockRegistered_) {
        return;
    }
    reregisterClock(clockTimeBase_, clockHandler_);
    clockRegistered_ = true;
}

void MemoryInitializationBarrierController::releaseBatch(
    std::uint64_t cycle)
{
    if (!releasePending_ || released_ ||
        arrivalCount_ != activeTileCount_ ||
        releasedTileCount_ >= activeTileCount_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "memory initialization barrier impossible release transition: arrivals=%u/%u tile_releases=%u pending=%u released=%u\n",
            static_cast<unsigned>(arrivalCount_),
            static_cast<unsigned>(activeTileCount_),
            static_cast<unsigned>(releasedTileCount_),
            releasePending_ ? 1U : 0U,
            released_ ? 1U : 0U);
    }

    if (releasedTileCount_ == 0) {
        releaseStartCycle_ = cycle;
    }
    const std::uint32_t end = std::min(
        activeTileCount_,
        releasedTileCount_ + releaseTilesPerCycle_);
    while (releasedTileCount_ < end) {
        const std::uint32_t tile = activeTileIds_[releasedTileCount_];
        if (links_[tile] == nullptr || arrivalCycles_[tile] == UINT64_MAX ||
            arrivalCycles_[tile] > cycle) {
            output_.fatal(
                CALL_INFO,
                -1,
                "memory initialization barrier release has an invalid timeline for tile %u\n",
                static_cast<unsigned>(tile));
        }
        const std::uint64_t tileWait = cycle - arrivalCycles_[tile];
        if (tileWait > UINT64_MAX - tileWaitCycles_) {
            output_.fatal(
                CALL_INFO,
                -1,
                "memory initialization barrier tile-wait accounting overflowed\n");
        }
        try {
            performanceProfile_.record(
                MemoryInitializationBarrierTimeline{
                    tile, arrivalCycles_[tile], cycle});
        } catch (const std::exception& error) {
            output_.fatal(
                CALL_INFO,
                -1,
                "cannot record memory initialization barrier timeline: %s\n",
                error.what());
        }
        tileWaitCycles_ += tileWait;
        tileWaitStatistic_->addData(tileWait);
        links_[tile]->send(new MemoryInitializationBarrierEvent(
            tile,
            MemoryInitializationBarrierMessage::Release));
        ++releasedTileCount_;
    }

    if (releasedTileCount_ != activeTileCount_) {
        if (cycle == UINT64_MAX) {
            output_.fatal(
                CALL_INFO,
                -1,
                "memory initialization barrier release bandwidth scheduling overflowed\n");
        }
        releaseCycle_ = cycle + 1;
        return;
    }

    releaseStatistic_->addData(1);
    waitStatistic_->addData(cycle - firstArrivalCycle_);
    lastReleaseCycle_ = cycle;
    output_.output(
        "MITTENS_MEMORY_INIT_BARRIER_RELEASE arrivals=%u first_arrival_cycle=%llu first_release_cycle=%llu release_cycle=%llu release_tiles_per_cycle=%u\n",
        static_cast<unsigned>(arrivalCount_),
        static_cast<unsigned long long>(firstArrivalCycle_),
        static_cast<unsigned long long>(releaseStartCycle_),
        static_cast<unsigned long long>(cycle),
        static_cast<unsigned>(releaseTilesPerCycle_));

    releasePending_ = false;
    released_ = true;
    releaseCycle_ = 0;
}

void MemoryInitializationBarrierController::finish()
{
    if (!released_ || arrivalCount_ != activeTileCount_) {
        output_.fatal(
            CALL_INFO,
            -1,
            "memory initialization barrier simulation ended incomplete: arrivals=%u/%u release_pending=%u released=%u\n",
            static_cast<unsigned>(arrivalCount_),
            static_cast<unsigned>(activeTileCount_),
            releasePending_ ? 1U : 0U,
            released_ ? 1U : 0U);
    }
    if (!performanceProfile_.totalsReconcile(
            activeTileCount_,
            tileWaitCycles_,
            lastReleaseCycle_ - firstArrivalCycle_)) {
        output_.fatal(
            CALL_INFO,
            -1,
            "memory initialization barrier counters do not reconcile with the controller timeline\n");
    }
}

} // namespace Mittens
} // namespace SST
