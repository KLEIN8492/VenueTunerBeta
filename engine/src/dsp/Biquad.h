#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <string>
#include <vector>

namespace vt {

// Normalised biquad: y = b0·x + b1·x1 + b2·x2 − a1·y1 − a2·y2 (a0 = 1).
// The TypeScript mirror lives in ui/src/lib/filters.ts — keep them in step.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    static Biquad identity() { return {}; }
    bool isIdentity() const { return b0 == 1 && b1 == 0 && b2 == 0 && a1 == 0 && a2 == 0; }
    bool operator==(const Biquad&) const = default;

    std::complex<double> response(double f, double sr) const
    {
        const auto z1 = std::polar(1.0, -2.0 * M_PI * f / sr), z2 = z1 * z1;
        return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
    }
};

inline std::complex<double> chainResponse(const Biquad* s, int n, double f, double sr)
{
    std::complex<double> h = 1.0;
    for (int i = 0; i < n; ++i) h *= s[i].response(f, sr);
    return h;
}

// Transposed Direct Form II state, double precision (stable for low crossovers at high rates).
struct BiquadState
{
    double s1 = 0, s2 = 0;
    double process(const Biquad& c, double x)
    {
        const double y = c.b0 * x + s1;
        s1 = c.b1 * x - c.a1 * y + s2;
        s2 = c.b2 * x - c.a2 * y;
        return y;
    }
    void reset() { s1 = s2 = 0; }
};

enum class XoType { Off, BW6, BW12, BW18, BW24, BW36, BW48, LR12, LR24, LR48 };

namespace design {

inline double clampF(double f, double sr) { return std::clamp(f, 1.0, 0.49 * sr); }

namespace detail {
inline Biquad norm(double b0, double b1, double b2, double a0, double a1, double a2)
{
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}
struct W { double cs, sn; };
inline W w(double f, double sr) { const double w0 = 2 * M_PI * clampF(f, sr) / sr; return { std::cos(w0), std::sin(w0) }; }
} // namespace detail

// RBJ Audio EQ Cookbook designs.
inline Biquad bell(double f, double q, double gainDb, double sr)
{
    if (gainDb == 0) return Biquad::identity();
    const auto [cs, sn] = detail::w(f, sr);
    const double A = std::pow(10.0, gainDb / 40.0), al = sn / (2 * q);
    return detail::norm(1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A);
}

inline Biquad lowShelf(double f, double q, double gainDb, double sr)
{
    if (gainDb == 0) return Biquad::identity();
    const auto [cs, sn] = detail::w(f, sr);
    const double A = std::pow(10.0, gainDb / 40.0), al = sn / (2 * q), r = 2 * std::sqrt(A) * al;
    return detail::norm(A * ((A + 1) - (A - 1) * cs + r), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - r),
                        (A + 1) + (A - 1) * cs + r, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - r);
}

inline Biquad highShelf(double f, double q, double gainDb, double sr)
{
    if (gainDb == 0) return Biquad::identity();
    const auto [cs, sn] = detail::w(f, sr);
    const double A = std::pow(10.0, gainDb / 40.0), al = sn / (2 * q), r = 2 * std::sqrt(A) * al;
    return detail::norm(A * ((A + 1) + (A - 1) * cs + r), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - r),
                        (A + 1) - (A - 1) * cs + r, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - r);
}

inline Biquad notch(double f, double q, double sr)
{
    const auto [cs, sn] = detail::w(f, sr);
    const double al = sn / (2 * q);
    return detail::norm(1, -2 * cs, 1, 1 + al, -2 * cs, 1 - al);
}

inline Biquad lowpass2(double f, double q, double sr)
{
    const auto [cs, sn] = detail::w(f, sr);
    const double al = sn / (2 * q);
    return detail::norm((1 - cs) / 2, 1 - cs, (1 - cs) / 2, 1 + al, -2 * cs, 1 - al);
}

inline Biquad highpass2(double f, double q, double sr)
{
    const auto [cs, sn] = detail::w(f, sr);
    const double al = sn / (2 * q);
    return detail::norm((1 + cs) / 2, -(1 + cs), (1 + cs) / 2, 1 + al, -2 * cs, 1 - al);
}

