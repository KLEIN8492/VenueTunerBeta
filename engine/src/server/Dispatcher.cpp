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

    if (cmd == "measure.sweep") return startSweep(args.hasProperty("seconds") ? double(args["seconds"]) : 3.0, err);
    if (cmd == "align.auto") return startAutoAlign(args["outputs"], args.hasProperty("reference") ? int(args["reference"]) : -1, err);
    if (cmd == "align.cancel") { if (alignJob.active) finishAlign("cancelled"); return true; }

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
    auto p = toParams(state, sampleRate());
    applyTransient(p);
    engine.setParams(p);
    engine.analyzer().setMultiWindow(state.measure.multiWindow);
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
    if (sweepJob.active) tickSweep();
    if (alignJob.active) tickAlign();
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
    set(v, "sweeping", sweepJob.active);
    set(v, "aligning", alignJob.active);
    juce::Array<juce::var> inPk, outPk, gr;
    for (int c = 0; c < st.numIn; ++c) inPk.add(st.inPeak[size_t(c)]);
    for (int c = 0; c < st.numOut; ++c) { outPk.add(st.outPeak[size_t(c)]); gr.add(st.limGrDb[size_t(c)]); }
    set(v, "inPeak", inPk);
    set(v, "outPeak", outPk);
    set(v, "limGr", gr);
    return v;
}

} // namespace vt

