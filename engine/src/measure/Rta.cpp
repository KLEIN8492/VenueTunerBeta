#include "measure/Rta.h"
#include <cmath>

namespace vt {

void Rta::prepare(double sampleRate)
{
    sr = sampleRate;
    window.resize(N);
    windowPower = 0;
    for (int i = 0; i < N; ++i) {
        window[size_t(i)] = float(0.5 - 0.5 * std::cos(2.0 * M_PI * i / N));
        windowPower += double(window[size_t(i)]) * window[size_t(i)];
    }
    ring.assign(N, 0.0f);
    buf.assign(2 * N, 0.0f);
    power.assign(N / 2 + 1, 0.0);
    writePos = sinceFrame = 0;
    frames = 0;
}

void Rta::process(const float* x, int n)
{
    for (int i = 0; i < n; ++i) {
        ring[writePos] = x[i];
        writePos = (writePos + 1) % N;
        if (++sinceFrame >= N / 2) { sinceFrame = 0; frame(); } // 50 % overlap
    }
}

void Rta::frame()
{
    for (int i = 0; i < N; ++i) buf[size_t(i)] = ring[(writePos + size_t(i)) % N] * window[size_t(i)];
    std::fill(buf.begin() + N, buf.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform(buf.data(), true);
    ++frames;
    // ~1 s averaging: frames arrive every N/2 samples.
    const double tau = 1.0 * sr / (N / 2);
    const double alpha = frames < tau ? 1.0 / frames : 1.0 / tau;
    for (int k = 0; k <= N / 2; ++k) power[size_t(k)] += alpha * (double(buf[size_t(k)]) * buf[size_t(k)] - power[size_t(k)]);

    // Mean square in a band = 2·Σ|X_k|² / (N·Σw²) (one-sided spectrum, Hann-compensated).
    Result r;
    const int res = resolution.load();
    const double df = sr / N;
    for (int b = 0;; ++b) {
        const double fc = 1000.0 * std::pow(2.0, (b - 10 * res) / double(res)); // starts near 1 Hz
        if (fc < 19.0) continue;
        if (fc > 20500.0) break;
        const double lo = fc * std::pow(2.0, -0.5 / res), hi = fc * std::pow(2.0, 0.5 / res);
        // Each bin covers [k−½, k+½]·df; weight it by how much of that falls inside the band,
        // otherwise narrow low bands zig-zag as whole bins drop in and out.
        double p = 0;
        const int k0 = std::max(0, int(std::floor(lo / df - 0.5))), k1 = std::min(N / 2, int(std::ceil(hi / df + 0.5)));
        for (int k = k0; k <= k1; ++k) {
            const double overlap = std::min(hi, (k + 0.5) * df) - std::max(lo, (k - 0.5) * df);
            if (overlap > 0) p += power[size_t(k)] * overlap / df;
        }
        const double ms = 2.0 * p / (double(N) * windowPower);
        r.freq.push_back(float(fc));
        r.levelDb.push_back(float(ms > 0 ? 10.0 * std::log10(ms) : -200.0));
    }
    std::lock_guard<std::mutex> l(lock);
    latest = std::move(r);
}

} // namespace vt
