#pragma once
#include <juce_dsp/juce_dsp.h>
#include <memory>
#include <vector>

namespace vt {

// Pseudorandom pink noise, built like Smaart's "pseudorandom" generator signal:
//   - one period of 2^19 samples (~10.9 s at 48 kHz), played in a loop, so every analysis frame
//     sees a stationary, exactly pink excitation;
//   - band-limited 10 Hz – 22.4 kHz (SMPTE ST 2095-1 bandwidth): no DC, no infrasound eating
//     sub excursion and limiter headroom;
//   - crest factor reduced to ~12 dB (random pink is ~14 dB) without touching the spectrum:
//     clip the peaks, then restore the exact pink magnitudes, and repeat.
// Unit RMS, so a level in dBFS RMS stays what it says. The table is built in prepare() (never on
// the audio thread) and cached per sample rate.
class PinkPeriodic
{
public:
    static constexpr int kOrder = 19;
    static constexpr double kLowHz = 10.0, kHighHz = 22400.0, kCrestDb = 11.75;

    void prepare(double sampleRate);
    float next() noexcept
    {
        const float v = (*data)[pos];
        pos = (pos + 1) & (data->size() - 1);
        return v;
    }
    const std::vector<float>& table() const { return *data; }

private:
    static std::shared_ptr<const std::vector<float>> build(double sampleRate);
    std::shared_ptr<const std::vector<float>> data = std::make_shared<const std::vector<float>>(size_t(1) << kOrder, 0.0f);
    size_t pos = 0;
};

} // namespace vt
