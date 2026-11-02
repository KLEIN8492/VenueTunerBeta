#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vt {

// Feed-forward peak limiter. This box sits between a console and a PA, so the threshold is a
// promise, not a target:
//  - attack 0 ms: no look-ahead, zero latency; the gain drops instantly on the sample that needs
//    it, so the output can never exceed the threshold.
//  - attack > 0 ms: a smoothed gain drop needs to start BEFORE the peak, or the peak gets through
//    (a plain attack coefficient overshoots a transient by many dB). The audio is held back by
//    L = min(attack, 10 ms) samples of look-ahead; the detector takes the minimum required gain
//    over the last L+1 samples, applies the release, then averages it over L samples. Every value
//    in that average already "saw" the peak, so the gain applied to the peak is at most what it
//    requires: no overshoot by construction, with an L-sample smooth attack. OutputChain borrows
//    the look-ahead from the output's delay line, so it only adds latency when the output delay
//    is shorter than L.
// A safety clip at the threshold backs this up for the 50 ms after the look-ahead length changes
// (the delay taps crossfade then); safetyClips() counts how often it acted (0 in normal use).
// prepare() allocates; process() never does.
class Limiter
{
public:
    static constexpr float kMaxLookaheadMs = 10.0f;

    void prepare(double sr)
    {
        sampleRate = sr;
        updateCoefs();
        gain = 1.0f;
        minGain = 1.0f;
        const int maxLa = int(std::ceil(kMaxLookaheadMs * 0.001 * sr));
        size_t size = 1;
        while (size < size_t(maxLa) + 2) size <<= 1;
        mask = size - 1;
        audio.assign(size, 0.0f);
        req.assign(size, 1.0f);
        held.assign(size, 1.0f);
        window.assign(size, 0);
        fadeLen = std::max(1, int(std::lround(0.05 * sr)));
        pos = 0;
        lookahead = targetLookahead();
        startLookahead(lookahead, 1.0f);
        fadePos = -1;
    }

    void set(float thresholdDb, float attackMs, float releaseMs, bool isEnabled)
    {
        const bool enabledChanged = isEnabled != enabled;
        enabled = isEnabled;
        if (thresholdDb != lastThreshDb || attackMs != attack || releaseMs != release) { // called every block
            lastThreshDb = thresholdDb;
            threshold = std::pow(10.0f, thresholdDb / 20.0f);
            attack = attackMs;
            release = releaseMs;
            updateCoefs();
        } else if (!enabledChanged) {
            return;
        }
        const int want = targetLookahead();
        if (want != lookahead) { // rare (attack edit, limiter on/off): crossfade the audio tap
            fadeFrom = lookahead;
            fadePos = 0;
            lookahead = want;
            startLookahead(lookahead, lookahead > 0 || fadeFrom > 0 ? currentGain() : 1.0f);
        }
    }

    // Look-ahead (samples) the limiter holds the audio back by; the output chain borrows it
    // from the output delay. 0 = zero-latency mode.
    int lookaheadSamples() const { return lookahead; }

    float process(float x)
    {
        if (!std::isfinite(x)) x = 0.0f;
        const float a = std::abs(x);
        const float want = enabled && a > threshold ? threshold / a : 1.0f;
        // The history is kept even in zero-latency mode, so switching to look-ahead has real
        // audio in its buffer to crossfade to, not stale samples.
        audio[pos & mask] = x;
        req[pos & mask] = want;

        if (lookahead == 0 && fadePos < 0) { // zero latency: the original limiter, untouched
            ++pos;
            if (!std::isfinite(gain)) gain = 1.0f;
            if (!enabled) { gain = 1.0f; return x; }
            gain += (want - gain) * (want < gain ? attackCoef : releaseCoef);
            minGain = std::min(minGain, gain);
            return x * gain;
        }

        const int L = std::max(1, lookahead);
        // Sliding minimum of the required gain over the last L+1 samples (monotonic deque).
        while (dqTail != dqHead && req[window[(dqTail - 1) & mask] & mask] >= want) --dqTail;
        window[dqTail++ & mask] = pos;
        while (window[dqHead & mask] + uint64_t(L) < pos) ++dqHead;
        const float h = req[window[dqHead & mask] & mask];
        // Release (instant down, smooth up): never above h, so the average below stays safe.
        hold = h < hold ? h : hold + (h - hold) * releaseCoef;
        boxSum += double(hold) - double(held[(pos - uint64_t(L)) & mask]);
        held[pos & mask] = hold;
        const float g = float(boxSum / L);

        float y = audio[(pos - uint64_t(lookahead)) & mask];
        if (fadePos >= 0) { // look-ahead length changed: crossfade old tap → new tap
            const float k = float(fadePos) / float(fadeLen);
            y = (1.0f - k) * audio[(pos - uint64_t(fadeFrom)) & mask] + k * y;
            if (++fadePos > fadeLen) { fadePos = -1; if (lookahead == 0) gain = g; }
        }
        ++pos;
        y *= enabled ? g : 1.0f;
        if (enabled) {
            minGain = std::min(minGain, g);
            if (std::abs(y) > threshold) { y = std::copysign(threshold, y); ++clips; } // safety net only
        }
        return y;
    }

    // Lowest gain since last call, in dB (<= 0). Read by the control thread via OutputChain.
    float gainReductionDb()
    {
        const float g = minGain;
        minGain = 1.0f;
        return 20.0f * std::log10(std::max(g, 1e-6f));
    }

    // How many samples the safety clip had to catch (tests / diagnostics).
    uint64_t safetyClips() const { return clips; }

private:
    int targetLookahead() const
    {
        if (!enabled || attack <= 0.0f) return 0;
        return std::max(1, int(std::lround(std::min(attack, kMaxLookaheadMs) * 0.001 * sampleRate)));
    }

    float currentGain() const { return lookahead == 0 && fadePos < 0 ? gain : hold; }

    // (Re)start the look-ahead detector at a uniform gain g.
    void startLookahead(int L, float g)
    {
        dqHead = dqTail = 0;
        hold = g;
        std::fill(held.begin(), held.end(), g);
        boxSum = double(g) * std::max(1, L);
    }

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

    // Look-ahead state (buffers sized for 10 ms at the current rate in prepare()).
    std::vector<float> audio, req, held;
    std::vector<uint64_t> window; // deque of sample positions, indexed by dqHead/dqTail & mask
    uint64_t mask = 0, pos = 0, dqHead = 0, dqTail = 0, clips = 0;
    int lookahead = 0, fadeFrom = 0, fadePos = -1, fadeLen = 1;
    float hold = 1.0f;
    double boxSum = 1.0;
};

} // namespace vt
