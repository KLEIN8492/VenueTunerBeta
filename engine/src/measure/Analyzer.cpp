#include "measure/Analyzer.h"
#include <cmath>

namespace vt {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int N = 1 << Analyzer::kFftOrder;
constexpr int L = 1 << Analyzer::kDelayOrder;
using cfloat = std::complex<float>;
}

Analyzer::Analyzer()
{
    refHist.assign(kHistory, 0.0f);
    measHist.assign(kHistory, 0.0f);
    scratch.assign(1 << 18, 0.0f);
    window.resize(N);
    for (int i = 0; i < N; ++i) window[size_t(i)] = float(0.5 - 0.5 * std::cos(2.0 * kPi * i / N));
    bufX.assign(2 * N, 0.0f);
    bufY.assign(2 * N, 0.0f);
    bigX.assign(4 * L, 0.0f);
    bigY.assign(4 * L, 0.0f);
    sxx.assign(N / 2 + 1, 0.0);
    syy.assign(N / 2 + 1, 0.0);
    sxy.assign(N / 2 + 1, {});
}

void Analyzer::prepare(double sampleRate)
{
    sr = sampleRate;
    refFifo.clear();
    measFifo.clear();
    doReset();
}

void Analyzer::doReset()
{
    std::fill(refHist.begin(), refHist.end(), 0.0f);
    std::fill(measHist.begin(), measHist.end(), 0.0f);
    total = 0;
    nextFrameEnd = N;
    nextDelayAt = L;
    frameCount = 0;
    std::fill(sxx.begin(), sxx.end(), 0.0);
    std::fill(syy.begin(), syy.end(), 0.0);
    std::fill(sxy.begin(), sxy.end(), std::complex<double> {});
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
    if (resetRequested.exchange(false)) doReset();

    // Consume ref and meas in lock-step (the audio thread always pushes both).
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
    while (nextFrameEnd <= total) {
        if (delayValid) addFrame(nextFrameEnd);
        nextFrameEnd += N / 2; // 50 % overlap
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
    if (pk < 10.0 * rms) { delayValid = false; return; } // no clear arrival: keep waiting

    const double a = std::abs(ir[(peak - 1 + M) % M]), b = pk, c = std::abs(ir[peak + 1]);
    const double denom = a - 2 * b + c;
    const double frac = std::abs(denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0;
    const double d = peak + std::clamp(frac, -0.5, 0.5);

    if (!delayValid || std::abs(d - double(alignInt)) > 1.0) {
        // Alignment moved: restart averaging so frames with different alignments never mix.
        alignInt = int(std::lround(d));
        frameCount = 0;
        std::fill(sxx.begin(), sxx.end(), 0.0);
        std::fill(syy.begin(), syy.end(), 0.0);
        std::fill(sxy.begin(), sxy.end(), std::complex<double> {});
        nextFrameEnd = std::max<int64_t>(N + peak + 1, total);
    }
    delayTotal.store(d);
    delayValid = true;

    irStart = std::max(0, peak - 2048);
    const int irLen = std::min(8192, L - irStart);
    irExcerpt.assign(ir + irStart, ir + irStart + irLen);
}

void Analyzer::addFrame(int64_t frameEnd)
{
    const int dInt = alignInt;
    const int64_t mStart = frameEnd - N, rStart = mStart - dInt;
    if (rStart < 0 || rStart < total - kHistory) return;

    for (int i = 0; i < N; ++i) {
        bufX[size_t(i)] = hist(refHist, rStart + i) * window[size_t(i)];
        bufY[size_t(i)] = hist(measHist, mStart + i) * window[size_t(i)];
    }
    std::fill(bufX.begin() + N, bufX.end(), 0.0f);
    std::fill(bufY.begin() + N, bufY.end(), 0.0f);
    fft.performRealOnlyForwardTransform(bufX.data(), true);
    fft.performRealOnlyForwardTransform(bufY.data(), true);
    const auto* X = reinterpret_cast<const cfloat*>(bufX.data());
    const auto* Y = reinterpret_cast<const cfloat*>(bufY.data());

    ++frameCount;
    const double alpha = 1.0 / std::min(frameCount, averaging.load());
    for (int k = 0; k <= N / 2; ++k) {
        const std::complex<double> x(X[k]), y(Y[k]);
        sxx[size_t(k)] += alpha * (std::norm(x) - sxx[size_t(k)]);
        syy[size_t(k)] += alpha * (std::norm(y) - syy[size_t(k)]);
        sxy[size_t(k)] += alpha * (y * std::conj(x) - sxy[size_t(k)]);
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
    r.frames = frameCount;

    if (frameCount > 0) {
        const double df = sr / N;
        const double frac = r.delaySamples - double(alignInt); // sub-sample remainder
        const int sm = smoothing.load();
        const double halfBand = sm > 0 ? 0.5 / sm : 1.0 / 48.0;
        MicCal c;
        { std::lock_guard<std::mutex> l(calLock); c = cal; }

        for (int i = 0;; ++i) {
            const double f = 20.0 * std::pow(2.0, i / 24.0);
            if (f > 20000.0) break;
            int k0 = int(std::ceil(f * std::pow(2.0, -halfBand) / df));
            int k1 = int(std::floor(f * std::pow(2.0, halfBand) / df));
            if (sm == 0 || k1 < k0) k0 = k1 = int(std::lround(f / df));
            k0 = std::clamp(k0, 1, N / 2); k1 = std::clamp(k1, k0, N / 2);

            double px = 0, py = 0;
            std::complex<double> pxy;
            for (int k = k0; k <= k1; ++k) {
                // Remove the fractional part of the delay so phase is flat for a pure delay.
                const double w = 2.0 * kPi * (k * df) * frac / sr;
                pxy += sxy[size_t(k)] * std::polar(1.0, w);
                px += sxx[size_t(k)];
                py += syy[size_t(k)];
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
