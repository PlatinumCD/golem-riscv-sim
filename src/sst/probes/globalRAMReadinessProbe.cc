#include "sst_config.h"

#include "globalRAMReadinessProbe.h"

namespace SST {
namespace Mittens {

namespace {

constexpr std::uint64_t FirstExecution = 17;
constexpr std::uint64_t SecondExecution = 18;
constexpr std::uint32_t FrameBytes = 4096;
constexpr std::uint32_t Exact = GlobalDMAExactReadiness;
constexpr std::uint32_t Initial =
    GlobalDMAExactReadiness | GlobalDMAEpochZeroSource;
constexpr std::uint32_t Teardown =
    GlobalDMAExactReadiness | GlobalDMAExactExecutionTeardown;

} // namespace

GlobalRAMReadinessProbe::GlobalRAMReadinessProbe(
    SST::ComponentId_t id, SST::Params& params) :
    SST::Component(id),
    output_(
        "mittens-global-ram-readiness-probe: ",
        0,
        0,
        SST::Output::STDOUT),
    tileId_(params.find<std::uint32_t>("tile_id", 0)),
    scenario_(params.find<std::string>("scenario", "coverage"))
{
    if (scenario_ != "coverage" && scenario_ != "demand" &&
        scenario_ != "priority" &&
        scenario_ != "reservation" && scenario_ != "duplicate") {
        output_.fatal(CALL_INFO, -1, "invalid readiness probe scenario\n");
    }
    if (scenario_ == "duplicate" && tileId_ != 0) {
        output_.fatal(
            CALL_INFO, -1, "duplicate readiness probe requires tile zero\n");
    }
    ramLink_ = configureLink(
        "ram",
        new SST::Event::Handler<
            GlobalRAMReadinessProbe,
            &GlobalRAMReadinessProbe::handleCompletion>(this));
    if (ramLink_ == nullptr) {
        output_.fatal(CALL_INFO, -1, "readiness probe RAM link is absent\n");
    }
    producerStartLink_ = configureSelfLink(
        "producer-start",
        "1ns",
        new SST::Event::Handler<
            GlobalRAMReadinessProbe,
            &GlobalRAMReadinessProbe::handleProducerStart>(this));

    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
    registerClock(
        "1GHz",
        new SST::Clock::Handler<
            GlobalRAMReadinessProbe,
            &GlobalRAMReadinessProbe::start>(this));
}

bool GlobalRAMReadinessProbe::start(SST::Cycle_t)
{
    if (scenario_ == "duplicate") {
        submitDuplicateScenario();
    } else if (scenario_ == "demand") {
        submitDemandScenario();
    } else if (scenario_ == "priority") {
        submitPriorityScenario();
    } else if (scenario_ == "reservation") {
        submitReservationScenario();
    } else {
        submitCoveragePhase();
    }
    return true;
}

void GlobalRAMReadinessProbe::submitDemandScenario()
{
    ++phase_;
    // The read blocks first.  Although the unrelated write is queued before
    // its producer, demand-aware arbitration must serve the producer write,
    // release and serve the read, and only then retire the unrelated write.
    submit(
        FirstExecution,
        0,
        0,
        FrameBytes,
        GlobalDMADirection::GlobalRAMToScratchpad,
        Exact);
    submit(
        FirstExecution,
        100,
        FrameBytes,
        FrameBytes,
        GlobalDMADirection::ScratchpadToGlobalRAM,
        Exact);
    submit(
        FirstExecution,
        101,
        0,
        FrameBytes,
        GlobalDMADirection::ScratchpadToGlobalRAM,
        Exact);
}

void GlobalRAMReadinessProbe::submitReservationScenario()
{
    ++phase_;
    // With two channels and one reserved for reads, only the first write may
    // enter service.  The delayed ready read must use the reserved capacity
    // and complete before the second write.
    submit(
        FirstExecution,
        100,
        0,
        FrameBytes,
        GlobalDMADirection::ScratchpadToGlobalRAM,
        Exact);
    submit(
        FirstExecution,
        101,
        FrameBytes,
        FrameBytes,
        GlobalDMADirection::ScratchpadToGlobalRAM,
        Exact);
    producerStartLink_->send(1, new SST::Event());
}

void GlobalRAMReadinessProbe::submitPriorityScenario()
{
    ++phase_;
    // Deliberately queue writes first.  A four-request read-priority burst
    // must bypass them, force one write, serve the remaining read, and then
    // serve the final write.  Epoch-zero reads are immediately schedulable.
    submit(
        FirstExecution,
        100,
        0,
        FrameBytes,
        GlobalDMADirection::ScratchpadToGlobalRAM,
        Exact);
    submit(
        FirstExecution,
        101,
        FrameBytes,
        FrameBytes,
        GlobalDMADirection::ScratchpadToGlobalRAM,
        Exact);
    for (std::uint32_t token = 0; token < 5; ++token) {
        submit(
            FirstExecution,
            token,
            (2U + token) * FrameBytes,
            FrameBytes,
            GlobalDMADirection::GlobalRAMToScratchpad,
            Initial);
    }
}

void GlobalRAMReadinessProbe::submit(
    std::uint64_t executionId,
    std::uint32_t tokenId,
    std::uint64_t globalOffset,
    std::uint32_t byteCount,
    GlobalDMADirection direction,
    std::uint32_t flags)
{
    ramLink_->send(new GlobalDMAEvent(
        tileId_,
        executionId,
        tokenId,
        tokenId,
        globalOffset,
        0,
        byteCount,
        direction,
        flags,
        false));
    ++pendingData_;
}

void GlobalRAMReadinessProbe::submitTeardown(std::uint64_t executionId)
{
    ramLink_->send(new GlobalDMAEvent(
        tileId_,
        executionId,
        1000U + phase_,
        0,
        0,
        0,
        0,
        GlobalDMADirection::GlobalRAMToScratchpad,
        Teardown,
        false));
}

void GlobalRAMReadinessProbe::submitCoveragePhase()
{
    ++phase_;
    const std::uint64_t executionId =
        phase_ == 1 ? FirstExecution : SecondExecution;
    if (tileId_ == 0) {
        // Both reads arrive first.  The controller must sleep with no eligible
        // work, then wake and bypass these blocked queue heads when the
        // producer writes arrive ten cycles later.
        submit(
            executionId,
            0,
            0,
            2U * FrameBytes,
            GlobalDMADirection::GlobalRAMToScratchpad,
            Exact);
        if (phase_ == 1) {
            submit(
                executionId,
                3,
                0,
                FrameBytes,
                GlobalDMADirection::GlobalRAMToScratchpad,
                Exact);
        }
        producerStartLink_->send(10, new SST::Event());
    } else if (tileId_ == 1) {
        submit(
            executionId,
            10,
            0,
            2U * FrameBytes,
            GlobalDMADirection::GlobalRAMToScratchpad,
            Exact);
        if (phase_ == 1) {
            submit(
                executionId,
                11,
                0,
                FrameBytes,
                GlobalDMADirection::GlobalRAMToScratchpad,
                Exact);
            submit(
                executionId,
                12,
                4U * FrameBytes,
                FrameBytes,
                GlobalDMADirection::GlobalRAMToScratchpad,
                Initial);
        }
    } else {
        output_.fatal(CALL_INFO, -1, "coverage probe has invalid tile ID\n");
    }
}

void GlobalRAMReadinessProbe::handleProducerStart(SST::Event* event)
{
    delete event;
    if (scenario_ == "reservation" && tileId_ == 0 && phase_ == 1) {
        submit(
            FirstExecution,
            0,
            2U * FrameBytes,
            FrameBytes,
            GlobalDMADirection::GlobalRAMToScratchpad,
            Initial);
        return;
    }
    if (scenario_ != "coverage" || tileId_ != 0 ||
        (phase_ != 1 && phase_ != 2)) {
        output_.fatal(CALL_INFO, -1, "invalid delayed producer event\n");
    }
    const std::uint64_t executionId =
        phase_ == 1 ? FirstExecution : SecondExecution;
    submit(
        executionId,
        1,
        0,
        FrameBytes,
        GlobalDMADirection::ScratchpadToGlobalRAM,
        Exact);
    submit(
        executionId,
        2,
        FrameBytes,
        FrameBytes,
        GlobalDMADirection::ScratchpadToGlobalRAM,
        Exact);
}

void GlobalRAMReadinessProbe::submitDuplicateScenario()
{
    ++phase_;
    submit(
        FirstExecution,
        1,
        0,
        FrameBytes,
        GlobalDMADirection::ScratchpadToGlobalRAM,
        Exact);
    submit(
        FirstExecution,
        2,
        0,
        FrameBytes,
        GlobalDMADirection::ScratchpadToGlobalRAM,
        Exact);
}

void GlobalRAMReadinessProbe::handleCompletion(SST::Event* rawEvent)
{
    auto* event = dynamic_cast<GlobalDMAEvent*>(rawEvent);
    if (event == nullptr || !event->completion() ||
        event->tileId() != tileId_) {
        delete rawEvent;
        output_.fatal(CALL_INFO, -1, "invalid readiness probe completion\n");
    }
    if (scenario_ == "priority") {
        if ((event->requestFlags() & GlobalDMAExactExecutionTeardown) != 0U) {
            if (pendingData_ != 0 || priorityCompletionIndex_ != 7 ||
                event->requestFlags() != Teardown) {
                delete event;
                output_.fatal(CALL_INFO, -1,
                              "early or malformed priority teardown ack\n");
            }
            delete event;
            output_.output("tile %u bounded read priority: PASS\n",
                           static_cast<unsigned>(tileId_));
            primaryComponentOKToEndSim();
            return;
        }
        constexpr std::uint32_t ExpectedOrder[] = {0, 1, 2, 3, 100, 4, 101};
        if (priorityCompletionIndex_ >= 7 ||
            event->tokenId() != ExpectedOrder[priorityCompletionIndex_]) {
            delete event;
            output_.fatal(CALL_INFO, -1,
                          "bounded read-priority completion order mismatch\n");
        }
        ++priorityCompletionIndex_;
        if (pendingData_ == 0) {
            delete event;
            output_.fatal(CALL_INFO, -1,
                          "priority completion underflow\n");
        }
        --pendingData_;
        delete event;
        if (pendingData_ == 0) {
            submitTeardown(FirstExecution);
        }
        return;
    }
    if (scenario_ == "demand") {
        if ((event->requestFlags() & GlobalDMAExactExecutionTeardown) != 0U) {
            if (pendingData_ != 0 || demandCompletionIndex_ != 3 ||
                event->requestFlags() != Teardown) {
                delete event;
                output_.fatal(CALL_INFO, -1,
                              "early or malformed demand teardown ack\n");
            }
            delete event;
            output_.output("tile %u blocked-read demand priority: PASS\n",
                           static_cast<unsigned>(tileId_));
            primaryComponentOKToEndSim();
            return;
        }
        constexpr std::uint32_t ExpectedOrder[] = {101, 0, 100};
        if (demandCompletionIndex_ >= 3 ||
            event->tokenId() != ExpectedOrder[demandCompletionIndex_]) {
            delete event;
            output_.fatal(CALL_INFO, -1,
                          "blocked-read demand completion order mismatch\n");
        }
        ++demandCompletionIndex_;
        if (pendingData_ == 0) {
            delete event;
            output_.fatal(CALL_INFO, -1,
                          "demand completion underflow\n");
        }
        --pendingData_;
        delete event;
        if (pendingData_ == 0) {
            submitTeardown(FirstExecution);
        }
        return;
    }
    if (scenario_ == "reservation") {
        if ((event->requestFlags() & GlobalDMAExactExecutionTeardown) != 0U) {
            if (pendingData_ != 0 || reservationCompletionIndex_ != 3 ||
                event->requestFlags() != Teardown) {
                delete event;
                output_.fatal(CALL_INFO, -1,
                              "early or malformed reservation teardown ack\n");
            }
            delete event;
            output_.output("tile %u reserved read channel: PASS\n",
                           static_cast<unsigned>(tileId_));
            primaryComponentOKToEndSim();
            return;
        }
        constexpr std::uint32_t ExpectedOrder[] = {100, 0, 101};
        if (reservationCompletionIndex_ >= 3 ||
            event->tokenId() != ExpectedOrder[reservationCompletionIndex_]) {
            delete event;
            output_.fatal(CALL_INFO, -1,
                          "reserved read-channel completion order mismatch\n");
        }
        ++reservationCompletionIndex_;
        if (pendingData_ == 0) {
            delete event;
            output_.fatal(CALL_INFO, -1,
                          "reservation completion underflow\n");
        }
        --pendingData_;
        delete event;
        if (pendingData_ == 0) {
            submitTeardown(FirstExecution);
        }
        return;
    }
    if ((event->requestFlags() & GlobalDMAExactExecutionTeardown) != 0U) {
        if (pendingData_ != 0 || event->requestFlags() != Teardown) {
            delete event;
            output_.fatal(
                CALL_INFO, -1, "early or malformed readiness teardown ack\n");
        }
        delete event;
        if (phase_ == 1) {
            submitCoveragePhase();
            return;
        }
        output_.output(
            "tile %u exact readiness: PASS\n",
            static_cast<unsigned>(tileId_));
        primaryComponentOKToEndSim();
        return;
    }

    if (pendingData_ == 0) {
        delete event;
        output_.fatal(CALL_INFO, -1, "readiness completion underflow\n");
    }
    --pendingData_;
    delete event;
    if (pendingData_ == 0 && scenario_ == "coverage") {
        submitTeardown(phase_ == 1 ? FirstExecution : SecondExecution);
    }
}

} // namespace Mittens
} // namespace SST
