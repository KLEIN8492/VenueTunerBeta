# VenueTuner Milestone 1 (Walking Skeleton) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A working desktop app on macOS that passes console audio through per-output gain / polarity / delay / limiter / mute, generates pink noise, and measures transfer function, coherence and impulse response against a software or physical loopback reference.

**Architecture:** Headless C++20/JUCE engine (audio thread + analysis thread + JUCE message thread as control thread) exposing a WebSocket JSON/binary protocol on localhost. Electron shell spawns and supervises the engine and hosts three windows (Measurement, Live/Tuning, Settings) built with Vite + TypeScript and canvas 2D.

**Tech Stack:** C++20, JUCE 8.0.4 (core, events, audio_basics, audio_devices, dsp), IXWebSocket 11.4.5, Catch2 3.7.1, CMake ≥ 3.22, TypeScript 5, Vite 5, Vitest 2, Electron 33.

**Spec:** `docs/superpowers/specs/2026-10-08-venuetuner-design.md`

## Global Constraints

- macOS arm64 for M1; engine code must not use GUI modules (portable to Linux later).
- Audio thread: no allocation, no locks, no I/O, no logging.
- Default sample rate 48 kHz, default buffer 64; IIR/gain path adds 0 samples.
- Outputs start muted on launch, on device reconnect, after engine restart.
- Generator starts at −40 dBFS, fades in, hard cap −6 dBFS.
- Max delay 1 s; delay changes crossfade (no clicks); gain ramps ~20 ms.
- Max channels handled: 64 in / 64 out.
- UI style: flat dark grey, monospace numerics, no rounded corners, no animation; red only for mute/clip/errors.
- Protocol: WebSocket, JSON text commands `{id, cmd, args}` → `{id, ok, result|error}`; events `{event, ...}`; binary frames for spectra / IR.

## Deliberate M1 simplifications (spec items deferred, by design)

- Transfer function uses a single 16384-point Welch FFT + log-frequency fractional-octave smoothing; the multi-time-window FFT arrives in M2 with PEQ.
- Log sweep, SPL calibration, trace import, spatial averaging UI: M2.
- Snapshots contain routing, gains, polarity, delays, mutes, limiter settings (no EQ yet).

## File Structure

```
engine/
  CMakeLists.txt                 build: engine lib, executable, tests
  src/rt/TripleBuffer.h          lock-free latest-value exchange (control → audio)
  src/rt/SpscFifo.h              lock-free single-producer/consumer sample FIFO
  src/dsp/PinkNoise.h            pink noise generator (Kellet filter)
  src/dsp/DelayLine.h            delay line with crossfaded delay changes
  src/dsp/Limiter.h              peak limiter
  src/dsp/OutputChain.h          per-output chain: gain→pol→delay→lim→mute
  src/core/Params.h              POD processing params (what the audio thread reads)
  src/core/SystemState.h/.cpp    full state + JSON (de)serialisation + validation
  src/core/AudioEngine.h/.cpp    device management + audio callback
  src/measure/Analyzer.h/.cpp    Welch TF, coherence, IR, delay finder, log smoothing
  src/measure/MicCal.h/.cpp      mic calibration file parser + interpolation
  src/server/Server.h/.cpp       WebSocket server, command dispatch, broadcasting
  src/server/Snapshots.h/.cpp    snapshot store + autosave on disk
  src/main.cpp                   process entry, signal handling, message loop
  tests/*.cpp                    Catch2 tests per unit
ui/
  package.json, vite.config.ts, tsconfig.json
  measurement.html, live.html, settings.html
  src/lib/engine.ts              WS client: reconnect, request/reply, events, binary decode
  src/lib/plot.ts                log-frequency canvas plot
  src/lib/scale.ts               pure axis math (tested)
  src/lib/statusbar.ts           shared status bar
  src/lib/style.css              raw theme
  src/measurement.ts, live.ts, settings.ts
  src/lib/*.test.ts              vitest
app/
  package.json
  main.js                        Electron: spawn/supervise engine, windows, menu
  preload.js                     exposes engine port to windows
```

---

### Task 1: Engine build skeleton + RT primitives

**Files:** Create `engine/CMakeLists.txt`, `engine/src/rt/TripleBuffer.h`, `engine/src/rt/SpscFifo.h`, `engine/tests/test_rt.cpp`, `.gitignore`

**Interfaces — Produces:**
- `template<class T> class TripleBuffer { void write(const T&); /*control*/ const T& read(); /*audio: returns latest*/ }`
- `template<class T> class SpscFifo { explicit SpscFifo(size_t capacityPow2); size_t push(const T*, size_t n); size_t pop(T*, size_t n); size_t available() const; }`

