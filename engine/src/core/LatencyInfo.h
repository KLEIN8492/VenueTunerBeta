#pragma once
#include <juce_core/juce_core.h>

namespace vt {

// Per-direction latency components as declared by the OS audio driver (macOS CoreAudio):
// device latency, safety offset, stream latency (hardware DSP shows up here) and the
// current buffer. Returns an empty array where the platform doesn't expose them.
juce::var latencyBreakdown(const juce::String& inputDevice, const juce::String& outputDevice, int bufferSize);

} // namespace vt
