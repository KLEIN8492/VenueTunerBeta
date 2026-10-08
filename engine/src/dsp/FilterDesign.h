#pragma once
#include "core/Params.h"
#include <juce_core/juce_core.h>

namespace vt {

struct OutputConfig;

XoType parseXoType(const juce::String&);
// Fills the 28 fixed IIR slots (PEQ 0–15, HP 16–19, LP 20–23, AP 24–27) for one output.
void designOutputIir(const OutputConfig&, double sampleRate, Biquad* slots);

} // namespace vt
