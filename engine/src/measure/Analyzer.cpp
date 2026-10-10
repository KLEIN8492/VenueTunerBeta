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
    delayCount = 0;
    alignInt = 0;
    delayTotal.store(0.0);
    irExcerpt.clear();
    if (lockOn.load()) lockPending.store(true); // re-apply the user's delay after a reset
    std::lock_guard<std::mutex> l(resultLock);
    latest = Result {};
    latest.sampleRate = float(sr);
}

bool Analyzer::process()
{
    struct ApplyLock { Analyzer& a; ~ApplyLock() { if (a.lockOn.load() && a.lockPending.exchange(false)) a.applyLock(); } } applyLockOnExit { *this };
    if (resetRequested.exchange(false)) {
        if (const double p = pendingSr.exchange(0.0); p > 0.0) sr = p;
        refFifo.clear();
        measFifo.clear();
        overflow.store(false);
        doReset();
    }
    // A dropped block (analysis didn't keep up) would otherwise leave a hole in one
    // history ring. Throw both FIFOs away and restart averaging from a clean lock-step.
    if (overflow.exchange(false)) {
        refFifo.clear();
        measFifo.clear();
        doReset();
        return false;
    }

    // Consume ref and meas in lock-step (the audio thread always pushes both).
    if (const int64_t len = sweepRequest.exchange(0); len > 0)
        analyseSweep(std::min<int64_t>(len, std::min<int64_t>(total, kHistory)), sweepLength.load());

    const size_t n = std::min(refFifo.available(), measFifo.available());
    if (n == 0) return false;
    size_t done = 0;
    while (done < n) {
        const size_t chunk = std::min(n - done, scratch.size());
        const double a = 1.0 - std::exp(-double(chunk) / (0.3 * sr)); // ~300 ms level average
        auto meanSquare = [&] { double e = 0; for (size_t i = 0; i < chunk; ++i) e += double(scratch[i]) * scratch[i]; return e / double(chunk); };
        refFifo.pop(scratch.data(), chunk);
        for (size_t i = 0; i < chunk; ++i) refHist[size_t(total + int64_t(i)) & (kHistory - 1)] = scratch[i];
        refMs.store(refMs.load() + a * (meanSquare() - refMs.load()));
        measFifo.pop(scratch.data(), chunk);
        for (size_t i = 0; i < chunk; ++i) measHist[size_t(total + int64_t(i)) & (kHistory - 1)] = scratch[i];
        measMs.store(measMs.load() + a * (meanSquare() - measMs.load()));
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
    const double strongest = std::abs(ir[peak]);
    if (!(strongest > 0.0) || strongest < 10.0 * rms) { // silence or no clear arrival
        if (!lockOn.load()) delayValid = false;            // a locked delay survives a pause in the noise
        return;
    }

    // Two arrivals of nearly the same level (L+R heard off-centre, a strong reflection): the
    // strongest one can swap from one estimate to the next, and every swap would restart the
    // averaging. Stay on the arrival already locked while it is within 2 dB of the strongest.
    if (delayValid && std::abs(peak - alignInt) > 1) {
        int held = -1;
        for (int k = std::max(0, alignInt - 2); k <= std::min(L - 1, alignInt + 2); ++k)
            if (held < 0 || std::abs(ir[k]) > std::abs(ir[held])) held = k;
        if (held >= 0 && std::abs(ir[held]) >= 0.8 * strongest) peak = held;
    }
    const double pk = std::abs(ir[peak]);

    const double a = std::abs(ir[(peak - 1 + M) % M]), b = pk, c = std::abs(ir[peak + 1]);
    const double denom = a - 2 * b + c;
    const double frac = std::abs(denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0;
    const double d = peak + std::clamp(frac, -0.5, 0.5);
    autoDelay.store(d);

    if (lockOn.load()) { // the user's delay rules: keep the alignment, only refresh the IR view
        ++delayCount;
        irStart = std::max(0, peak - 2048);
        const int lockedLen = std::min(8192, L - irStart);
        irExcerpt.assign(ir + irStart, ir + irStart + lockedLen);
        return;
    }

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
    ++delayCount;

    irStart = std::max(0, peak - 2048);
    const int irLen = std::min(8192, L - irStart);
    irExcerpt.assign(ir + irStart, ir + irStart + irLen);
}

void Analyzer::applyLock()
{
    const double d = lockSamples.load();
    if (!delayValid || std::abs(d - double(alignInt)) > 0.5) {
        // Beyond half a sample the frames themselves must move: realign and restart averaging.
        alignInt = int(std::lround(d));
        for (auto& band : bands) {
            band.clear();
            band.nextFrameEnd = std::max<int64_t>(band.n + alignInt + 1, total);
        }
    }
    delayTotal.store(d); // the sub-sample remainder is applied to the phase in buildResult()
    delayValid = true;
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
    int effective = minFrames;
    const int avg = averaging.load();
    for (auto& band : bands) {
        minFrames = std::min(minFrames, band.frameCount);
        effective = std::min(effective, std::min(band.frameCount, std::min(1024, avg * band.avgScale)));
    }
    r.frames = minFrames;
    r.effectiveFrames = effective;
    r.delayEstimates = delayCount;
    r.autoDelaySamples = autoDelay.load();
    r.delayLocked = lockOn.load();

    if (minFrames > 0) {
        const double frac = r.delaySamples - double(alignInt); // sub-sample remainder
        const int sm = smoothing.load(), psm = phaseSmoothing.load();
        const double halfBand = sm > 0 ? 0.5 / sm : 1.0 / 96.0;
        const double phaseHalf = psm > 0 ? 0.5 / psm : 1.0 / 96.0;
        MicCal c;
        { std::lock_guard<std::mutex> l(calLock); c = cal; }

        for (int i = 0;; ++i) {
            const double f = 20.0 * std::pow(2.0, i / double(kPointsPerOctave));
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

            // Magnitude and coherence are averaged per bin over the smoothing band (weighted by
            // the reference power), phase as a vector. A vector average of the magnitude would
            // act as a hidden time window around the IR peak: an HF driver arriving 1 ms off the
            // peak would read low, and the same speaker would look different in a sweep.
            double px = 0, h2 = 0, coh = 0;
            std::complex<double> pxy;
            for (int k = k0; k <= k1; ++k) {
                // Remove the fractional part of the delay so phase is flat for a pure delay.
                const double w = 2.0 * kPi * (k * df) * frac / sr;
                const auto sxy = band.sxy[size_t(k)] * std::polar(1.0, w);
                const double sxx = band.sxx[size_t(k)], syy = band.syy[size_t(k)];
                pxy += sxy;
                px += sxx;
                if (sxx > 0) h2 += std::norm(sxy) / sxx;  // = Sxx·|H|²
                if (syy > 0) coh += std::norm(sxy) / syy; // = Sxx·γ²
            }
            // Phase over its own (usually narrower) band.
            int p0 = int(std::ceil(f * std::pow(2.0, -phaseHalf) / df));
            int p1 = int(std::floor(f * std::pow(2.0, phaseHalf) / df));
            if (psm == 0 || p1 < p0) p0 = p1 = int(std::lround(f / df));
            p0 = std::clamp(p0, 1, N / 2); p1 = std::clamp(p1, p0, N / 2);
            std::complex<double> pph;
            for (int k = p0; k <= p1; ++k) pph += band.sxy[size_t(k)] * std::polar(1.0, 2.0 * kPi * (k * df) * frac / sr);
            r.freq.push_back(float(f));
            r.magDb.push_back(float(10.0 * std::log10(std::max(px > 0 ? h2 / px : 0.0, 1e-18))) - c.offsetDb(float(f)));
            r.phaseDeg.push_back(float(std::arg(pph) * 180.0 / kPi));
            r.coherence.push_back(px > 0 ? float(std::clamp(coh / px, 0.0, 1.0)) : 0.0f);
        }
    }
    std::lock_guard<std::mutex> l(resultLock);
    latest = std::move(r);
}

} // namespace vt

namespace vt {

// Sweep analysis: regularised deconvolution over the whole capture → impulse response →
// frequency-dependent window around the arrival → transfer function on the same frequency
// points, with the same time windows, as the live (multi-time-window) measurement — so a sweep
// and pink noise of the same speaker read the same, and room reflections are excluded in the
// highs just as they are live. "Coherence" comes from the signal-to-noise ratio: the windowed
// arrival against an equal window of the late IR (noise floor, or late reverb in a big room).
void Analyzer::analyseSweep(int64_t len, int64_t sweepSamples)
{
    // Linear deconvolution needs ≥ 2·len points. The history is 2^20 samples, so the FFT never
    // exceeds 2^21: the whole capture is always used and the start of the sweep (the lows) is
    // never dropped, at any sample rate.
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

    // IR lags fully covered by the capture: up to the end of the tail recorded after the sweep.
    const int64_t covered = sweepSamples > 0 ? std::max<int64_t>(0, len - sweepSamples) : len / 2;
    const int noiseEnd = int(std::min<int64_t>(peak + int64_t(2.9 * sr), std::min<int64_t>(covered - int64_t(0.05 * sr), M / 2)));

    // Time windows (after the arrival), matching the live multi-time-window bands.
    struct Win { double fHi, postSec; };
    std::vector<Win> wins;
    if (multiWindow.load()) wins = { { 200, 0.5 }, { 800, 0.17 }, { 3200, 0.043 }, { 1e9, 0.011 } };
    else wins = { { 1e9, 0.5 } };

    struct Spectrum { int W = 0; std::vector<float> sig, noise; bool hasNoise = false; };
    auto windowed = [&](int pre, int post, int at, int Wsz) {
        std::vector<float> w(size_t(2 * Wsz), 0.0f);
        const int fadeOut = std::max(1, post / 4);
        for (int i = -pre; i < post; ++i) {
            double g = 1.0;
            if (i < 0) g = 0.5 - 0.5 * std::cos(kPi * double(i + pre) / pre);
            else if (i > post - fadeOut) g = 0.5 + 0.5 * std::cos(kPi * double(i - (post - fadeOut)) / fadeOut);
            // Peak at index 0 (negatives wrapped) so the phase is relative to the arrival.
            w[size_t((i + Wsz) % Wsz)] = float(g * ir[(at + i + M) % M]);
        }
        return w;
    };
    std::vector<Spectrum> spec(wins.size());
    for (size_t b = 0; b < wins.size(); ++b) {
        const int post = std::max(16, int(wins[b].postSec * sr));
        const int pre = std::max(8, std::min(int(0.01 * sr), post / 4));
        int wOrder = 1;
        while ((1 << wOrder) < 2 * (pre + post)) ++wOrder;
        auto& s = spec[b];
        s.W = 1 << wOrder;
        juce::dsp::FFT wf(wOrder);
        s.sig = windowed(pre, post, peak, s.W);
        wf.performRealOnlyForwardTransform(s.sig.data());
        // Noise window: same shape, late in the IR, clear of this window.
        const int noiseAt = noiseEnd - post;
        if (noiseAt - pre > peak + post) {
            s.noise = windowed(pre, post, noiseAt, s.W);
            wf.performRealOnlyForwardTransform(s.noise.data());
            s.hasNoise = true;
        }
    }

    Result r;
    r.sampleRate = float(sr);
    r.delaySamples = d;
    r.delayValid = true;
    r.frames = r.effectiveFrames = 1;
    const double frac = d - peak;
    const int sm = smoothing.load();
    const double halfBand = sm > 0 ? 0.5 / sm : 1.0 / 48.0;
    MicCal cal_;
    { std::lock_guard<std::mutex> l(calLock); cal_ = cal; }
    for (int i = 0;; ++i) {
        const double fr = 20.0 * std::pow(2.0, i / double(kPointsPerOctave)); // same points as the live measurement
        if (fr > 20000.0) break;
        size_t b = 0;
        while (b + 1 < wins.size() && fr >= wins[b].fHi) ++b;
        const auto& s = spec[b];
        const auto* H = reinterpret_cast<const cfloat*>(s.sig.data());
        const auto* N = reinterpret_cast<const cfloat*>(s.noise.data());
        const double df = sr / s.W;
        int k0 = int(std::ceil(fr * std::pow(2.0, -halfBand) / df)), k1 = int(std::floor(fr * std::pow(2.0, halfBand) / df));
        if (k1 < k0) k0 = k1 = int(std::lround(fr / df));
        k0 = std::clamp(k0, 1, s.W / 2); k1 = std::clamp(k1, k0, s.W / 2);
        std::complex<double> h;
        double p = 0, pn = 0;
        for (int k = k0; k <= k1; ++k) {
            const auto v = std::complex<double>(H[k]) * std::polar(1.0, 2.0 * kPi * (k * df) * frac / sr);
            h += v;
            p += std::norm(v);
            if (s.hasNoise) pn += std::norm(std::complex<double>(N[k]));
        }
        const double n = k1 - k0 + 1;
        // Power average for magnitude (no cancellation), complex average for phase.
        r.freq.push_back(float(fr));
        r.magDb.push_back(float(10.0 * std::log10(std::max(p / n, 1e-18))) - cal_.offsetDb(float(fr)));
        r.phaseDeg.push_back(float(std::arg(h) * 180.0 / kPi));
        // γ² = SNR / (1 + SNR) = 1 − noise / (signal + noise), as for an additive-noise channel.
        r.coherence.push_back(s.hasNoise && p > 0 ? float(std::clamp(1.0 - pn / p, 0.0, 1.0)) : 1.0f);
    }
    // Room decay from the direct sound onwards (up to 3 s or what was captured).
    {
        const int tailLen = int(std::min<int64_t>(int64_t(3.0 * sr), std::min<int64_t>(M / 2 - peak, std::max<int64_t>(0, covered - peak))));
        std::vector<float> tail(size_t(std::max(0, tailLen)));
        for (int i = 0; i < tailLen; ++i) tail[size_t(i)] = ir[(peak + i) % M];
        r.decay = roomDecay(tail, sr);
    }
    const int pre = int(0.01 * sr), post = int(0.5 * sr);
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
