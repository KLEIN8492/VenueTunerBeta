#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/Biquad.h"
#include <cmath>

using Catch::Matchers::WithinAbs;
using namespace vt;
static constexpr double sr = 48000.0;

static double magDb(const std::vector<Biquad>& c, double f)
{
    return 20.0 * std::log10(std::abs(chainResponse(c.data(), int(c.size()), f, sr)));
}
static double magDb(const Biquad& b, double f) { return 20.0 * std::log10(std::abs(b.response(f, sr))); }

TEST_CASE("Bell, shelves, notch")
{
    CHECK_THAT(magDb(design::bell(1000, 2.0, 6.0, sr), 1000), WithinAbs(6.0, 0.01));
    CHECK_THAT(magDb(design::bell(1000, 2.0, -9.0, sr), 1000), WithinAbs(-9.0, 0.01));
    CHECK_THAT(magDb(design::bell(1000, 2.0, 6.0, sr), 50), WithinAbs(0.0, 0.05));
    CHECK_THAT(magDb(design::lowShelf(200, 0.707, 6.0, sr), 20), WithinAbs(6.0, 0.1));
    CHECK_THAT(magDb(design::lowShelf(200, 0.707, 6.0, sr), 10000), WithinAbs(0.0, 0.1));
    CHECK_THAT(magDb(design::highShelf(5000, 0.707, -4.0, sr), 20000), WithinAbs(-4.0, 0.15));
    CHECK_THAT(magDb(design::highShelf(5000, 0.707, -4.0, sr), 100), WithinAbs(0.0, 0.05));
    CHECK(magDb(design::notch(1000, 5.0, sr), 1000) < -60.0);
    CHECK(design::bell(1000, 1, 0, sr).isIdentity());
}

TEST_CASE("Butterworth crossovers are -3 dB at fc")
{
    for (auto t : { XoType::BW12, XoType::BW18, XoType::BW24, XoType::BW36, XoType::BW48 }) {
        CHECK_THAT(magDb(design::crossover(t, true, 500, sr), 500), WithinAbs(-3.01, 0.02));
        CHECK_THAT(magDb(design::crossover(t, false, 500, sr), 500), WithinAbs(-3.01, 0.02));
    }
    CHECK(magDb(design::crossover(XoType::BW24, false, 500, sr), 5000) < -79.0); // >= 24 dB/oct (bilinear warps steeper near Nyquist)
    CHECK(design::crossover(XoType::Off, true, 500, sr).empty());
}

TEST_CASE("Linkwitz-Riley crossovers sum flat")
{
    for (auto t : { XoType::LR24, XoType::LR48 }) {
        auto hp = design::crossover(t, true, 100, sr), lp = design::crossover(t, false, 100, sr);
        CHECK_THAT(magDb(hp, 100), WithinAbs(-6.02, 0.02));
        CHECK_THAT(magDb(lp, 100), WithinAbs(-6.02, 0.02));
        for (double f = 20; f <= 20000; f *= 1.1) {
            auto sum = chainResponse(hp.data(), int(hp.size()), f, sr) + chainResponse(lp.data(), int(lp.size()), f, sr);
            CHECK_THAT(20 * std::log10(std::abs(sum)), WithinAbs(0.0, 0.01));
        }
    }
    // LR12 sums flat with one side inverted
    auto hp = design::crossover(XoType::LR12, true, 1000, sr), lp = design::crossover(XoType::LR12, false, 1000, sr);
    auto sum = -chainResponse(hp.data(), int(hp.size()), 1000, sr) + chainResponse(lp.data(), int(lp.size()), 1000, sr);
    CHECK_THAT(20 * std::log10(std::abs(sum)), WithinAbs(0.0, 0.01));
}

TEST_CASE("All-pass filters are flat with the expected phase")
{
    auto ap2 = design::allpass2(1000, 0.7, sr), ap1 = design::allpass1(1000, sr);
    for (double f = 20; f <= 20000; f *= 1.2) {
        CHECK_THAT(magDb(ap2, f), WithinAbs(0.0, 1e-9));
        CHECK_THAT(magDb(ap1, f), WithinAbs(0.0, 1e-9));
    }
    CHECK_THAT(std::abs(std::arg(ap2.response(1000, sr))) * 180 / M_PI, WithinAbs(180.0, 0.01));
    CHECK_THAT(std::arg(ap1.response(1000, sr)) * 180 / M_PI, WithinAbs(-90.0, 0.01));
}

TEST_CASE("Frequencies above 0.49 fs are clamped, never NaN")
{
    auto b = design::bell(30000, 1.0, 6.0, sr);
    CHECK(std::isfinite(b.b0));
    CHECK(std::isfinite(magDb(b, 1000)));
    auto x = design::crossover(XoType::LR48, false, 40000, sr);
    for (auto& s : x) CHECK(std::isfinite(s.a1));
}

TEST_CASE("Section processes like its transfer function")
{
    auto b = design::bell(1000, 1.0, 6.0, sr);
    BiquadState st;
    double peak = 0;
    for (int i = 0; i < 48000; ++i) {
        double y = st.process(b, std::sin(2 * M_PI * 1000 * i / sr));
        if (i > 24000) peak = std::max(peak, std::abs(y));
    }
    CHECK_THAT(20 * std::log10(peak), WithinAbs(6.0, 0.02));
}

TEST_CASE("LR-matched all-pass equals the Linkwitz-Riley crossover sum")
{
    // A Linkwitz-Riley LP+HP sums to an all-pass; the "LR-matched" all-pass must have exactly
    // that phase (half the LR order, Butterworth poles), so a speaker can be phase-matched to
    // another one that went through that crossover.
    for (auto [type, xo] : { std::pair { "lr24", XoType::LR24 }, std::pair { "lr48", XoType::LR48 } }) {
        auto ap = design::allpassMatched(type, 120, sr);
        auto hp = design::crossover(xo, true, 120, sr), lp = design::crossover(xo, false, 120, sr);
        for (double f = 20; f <= 2000; f *= 1.25) {
            const auto sum = chainResponse(hp.data(), int(hp.size()), f, sr) + chainResponse(lp.data(), int(lp.size()), f, sr);
            const auto a = chainResponse(ap.data(), int(ap.size()), f, sr);
            INFO(type << " at " << f << " Hz");
            CHECK_THAT(std::abs(a), WithinAbs(1.0, 1e-9));
            CHECK_THAT(std::abs(a - sum), WithinAbs(0.0, 1e-6));
        }
    }
    CHECK(design::allpassMatched("1st", 100, sr).size() == 1);
    CHECK(design::allpassMatched("lr48", 100, sr).size() == 2);
}
