// Not a correctness test: a timing benchmark, run with `vt_tests "[.bench]"`.
#include <catch2/catch_test_macros.hpp>
#include "core/AudioEngine.h"
#include <chrono>
#include <cstdio>
#include <thread>

static void runBench(bool filters)
{
    constexpr int kIn = 2, kOut = 16, kBlock = 64;
    constexpr double sr = 48000;
    vt::AudioEngine engine;
    engine.prepareForTest(sr, kBlock, kIn, kOut);
    vt::SystemState s;
    s.sources = { { "L", "program", 0 }, { "R", "program", 1 } };
    for (int o = 0; o < kOut; ++o) {
        vt::OutputConfig c;
        c.channel = o;
        c.mix = { 0.5f, 0.5f };
        c.delayMs = 3.0f;
        if (filters) for (int b = 0; b < 16; ++b) c.peq.push_back({ true, "bell", 50.0f * float(b + 1), (b % 2 ? -3.0f : 2.0f), 2.0f });
        if (filters) {
            c.hp = { "lr24", 80 };
            c.lp = { "lr48", 16000 };
            c.ap = { { true, 2, "2nd", 120, 0.7f }, { true, 2, "2nd", 2000, 0.7f }, { true, 1, "1st", 500, 0.7f }, { true, 2, "lr48", 8000, 0.7f } };
        }
        s.outputs.push_back(c);
    }
    engine.setParams(vt::toParams(s, sr));
    engine.unmuteAfterFault();
    std::vector<std::vector<float>> in(kIn, std::vector<float>(kBlock)), out(kOut, std::vector<float>(kBlock));
    for (auto& ch : in) for (size_t i = 0; i < ch.size(); ++i) ch[i] = float(std::sin(0.01 * double(i)));
    const float* ins[kIn] = { in[0].data(), in[1].data() };
    float* outs[kOut];
    for (int o = 0; o < kOut; ++o) outs[o] = out[size_t(o)].data();

    for (int i = 0; i < 2000; ++i) engine.processBlock(ins, kIn, outs, kOut, kBlock); // warm-up
    const int blocks = 20000; // ~26.7 s of audio
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < blocks; ++i) engine.processBlock(ins, kIn, outs, kOut, kBlock);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double audio = blocks * kBlock / sr;
    std::printf("16 outputs, %s, 48 kHz / 64: %.2f us per block, %.2f %% of one core\n",
                filters ? "28 IIR sections each" : "no filters", sec / blocks * 1e6, 100.0 * sec / audio);
}

// 16 outputs, each with an 8192-tap FIR loaded but switched off: the case where an idle
// convolution used to keep burning CPU. The engine's control thread is simulated by offering
// the unload hook every block (Dispatcher does it every 250 ms).
static void runFirOffBench()
{
    constexpr int kIn = 2, kOut = 16, kBlock = 64;
    constexpr double sr = 48000;
    vt::AudioEngine engine;
    engine.prepareForTest(sr, kBlock, kIn, kOut);
    vt::SystemState s;
    s.sources = { { "L", "program", 0 }, { "R", "program", 1 } };
    for (int o = 0; o < kOut; ++o) {
        vt::OutputConfig c;
        c.channel = o;
        c.mix = { 0.5f, 0.5f };
        c.fir.on = false;
        c.fir.taps = 8192; // "loaded" (firLoaded) as far as the processing is concerned
        s.outputs.push_back(c);
    }
    auto p = vt::toParams(s, sr);
    for (int o = 0; o < kOut; ++o) p.out[o].slot = o;
    engine.setParams(p);
    engine.unmuteAfterFault();
    for (int o = 0; o < kOut; ++o) {
        juce::AudioBuffer<float> ir(1, 8192);
        for (int i = 0; i < 8192; ++i) ir.setSample(0, i, 0.001f * float(std::sin(0.1 * i)) * float(std::exp(-i / 2000.0)));
        engine.loadFir(o, std::move(ir), sr);
    }
    std::vector<std::vector<float>> in(kIn, std::vector<float>(kBlock)), out(kOut, std::vector<float>(kBlock));
    for (auto& ch : in) for (size_t i = 0; i < ch.size(); ++i) ch[i] = float(std::sin(0.01 * double(i)));
    const float* ins[kIn] = { in[0].data(), in[1].data() };
    float* outs[kOut];
    for (int o = 0; o < kOut; ++o) outs[o] = out[size_t(o)].data();
    // Warm-up 4 s: IRs land, then (with the parking) the FIRs settle, unload and park.
    for (int i = 0; i < 3000; ++i) {
        engine.processBlock(ins, kIn, outs, kOut, kBlock);
        for (int o = 0; o < kOut; ++o) (void) engine.unloadFirIfIdle(o);
        if (i % 50 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1)); // let the IR thread work
    }
    const int blocks = 20000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < blocks; ++i) engine.processBlock(ins, kIn, outs, kOut, kBlock);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("16 outputs, 8192-tap FIR loaded but off, 48 kHz / 64: %.2f us per block, %.2f %% of one core\n",
                sec / blocks * 1e6, 100.0 * sec / (blocks * kBlock / sr));
}

TEST_CASE("Benchmark: FIR loaded but off", "[.bench]")
{
    runFirOffBench();
    SUCCEED();
}

TEST_CASE("Benchmark: full processing load", "[.bench]")
{
    runBench(false);
    runBench(true);
    SUCCEED();
}
