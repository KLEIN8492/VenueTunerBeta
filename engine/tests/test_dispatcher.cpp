#include <cmath>
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

TEST_CASE("Dispatcher: outputs keep their processing slot when others are removed or moved")
{
    Fixture f;
    auto& s = f.d->stateForTest();
    f.d->handle(cmd(R"({"id":1,"cmd":"routing.addOutput","args":{"preset":"single"}})"));
    REQUIRE(s.outputs.size() == 3);
    const int idA = s.outputs[0].id, idB = s.outputs[1].id, idC = s.outputs[2].id;
    CHECK(idA != idB);
    CHECK(idB != idC);
    const int slotB = f.d->slotForTest(idB), slotC = f.d->slotForTest(idC);

    f.d->handle(cmd(R"({"id":2,"cmd":"routing.removeOutput","args":{"index":0}})"));
    CHECK(f.d->slotForTest(idB) == slotB);
    CHECK(f.d->slotForTest(idC) == slotC);
    CHECK(f.d->slotForTest(idA) == -1);

    f.d->handle(cmd(R"({"id":3,"cmd":"routing.moveOutput","args":{"index":1,"to":0}})"));
    CHECK(s.outputs[0].id == idC);
    CHECK(f.d->slotForTest(idC) == slotC);

    f.d->handle(cmd(R"({"id":4,"cmd":"routing.duplicateOutput","args":{"index":0}})"));
    CHECK(s.outputs[1].id != idC);                         // the copy has its own identity…
    CHECK(f.d->slotForTest(s.outputs[1].id) != slotC);     // …and its own slot
}

TEST_CASE("Dispatcher: SPL calibration is stored per mic")
{
    Fixture f;
    f.d->handle(cmd(R"({"id":1,"cmd":"routing.addSource","args":{"type":"mic","name":"Mic FOH","channel":1}})"));
    // 1 kHz at −40 dBFS RMS into the analyzer's mic path for 6 s.
    std::vector<float> ref(512, 0.0f), mic(512);
    double ph = 0;
    for (int b = 0; b < int(6 * 48000 / 512); ++b) {
        for (auto& x : mic) { x = float(0.01 * std::sqrt(2.0) * std::sin(ph)); ph += 2 * M_PI * 1000 / 48000; }
        f.engine.analyzer().pushRef(ref.data(), 512);
        f.engine.analyzer().pushMeas(mic.data(), 512);
        f.engine.analyzer().process();
    }
    auto r = f.d->handle(cmd(R"({"id":2,"cmd":"spl.calibrate","args":{"level":94}})"));
    REQUIRE(bool(r["ok"]));
    CHECK(std::abs(double(r["result"]) - 134.0) < 0.2);
    CHECK(f.d->stateForTest().cal.splOffset.count("Mic FOH") == 1);
    auto st = f.d->statusVar();
    CHECK(bool(st["spl"]["calibrated"]));
    CHECK(std::abs(double(st["spl"]["slow"][0]) - 94.0) < 0.2);
}
