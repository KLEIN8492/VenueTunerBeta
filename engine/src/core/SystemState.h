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

// A named signal coming in on a physical input channel.
struct SourceConfig
{
    juce::String name;
    juce::String type = "program"; // program (may feed outputs) | mic | loopback (measurement only)
    int channel = -1;              // physical input, -1 = unpatched
};

struct PeqBand
{
    bool on = true;
    juce::String type = "bell"; // bell | lowshelf | highshelf | notch | lowpass | highpass
    float freq = 1000.0f, gainDb = 0.0f, q = 1.0f;
};

struct XoConfig
{
    juce::String type = "off"; // off | bw6..bw48 | lr12 | lr24 | lr48
    float freq = 100.0f;
};

struct ApBand
{
    bool on = true;
    int order = 2;
    float freq = 1000.0f, q = 0.7f;
};

struct FirConfig
{
    bool on = false;
    juce::String path;
    int taps = 0, latencySamples = 0; // filled in by the engine when loaded
};

constexpr int kMaxPeq = 16, kMaxAp = 4, kMaxSources = 64, kMaxOutputs = 64;

// A user-defined processing channel, patched to one physical output (or none).
struct OutputConfig
{
    juce::String name, type = "full", colour = "#7fb2ff";
    int id = 0;       // stable identity (processing slot follows it); assigned when 0
    juce::String preset; // speaker preset last applied (display only)
    int channel = -1; // physical output, -1 = unassigned (processed, silent)
    float gainDb = 0.0f;
    bool polarityInvert = false;
    float delayMs = 0.0f;
    bool mute = false;
    bool limiterOn = true;
    float limThreshDb = -1.0f, limAttackMs = 0.0f, limReleaseMs = 100.0f;
    std::vector<float> mix; // linear gain per source (program sources only take effect)
    std::vector<PeqBand> peq;
    XoConfig hp, lp;
    std::vector<ApBand> ap;
    FirConfig fir;
};

struct GeneratorConfig
{
    bool on = false;
    juce::String signal = "pink";
    float levelDb = -40.0f; // dBFS RMS
    std::vector<int> targets;
};

// Outputs played together during a sequenced measurement (e.g. "Subs" = outputs 3 and 4).
struct MeasureGroup
{
    juce::String name;
    std::vector<int> outputs; // indices into outputs
};

struct MeasureConfig
{
    std::vector<juce::String> positions; // mic positions ("FOH", "Front"...)
    std::vector<MeasureGroup> groups;
    int micSource = -1;             // index into sources
    juce::String ref = "generator"; // generator (internal loopback) | source
    int refSource = -1;
    int averaging = 16;
    int smoothing = 24; // 1/N octave, 0 = none
    bool multiWindow = true;
};

struct CalibrationData
{
    std::map<juce::String, double> ioLatency; // "outDev|inDev|sr|buffer" -> samples
    std::map<juce::String, double> splOffset; // mic source name -> dB to add to dBFS for dB SPL
    juce::String micCalPath;
};

struct SystemState
{
    DeviceConfig device;
    std::vector<SourceConfig> sources;
    std::vector<OutputConfig> outputs;
    GeneratorConfig gen;
    MeasureConfig measure;
    CalibrationData cal;

    // First run only (no sources, no outputs): Input 1/2 and Main L/R patched 1:1, unrouted.
    void ensureDefaults(int numIn, int numOut);
    // Remove a source and its mix column; fix measurement references.
    void removeSource(int index);
    void removeOutput(int index);
};

constexpr float kGeneratorMaxDb = -12.0f; // dBFS RMS hard cap (pink crest ~12 dB)

juce::var toVar(const SystemState&);
juce::var outputsToVar(const std::vector<OutputConfig>&);
juce::var sourcesToVar(const std::vector<SourceConfig>&);
bool sourcesFromVar(const juce::var&, std::vector<SourceConfig>& out, juce::String& error);
// All-or-nothing: `out` is only written when the whole document is valid.
bool fromVar(const juce::var&, SystemState& out, juce::String& error);
bool outputsFromVar(const juce::var&, std::vector<OutputConfig>& out, juce::String& error);
// Path like "outputs/2/gainDb". Unknown paths, wrong types, out-of-range values fail
// and leave the state unchanged.
bool applyPatch(SystemState&, const juce::String& path, const juce::var& value, juce::String& error);
Params toParams(const SystemState&, double sampleRate = 48000.0);

} // namespace vt
