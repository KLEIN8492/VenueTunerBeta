#include "server/Dispatcher.h"
#include "dsp/FirLoader.h"
#include "measure/Quality.h"

namespace vt {

namespace {
juce::var object() { return juce::var(new juce::DynamicObject()); }
void set(juce::var& o, const juce::Identifier& k, const juce::var& v) { o.getDynamicObject()->setProperty(k, v); }
}

Dispatcher::Dispatcher(AudioEngine& e, juce::File dataDir) : engine(e), snapshots(dataDir), presets(dataDir)
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
        bool measureChanged = false, tuning = false;
        juce::String key;
        for (auto& p : patches) {
            const auto path = p["path"].toString();
            if (path.startsWith("outputs") || path.startsWith("sources")) { tuning = true; key << path << ";"; }
        }
        if (tuning) recordUndo(key);
        for (auto& p : patches) {
            const auto path = p["path"].toString();
            if (path.startsWith("device")) { err = "use device.open to change the audio device"; return {}; }
            if (!applyPatch(next, path, p["value"], err)) return {};
            measureChanged |= path.startsWith("measure/micSource") || path.startsWith("measure/ref")
                           || path.startsWith("sources");
        }
        state = std::move(next);
        commit(measureChanged);
        return toVar(state);
    }

    if (cmd == "devices.list") return engine.listDevices();
    if (cmd == "device.channels") return engine.channelNames();
    if (cmd == "device.latency") return engine.latencyBreakdown();
    if (cmd.startsWith("routing.")) { recordUndo(cmd); forceNewUndoStep(); return routing(cmd, args, err); }

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
        return traceVar(r, args["name"].toString());
    }

    if (cmd == "measure.sweep") return startSweep(args.hasProperty("seconds") ? double(args["seconds"]) : 3.0, err);
    if (cmd == "align.auto") return startAutoAlign(args["outputs"], args.hasProperty("reference") ? int(args["reference"]) : -1, err);
    if (cmd == "align.cancel") { if (alignJob.active) finishAlign("cancelled"); return true; }
    if (cmd == "matrix.run") return startMatrix(args, err);
    if (cmd.startsWith("preset.")) return presetCommand(cmd, args, err);
    if (cmd == "matrix.cancel") { if (matrixJob.active) finishMatrix("cancelled"); return true; }

    if (cmd == "spl.calibrate") {
        const int mic = state.measure.micSource;
        if (mic < 0) { err = "choose the measurement mic first"; return {}; }
        const double level = args.hasProperty("level") ? double(args["level"]) : 94.0;
        const auto r = engine.analyzer().spl().reading();
        if (r.slow[0] < -100) { err = "no signal on the mic: put the calibrator on it"; return {}; }
        state.cal.splOffset[state.sources[size_t(mic)].name] = level - r.slow[0];
        commit();
        return state.cal.splOffset[state.sources[size_t(mic)].name];
    }
    if (cmd == "spl.clearCal") {
        if (state.measure.micSource >= 0) state.cal.splOffset.erase(state.sources[size_t(state.measure.micSource)].name);
        commit();
        return true;
    }
    if (cmd == "spl.resetLeq") { engine.analyzer().spl().resetLeq(); return true; }
    if (cmd == "rta.setResolution") { engine.analyzer().rta().setResolution(int(args["bands"])); return true; }

    if (cmd == "fir.load") {
        recordUndo("fir");
        forceNewUndoStep();
        const int o = int(args["output"]);
        if (o < 0 || o >= int(state.outputs.size())) { err = "invalid output"; return {}; }
        if (!loadFirFile(o, args["path"].toString(), err)) return {};
        state.outputs[size_t(o)].fir.on = true;
        commit();
        return toVar(state);
    }
    if (cmd == "fir.clear") {
        recordUndo("fir");
        forceNewUndoStep();
        const int o = int(args["output"]);
        if (o < 0 || o >= int(state.outputs.size())) { err = "invalid output"; return {}; }
        state.outputs[size_t(o)].fir = {};
        loadedFir[size_t(slotOf[state.outputs[size_t(o)].id])] = {};
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
        recordUndo("recall");
        forceNewUndoStep();
        auto r = recallSnapshot(args, err);
        if (err.isNotEmpty() || r.hasProperty("needsRemap")) undoStack.pop_back(); // nothing applied
        return r;
    }
    if (cmd == "history.undo") return stepHistory(true, err);
    if (cmd == "history.redo") return stepHistory(false, err);
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
        // Store the file as given (it may come from another system / interface).
        const auto f = snapshots.fileFor(args["name"].toString());
        if (args["name"].toString().trim().isEmpty()) { err = "Snapshot name is empty"; return {}; }
        f.getParentDirectory().createDirectory();
        if (!f.replaceWithText(juce::JSON::toString(v))) { err = "Could not write snapshot"; return {}; }
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
        state.device.inputDevice = st.inputDevice;   // remember what was actually opened
        state.device.outputDevice = st.outputDevice; // (defaults resolved on a fresh install)
        state.device.sampleRate = st.sampleRate;
        state.device.bufferSize = st.bufferSize;
        state.ensureDefaults(st.numIn, st.numOut);
        loadedFir.fill({}); // reload FIRs: latency in samples depends on the new rate
        commit(true);
    }
    return statusVar();
}

