#include <catch2/catch_test_macros.hpp>
#include "server/Dispatcher.h"

namespace {
juce::var cmd(const juce::String& json)
{
    return juce::JSON::parse(json);
}
struct Fixture
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("vt_disp_test");
    vt::AudioEngine engine;
    std::unique_ptr<vt::Dispatcher> d;
    int stateEvents = 0;
    Fixture()
    {
        dir.deleteRecursively();
        engine.prepareForTest(48000, 64, 2, 2);
        d = std::make_unique<vt::Dispatcher>(engine, dir);
        d->stateForTest().ensureDefaults(2, 2);
        d->onStateChanged = [this] { ++stateEvents; };
    }
    ~Fixture() { d.reset(); dir.deleteRecursively(); }
};
}

TEST_CASE("Dispatcher: state.get, patch, errors")
{
    Fixture f;
    auto r = f.d->handle(cmd(R"({"id":1,"cmd":"state.get"})"));
    CHECK(r["id"] == juce::var(1));
    CHECK(bool(r["ok"]));
    CHECK(r["result"]["outputs"].size() == 2);

    r = f.d->handle(cmd(R"({"id":2,"cmd":"state.patch","args":{"path":"outputs/1/gainDb","value":-4}})"));
    CHECK(bool(r["ok"]));
    CHECK(f.d->stateForTest().outputs[1].gainDb == -4.0f);
    CHECK(f.stateEvents == 1);

    r = f.d->handle(cmd(R"({"id":3,"cmd":"state.patch","args":{"path":"outputs/1/gainDb","value":99}})"));
    CHECK_FALSE(bool(r["ok"]));
    CHECK(r["error"].toString().contains("gainDb"));

    r = f.d->handle(cmd(R"({"id":4,"cmd":"nope"})"));
    CHECK_FALSE(bool(r["ok"]));
}

TEST_CASE("Dispatcher: patchMany is all-or-nothing")
{
    Fixture f;
    auto r = f.d->handle(cmd(R"({"id":1,"cmd":"state.patchMany","args":{"patches":[
        {"path":"outputs/0/gainDb","value":-2},{"path":"outputs/1/gainDb","value":50}]}})"));
    CHECK_FALSE(bool(r["ok"]));
    CHECK(f.d->stateForTest().outputs[0].gainDb == 0.0f);
    r = f.d->handle(cmd(R"({"id":2,"cmd":"state.patchMany","args":{"patches":[
        {"path":"outputs/0/gainDb","value":-2},{"path":"outputs/1/gainDb","value":-3}]}})"));
    CHECK(bool(r["ok"]));
    CHECK(f.d->stateForTest().outputs[1].gainDb == -3.0f);
}

TEST_CASE("Dispatcher: snapshot save and recall")
{
    Fixture f;
    f.d->handle(cmd(R"({"id":1,"cmd":"state.patch","args":{"path":"outputs/0/delayMs","value":3.5}})"));
    auto r = f.d->handle(cmd(R"({"id":2,"cmd":"snapshot.save","args":{"name":"A"}})"));
    REQUIRE(bool(r["ok"]));
    f.d->handle(cmd(R"({"id":3,"cmd":"state.patch","args":{"path":"outputs/0/delayMs","value":9}})"));
    r = f.d->handle(cmd(R"({"id":4,"cmd":"snapshot.recall","args":{"name":"A"}})"));
    REQUIRE(bool(r["ok"]));
    CHECK(f.d->stateForTest().outputs[0].delayMs == 3.5f);
    r = f.d->handle(cmd(R"({"id":5,"cmd":"snapshot.list"})"));
    CHECK(r["result"].size() == 1);
}

TEST_CASE("Dispatcher: io latency key and driver fallback")
{
    Fixture f;
    auto& s = f.d->stateForTest();
    s.cal.ioLatency[vt::Dispatcher::latencyKey("Out", "In", 48000, 64)] = 300;
    CHECK(f.d->ioLatency("Out", "In", 48000, 64, 200).samples == 300);
    CHECK(f.d->ioLatency("Out", "In", 48000, 64, 200).calibrated);
    CHECK(f.d->ioLatency("Out", "In", 48000, 128, 200).samples == 200);
}

TEST_CASE("Dispatcher: routing commands")
{
    Fixture f;
    auto r = f.d->handle(cmd(R"({"id":1,"cmd":"routing.addSource","args":{"type":"mic","channel":1}})"));
    REQUIRE(bool(r["ok"]));
    auto& s = f.d->stateForTest();
    CHECK(s.sources.size() == 3);
    CHECK(s.measure.micSource == 2);       // first mic becomes the measurement mic
    CHECK(s.outputs[0].mix.size() == 3);

    r = f.d->handle(cmd(R"({"id":2,"cmd":"routing.addOutput","args":{"preset":"sub"}})"));
    REQUIRE(bool(r["ok"]));
    CHECK(s.outputs.size() == 3);
    CHECK(s.outputs[2].name == "Sub");
    CHECK(s.outputs[2].mix[0] == 0.5f);
    CHECK(s.outputs[2].lp.type == "lr24");
    CHECK(s.outputs[2].channel == -1);     // no free channel on a device that isn't open

    r = f.d->handle(cmd(R"({"id":3,"cmd":"routing.removeSource","args":{"index":0}})"));
    REQUIRE(bool(r["ok"]));
    CHECK(s.sources.size() == 2);
    CHECK(s.measure.micSource == 1);
    CHECK(s.outputs[2].mix.size() == 2);

    r = f.d->handle(cmd(R"({"id":4,"cmd":"routing.removeOutput","args":{"index":9}})"));
    CHECK_FALSE(bool(r["ok"]));
}

TEST_CASE("Dispatcher: snapshot on another interface asks for a remap")
{
    Fixture f;
    auto& s = f.d->stateForTest();
    s.device.inputDevice = s.device.outputDevice = "Interface A";
    s.outputs[1].channel = 1;
    f.d->handle(cmd(R"({"id":1,"cmd":"snapshot.save","args":{"name":"Show"}})"));
    f.d->handle(cmd(R"({"id":2,"cmd":"state.patch","args":{"path":"outputs/0/gainDb","value":-9}})"));

    s.device.inputDevice = s.device.outputDevice = "Interface B";
    auto r = f.d->handle(cmd(R"({"id":3,"cmd":"snapshot.recall","args":{"name":"Show"}})"));
    REQUIRE(bool(r["ok"]));
    CHECK(bool(r["result"]["needsRemap"]));
    CHECK(r["result"]["outputs"].size() == 2);
    CHECK(s.outputs[0].gainDb == -9.0f);   // nothing applied yet

    r = f.d->handle(cmd(R"({"id":4,"cmd":"snapshot.recall","args":{"name":"Show","remap":{"sources":[4,5],"outputs":[6,-1]}}})"));
    REQUIRE(bool(r["ok"]));
    CHECK(s.outputs[0].gainDb == 0.0f);
    CHECK(s.outputs[0].channel == 6);
    CHECK(s.outputs[1].channel == -1);
    CHECK(s.sources[0].channel == 4);

    s.device.inputDevice = s.device.outputDevice = "Interface A"; // same device: applies directly
    f.d->handle(cmd(R"({"id":5,"cmd":"snapshot.save","args":{"name":"Show2"}})"));
    r = f.d->handle(cmd(R"({"id":6,"cmd":"snapshot.recall","args":{"name":"Show2"}})"));
    CHECK_FALSE(r["result"].hasProperty("needsRemap"));
}
