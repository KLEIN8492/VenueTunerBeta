#include <catch2/catch_test_macros.hpp>
#include "core/SystemState.h"
#include "server/Snapshots.h"

static vt::SystemState sample()
{
    vt::SystemState s;
    s.sources = { { "Console L", "program", 0 }, { "Mic FOH", "mic", 1 } };
    s.outputs.resize(4);
    for (int i = 0; i < 4; ++i) { s.outputs[size_t(i)].name = "Out " + juce::String(i + 1); s.outputs[size_t(i)].channel = i; s.outputs[size_t(i)].id = i + 1; s.outputs[size_t(i)].mix = { 0, 0 }; }
    s.outputs[0].name = "Main L";
    s.outputs[0].mix[0] = 1.0f;
    s.outputs[3].name = "Sub";
    s.outputs[3].mix = { 0.5f, 0.5f };
    s.outputs[3].delayMs = 4.5f;
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
    s.measure.micSource = 1;
    s.outputs[2].channel = 9;
    auto p = vt::toParams(s);
    CHECK(p.numOut == 4);
    CHECK(p.out[3].mix[0] == 0.5f);
    CHECK(p.out[3].mix[1] == 0.0f); // the mic source is never routed, even with a mix value
    CHECK(p.out[2].channel == 9);
    CHECK(p.consoleInputs == 1ull);
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
    vt::SnapshotData d;
    REQUIRE(snaps.load("Show A", d, err));
    CHECK(d.hasPatch);
    CHECK(d.outputs[0].gainDb == 0.0f);
    CHECK(d.outputs[0].name == "Main L");
    CHECK(d.sources[1].name == "Mic FOH");

    dir.getChildFile("snapshots/Broken.json").replaceWithText("{ not json");
    s.outputs[0].gainDb = -7.0f;
    CHECK_FALSE(snaps.load("Broken", d, err));

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
    o.ap.push_back({ true, 2, "2nd", 120.0f, 0.8f });
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
    CHECK(vt::applyPatch(s, "outputs/0/hp", juce::JSON::parse(R"({"type":"bw24","freq":80})"), err)); // whole object
    CHECK(s.outputs[0].hp.freq == 80.0f);
    CHECK_FALSE(vt::applyPatch(s, "outputs/0/hp", juce::JSON::parse(R"({"type":"bw24","freq":2})"), err));
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

TEST_CASE("Pre-patch state migrates to sources and patched outputs")
{
    auto v = juce::JSON::parse(R"({
        "inputs":[{"name":"Mic","role":"mic"},{"name":"Unused","role":"none"},{"name":"Desk L","role":"console"},{"name":"Desk R","role":"console"},{"name":"In 5","role":"console"}],
        "outputs":[{"name":"Out 1","mix":[0,0,1,0,0]},{"name":"Out 2","mix":[0,0,0,1,0]},{"name":"Out 3","mix":[0,0,0,0,0]},{"name":"Fill","mix":[0,0,0,0,0]}],
        "measure":{"micInput":0,"ref":"generator"}})");
    vt::SystemState s;
    juce::String err;
    REQUIRE(vt::fromVar(v, s, err));
    REQUIRE(s.sources.size() == 3); // "none" and the untouched, unrouted "In 5" dropped
    REQUIRE(s.outputs.size() == 3);  // untouched "Out 3" dropped, renamed "Fill" kept
    CHECK(s.outputs[2].name == "Fill");
    CHECK(s.outputs[2].channel == 3);
    CHECK(s.sources[0].type == "mic");
    CHECK(s.sources[1].name == "Desk L");
    CHECK(s.sources[1].type == "program");
    CHECK(s.sources[1].channel == 2);
    CHECK(s.outputs[1].channel == 1);
    CHECK(s.outputs[0].mix == std::vector<float> { 0, 1, 0 });
    CHECK(s.outputs[1].mix == std::vector<float> { 0, 0, 1 });
    CHECK(s.measure.micSource == 0);
    // Saved again, it is in the new format.
    CHECK(vt::toVar(s).hasProperty("sources"));
    CHECK_FALSE(vt::toVar(s).hasProperty("inputs"));
}

TEST_CASE("Patch rules: one output per channel, removing a source fixes references")
{
    auto s = sample();
    juce::String err;
    CHECK_FALSE(vt::applyPatch(s, "outputs/1/channel", 0, err)); // channel 0 used by Main L
    CHECK(err.contains("Main L"));
    CHECK(vt::applyPatch(s, "outputs/1/channel", -1, err));     // unassigned is fine
    CHECK(vt::applyPatch(s, "outputs/2/channel", -1, err));     // several unassigned too

    s.measure.micSource = 1;
    s.removeSource(0);
    CHECK(s.sources.size() == 1);
    CHECK(s.measure.micSource == 0);
    CHECK(s.outputs[3].mix == std::vector<float> { 0.5f });

    vt::SystemState fresh;
    fresh.ensureDefaults(1, 2);
    CHECK(fresh.sources.size() == 1);
    CHECK(fresh.outputs.size() == 2);
    CHECK(fresh.outputs[1].channel == 1);
    CHECK(fresh.outputs[0].mix[0] == 0.0f); // never routed by default
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
