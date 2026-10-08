# VenueTuner Milestone 2 (Processing) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the processor's filtering (PEQ, crossovers, all-pass, FIR) with an interactive EQ editor and "predicted after EQ", plus multi-time-window FFT, log-sweep capture and automatic delay alignment.

**Architecture:** Filter design (coefficients) happens on the control thread in `SystemState → Params`; the audio thread runs up to 28 TDF-II double biquads per output with per-sample coefficient ramps, then an optional JUCE non-uniform partitioned convolution. The UI mirrors the biquad math in TypeScript to draw curves instantly.

**Tech Stack:** as M1 + `juce_audio_formats` (WAV import).

**Spec:** `docs/superpowers/specs/2026-10-08-venuetuner-design.md` (sections 4, 5, 6, 7; milestone 2)

## Global Constraints

- All M1 constraints still apply (no allocation/locks on the audio thread, outputs muted on faults, etc.).
- Chain order per output: gain·pol → delay → PEQ(16) → HP → LP → AP(4) → FIR → limiter → mute.
- IIR adds 0 samples latency; FIR has zero *processing* latency (non-uniform partitioning); its own latency (IR peak index) is shown.
- Biquads: Transposed Direct Form II, double precision; coefficient changes ramp linearly over 256 samples (stable: the (a1,a2) stability triangle is convex).
- Frequencies are clamped to ≤ 0.49·fs at design time.
- Ranges: freq 10–24000 Hz; PEQ gain −30…+15 dB; Q 0.1–30; all-pass Q 0.1–10.
- Automatic features write ordinary parameters and save "before"/"after" snapshots.

## File Structure

```
engine/src/dsp/Biquad.h              coefficient struct, RBJ designs, response(), TDF-II section
engine/src/dsp/FilterDesign.h/.cpp   crossover (BW/LR) + full output IIR list from config
engine/src/dsp/OutputChain.h         + IIR bank with coefficient ramps
engine/src/dsp/FirLoader.h/.cpp      WAV / text IR loading, peak (latency) detection
engine/src/core/Params.h             + iir[28], numIir, firOn
engine/src/core/SystemState.*        + peq/hp/lp/ap/fir config, validation, toParams(state, sr)
engine/src/core/AudioEngine.*        + per-output juce::dsp::Convolution, sweep generator
engine/src/measure/Analyzer.*        + multi-time-window bands, sweep analysis
engine/src/measure/Align.h           pure alignment math
engine/src/server/Dispatcher.*       + fir.load/clear, measure.sweep, align.auto state machines
ui/src/lib/filters.ts                biquad design + response (mirror of Biquad.h)
ui/src/lib/eqeditor.ts               interactive EQ graph + band/XO/AP/FIR panels
ui/src/live.ts / live.html           editor panel under the strips
ui/src/measurement.ts                MTW switch, sweep button, auto-align panel, traces keep outputs-at-capture
```

---

### Task 1: Biquad designs + filter design (engine)
**Produces:** `struct Biquad { double b0,b1,b2,a1,a2; static Biquad identity(); bool isIdentity() const; std::complex<double> response(double f, double sr) const; }`; designs `bell, lowShelf, highShelf, notch, lowpass2, highpass2, lowpass1, highpass1, allpass1, allpass2 (f, q, gainDb, sr)`; `std::vector<Biquad> crossover(XoType, bool highpass, double f, double sr)` for BW6/12/18/24/36/48, LR12/24/48; `int designOutputIir(const OutputConfig&, double sr, Biquad* out /*28*/)`.
- [x] Tests: bell gain at fc = gainDb ±0.01; shelves reach gain at the far end ±0.1 and 0 dB at the other; HP/LP BW12 −3.01 dB at fc; BW24 −3.01 at fc; LR24 HP and LP each −6.02 at fc and |HP+LP| = 0 dB ±0.01 from 20 Hz–20 kHz; LR48 likewise; all-pass magnitude 0 dB ±1e-9 and phase −180° at fc (2nd order); freq above 0.49 fs is clamped without NaN.
- [x] Implement; tests pass; commit.

### Task 2: IIR in the output chain
**Produces:** `OutputParams::iir[kMaxIir=28]`, `numIir`; `OutputChain` processes active sections with 256-sample linear coefficient ramps.
- [x] Tests: 1 kHz sine through +6 dB bell at 1 kHz → amplitude ×1.995 ±0.5 %; changing bell 0→+12 dB mid-stream: max sample-to-sample jump < 0.05 for a 0.5-amplitude 100 Hz sine; identity sections are skipped (output bit-exact with/without 28 identity sections).
- [x] Implement; commit.

### Task 3: State + validation for PEQ / XO / AP / FIR
**Produces:** `PeqBand{on,type,freq,gainDb,q}`, `XoConfig{type,freq}`, `ApBand{on,order,freq,q}`, `FirConfig{on,path,taps,latencySamples}` in `OutputConfig`; JSON + ranges; `toParams(state, sampleRate)` fills `iir`.
- [x] Tests: JSON round-trip including new fields; out-of-range freq/Q/gain rejected; >16 PEQ bands rejected; unknown type rejected; old M1 snapshot (no peq field) still loads.
- [x] Implement; commit.

