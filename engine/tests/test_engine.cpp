#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "core/AudioEngine.h"
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {
struct Harness
{
    static constexpr int kIn = 2, kOut = 4, kBlock = 64;
    vt::AudioEngine engine;
    vt::SystemState state;
    std::vector<std::vector<float>> in { kIn, std::vector<float>(kBlock) }, out { kOut, std::vector<float>(kBlock) };
    float loopback[kBlock] {}; // previous block of output 0, fed to input 1 (a 64-sample loop)

    Harness()
    {
        engine.prepareForTest(48000.0, kBlock, kIn, kOut);
        state.resizeIO(kIn, kOut);
        state.inputs[1].role = "mic";
    }
    void apply() { engine.setParams(vt::toParams(state)); }

    // Runs `blocks` blocks; input 0 gets `consoleValue`, input 1 gets output 0 delayed one block.
    void run(int blocks, float consoleValue = 0.0f)
    {
        for (int b = 0; b < blocks; ++b) {
            std::fill(in[0].begin(), in[0].end(), consoleValue);
            std::copy(loopback, loopback + kBlock, in[1].begin());
            const float* ins[kIn] = { in[0].data(), in[1].data() };
            float* outs[kOut] = { out[0].data(), out[1].data(), out[2].data(), out[3].data() };
            engine.processBlock(ins, kIn, outs, kOut, kBlock);
            std::copy(out[0].begin(), out[0].end(), loopback);
            engine.analyzer().process();
        }
    }
};
}

TEST_CASE("Engine starts fault-muted and passes console audio once unmuted")
{
    Harness h;
    h.state.outputs[1].mix[0] = 1.0f;
    h.apply();
    h.run(100, 0.5f);
    CHECK(h.out[1].back() == 0.0f); // muted at startup
    h.engine.unmuteAfterFault();
    h.run(100, 0.5f);
    CHECK_THAT(h.out[1].back(), WithinAbs(0.5, 1e-4));
    CHECK(h.out[2].back() == 0.0f); // not routed
}

TEST_CASE("Mic-role inputs are never routed to outputs (feedback guard)")
{
    Harness h;
    h.state.outputs[2].mix[1] = 1.0f; // input 1 is a mic
    h.apply();
    h.engine.unmuteAfterFault();
    h.in[1].assign(Harness::kBlock, 0.0f);
    std::fill(std::begin(h.loopback), std::end(h.loopback), 0.7f);
    h.run(1);
    CHECK(h.out[2].back() == 0.0f);
}

TEST_CASE("Device fault re-mutes; MUTE ALL silences")
{
    Harness h;
    h.state.outputs[0].mix[0] = 1.0f;
    h.apply();
    h.engine.unmuteAfterFault();
    h.run(100, 0.5f);
    REQUIRE(h.out[0].back() != 0.0f);
    h.engine.simulateDeviceFault();
    h.run(100, 0.5f);
    CHECK(h.out[0].back() == 0.0f);
    CHECK(h.engine.status().faultMuted);
    h.engine.unmuteAfterFault();
    h.engine.setMuteAll(true);
    h.run(100, 0.5f);
    CHECK(h.out[0].back() == 0.0f);
}

TEST_CASE("Generator plays pink noise at the set level on target outputs only")
{
    Harness h;
    h.state.gen.on = true;
    h.state.gen.levelDb = -20.0f;
    h.state.gen.targets = { 0 };
    h.state.outputs[0].limiterOn = false;
    h.apply();
    h.engine.unmuteAfterFault();
    h.run(1500); // fade-in
    double sum = 0;
    for (int b = 0; b < 750; ++b) { h.run(1); for (float x : h.out[0]) sum += double(x) * x; }
    const double rmsDb = 10 * std::log10(sum / (750.0 * Harness::kBlock));
    CHECK_THAT(rmsDb, WithinAbs(-20.0, 1.0));
    CHECK(h.out[3].back() == 0.0f);
}

TEST_CASE("Internal loopback: analyzer measures the output-to-mic loop delay")
{
    Harness h;
    h.state.gen.on = true;
    h.state.gen.levelDb = -20.0f;
    h.state.gen.targets = { 0 };
    h.state.measure.micInput = 1;     // input 1 = output 0 delayed by one block
    h.state.outputs[0].delayMs = 1.0f; // + 48 samples of processing delay
    h.apply();
    h.engine.unmuteAfterFault();
    h.run(6000); // 8 s
    CHECK_THAT(h.engine.analyzer().delaySamples(), WithinAbs(64.0 + 48.0, 0.2));
    auto r = h.engine.analyzer().result();
    REQUIRE(!r.magDb.empty());
    CHECK_THAT(r.magDb[100], WithinAbs(0.0, 0.5));
}
