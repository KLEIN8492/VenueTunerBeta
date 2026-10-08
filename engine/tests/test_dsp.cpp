#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/PinkNoise.h"
#include "dsp/DelayLine.h"
#include "dsp/Limiter.h"
#include "dsp/OutputChain.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;
static constexpr double kSr = 48000.0;

TEST_CASE("Pink noise has equal energy per octave (-3 dB/oct PSD) and unit RMS")
{
    vt::PinkNoise pn;
    constexpr int order = 14, N = 1 << order, frames = 128;
    juce::dsp::FFT fft(order);
    std::vector<float> win(N), buf(2 * N);
    for (int i = 0; i < N; ++i) win[i] = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * i / N);
    std::vector<double> psd(N / 2 + 1, 0.0);
    double sumSq = 0;
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < N; ++i) { float s = pn.next(); sumSq += s * s; buf[i] = s * win[i]; }
        std::fill(buf.begin() + N, buf.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform(buf.data(), true);
        for (int k = 0; k <= N / 2; ++k) psd[k] += double(buf[k]) * buf[k];
    }
    const double rms = std::sqrt(sumSq / (double(N) * frames));
    CHECK_THAT(20 * std::log10(rms), WithinAbs(0.0, 0.5));

    auto bandDb = [&](double fc) {
        double lo = fc / std::sqrt(2.0), hi = fc * std::sqrt(2.0), e = 0;
        for (int k = int(lo * N / kSr); k <= int(hi * N / kSr); ++k) e += psd[k];
        return 10 * std::log10(e);
    };
    const double ref = bandDb(1000);
    for (double fc : {63.0, 125.0, 250.0, 500.0, 2000.0, 4000.0, 8000.0})
        CHECK_THAT(bandDb(fc) - ref, WithinAbs(0.0, 1.0));
}

TEST_CASE("DelayLine is sample-exact once settled")
{
    vt::DelayLine d;
    d.prepare(kSr, 48000);
    d.setDelaySamples(100);
    for (int i = 0; i < 5000; ++i) d.process(0.0f); // let crossfade settle
    std::vector<float> out(300);
    for (int i = 0; i < 300; ++i) out[i] = d.process(i == 0 ? 1.0f : 0.0f);
    CHECK(out[100] == 1.0f);
    CHECK(out[99] == 0.0f);
    CHECK(out[101] == 0.0f);
}

TEST_CASE("DelayLine changes delay without clicks")
{
    vt::DelayLine d;
    d.prepare(kSr, 48000);
    float prev = 0, maxJump = 0;
    for (int i = 0; i < 48000; ++i) {
        if (i == 10000) d.setDelaySamples(240); // half a period of 100 Hz: worst case
        float y = d.process(0.5f * std::sin(2.0 * juce::MathConstants<double>::pi * 100.0 * i / kSr));
        if (i > 0) maxJump = std::max(maxJump, std::abs(y - prev));
        prev = y;
    }
    CHECK(maxJump < 0.02f);
}

TEST_CASE("Limiter with zero attack never exceeds threshold")
{
    vt::Limiter lim;
    lim.prepare(kSr);
    lim.set(-6.0f, 0.0f, 50.0f, true);
    const float thr = std::pow(10.0f, -6.0f / 20.0f);
    float maxOut = 0;
    for (int i = 0; i < 48000; ++i) {
        float x = 4.0f * std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * i / kSr); // +12 dBFS
        maxOut = std::max(maxOut, std::abs(lim.process(x)));
    }
    CHECK(maxOut <= thr * 1.0012f);
    CHECK(lim.gainReductionDb() < -10.0f);
}

TEST_CASE("OutputChain gain, polarity and mute")
{
    vt::OutputChain chain;
    chain.prepare(kSr, 512);
    vt::OutputParams p {};
    p.gainDb = -6.0f;
    std::vector<float> buf(4800);

    auto run = [&](bool muteAll) {
        std::fill(buf.begin(), buf.end(), 1.0f);
        chain.process(p, buf.data(), int(buf.size()), muteAll);
        return buf.back();
    };
    run(false);
    CHECK_THAT(run(false), WithinAbs(0.5012, 0.001));
    p.polarityInvert = true;
    run(false);
    CHECK_THAT(run(false), WithinAbs(-0.5012, 0.001));
    p.mute = true;
    run(false); // 100 ms > 20 ms ramp
    CHECK(run(false) == 0.0f);
    p.mute = false;
    run(true);
    CHECK(run(true) == 0.0f);
}

TEST_CASE("OutputChain IIR: bell boosts, ramps without clicks, identity is bit-exact")
{
    vt::OutputChain chain;
    chain.prepare(kSr, 512);
    vt::OutputParams p {};
    p.limiterOn = false;
    std::vector<float> buf(480);
    double phase = 0;
    auto run = [&](double freq, float amp, float* maxJump = nullptr) {
        float peak = 0, prev = buf.back();
        for (int b = 0; b < 100; ++b) {
            for (auto& x : buf) { x = amp * float(std::sin(phase)); phase += 2 * M_PI * freq / kSr; }
            chain.process(p, buf.data(), int(buf.size()), false);
            for (float y : buf) {
                if (b > 50) peak = std::max(peak, std::abs(y));
                if (maxJump) *maxJump = std::max(*maxJump, std::abs(y - prev));
                prev = y;
            }
        }
        return peak;
    };
    run(1000, 0.25f); // settle the gain ramp
    p.iir[vt::kIirPeq] = vt::design::bell(1000, 1.0, 6.0, kSr);
    CHECK_THAT(run(1000, 0.25f) / 0.25f, WithinAbs(1.9953, 0.01));

    // Jump from +6 to +12 dB mid-stream: no click.
    float jump = 0;
    p.iir[vt::kIirPeq] = vt::design::bell(100, 1.0, 0.1, kSr);
    run(100, 0.5f);
    p.iir[vt::kIirPeq] = vt::design::bell(100, 1.0, 12.0, kSr);
    run(100, 0.5f, &jump);
    CHECK(jump < 0.05f);

    // All-identity sections: output equals input exactly.
    vt::OutputChain plain;
    plain.prepare(kSr, 512);
    vt::OutputParams q {};
    q.limiterOn = false;
    std::vector<float> a(4800, 0.3f);
    for (int i = 0; i < 10; ++i) { std::fill(a.begin(), a.end(), 0.3f); plain.process(q, a.data(), int(a.size()), false); }
    CHECK(a.back() == 0.3f);
}
