#include "nativeAnalogBackend.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace SST {
namespace Mittens {

NativeAnalogBackend::NativeAnalogBackend(
    std::uint32_t arrayCount,
    std::uint32_t arrayRows,
    std::uint32_t arrayColumns) :
    arrayRows_(arrayRows),
    arrayColumns_(arrayColumns)
{
    if (arrayCount == 0) {
        throw std::invalid_argument(
            "an analog backend requires at least one array");
    }
    if (arrayRows_ == 0 || arrayColumns_ == 0) {
        throw std::invalid_argument(
            "analog array dimensions must be nonzero");
    }

    arrays_.reserve(arrayCount);
    for (std::uint32_t index = 0; index < arrayCount; ++index) {
        Array arrayState;
        arrayState.matrix.resize(
            static_cast<std::size_t>(arrayRows_) * arrayColumns_);
        arrayState.input.resize(arrayColumns_);
        arrayState.output.resize(arrayRows_);
        arrays_.push_back(std::move(arrayState));
    }
}

std::size_t NativeAnalogBackend::arrayCount() const noexcept
{
    return arrays_.size();
}

void NativeAnalogBackend::setMatrix(
    std::uint32_t arrayId,
    const std::vector<float>& matrix)
{
    Array& target = array(arrayId);
    const std::size_t expected =
        static_cast<std::size_t>(arrayRows_) * arrayColumns_;
    if (matrix.size() != expected) {
        throw std::invalid_argument("matrix size does not match array shape");
    }

    target.matrix = matrix;
    target.matrixLoaded = true;
    target.outputReady = false;
}

void NativeAnalogBackend::loadVector(
    std::uint32_t arrayId,
    const std::vector<float>& input)
{
    Array& target = array(arrayId);
    if (input.size() != arrayColumns_) {
        throw std::invalid_argument("input size does not match array shape");
    }

    target.input = input;
    target.inputLoaded = true;
    target.outputReady = false;
}

void NativeAnalogBackend::compute(std::uint32_t arrayId)
{
    Array& target = array(arrayId);
    if (!target.matrixLoaded) {
        throw std::logic_error("cannot compute before loading a matrix");
    }
    if (!target.inputLoaded) {
        throw std::logic_error("cannot compute before loading an input vector");
    }

    std::fill(target.output.begin(), target.output.end(), 0.0F);
    for (std::uint32_t row = 0; row < arrayRows_; ++row) {
        for (std::uint32_t column = 0;
             column < arrayColumns_;
             ++column) {
            const std::size_t matrixIndex =
                static_cast<std::size_t>(row) * arrayColumns_ + column;
            target.output[row] +=
                target.matrix[matrixIndex] * target.input[column];
        }
    }
    target.outputReady = true;
}

std::vector<float> NativeAnalogBackend::output(std::uint32_t arrayId) const
{
    const Array& source = array(arrayId);
    if (!source.outputReady) {
        throw std::logic_error("analog array output is not ready");
    }
    return source.output;
}

void NativeAnalogBackend::moveOutput(
    std::uint32_t sourceArrayId,
    std::uint32_t destinationArrayId)
{
    const Array& source = array(sourceArrayId);
    Array& destination = array(destinationArrayId);

    if (!source.outputReady) {
        throw std::logic_error("source analog array output is not ready");
    }
    if (source.output.size() != destination.input.size()) {
        throw std::invalid_argument(
            "source output does not fit destination input");
    }
    destination.input = source.output;
    destination.inputLoaded = true;
    destination.outputReady = false;
}

NativeAnalogBackend::Array&
NativeAnalogBackend::array(std::uint32_t arrayId)
{
    if (arrayId >= arrays_.size()) {
        throw std::out_of_range(
            "analog array ID " + std::to_string(arrayId) + " is invalid");
    }
    return arrays_[arrayId];
}

const NativeAnalogBackend::Array&
NativeAnalogBackend::array(std::uint32_t arrayId) const
{
    if (arrayId >= arrays_.size()) {
        throw std::out_of_range(
            "analog array ID " + std::to_string(arrayId) + " is invalid");
    }
    return arrays_[arrayId];
}

} // namespace Mittens
} // namespace SST
