#pragma once
#include "measure/Rta.h"
#include "measure/SplMeter.h"
#include "rt/SpscFifo.h"
#include <algorithm>
#include <atomic>
#include <vector>

namespace vt {

// Sound level meter + RTA of the measurement mic, on a feed of their own.
//
// Kept apart from the transfer-function analyzer on purpose: automatic jobs borrow the analyzer
// (latency calibration listens to a loopback cable, the matrix resets it for every group), and
// none of that may reach a 15-minute LAeq. The Leq only restarts when the mic changes, the
// sample rate changes or the user resets it — never because of a measurement.
//
// push() runs on the audio thread (wait-free); process() and the meters on the analysis thread.
class MicMeters
{
public:
    // Audio thread. `input` is the physical channel the samples come from (a different mic
    // starts a new measurement).
    void push(const float* x, int n, int input)
    {
        if (n <= 0) return;
        if (input != lastInput) { lastInput = input; newMic.store(true, std::memory_order_release); }
        if (fifo.space() < size_t(n)) { dropped.fetch_add(n, std::memory_order_relaxed); return; }
        fifo.push(x, size_t(n));
    }
    void setSampleRate(double sampleRate) { pendingSr.store(sampleRate); }

    // Analysis thread.
    void process()
    {
        if (const double p = pendingSr.exchange(0.0); p > 0.0 && p != sr) {
            sr = p;
            splMeter.prepare(sr);
            rtaMeter.prepare(sr);
            fifo.clear();
            dropped.store(0);
        }
        if (newMic.exchange(false)) {
            splMeter.resetLeq();
            dropped.store(0);
        }
        size_t n = fifo.available();
        if (sr <= 0.0) { fifo.clear(); return; }
        while (n > 0) {
            const size_t chunk = std::min(n, scratch.size());
            fifo.pop(scratch.data(), chunk);
            splMeter.process(scratch.data(), int(chunk));
            rtaMeter.process(scratch.data(), int(chunk));
            n -= chunk;
        }
    }

    SplMeter& spl() { return splMeter; }
    Rta& rta() { return rtaMeter; }
    void resetLeq() { splMeter.resetLeq(); dropped.store(0); }
    // Audio missing from the Leq because analysis couldn't keep up (since the last reset).
    double gapSeconds() const { return sr > 0 ? double(dropped.load()) / sr : 0.0; }

private:
    SpscFifo<float> fifo { 1 << 18 };
    std::vector<float> scratch = std::vector<float>(1 << 14);
    SplMeter splMeter;
    Rta rtaMeter;
    double sr = 0.0;
    std::atomic<double> pendingSr { 0.0 };
    std::atomic<bool> newMic { false };
    std::atomic<int64_t> dropped { 0 };
    int lastInput = -1; // audio thread only
};

} // namespace vt
