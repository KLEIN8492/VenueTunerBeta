#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/FirLoader.h"
#include "dsp/OutputChain.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <random>
#include <thread>

using Catch::Matchers::WithinAbs;

TEST_CASE("FirLoader parses text and WAV, finds the peak")
{
    auto txt = juce::File::createTempFile(".txt");
    txt.replaceWithText("* rePhase export\n# comment\n0.0\n0.25\n1.0\n0.25\n0.0\n");
    juce::AudioBuffer<float> ir;
    double fileSr = 0;
    juce::String err;
    REQUIRE(vt::FirLoader::load(txt, 48000, ir, fileSr, err));
    CHECK(ir.getNumSamples() == 64); // 5 taps, zero-padded
    CHECK(ir.getSample(0, 2) == 1.0f);
    CHECK(ir.getSample(0, 40) == 0.0f);
    CHECK(fileSr == 48000);
    CHECK(vt::FirLoader::peakIndex(ir) == 2);

    auto wav = juce::File::createTempFile(".wav");
    {
        juce::AudioBuffer<float> b(1, 255);
        for (int i = 0; i < 255; ++i) {
            const double x = i - 127;
            b.setSample(0, i, float(x == 0 ? 1.0 : std::sin(0.8 * M_PI * x) / (0.8 * M_PI * x)));
        }
        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(new juce::FileOutputStream(wav), 96000, 1, 32, {}, 0));
        REQUIRE(w != nullptr);
        w->writeFromAudioSampleBuffer(b, 0, 255);
    }
    REQUIRE(vt::FirLoader::load(wav, 48000, ir, fileSr, err));
    CHECK(fileSr == 96000);
    CHECK(vt::FirLoader::peakIndex(ir) == 127);

    txt.replaceWithText("hello\n");
    CHECK_FALSE(vt::FirLoader::load(txt, 48000, ir, fileSr, err));
    txt.deleteFile();
    wav.deleteFile();
}

namespace {
// Runs blocks through a chain until the convolution has picked up an IR of `taps`.
void waitForIr(vt::OutputChain& chain, juce::dsp::Convolution& conv, vt::OutputParams& p, int taps)
{
    std::vector<float> buf(64, 0.0f);
    for (int i = 0; i < 2000 && int(conv.getCurrentIRSize()) != taps; ++i) {
        chain.process(p, buf.data(), 64, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(int(conv.getCurrentIRSize()) == taps);
    for (int i = 0; i < 200; ++i) chain.process(p, buf.data(), 64, false); // finish the crossfade
}
}

TEST_CASE("FIR convolution: delta delays exactly; random IR equals direct convolution")
{
    juce::dsp::ConvolutionMessageQueue queue;
    juce::dsp::Convolution conv(juce::dsp::Convolution::NonUniform { 256 }, queue);
    vt::OutputChain chain;
    chain.setConvolution(&conv);
    chain.prepare(48000, 4096);
    vt::OutputParams p {};
    p.limiterOn = false;
    p.firOn = true;

    SECTION("delta at index 10")
    {
        juce::AudioBuffer<float> ir(1, 32);
        ir.clear();
        ir.setSample(0, 10, 1.0f);
        conv.loadImpulseResponse(std::move(ir), 48000, juce::dsp::Convolution::Stereo::no,
                                 juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
        waitForIr(chain, conv, p, 32);
        std::vector<float> buf(64, 0.0f);
        buf[0] = 1.0f;
        chain.process(p, buf.data(), 64, false);
        CHECK_THAT(buf[10], WithinAbs(1.0, 1e-5));
        CHECK_THAT(buf[9], WithinAbs(0.0, 1e-5));
        CHECK_THAT(buf[0], WithinAbs(0.0, 1e-5));
    }

    SECTION("random 300-tap IR")
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> u(-1, 1);
        std::vector<float> h(300);
        for (auto& x : h) x = u(rng) * 0.1f;
        juce::AudioBuffer<float> ir(1, 300);
        for (int i = 0; i < 300; ++i) ir.setSample(0, i, h[size_t(i)]);
        conv.loadImpulseResponse(std::move(ir), 48000, juce::dsp::Convolution::Stereo::no,
                                 juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
        waitForIr(chain, conv, p, 300);
        std::vector<float> in(4096), out(4096);
        for (auto& x : in) x = u(rng);
        // Prime with zeros so the history is known, then process the noise in 64-sample blocks.
        std::vector<float> z(64, 0.0f);
        for (int i = 0; i < 20; ++i) chain.process(p, z.data(), 64, false);
        for (int b = 0; b < 64; ++b) {
            std::copy(in.begin() + b * 64, in.begin() + (b + 1) * 64, out.begin() + b * 64);
            chain.process(p, out.data() + b * 64, 64, false);
        }
        float maxErr = 0;
        for (int n = 0; n < 4096; ++n) {
            double y = 0;
            for (int k = 0; k < 300 && k <= n; ++k) y += double(h[size_t(k)]) * in[size_t(n - k)];
            maxErr = std::max(maxErr, float(std::abs(y - out[size_t(n)])));
        }
        CHECK(maxErr < 1e-4f);
    }
}

TEST_CASE("FIR on/off crossfades instead of jumping")
{
    juce::dsp::ConvolutionMessageQueue queue;
    juce::dsp::Convolution conv(juce::dsp::Convolution::NonUniform { 256 }, queue);
    vt::OutputChain chain;
    chain.setConvolution(&conv);
    chain.prepare(48000, 4096);
    vt::OutputParams p {};
    p.limiterOn = false;
    p.firLoaded = true;
    p.firOn = false;
    juce::AudioBuffer<float> ir(1, 32);
    ir.clear();
    ir.setSample(0, 0, 0.25f); // wet = −12 dB copy of dry, no delay
    conv.loadImpulseResponse(std::move(ir), 48000, juce::dsp::Convolution::Stereo::no,
                             juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    std::vector<float> buf(64);
    float last = 1.0f; // carried across runs so a toggle is measured where it happens
    auto run = [&](int blocks, float* maxJump) {
        for (int b = 0; b < blocks; ++b) {
            std::fill(buf.begin(), buf.end(), 1.0f);
            chain.process(p, buf.data(), 64, false);
            for (float y : buf) { if (maxJump) *maxJump = std::max(*maxJump, std::abs(y - last)); last = y; }
        }
        return last;
    };
    for (int i = 0; i < 2000 && conv.getCurrentIRSize() != 32; ++i) { run(1, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    REQUIRE(conv.getCurrentIRSize() == 32);
    CHECK_THAT(run(100, nullptr), WithinAbs(1.0, 1e-3)); // off: dry
    p.firOn = true;
    float jump = 0;
    CHECK_THAT(run(100, &jump), WithinAbs(0.25, 1e-3)); // on: wet
    CHECK(jump < 0.01f);                                 // 0.75 change spread over 30 ms
    p.firOn = false;
    jump = 0;
    CHECK_THAT(run(100, &jump), WithinAbs(1.0, 1e-3));
    CHECK(jump < 0.01f);
}
