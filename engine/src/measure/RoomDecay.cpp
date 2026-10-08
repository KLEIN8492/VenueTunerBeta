#include "measure/RoomDecay.h"
#include "dsp/Biquad.h"
#include <cmath>

namespace vt {

namespace {
// Least-squares slope (dB/s) of the curve between two levels; −1 if not reached.
double fitRt(const std::vector<double>& dbCurve, double sr, double from, double to)
{
    size_t i0 = 0, i1 = 0;
    while (i0 < dbCurve.size() && dbCurve[i0] > from) ++i0;
    i1 = i0;
    while (i1 < dbCurve.size() && dbCurve[i1] > to) ++i1;
    if (i1 >= dbCurve.size() || i1 <= i0 + 10) return -1;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const double n = double(i1 - i0);
    for (size_t i = i0; i < i1; ++i) {
        const double x = double(i) / sr, y = dbCurve[i];
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    return slope < 0 ? -60.0 / slope : -1;
}
}

std::vector<DecayBand> roomDecay(const std::vector<float>& ir, double sr)
{
    std::vector<DecayBand> out;
    if (ir.size() < size_t(0.2 * sr)) return out;
    for (double fc = 62.5; fc < 9000; fc *= 2) {
        DecayBand band;
        band.fc = std::round(fc == 62.5 ? 63 : fc);
        auto filt = design::crossover(XoType::BW24, true, fc / std::sqrt(2.0), sr);
        auto lp = design::crossover(XoType::BW24, false, fc * std::sqrt(2.0), sr);
        filt.insert(filt.end(), lp.begin(), lp.end());
        std::vector<BiquadState> st(filt.size());
        std::vector<double> e(ir.size());
        for (size_t i = 0; i < ir.size(); ++i) {
            double y = ir[i];
            for (size_t s = 0; s < filt.size(); ++s) y = st[s].process(filt[s], y);
            e[i] = y * y;
        }
        // Noise floor from the last 10 %; truncate where the 10 ms envelope meets it (+3 dB).
        const size_t tail = e.size() / 10;
        double noise = 0;
        for (size_t i = e.size() - tail; i < e.size(); ++i) noise += e[i];
        noise /= double(tail);
        const size_t win = size_t(0.01 * sr);
        size_t cut = e.size() - tail;
        for (size_t i = 0; i + win < e.size() - tail; i += win) {
            double m = 0;
            for (size_t k = i; k < i + win; ++k) m += e[k];
            if (m / double(win) > 2.0 * noise) cut = i + win;
        }
        // Schroeder integral of the noise-compensated energy up to the truncation point, plus
        // (Lundeby) the energy the decay would still have had after the cut, assuming it keeps
        // decaying at the rate found in a first pass. Without it the curve bends down at the end.
        auto schroeder = [&](double tailEnergy) {
            std::vector<double> dbc(cut);
            std::vector<double> sch(cut, 0.0);
            double acc = tailEnergy;
            for (size_t i = cut; i-- > 0;) { acc += std::max(0.0, e[i] - noise); sch[i] = acc; }
            for (size_t i = 0; i < cut; ++i) dbc[i] = 10.0 * std::log10(std::max(sch[i], 1e-30) / std::max(sch[0], 1e-30));
            return dbc;
        };
        auto dbCurve = schroeder(0.0);
        if (cut < win * 2 || !(dbCurve.size() > 1)) { out.push_back(band); continue; }
        const double rtPre = std::max(fitRt(dbCurve, sr, -5.0, -25.0), fitRt(dbCurve, sr, 0.0, -10.0));
        if (rtPre > 0) {
            double envAtCut = 0;
            for (size_t k = cut - win; k < cut; ++k) envAtCut += std::max(0.0, e[k] - noise);
            envAtCut /= double(win);
            dbCurve = schroeder(envAtCut * rtPre * sr / 13.8155); // ∫ e·exp(−13.8 t/RT) dt
        }
        band.edt = fitRt(dbCurve, sr, 0.0, -10.0);
        band.t20 = fitRt(dbCurve, sr, -5.0, -25.0);
        band.t30 = fitRt(dbCurve, sr, -5.0, -35.0);
        for (size_t i = 0; i < cut; i += win) band.curveDb.push_back(float(dbCurve[i]));
        out.push_back(band);
    }
    return out;
}

} // namespace vt
