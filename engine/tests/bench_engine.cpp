// Not a correctness test: a timing benchmark, run with `vt_tests "[.bench]"`.
#include <catch2/catch_test_macros.hpp>
#include "core/AudioEngine.h"
#include <chrono>
#include <cstdio>

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
            c.ap = { { true, 2, 120, 0.7f }, { true, 2, 2000, 0.7f }, { true, 1, 500, 0.7f }, { true, 2, 8000, 0.7f } };
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

TEST_CASE("Benchmark: full processing load", "[.bench]")
{
    runBench(false);
    runBench(true);
    SUCCEED();
}
