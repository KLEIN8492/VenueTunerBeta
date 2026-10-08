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

TEST_CASE("Filter config round-trips and validates")
{
    auto s = sample();
    auto& o = s.outputs[0];
    o.peq.push_back({ true, "bell", 1000.0f, -4.5f, 2.0f });
    o.peq.push_back({ false, "highshelf", 8000.0f, 3.0f, 0.7f });
    o.hp = { "lr24", 90.0f };
    o.lp = { "bw12", 18000.0f };
    o.ap.push_back({ true, 2, 120.0f, 0.8f });
    o.fir = { true, "/tmp/x.wav", 1024, 511 };
    vt::SystemState back;
    juce::String err;
    REQUIRE(vt::fromVar(vt::toVar(s), back, err));
    CHECK(juce::JSON::toString(vt::toVar(back)) == juce::JSON::toString(vt::toVar(s)));
    CHECK(back.outputs[0].peq[1].type == "highshelf");
    CHECK(back.outputs[0].hp.type == "lr24");
    CHECK(back.outputs[0].fir.latencySamples == 511);

    CHECK(vt::applyPatch(s, "outputs/0/peq/0/gainDb", -6.0, err));
    CHECK(s.outputs[0].peq[0].gainDb == -6.0f);
    CHECK_FALSE(vt::applyPatch(s, "outputs/0/peq/0/q", 0.0, err));        // Q too small
    CHECK_FALSE(vt::applyPatch(s, "outputs/0/peq/0/freq", 5.0, err));     // below 10 Hz
    CHECK_FALSE(vt::applyPatch(s, "outputs/0/peq/0/type", "wobble", err));
    CHECK_FALSE(vt::applyPatch(s, "outputs/0/hp/type", "lr96", err));
    CHECK_FALSE(vt::applyPatch(s, "outputs/0/ap/0/order", 3, err));

    juce::Array<juce::var> many;
    for (int i = 0; i < 17; ++i) {
        auto b = juce::var(new juce::DynamicObject());
        b.getDynamicObject()->setProperty("freq", 1000);
        many.add(b);
    }
    CHECK_FALSE(vt::applyPatch(s, "outputs/0/peq", many, err)); // > 16 bands
    CHECK(err.contains("16"));
}

TEST_CASE("Milestone 1 snapshots without filter fields still load")
{
    vt::SystemState s;
    juce::String err;
    auto v = juce::JSON::parse(R"({"outputs":[{"name":"Old","gainDb":-3}]})");
    REQUIRE(vt::fromVar(v, s, err));
    CHECK(s.outputs[0].peq.empty());
    CHECK(s.outputs[0].hp.type == "off");
    CHECK_FALSE(s.outputs[0].fir.on);
}
