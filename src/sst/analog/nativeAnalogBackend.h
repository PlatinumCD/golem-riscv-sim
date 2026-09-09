#ifndef SST_MITTENS_NATIVE_ANALOG_BACKEND_H
#define SST_MITTENS_NATIVE_ANALOG_BACKEND_H

#include "analogBackend.h"

namespace SST {
namespace Mittens {

class NativeAnalogBackend final : public AnalogBackend
{
  public:
    NativeAnalogBackend(std::uint32_t arrayCount,
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

    std::uint64_t lastComputeMultiplyCount(
        std::uint32_t arrayId) const;

  private:
    struct Array {
        std::vector<float> matrix;
        std::vector<float> input;
        std::vector<float> output;
        std::uint32_t activeRows = 0;
        std::uint32_t activeColumns = 0;
        std::uint64_t lastComputeMultiplyCount = 0;
        bool inputFinite = true;
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
