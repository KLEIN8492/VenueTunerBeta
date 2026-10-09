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
    static constexpr int kWindows = 5;
    static constexpr int kWindowSeconds[kWindows] = { 1, 10, 60, 300, 900 }; // sliding Leq: 1 s … 15 min

    struct Reading
    {
        std::array<double, 3> fast { -200, -200, -200 }, slow { -200, -200, -200 }, impulse { -200, -200, -200 };
        std::array<double, 3> leq { -200, -200, -200 };      // since resetLeq()
        std::array<double, 3> maxFast { -200, -200, -200 };  // max of the Fast level since resetLeq()
        std::array<std::array<double, 3>, kWindows> leqWindow {}; // sliding windows (kWindowSeconds)
        double peakZ = -200;   // since last reading()
        double cPeak = -200;   // LCpeak: max |C-weighted| since resetLeq()
        double leqSeconds = 0;
    };

    void prepare(double sampleRate);
    void process(const float* x, int n);
    Reading reading(); // also resets the peak hold
    void resetLeq();

private:
    std::array<std::vector<Biquad>, 3> filters;
    std::array<std::vector<BiquadState>, 3> states;
    std::array<double, 3> msFast {}, msSlow {}, msImp {}, imp35 {}, leqSum {}, maxFast {};
    double aFast = 0, aSlow = 0, aImpRise = 0, aImpFall = 0, peak = 0, cPeak = 0, sr = 48000;
    // Sliding Leq: mean square of each completed second (ring of 900 s) + the current second.
    std::array<std::vector<double>, 3> secMs;
    std::array<double, 3> secSum {};
    long long secCount = 0;
    size_t secHead = 0, secFilled = 0;
    long long leqCount = 0;
    std::mutex lock;
};

} // namespace vt
