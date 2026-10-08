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
    auto* d = v.getDynamicObject();
    d->setProperty("version", 2);
    d->setProperty("inputDevice", s.device.inputDevice);
    d->setProperty("outputDevice", s.device.outputDevice);
    d->setProperty("sources", sourcesToVar(s.sources));
    d->setProperty("outputs", outputsToVar(s.outputs));
    if (!writeAtomically(fileFor(name), juce::JSON::toString(v))) { err = "Could not write snapshot"; return false; }
    return true;
}

bool Snapshots::load(const juce::String& name, SnapshotData& out, juce::String& err) const
{
    const auto f = fileFor(name);
    if (!f.existsAsFile()) { err = "No snapshot named " + name; return false; }
    juce::var v;
    if (juce::JSON::parse(f.loadFileAsString(), v).failed() || !v.isObject()) { err = "Snapshot file is corrupt"; return false; }
    SnapshotData d;
    d.hasPatch = int(v["version"]) >= 2;
    d.inputDevice = v["inputDevice"].toString();
    d.outputDevice = v["outputDevice"].toString();
    if (d.hasPatch && !sourcesFromVar(v["sources"], d.sources, err)) { err = "Snapshot invalid: " + err; return false; }
    if (!outputsFromVar(v["outputs"], d.outputs, err)) { err = "Snapshot invalid: " + err; return false; }
    out = std::move(d);
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