- [ ] Write tests: TripleBuffer read returns last written value; reading twice without write returns same value; SpscFifo push/pop preserves order across wraparound; push returns fewer than n when full; threaded test: producer pushes 1e6 increasing ints, consumer checks order.
- [ ] Configure: `cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release` (FetchContent JUCE, Catch2, IXWebSocket); build; tests fail to compile.
- [ ] Implement both headers with `std::atomic` and acquire/release ordering.
- [ ] `ctest --test-dir engine/build --output-on-failure` → PASS. Commit.

### Task 2: DSP blocks

**Files:** Create `engine/src/dsp/PinkNoise.h`, `DelayLine.h`, `Limiter.h`, `OutputChain.h`, `engine/src/core/Params.h`; tests `engine/tests/test_dsp.cpp`

**Interfaces — Produces:**
- `PinkNoise { void setSeed(uint32_t); float next(); }` — unit RMS ≈ 0.25 scaled output
- `DelayLine { void prepare(double sr, int maxSamples); void setDelaySamples(int d); float process(float x); }` — on change, crossfades old→new tap over 50 ms
- `Limiter { void prepare(double sr); void set(float thresholdDb, float attackMs, float releaseMs, bool enabled); float process(float x); float gainReductionDb() const; }`
- `struct OutputParams { float gainDb; bool polarityInvert; float delayMs; bool mute; bool limiterOn; float limThreshDb, limAttackMs, limReleaseMs; float mix[kMaxCh]; bool genTarget; }`
- `struct Params { int numOut, numIn; OutputParams out[kMaxCh]; GeneratorParams gen; bool muteAll; }` with `kMaxCh = 64`
- `OutputChain { void prepare(double sr, int maxBlock); void process(const OutputParams&, float* buf, int n, bool muteAll); float peak(); }`

- [ ] Tests: pink noise octave-band energy slope −3 dB/oct ±1 dB from 63 Hz to 8 kHz; DelayLine integer delay is sample-exact for an impulse after crossfade settles; delay change produces no sample jump > 0.1 for a 100 Hz sine at 0.5 amplitude; limiter with 0 ms attack never exceeds threshold (+0.01 dB) on a +12 dB sine; OutputChain mute converges to exact 0 within 30 ms; polarity flips sign; gain −6 dB ≈ 0.501.
- [ ] Run → FAIL. Implement. Run → PASS. Commit.

### Task 3: Analyzer (transfer function, coherence, IR, delay finder)

**Files:** Create `engine/src/measure/Analyzer.h/.cpp`, `engine/src/measure/MicCal.h/.cpp`; tests `engine/tests/test_analyzer.cpp`

**Interfaces — Produces:**
- `Analyzer { void prepare(double sr); void pushRef(const float*, int); void pushMeas(const float*, int); bool process(); /*called on analysis thread; true when new result*/ void reset(); void setAveraging(int n); Result result() const; double delaySamples() const; }`
- `struct Result { std::vector<float> freq, magDb, phaseDeg, coherence; std::vector<float> ir; double delaySamples; float sampleRate; }` with log-spaced 1/24-oct display points 20 Hz–20 kHz (~240 points) and smoothing selectable (`setSmoothing(int fractionDenom)`, 0 = none)
- Delay finder: GCC-PHAT-free plain cross-spectrum IFFT over the last 65536 samples, peak with parabolic interpolation; recomputed every 1 s; TF uses reference delayed by the integer part.
- `MicCal { bool load(const juce::File&, juce::String& err); float offsetDb(float hz) const; }` (log-frequency linear interpolation; flat outside range)

- [ ] Tests (synthetic room): ref = pink noise; meas = ref delayed 480 samples × 0.5 → delay 480 ± 0.1, magnitude −6.02 ± 0.2 dB over 50 Hz–15 kHz, phase ≈ 0 ± 5° after delay compensation, coherence > 0.98. Fractional 480.5-sample delay (windowed-sinc) → 480.5 ± 0.1. Adding uncorrelated noise at equal level → mean coherence drops below 0.7. Meas = ref through a one-pole lowpass at 1 kHz → −3 dB ± 0.5 at 1 kHz. MicCal parses REW-style file with header lines and quotes, interpolates.
- [ ] Run → FAIL. Implement. Run → PASS. Commit.

### Task 4: SystemState + JSON + validation + snapshots

