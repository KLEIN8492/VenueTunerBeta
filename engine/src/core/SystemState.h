#pragma once
#include "core/Params.h"
#include <juce_core/juce_core.h>
#include <map>
#include <vector>

namespace vt {

struct DeviceConfig
{
    juce::String inputDevice, outputDevice; // empty = system default
    double sampleRate = 48000.0;
    int bufferSize = 64;
};

struct InputConfig
{
    juce::String name;
    juce::String role = "console"; // console | mic | loopback | none
};

struct OutputConfig
{
    juce::String name, type = "full", colour = "#7fb2ff";
    float gainDb = 0.0f;
    bool polarityInvert = false;
    float delayMs = 0.0f;
    bool mute = false;
    bool limiterOn = true;
    float limThreshDb = -1.0f, limAttackMs = 0.0f, limReleaseMs = 100.0f;
    std::vector<float> mix; // linear gain per input
};

struct GeneratorConfig
{
    bool on = false;
    juce::String signal = "pink";
    float levelDb = -40.0f; // dBFS RMS
    std::vector<int> targets;
};

struct MeasureConfig
{
    int micInput = -1;
    juce::String ref = "generator"; // generator | input
    int refInput = -1;
    int averaging = 16;
    int smoothing = 24; // 1/N octave, 0 = none
};

struct CalibrationData
{
    std::map<juce::String, double> ioLatency; // "outDev|inDev|sr|buffer" -> samples
    juce::String micCalPath;
};

struct SystemState
{
    DeviceConfig device;
    std::vector<InputConfig> inputs;
    std::vector<OutputConfig> outputs;
    GeneratorConfig gen;
    MeasureConfig measure;
    CalibrationData cal;

    // Match the channel counts of the open device, keeping existing settings.
    void resizeIO(int numIn, int numOut);
};

constexpr float kGeneratorMaxDb = -12.0f; // dBFS RMS hard cap (pink crest ~12 dB)

juce::var toVar(const SystemState&);
juce::var outputsToVar(const std::vector<OutputConfig>&);
// All-or-nothing: `out` is only written when the whole document is valid.
bool fromVar(const juce::var&, SystemState& out, juce::String& error);
bool outputsFromVar(const juce::var&, std::vector<OutputConfig>& out, juce::String& error);
// Path like "outputs/2/gainDb". Unknown paths, wrong types, out-of-range values fail
// and leave the state unchanged.
bool applyPatch(SystemState&, const juce::String& path, const juce::var& value, juce::String& error);
Params toParams(const SystemState&);

} // namespace vt
