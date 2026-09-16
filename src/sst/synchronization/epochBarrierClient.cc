#include "epochBarrierClient.h"

namespace SST::Mittens
{
CpuDeviceResult EpochBarrierClient::executeCpuBarrier(const CpuBarrierAction& action)
{
    CpuDeviceResult result;
    if (config_.epochBarrierEpochs == 0 || !host_.sendEpoch ||
        action.epoch != state_.expectedEpochBarrier_ ||
        state_.expectedEpochBarrier_ >= config_.epochBarrierEpochs ||
        action.flags != MITTENS_SYNC_EVENT_FLAG_WAIT_FOR_COMPLETION ||
        (action.contribution != MITTENS_SYNC_EPOCH_WORK_COMPLETE &&
         action.contribution != MITTENS_SYNC_EPOCH_IDLE))
    {
        output_.fatal(-1,
                      "tile %u received an invalid epoch barrier arrival: epoch=%u expected=%u "
                      "epochs=%u contribution=%u flags=%u\n",
                      static_cast<unsigned>(config_.tileId), static_cast<unsigned>(action.epoch),
                      static_cast<unsigned>(state_.expectedEpochBarrier_),
                      static_cast<unsigned>(config_.epochBarrierEpochs),
                      static_cast<unsigned>(action.contribution),
                      static_cast<unsigned>(action.flags));
    }

    if (!state_.epochBarrierArrivalSent_)
    {
        if (state_.epochBarrierReleaseReady_)
        {
            output_.fatal(-1, "tile %u has an epoch release without an arrival for epoch %u\n",
                          static_cast<unsigned>(config_.tileId),
                          static_cast<unsigned>(state_.expectedEpochBarrier_));
        }
        if (!host_.globalDMADrained())
        {
            output_.fatal(-1, "tile %u reached epoch barrier %u with unretired global DMA tokens\n",
                          static_cast<unsigned>(config_.tileId),
                          static_cast<unsigned>(state_.expectedEpochBarrier_));
        }

        if (config_.epochBarrierDrainAnalog) {
            if (!host_.analogDrained)
                output_.fatal(-1, "%s", "analog-draining barrier has no completion source\n");
            if (!host_.analogDrained()) {
                waitingForAnalog_ = true;
                return result;
            }
        }
        waitingForAnalog_ = false;
        const EpochBarrierContribution contribution =
            action.contribution == MITTENS_SYNC_EPOCH_WORK_COMPLETE
                ? EpochBarrierContribution::WorkComplete
                : EpochBarrierContribution::Idle;
        state_.epochBarrierArrivalSent_ = true;
        ++state_.epochBarrierArrivals_;
        output_.verbose(2, 0, "tile %u reached epoch barrier %u contribution=%u\n",
                        static_cast<unsigned>(config_.tileId),
                        static_cast<unsigned>(state_.expectedEpochBarrier_),
                        static_cast<unsigned>(contribution));
        host_.sendEpoch(state_.expectedEpochBarrier_, contribution);
        return result;
    }

    if (!state_.epochBarrierReleaseReady_)
    {
        return result;
    }
    state_.epochBarrierArrivalSent_ = false;
    state_.epochBarrierReleaseReady_ = false;
    ++state_.expectedEpochBarrier_;
    ++state_.epochBarrierReleases_;
    output_.verbose(2, 0, "tile %u released from epoch barrier %u (next=%u)\n",
                    static_cast<unsigned>(config_.tileId),
                    static_cast<unsigned>(state_.expectedEpochBarrier_ - 1U),
                    static_cast<unsigned>(state_.expectedEpochBarrier_));
    result.complete = true;
    return result;
}

void EpochBarrierClient::onAnalogProgress()
{
    if (waitingForAnalog_ && host_.analogDrained && host_.analogDrained()) {
        waitingForAnalog_ = false;
        host_.wakeEpoch();
    }
}

bool EpochBarrierClient::onEpochRelease(std::uint32_t tile, std::uint32_t completedEpoch,
                                                 EpochBarrierMessage message,
                                                 EpochBarrierContribution contribution)
{
    const bool prefixStop = message == EpochBarrierMessage::PrefixStop;
    if (config_.epochBarrierEpochs == 0 || !host_.sendEpoch || !host_.running() ||
        (message != EpochBarrierMessage::Release && !prefixStop) ||
        contribution != EpochBarrierContribution::None || tile != config_.tileId ||
        completedEpoch != state_.expectedEpochBarrier_ ||
        state_.expectedEpochBarrier_ >= config_.epochBarrierEpochs ||
        !state_.epochBarrierArrivalSent_ || state_.epochBarrierReleaseReady_ ||
        !host_.pending().has_value() ||
        host_.pending()->stopReason != MITTENS_SYNC_STOP_EPOCH_BARRIER_ARRIVE ||
        host_.pending()->epochId != completedEpoch)
    {
        output_.fatal(
            -1,
            "tile %u received an impossible epoch barrier release: tile=%u epoch=%u expected=%u "
            "epochs=%u message=%u contribution=%u arrival_sent=%u release_ready=%u "
            "pending_reason=%u pending_epoch=%u\n",
            static_cast<unsigned>(config_.tileId), static_cast<unsigned>(tile),
            static_cast<unsigned>(completedEpoch),
            static_cast<unsigned>(state_.expectedEpochBarrier_),
            static_cast<unsigned>(config_.epochBarrierEpochs), static_cast<unsigned>(message),
            static_cast<unsigned>(contribution), state_.epochBarrierArrivalSent_ ? 1U : 0U,
            state_.epochBarrierReleaseReady_ ? 1U : 0U,
            static_cast<unsigned>(host_.pending().has_value()
                                      ? host_.pending()->stopReason
                                      : static_cast<std::uint32_t>(MITTENS_SYNC_STOP_NONE)),
            static_cast<unsigned>(host_.pending().has_value() ? host_.pending()->epochId
                                                              : UINT32_MAX));
    }

    if (prefixStop)
    {
        host_.completeWait();
        state_.epochBarrierArrivalSent_ = false;
        state_.epochBarrierReleaseReady_ = false;
        ++state_.expectedEpochBarrier_;
        ++state_.epochBarrierReleases_;
        return true;
    }

    state_.epochBarrierReleaseReady_ = true;
    host_.wakeEpoch();
    return false;
}

void EpochBarrierClient::validateExit() const
{
    if (config_.epochBarrierEpochs != 0 &&
        (state_.expectedEpochBarrier_ != config_.epochBarrierEpochs ||
         state_.epochBarrierArrivalSent_ || state_.epochBarrierReleaseReady_))
    {
        output_.fatal(-1,
                      "QEMU tile %u exited before completing the modeled epoch barrier: "
                      "completed=%u epochs=%u arrival_sent=%u release_ready=%u\n",
                      static_cast<unsigned>(config_.tileId),
                      static_cast<unsigned>(state_.expectedEpochBarrier_),
                      static_cast<unsigned>(config_.epochBarrierEpochs),
                      state_.epochBarrierArrivalSent_ ? 1U : 0U,
                      state_.epochBarrierReleaseReady_ ? 1U : 0U);
    }
}
} // namespace SST::Mittens