namespace vt {

void Dispatcher::emit(const juce::String& event, std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    if (!onEvent) return;
    auto e = object();
    set(e, "event", event);
    for (auto& [k, v] : fields) set(e, k, v);
    onEvent(e);
}

juce::String Dispatcher::preflight()
{
    const auto st = engine.status();
    if (st.faultMuted || st.muteAll) return "outputs are muted: unmute first";
    if (state.measure.micInput < 0) return "choose a measurement mic first";
    if (calib.active || sweepJob.active || alignJob.active) return "another automatic measurement is running";
    return {};
}

void Dispatcher::applyTransient(Params& p) const
{
    if (sweepJob.active && !sweepJob.analysing) {
        p.gen.on = true;
        p.gen.mode = GeneratorParams::Sweep;
        p.gen.sweepId = sweepJob.id;
        p.gen.sweepLength = sweepJob.length;
    }
    if (alignJob.active && alignJob.index < alignJob.outputs.size()) {
        p.gen.on = true;
        p.gen.mode = GeneratorParams::Pink;
        p.gen.targets = 1ull << alignJob.outputs[alignJob.index];
    }
}

// ---------------------------------------------------------------- log sweep
juce::var Dispatcher::startSweep(double seconds, juce::String& err)
{
    if (err = preflight(); err.isNotEmpty()) return {};
    if (state.gen.targets.empty()) { err = "choose generator outputs first"; return {}; }
    sweepJob = {};
    sweepJob.active = true;
    sweepJob.seconds = std::clamp(seconds, 1.0, 8.0);
    sweepJob.length = int64_t(sweepJob.seconds * sampleRate());
    sweepJob.id = ++sweepCounter;
    sweepJob.startedMs = juce::Time::getMillisecondCounterHiRes();
    sweepJob.generation = engine.analyzer().sweepGeneration();
    commit();
    return "started";
}

void Dispatcher::tickSweep()
{
    const double elapsed = (juce::Time::getMillisecondCounterHiRes() - sweepJob.startedMs) / 1000.0;
    if (!sweepJob.analysing && elapsed > sweepJob.seconds + 1.5) {
        // Sweep + 1.5 s of tail (room decay, latency) is in the analyzer history now.
        sweepJob.analysing = true;
        commit(); // generator back to normal
        engine.analyzer().requestSweep(int64_t((elapsed + 0.25) * sampleRate()));
        return;
    }
    if (sweepJob.analysing && engine.analyzer().sweepGeneration() != sweepJob.generation) {
        sweepJob.active = false;
        const auto r = engine.analyzer().sweepResult();
        auto toArr = [](const std::vector<float>& v) { juce::Array<juce::var> a; for (float x : v) a.add(x); return juce::var(a); };
        auto t = object();
        set(t, "name", "Sweep " + juce::Time::getCurrentTime().formatted("%H:%M:%S"));
        set(t, "freq", toArr(r.freq));
        set(t, "mag", toArr(r.magDb));
        set(t, "phase", toArr(r.phaseDeg));
        set(t, "coh", toArr(r.coherence));
        set(t, "delaySamples", r.delaySamples);
        set(t, "sampleRate", r.sampleRate);
        emit("sweep", { { "ok", true }, { "trace", t } });
        commit();
    } else if (elapsed > sweepJob.seconds + 10) {
        sweepJob.active = false;
        emit("sweep", { { "ok", false }, { "message", "sweep analysis timed out" } });
        commit();
    }
}

// ---------------------------------------------------------------- auto delay alignment
juce::var Dispatcher::startAutoAlign(const juce::var& outs, int reference, juce::String& err)
{
    if (err = preflight(); err.isNotEmpty()) return {};
    std::vector<int> list;
    if (auto* a = outs.getArray())
        for (auto& v : *a)
            if (int(v) >= 0 && int(v) < int(state.outputs.size())) list.push_back(int(v));
    if (list.size() < 2) { err = "choose at least two outputs to align"; return {}; }
    alignJob = {};
    alignJob.active = true;
    alignJob.outputs = list;
    alignJob.reference = reference;
    alignJob.beforeName = "Before auto-align " + juce::Time::getCurrentTime().formatted("%H-%M-%S");
    snapshots.save(alignJob.beforeName, state, err);
    err = {};
    engine.analyzer().reset();
    commit();
    emit("align", { { "stage", "measuring" }, { "output", list[0] } });
    return "started";
}

void Dispatcher::tickAlign()
{
    ++alignJob.ticks;
    const auto r = engine.analyzer().result();
    if (r.delayValid && alignJob.ticks > 4) { // let the reset settle (~1 s)
        alignJob.stable = std::abs(r.delaySamples - alignJob.last) < 0.05 ? alignJob.stable + 1 : 0;
        alignJob.last = r.delaySamples;
    }
    const int out = alignJob.outputs[alignJob.index];
    if (alignJob.stable >= 3) {
        const auto lat = currentLatency();
        const double io = state.measure.ref == "generator" ? lat.samples : 0.0;
        const double arrivalMs = (r.delaySamples - io) * 1000.0 / r.sampleRate;
        alignJob.arrivals.push_back({ out, arrivalMs, state.outputs[size_t(out)].delayMs });
        emit("align", { { "stage", "measured" }, { "output", out }, { "arrivalMs", arrivalMs } });
        if (++alignJob.index >= alignJob.outputs.size()) { finishAlign({}); return; }
        alignJob.ticks = alignJob.stable = 0;
        alignJob.last = -1;
        engine.analyzer().reset();
        commit();
        emit("align", { { "stage", "measuring" }, { "output", alignJob.outputs[alignJob.index] } });
    } else if (alignJob.ticks > 40) {
        finishAlign("no clear arrival on output " + juce::String(out + 1) + ": check the mic and the speaker");
    }
}

void Dispatcher::finishAlign(const juce::String& error)
{
    const auto arrivals = alignJob.arrivals;
    const int reference = alignJob.reference;
    alignJob.active = false;
    if (error.isNotEmpty()) {
        commit();
        emit("align", { { "stage", "error" }, { "message", error } });
        return;
    }
    juce::Array<juce::var> result;
    for (auto& d : computeAlignment(arrivals, reference)) {
        state.outputs[size_t(d.output)].delayMs = float(std::round(d.delayMs * 1000.0) / 1000.0);
        auto o = object();
        set(o, "output", d.output);
        set(o, "delayMs", d.delayMs);
        result.add(o);
    }
    const auto name = "Auto-align " + juce::Time::getCurrentTime().formatted("%H-%M-%S");
    juce::String err;
    snapshots.save(name, state, err);
    engine.analyzer().reset();
    commit();
    emit("align", { { "stage", "done" }, { "delays", result }, { "snapshot", name } });
}

} // namespace vt
