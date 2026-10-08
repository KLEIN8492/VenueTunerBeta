#include "measure/Analyzer.h"
#include <cmath>

namespace vt {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int L = 1 << Analyzer::kDelayOrder;
using cfloat = std::complex<float>;
}

Analyzer::Analyzer()
{
    refHist.assign(kHistory, 0.0f);
    measHist.assign(kHistory, 0.0f);
    scratch.assign(1 << 18, 0.0f);
    bigX.assign(4 * L, 0.0f);
    bigY.assign(4 * L, 0.0f);
    configureBands();
}

void Analyzer::Band::clear()
{
    frameCount = 0;
    std::fill(sxx.begin(), sxx.end(), 0.0);
    std::fill(syy.begin(), syy.end(), 0.0);
    std::fill(sxy.begin(), sxy.end(), std::complex<double> {});
}

// Window lengths are chosen in time (defined at 48 kHz, scaled with the rate) so that each
// band keeps ~1/24-octave resolution at its lowest frequency.
void Analyzer::configureBands()
{
    struct Spec { int order48k; double lo, hi; };
    std::vector<Spec> specs;
    if (multiWindow.load()) specs = { { 15, 0, 200 }, { 13, 200, 800 }, { 11, 800, 3200 }, { 9, 3200, 1e9 } };
    else specs = { { kFixedOrder, 0, 1e9 } };
    const int rateShift = std::max(0, int(std::lround(std::log2(sr / 48000.0))));
    bands.clear();
    bands.resize(specs.size());
    for (size_t b = 0; b < specs.size(); ++b) {
        auto& band = bands[b];
        band.order = std::min(specs[b].order48k + rateShift, 16);
        band.n = 1 << band.order;
        band.fLo = specs[b].lo;
        band.fHi = specs[b].hi;
        band.avgScale = std::max(1, (1 << kFixedOrder) / band.n);
        band.fft = std::make_unique<juce::dsp::FFT>(band.order);
        band.window.resize(size_t(band.n));
        for (int i = 0; i < band.n; ++i) band.window[size_t(i)] = float(0.5 - 0.5 * std::cos(2.0 * kPi * i / band.n));
        band.bufX.assign(size_t(2 * band.n), 0.0f);
        band.bufY.assign(size_t(2 * band.n), 0.0f);
        band.sxx.assign(size_t(band.n / 2 + 1), 0.0);
        band.syy.assign(size_t(band.n / 2 + 1), 0.0);
        band.sxy.assign(size_t(band.n / 2 + 1), {});
        band.nextFrameEnd = band.n;
    }
}

void Analyzer::prepare(double sampleRate)
{
    sr = sampleRate;
    resetRequested.store(false);
    refFifo.clear();
    measFifo.clear();
    doReset();
}

void Analyzer::doReset()
{
    std::fill(refHist.begin(), refHist.end(), 0.0f);
    std::fill(measHist.begin(), measHist.end(), 0.0f);
    total = 0;
    nextDelayAt = L;
    configureBands();
    delayValid = false;
    alignInt = 0;
    delayTotal.store(0.0);
    irExcerpt.clear();
    std::lock_guard<std::mutex> l(resultLock);
    latest = Result {};
    latest.sampleRate = float(sr);
}

bool Analyzer::process()
{
    if (resetRequested.exchange(false)) {
        if (const double p = pendingSr.exchange(0.0); p > 0.0) sr = p;
        refFifo.clear();
        measFifo.clear();
        doReset();
    }

    // Consume ref and meas in lock-step (the audio thread always pushes both).
    if (const int64_t len = sweepRequest.exchange(0); len > 0)
        analyseSweep(std::min<int64_t>(len, std::min<int64_t>(total, kHistory)));

    const size_t n = std::min(refFifo.available(), measFifo.available());
    if (n == 0) return false;
    size_t done = 0;
    while (done < n) {
        const size_t chunk = std::min(n - done, scratch.size());
        refFifo.pop(scratch.data(), chunk);
        for (size_t i = 0; i < chunk; ++i) refHist[size_t(total + int64_t(i)) & (kHistory - 1)] = scratch[i];
        measFifo.pop(scratch.data(), chunk);
        for (size_t i = 0; i < chunk; ++i) measHist[size_t(total + int64_t(i)) & (kHistory - 1)] = scratch[i];
        total += int64_t(chunk);
        done += chunk;
    }

    bool changed = false;
    if (total >= nextDelayAt) {
        findDelay();
        nextDelayAt = total + int64_t(sr); // re-check once per second
        changed = true;
    }
    for (auto& band : bands)
        while (band.nextFrameEnd <= total) {
            if (delayValid) addFrame(band, band.nextFrameEnd);
            band.nextFrameEnd += band.n / 2; // 50 % overlap
            changed = true;
        }
    if (changed) buildResult();
    return changed;
}

