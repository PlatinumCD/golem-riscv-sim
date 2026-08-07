// Small freestanding f32 math subset for compiler-generated recurrent models.
// This is an execution-enablement library, not an accuracy model for analog
// compute.  It avoids a hosted libm dependency in bare-metal tile ELFs.

namespace {

float absolute(float value) {
    return value < 0.0F ? -value : value;
}

}  // namespace

extern "C" float expf(float value) {
    if (value > 88.0F) {
        return 3.402823466e38F;
    }
    if (value < -88.0F) {
        return 0.0F;
    }
    // exp(x) = 2^(x / ln(2)); a fifth-order local approximation is adequate
    // for activation functions in this fixed functional smoke harness.
    constexpr float kInvLn2 = 1.44269504089F;
    constexpr float kLn2 = 0.69314718056F;
    const int exponent = static_cast<int>(value * kInvLn2);
    const float fraction = value - static_cast<float>(exponent) * kLn2;
    const float polynomial = 1.0F + fraction * (
        1.0F + fraction * (0.5F + fraction * (
            0.16666667F + fraction * (0.04166667F + fraction * 0.00833333F))));
    int scaled = exponent + 127;
    if (scaled <= 0) {
        return 0.0F;
    }
    if (scaled >= 255) {
        return 3.402823466e38F;
    }
    const unsigned int bits = static_cast<unsigned int>(scaled) << 23U;
    const float scale = __builtin_bit_cast(float, bits);
    return polynomial * scale;
}

extern "C" float tanhf(float value) {
    if (value > 8.0F) {
        return 1.0F;
    }
    if (value < -8.0F) {
        return -1.0F;
    }
    const float exponential = expf(2.0F * value);
    return (exponential - 1.0F) / (exponential + 1.0F);
}

extern "C" float sqrtf(float value) {
    if (value <= 0.0F) {
        return 0.0F;
    }
    float estimate = value >= 1.0F ? value : 1.0F;
    for (int iteration = 0; iteration < 8; ++iteration) {
        estimate = 0.5F * (estimate + value / estimate);
    }
    return estimate;
}

extern "C" float rsqrtf(float value) {
    if (value <= 0.0F) {
        return 0.0F;
    }
    return 1.0F / sqrtf(value);
}

extern "C" float erff(float value) {
    // Abramowitz-Stegun 7.1.26, maximum error below 1.5e-7.
    const float sign = value < 0.0F ? -1.0F : 1.0F;
    const float x = absolute(value);
    const float t = 1.0F / (1.0F + 0.3275911F * x);
    const float polynomial = (((((1.061405429F * t - 1.453152027F) * t +
        1.421413741F) * t - 0.284496736F) * t + 0.254829592F) * t);
    return sign * (1.0F - polynomial * expf(-x * x));
}
