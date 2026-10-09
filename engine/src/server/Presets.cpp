#include "server/Presets.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace vt {

Presets::Presets(juce::File dataDir) : dir(dataDir.getChildFile("presets")) { dir.createDirectory(); }

juce::File Presets::fileFor(const juce::String& name) const
{
    return dir.getChildFile(juce::File::createLegalFileName(name) + ".vtpreset");
}

juce::StringArray Presets::list() const
{
    juce::StringArray names;
    for (auto& f : dir.findChildFiles(juce::File::findFiles, false, "*.vtpreset")) names.add(f.getFileNameWithoutExtension());
    names.sort(true);
    return names;
}

void Presets::applySpeakerFields(const OutputConfig& from, OutputConfig& to)
{
    to.hp = from.hp;
    to.lp = from.lp;
    to.peq = from.peq;
    to.ap = from.ap;
    to.limiterOn = from.limiterOn;
    to.limThreshDb = from.limThreshDb;
    to.limAttackMs = from.limAttackMs;
    to.limReleaseMs = from.limReleaseMs;
    to.polarityInvert = from.polarityInvert;
    to.gainDb = from.gainDb;
}

juce::var Presets::toVar(const SpeakerPreset& p)
{
    auto o = p.processing;
    o.fir = {}; // the coefficients travel in "fir", not as a path on this machine
    o.mix.clear();
    auto v = juce::var(new juce::DynamicObject());
    auto* d = v.getDynamicObject();
    d->setProperty("format", "venuetuner-speaker-preset");
    d->setProperty("version", 1);
    d->setProperty("name", p.name);
    d->setProperty("processing", outputsToVar({ o })[0]);
    if (!p.fir.empty()) {
        juce::Array<juce::var> taps;
        for (float t : p.fir) taps.add(t);
        auto f = juce::var(new juce::DynamicObject());
        f.getDynamicObject()->setProperty("sampleRate", p.firSampleRate);
        f.getDynamicObject()->setProperty("enabled", p.processing.fir.on);
        f.getDynamicObject()->setProperty("taps", taps);
        d->setProperty("fir", f);
    }
    return v;
}

bool Presets::fromVar(const juce::var& v, SpeakerPreset& out, juce::String& err)
{
    if (!v.isObject() || v["format"].toString() != "venuetuner-speaker-preset") { err = "not a VenueTuner speaker preset"; return false; }
    std::vector<OutputConfig> one;
    juce::Array<juce::var> wrap { v["processing"] };
    if (!outputsFromVar(wrap, one, err) || one.empty()) { err = "preset invalid: " + err; return false; }
    SpeakerPreset p;
    p.name = v["name"].toString();
    p.processing = one[0];
    if (const auto& f = v["fir"]; f.isObject()) {
        const auto* taps = f["taps"].getArray();
        if (taps == nullptr || taps->isEmpty() || taps->size() > 65536) { err = "preset FIR invalid"; return false; }
        for (auto& t : *taps) p.fir.push_back(float(double(t)));
        p.firSampleRate = double(f["sampleRate"]);
        if (p.firSampleRate < 8000) { err = "preset FIR sample rate invalid"; return false; }
        p.processing.fir.on = bool(f["enabled"]);
    }
    out = std::move(p);
    return true;
}

bool Presets::save(const SpeakerPreset& p, juce::String& err)
{
    if (p.name.trim().isEmpty()) { err = "Preset name is empty"; return false; }
    juce::TemporaryFile tmp(fileFor(p.name));
    if (!tmp.getFile().replaceWithText(juce::JSON::toString(toVar(p))) || !tmp.overwriteTargetFileWithTemporary()) {
        err = "Could not write preset";
        return false;
    }
    return true;
}

bool Presets::load(const juce::String& name, SpeakerPreset& out, juce::String& err) const
{
    const auto f = fileFor(name);
    if (!f.existsAsFile()) { err = "No preset named " + name; return false; }
    juce::var v;
    if (juce::JSON::parse(f.loadFileAsString(), v).failed()) { err = "Preset file is corrupt"; return false; }
    return fromVar(v, out, err);
}

bool Presets::remove(const juce::String& name) { return fileFor(name).deleteFile(); }

juce::String Presets::exportJson(const juce::String& name, juce::String& err) const
{
    SpeakerPreset p;
    if (!load(name, p, err)) return {};
    return juce::JSON::toString(toVar(p));
}

bool Presets::importJson(const juce::String& name, const juce::String& json, juce::String& err)
{
    juce::var v;
    if (juce::JSON::parse(json, v).failed()) { err = "invalid preset JSON"; return false; }
    SpeakerPreset p;
    if (!fromVar(v, p, err)) return false;
    p.name = name;
    return save(p, err);
}

juce::String Presets::materialiseFir(const SpeakerPreset& p, juce::String& err) const
{
    if (p.fir.empty()) return {};
    auto firDir = dir.getChildFile("fir");
    firDir.createDirectory();
    const auto file = firDir.getChildFile(juce::File::createLegalFileName(p.name) + ".wav");
    juce::AudioBuffer<float> b(1, int(p.fir.size()));
    for (size_t i = 0; i < p.fir.size(); ++i) b.setSample(0, int(i), p.fir[i]);
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(new juce::FileOutputStream(file), p.firSampleRate, 1, 32, {}, 0));
    if (w == nullptr || !w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples())) { err = "could not write the preset's FIR"; return {}; }
    return file.getFullPathName();
}

} // namespace vt
