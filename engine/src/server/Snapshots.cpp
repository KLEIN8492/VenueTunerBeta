#include "server/Snapshots.h"

namespace vt {

namespace {
bool writeAtomically(const juce::File& target, const juce::String& text)
{
    target.getParentDirectory().createDirectory();
    juce::TemporaryFile tmp(target);
    return tmp.getFile().replaceWithText(text) && tmp.overwriteTargetFileWithTemporary();
}
}

Snapshots::Snapshots(juce::File d) : dir(std::move(d)), snapDir(dir.getChildFile("snapshots"))
{
    snapDir.createDirectory();
}

juce::File Snapshots::fileFor(const juce::String& name) const
{
    return snapDir.getChildFile(juce::File::createLegalFileName(name) + ".json");
}

juce::StringArray Snapshots::list() const
{
    juce::StringArray names;
    for (auto& f : snapDir.findChildFiles(juce::File::findFiles, false, "*.json"))
        names.add(f.getFileNameWithoutExtension());
    names.sort(true);
    return names;
}

bool Snapshots::save(const juce::String& name, const SystemState& s, juce::String& err)
{
    if (name.trim().isEmpty()) { err = "Snapshot name is empty"; return false; }
    auto v = juce::var(new juce::DynamicObject());
    v.getDynamicObject()->setProperty("version", 1);
    v.getDynamicObject()->setProperty("outputs", outputsToVar(s.outputs));
    if (!writeAtomically(fileFor(name), juce::JSON::toString(v))) { err = "Could not write snapshot"; return false; }
    return true;
}

bool Snapshots::recall(const juce::String& name, SystemState& s, juce::String& err) const
{
    const auto f = fileFor(name);
    if (!f.existsAsFile()) { err = "No snapshot named " + name; return false; }
    juce::var v;
    if (juce::JSON::parse(f.loadFileAsString(), v).failed() || !v.isObject()) { err = "Snapshot file is corrupt"; return false; }
    std::vector<OutputConfig> outs;
    if (!outputsFromVar(v["outputs"], outs, err)) { err = "Snapshot invalid: " + err; return false; }
    for (auto& o : outs) o.mix.resize(s.inputs.size(), 0.0f);
    outs.resize(s.outputs.size(), outs.empty() ? OutputConfig {} : outs.back());
    for (size_t i = 0; i < outs.size(); ++i)
        if (outs[i].name.isEmpty()) outs[i].name = "Out " + juce::String(int(i) + 1);
    s.outputs = std::move(outs);
    return true;
}

bool Snapshots::remove(const juce::String& name) { return fileFor(name).deleteFile() && !fileFor(name).exists(); }

void Snapshots::autosave(const SystemState& s)
{
    writeAtomically(dir.getChildFile("state.json"), juce::JSON::toString(toVar(s)));
}

bool Snapshots::loadAutosave(SystemState& s, juce::String& err) const
{
    const auto f = dir.getChildFile("state.json");
    if (!f.existsAsFile()) { err = "No autosave"; return false; }
    juce::var v;
    if (juce::JSON::parse(f.loadFileAsString(), v).failed()) { err = "Autosave is corrupt"; return false; }
    return fromVar(v, s, err);
}

} // namespace vt
