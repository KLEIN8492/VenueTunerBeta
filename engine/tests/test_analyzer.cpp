#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "measure/Analyzer.h"
#include "measure/MicCal.h"
#include "dsp/PinkNoise.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <deque>
#include <functional>
#include <vector>

using Catch::Matchers::WithinAbs;
static constexpr double kSr = 48000.0;
static constexpr double kPi = 3.14159265358979323846;

// Runs `seconds` of pink noise through `room` (meas = room(ref)) into an analyzer.
static vt::Analyzer::Result runRoom(vt::Analyzer& an, std::function<float(float)> room, double seconds = 6.0)
{
    vt::PinkNoise pn;
    constexpr int block = 512;
    std::vector<float> ref(block), meas(block);
    const int blocks = int(seconds * kSr / block);
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < block; ++i) { ref[i] = pn.next(); meas[i] = room(ref[i]); }
        an.pushRef(ref.data(), block);
        an.pushMeas(meas.data(), block);
        an.process();
    }
    return an.result();
}

// Integer delay + gain.
struct DelayGain {
    int d; float g; std::deque<float> q;
    DelayGain(int delay, float gain) : d(delay), g(gain), q(size_t(delay), 0.0f) {}
    float operator()(float x) { q.push_back(x); float y = q.front(); q.pop_front(); return g * y; }
};

static float meanOver(const vt::Analyzer::Result& r, const std::vector<float>& v, float f0, float f1)
{
    double s = 0; int n = 0;
    for (size_t i = 0; i < r.freq.size(); ++i)
        if (r.freq[i] >= f0 && r.freq[i] <= f1) { s += v[i]; ++n; }
    return float(s / n);
}

TEST_CASE("Analyzer recovers delay, gain and flat phase of a delayed copy")
{
    const bool mtw = GENERATE(true, false);
    vt::Analyzer an;
    an.setMultiWindow(mtw);
    an.prepare(kSr);
    auto r = runRoom(an, DelayGain(480, 0.5f));
    CHECK_THAT(an.delaySamples(), WithinAbs(480.0, 0.1));
    REQUIRE(r.freq.size() > 200);
    for (size_t i = 0; i < r.freq.size(); ++i) {
        if (r.freq[i] < 50 || r.freq[i] > 15000) continue;
        CHECK_THAT(r.magDb[i], WithinAbs(-6.02, 0.2));
        CHECK_THAT(r.phaseDeg[i], WithinAbs(0.0, 5.0));
        CHECK(r.coherence[i] > 0.98f);
    }
}

TEST_CASE("Analyzer finds a fractional delay")
{
    // 480.5-sample delay via a Hann-windowed sinc FIR
    std::vector<float> h(129);
    const double centre = 64.5;
    for (int k = 0; k < 129; ++k) {
        double x = k - centre;
        double s = std::abs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        h[size_t(k)] = float(s * (0.5 + 0.5 * std::cos(kPi * x / 65.0)));
    }
    std::deque<float> hist(129, 0.0f);
    DelayGain coarse(416, 1.0f); // 416 + 64.5 = 480.5
    vt::Analyzer an;
    an.prepare(kSr);
    auto r = runRoom(an, [&](float x) {
        hist.push_front(coarse(x)); hist.pop_back();
        float y = 0; for (int k = 0; k < 129; ++k) y += h[size_t(k)] * hist[size_t(k)];
        return y;
    });
    CHECK_THAT(an.delaySamples(), WithinAbs(480.5, 0.1));
    for (size_t i = 0; i < r.freq.size(); ++i)
        if (r.freq[i] > 50 && r.freq[i] < 10000)
            CHECK_THAT(r.phaseDeg[i], WithinAbs(0.0, 10.0)); // fractional part compensated
}

TEST_CASE("Coherence drops with uncorrelated noise")
{
    vt::PinkNoise other;
    other.setSeed(12345);
    DelayGain dg(100, 0.5f);
    vt::Analyzer an;
    an.prepare(kSr);
    auto r = runRoom(an, [&](float x) { return dg(x) + 0.5f * other.next(); });
    CHECK(meanOver(r, r.coherence, 100, 10000) < 0.7f);
}

TEST_CASE("Analyzer measures a one-pole lowpass at -3 dB at its cutoff")
{
    const bool mtw = GENERATE(true, false);
    const float a = float(1.0 - std::exp(-2.0 * kPi * 1000.0 / kSr));
    float y = 0;
    vt::Analyzer an;
    an.setMultiWindow(mtw);
    an.prepare(kSr);
    an.setSmoothing(0);
    auto r = runRoom(an, [&](float x) { y += a * (x - y); return y; });
    size_t best = 0;
    for (size_t i = 0; i < r.freq.size(); ++i)
        if (std::abs(r.freq[i] - 1000.0f) < std::abs(r.freq[best] - 1000.0f)) best = i;
    CHECK_THAT(r.magDb[best], WithinAbs(-3.0, 0.5));
    CHECK_THAT(meanOver(r, r.magDb, 50, 100), WithinAbs(0.0, 0.3));
}

TEST_CASE("MicCal parses REW-style files and interpolates in log frequency")
{
    auto f = juce::File::createTempFile(".cal");
    f.replaceWithText("\"Sens Factor =-1.2dB, SERNO: 123\"\n* comment\n20\t-2.0\n100 0.0 12\n1000,1.0\n10000;3.0\n");
    vt::MicCal cal;
    juce::String err;
    REQUIRE(cal.load(f, err));
    CHECK_THAT(cal.offsetDb(20), WithinAbs(-2.0, 1e-4));
    CHECK_THAT(cal.offsetDb(10), WithinAbs(-2.0, 1e-4));       // flat below range
    CHECK_THAT(cal.offsetDb(316.2278f), WithinAbs(0.5, 1e-3)); // log midpoint of 100..1000
    CHECK_THAT(cal.offsetDb(20000), WithinAbs(3.0, 1e-4));
    f.replaceWithText("not a cal file\n");
    CHECK_FALSE(cal.load(f, err));
    CHECK(err.isNotEmpty());
    f.deleteFile();
}

static float rippleDb(const vt::Analyzer::Result& r, float f0, float f1)
{
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = 0; i < r.freq.size(); ++i)
        if (r.freq[i] >= f0 && r.freq[i] <= f1) { lo = std::min(lo, r.magDb[i]); hi = std::max(hi, r.magDb[i]); }
    return hi - lo;
}

TEST_CASE("Multi-time-window rejects a late reflection in the highs")
{
    auto room = [] {
        return [d = DelayGain(480, 1.0f), refl = DelayGain(480 + 2400, 0.7f)](float x) mutable { return d(x) + refl(x); };
    };
    vt::Analyzer mtw, fixed;
    mtw.setMultiWindow(true);
    fixed.setMultiWindow(false);
    for (auto* an : { &mtw, &fixed }) { an->prepare(kSr); an->setSmoothing(0); }
    auto rm = runRoom(mtw, room(), 8.0);
    auto rf = runRoom(fixed, room(), 8.0);
    CHECK(rippleDb(rm, 5000, 15000) < 1.5f);
    CHECK(rippleDb(rf, 5000, 15000) > 6.0f);
}
