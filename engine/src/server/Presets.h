#pragma once
#include "core/SystemState.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace vt {

// A speaker preset: the processing that belongs to the loudspeaker (crossover, PEQ, all-pass,
// limiter, polarity, level trim, FIR), not to the venue (delay, routing, channel, mute).
// Stored as JSON under <data>/presets with the FIR coefficients embedded, so a preset file
// is self-contained and can be shared.
struct SpeakerPreset
{
    juce::String name;
    OutputConfig processing;      // only the speaker fields are used
    std::vector<float> fir;       // embedded coefficients (empty = none)
    double firSampleRate = 0;
};

class Presets
{
public:
    explicit Presets(juce::File dataDir);
    juce::StringArray list() const;
    bool save(const SpeakerPreset&, juce::String& error);
    bool load(const juce::String& name, SpeakerPreset& out, juce::String& error) const;
    bool remove(const juce::String& name);
    juce::String exportJson(const juce::String& name, juce::String& error) const;
    bool importJson(const juce::String& name, const juce::String& json, juce::String& error);
    // Writes the embedded FIR as a WAV (keeps its sample rate) and returns its path.
    juce::String materialiseFir(const SpeakerPreset&, juce::String& error) const;

    static void applySpeakerFields(const OutputConfig& from, OutputConfig& to);

private:
    juce::File fileFor(const juce::String& name) const;
    static juce::var toVar(const SpeakerPreset&);
    static bool fromVar(const juce::var&, SpeakerPreset&, juce::String& error);
    juce::File dir;
};

} // namespace vt