inline Biquad allpass2(double f, double q, double sr)
{
    const auto [cs, sn] = detail::w(f, sr);
    const double al = sn / (2 * q);
    return detail::norm(1 - al, -2 * cs, 1 + al, 1 + al, -2 * cs, 1 - al);
}

// First-order sections via the prewarped bilinear transform (K = tan(w0/2)).
inline double k1(double f, double sr) { return std::tan(M_PI * clampF(f, sr) / sr); }
inline Biquad lowpass1(double f, double sr) { const double K = k1(f, sr); return { K / (1 + K), K / (1 + K), 0, (K - 1) / (K + 1), 0 }; }
inline Biquad highpass1(double f, double sr) { const double K = k1(f, sr); return { 1 / (1 + K), -1 / (1 + K), 0, (K - 1) / (K + 1), 0 }; }
inline Biquad allpass1(double f, double sr) { const double K = k1(f, sr), c = (K - 1) / (K + 1); return { c, 1, 0, c, 0 }; }

// Butterworth of order n as cascaded sections, Q_k = 1 / (2 cos θ_k), with pole angles
// θ_k = (2k−1)π/2n for even n and kπ/n for odd n (plus a real pole: first-order section).
inline std::vector<Biquad> butterworth(int n, bool highpass, double f, double sr)
{
    std::vector<Biquad> s;
    if (n % 2) s.push_back(highpass ? highpass1(f, sr) : lowpass1(f, sr));
    for (int k = 1; k <= n / 2; ++k) {
        const double theta = n % 2 ? M_PI * k / n : M_PI * (2 * k - 1) / (2.0 * n);
        const double q = 1.0 / (2.0 * std::cos(theta));
        s.push_back(highpass ? highpass2(f, q, sr) : lowpass2(f, q, sr));
    }
    return s;
}

// Linkwitz-Riley of order 2n = Butterworth n, squared.
inline std::vector<Biquad> linkwitzRiley(int n, bool highpass, double f, double sr)
{
    auto s = butterworth(n, highpass, f, sr);
    auto twice = s;
    twice.insert(twice.end(), s.begin(), s.end());
    // Merge pairs of first-order sections into one biquad to save a slot.
    if (n % 2 && twice.size() >= 2) {
        const Biquad a = twice[0];
        twice.erase(twice.begin() + long(s.size()));
        twice[0] = { a.b0 * a.b0, 2 * a.b0 * a.b1, a.b1 * a.b1, 2 * a.a1, a.a1 * a.a1 };
    }
    return twice;
}

// All-pass by type: "1st" (90° at f, 180° total — the phase of a 6 dB/oct crossover),
// "2nd" (180° at f, Q sets how fast it turns), and crossover-matched "lr24" / "lr48": the exact
// phase of a Linkwitz-Riley 24 / 48 dB/oct crossover sum (half the order, Butterworth poles).
inline std::vector<Biquad> allpassMatched(const std::string& type, double f, double sr, double q = 0.707)
{
    if (type == "1st") return { allpass1(f, sr) };
    if (type == "lr24") return { allpass2(f, 1.0 / std::sqrt(2.0), sr) };
    if (type == "lr48") {
        std::vector<Biquad> s;
        for (int k = 1; k <= 2; ++k) s.push_back(allpass2(f, 1.0 / (2.0 * std::cos(M_PI * (2 * k - 1) / 8.0)), sr));
        return s;
    }
    return { allpass2(f, q, sr) };
}

inline std::vector<Biquad> crossover(XoType t, bool highpass, double f, double sr)
{
    switch (t) {
        case XoType::Off: return {};
        case XoType::BW6: return butterworth(1, highpass, f, sr);
        case XoType::BW12: return butterworth(2, highpass, f, sr);
        case XoType::BW18: return butterworth(3, highpass, f, sr);
        case XoType::BW24: return butterworth(4, highpass, f, sr);
        case XoType::BW36: return butterworth(6, highpass, f, sr);
        case XoType::BW48: return butterworth(8, highpass, f, sr);
        case XoType::LR12: return linkwitzRiley(1, highpass, f, sr);
        case XoType::LR24: return linkwitzRiley(2, highpass, f, sr);
        case XoType::LR48: return linkwitzRiley(4, highpass, f, sr);
    }
    return {};
}

} // namespace design
} // namespace vt