// Impulse response by regularised deconvolution of the last L samples (zero-padded to
// 2L, so correlation is linear, not circular). The IR peak gives the delay.
void Analyzer::findDelay()
{
    const int64_t start = total - L;
    std::fill(bigX.begin(), bigX.end(), 0.0f);
    std::fill(bigY.begin(), bigY.end(), 0.0f);
    for (int i = 0; i < L; ++i) {
        bigX[size_t(i)] = hist(refHist, start + i);
        bigY[size_t(i)] = hist(measHist, start + i);
    }
    delayFft.performRealOnlyForwardTransform(bigX.data());
    delayFft.performRealOnlyForwardTransform(bigY.data());
    auto* X = reinterpret_cast<cfloat*>(bigX.data());
    auto* Y = reinterpret_cast<cfloat*>(bigY.data());
    const int M = 2 * L;
    double meanPow = 0;
    for (int k = 0; k <= M / 2; ++k) meanPow += std::norm(X[k]);
    meanPow /= (M / 2 + 1);
    if (meanPow <= 1e-12) { delayValid = false; return; } // no reference signal
    const double eps = 1e-3 * meanPow;
    for (int k = 0; k <= M / 2; ++k) {
        const cfloat h = Y[k] * std::conj(X[k]) / float(std::norm(X[k]) + eps);
        Y[k] = h;
        if (k > 0 && k < M / 2) Y[M - k] = std::conj(h);
    }
    delayFft.performRealOnlyInverseTransform(bigY.data());
    const float* ir = bigY.data();

    // Peak in positive lags (measurement can only lag the reference).
    int peak = 0;
    double sumSq = 0;
    for (int k = 0; k < L; ++k) {
        sumSq += double(ir[k]) * ir[k];
        if (std::abs(ir[k]) > std::abs(ir[peak])) peak = k;
    }
    const double rms = std::sqrt(sumSq / L);
    const double pk = std::abs(ir[peak]);
    if (!(pk > 0.0) || pk < 10.0 * rms) { delayValid = false; return; } // silence or no clear arrival

    const double a = std::abs(ir[(peak - 1 + M) % M]), b = pk, c = std::abs(ir[peak + 1]);
    const double denom = a - 2 * b + c;
    const double frac = std::abs(denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0;
    const double d = peak + std::clamp(frac, -0.5, 0.5);

    if (!delayValid || std::abs(d - double(alignInt)) > 1.0) {
        // Alignment moved: restart averaging so frames with different alignments never mix.
        alignInt = int(std::lround(d));
        for (auto& band : bands) {
            band.clear();
            band.nextFrameEnd = std::max<int64_t>(band.n + peak + 1, total);
        }
    }
    delayTotal.store(d);
    delayValid = true;

    irStart = std::max(0, peak - 2048);
    const int irLen = std::min(8192, L - irStart);
    irExcerpt.assign(ir + irStart, ir + irStart + irLen);
}

void Analyzer::addFrame(Band& band, int64_t frameEnd)
{
    const int N = band.n;
    const int64_t mStart = frameEnd - N, rStart = mStart - alignInt;
    if (rStart < 0 || rStart < total - kHistory) return;

    for (int i = 0; i < N; ++i) {
        band.bufX[size_t(i)] = hist(refHist, rStart + i) * band.window[size_t(i)];
        band.bufY[size_t(i)] = hist(measHist, mStart + i) * band.window[size_t(i)];
    }
    std::fill(band.bufX.begin() + N, band.bufX.end(), 0.0f);
    std::fill(band.bufY.begin() + N, band.bufY.end(), 0.0f);
    band.fft->performRealOnlyForwardTransform(band.bufX.data(), true);
    band.fft->performRealOnlyForwardTransform(band.bufY.data(), true);
    const auto* X = reinterpret_cast<const cfloat*>(band.bufX.data());
    const auto* Y = reinterpret_cast<const cfloat*>(band.bufY.data());

    ++band.frameCount;
    const int frames = std::min(1024, averaging.load() * band.avgScale);
    const double alpha = 1.0 / std::min(band.frameCount, frames);
    for (int k = 0; k <= N / 2; ++k) {
        const std::complex<double> x(X[k]), y(Y[k]);
        band.sxx[size_t(k)] += alpha * (std::norm(x) - band.sxx[size_t(k)]);
        band.syy[size_t(k)] += alpha * (std::norm(y) - band.syy[size_t(k)]);
        band.sxy[size_t(k)] += alpha * (y * std::conj(x) - band.sxy[size_t(k)]);
    }
}

void Analyzer::buildResult()
{
    Result r;
    r.sampleRate = float(sr);
    r.delaySamples = delayTotal.load();
    r.delayValid = delayValid;
    r.ir = irExcerpt;
    r.irStart = irStart;
    int minFrames = bands.empty() ? 0 : bands[0].frameCount;
    for (auto& band : bands) minFrames = std::min(minFrames, band.frameCount);
    r.frames = minFrames;

    if (minFrames > 0) {
        const double frac = r.delaySamples - double(alignInt); // sub-sample remainder
        const int sm = smoothing.load();
        const double halfBand = sm > 0 ? 0.5 / sm : 1.0 / 48.0;
        MicCal c;
        { std::lock_guard<std::mutex> l(calLock); c = cal; }

        for (int i = 0;; ++i) {
            const double f = 20.0 * std::pow(2.0, i / 24.0);
            if (f > 20000.0) break;
            const Band* bp = &bands.back();
            for (auto& band : bands)
                if (f >= band.fLo && f < band.fHi) { bp = &band; break; }
            const Band& band = *bp;
            const int N = band.n;
            const double df = sr / N;
            int k0 = int(std::ceil(f * std::pow(2.0, -halfBand) / df));
            int k1 = int(std::floor(f * std::pow(2.0, halfBand) / df));
            if (sm == 0 || k1 < k0) k0 = k1 = int(std::lround(f / df));
            k0 = std::clamp(k0, 1, N / 2); k1 = std::clamp(k1, k0, N / 2);

            double px = 0, py = 0;
            std::complex<double> pxy;
            for (int k = k0; k <= k1; ++k) {
                // Remove the fractional part of the delay so phase is flat for a pure delay.
                const double w = 2.0 * kPi * (k * df) * frac / sr;
                pxy += band.sxy[size_t(k)] * std::polar(1.0, w);
                px += band.sxx[size_t(k)];
                py += band.syy[size_t(k)];
            }
            const std::complex<double> h = px > 0 ? pxy / px : std::complex<double> {};
            r.freq.push_back(float(f));
            r.magDb.push_back(float(20.0 * std::log10(std::max(std::abs(h), 1e-9))) - c.offsetDb(float(f)));
            r.phaseDeg.push_back(float(std::arg(h) * 180.0 / kPi));
            r.coherence.push_back(px > 0 && py > 0 ? float(std::norm(pxy) / (px * py)) : 0.0f);
        }
    }
    std::lock_guard<std::mutex> l(resultLock);
    latest = std::move(r);
}

} // namespace vt

