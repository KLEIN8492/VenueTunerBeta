#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "measure/SplMeter.h"
#include "measure/Rta.h"
#include "measure/RoomDecay.h"
#include "dsp/PinkNoise.h"
#include <cmath>
#include <random>
#include <vector>

using Catch::Matchers::WithinAbs;
static constexpr double sr = 48000.0;

static std::vector<float> sine(double f, double rmsDb, double seconds)
{
    const double a = std::pow(10.0, rmsDb / 20.0) * std::sqrt(2.0);
    std::vector<float> x(size_t(seconds * sr));
    for (size_t i = 0; i < x.size(); ++i) x[i] = float(a * std::sin(2 * M_PI * f * double(i) / sr));
    return x;
}

TEST_CASE("SPL meter: weightings, Leq, peak")
{
    vt::SplMeter m;
    m.prepare(sr);
    auto x = sine(1000, -20, 8.0); // Slow (1 s) needs a few time constants to settle
    m.process(x.data(), int(x.size()));
    auto r = m.reading();
    CHECK_THAT(r.slow[0], WithinAbs(-20.0, 0.1)); // Z
    CHECK_THAT(r.slow[1], WithinAbs(-20.0, 0.1)); // A
    CHECK_THAT(r.slow[2], WithinAbs(-20.0, 0.1)); // C
    CHECK_THAT(r.peakZ, WithinAbs(-20.0 + 3.01, 0.05));

    vt::SplMeter lf;
    lf.prepare(sr);
    auto y = sine(100, -20, 3.0);
    lf.process(y.data(), int(y.size()));
    auto q = lf.reading();
    CHECK_THAT(q.slow[1] - q.slow[0], WithinAbs(-19.1, 0.3));
    CHECK_THAT(q.slow[2] - q.slow[0], WithinAbs(-0.3, 0.2));

    vt::SplMeter leq;
    leq.prepare(sr);
    auto a = sine(1000, -20, 1.0), b = sine(1000, -40, 1.0);
    leq.process(a.data(), int(a.size()));
    leq.process(b.data(), int(b.size()));
    CHECK_THAT(leq.reading().leq[0], WithinAbs(-22.96, 0.1)); // 10·log10((0.01 + 0.0001)/2)
    leq.resetLeq();
    leq.process(b.data(), int(b.size()));
    CHECK_THAT(leq.reading().leq[0], WithinAbs(-40.0, 0.1));
}

TEST_CASE("RTA: pink noise is flat in 1/3 octaves, a sine dominates its band")
{
    vt::Rta rta;
    rta.prepare(sr);
    rta.setResolution(3);
    vt::PinkNoise pn;
    std::vector<float> x(size_t(8 * sr));
    for (auto& v : x) v = pn.next() * 0.1f;
    rta.process(x.data(), int(x.size()));
    auto r = rta.result();
    double ref = 0;
    for (size_t i = 0; i < r.freq.size(); ++i) if (std::abs(r.freq[i] - 1000) < 1) ref = r.levelDb[i];
    for (size_t i = 0; i < r.freq.size(); ++i)
        if (r.freq[i] >= 63 && r.freq[i] <= 10000) CHECK_THAT(r.levelDb[i] - ref, WithinAbs(0.0, 1.0));

    vt::Rta s;
    s.prepare(sr);
    s.setResolution(3);
    auto t = sine(1000, -20, 3.0);
    s.process(t.data(), int(t.size()));
    auto q = s.result();
    double at1k = -200, other = -200;
    for (size_t i = 0; i < q.freq.size(); ++i) {
        if (std::abs(q.freq[i] - 1000) < 1) at1k = q.levelDb[i];
        else if (q.freq[i] < 700 || q.freq[i] > 1400) other = std::max(other, double(q.levelDb[i]));
    }
    CHECK_THAT(at1k, WithinAbs(-20.0, 0.5));
    CHECK(at1k - other > 30.0);
}

