#pragma once
#include "core/Params.h"
#include "dsp/DelayLine.h"
#include "dsp/Limiter.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cmath>

namespace vt {

// One output's processing: gain·polarity → delay → limiter → mute.
// IIR/gain path: zero added latency. prepare() allocates; process() never does.
class OutputChain
{
public:
    void prepare(double sr, int /*maxBlock*/)
    {
        sampleRate = sr;
        delay.prepare(sr, int(sr)); // up to 1 s
        limiter.prepare(sr);
        gain.reset(sr, 0.02);
        muteGain.reset(sr, 0.02);
        gain.setCurrentAndTargetValue(0.0f);
        muteGain.setCurrentAndTargetValue(0.0f);
    }

    void process(const OutputParams& p, float* buf, int n, bool muteAll)
    {
        const float g = std::pow(10.0f, p.gainDb / 20.0f) * (p.polarityInvert ? -1.0f : 1.0f);
        gain.setTargetValue(g);
        muteGain.setTargetValue((p.mute || muteAll) ? 0.0f : 1.0f);
        delay.setDelaySamples(int(std::lround(p.delayMs * 0.001 * sampleRate)));
        limiter.set(p.limThreshDb, p.limAttackMs, p.limReleaseMs, p.limiterOn);

        float pk = 0.0f;
        for (int i = 0; i < n; ++i) {
            float y = delay.process(buf[i] * gain.getNextValue());
            y = limiter.process(y) * muteGain.getNextValue();
            buf[i] = y;
            pk = std::max(pk, std::abs(y));
        }
        if (pk > peakHold.load(std::memory_order_relaxed))
            peakHold.store(pk, std::memory_order_relaxed);
        const float gr = limiter.gainReductionDb();
        if (gr < grHold.load(std::memory_order_relaxed))
            grHold.store(gr, std::memory_order_relaxed);
    }

    // Control-thread readers: return max since the last call and reset.
    float takePeak() { return peakHold.exchange(0.0f, std::memory_order_relaxed); }
    float takeGainReductionDb() { return grHold.exchange(0.0f, std::memory_order_relaxed); }

private:
    double sampleRate = 48000.0;
    DelayLine delay;
    Limiter limiter;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gain, muteGain;
    std::atomic<float> peakHold { 0.0f }, grHold { 0.0f };
};

} // namespace vt
