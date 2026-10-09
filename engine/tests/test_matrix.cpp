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

TEST_CASE("Mics assigned to positions become extra analyzer slots")
{
    vt::SystemState s;
    s.sources = { { "Console", "program", 0 }, { "Mic A", "mic", 1 }, { "Mic B", "mic", 2 }, { "Mic C", "mic", 3 } };
    s.measure.micSource = 1;
    s.measure.positions = { "FOH", "Front", "Balcony", "Side" };
    s.measure.positionMics = { 1, 2, 3, 2 }; // main mic at FOH; Mic B twice → one slot
    CHECK(vt::extraMicSources(s) == std::vector<int> { 2, 3 });
    CHECK(vt::micSlot(s, 1) == 0);
    CHECK(vt::micSlot(s, 2) == 1);
    CHECK(vt::micSlot(s, 3) == 2);
    CHECK(vt::micSlot(s, 0) == -1);
    auto p = vt::toParams(s);
    CHECK(p.measure.numExtra == 2);
    CHECK(p.measure.extraMicInputs[0] == 2);
    CHECK(p.measure.extraMicInputs[1] == 3);

    s.removeSource(2); // Mic B gone: its positions fall back to "move the main mic"
    CHECK(s.measure.positionMics == std::vector<int> { 1, -1, 2, -1 });
}
