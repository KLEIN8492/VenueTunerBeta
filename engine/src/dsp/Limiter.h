#pragma once
#include <algorithm>
#include <cmath>

namespace vt {

// Feed-forward peak limiter without look-ahead (zero latency). With 0 ms attack the
// output can never exceed the threshold; release recovers smoothly.
class Limiter
{
public:
    void prepare(double sr) { sampleRate = sr; updateCoefs(); gain = 1.0f; minGain = 1.0f; }

    void set(float thresholdDb, float attackMs, float releaseMs, bool isEnabled)
    {
        enabled = isEnabled;
        if (thresholdDb == lastThreshDb && attackMs == attack && releaseMs == release) return; // called every block
        lastThreshDb = thresholdDb;
        threshold = std::pow(10.0f, thresholdDb / 20.0f);
        attack = attackMs;
        release = releaseMs;
        updateCoefs();
    }

    float process(float x)
    {
        if (!std::isfinite(x)) return 0.0f;
        if (!std::isfinite(gain)) gain = 1.0f;
        if (!enabled) { gain = 1.0f; return x; }
        const float a = std::abs(x);
        const float want = a > threshold ? threshold / a : 1.0f;
        gain += (want - gain) * (want < gain ? attackCoef : releaseCoef);
        minGain = std::min(minGain, gain);
        return x * gain;
    }

    // Lowest gain since last call, in dB (<= 0). Read by the control thread via OutputChain.
    float gainReductionDb()
    {
        const float g = minGain;
        minGain = 1.0f;
        return 20.0f * std::log10(std::max(g, 1e-6f));
    }

private:
    void updateCoefs()
    {
        auto coef = [this](float ms) {
            return ms <= 0.0f ? 1.0f : 1.0f - std::exp(-1.0f / (0.001f * ms * float(sampleRate)));
        };
        attackCoef = coef(attack);
        releaseCoef = coef(release);
    }

    double sampleRate = 48000.0;
    float threshold = 1.0f, attack = 0.0f, release = 50.0f, lastThreshDb = 0.0f;
    float attackCoef = 1.0f, releaseCoef = 0.001f;
    float gain = 1.0f, minGain = 1.0f;
    bool enabled = false;
};

} // namespace vt
