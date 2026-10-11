#include "dsp/PinkPeriodic.h"
#include <cmath>
#include <complex>
#include <map>
#include <mutex>
#include <random>

namespace vt {

void PinkPeriodic::prepare(double sampleRate)
{
    data = build(sampleRate);
    pos = 0;
}

std::shared_ptr<const std::vector<float>> PinkPeriodic::build(double sr)
{
    static std::mutex lock;
    static std::map<int, std::shared_ptr<const std::vector<float>>> cache;
    std::lock_guard<std::mutex> l(lock);
    if (auto it = cache.find(int(std::lround(sr))); it != cache.end()) return it->second;

    const int N = 1 << kOrder;
    const double hi = std::min(kHighHz, 0.49 * sr);
    juce::dsp::FFT fft(kOrder);

    // Target magnitudes: 1/sqrt(f) (power ∝ 1/f) inside the band, zero outside.
    std::vector<float> mag(size_t(N / 2 + 1), 0.0f);
    for (int k = 1; k <= N / 2; ++k) {
        const double f = k * sr / N;
        if (f >= kLowHz && f <= hi) mag[size_t(k)] = float(1.0 / std::sqrt(f));
    }

    // Random phases from a fixed seed: the same signal on every run and every machine.
    std::mt19937 rng(0x56544E31u); // "VTN1"
    std::uniform_real_distribution<float> phase(0.0f, 2.0f * juce::MathConstants<float>::pi);
    std::vector<std::complex<float>> spec(static_cast<size_t>(N)), sig(static_cast<size_t>(N));
    for (int k = 0; k <= N / 2; ++k) spec[size_t(k)] = std::polar(mag[size_t(k)], phase(rng));
    spec[0] = spec[size_t(N / 2)] = 0.0f;

    auto toTime = [&] {
        for (int k = 1; k < N / 2; ++k) spec[size_t(N - k)] = std::conj(spec[size_t(k)]); // real signal
        fft.perform(spec.data(), sig.data(), true);
    };
    auto rmsOf = [&] { double e = 0; for (auto& c : sig) e += double(c.real()) * c.real(); return std::sqrt(e / N); };

    // Crest factor reduction: clip at the target crest, restore the pink magnitudes, repeat.
    toTime();
    for (int it = 0; it < 40; ++it) {
        const float limit = float(rmsOf() * std::pow(10.0, kCrestDb / 20.0));
        float peak = 0.0f;
        for (auto& c : sig) peak = std::max(peak, std::abs(c.real()));
        if (peak <= limit * 1.03f) break; // within ~0.25 dB of the target
        for (auto& c : sig) c = std::clamp(c.real(), -limit, limit);
        fft.perform(sig.data(), spec.data(), false);
        for (int k = 0; k <= N / 2; ++k) {
            const float m = std::abs(spec[size_t(k)]);
            spec[size_t(k)] = m > 0.0f && mag[size_t(k)] > 0.0f ? spec[size_t(k)] * (mag[size_t(k)] / m) : 0.0f;
        }
        toTime();
    }

    const double rms = rmsOf();
    auto out = std::make_shared<std::vector<float>>(size_t(N));
    for (int i = 0; i < N; ++i) (*out)[size_t(i)] = float(sig[size_t(i)].real() / rms);
    cache[int(std::lround(sr))] = out;
    return out;
}

} // namespace vt
