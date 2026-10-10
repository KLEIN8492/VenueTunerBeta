#pragma once
#include <juce_core/juce_core.h>

namespace vt {

// Per-direction latency components as declared by the OS audio driver (macOS CoreAudio):
// device latency, safety offset, stream latency (hardware DSP shows up here) and the
// current buffer. Returns an empty array where the platform doesn't expose them.
juce::var latencyBreakdown(const juce::String& inputDevice, const juce::String& outputDevice, int bufferSize);

// True when input and output run on one sample clock: the same device, or two devices in the
// same hardware clock domain (CoreAudio). Otherwise they drift apart and internal-loopback timing
// can't be trusted. Queries the OS: call on a device change, not per status update.
bool sharedClock(const juce::String& inputDevice, const juce::String& outputDevice);

} // namespace vt
