#pragma once
#include "core/SystemState.h"

namespace vt {

// Named snapshots of the processing (outputs: routing + chain) and the autosaved full
// state, stored as JSON under `dir`. Writes go to a temp file then rename (atomic).
class Snapshots
{
public:
    explicit Snapshots(juce::File dir);

    juce::StringArray list() const;
    bool save(const juce::String& name, const SystemState&, juce::String& error);
    // Replaces state.outputs only if the snapshot is valid and matches the input count.
    bool recall(const juce::String& name, SystemState&, juce::String& error) const;
    bool remove(const juce::String& name);
    juce::File fileFor(const juce::String& name) const;

    void autosave(const SystemState&);
    bool loadAutosave(SystemState&, juce::String& error) const;

private:
    juce::File dir, snapDir;
};

} // namespace vt
