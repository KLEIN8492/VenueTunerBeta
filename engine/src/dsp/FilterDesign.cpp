#include "dsp/FilterDesign.h"
#include "core/SystemState.h"

namespace vt {

XoType parseXoType(const juce::String& t)
{
    static const std::pair<const char*, XoType> table[] = {
        { "bw6", XoType::BW6 }, { "bw12", XoType::BW12 }, { "bw18", XoType::BW18 }, { "bw24", XoType::BW24 },
        { "bw36", XoType::BW36 }, { "bw48", XoType::BW48 }, { "lr12", XoType::LR12 }, { "lr24", XoType::LR24 },
        { "lr48", XoType::LR48 },
    };
    for (auto& [name, type] : table)
        if (t == name) return type;
    return XoType::Off;
}

void designOutputIir(const OutputConfig& o, double sr, Biquad* slots)
{
    for (int i = 0; i < kMaxIir; ++i) slots[i] = Biquad::identity();

    for (size_t i = 0; i < o.peq.size() && i < size_t(kMaxPeq); ++i) {
        const auto& b = o.peq[i];
        if (!b.on) continue;
        Biquad& s = slots[kIirPeq + int(i)];
        if (b.type == "bell") s = design::bell(b.freq, b.q, b.gainDb, sr);
        else if (b.type == "lowshelf") s = design::lowShelf(b.freq, b.q, b.gainDb, sr);
        else if (b.type == "highshelf") s = design::highShelf(b.freq, b.q, b.gainDb, sr);
        else if (b.type == "notch") s = design::notch(b.freq, b.q, sr);
        else if (b.type == "lowpass") s = design::lowpass2(b.freq, b.q, sr);
        else if (b.type == "highpass") s = design::highpass2(b.freq, b.q, sr);
    }
    auto hp = design::crossover(parseXoType(o.hp.type), true, o.hp.freq, sr);
    auto lp = design::crossover(parseXoType(o.lp.type), false, o.lp.freq, sr);
    for (size_t i = 0; i < hp.size() && i < 4; ++i) slots[kIirHp + int(i)] = hp[i];
    for (size_t i = 0; i < lp.size() && i < 4; ++i) slots[kIirLp + int(i)] = lp[i];
    for (size_t i = 0; i < o.ap.size() && i < size_t(kMaxAp); ++i) {
        const auto& a = o.ap[i];
        if (a.on) slots[kIirAp + int(i)] = a.order == 1 ? design::allpass1(a.freq, sr) : design::allpass2(a.freq, a.q, sr);
    }
    if (o.airGainDb != 0.0f) slots[kIirAir] = design::highShelf(o.airFreq, 0.707, o.airGainDb, sr);
}

} // namespace vt