**Files:** Create `engine/src/core/SystemState.h/.cpp`, `engine/src/server/Snapshots.h/.cpp`; tests `engine/tests/test_state.cpp`

**Interfaces — Produces:**
- `struct SystemState { DeviceConfig device; std::vector<InputRole> inputs; std::vector<OutputConfig> outputs; GeneratorConfig gen; MeasureConfig measure; CalibrationData cal; }`
- `juce::var toVar(const SystemState&)`, `bool fromVar(const juce::var&, SystemState&, juce::String& err)` (all-or-nothing)
- `bool applyPatch(SystemState&, const juce::String& path, const juce::var& value, juce::String& err)` — paths like `outputs/2/gainDb`; validates ranges (gain −80…+12 dB, delay 0…1000 ms, limiter threshold −40…0 dBFS, generator level ≤ −6 dBFS)
- `Params toParams(const SystemState&)`
- `Snapshots { explicit Snapshots(juce::File dir); juce::StringArray list(); bool save(name, const SystemState&); bool load(name, SystemState& out, juce::String& err); bool remove(name); void autosave(const SystemState&); bool loadAutosave(SystemState&); }` — writes to temp file then renames (atomic)

- [ ] Tests: round-trip toVar/fromVar equality; patch valid path changes value; out-of-range value rejected and state unchanged; unknown path rejected; corrupt JSON snapshot load fails and leaves output untouched; generator level above cap rejected.
- [ ] Run → FAIL. Implement. Run → PASS. Commit.

### Task 5: AudioEngine (device + callback)

**Files:** Create `engine/src/core/AudioEngine.h/.cpp`; test `engine/tests/test_engine.cpp`

**Interfaces — Consumes:** Tasks 1–4. **Produces:**
- `AudioEngine : juce::AudioIODeviceCallback, juce::ChangeListener { juce::String openDevice(const DeviceConfig&); void close(); void setParams(const Params&); void setMuteAll(bool); void unmuteAfterFault(); Analyzer& analyzer(); Status status() const; juce::var listDevices(); /* for tests: */ void processBlock(const float* const* in, int nIn, float* const* out, int nOut, int n); }`
- `struct Status { juce::String inDevice, outDevice; double sr; int buffer; double cpu; int xruns; double roundTripMs; bool faultMuted; float inPeak[64], outPeak[64], limGr[64]; }`
- Measurement routing: `MeasureConfig { int micInput; RefSource ref /*Generator, PhysicalInput*/; int refInput; }`; generator signal is pushed as reference for Generator mode.
- I/O latency used for acoustic delay = `cal` override for this device/sr/buffer if present, else driver-reported `getInputLatencyInSamples() + getOutputLatencyInSamples()`.
- Device fault (removed / restarted): `faultMuted = true` — outputs forced silent until `unmuteAfterFault()`.

- [ ] Tests using `processBlock` directly (no device): console input 0 routed to output 1 with mix 1.0 appears at output after unmute; faultMuted → silence; generator on output 0 only, level −20 dBFS → RMS within 1 dB of expected pink level after fade; mic + reference pushed to analyzer (delay finder recovers a synthetic 100-sample loop when output 0 is fed back to input 1 in the test harness).
- [ ] Run → FAIL. Implement. Run → PASS. Commit.

### Task 6: Server + main executable

**Files:** Create `engine/src/server/Server.h/.cpp`, `engine/src/main.cpp`; test `engine/tests/test_server.cpp` (dispatch only, no sockets)

**Commands:** `state.get`, `state.patch {path, value}`, `devices.list`, `device.open {device config}`, `mute.all {on}`, `unmute.fault`, `generator.set {on, level, targets}`, `measure.reset`, `measure.capture {name}` (returns trace), `measure.setSmoothing {n}`, `measure.setAveraging {n}`, `calibrate.latency {output, input}` (starts loopback measurement; result stored in `cal.ioLatencySamples` keyed by device/sr/buffer), `cal.loadMic {path}`, `snapshot.list|save|recall|delete {name}`.

**Events:** `state` (full state after every change), `status` (10 Hz: meters, cpu, xruns, latency, faultMuted), binary `TF` frames (type 1: n, freq[], mag[], phase[], coh[]; 15 Hz), binary `IR` frames (type 2: n, sr, delay, samples[]; 2 Hz).

