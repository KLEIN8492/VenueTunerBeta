#pragma once
#include "measure/MicCal.h"
#include "measure/RoomDecay.h"
#include "rt/SpscFifo.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
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
        // Frames actually inside the average of the longest window (capped by the averaging
        // setting). Coherence from fewer than ~8 is meaningless: one frame always reads 100 %.
        int effectiveFrames = 0;
        int delayEstimates = 0;                               // independent delay-finder runs since reset
        double autoDelaySamples = 0.0;                        // what the delay finder sees (even when locked)
        bool delayLocked = false;                             // delaySamples is the user's locked value
        std::vector<DecayBand> decay;                         // sweep only: octave-band RT60
    };

    Analyzer();
    void prepare(double sampleRate); // tests / before streaming: resets immediately
    // Engine: safe while streaming — the analysis thread applies it on its next process().
    void requestReset(double sampleRate) { pendingSr.store(sampleRate); resetRequested.store(true); }

    void pushRef(const float* data, int n) { refFifo.push(data, size_t(n)); }
    void pushMeas(const float* data, int n) { measFifo.push(data, size_t(n)); }
    // Audio thread: write both channels together. If either FIFO cannot take the whole
    // block, drop it on both (never a partial write) and flag an overflow so process()
    // can reset — independent drops would desynchronise ref and meas.
    void pushPair(const float* ref, const float* meas, int n)
    {
        const size_t need = size_t(std::max(0, n));
        if (need == 0) return;
        if (refFifo.space() < need || measFifo.space() < need) {
            overflow.store(true, std::memory_order_release);
            return;
        }
        refFifo.push(ref, need);
        measFifo.push(meas, need);
    }

    // Analysis thread: consume pending samples; returns true when a new result exists.
    bool process();

    void reset() { resetRequested.store(true); }
    void setAveraging(int frames) { averaging.store(std::max(1, frames)); }
    void setSmoothing(int octaveFraction) { smoothing.store(std::max(0, octaveFraction)); }
    // Phase is smoothed separately (Smaart's unlinked phase/magnitude smoothing): high-frequency
    // phase turns fast and needs fine resolution even when the magnitude is smoothed.
    void setPhaseSmoothing(int octaveFraction) { phaseSmoothing.store(std::max(0, octaveFraction)); }
    // Measurement delay: locked to the user's value instead of the delay finder's, so it can be
    // nudged until the phase is flat (e.g. at the sub/top crossover). Changes within half a
    // sample only move the phase; larger ones realign and restart the averaging.
    void setDelayLock(bool on, double samples)
    {
        lockSamples.store(samples);
        lockOn.store(on);
        lockPending.store(true);
    }
    double autoDelaySamples() const { return autoDelay.load(); }
    static constexpr int kPointsPerOctave = 48; // display grid: 1/48 octave, 20 Hz – 20 kHz
    // Multi-time-window (Smaart-style): long FFTs in the lows, short in the highs, so late
    // reflections don't comb the HF. false = one fixed 16k FFT. Applied on next reset.
    void setMultiWindow(bool on) { if (multiWindow.exchange(on) != on) resetRequested.store(true); }
    void setMicCal(const MicCal& c) { std::lock_guard<std::mutex> l(calLock); cal = c; }

    Result result() const { std::lock_guard<std::mutex> l(resultLock); return latest; }

    // Sweep capture: deconvolve the last `captureSamples` of history (pre-roll, the sweep of
    // `sweepSamples` and its tail) on the analysis thread. When done, sweepGeneration() increments.
    void requestSweep(int64_t captureSamples, int64_t sweepSamples)
    {
        sweepLength.store(sweepSamples);
        sweepRequest.store(captureSamples);
    }
    int sweepGeneration() const { return sweepGen.load(); }
    Result sweepResult() const { std::lock_guard<std::mutex> l(resultLock); return sweep; }
    double delaySamples() const { return delayTotal.load(); }

    // Reference and mic levels (dBFS RMS, ~300 ms): for matching the mic gain to the reference.
    double refLevelDb() const { const double v = refMs.load(); return v > 0 ? 10.0 * std::log10(v) : -200.0; }
    double micLevelDb() const { const double v = measMs.load(); return v > 0 ? 10.0 * std::log10(v) : -200.0; }

    static constexpr int kFixedOrder = 14;             // fixed mode: 16384 points
    static constexpr int kDelayOrder = 16;             // delay finder block: 65536
    static constexpr int kHistory = 1 << 20;           // ~21.8 s at 48 kHz, 10.9 s at 96 kHz
    static constexpr int kMinCaptureFrames = 8;        // averages needed before a capture is trusted

private:
    void doReset();
    void findDelay();
    struct Band
    {
        int order = 14, n = 1 << 14;
        double fLo = 0, fHi = 1e9; // display range served by this band
        int avgScale = 1;          // more frames for short windows: same averaging time
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, bufX, bufY;
        std::vector<double> sxx, syy;
        std::vector<std::complex<double>> sxy;
        int frameCount = 0;
        int64_t nextFrameEnd = 0;
        void clear();
    };
    void configureBands();
    void addFrame(Band&, int64_t frameEnd);
    void buildResult();
    void analyseSweep(int64_t length, int64_t sweepSamples);
    float hist(const std::vector<float>& h, int64_t index) const { return h[size_t(index) & (kHistory - 1)]; }

    double sr = 48000.0;
    SpscFifo<float> refFifo { 1 << 18 }, measFifo { 1 << 18 };
    std::vector<float> refHist, measHist, scratch;
    int64_t total = 0, nextDelayAt = 0;

    juce::dsp::FFT delayFft { kDelayOrder + 1 };
    std::vector<float> bigX, bigY;
    std::vector<Band> bands;

    std::atomic<double> delayTotal { 0.0 };
    bool delayValid = false;
    int delayCount = 0;
    int alignInt = 0; // integer alignment used by every averaged frame
    std::vector<float> irExcerpt;
    int irStart = 0;

    std::atomic<bool> resetRequested { false };
    std::atomic<double> pendingSr { 0.0 };
    std::atomic<int> averaging { 16 }, smoothing { 24 };
    std::atomic<bool> multiWindow { true };

    mutable std::mutex calLock;
    MicCal cal;
    mutable std::mutex resultLock;
    Result latest, sweep;
    std::atomic<double> refMs { 0.0 }, measMs { 0.0 };
    std::atomic<int64_t> sweepRequest { 0 }, sweepLength { 0 };
    std::atomic<int> sweepGen { 0 };
    std::atomic<bool> overflow { false };
    std::atomic<int> phaseSmoothing { 48 };
    std::atomic<bool> lockOn { false }, lockPending { false };
    std::atomic<double> lockSamples { 0.0 }, autoDelay { 0.0 };
    void applyLock();
};

} // namespace vt
