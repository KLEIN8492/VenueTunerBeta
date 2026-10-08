#pragma once
#include "core/AudioEngine.h"
#include "core/SystemState.h"
#include "measure/MicCal.h"
#include "server/Snapshots.h"
#include "measure/Align.h"
#include <array>
#include <functional>

namespace vt {

// Executes protocol commands against the system state and the audio engine.
// Runs on the JUCE message thread only.
//   request:  {"id": n, "cmd": "state.patch", "args": {...}}
//   reply:    {"id": n, "ok": true, "result": ...} | {"id": n, "ok": false, "error": "..."}
class Dispatcher : private juce::Timer
{
public:
    Dispatcher(AudioEngine&, juce::File dataDir);
    ~Dispatcher() override;

    juce::var handle(const juce::var& request);

    // Load autosave (generator forced off) and open the saved device.
    void start();
    void shutdown(); // autosave

    std::function<void()> onStateChanged;
    std::function<void(const juce::var&)> onEvent; // {"event": ..., ...}

    struct Latency { double samples; bool calibrated; };
    static juce::String latencyKey(const juce::String& outDev, const juce::String& inDev, double sr, int buffer);
    Latency ioLatency(const juce::String& outDev, const juce::String& inDev, double sr, int buffer, int driverReported) const;
    Latency currentLatency();

    juce::var statusVar();
    SystemState& stateForTest() { return state; }
    const SystemState& getState() const { return state; }

private:
    juce::var run(const juce::String& cmd, const juce::var& args, juce::String& error);
    void commit(bool measureChanged = false); // push params, broadcast, schedule autosave
    double sampleRate();
    void syncFirs(); // (re)load FIR files whose path changed (snapshot recall, start-up)
    bool loadFirFile(int output, const juce::String& path, juce::String& error);
    juce::var openDevice(const DeviceConfig&, juce::String& error);
    juce::var startLatencyCalibration(int output, int input, juce::String& error);
    void finishCalibration(bool ok, double samples, const juce::String& message);
    juce::var startSweep(double seconds, juce::String& error);
    juce::var startAutoAlign(const juce::var& outputs, int reference, juce::String& error);
    void tickSweep();
    void tickAlign();
    void finishAlign(const juce::String& error);
    void emit(const juce::String& event, std::initializer_list<std::pair<const char*, juce::var>> fields);
    juce::String preflight(); // why automatic measurement can't run now, or empty
    void applyTransient(Params&) const; // sweep / auto-align generator overrides
    void timerCallback() override;

    AudioEngine& engine;
    Snapshots snapshots;
    SystemState state;
    MicCal micCal;
    bool autosavePending = false;

    struct Calibration
    {
        bool active = false;
        GeneratorConfig savedGen;
        MeasureConfig savedMeasure;
        int ticks = 0;
        double last = -1;
        int stable = 0;
    } calib;
    int calibOutput = 0;
    std::array<juce::String, kMaxCh> loadedFir; // path currently in each convolution

    struct Sweep
    {
        bool active = false, analysing = false;
        uint32_t id = 0;
        int64_t length = 0;
        double startedMs = 0, seconds = 0;
        int generation = 0;
    } sweepJob;
    uint32_t sweepCounter = 0;

    struct Align
    {
        bool active = false;
        std::vector<int> outputs;
        size_t index = 0;
        int reference = -1;
        int ticks = 0, stable = 0;
        double last = -1;
        std::vector<Arrival> arrivals;
        juce::String beforeName;
    } alignJob;
};

} // namespace vt
