#pragma once
#include <vector>

namespace vt {

struct DecayBand
{
    double fc = 0;
    double edt = -1, t20 = -1, t30 = -1; // seconds, −1 when the decay range wasn't reached
    std::vector<float> curveDb;          // Schroeder decay, one point per 10 ms
};

// Octave-band reverberation times (63 Hz – 8 kHz) from an impulse response that starts at
// the direct sound: band-pass, Schroeder backward integration with noise compensation and
// truncation, linear fits over 0…−10 (EDT), −5…−25 (T20), −5…−35 dB (T30).
std::vector<DecayBand> roomDecay(const std::vector<float>& ir, double sampleRate);

} // namespace vt
