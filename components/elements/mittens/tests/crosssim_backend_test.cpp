#include "analog/crossSimAnalogBackend.h"

#include <cassert>
#include <cmath>
#include <vector>

using SST::Mittens::CrossSimAnalogBackend;

namespace {

bool close(float actual, float expected)
{
    return std::fabs(actual - expected) < 1.0e-5F;
}

void requireVector(const std::vector<float>& actual,
                   const std::vector<float>& expected)
{
    assert(actual.size() == expected.size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        assert(close(actual[index], expected[index]));
    }
}

} // namespace

int main()
{
    CrossSimAnalogBackend tile0(2, 2, 3);
    CrossSimAnalogBackend tile1(2, 2, 3, "default");

    const std::vector<float> input{0.5F, -1.0F, 0.25F};
    tile0.setMatrix(
        0,
        {
            1.0F, 0.5F, -0.5F,
            -1.0F, 0.25F, 0.75F,
        });
    tile1.setMatrix(
        0,
        {
            0.5F, 0.0F, 0.0F,
            0.0F, -0.5F, 0.0F,
        });

    tile0.loadVector(0, input);
    tile1.loadVector(0, input);
    tile0.compute(0);
    tile1.compute(0);

    requireVector(tile0.output(0), {-0.125F, -0.5625F});
    requireVector(tile1.output(0), {0.25F, 0.5F});

    assert(tile0.arrayCount() == 2);
    assert(tile1.arrayCount() == 2);
    assert(tile0.arrayRows() == tile1.arrayRows());
    assert(tile0.arrayColumns() == tile1.arrayColumns());
    return 0;
}
