#include "dsp/FirLoader.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace vt {

bool FirLoader::load(const juce::File& f, double engineSr, juce::AudioBuffer<float>& out, double& fileSr, juce::String& err)
{
    if (!f.existsAsFile()) { err = "File not found: " + f.getFullPathName(); return false; }
    const auto ext = f.getFileExtension().toLowerCase();

    if (ext == ".txt" || ext == ".csv" || ext == ".dat") {
        std::vector<float> taps;
        for (auto line : juce::StringArray::fromLines(f.loadFileAsString())) {
            line = line.trim();
            if (line.isEmpty()) continue;
            const auto c = line[0];
            if (!(juce::CharacterFunctions::isDigit(c) || c == '-' || c == '+' || c == '.')) continue;
            taps.push_back(line.upToFirstOccurrenceOf(",", false, false).getFloatValue());
            if (int(taps.size()) > kMaxTaps) { err = "FIR longer than " + juce::String(kMaxTaps) + " taps"; return false; }
        }
        if (taps.empty()) { err = "No coefficients found in " + f.getFileName(); return false; }
        out.setSize(1, int(taps.size()));
        for (int i = 0; i < int(taps.size()); ++i) out.setSample(0, i, taps[size_t(i)]);
        fileSr = engineSr; // text files carry no rate: assume they were designed for this one
        return true;
    }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
    if (r == nullptr) { err = "Unsupported FIR file (use WAV/AIFF or text): " + f.getFileName(); return false; }
    if (r->lengthInSamples <= 0 || r->lengthInSamples > kMaxTaps) { err = "FIR must have 1–" + juce::String(kMaxTaps) + " taps"; return false; }
    const int n = int(r->lengthInSamples);
    juce::AudioBuffer<float> tmp(int(r->numChannels), n);
    r->read(&tmp, 0, n, 0, true, true);
    out.setSize(1, n);
    out.copyFrom(0, 0, tmp, 0, 0, n);
    fileSr = r->sampleRate;
    return true;
}

int FirLoader::peakIndex(const juce::AudioBuffer<float>& b)
{
    int best = 0;
    const float* d = b.getReadPointer(0);
    for (int i = 1; i < b.getNumSamples(); ++i)
        if (std::abs(d[i]) > std::abs(d[best])) best = i;
    return best;
}

} // namespace vt
