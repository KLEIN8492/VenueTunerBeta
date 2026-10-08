#pragma once
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace vt {

// Real-time analyzer of the measurement mic: fractional-octave band levels (dBFS RMS),
// 8192-point Hann Welch spectrum with ~1 s exponential averaging. Analysis thread only.
class Rta
{
public:
    struct Result { std::vector<float> freq, levelDb; };

    void prepare(double sampleRate);
    void setResolution(int bandsPerOctave) { resolution.store(bandsPerOctave == 6 ? 6 : 3); }
    void process(const float* x, int n);
    Result result() const { std::lock_guard<std::mutex> l(lock); return latest; }

private:
    static constexpr int kOrder = 13, N = 1 << kOrder;
    void frame();

    double sr = 48000;
    juce::dsp::FFT fft { kOrder };
    std::vector<float> window, ring, buf;
    std::vector<double> power;
    double windowPower = 1;
    size_t writePos = 0, sinceFrame = 0;
    int frames = 0;
    std::atomic<int> resolution { 3 };
    mutable std::mutex lock;
    Result latest;
};

} // namespace vt
