#pragma once
#include "dsp/Biquad.h"
#include <array>
#include <mutex>

namespace vt {

// Sound level meter on the measurement mic (analysis thread). All values in dBFS RMS;
// the dispatcher adds the mic's calibration offset to get dB SPL.
// Index 0 = Z (flat), 1 = A, 2 = C (IEC 61672 weightings).
class SplMeter
{
public:
    struct Reading
    {
        std::array<double, 3> fast { -200, -200, -200 }, slow { -200, -200, -200 }, leq { -200, -200, -200 };
        double peakZ = -200;   // since last reading()
        double leqSeconds = 0;
    };

    void prepare(double sampleRate);
    void process(const float* x, int n);
    Reading reading(); // also resets the peak hold
    void resetLeq();

private:
    std::array<std::vector<Biquad>, 3> filters;
    std::array<std::vector<BiquadState>, 3> states;
    std::array<double, 3> msFast {}, msSlow {}, leqSum {};
    double aFast = 0, aSlow = 0, peak = 0, sr = 48000;
    long long leqCount = 0;
    std::mutex lock;
};

} // namespace vt
