#pragma once
#include "dsp/Biquad.h"
#include <algorithm>
#include <cmath>

namespace vt::air {

struct Conditions { double tempC = 20, humidity = 50, pressureKPa = 101.325; };

// Atmospheric absorption coefficient in dB per metre (ISO 9613-1).
inline double alphaDbPerM(double f, double tempC, double rhPercent, double pressureKPa = 101.325)
{
    constexpr double pr = 101.325, T0 = 293.15, T01 = 273.16;
    const double T = tempC + 273.15, pa = pressureKPa;
    const double psatRatio = std::pow(10.0, -6.8346 * std::pow(T01 / T, 1.261) + 4.6151);
    const double h = rhPercent * psatRatio * (pr / pa); // molar concentration of water vapour (%)
    const double frO = (pa / pr) * (24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h));
    const double frN = (pa / pr) * std::pow(T / T0, -0.5) * (9.0 + 280.0 * h * std::exp(-4.170 * (std::pow(T / T0, -1.0 / 3.0) - 1.0)));
    const double f2 = f * f;
    return 8.686 * f2 * (1.84e-11 * (pr / pa) * std::sqrt(T / T0)
                         + std::pow(T / T0, -2.5) * (0.01275 * std::exp(-2239.1 / T) / (frO + f2 / frO)
                                                     + 0.1068 * std::exp(-3352.0 / T) / (frN + f2 / frN)));
}

struct Shelf { double freq = 8000, gainDb = 0; };

// High shelf that restores the HF the air now absorbs compared with the tuning reference,
// over `distanceM`. Least-squares fit over 1–16 kHz; gain limited to −6…+9 dB.
inline Shelf compensation(double distanceM, Conditions now, Conditions ref, double sr)
{
    if (distanceM <= 0) return {};
    auto target = [&](double f) {
        return distanceM * (alphaDbPerM(f, now.tempC, now.humidity, now.pressureKPa) - alphaDbPerM(f, ref.tempC, ref.humidity, ref.pressureKPa));
    };
    double peak = 0;
    for (double f = 1000; f <= 16000; f *= 1.2) peak = std::max(peak, std::abs(target(f)));
    if (peak < 0.2) return {}; // below anything audible: leave the slot empty
    Shelf best;
    double bestErr = 1e300;
    for (double fc = 1500; fc <= 14000; fc *= std::pow(2.0, 1.0 / 12.0))
        for (double g = -6.0; g <= 9.0; g += 0.1) {
            const auto b = design::highShelf(fc, 0.707, g, sr);
            double e = 0;
            for (double f = 1000; f <= 16000; f *= 1.12) {
                const double d = 20.0 * std::log10(std::abs(b.response(f, sr))) - target(f);
                e += d * d;
            }
            if (e < bestErr) { bestErr = e; best = { fc, std::round(g * 10.0) / 10.0 }; }
        }
    return best;
}

} // namespace vt::air
