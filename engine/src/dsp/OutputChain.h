#pragma once
#include "core/Params.h"
#include "dsp/Biquad.h"
#include "dsp/DelayLine.h"
#include "dsp/Limiter.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <cmath>

namespace vt {

// One output's processing: gain·polarity → delay → IIR (PEQ, HP, LP, AP) → FIR → limiter → mute.
// IIR/gain path: zero added latency. prepare() allocates; process() never does.
// Coefficient changes ramp linearly over kRamp samples; interpolating (a1, a2) between two
// stable filters stays stable because the biquad stability triangle is convex.
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
        for (int k = 0; k < kMaxIir; ++k) { cur[k] = target[k] = Biquad::identity(); state[k].reset(); }
        rampLeft = 0;
        if (conv) conv->prepare({ sr, juce::uint32(4096), 1 });
    }

    // Optional zero-latency convolution (owned by the engine, shared message queue).
    void setConvolution(juce::dsp::Convolution* c) { conv = c; }

    void process(const OutputParams& p, float* buf, int n, bool muteAll)
    {
        if (p.slotGen != lastSlotGen) { lastSlotGen = p.slotGen; resetState(); }
        const float g = std::pow(10.0f, p.gainDb / 20.0f) * (p.polarityInvert ? -1.0f : 1.0f);
        gain.setTargetValue(g);
        muteGain.setTargetValue((p.mute || muteAll) ? 0.0f : 1.0f);
        delay.setDelaySamples(int(std::lround(p.delayMs * 0.001 * sampleRate)));
        limiter.set(p.limThreshDb, p.limAttackMs, p.limReleaseMs, p.limiterOn);

        startRampIfChanged(p);

        // Pass 1: gain · polarity → delay → IIR bank
        for (int i = 0; i < n; ++i) {
            double y = delay.process(buf[i] * gain.getNextValue());
            if (rampLeft > 0) stepRamp();
            for (int k = 0; k < kMaxIir; ++k)
                if (active[k]) y = state[k].process(cur[k], y);
            buf[i] = float(y);
        }
        // Pass 2: FIR (block-based convolution)
        if (p.firOn && conv != nullptr) {
            float* chans[1] = { buf };
            juce::dsp::AudioBlock<float> block(chans, 1, size_t(n));
            conv->process(juce::dsp::ProcessContextReplacing<float>(block));
        }
        // Pass 3: limiter → mute
        float pk = 0.0f;
        for (int i = 0; i < n; ++i) {
            const float y = limiter.process(buf[i]) * muteGain.getNextValue();
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
    static constexpr int kRamp = 256;

    // The slot now belongs to another output: forget the previous one's audio (delay line,
    // filter memory) and fade in from silence. Rare (patch edits only).
    void resetState()
    {
        delay.clear();
        for (int k = 0; k < kMaxIir; ++k) { cur[k] = target[k] = Biquad::identity(); state[k].reset(); active[k] = false; }
        rampLeft = 0;
        limiter.prepare(sampleRate);
        gain.setCurrentAndTargetValue(0.0f);
        muteGain.setCurrentAndTargetValue(0.0f);
    }
    uint32_t lastSlotGen = 0;

    void startRampIfChanged(const OutputParams& p)
    {
        bool changed = false;
        for (int k = 0; k < kMaxIir; ++k) changed |= !(p.iir[k] == target[k]);
        if (!changed) return;
        for (int k = 0; k < kMaxIir; ++k) {
            target[k] = p.iir[k];
            const Biquad &a = cur[k], &b = target[k];
            step[k] = { (b.b0 - a.b0) / kRamp, (b.b1 - a.b1) / kRamp, (b.b2 - a.b2) / kRamp,
                        (b.a1 - a.a1) / kRamp, (b.a2 - a.a2) / kRamp };
            active[k] = !a.isIdentity() || !b.isIdentity();
        }
        rampLeft = kRamp;
    }

    void stepRamp()
    {
        if (--rampLeft == 0) {
            for (int k = 0; k < kMaxIir; ++k) {
                cur[k] = target[k]; // land exactly on the target
                active[k] = !cur[k].isIdentity();
                if (!active[k]) state[k].reset();
            }
            return;
        }
        for (int k = 0; k < kMaxIir; ++k) {
            if (!active[k]) continue;
            auto& c = cur[k];
            const auto& d = step[k];
            c.b0 += d.b0; c.b1 += d.b1; c.b2 += d.b2; c.a1 += d.a1; c.a2 += d.a2;
        }
    }

    Biquad cur[kMaxIir], target[kMaxIir], step[kMaxIir];
    BiquadState state[kMaxIir];
    bool active[kMaxIir] {};
    int rampLeft = 0;
    juce::dsp::Convolution* conv = nullptr;

    double sampleRate = 48000.0;
    DelayLine delay;
    Limiter limiter;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gain, muteGain;
    std::atomic<float> peakHold { 0.0f }, grHold { 0.0f };
};

} // namespace vt
