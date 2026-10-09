#pragma once
#include "measure/Analyzer.h"

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

inline bool captureAccepted(const Analyzer::Result& r, double fLo, double fHi, double minQuality = 0.8)
{
    return r.delayValid && r.frames > 0 && captureQuality(r, fLo, fHi) >= minQuality;
}

} // namespace vt
