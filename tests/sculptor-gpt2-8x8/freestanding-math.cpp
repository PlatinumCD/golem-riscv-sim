#include <stdint.h>

namespace {

union FloatBits {
    float value;
    uint32_t bits;
};

float powerOfTwo(int exponent) {
    if (exponent < -126) {
        return 0.0F;
    }
    if (exponent > 127) {
        return FloatBits{.bits = UINT32_C(0x7f800000)}.value;
    }
    return FloatBits{
        .bits = static_cast<uint32_t>(exponent + 127) << 23,
    }.value;
}

}  // namespace

extern "C" float expf(float value) {
    if (value <= -87.0F) {
        return 0.0F;
    }
    if (value >= 88.0F) {
        return FloatBits{.bits = UINT32_C(0x7f800000)}.value;
    }

    constexpr float inverse_log_two = 1.4426950408889634F;
    constexpr float log_two = 0.6931471805599453F;
    const int exponent = static_cast<int>(value * inverse_log_two);
    const float reduced = value - static_cast<float>(exponent) * log_two;
    const float polynomial =
        1.0F +
        reduced *
            (1.0F +
             reduced *
                 (0.5F +
                  reduced *
                      (0.1666666716F +
                       reduced *
                           (0.0416666679F +
                            reduced *
                                (0.0083333338F +
                                 reduced * 0.0013888889F)))));
    return polynomial * powerOfTwo(exponent);
}

extern "C" float erff(float value) {
    constexpr float p = 0.3275911F;
    constexpr float a1 = 0.254829592F;
    constexpr float a2 = -0.284496736F;
    constexpr float a3 = 1.421413741F;
    constexpr float a4 = -1.453152027F;
    constexpr float a5 = 1.061405429F;

    const bool negative = value < 0.0F;
    const float magnitude = negative ? -value : value;
    const float t = 1.0F / (1.0F + p * magnitude);
    const float polynomial =
        (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t;
    const float result =
        1.0F - polynomial * expf(-magnitude * magnitude);
    return negative ? -result : result;
}

extern "C" float rsqrtf(float value) {
    float root = 0.0F;
    __asm__ volatile("fsqrt.s %0, %1" : "=f"(root) : "f"(value));
    return 1.0F / root;
}
