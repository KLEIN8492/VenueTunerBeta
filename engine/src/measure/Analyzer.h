#pragma once
#include "measure/MicCal.h"
#include "rt/SpscFifo.h"
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <complex>
#include <mutex>
#include <vector>

namespace vt {

// Dual-channel FFT analyzer: reference (generator / loopback / console) vs measurement
// (mic). Produces transfer function, coherence and impulse response, and finds the
// delay between the two so the reference can be aligned automatically.
//
// Threading: pushRef/pushMeas are called from the audio thread (wait-free).
// process() runs on the analysis thread. Setters and result() may be called from any
// non-audio thread.
class Analyzer
{
public:
    struct Result
    {
        std::vector<float> freq, magDb, phaseDeg, coherence; // log-spaced display points
        std::vector<float> ir;                                // impulse response excerpt
        int irStart = 0;                                      // lag (samples) of ir[0]
        double delaySamples = 0.0;                            // total ref → meas delay
        bool delayValid = false;
        float sampleRate = 48000.0f;
        int frames = 0;                                       // averaged frames so far
    };

    Analyzer();
    void prepare(double sampleRate); // tests / before streaming: resets immediately
    // Engine: safe while streaming — the analysis thread applies it on its next process().
    void requestReset(double sampleRate) { pendingSr.store(sampleRate); resetRequested.store(true); }

    void pushRef(const float* data, int n) { refFifo.push(data, size_t(n)); }
    void pushMeas(const float* data, int n) { measFifo.push(data, size_t(n)); }

    // Analysis thread: consume pending samples; returns true when a new result exists.
    bool process();

    void reset() { resetRequested.store(true); }
    void setAveraging(int frames) { averaging.store(std::max(1, frames)); }
    void setSmoothing(int octaveFraction) { smoothing.store(std::max(0, octaveFraction)); }
    void setMicCal(const MicCal& c) { std::lock_guard<std::mutex> l(calLock); cal = c; }

    Result result() const { std::lock_guard<std::mutex> l(resultLock); return latest; }
    double delaySamples() const { return delayTotal.load(); }

    static constexpr int kFftOrder = 14;               // TF frames: 16384 points
    static constexpr int kDelayOrder = 16;             // delay finder block: 65536
    static constexpr int kHistory = 1 << 19;           // ~10.9 s at 48 kHz

private:
    void doReset();
    void findDelay();
    void addFrame(int64_t frameEnd);
    void buildResult();
    float hist(const std::vector<float>& h, int64_t index) const { return h[size_t(index) & (kHistory - 1)]; }

    double sr = 48000.0;
    SpscFifo<float> refFifo { 1 << 18 }, measFifo { 1 << 18 };
    std::vector<float> refHist, measHist, scratch;
    int64_t total = 0, nextFrameEnd = 0, nextDelayAt = 0;

    juce::dsp::FFT fft { kFftOrder }, delayFft { kDelayOrder + 1 };
    std::vector<float> window, bufX, bufY, bigX, bigY;
    std::vector<double> sxx, syy;
    std::vector<std::complex<double>> sxy;
    int frameCount = 0;

    std::atomic<double> delayTotal { 0.0 };
    bool delayValid = false;
    int alignInt = 0; // integer alignment used by every averaged frame
    std::vector<float> irExcerpt;
    int irStart = 0;

    std::atomic<bool> resetRequested { false };
    std::atomic<double> pendingSr { 0.0 };
    std::atomic<int> averaging { 16 }, smoothing { 24 };

    mutable std::mutex calLock;
    MicCal cal;
    mutable std::mutex resultLock;
    Result latest;
};

} // namespace vt
