#include "timingAnalogBackend.h"

#include <stdexcept>
#include <string>

namespace SST {
namespace Mittens {

TimingAnalogBackend::TimingAnalogBackend(
    std::uint32_t arrayCount,
    std::uint32_t arrayRows,
    std::uint32_t arrayColumns) :
    arrayRows_(arrayRows),
    arrayColumns_(arrayColumns),
    arrays_(arrayCount)
{
    if (arrayCount == 0 || arrayRows_ == 0 || arrayColumns_ == 0) {
        throw std::invalid_argument(
            "timing analog backend dimensions must be nonzero");
    }
}

std::size_t TimingAnalogBackend::arrayCount() const noexcept
{
    return arrays_.size();
}

void TimingAnalogBackend::setMatrix(
    std::uint32_t arrayId,
    const std::vector<float>& matrix)
{
    Array& target = array(arrayId);
    const std::size_t expected =
        static_cast<std::size_t>(arrayRows_) * arrayColumns_;
    if (matrix.size() != expected) {
        throw std::invalid_argument("matrix size does not match array shape");
    }
    target.matrixLoaded = true;
    target.outputReady = false;
}

void TimingAnalogBackend::loadVector(
    std::uint32_t arrayId,
    const std::vector<float>& input)
{
    Array& target = array(arrayId);
    if (input.size() != arrayColumns_) {
        throw std::invalid_argument("input size does not match array shape");
    }
    target.inputLoaded = true;
    target.outputReady = false;
}

void TimingAnalogBackend::compute(std::uint32_t arrayId)
{
    Array& target = array(arrayId);
    if (!target.matrixLoaded) {
        throw std::logic_error("cannot compute before loading a matrix");
    }
    if (!target.inputLoaded) {
        throw std::logic_error("cannot compute before loading an input vector");
    }
    target.outputReady = true;
}

std::vector<float> TimingAnalogBackend::output(
    std::uint32_t arrayId) const
{
    const Array& source = array(arrayId);
    if (!source.outputReady) {
        throw std::logic_error("analog array output is not ready");
    }
    return std::vector<float>(arrayRows_, 0.0F);
}

void TimingAnalogBackend::moveOutput(
    std::uint32_t sourceArrayId,
    std::uint32_t destinationArrayId)
{
    const Array& source = array(sourceArrayId);
    Array& destination = array(destinationArrayId);
    if (!source.outputReady) {
        throw std::logic_error("source analog array output is not ready");
    }
    if (arrayRows_ != arrayColumns_) {
        throw std::invalid_argument(
            "source output does not fit destination input");
    }
    destination.inputLoaded = true;
    destination.outputReady = false;
}

TimingAnalogBackend::Array& TimingAnalogBackend::array(
    std::uint32_t arrayId)
{
    if (arrayId >= arrays_.size()) {
        throw std::out_of_range(
            "analog array ID " + std::to_string(arrayId) + " is invalid");
    }
    return arrays_[arrayId];
}

const TimingAnalogBackend::Array& TimingAnalogBackend::array(
    std::uint32_t arrayId) const
{
    if (arrayId >= arrays_.size()) {
        throw std::out_of_range(
            "analog array ID " + std::to_string(arrayId) + " is invalid");
    }
    return arrays_[arrayId];
}

} // namespace Mittens
} // namespace SST
