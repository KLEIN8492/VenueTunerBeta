#include <catch2/catch_test_macros.hpp>
#include "server/Dispatcher.h"

namespace {
juce::var cmd(const juce::String& json) { return juce::JSON::parse(json); }
juce::var req(const juce::String& c, juce::var args)
{
    auto r = juce::var(new juce::DynamicObject());
    r.getDynamicObject()->setProperty("id", 1);
    r.getDynamicObject()->setProperty("cmd", c);
    r.getDynamicObject()->setProperty("args", args);
    return r;
}
juce::var obj(std::initializer_list<std::pair<const char*, juce::var>> kv)
{
    auto o = juce::var(new juce::DynamicObject());
    for (auto& [k, v] : kv) o.getDynamicObject()->setProperty(k, v);
    return o;
}
}

TEST_CASE("Speaker presets: save, apply (speaker fields only), embedded FIR, export/import")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("vt_preset_test");
    dir.deleteRecursively();
    vt::AudioEngine engine;
    engine.prepareForTest(48000, 64, 2, 2);
    vt::Dispatcher d(engine, dir);
    auto& s = d.stateForTest();
    s.ensureDefaults(2, 2);

    // Output 0: a "speaker" tuning + venue settings.
    auto& a = s.outputs[0];
    a.peq = { { true, "bell", 2500.0f, -3.0f, 2.0f } };
    a.hp = { "lr24", 80.0f };
    a.limThreshDb = -6.0f;
    a.gainDb = -2.0f;
    a.delayMs = 12.0f;
    a.mix = { 1.0f, 0.0f };
    auto fir = dir.getChildFile("speaker.txt");
    dir.createDirectory();
    juce::String taps;
    for (int i = 0; i < 100; ++i) taps << (i == 10 ? "1.0" : "0.0") << "\n";
    fir.replaceWithText(taps);
    REQUIRE(bool(d.handle(req("fir.load", obj({ { "output", 0 }, { "path", fir.getFullPathName() } })))["ok"]));

    auto r = d.handle(req("preset.save", obj({ { "name", "My Top" }, { "output", 0 } })));
    REQUIRE(bool(r["ok"]));
    fir.deleteFile(); // the preset must not depend on the original file

    // Output 1 has its own venue settings, which must survive applying the preset.
    s.outputs[1].delayMs = 3.0f;
    s.outputs[1].mix = { 0.0f, 1.0f };
    s.outputs[1].channel = 1;
    r = d.handle(req("preset.apply", obj({ { "name", "My Top" }, { "outputs", juce::Array<juce::var> { 1 } } })));
    REQUIRE(bool(r["ok"]));
    const auto& b = s.outputs[1];
    CHECK(b.peq.size() == 1);
    CHECK(b.hp.type == "lr24");
    CHECK(b.limThreshDb == -6.0f);
    CHECK(b.gainDb == -2.0f);
    CHECK(b.delayMs == 3.0f);               // venue: untouched
    CHECK(b.mix == std::vector<float> { 0.0f, 1.0f });
    CHECK(b.channel == 1);
    CHECK(b.preset == "My Top");
    CHECK(b.fir.taps >= 100);
    CHECK(juce::File(b.fir.path).existsAsFile()); // FIR restored from the preset itself

    // Undo returns output 1 to its previous processing.
    d.handle(cmd(R"({"id":2,"cmd":"history.undo"})"));
    CHECK(s.outputs[1].peq.empty());

    // Export → import under another name → identical behaviour.
    r = d.handle(req("preset.export", obj({ { "name", "My Top" } })));
    REQUIRE(bool(r["ok"]));
    REQUIRE(bool(d.handle(req("preset.import", obj({ { "name", "Shared Top" }, { "json", r["result"] } })))["ok"]));
    auto list = d.handle(cmd(R"({"id":3,"cmd":"preset.list"})"))["result"];
    CHECK(list.size() == 2);
    REQUIRE(bool(d.handle(req("preset.apply", obj({ { "name", "Shared Top" }, { "outputs", juce::Array<juce::var> { 1 } } })))["ok"]));
    CHECK(s.outputs[1].hp.freq == 80.0f);
    CHECK(s.outputs[1].fir.taps >= 100);

    CHECK_FALSE(bool(d.handle(req("preset.import", obj({ { "name", "Bad" }, { "json", "{oops" } })))["ok"]));
    dir.deleteRecursively();
}
