#ifndef SST_MITTENS_TIMING_ANALOG_BACKEND_H
#define SST_MITTENS_TIMING_ANALOG_BACKEND_H

#include "analogBackend.h"

#include <vector>

namespace SST {
namespace Mittens {

// Timing-only backend for runs that intentionally do not validate numerical
// model outputs. AnalogDevice remains the sole owner of command ordering,
// queueing, link occupancy, compute latency, and completion timing; this
// backend removes only host-side matrix storage and arithmetic.
class TimingAnalogBackend final : public AnalogBackend
{
  public:
    TimingAnalogBackend(std::uint32_t arrayCount,
                        std::uint32_t arrayRows,
                        std::uint32_t arrayColumns);

    std::size_t arrayCount() const noexcept override;
    std::uint32_t arrayRows() const noexcept override { return arrayRows_; }
    std::uint32_t arrayColumns() const noexcept override
    {
        return arrayColumns_;
    }

    void setMatrix(std::uint32_t arrayId,
                   const std::vector<float>& matrix) override;
    void loadVector(std::uint32_t arrayId,
                    const std::vector<float>& input) override;
    void compute(std::uint32_t arrayId) override;
    std::vector<float> output(std::uint32_t arrayId) const override;
    void moveOutput(std::uint32_t sourceArrayId,
                    std::uint32_t destinationArrayId) override;

  private:
    struct Array {
        bool matrixLoaded = false;
        bool inputLoaded = false;
        bool outputReady = false;
    };

    Array& array(std::uint32_t arrayId);
    const Array& array(std::uint32_t arrayId) const;

    std::uint32_t arrayRows_;
    std::uint32_t arrayColumns_;
    std::vector<Array> arrays_;
};

} // namespace Mittens
} // namespace SST

#endif
