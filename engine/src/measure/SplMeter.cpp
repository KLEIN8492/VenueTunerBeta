#include "measure/SplMeter.h"
#include <cmath>

namespace vt {

namespace {
// Bilinear transform of an analog biquad (b2 s² + b1 s + b0)/(a2 s² + a1 s + a0).
Biquad bilinear(double b2, double b1, double b0, double a2, double a1, double a0, double sr)
{
    const double K = 2.0 * sr, K2 = K * K;
    const double A0 = a2 * K2 + a1 * K + a0;
    return { (b2 * K2 + b1 * K + b0) / A0, 2.0 * (b0 - b2 * K2) / A0, (b2 * K2 - b1 * K + b0) / A0,
             2.0 * (a0 - a2 * K2) / A0, (a2 * K2 - a1 * K + a0) / A0 };
}

// Pole frequency prewarped so the digital filter's corner lands exactly where the analog one is.
double w(double f, double sr) { return 2.0 * sr * std::tan(M_PI * f / sr); }

std::vector<Biquad> weighting(int kind, double sr)
{
    constexpr double f1 = 20.598997, f2 = 107.65265, f3 = 737.86223, f4 = 12194.217;
    const double w1 = w(f1, sr), w2 = w(f2, sr), w3 = w(f3, sr), w4 = w(f4, sr);
    std::vector<Biquad> s;
    if (kind == 0) return s;
    s.push_back(bilinear(1, 0, 0, 1, 2 * w1, w1 * w1, sr));                 // s² / (s + w1)²
    if (kind == 1) s.push_back(bilinear(1, 0, 0, 1, w2 + w3, w2 * w3, sr)); // s² / ((s + w2)(s + w3))
    s.push_back(bilinear(0, 0, 1, 1, 2 * w4, w4 * w4, sr));                 // 1 / (s + w4)²
    // Normalise to 0 dB at 1 kHz.
    const double g = std::abs(chainResponse(s.data(), int(s.size()), 1000.0, sr));
    s[0].b0 /= g; s[0].b1 /= g; s[0].b2 /= g;
    return s;
}

double db(double ms) { return ms > 0 ? 10.0 * std::log10(ms) : -200.0; }
}

void SplMeter::prepare(double sampleRate)
{
    std::lock_guard<std::mutex> l(lock);
    sr = sampleRate;
    for (int k = 0; k < 3; ++k) {
        filters[size_t(k)] = weighting(k, sr);
        states[size_t(k)].assign(filters[size_t(k)].size(), {});
    }
    aFast = 1.0 - std::exp(-1.0 / (0.125 * sr));
    aSlow = 1.0 - std::exp(-1.0 / (1.0 * sr));
    msFast = msSlow = leqSum = {};
    leqCount = 0;
    peak = 0;
}

void SplMeter::process(const float* x, int n)
{
    std::lock_guard<std::mutex> l(lock);
    for (int i = 0; i < n; ++i) {
        peak = std::max(peak, double(std::abs(x[i])));
        for (size_t k = 0; k < 3; ++k) {
            double y = x[i];
            for (size_t s = 0; s < filters[k].size(); ++s) y = states[k][s].process(filters[k][s], y);
            const double y2 = y * y;
            msFast[k] += aFast * (y2 - msFast[k]);
            msSlow[k] += aSlow * (y2 - msSlow[k]);
            leqSum[k] += y2;
        }
    }
    leqCount += n;
}

SplMeter::Reading SplMeter::reading()
{
    std::lock_guard<std::mutex> l(lock);
    Reading r;
    for (size_t k = 0; k < 3; ++k) {
        r.fast[k] = db(msFast[k]);
        r.slow[k] = db(msSlow[k]);
        r.leq[k] = leqCount ? db(leqSum[k] / double(leqCount)) : -200.0;
    }
    r.peakZ = peak > 0 ? 20.0 * std::log10(peak) : -200.0;
    r.leqSeconds = double(leqCount) / sr;
    peak = 0;
    return r;
}

void SplMeter::resetLeq()
{
    std::lock_guard<std::mutex> l(lock);
    leqSum = {};
    leqCount = 0;
}

} // namespace vt
