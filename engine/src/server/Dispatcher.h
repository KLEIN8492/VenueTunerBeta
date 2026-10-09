#pragma once
#include "core/AudioEngine.h"
#include "core/SystemState.h"
#include "measure/MicCal.h"
#include "server/Snapshots.h"
#include "server/Presets.h"
#include "measure/Align.h"
#include <array>
#include <map>
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
    // Next tuning change starts a new undo step (tests; the UI uses time-based merging).
    void forceNewUndoStep() { lastUndoKey = {}; }
    // Offset (dB) to turn the main mic's dBFS into dB SPL, and how it was obtained:
    // "calibrator" | "reference" | "estimate" (datasheet values) | "default" (typical values).
    struct SplCal { double offset; juce::String method; bool calibrated() const { return method == "calibrator" || method == "reference"; } };
    SplCal splCal() const;
    std::pair<double, bool> splOffset() const { const auto c = splCal(); return { c.offset, c.calibrated() }; }
    // Typical measurement mic (10 mV/Pa), interface at +18 dBu for 0 dBFS, 40 dB of preamp gain.
    static constexpr double kDefaultSplOffset = 109.8;
    SystemState& stateForTest() { return state; }
    const SystemState& getState() const { return state; }

private:
    juce::var run(const juce::String& cmd, const juce::var& args, juce::String& error);
    void commit(bool measureChanged = false); // push params, broadcast, schedule autosave
    double sampleRate();
    void syncFirs(); // (re)load FIR files whose path changed (snapshot recall, start-up)
    bool loadFirFile(int output, const juce::String& path, juce::String& error);
    juce::var openDevice(const DeviceConfig&, juce::String& error);
    // Validate a whole new state (round-trip through JSON) and make it current.
    bool replaceState(const SystemState& next, juce::String& error, bool structural);

    // Undo history of the tuning (sources + outputs). Rapid changes with the same key (e.g.
    // one field being dragged) merge into one step.
    struct Tuning { std::vector<SourceConfig> sources; std::vector<OutputConfig> outputs; };
    void recordUndo(const juce::String& key);
    juce::var stepHistory(bool undo, juce::String& error);
    std::vector<Tuning> undoStack, redoStack;
    juce::String lastUndoKey;
    double lastUndoMs = 0;
    juce::var routing(const juce::String& cmd, const juce::var& args, juce::String& error);
    juce::var recallSnapshot(const juce::var& args, juce::String& error);
    int nextFreeOutputChannel(const SystemState&) const;
    juce::var startLatencyCalibration(int output, int input, juce::String& error);
    void finishCalibration(bool ok, double samples, const juce::String& message);
    juce::var startSweep(double seconds, juce::String& error);
    juce::var startAutoAlign(const juce::var& outputs, int reference, juce::String& error);
    void tickSweep();
    void tickAlign();
    void finishAlign(const juce::String& error);
    juce::var startMatrix(const juce::var& args, juce::String& error);
    void tickMatrix();
    void finishMatrix(const juce::String& error);
    juce::var traceVar(const Analyzer::Result&, const juce::String& name) const;
    std::pair<double, double> groupBand(const MeasureGroup&) const;
    void emit(const juce::String& event, std::initializer_list<std::pair<const char*, juce::var>> fields);
    juce::String preflight(); // why automatic measurement can't run now, or empty
    void applyTransient(Params&) const; // sweep / auto-align generator overrides
    void timerCallback() override;

    AudioEngine& engine;
    Snapshots snapshots;
    Presets presets;
    juce::var presetCommand(const juce::String& cmd, const juce::var& args, juce::String& error);
    SystemState state;
    MicCal micCal;
    bool autosavePending = false;

    struct Calibration
    {
        bool active = false;
        int output = 0, input = 0; // logical output, physical input
        int ticks = 0;
        double last = -1;
        int stable = 0;
    } calib;
    std::array<juce::String, kMaxCh> loadedFir; // path currently in each slot's convolution
    std::map<int, int> slotOf;                 // output id -> engine slot
    std::array<uint32_t, kMaxCh> slotGen {};   // bumped when a slot changes owner
    void assignSlots();
public:
    int slotForTest(int outputId) const { auto it = slotOf.find(outputId); return it == slotOf.end() ? -1 : it->second; }
private:

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

    // Sequenced measurement: each group alone; one capture per position measured at once
    // (one position with the main mic, or every position that has its own mic).
    struct Capture
    {
        int position = 0, slot = 0;
        int ticks = 0, stable = 0, settledTicks = -1, attempt = 0;
        double last = -1;
        bool done = false;
    };
    struct Matrix
    {
        bool active = false;
        std::vector<int> groups;
        size_t index = 0;
        std::vector<Capture> captures;
    } matrixJob;
    void resetCaptures();
};

} // namespace vt
