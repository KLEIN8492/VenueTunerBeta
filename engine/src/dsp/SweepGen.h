#pragma once
#include <cmath>
#include <cstdint>

namespace vt {

// Exponential (Farina) sine sweep 20 Hz → 20 kHz with 10 ms raised-cosine fades.
// Stateless: sample(i) can be computed anywhere (audio thread, tests).
struct SweepGen
{
    double sr = 48000.0;
    int64_t length = 0; // samples
    double f1 = 20.0, f2 = 20000.0;

    float sample(int64_t i) const
    {
        if (i < 0 || i >= length) return 0.0f;
        const double T = double(length) / sr, K = T / std::log(f2 / f1), t = double(i) / sr;
        double x = std::sin(2.0 * M_PI * f1 * K * (std::exp(t / K) - 1.0));
        const int64_t fade = int64_t(0.01 * sr);
        if (i < fade) x *= 0.5 - 0.5 * std::cos(M_PI * double(i) / double(fade));
        if (length - 1 - i < fade) x *= 0.5 - 0.5 * std::cos(M_PI * double(length - 1 - i) / double(fade));
        return float(x);
    }
};

} // namespace vt
