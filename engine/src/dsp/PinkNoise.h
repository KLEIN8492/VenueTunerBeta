#pragma once
#include <cstdint>

namespace vt {

// Pink noise via Paul Kellet's refined filter on uniform white noise (xorshift32).
// Output is normalised to ~unit RMS; equal energy per octave from ~20 Hz to 20 kHz.
class PinkNoise
{
public:
    void setSeed(uint32_t s) { state = s ? s : 0x9E3779B9u; }

    float next()
    {
        const float white = nextWhite();
        b0 = 0.99886f * b0 + white * 0.0555179f;
        b1 = 0.99332f * b1 + white * 0.0750759f;
        b2 = 0.96900f * b2 + white * 0.1538520f;
        b3 = 0.86650f * b3 + white * 0.3104856f;
        b4 = 0.55000f * b4 + white * 0.5329522f;
        b5 = -0.7616f * b5 - white * 0.0168980f;
        const float pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f;
        b6 = white * 0.115926f;
        return pink * kNorm;
    }

private:
    float nextWhite()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(static_cast<int32_t>(state)) * (1.0f / 2147483648.0f);
    }

    static constexpr float kNorm = 0.5629f; // calibrated so RMS ≈ 1 (see test_dsp)
    uint32_t state = 0x9E3779B9u;
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
};

} // namespace vt
