#include <catch2/catch_test_macros.hpp>
#include "core/SystemState.h"
#include "server/Snapshots.h"

static vt::SystemState sample()
{
    vt::SystemState s;
    s.resizeIO(2, 4);
    s.outputs[0].name = "Main L";
    s.outputs[0].mix[0] = 1.0f;
    s.outputs[3].name = "Sub";
    s.outputs[3].mix = { 0.5f, 0.5f };
    s.outputs[3].delayMs = 4.5f;
    s.inputs[1].role = "mic";
    s.gen.targets = { 0, 3 };
    s.cal.ioLatency["BlackHole|BlackHole|48000|64"] = 192.0;
    return s;
}

TEST_CASE("SystemState JSON round-trip")
{
    auto s = sample();
    vt::SystemState back;
    juce::String err;
    REQUIRE(vt::fromVar(vt::toVar(s), back, err));
    CHECK(juce::JSON::toString(vt::toVar(back)) == juce::JSON::toString(vt::toVar(s)));
    CHECK(back.outputs[3].mix[1] == 0.5f);
    CHECK(back.cal.ioLatency["BlackHole|BlackHole|48000|64"] == 192.0);
}

TEST_CASE("applyPatch changes valid values and rejects invalid ones atomically")
{
    auto s = sample();
    juce::String err;
    CHECK(vt::applyPatch(s, "outputs/2/gainDb", -3.5, err));
    CHECK(s.outputs[2].gainDb == -3.5f);

    CHECK_FALSE(vt::applyPatch(s, "outputs/2/gainDb", 40.0, err)); // > +12 dB
    CHECK(err.contains("gainDb"));
    CHECK(s.outputs[2].gainDb == -3.5f);

    CHECK_FALSE(vt::applyPatch(s, "outputs/9/gainDb", 0.0, err));   // no such output
    CHECK_FALSE(vt::applyPatch(s, "outputs/1/bogus", 0.0, err));     // unknown field
    CHECK_FALSE(vt::applyPatch(s, "outputs/1/mute", "yes", err));    // wrong type
    CHECK_FALSE(vt::applyPatch(s, "gen/levelDb", -3.0, err));        // above generator cap
    CHECK(vt::applyPatch(s, "gen/levelDb", -20.0, err));
    CHECK(vt::applyPatch(s, "outputs/0/mix", juce::Array<juce::var>{ 0.0, 1.0 }, err));
    CHECK(s.outputs[0].mix[1] == 1.0f);
}

TEST_CASE("toParams maps state to the audio-thread structure")
{
    auto s = sample();
    s.outputs[1].polarityInvert = true;
    s.gen.on = true;
    s.measure.micInput = 1;
    auto p = vt::toParams(s);
    CHECK(p.numOut == 4);
    CHECK(p.out[3].mix[0] == 0.5f);
    CHECK(p.out[1].polarityInvert);
    CHECK(p.gen.on);
    CHECK(p.gen.targets == ((1ull << 0) | (1ull << 3)));
    CHECK(p.measure.micInput == 1);
}

TEST_CASE("Snapshots save, recall, reject corrupt files, autosave")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("vt_snap_test");
    dir.deleteRecursively();
    vt::Snapshots snaps(dir);
    auto s = sample();
    juce::String err;
    REQUIRE(snaps.save("Show A", s, err));
    CHECK(snaps.list().contains("Show A"));

    s.outputs[0].gainDb = -10.0f;
    REQUIRE(snaps.recall("Show A", s, err));
    CHECK(s.outputs[0].gainDb == 0.0f);
    CHECK(s.outputs[0].name == "Main L");

    dir.getChildFile("snapshots/Broken.json").replaceWithText("{ not json");
    s.outputs[0].gainDb = -7.0f;
    CHECK_FALSE(snaps.recall("Broken", s, err));
    CHECK(s.outputs[0].gainDb == -7.0f); // untouched

    snaps.autosave(s);
    vt::SystemState loaded;
    REQUIRE(snaps.loadAutosave(loaded, err));
    CHECK(loaded.outputs[0].gainDb == -7.0f);

    CHECK(snaps.remove("Show A"));
    CHECK_FALSE(snaps.list().contains("Show A"));
    dir.deleteRecursively();
}
