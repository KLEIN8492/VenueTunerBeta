#include "core/LatencyInfo.h"

#if JUCE_MAC
#include <CoreAudio/CoreAudio.h>

namespace vt {

namespace {
UInt32 prop(AudioObjectID o, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress a { sel, scope, kAudioObjectPropertyElementMain };
    UInt32 v = 0, n = sizeof v;
    return AudioObjectGetPropertyData(o, &a, 0, nullptr, &n, &v) == noErr ? v : 0;
}

AudioObjectID findDevice(const juce::String& name)
{
    AudioObjectPropertyAddress a { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return 0;
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr) return 0;
    for (auto id : ids) {
        CFStringRef cf = nullptr;
        UInt32 sz = sizeof cf;
        AudioObjectPropertyAddress na { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(id, &na, 0, nullptr, &sz, &cf) != noErr || cf == nullptr) continue;
        const auto s = juce::String::fromCFString(cf);
        CFRelease(cf);
        if (s == name) return id;
    }
    return 0;
}

juce::var direction(const juce::String& name, bool output, int buffer)
{
    const auto id = findDevice(name);
    if (id == 0) return {};
    const auto scope = output ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput;
    UInt32 streamLatency = 0;
    AudioObjectPropertyAddress sa { kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &sa, 0, nullptr, &size) == noErr && size >= sizeof(AudioStreamID)) {
        std::vector<AudioStreamID> streams(size / sizeof(AudioStreamID));
        if (AudioObjectGetPropertyData(id, &sa, 0, nullptr, &size, streams.data()) == noErr)
            streamLatency = prop(streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal);
    }
    auto v = juce::var(new juce::DynamicObject());
    auto* d = v.getDynamicObject();
    d->setProperty("direction", output ? "output" : "input");
    d->setProperty("device", name);
    d->setProperty("deviceLatency", int(prop(id, kAudioDevicePropertyLatency, scope)));
    d->setProperty("safetyOffset", int(prop(id, kAudioDevicePropertySafetyOffset, scope)));
    d->setProperty("streamLatency", int(streamLatency));
    d->setProperty("buffer", buffer);
    return v;
}
} // namespace

bool sharedClock(const juce::String& in, const juce::String& out)
{
    if (in == out) return true;
    const auto a = findDevice(in), b = findDevice(out);
    if (a == 0 || b == 0) return false;
    const auto da = prop(a, kAudioDevicePropertyClockDomain, kAudioObjectPropertyScopeGlobal);
    const auto db = prop(b, kAudioDevicePropertyClockDomain, kAudioObjectPropertyScopeGlobal);
    return da != 0 && da == db; // 0 = domain unknown
}

juce::var latencyBreakdown(const juce::String& in, const juce::String& out, int buffer)
{
    juce::Array<juce::var> a;
    if (auto v = direction(in, false, buffer); v.isObject()) a.add(v);
    if (auto v = direction(out, true, buffer); v.isObject()) a.add(v);
    return a;
}

} // namespace vt

#else

namespace vt {
juce::var latencyBreakdown(const juce::String&, const juce::String&, int) { return juce::Array<juce::var>(); }
bool sharedClock(const juce::String& in, const juce::String& out) { return in == out; }
}

#endif
