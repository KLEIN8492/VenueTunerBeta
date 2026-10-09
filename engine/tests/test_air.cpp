#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/AirAbsorption.h"
#include "dsp/FilterDesign.h"
#include "core/SystemState.h"

using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinAbs;

TEST_CASE("ISO 9613-1 attenuation matches the standard's table (20 °C, 70 % RH, 101.325 kPa)")
{
    // ISO 9613-1 Table 1 (dB/km): 1 kHz 4.98, 2 kHz 9.02, 4 kHz 22.9, 8 kHz 76.6
    CHECK_THAT(vt::air::alphaDbPerM(1000, 20, 70) * 1000, WithinRel(4.98, 0.03));
    CHECK_THAT(vt::air::alphaDbPerM(2000, 20, 70) * 1000, WithinRel(9.02, 0.03));
    CHECK_THAT(vt::air::alphaDbPerM(4000, 20, 70) * 1000, WithinRel(22.9, 0.03));
    CHECK_THAT(vt::air::alphaDbPerM(8000, 20, 70) * 1000, WithinRel(76.6, 0.03));
    // Dry air absorbs more HF than humid air at 20 °C.
    CHECK(vt::air::alphaDbPerM(10000, 20, 20) > vt::air::alphaDbPerM(10000, 20, 70));
}

TEST_CASE("Compensation shelf follows the absorption difference")
{
    constexpr double sr = 48000;
    // Typical evening change: tuned at 24 °C / 55 %, now 16 °C / 40 %, delay tower 40 m away.
    auto s = vt::air::compensation(40, { 16, 40, 101.325 }, { 24, 55, 101.325 }, sr);
    CHECK(s.gainDb > 1.0);
    const auto b = vt::design::highShelf(s.freq, 0.707, s.gainDb, sr);
    for (double f : { 4000.0, 8000.0, 12000.0 }) {
        const double want = 40 * (vt::air::alphaDbPerM(f, 16, 40) - vt::air::alphaDbPerM(f, 24, 55));
        const double got = 20 * std::log10(std::abs(b.response(f, sr)));
        INFO(f << " Hz: want " << want << " got " << got);
        CHECK_THAT(got, WithinAbs(want, 1.5));
    }
    // Extreme change (70 % → 25 % RH over 60 m wants > +10 dB at 12 kHz): the safety cap holds.
    CHECK(vt::air::compensation(60, { 20, 25, 101.325 }, { 20, 70, 101.325 }, sr).gainDb <= 9.0);
    // Same conditions → nothing to do. Zero distance → nothing to do.
    CHECK(vt::air::compensation(60, { 20, 70, 101.325 }, { 20, 70, 101.325 }, sr).gainDb == 0.0);
    CHECK(vt::air::compensation(0, { 20, 25, 101.325 }, { 20, 70, 101.325 }, sr).gainDb == 0.0);
}

TEST_CASE("Air shelf lands in its own IIR slot")
{
    vt::OutputConfig o;
    o.airFreq = 8000;
    o.airGainDb = 2.5f;
    vt::Biquad slots[vt::kMaxIir];
    vt::designOutputIir(o, 48000, slots);
    CHECK_FALSE(slots[vt::kIirAir].isIdentity());
    for (int k = 0; k < vt::kIirAir; ++k) CHECK(slots[k].isIdentity());
}
