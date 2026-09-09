#ifndef SST_MITTENS_ANALOG_BACKEND_H
#define SST_MITTENS_ANALOG_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace SST {
namespace Mittens {

class AnalogBackend
{
  public:
    virtual ~AnalogBackend() = default;

    virtual std::size_t arrayCount() const noexcept = 0;
    virtual std::uint32_t arrayRows() const noexcept = 0;
    virtual std::uint32_t arrayColumns() const noexcept = 0;

    virtual void setMatrix(std::uint32_t arrayId,
                           const std::vector<float>& matrix) = 0;
    virtual void loadVector(std::uint32_t arrayId,
                            const std::vector<float>& input) = 0;
    virtual void compute(std::uint32_t arrayId) = 0;
    virtual std::vector<float> output(std::uint32_t arrayId) const = 0;
    virtual void moveOutput(std::uint32_t sourceArrayId,
                            std::uint32_t destinationArrayId) = 0;
};

} // namespace Mittens
} // namespace SST

#endif