double Dispatcher::sampleRate()
{
    const double sr = engine.status().sampleRate;
    return sr > 0 ? sr : state.device.sampleRate;
}

// Outputs keep their engine slot for life; a freed slot is reset before a new output uses it.
void Dispatcher::assignSlots()
{
    std::map<int, int> next;
    std::array<bool, kMaxCh> used {};
    for (auto& o : state.outputs)
        if (auto it = slotOf.find(o.id); it != slotOf.end()) { next[o.id] = it->second; used[size_t(it->second)] = true; }
    for (auto& o : state.outputs) {
        if (next.count(o.id)) continue;
        for (int sl = 0; sl < kMaxCh; ++sl)
            if (!used[size_t(sl)]) {
                used[size_t(sl)] = true;
                next[o.id] = sl;
                ++slotGen[size_t(sl)];
                loadedFir[size_t(sl)] = {}; // the previous owner's FIR must not leak to the new output
                break;
            }
    }
    slotOf = std::move(next);
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
    assignSlots();
    const int slot = slotOf[state.outputs[size_t(o)].id];
    engine.loadFir(slot, std::move(ir), fileSr);
    loadedFir[size_t(slot)] = path;
    return true;
}

void Dispatcher::syncFirs()
{
    assignSlots();
    for (size_t o = 0; o < state.outputs.size(); ++o) {
        auto& f = state.outputs[o].fir;
        if (f.path.isEmpty() || f.path == loadedFir[size_t(slotOf[state.outputs[o].id])]) continue;
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
    for (size_t o = 0; o < state.outputs.size(); ++o) {
        const int sl = slotOf[state.outputs[o].id];
        p.out[o].slot = sl;
        p.out[o].slotGen = slotGen[size_t(sl)];
    }
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
    if (calib.active || sweepJob.active || alignJob.active) { err = "another automatic measurement is running"; return {}; }
    const auto st = engine.status();
    if (output < 0 || output >= int(state.outputs.size()) || input < 0 || input >= st.numIn) {
        err = "invalid output/input";
        return {};
    }
    if (state.outputs[size_t(output)].channel < 0) { err = "that output is not patched to a physical channel"; return {}; }
    if (st.faultMuted || st.muteAll) { err = "outputs are muted: unmute before calibrating"; return {}; }
    calib = {};
    calib.active = true;
    calib.output = output;
    calib.input = input;
    engine.analyzer().reset();
    commit();
    return "started";
}

void Dispatcher::finishCalibration(bool ok, double samples, const juce::String& message)
{
    calib.active = false;
    if (ok) {
        const auto st = engine.status();
        state.cal.ioLatency[latencyKey(st.outputDevice, st.inputDevice, st.sampleRate, st.bufferSize)] = samples;
    }
    engine.analyzer().reset();
    commit();
    emit("calibration", { { "ok", ok }, { "samples", samples }, { "message", message } });
}

void Dispatcher::timerCallback()
{
    if (sweepJob.active) tickSweep();
    if (matrixJob.active) tickMatrix();
    if (alignJob.active) tickAlign();
    if (calib.active) {
        ++calib.ticks;
        const auto r = engine.analyzer().result();
        if (r.delayValid) {
            // Remove the output's own processing delay (delay + FIR): what remains is the interface.
            const auto& o = state.outputs[size_t(calib.output)];
            const double d = r.delaySamples - o.delayMs * 0.001 * r.sampleRate - (o.fir.on ? o.fir.latencySamples : 0);
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
    {
        const auto r = engine.analyzer().spl().reading();
        const auto [off, cal] = splOffset();
        auto spl = object();
        auto arr3 = [off = off](const std::array<double, 3>& a) {
            juce::Array<juce::var> x;
            for (double d : a) x.add(d > -150 ? d + off : -200.0);
            return juce::var(x);
        };
        set(spl, "fast", arr3(r.fast));
        set(spl, "slow", arr3(r.slow));
        set(spl, "leq", arr3(r.leq));
        set(spl, "peak", r.peakZ > -150 ? r.peakZ + off : -200.0);
        set(spl, "leqSeconds", r.leqSeconds);
        set(spl, "calibrated", cal);
        set(v, "spl", spl);
    }
    set(v, "sweeping", sweepJob.active);
    set(v, "canUndo", int(undoStack.size()));
    set(v, "canRedo", int(redoStack.size()));
    set(v, "aligning", alignJob.active);
    set(v, "matrixRunning", matrixJob.active);
    juce::Array<juce::var> inPk, outPk, gr;
    for (int c = 0; c < st.numIn; ++c) inPk.add(st.inPeak[size_t(c)]);
    // Meters per output (in output order), read from each output's slot.
    for (auto& o : state.outputs) {
        auto it = slotOf.find(o.id);
        const size_t sl = it == slotOf.end() ? 0 : size_t(it->second);
        outPk.add(it == slotOf.end() ? 0.0f : st.outPeak[sl]);
        gr.add(it == slotOf.end() ? 0.0f : st.limGrDb[sl]);
    }
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
    const int mic = state.measure.micSource;
    if (mic < 0 || state.sources[size_t(mic)].channel < 0) return "choose a measurement mic (a patched source) first";
    if (calib.active || sweepJob.active || alignJob.active || matrixJob.active) return "another automatic measurement is running";
    return {};
}

void Dispatcher::applyTransient(Params& p) const
{
    if (calib.active) {
        p.gen.on = true;
        p.gen.mode = GeneratorParams::Pink;
        p.gen.levelDb = -20.0f;
        p.gen.targets = 1ull << calib.output;
        p.measure.micInput = calib.input;
        p.measure.ref = RefSource::Generator;
    }
    if (sweepJob.active && !sweepJob.analysing) {
        p.gen.on = true;
        p.gen.mode = GeneratorParams::Sweep;
        p.gen.sweepId = sweepJob.id;
        p.gen.sweepLength = sweepJob.length;
    }
    if (matrixJob.active && matrixJob.index < matrixJob.groups.size()) {
        p.gen.on = true;
        p.gen.mode = GeneratorParams::Pink;
        p.gen.targets = 0;
        for (int o : state.measure.groups[size_t(matrixJob.groups[matrixJob.index])].outputs)
            if (o >= 0 && o < kMaxCh) p.gen.targets |= 1ull << o;
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
    // The analyzer history must hold sweep + 3 s of tail (10.9 s at 96 kHz).
    sweepJob.seconds = std::clamp(seconds, 1.0, std::min(8.0, double(Analyzer::kHistory) / sampleRate() - 3.5));
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
    if (!sweepJob.analysing && elapsed > sweepJob.seconds + 3.0) {
        // Sweep + 3 s of tail (room decay up to ~2.5 s, latency) is in the analyzer history now.
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
        juce::Array<juce::var> decay;
        for (auto& b : r.decay) {
            auto d = object();
            set(d, "fc", b.fc);
            set(d, "edt", b.edt);
            set(d, "t20", b.t20);
            set(d, "t30", b.t30);
            set(d, "curve", toArr(b.curveDb));
            decay.add(d);
        }
        emit("sweep", { { "ok", true }, { "trace", t }, { "decay", decay } });
        commit();
    } else if (elapsed > sweepJob.seconds + 12) {
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

namespace vt {

bool Dispatcher::replaceState(const SystemState& next, juce::String& err, bool structural)
{
    SystemState checked;
    if (!fromVar(toVar(next), checked, err)) return false;
    state = std::move(checked);
    commit(true);
    return true;
}

int Dispatcher::nextFreeOutputChannel(const SystemState& s) const
{
    const int n = engine.status().numOut;
    for (int c = 0; c < n; ++c) {
        bool used = false;
        for (auto& o : s.outputs) used |= o.channel == c;
        if (!used) return c;
    }
    return -1;
}

juce::var Dispatcher::routing(const juce::String& cmd, const juce::var& args, juce::String& err)
{
    SystemState next = state;
    const int index = args.hasProperty("index") ? int(args["index"]) : -1;
    auto programSources = [&] {
        std::vector<int> p;
        for (size_t i = 0; i < next.sources.size(); ++i)
            if (next.sources[i].type == "program") p.push_back(int(i));
        return p;
    };

    if (cmd == "routing.addSource") {
        if (int(next.sources.size()) >= kMaxSources) { err = "too many sources"; return {}; }
        SourceConfig src;
        src.type = args.hasProperty("type") ? args["type"].toString() : "program";
        src.channel = args.hasProperty("channel") ? int(args["channel"]) : -1;
        src.name = args.hasProperty("name") ? args["name"].toString()
                 : (src.type == "mic" ? "Mic " : src.type == "loopback" ? "Loopback " : "Source ") + juce::String(int(next.sources.size()) + 1);
        next.sources.push_back(src);
        for (auto& o : next.outputs) o.mix.resize(next.sources.size(), 0.0f);
        if (src.type == "mic" && next.measure.micSource < 0) next.measure.micSource = int(next.sources.size()) - 1;
    } else if (cmd == "routing.removeSource") {
        if (index < 0 || index >= int(next.sources.size())) { err = "no such source"; return {}; }
        next.removeSource(index);
    } else if (cmd == "routing.addOutput") {
        const auto preset = args.hasProperty("preset") ? args["preset"].toString() : "single";
        const auto prog = programSources();
        auto make = [&](const juce::String& name, const juce::String& type) {
            OutputConfig o;
            o.name = name;
            o.type = type;
            o.mix.assign(next.sources.size(), 0.0f);
            o.channel = nextFreeOutputChannel(next);
            next.outputs.push_back(o);
            return int(next.outputs.size()) - 1;
        };
        const int room = kMaxOutputs - int(next.outputs.size());
        if (room < (preset == "stereo" ? 2 : 1)) { err = "too many outputs"; return {}; }
        if (preset == "stereo") {
            const int l = make(args.hasProperty("name") ? args["name"].toString() + " L" : "Out L", "main");
            const int r = make(args.hasProperty("name") ? args["name"].toString() + " R" : "Out R", "main");
            if (prog.size() >= 2) { next.outputs[size_t(l)].mix[size_t(prog[0])] = 1.0f; next.outputs[size_t(r)].mix[size_t(prog[1])] = 1.0f; }
        } else if (preset == "sub") {
            const int s = make(args.hasProperty("name") ? args["name"].toString() : "Sub", "sub");
            for (size_t k = 0; k < prog.size() && k < 2; ++k) next.outputs[size_t(s)].mix[size_t(prog[k])] = prog.size() >= 2 ? 0.5f : 1.0f;
            next.outputs[size_t(s)].lp = { "lr24", 100.0f };
        } else {
            make(args.hasProperty("name") ? args["name"].toString() : "Out " + juce::String(int(next.outputs.size()) + 1), "full");
        }
    } else if (cmd == "routing.removeOutput") {
        if (index < 0 || index >= int(next.outputs.size())) { err = "no such output"; return {}; }
        next.removeOutput(index);
    } else if (cmd == "routing.duplicateOutput") {
        if (index < 0 || index >= int(next.outputs.size())) { err = "no such output"; return {}; }
        if (int(next.outputs.size()) >= kMaxOutputs) { err = "too many outputs"; return {}; }
        auto copy = next.outputs[size_t(index)];
        copy.name += " copy";
        copy.id = 0; // gets a fresh id (and slot)
        copy.channel = nextFreeOutputChannel(next);
        next.outputs.insert(next.outputs.begin() + index + 1, copy);
    } else if (cmd == "routing.moveOutput") {
        const int to = int(args["to"]);
        if (index < 0 || index >= int(next.outputs.size()) || to < 0 || to >= int(next.outputs.size())) { err = "no such output"; return {}; }
        auto o = next.outputs[size_t(index)];
        next.outputs.erase(next.outputs.begin() + index);
        next.outputs.insert(next.outputs.begin() + to, o);
        next.gen.targets.clear();
    } else {
        err = "unknown command: " + cmd;
        return {};
    }
    if (!replaceState(next, err, true)) return {};
    return toVar(state);
}

// Snapshots carry the patch. On the device they were made on (or with an explicit
// `remap`) they apply directly; on another interface the reply asks for a remap,
// prefilled: same name -> same channel, else same channel number if it exists.
juce::var Dispatcher::recallSnapshot(const juce::var& args, juce::String& err)
{
    SnapshotData snap;
    if (!snapshots.load(args["name"].toString(), snap, err)) return {};
    const auto st = engine.status();
    SystemState next = state;

    if (!snap.hasPatch) {
        // Version 1: outputs were physical channels, mixes indexed by physical input.
        for (size_t o = 0; o < snap.outputs.size(); ++o) {
            auto& out = snap.outputs[o];
            out.channel = int(o) < st.numOut || st.numOut == 0 ? int(o) : -1;
            std::vector<float> mix(state.sources.size(), 0.0f);
            for (size_t phys = 0; phys < out.mix.size(); ++phys)
                for (size_t s = 0; s < state.sources.size(); ++s)
                    if (state.sources[s].type == "program" && state.sources[s].channel == int(phys)) { mix[s] = out.mix[phys]; break; }
            out.mix = mix;
        }
        next.outputs = snap.outputs;
        if (!replaceState(next, err, true)) return {};
        return toVar(state);
    }

    const bool sameDevice = snap.inputDevice == state.device.inputDevice && snap.outputDevice == state.device.outputDevice;
    const auto& remap = args["remap"];
    if (!sameDevice && !remap.isObject()) {
        auto prefill = [](const juce::String& name, int ch, int count, auto& current) {
            for (auto& c : current)
                if (c.name == name) return c.channel;
            return ch < count ? ch : -1;
        };
        juce::Array<juce::var> srcs, outs;
        for (auto& s : snap.sources) {
            auto x = juce::var(new juce::DynamicObject());
            x.getDynamicObject()->setProperty("name", s.name);
            x.getDynamicObject()->setProperty("type", s.type);
            x.getDynamicObject()->setProperty("was", s.channel);
            x.getDynamicObject()->setProperty("channel", prefill(s.name, s.channel, st.numIn, state.sources));
            srcs.add(x);
        }
        std::vector<int> used;
        for (auto& o : snap.outputs) {
            int ch = prefill(o.name, o.channel, st.numOut, state.outputs);
            if (std::find(used.begin(), used.end(), ch) != used.end()) ch = -1;
            if (ch >= 0) used.push_back(ch);
            auto x = juce::var(new juce::DynamicObject());
            x.getDynamicObject()->setProperty("name", o.name);
            x.getDynamicObject()->setProperty("was", o.channel);
            x.getDynamicObject()->setProperty("channel", ch);
            outs.add(x);
        }
        auto r = juce::var(new juce::DynamicObject());
        r.getDynamicObject()->setProperty("needsRemap", true);
        r.getDynamicObject()->setProperty("name", args["name"]);
        r.getDynamicObject()->setProperty("fromInput", snap.inputDevice);
        r.getDynamicObject()->setProperty("fromOutput", snap.outputDevice);
        r.getDynamicObject()->setProperty("sources", srcs);
        r.getDynamicObject()->setProperty("outputs", outs);
        return r;
    }
    if (remap.isObject()) {
        const auto* rs = remap["sources"].getArray();
        const auto* ro = remap["outputs"].getArray();
        for (size_t i = 0; rs && i < snap.sources.size() && int(i) < rs->size(); ++i) snap.sources[i].channel = int((*rs)[int(i)]);
        for (size_t i = 0; ro && i < snap.outputs.size() && int(i) < ro->size(); ++i) snap.outputs[i].channel = int((*ro)[int(i)]);
    }
    next.sources = snap.sources;
    next.outputs = snap.outputs;
    if (next.measure.micSource >= int(next.sources.size())) next.measure.micSource = -1;
    if (next.measure.refSource >= int(next.sources.size())) { next.measure.refSource = -1; next.measure.ref = "generator"; }
    if (!replaceState(next, err, true)) return {};
    return toVar(state);
}

} // namespace vt

namespace vt {

std::pair<double, bool> Dispatcher::splOffset() const
{
    const int mic = state.measure.micSource;
    if (mic < 0 || mic >= int(state.sources.size())) return { 0.0, false };
    auto it = state.cal.splOffset.find(state.sources[size_t(mic)].name);
    return it == state.cal.splOffset.end() ? std::pair { 0.0, false } : std::pair { it->second, true };
}

} // namespace vt

namespace vt {

void Dispatcher::recordUndo(const juce::String& key)
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const bool merge = key.isNotEmpty() && key == lastUndoKey && now - lastUndoMs < 1500.0;
    lastUndoKey = key;
    lastUndoMs = now;
    if (merge) return; // same field still being dragged: keep the state from before the drag
    undoStack.push_back({ state.sources, state.outputs });
    if (undoStack.size() > 100) undoStack.erase(undoStack.begin());
    redoStack.clear();
}

juce::var Dispatcher::stepHistory(bool undo, juce::String& err)
{
    auto& from = undo ? undoStack : redoStack;
    auto& to = undo ? redoStack : undoStack;
    if (from.empty()) { err = undo ? "nothing to undo" : "nothing to redo"; return {}; }
    to.push_back({ state.sources, state.outputs });
    SystemState next = state; // generator, measurement, device and calibration stay as they are
    next.sources = from.back().sources;
    next.outputs = from.back().outputs;
    from.pop_back();
    forceNewUndoStep();
    if (!replaceState(next, err, true)) return {};
    return toVar(state);
}

} // namespace vt

namespace vt {

juce::var Dispatcher::traceVar(const Analyzer::Result& r, const juce::String& name) const
{
    auto toArr = [](const std::vector<float>& v) { juce::Array<juce::var> a; for (float x : v) a.add(x); return juce::var(a); };
    auto t = object();
    set(t, "name", name);
    set(t, "freq", toArr(r.freq));
    set(t, "mag", toArr(r.magDb));
    set(t, "phase", toArr(r.phaseDeg));
    set(t, "coh", toArr(r.coherence));
    set(t, "delaySamples", r.delaySamples);
    set(t, "sampleRate", r.sampleRate);
    return t;
}

// Frequency range a group actually covers, from its outputs' crossovers: a sub is judged on
// its own band, not on 10 kHz where it (correctly) has no coherence.
std::pair<double, double> Dispatcher::groupBand(const MeasureGroup& g) const
{
    double lo = 1e9, hi = 0;
    for (int o : g.outputs) {
        if (o < 0 || o >= int(state.outputs.size())) continue;
        const auto& out = state.outputs[size_t(o)];
        lo = std::min(lo, out.hp.type != "off" ? double(out.hp.freq) : 40.0);
        hi = std::max(hi, out.lp.type != "off" ? double(out.lp.freq) : 12000.0);
    }
    if (hi <= 0) return { 100.0, 10000.0 };
    return { std::max(25.0, lo * 1.2), std::max(lo * 1.5, hi * 0.8) };
}

juce::var Dispatcher::startMatrix(const juce::var& args, juce::String& err)
{
    if (err = preflight(); err.isNotEmpty()) return {};
    const int pos = int(args["position"]);
    if (pos < 0 || pos >= int(state.measure.positions.size())) { err = "choose a mic position"; return {}; }
    std::vector<int> groups;
    if (const auto* a = args["groups"].getArray()) for (auto& g : *a) groups.push_back(int(g));
    else for (int g = 0; g < int(state.measure.groups.size()); ++g) groups.push_back(g);
    std::vector<int> valid;
    for (int g : groups)
        if (g >= 0 && g < int(state.measure.groups.size()) && !state.measure.groups[size_t(g)].outputs.empty()) valid.push_back(g);
    if (valid.empty()) { err = "no measurement groups with outputs: create them first"; return {}; }
    matrixJob = {};
    matrixJob.active = true;
    matrixJob.position = pos;
    matrixJob.groups = valid;
    engine.analyzer().reset();
    commit();
    emit("matrix", { { "stage", "measuring" }, { "group", valid[0] }, { "position", pos } });
    return "started";
}

void Dispatcher::tickMatrix()
{
    auto& j = matrixJob;
    ++j.ticks;
    const auto r = engine.analyzer().result();
    if (r.delayValid && j.ticks > 4) {
        j.stable = std::abs(r.delaySamples - j.last) < 0.5 ? j.stable + 1 : 0;
        j.last = r.delaySamples;
        if (j.stable >= 3 && j.settledTicks < 0) j.settledTicks = j.ticks; // arrival stable: start averaging
    }
    const int group = j.groups[j.index];
    const auto& g = state.measure.groups[size_t(group)];
    const bool averaged = j.settledTicks >= 0 && j.ticks - j.settledTicks >= 12; // + 3 s of averaging
    const bool timedOut = j.ticks > 72;                                           // 18 s per attempt
    if (!averaged && !timedOut) return;

    const auto [lo, hi] = groupBand(g);
    const double q = captureQuality(r, lo, hi);
    const bool ok = averaged && captureAccepted(r, lo, hi);
    auto next = [&] {
        j.ticks = j.stable = 0;
        j.settledTicks = -1;
        j.last = -1;
        engine.analyzer().reset();
    };
    if (!ok && j.attempt == 0) { // disturbed or no arrival: one more try
        j.attempt = 1;
        emit("matrix", { { "stage", "retry" }, { "group", group }, { "position", j.position }, { "quality", q } });
        next();
        return;
    }
    const auto name = g.name + " @ " + state.measure.positions[size_t(j.position)];
    if (ok) emit("matrix", { { "stage", "captured" }, { "group", group }, { "position", j.position }, { "quality", q }, { "trace", traceVar(r, name) } });
    else emit("matrix", { { "stage", "rejected" }, { "group", group }, { "position", j.position }, { "quality", q },
                          { "message", r.delayValid ? "low coherence: noise in the room or source too quiet" : "no clear arrival: check the mic and the speaker" } });
    j.attempt = 0;
    if (++j.index >= j.groups.size()) { finishMatrix({}); return; }
    next();
    commit(); // generator moves to the next group
    emit("matrix", { { "stage", "measuring" }, { "group", j.groups[j.index] }, { "position", j.position } });
}

void Dispatcher::finishMatrix(const juce::String& error)
{
    matrixJob.active = false;
    engine.analyzer().reset();
    commit();
    emit("matrix", { { "stage", error.isEmpty() ? "done" : "error" }, { "message", error } });
}

} // namespace vt

namespace vt {

juce::var Dispatcher::presetCommand(const juce::String& cmd, const juce::var& args, juce::String& err)
{
    const auto name = args["name"].toString();
    if (cmd == "preset.list") { juce::Array<juce::var> a; for (auto& n : presets.list()) a.add(n); return a; }
    if (cmd == "preset.delete") { if (!presets.remove(name)) { err = "could not delete preset"; return {}; } return true; }
    if (cmd == "preset.export") { auto j = presets.exportJson(name, err); if (err.isNotEmpty()) return {}; return j; }
    if (cmd == "preset.import") { if (!presets.importJson(name, args["json"].toString(), err)) return {}; return true; }

    if (cmd == "preset.save") {
        const int o = int(args["output"]);
        if (o < 0 || o >= int(state.outputs.size())) { err = "invalid output"; return {}; }
        SpeakerPreset p;
        p.name = name;
        p.processing = state.outputs[size_t(o)];
        if (p.processing.fir.taps > 0 && p.processing.fir.path.isNotEmpty()) { // embed the FIR itself
            juce::AudioBuffer<float> ir;
            double fileSr = 0;
            if (!FirLoader::load(juce::File(p.processing.fir.path), sampleRate(), ir, fileSr, err)) return {};
            p.fir.assign(ir.getReadPointer(0), ir.getReadPointer(0) + ir.getNumSamples());
            p.firSampleRate = fileSr;
        }
        if (!presets.save(p, err)) return {};
        state.outputs[size_t(o)].preset = name;
        commit();
        return true;
    }

    if (cmd == "preset.apply") {
        SpeakerPreset p;
        if (!presets.load(name, p, err)) return {};
        std::vector<int> outs;
        if (const auto* a = args["outputs"].getArray()) for (auto& v : *a) outs.push_back(int(v));
        for (int o : outs) if (o < 0 || o >= int(state.outputs.size())) { err = "invalid output"; return {}; }
        const auto firPath = presets.materialiseFir(p, err);
        if (err.isNotEmpty()) return {};
        recordUndo("preset");
        forceNewUndoStep();
        SystemState next = state;
        for (int o : outs) {
            auto& out = next.outputs[size_t(o)];
            Presets::applySpeakerFields(p.processing, out);
            out.preset = p.name;
            out.fir = {};
            if (firPath.isNotEmpty()) { out.fir.path = firPath; out.fir.on = p.processing.fir.on; }
        }
        if (!replaceState(next, err, false)) return {};
        // replaceState loads new FIR paths; mark them on, with taps/latency now known.
        for (int o : outs)
            if (firPath.isNotEmpty() && !loadFirFile(o, firPath, err)) return {};
        commit();
        return toVar(state);
    }
    err = "unknown command: " + cmd;
    return {};
}

} // namespace vt