namespace vt {

// REW-style sweep analysis: regularised deconvolution over the whole capture → impulse
// response → window around the arrival (−10 ms … +500 ms) → transfer function.
void Analyzer::analyseSweep(int64_t len)
{
    int order = 1;
    while ((int64_t(1) << order) < 2 * len) ++order;
    const int M = 1 << order;
    juce::dsp::FFT f(order);
    std::vector<float> X(size_t(2 * M), 0.0f), Y(size_t(2 * M), 0.0f);
    const int64_t start = total - len;
    for (int64_t i = 0; i < len; ++i) {
        X[size_t(i)] = hist(refHist, start + i);
        Y[size_t(i)] = hist(measHist, start + i);
    }
    f.performRealOnlyForwardTransform(X.data());
    f.performRealOnlyForwardTransform(Y.data());
    auto* Xc = reinterpret_cast<cfloat*>(X.data());
    auto* Yc = reinterpret_cast<cfloat*>(Y.data());
    double maxPow = 0;
    for (int k = 0; k <= M / 2; ++k) maxPow = std::max(maxPow, double(std::norm(Xc[k])));
    if (maxPow <= 0) return;
    const double eps = 1e-6 * maxPow; // −60 dB: outside the swept band H → 0
    for (int k = 0; k <= M / 2; ++k) {
        const cfloat h = Yc[k] * std::conj(Xc[k]) / float(std::norm(Xc[k]) + eps);
        Yc[k] = h;
        if (k > 0 && k < M / 2) Yc[M - k] = std::conj(h);
    }
    f.performRealOnlyInverseTransform(Y.data());
    const float* ir = Y.data();

    int peak = 0;
    for (int k = 0; k < M / 2; ++k)
        if (std::abs(ir[k]) > std::abs(ir[peak])) peak = k;
    const double a = std::abs(ir[(peak - 1 + M) % M]), b = std::abs(ir[peak]), c = std::abs(ir[peak + 1]);
    const double den = a - 2 * b + c;
    const double d = peak + (std::abs(den) > 1e-12 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0);

    // Window: 10 ms raised-cosine fade-in before the peak, flat to +450 ms, 50 ms fade-out.
    const int pre = int(0.01 * sr), post = int(0.5 * sr), fadeOut = int(0.05 * sr);
    int wOrder = 1;
    while ((1 << wOrder) < 2 * (pre + post)) ++wOrder;
    const int W = 1 << wOrder;
    juce::dsp::FFT wf(wOrder);
    std::vector<float> w(size_t(2 * W), 0.0f);
    for (int i = -pre; i < post; ++i) {
        double g = 1.0;
        if (i < 0) g = 0.5 - 0.5 * std::cos(M_PI * double(i + pre) / pre);
        else if (i > post - fadeOut) g = 0.5 + 0.5 * std::cos(M_PI * double(i - (post - fadeOut)) / fadeOut);
        // Store with the peak at index 0 (wrap negatives) so the phase is relative to the arrival.
        w[size_t((i + W) % W)] = float(g * ir[(peak + i + M) % M]);
    }
    wf.performRealOnlyForwardTransform(w.data());
    const auto* H = reinterpret_cast<const cfloat*>(w.data());

    Result r;
    r.sampleRate = float(sr);
    r.delaySamples = d;
    r.delayValid = true;
    r.frames = 1;
    const double df = sr / W, frac = d - peak;
    const int sm = smoothing.load();
    const double halfBand = sm > 0 ? 0.5 / sm : 1.0 / 96.0;
    MicCal cal_;
    { std::lock_guard<std::mutex> l(calLock); cal_ = cal; }
    for (int i = 0;; ++i) {
        const double fr = 20.0 * std::pow(2.0, i / 48.0);
        if (fr > 20000.0) break;
        int k0 = int(std::ceil(fr * std::pow(2.0, -halfBand) / df)), k1 = int(std::floor(fr * std::pow(2.0, halfBand) / df));
        if (k1 < k0) k0 = k1 = int(std::lround(fr / df));
        k0 = std::clamp(k0, 1, W / 2); k1 = std::clamp(k1, k0, W / 2);
        std::complex<double> h;
        double p = 0;
        for (int k = k0; k <= k1; ++k) {
            const auto v = std::complex<double>(H[k]) * std::polar(1.0, 2.0 * kPi * (k * df) * frac / sr);
            h += v;
            p += std::norm(v);
        }
        const double n = k1 - k0 + 1;
        // Power average for magnitude (no cancellation), complex average for phase.
        r.freq.push_back(float(fr));
        r.magDb.push_back(float(10.0 * std::log10(std::max(p / n, 1e-18))) - cal_.offsetDb(float(fr)));
        r.phaseDeg.push_back(float(std::arg(h) * 180.0 / kPi));
        r.coherence.push_back(1.0f);
    }
    r.irStart = std::max(0, peak - pre);
    r.ir.assign(size_t(pre + post), 0.0f);
    for (int i = 0; i < pre + post; ++i) r.ir[size_t(i)] = ir[(r.irStart + i) % M];
    {
        std::lock_guard<std::mutex> l(resultLock);
        sweep = std::move(r);
    }
    sweepGen.fetch_add(1);
}

} // namespace vt
