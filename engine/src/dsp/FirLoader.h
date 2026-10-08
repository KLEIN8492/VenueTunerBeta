#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

namespace vt {

// Loads FIR coefficients from WAV/AIFF (first channel) or text (one coefficient per
// line; lines starting with '*', '#', ';' or letters are ignored — rePhase / FIR Designer
// style). The file's own sample rate is returned; JUCE's Convolution resamples if needed.
struct FirLoader
{
    static constexpr int kMaxTaps = 65536;
    static bool load(const juce::File&, double engineSr, juce::AudioBuffer<float>& out, double& fileSr, juce::String& error);
    static int peakIndex(const juce::AudioBuffer<float>&);
private:
    static bool loadRaw(const juce::File&, double engineSr, juce::AudioBuffer<float>& out, double& fileSr, juce::String& error);
};

} // namespace vt