- [ ] Tests: `Dispatcher::handle(json)` returns ok for `state.get`; `state.patch` invalid → `ok:false` with message; `snapshot.save` then `snapshot.recall` restores a changed gain; unknown cmd → error.
- [ ] Implement server with IXWebSocket on 127.0.0.1:`--port` (default 47800); all commands marshalled to the message thread via `MessageManager::callAsync`.
- [ ] main: parse `--port`, `--data-dir`; load autosave; open device from state (outputs muted); run dispatch loop; SIGTERM/SIGINT → autosave + quit.
- [ ] Build; run `VenueTunerEngine --port 47800` manually; `node -e` websocket smoke test `state.get`. Commit.

### Task 7: UI foundation (engine client, plot, theme, status bar)

**Files:** Create `ui/package.json`, `ui/vite.config.ts`, `ui/tsconfig.json`, `ui/src/lib/engine.ts`, `ui/src/lib/scale.ts`, `ui/src/lib/plot.ts`, `ui/src/lib/statusbar.ts`, `ui/src/lib/style.css`; tests `ui/src/lib/scale.test.ts`, `ui/src/lib/engine.test.ts`

**Interfaces — Produces:**
- `class Engine { constructor(url: string); request<T>(cmd: string, args?: object): Promise<T>; on(event: 'state'|'status'|'tf'|'ir'|'open'|'close', cb); }` — auto-reconnect every 1 s
- `decodeBinary(buf: ArrayBuffer): {type:'tf', freq, mag, phase, coh} | {type:'ir', sr, delay, samples}`
- `logX(f, fMin, fMax, width)`, `invLogX(...)`, `linY(v, vMin, vMax, height)`, `freqTicks(fMin,fMax)`
- `class Plot { constructor(canvas, opts: {yMin, yMax, yLabel, logX?:boolean}); setTraces(traces: Trace[]); draw(); }`
- `mountStatusBar(el, engine)`

- [ ] Tests: logX(20)=0, logX(20000)=width, logX(632.5)≈width/2; invLogX inverse; decodeBinary decodes a hand-built TF frame; freqTicks includes 100, 1k, 10k.
- [ ] `npm test` → FAIL; implement; PASS. Commit.

### Task 8: Windows — Measurement, Live/Tuning, Settings

**Files:** Create `ui/measurement.html`, `ui/live.html`, `ui/settings.html`, `ui/src/measurement.ts`, `ui/src/live.ts`, `ui/src/settings.ts`

- Measurement: magnitude / phase / coherence stacked plots, IR strip with delay readout (total, acoustic = total − calibrated I/O latency, in ms / samples / metres at 343 m/s), "apply delay to output" select+button, trace list (capture, show/hide, rename, delete, export CSV), generator panel (on/off, level, targets), mic & reference selector, averaging & smoothing selectors.
- Live: output strips (name, meter, gain, mute, polarity, delay ms, limiter on/threshold, GR meter, latency budget), MUTE ALL, fault banner with "UNMUTE OUTPUTS", snapshots panel (save/recall/delete/export/import).
- Settings: input device, output device, sample rate, buffer; input roles; routing matrix (console inputs × outputs, dB values); output names/colours; latency calibration (pick output + input, run, shows result); mic cal file path load.
- [ ] `npm run build` succeeds (type-check). Manual check in browser via `npx vite` against running engine. Commit.

### Task 9: Electron shell

**Files:** Create `app/package.json`, `app/main.js`, `app/preload.js`, root `package.json` scripts

- Spawns engine binary (`engine/build/VenueTunerEngine_artefacts/Release/VenueTunerEngine`), port 47800, data dir `~/Library/Application Support/VenueTuner`; restarts on exit (backoff 1 s, max 5/min) and sends `engine-restarted` to windows.
- Windows: Measurement (⌘1), Live/Tuning (⌘2), Settings (⌘,) — single instance each, loads `ui/dist/*.html`.
- Menu: App (About, Settings ⌘,, Quit), Window (Measurement ⌘1, Live ⌘2), View (Reload, DevTools).
- On quit: SIGTERM engine (autosave), wait 2 s.
- [ ] `npm run app` from root opens both windows and engine. Commit.

### Task 10: End-to-end verification

- [ ] All engine and UI tests pass.
- [ ] With BlackHole 16ch as in+out device: generator → out 1; mic input = BlackHole in 1 (loopback) → delay finder reports the BlackHole buffer latency; magnitude flat ±0.5 dB, coherence ≈ 1.
- [ ] Latency calibration on BlackHole stores value; acoustic delay then reads ≈ 0.
- [ ] Kill engine process → Electron restarts it, outputs muted, state restored.
- [ ] Snapshot save / change gain / recall restores gain.
- [ ] Manual (user, real hardware): interface + mic vs REW comparison.
