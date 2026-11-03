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
        // Console on physical in 0, measurement mic on physical in 1; outputs patched 1:1.
        state.sources = { { "Console", "program", 0 }, { "Mic", "mic", 1 } };
        for (int o = 0; o < kOut; ++o) {
            vt::OutputConfig c;
            c.name = "Out " + juce::String(o + 1);
            c.channel = o;
            c.mix = { 0.0f, 0.0f };
            state.outputs.push_back(c);
        }
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
            engine.micMeters().process();
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
    h.state.measure.micSource = 1;    // input 1 = output 0 delayed by one block
    h.state.outputs[0].delayMs = 1.0f; // + 48 samples of processing delay
    h.apply();
    h.engine.unmuteAfterFault();
    h.run(6000); // 8 s
    CHECK_THAT(h.engine.analyzer().delaySamples(), WithinAbs(64.0 + 48.0, 0.2));
    auto r = h.engine.analyzer().result();
    REQUIRE(!r.magDb.empty());
    CHECK_THAT(r.magDb[100], WithinAbs(0.0, 0.5));
}

TEST_CASE("Sound level keeps measuring the user's mic while a job borrows the analyzer")
{
    Harness h;
    h.state.gen.on = true;
    h.state.gen.levelDb = -20.0f;
    h.state.gen.targets = { 0 };
    h.state.measure.micSource = 1;
    h.apply();
    h.engine.unmuteAfterFault();
    h.run(1500); // 2 s of pink noise on the mic
    const double before = h.engine.spl().reading().leqWindow[0][0]; // last second
    // A job (e.g. latency calibration) points the analyzer at another input carrying a loud signal.
    auto p = vt::toParams(h.state);
    p.measure.micInput = 0;
    h.engine.setParams(p);
    h.run(1500, 0.5f);
    CHECK_THAT(h.engine.spl().reading().leqWindow[0][0], WithinAbs(before, 1.0)); // still the pink noise, not −6 dBFS
}

TEST_CASE("Outputs write to their patched physical channel; the rest stays silent")
{
    Harness h;
    h.state.outputs.resize(2);
    h.state.outputs[0].channel = 3;   // logical output 0 → physical 3
    h.state.outputs[0].mix[0] = 1.0f;
    h.state.outputs[1].channel = -1;  // unassigned: processed, goes nowhere
    h.state.outputs[1].mix[0] = 1.0f;
    h.apply();
    h.engine.unmuteAfterFault();
    h.run(100, 0.5f);
    CHECK_THAT(h.out[3].back(), WithinAbs(0.5, 1e-4));
    CHECK(h.out[0].back() == 0.0f);
    CHECK(h.out[1].back() == 0.0f);
    CHECK(h.out[2].back() == 0.0f);
}

TEST_CASE("Silence after signal does not leave denormals in the output")
{
    Harness h;
    h.state.outputs[0].mix[0] = 1.0f;
    h.state.outputs[0].hp = { "lr48", 30 };   // long low-frequency decay
    h.state.outputs[0].lp = { "lr48", 80 };
    h.apply();
    h.engine.unmuteAfterFault();
    h.run(500, 0.5f);
    h.run(20000, 0.0f); // ~27 s of silence: states decay towards zero
    for (float y : h.out[0]) CHECK((y == 0.0f || std::fpclassify(y) == FP_NORMAL));
}

TEST_CASE("Several mics are measured at the same time against one reference")
{
    // 3 inputs: 0 console, 1 = output 0 one block later (mic A), 2 = same at half level (mic B).
    constexpr int kIn = 3, kOut = 2, kBlock = 64;
    vt::AudioEngine engine;
    engine.prepareForTest(48000.0, kBlock, kIn, kOut);
    vt::SystemState s;
    s.sources = { { "Console", "program", 0 }, { "Mic A", "mic", 1 }, { "Mic B", "mic", 2 } };
    for (int o = 0; o < kOut; ++o) { vt::OutputConfig c; c.channel = o; c.mix = { 0, 0, 0 }; s.outputs.push_back(c); }
    s.gen.on = true;
    s.gen.levelDb = -20.0f;
    s.gen.targets = { 0 };
    s.measure.micSource = 1;
    auto p = vt::toParams(s);
    p.measure.extraMicInputs[0] = 2;
    p.measure.numExtra = 1;
    engine.setParams(p);
    engine.unmuteAfterFault();
    REQUIRE(engine.analyzerForSlot(1) != nullptr);

    std::vector<std::vector<float>> in(kIn, std::vector<float>(kBlock)), out(kOut, std::vector<float>(kBlock));
    std::vector<float> prev(kBlock, 0.0f);
    for (int b = 0; b < 6000; ++b) {
        for (int i = 0; i < kBlock; ++i) { in[1][size_t(i)] = prev[size_t(i)]; in[2][size_t(i)] = 0.5f * prev[size_t(i)]; }
        const float* ins[kIn] = { in[0].data(), in[1].data(), in[2].data() };
        float* outs[kOut] = { out[0].data(), out[1].data() };
        engine.processBlock(ins, kIn, outs, kOut, kBlock);
        prev = out[0];
        engine.analyzer().process();
        engine.analyzerForSlot(1)->process();
    }
    auto a = engine.analyzer().result(), bRes = engine.analyzerForSlot(1)->result();
    CHECK_THAT(engine.analyzer().delaySamples(), WithinAbs(64.0, 0.2));
    CHECK_THAT(engine.analyzerForSlot(1)->delaySamples(), WithinAbs(64.0, 0.2));
    REQUIRE(!bRes.magDb.empty());
    CHECK_THAT(a.magDb[100], WithinAbs(0.0, 0.3));
    CHECK_THAT(bRes.magDb[100], WithinAbs(-6.02, 0.3));
}

