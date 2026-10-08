#pragma once
#include <algorithm>
#include <vector>

namespace vt {

struct Arrival
{
    int output;
    double arrivalMs;      // measured arrival, includes the output's current delay
    double currentDelayMs; // the output's delay setting during the measurement
};

struct AlignedDelay { int output; double delayMs; };

// Delays that make every measured output arrive together.
//   reference < 0  → align everything to the latest arrival (no output gets less delay).
//   reference = n  → align to output n's arrival; if that would need negative delay
//                    somewhere, everything (reference included) is shifted later.
inline std::vector<AlignedDelay> computeAlignment(const std::vector<Arrival>& in, int reference)
{
    if (in.empty()) return {};
    double target = in.front().arrivalMs;
    bool found = false;
    for (const auto& a : in) {
        if (reference >= 0 && a.output == reference) { target = a.arrivalMs; found = true; }
        if (reference < 0) target = std::max(target, a.arrivalMs);
    }
    if (reference >= 0 && !found) return computeAlignment(in, -1);

    std::vector<AlignedDelay> out;
    double minDelay = 0;
    for (const auto& a : in) {
        out.push_back({ a.output, a.currentDelayMs + (target - a.arrivalMs) });
        minDelay = std::min(minDelay, out.back().delayMs);
    }
    for (auto& o : out) o.delayMs -= minDelay; // never negative
    return out;
}

} // namespace vt
