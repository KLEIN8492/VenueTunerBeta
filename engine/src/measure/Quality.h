#pragma once
#include "measure/Analyzer.h"
#include <algorithm>
#include <vector>

namespace vt {

// Quality of a capture = mean coherence over the band the source covers. Low coherence means
// something else was audible (talking, a door, HVAC bursts) or the source is too quiet.
inline double captureQuality(const Analyzer::Result& r, double fLo, double fHi)
{
    double sum = 0;
    int n = 0;
    for (size_t i = 0; i < r.freq.size() && i < r.coherence.size(); ++i)
        if (r.freq[i] >= fLo && r.freq[i] <= fHi) { sum += r.coherence[i]; ++n; }
    return n ? sum / n : 0.0;
}

// Accepted = a clear arrival, enough averages for coherence to mean something (one frame
// always reads 100 %), and good coherence over the band.
inline bool captureAccepted(const Analyzer::Result& r, double fLo, double fHi, double minQuality = 0.8)
{
    return r.delayValid && r.effectiveFrames >= Analyzer::kMinCaptureFrames && captureQuality(r, fLo, fHi) >= minQuality;
}

// Arrival time from the delay finder, trusted only when three successive independent
// estimates (one per second) agree — a single estimate can land on a reflection.
struct ArrivalVote
{
    int seen = 0;
    std::vector<double> estimates;

    void clear() { seen = 0; estimates.clear(); }
    // Feed every result; only new delay-finder runs count. Returns true when one was added.
    bool add(const Analyzer::Result& r)
    {
        if (!r.delayValid || r.delayEstimates == seen) return false;
        seen = r.delayEstimates;
        estimates.push_back(r.delaySamples);
        return true;
    }
    // Median of the last three if they agree within `tolerance` samples.
    bool agreed(double tolerance, double& out) const
    {
        if (estimates.size() < 3) return false;
        std::vector<double> last(estimates.end() - 3, estimates.end());
        std::sort(last.begin(), last.end());
        if (last[2] - last[0] > tolerance) return false;
        out = last[1];
        return true;
    }
};

} // namespace vt