namespace {
// Largest sample-to-sample step of output 0 over `blocks` blocks, with the given params.
float maxStep(Harness& h, const vt::Params& p, int blocks, float& prev)
{
    h.engine.setParams(p);
    float worst = 0.0f;
    for (int b = 0; b < blocks; ++b) {
        h.run(1);
        for (float x : h.out[0]) { worst = std::max(worst, std::abs(x - prev)); prev = x; }
    }
    return worst;
}

// A 10 s sweep on output 0 at -12 dBFS RMS: 0.5 s in, it is a ~28 Hz sine of amplitude 0.36,
// whose own sample-to-sample step is ~0.0013. A hard cut there would step by up to 0.36.
vt::Params sweepParams(Harness& h, uint32_t id, float levelDb)
{
    h.state.gen.on = true;
    h.state.gen.levelDb = levelDb;
    h.state.gen.targets = { 0 };
    h.state.outputs[0].limiterOn = false;
    auto p = vt::toParams(h.state);
    p.gen.mode = vt::GeneratorParams::Sweep;
    p.gen.sweepId = id;
    p.gen.sweepLength = 480000;
    return p;
}
}

TEST_CASE("Generator: re-triggering a sweep mid-stream fades out and in instead of cutting")
{
    Harness h;
    h.engine.unmuteAfterFault();
    float prev = 0.0f;
    const float steady = maxStep(h, sweepParams(h, 1, -12.0f), 375, prev); // 0.5 s into the sweep
    CHECK(steady < 0.003f);
    const float onRetrigger = maxStep(h, sweepParams(h, 2, -12.0f), 50, prev); // new sweepId
    CHECK(onRetrigger < 0.003f); // 10 ms fade: 0.36 / 480 per sample at most, + the sine itself
}

TEST_CASE("Generator: a sweep level change ramps instead of jumping")
{
    Harness h;
    h.engine.unmuteAfterFault();
    float prev = 0.0f;
    maxStep(h, sweepParams(h, 1, -12.0f), 375, prev);
    const float onLevel = maxStep(h, sweepParams(h, 1, -30.0f), 50, prev);
    CHECK(onLevel < 0.003f);
}

TEST_CASE("Generator: pink noise fades out before a sweep starts (no instant cut)")
{
    // Two identical engines play the same (deterministic) pink noise; one is then asked for a
    // sweep. Their sample ratio is exactly the switch fade: it must glide 1 → 0 over ~10 ms.
    Harness a, b;
    for (Harness* h : { &a, &b }) {
        h->state.gen.on = true;
        h->state.gen.levelDb = -12.0f;
        h->state.gen.targets = { 0 };
        h->state.outputs[0].limiterOn = false;
        h->apply();
        h->engine.unmuteAfterFault();
        h->run(1500); // pink fully faded in
    }
    auto p = sweepParams(b, 1, -12.0f);
    p.gen.sweepLength = 48000;
    b.engine.setParams(p);
    std::vector<float> ratio;
    for (int blk = 0; blk < 7; ++blk) { // 448 samples: inside the 480-sample fade
        a.run(1);
        b.run(1);
        for (int i = 0; i < Harness::kBlock; ++i)
            ratio.push_back(std::abs(a.out[0][size_t(i)]) > 1e-3f ? b.out[0][size_t(i)] / a.out[0][size_t(i)] : std::nanf(""));
    }
    auto near = [&](int at) { for (int k = at; k < at + 32; ++k) if (std::isfinite(ratio[size_t(k)])) return ratio[size_t(k)]; return -1.0f; };
    CHECK(near(0) > 0.97f);                         // not cut on the spot
    CHECK_THAT(near(240), WithinAbs(0.5, 0.08));    // halfway through the 10 ms fade
    CHECK(near(410) < 0.2f);
    float last = 2.0f;
    for (float r : ratio) if (std::isfinite(r)) { CHECK(r <= last + 1e-4f); last = r; } // monotonic
}