### Task 4: FIR (loading + convolution)
**Produces:** `FirLoader::load(File, double sr, juce::AudioBuffer<float>& out, double& fileSr, String& err)` (WAV mono/first channel, or text: one coefficient per line, `*`/`#` comments); `int peakIndex(const AudioBuffer<float>&)`; `AudioEngine::loadFir(int out, AudioBuffer<float>&&, double sr)`, `clearFir(int out)`.
- [x] Tests: text and WAV parse; a delta at index 10 delays a test signal by exactly 10 samples through the engine; random 300-tap IR output equals direct convolution (max error < 1e-4); peakIndex of a symmetric 255-tap sinc = 127.
- [x] Implement with `juce::dsp::Convolution(NonUniform{256}, sharedQueue)`; commit.

### Task 5: Multi-time-window analyzer
- Bands: N=32768 (< 200 Hz), 8192 (200–800 Hz), 2048 (800–3200 Hz), 512 (> 3200 Hz); each Welch 50 % overlap; averaging frames scaled by 32768/N (cap 1024) so all bands average the same time span. `setMultiWindow(bool)` (false = single 16384 window, M1 behaviour). Default on.
- [x] Tests: all M1 analyzer tests pass in both modes; reflection test: direct + 0.7× reflection 50 ms later → MTW magnitude ripple between 5 and 15 kHz < 1.5 dB p-p, while fixed-16k ripple > 6 dB p-p.
- [x] Implement; commit.

### Task 6: Log sweep capture
- Generator `signal = sweep`: exponential sine 20 Hz–20 kHz, `sweepSeconds` 1–8, 10 ms fades, plays once then generator turns itself off; level = RMS dBFS (peak +3 dB).
- `Analyzer::requestSweep(int64 lengthSamples)` → analysis thread deconvolves the last (length + 1 s tail) samples of mic vs reference (regularised H = Y·X*/(|X|²+ε)) → full-resolution trace (1/48-oct points, smoothing applied), IR, delay.
- Dispatcher `measure.sweep {seconds}` → plays, waits, returns event `sweep` with a trace.
- [x] Tests: synthetic room via sweep (delay 480, gain 0.5) → delay 480 ±0.1, magnitude −6.02 ±0.3 dB 30 Hz–16 kHz.
- [x] Implement; commit.

### Task 7: Automatic delay alignment
- Pure function `computeAlignment(std::vector<Arrival{output, arrivalMs, currentDelayMs}>, int referenceOutput /*-1 = latest*/)` → new delays (never negative; reference keeps its delay when given).
- Dispatcher `align.auto {outputs:[..], reference}`: saves snapshot "Before auto-align HH:MM:SS", for each output plays pink noise on it alone until the delay is stable (3 × 250 ms within 0.05 samples, timeout 8 s), collects arrivals, applies delays, saves "Auto-align HH:MM:SS", emits `align` events (progress, result, errors). Restores generator/measure config afterwards. Refuses when outputs are muted.
- [x] Tests: computeAlignment for latest-arrival and explicit-reference cases.
- [x] Implement; commit.

### Task 8: UI filter math
- `ui/src/lib/filters.ts`: mirror of Biquad designs + crossover + `outputResponse(cfg, sr, freqs) → {magDb[], phaseDeg[]}`.
- [x] Tests: same analytic checks as Task 1 (bell at fc, LR24 sum flat, all-pass flat); freq clamp.
- [x] Implement; commit.

### Task 9: EQ editor in the Live window
- Selecting a strip opens the editor under the strips: magnitude + phase graph of the output's filters, base trace selector (captured traces from the Measurement window, shared via localStorage), predicted = base + (curve_now − curve_at_capture) using the outputs saved with the trace.
- Bands: drag on graph (freq/gain), wheel = Q, double-click empty area adds a bell, Alt-click deletes; band table with on/type/freq/gain/Q; HP/LP selects; all-pass rows; FIR row (enable, load file, taps, latency).
- Edits draw instantly and patch the engine at most every 40 ms.
- [x] `npm run build` passes; manual check in browser against the engine on BlackHole: dragging a band changes the live TF accordingly.
- [x] Commit.

### Task 10: Measurement window additions + verification
- FFT mode switch (MTW / fixed), sweep button + seconds, auto-align panel (choose outputs + reference), traces store `outputs` at capture.
- [x] All engine + UI tests pass; BlackHole end-to-end: +6 dB bell at 1 kHz on Out 1 shows +6 dB in the live TF; LR24 HP at 100 Hz shows −6 dB at 100 Hz; FIR delta at 48 samples shows +1 ms arrival; sweep returns a flat trace; auto-align of outputs 1–2 with different delays equalises arrivals.
- [x] Commit.