TEST_CASE("Room decay: RT60 of a synthetic exponential decay")
{
    // One random decay is itself random (±5–8 % per band); average several "rooms" as one
    // would average measurement positions, then the estimator must land within 5 %.
    const double rt = 0.8;
    constexpr int runs = 6;
    std::vector<double> t20(8, 0.0), t30(8, 0.0), edt(8, 0.0);
    std::vector<double> fc;
    for (int seed = 1; seed <= runs; ++seed) {
        std::mt19937 rng { unsigned(seed) };
        std::normal_distribution<double> g(0, 1);
        std::vector<float> ir(size_t(3 * sr));
        for (size_t i = 0; i < ir.size(); ++i) {
            const double t = double(i) / sr;
            ir[i] = float(g(rng) * std::exp(-6.9078 * t / rt) + g(rng) * std::pow(10.0, -70.0 / 20.0));
        }
        auto bands = vt::roomDecay(ir, sr);
        REQUIRE(bands.size() == 8);
        fc.clear();
        for (size_t b = 0; b < 8; ++b) {
            fc.push_back(bands[b].fc);
            t20[b] += bands[b].t20 / runs;
            t30[b] += bands[b].t30 / runs;
            edt[b] += bands[b].edt / runs;
        }
    }
    for (size_t b = 0; b < 8; ++b) {
        if (fc[b] < 250 || fc[b] > 4000) continue;
        INFO("band " << fc[b]);
        CHECK_THAT(t20[b], WithinAbs(rt, rt * 0.05));
        CHECK_THAT(t30[b], WithinAbs(rt, rt * 0.05));
        CHECK_THAT(edt[b], WithinAbs(rt, rt * 0.1));
    }
}


TEST_CASE("SPL meter: Impulse, C-peak, max hold and sliding Leq windows")
{
    vt::SplMeter m;
    m.prepare(sr);
    auto loud = sine(1000, -20, 10.0), quiet = sine(1000, -40, 5.0);
    m.process(loud.data(), int(loud.size()));
    auto r = m.reading();
    CHECK_THAT(r.impulse[1], WithinAbs(-20.0, 0.2));        // steady tone: Impulse = Fast = Slow
    m.resetLeq();                                           // drop the filter's start-up overshoot
    m.process(loud.data(), int(sr));                        // (a peak meter rightly catches it)
    r = m.reading();
    CHECK_THAT(r.cPeak, WithinAbs(-20.0 + 3.01, 0.1));      // C is 0 dB at 1 kHz; sine peak = RMS + 3 dB
    m.process(quiet.data(), int(quiet.size()));
    r = m.reading();
    CHECK_THAT(r.maxFast[1], WithinAbs(-20.0, 0.2));        // max hold remembers the loud part
    // Leq windows: 1 s = last second (quiet); 10 s = 5 s loud + 5 s quiet.
    CHECK_THAT(r.leqWindow[0][1], WithinAbs(-40.0, 0.2));
    CHECK_THAT(r.leqWindow[1][1], WithinAbs(-22.96, 0.2));
    // 60 s window with only 16 s of history (10 + 1 + 5): Leq over what exists.
    CHECK_THAT(r.leqWindow[2][1], WithinAbs(10 * std::log10((11 * 0.01 + 5 * 0.0001) / 16.0), 0.2));
    // Impulse falls slowly (≈ 2.9 dB/s), Fast quickly.
    vt::SplMeter d;
    d.prepare(sr);
    auto burst = sine(1000, -20, 1.0), silence = std::vector<float>(size_t(0.5 * sr), 0.0f);
    d.process(burst.data(), int(burst.size()));
    d.process(silence.data(), int(silence.size()));
    auto q = d.reading();
    CHECK(q.impulse[0] > q.fast[0] + 10.0);
    CHECK_THAT(q.impulse[0], WithinAbs(-20.0 - 2.9 * 0.5, 0.5));
}
