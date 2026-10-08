#pragma once
#include <juce_core/juce_core.h>
#include <vector>

namespace vt {

// Measurement-mic calibration file (REW / miniDSP / Dayton style: "freq dB [phase]").
// offsetDb(f) is the mic's own deviation; the analyzer subtracts it.
class MicCal
{
public:
    bool load(const juce::File& file, juce::String& error);
    bool parse(const juce::String& text, juce::String& error);
    void clear() { freqs.clear(); dbs.clear(); }
    bool isLoaded() const { return !freqs.empty(); }
    float offsetDb(float hz) const;

private:
    std::vector<float> freqs, dbs;
};

} // namespace vt
