#pragma once
#include "core/SystemState.h"

namespace vt {

// What a snapshot file contains. Version 2 carries the patch (sources + output channels)
// and the device it was made on; version 1 files (outputs only, mixes indexed by physical
// input) load with hasPatch = false.
struct SnapshotData
{
    bool hasPatch = false;
    juce::String inputDevice, outputDevice;
    std::vector<SourceConfig> sources;
    std::vector<OutputConfig> outputs;
};

// Named snapshots and the autosaved full state, stored as JSON under `dir`.
// Writes go to a temp file then rename (atomic).
class Snapshots
{
public:
    explicit Snapshots(juce::File dir);

    juce::StringArray list() const;
    bool save(const juce::String& name, const SystemState&, juce::String& error);
    bool load(const juce::String& name, SnapshotData& out, juce::String& error) const;
    bool remove(const juce::String& name);
    juce::File fileFor(const juce::String& name) const;

    void autosave(const SystemState&);
    bool loadAutosave(SystemState&, juce::String& error) const;

private:
    juce::File dir, snapDir;
};

} // namespace vt
