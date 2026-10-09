#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "measure/Quality.h"
#include "core/SystemState.h"

using Catch::Matchers::WithinAbs;

static vt::Analyzer::Result result(float coh)
{
    vt::Analyzer::Result r;
    for (int i = 0; i < 240; ++i) {
        r.freq.push_back(20.0f * std::pow(2.0f, i / 24.0f));
        r.magDb.push_back(0);
        r.phaseDeg.push_back(0);
        r.coherence.push_back(r.freq.back() < 100 || r.freq.back() > 10000 ? 0.1f : coh);
    }
    r.delayValid = true;
    r.frames = 32;
    return r;
}

TEST_CASE("Capture quality: mean coherence over the useful band, accept threshold")
{
    CHECK_THAT(vt::captureQuality(result(0.95f), 100, 10000), WithinAbs(0.95, 1e-4));
    CHECK(vt::captureAccepted(result(0.95f), 100, 10000));
    CHECK_FALSE(vt::captureAccepted(result(0.5f), 100, 10000));
    auto r = result(0.95f);
    r.delayValid = false;
    CHECK_FALSE(vt::captureAccepted(r, 100, 10000)); // no arrival: never accepted
}

TEST_CASE("Measurement groups and positions round-trip and validate")
{
    vt::SystemState s;
    s.outputs.resize(3);
    for (int i = 0; i < 3; ++i) { s.outputs[size_t(i)].id = i + 1; s.outputs[size_t(i)].channel = i; }
    s.measure.positions = { "FOH", "Front" };
    s.measure.groups = { { "Main L", { 0 } }, { "Subs", { 2 } } };
    vt::SystemState back;
    juce::String err;
    REQUIRE(vt::fromVar(vt::toVar(s), back, err));
    CHECK(back.measure.positions[1] == "Front");
    CHECK(back.measure.groups[1].outputs == std::vector<int> { 2 });

    // Group members that no longer exist are dropped (e.g. after removing an output).
    s.removeOutput(2);
    CHECK(s.measure.groups[1].outputs.empty());
    s.removeOutput(0);
    CHECK(s.measure.groups[0].outputs.empty());
}
