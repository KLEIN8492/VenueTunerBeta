#include "server/Dispatcher.h"
#include "dsp/FirLoader.h"

namespace vt {

namespace {
juce::var object() { return juce::var(new juce::DynamicObject()); }
void set(juce::var& o, const juce::Identifier& k, const juce::var& v) { o.getDynamicObject()->setProperty(k, v); }
}

Dispatcher::Dispatcher(AudioEngine& e, juce::File dataDir) : engine(e), snapshots(std::move(dataDir))
{
    startTimer(250);
}

Dispatcher::~Dispatcher() { stopTimer(); }

void Dispatcher::start()
{
    juce::String err;
    if (!snapshots.loadAutosave(state, err)) state = SystemState {};
    state.gen.on = false; // never come back up playing noise
    if (state.cal.micCalPath.isNotEmpty() && micCal.load(juce::File(state.cal.micCalPath), err))
        engine.analyzer().setMicCal(micCal);
    openDevice(state.device, err); // failures are retried by the engine watchdog
    commit(true);
}

void Dispatcher::shutdown() { snapshots.autosave(state); }

juce::String Dispatcher::latencyKey(const juce::String& outDev, const juce::String& inDev, double sr, int buffer)
{
    return outDev + "|" + inDev + "|" + juce::String(juce::roundToInt(sr)) + "|" + juce::String(buffer);
}

Dispatcher::Latency Dispatcher::ioLatency(const juce::String& outDev, const juce::String& inDev, double sr,
                                          int buffer, int driverReported) const
{
    const auto it = state.cal.ioLatency.find(latencyKey(outDev, inDev, sr, buffer));
    if (it != state.cal.ioLatency.end()) return { it->second, true };
    return { double(driverReported), false };
}

Dispatcher::Latency Dispatcher::currentLatency()
{
    const auto st = engine.status();
    return ioLatency(st.outputDevice, st.inputDevice, st.sampleRate, st.bufferSize, st.driverLatencySamples);
}

juce::var Dispatcher::handle(const juce::var& request)
{
    auto reply = object();
    set(reply, "id", request["id"]);
    juce::String error;
    const auto cmd = request["cmd"].toString();
    juce::var result = cmd.isEmpty() ? juce::var() : run(cmd, request["args"], error);
    if (cmd.isEmpty()) error = "missing cmd";
    set(reply, "ok", error.isEmpty());
    if (error.isEmpty()) set(reply, "result", result);
    else set(reply, "error", error);
    return reply;
}

juce::var Dispatcher::run(const juce::String& cmd, const juce::var& args, juce::String& err)
{
    if (cmd == "state.get") return toVar(state);

    if (cmd == "state.patch" || cmd == "state.patchMany") {
        juce::Array<juce::var> patches;
        if (cmd == "state.patch") patches.add(args);
        else if (auto* a = args["patches"].getArray()) patches = *a;
        SystemState next = state;
        bool measureChanged = false;
        for (auto& p : patches) {
            const auto path = p["path"].toString();
            if (path.startsWith("device")) { err = "use device.open to change the audio device"; return {}; }
            if (!applyPatch(next, path, p["value"], err)) return {};
            measureChanged |= path.startsWith("measure/micInput") || path.startsWith("measure/ref");
        }
        state = std::move(next);
        commit(measureChanged);
        return toVar(state);
    }

    if (cmd == "devices.list") return engine.listDevices();

    if (cmd == "device.open") {
        DeviceConfig cfg = state.device;
        if (args.hasProperty("inputDevice")) cfg.inputDevice = args["inputDevice"].toString();
        if (args.hasProperty("outputDevice")) cfg.outputDevice = args["outputDevice"].toString();
        if (args.hasProperty("sampleRate")) cfg.sampleRate = double(args["sampleRate"]);
        if (args.hasProperty("bufferSize")) cfg.bufferSize = int(args["bufferSize"]);
        return openDevice(cfg, err);
    }

    if (cmd == "mute.all") { engine.setMuteAll(bool(args["on"])); return statusVar(); }
    if (cmd == "unmute.fault") { engine.unmuteAfterFault(); return statusVar(); }

    if (cmd == "measure.reset") { engine.analyzer().reset(); return true; }
    if (cmd == "measure.capture") {
        const auto r = engine.analyzer().result();
        if (r.frames == 0) { err = "no measurement data yet"; return {}; }
        auto t = object();
        set(t, "name", args["name"].toString());
        auto toArr = [](const std::vector<float>& v) { juce::Array<juce::var> a; for (float x : v) a.add(x); return juce::var(a); };
        set(t, "freq", toArr(r.freq));
        set(t, "mag", toArr(r.magDb));
        set(t, "phase", toArr(r.phaseDeg));
        set(t, "coh", toArr(r.coherence));
        set(t, "delaySamples", r.delaySamples);
        set(t, "sampleRate", r.sampleRate);
        return t;
    }

    if (cmd == "fir.load") {
        const int o = int(args["output"]);
        if (o < 0 || o >= int(state.outputs.size())) { err = "invalid output"; return {}; }
        if (!loadFirFile(o, args["path"].toString(), err)) return {};
        state.outputs[size_t(o)].fir.on = true;
        commit();
        return toVar(state);
    }
    if (cmd == "fir.clear") {
        const int o = int(args["output"]);
        if (o < 0 || o >= int(state.outputs.size())) { err = "invalid output"; return {}; }
        state.outputs[size_t(o)].fir = {};
        loadedFir[size_t(o)] = {};
        commit();
        return toVar(state);
    }

    if (cmd == "calibrate.latency") return startLatencyCalibration(int(args["output"]), int(args["input"]), err);
    if (cmd == "calibrate.clear") {
        const auto st = engine.status();
        state.cal.ioLatency.erase(latencyKey(st.outputDevice, st.inputDevice, st.sampleRate, st.bufferSize));
        commit();
        return true;
    }

    if (cmd == "cal.loadMic") {
        MicCal c;
        const juce::File f(args["path"].toString());
        if (!c.load(f, err)) return {};
        micCal = c;
        engine.analyzer().setMicCal(micCal);
        state.cal.micCalPath = f.getFullPathName();
        commit();
        return true;
    }
    if (cmd == "cal.clearMic") {
        micCal.clear();
        engine.analyzer().setMicCal(micCal);
        state.cal.micCalPath = {};
        commit();
        return true;
    }

    if (cmd == "snapshot.list") { juce::Array<juce::var> a; for (auto& n : snapshots.list()) a.add(n); return a; }
    if (cmd == "snapshot.save") { if (!snapshots.save(args["name"].toString(), state, err)) return {}; return true; }
    if (cmd == "snapshot.recall") {
        if (!snapshots.recall(args["name"].toString(), state, err)) return {};
        commit();
        return toVar(state);
    }
    if (cmd == "snapshot.delete") {
        if (!snapshots.remove(args["name"].toString())) { err = "could not delete snapshot"; return {}; }
        return true;
    }
    if (cmd == "snapshot.export") {
        const auto f = snapshots.fileFor(args["name"].toString());
        if (!f.existsAsFile()) { err = "no such snapshot"; return {}; }
        return f.loadFileAsString();
    }
    if (cmd == "snapshot.import") {
        juce::var v;
        std::vector<OutputConfig> outs;
        if (juce::JSON::parse(args["json"].toString(), v).failed() || !outputsFromVar(v["outputs"], outs, err)) {
            if (err.isEmpty()) err = "invalid snapshot JSON";
            return {};
        }
        SystemState tmp = state;
        tmp.outputs = outs;
        if (!snapshots.save(args["name"].toString(), tmp, err)) return {};
        return true;
    }

    err = "unknown command: " + cmd;
    return {};
}

juce::var Dispatcher::openDevice(const DeviceConfig& cfg, juce::String& err)
{
    err = engine.openDevice(cfg);
    const auto st = engine.status();
    if (err.isEmpty()) {
        state.device = cfg;
        state.device.sampleRate = st.sampleRate;
        state.device.bufferSize = st.bufferSize;
        state.resizeIO(st.numIn, st.numOut);
        loadedFir.fill({}); // reload FIRs: latency in samples depends on the new rate
        for (int i = 0; i < int(state.inputs.size()); ++i)
            if (state.inputs[size_t(i)].name.isEmpty()) state.inputs[size_t(i)].name = "In " + juce::String(i + 1);
        commit(true);
    }
    return statusVar();
}

double Dispatcher::sampleRate()
{
    const double sr = engine.status().sampleRate;
    return sr > 0 ? sr : state.device.sampleRate;
}

bool Dispatcher::loadFirFile(int o, const juce::String& path, juce::String& err)
{
    juce::AudioBuffer<float> ir;
    double fileSr = 0;
    if (!FirLoader::load(juce::File(path), sampleRate(), ir, fileSr, err)) return false;
    auto& f = state.outputs[size_t(o)].fir;
    f.path = path;
    f.taps = ir.getNumSamples();
    // Latency of the filter itself (e.g. half the length for linear phase), in engine samples.
    f.latencySamples = juce::roundToInt(FirLoader::peakIndex(ir) * sampleRate() / fileSr);
    engine.loadFir(o, std::move(ir), fileSr);
    loadedFir[size_t(o)] = path;
    return true;
}

void Dispatcher::syncFirs()
{
    for (size_t o = 0; o < state.outputs.size(); ++o) {
        auto& f = state.outputs[o].fir;
        if (f.path.isEmpty() || f.path == loadedFir[o]) continue;
        juce::String err;
        if (!loadFirFile(int(o), f.path, err)) {
            f.on = false;
            f.taps = 0;
            if (onEvent) {
                auto e = object();
                set(e, "event", "error");
                set(e, "message", "Output " + juce::String(int(o) + 1) + " FIR: " + err);
                onEvent(e);
            }
        }
    }
}

void Dispatcher::commit(bool measureChanged)
{
    syncFirs();
    engine.setParams(toParams(state, sampleRate()));
    engine.analyzer().setAveraging(state.measure.averaging);
    engine.analyzer().setSmoothing(state.measure.smoothing);
    if (measureChanged) engine.analyzer().reset();
    autosavePending = true;
    if (onStateChanged) onStateChanged();
}

// ---------------------------------------------------------------- latency calibration
juce::var Dispatcher::startLatencyCalibration(int output, int input, juce::String& err)
{
    if (calib.active) { err = "calibration already running"; return {}; }
    if (output < 0 || output >= int(state.outputs.size()) || input < 0 || input >= int(state.inputs.size())) {
        err = "invalid output/input";
        return {};
    }
    const auto st = engine.status();
    if (st.faultMuted || st.muteAll) { err = "outputs are muted: unmute before calibrating"; return {}; }
    calib = {};
    calib.active = true;
    calib.savedGen = state.gen;
    calib.savedMeasure = state.measure;
    state.gen.on = true;
    state.gen.levelDb = -20.0f;
    state.gen.targets = { output };
    state.measure.micInput = input;
    state.measure.ref = "generator";
    calibOutput = output;
    commit(true);
    return "started";
}

void Dispatcher::finishCalibration(bool ok, double samples, const juce::String& message)
{
    state.gen = calib.savedGen;
    state.measure = calib.savedMeasure;
    calib.active = false;
    if (ok) {
        const auto st = engine.status();
        state.cal.ioLatency[latencyKey(st.outputDevice, st.inputDevice, st.sampleRate, st.bufferSize)] = samples;
    }
    commit(true);
    if (onEvent) {
        auto e = object();
        set(e, "event", "calibration");
        set(e, "ok", ok);
        set(e, "samples", samples);
        set(e, "message", message);
        onEvent(e);
    }
}

void Dispatcher::timerCallback()
{
    if (calib.active) {
        ++calib.ticks;
        const auto r = engine.analyzer().result();
        if (r.delayValid) {
            // Remove the output's own processing delay: what remains is the interface.
            const double d = r.delaySamples - state.outputs[size_t(calibOutput)].delayMs * 0.001 * r.sampleRate;
            calib.stable = std::abs(d - calib.last) < 0.05 ? calib.stable + 1 : 0;
            calib.last = d;
            if (calib.stable >= 3) finishCalibration(true, d, "Interface latency measured");
        }
        if (calib.active && calib.ticks > 60)
            finishCalibration(false, 0, "No loopback signal found: check the cable from the output to the input");
    }
    if (autosavePending) { // debounce: at most 4 writes per second
        autosavePending = false;
        snapshots.autosave(state);
    }
}

juce::var Dispatcher::statusVar()
{
    const auto st = engine.status();
    const auto lat = ioLatency(st.outputDevice, st.inputDevice, st.sampleRate, st.bufferSize, st.driverLatencySamples);
    auto v = object();
    set(v, "event", "status");
    set(v, "inputDevice", st.inputDevice);
    set(v, "outputDevice", st.outputDevice);
    set(v, "sampleRate", st.sampleRate);
    set(v, "bufferSize", st.bufferSize);
    set(v, "numIn", st.numIn);
    set(v, "numOut", st.numOut);
    set(v, "cpu", st.cpu);
    set(v, "xruns", st.xruns);
    set(v, "callbacks", st.callbacks);
    set(v, "running", st.running);
    set(v, "faultMuted", st.faultMuted);
    set(v, "muteAll", st.muteAll);
    set(v, "error", st.lastError);
    set(v, "driverLatency", st.driverLatencySamples);
    set(v, "ioLatency", lat.samples);
    set(v, "ioLatencyCalibrated", lat.calibrated);
    set(v, "calibrating", calib.active);
    juce::Array<juce::var> inPk, outPk, gr;
    for (int c = 0; c < st.numIn; ++c) inPk.add(st.inPeak[size_t(c)]);
    for (int c = 0; c < st.numOut; ++c) { outPk.add(st.outPeak[size_t(c)]); gr.add(st.limGrDb[size_t(c)]); }
    set(v, "inPeak", inPk);
    set(v, "outPeak", outPk);
    set(v, "limGr", gr);
    return v;
}

} // namespace vt
