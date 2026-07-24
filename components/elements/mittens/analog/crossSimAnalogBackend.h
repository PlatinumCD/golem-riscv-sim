#ifndef SST_MITTENS_CROSSSIM_ANALOG_BACKEND_H
#define SST_MITTENS_CROSSSIM_ANALOG_BACKEND_H

#include "analogBackend.h"

#include <memory>
#include <string>

namespace SST {
namespace Mittens {

class CrossSimAnalogBackend final : public AnalogBackend
{
  public:
    CrossSimAnalogBackend(std::uint32_t arrayCount,
                          std::uint32_t arrayRows,
                          std::uint32_t arrayColumns,
                          std::string configurationPath = {});
    ~CrossSimAnalogBackend() override;

    CrossSimAnalogBackend(const CrossSimAnalogBackend&) = delete;
    CrossSimAnalogBackend& operator=(const CrossSimAnalogBackend&) = delete;
    CrossSimAnalogBackend(CrossSimAnalogBackend&&) = delete;
    CrossSimAnalogBackend& operator=(CrossSimAnalogBackend&&) = delete;

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
    struct Implementation;

    std::uint32_t arrayRows_;
    std::uint32_t arrayColumns_;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace Mittens
} // namespace SST

#endif
