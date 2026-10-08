#include "measure/MicCal.h"
#include <algorithm>
#include <cmath>

namespace vt {

bool MicCal::load(const juce::File& file, juce::String& error)
{
    if (!file.existsAsFile()) { error = "File not found: " + file.getFullPathName(); return false; }
    return parse(file.loadFileAsString(), error);
}

bool MicCal::parse(const juce::String& text, juce::String& error)
{
    std::vector<std::pair<float, float>> pts;
    for (auto line : juce::StringArray::fromLines(text)) {
        line = line.trim();
        if (line.isEmpty() || !(juce::CharacterFunctions::isDigit(line[0]) || line[0] == '.'))
            continue; // header, comment or quoted sensitivity line
        auto tok = juce::StringArray::fromTokens(line, " \t,;", "\"");
        tok.removeEmptyStrings();
        if (tok.size() < 2) continue;
        const float f = tok[0].getFloatValue(), db = tok[1].getFloatValue();
        if (f > 0.0f && std::isfinite(db)) pts.emplace_back(f, db);
    }
    if (pts.size() < 2) { error = "No frequency/dB data found in calibration file"; return false; }
    std::sort(pts.begin(), pts.end());
    freqs.clear(); dbs.clear();
    for (auto& [f, db] : pts) { freqs.push_back(f); dbs.push_back(db); }
    return true;
}

float MicCal::offsetDb(float hz) const
{
    if (freqs.empty()) return 0.0f;
    if (hz <= freqs.front()) return dbs.front();
    if (hz >= freqs.back()) return dbs.back();
    const auto it = std::upper_bound(freqs.begin(), freqs.end(), hz);
    const size_t i = size_t(it - freqs.begin());
    const float t = std::log(hz / freqs[i - 1]) / std::log(freqs[i] / freqs[i - 1]);
    return dbs[i - 1] + t * (dbs[i] - dbs[i - 1]);
}

} // namespace vt
