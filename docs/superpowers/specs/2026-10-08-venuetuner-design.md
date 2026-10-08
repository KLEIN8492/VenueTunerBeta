# VenueTuner — Design Spec

**Date:** 2026-10-08
**Status:** Draft, awaiting review

## 1. Purpose

VenueTuner is a live-PA system processor and measurement tool in one application:
a hybrid of Room EQ Wizard / Smaart (measurement) and L-Acoustics LA Network Manager
(processing and system tuning).

The user feeds a console output into an audio interface, VenueTuner processes it in
real time (routing, gain, delay, EQ, crossovers, all-pass, FIR, limiting) and sends it to
the amplifiers / powered speakers. A measurement microphone on the same interface lets
the user measure the room and the speakers against an internally generated reference,
then adjust the processing — manually or with optional automatic calibration — while
hearing the result live.

### Primary use case

Live sound reinforcement: mains + subs (+ fills / delays), multiple outputs with
crossovers. Low added latency is a hard requirement.

### Success criteria for the proof of concept (Milestone 1)

- Select any CoreAudio interface; console input passes through to N outputs with
  gain / polarity / delay / mute / limiter.
- Pink noise generated internally is played on selected outputs; mic input is measured
  against a software (or physical) loopback reference.
- Live transfer function (magnitude, phase, coherence) and impulse response with delay
  finder, displayed in the desktop app; magnitude agrees with REW within ~1 dB for the
  same speaker and mic position.
- Added latency beyond driver + converters is zero for the IIR path.
- Device unplug, engine crash, and UI disconnect never result in unmuted noise bursts.

## 2. Constraints

- **Platform now:** macOS (Apple Silicon), CoreAudio, developed on the user's laptop.
- **Platform later:** a dedicated box — small Linux computer (ARM, e.g. Raspberry Pi 5,
  or fanless x86 mini PC) or a Mac mini — plus a USB / Thunderbolt interface.
  Windows (ASIO) is a later target.
- **Interfaces:** any class-compliant wired USB / Thunderbolt interface, from
  2 in / 4 out up to 16+ channels. Network audio (Dante / AVB) is out of scope.
- **Latency:** the default processing path (IIR) adds zero samples. FIR is opt-in per
  output and its latency is always displayed.
- **Real-time safety:** no allocation, locks, or I/O on the audio thread.

## 3. Architecture

Three processes / layers:

```
┌───────────────────── Engine (C++20 / JUCE, headless) ──────────────────────┐
│                                                                            │
│  CoreAudio ──► Input router ──► [Console in] ──► Output chains ×N ──► CoreAudio out
│  (later ALSA)       │                               ▲                      │
│                     │          Pink / sweep gen ────┤                      │
│                     │                │ (software loopback ref)             │
│                     └─[Mic in]──► Analyzer (TF, coherence, IR, delay)       │
│                                                                            │
│  Audio thread ◄── lock-free cmd queue ◄── Control thread ──► Snapshot store│
│  Audio thread ──► lock-free ring buffers ──► Analysis thread               │
│                                                                            │
│  WebSocket server (JSON commands, binary spectra ~30 fps)                  │
└───────────────────────────────┬────────────────────────────────────────────┘
                                │ localhost (LAN later)
          Electron desktop app: Measurement window · Live/Tuning window · Settings (⌘,)
```

- **Engine** — headless C++20, JUCE `juce_core`, `juce_audio_basics`,
  `juce_audio_devices`, `juce_dsp` only (no GUI modules), built with CMake, JUCE pulled
  via FetchContent. Portable to Linux (ALSA / JACK) by changing only the device backend.
- **Controller UI** — TypeScript web UI (Vite), rendered with canvas 2D.
- **Desktop shell** — Electron: launches and supervises the engine as a child process,
  owns the native windows and the macOS menu bar. The same web UI can later be opened in
  a plain browser (e.g. tablet at FOH) against a remote engine.

### Engine threads

| Thread | Responsibility | Rules |
|---|---|---|
| Audio | Driver callback: input routing, generator, output chains, write analysis ring buffers | Real-time: no allocation, no locks, no syscalls, no logging |
| Analysis | FFT, averaging, transfer function, coherence, IR, delay finder | May allocate at setup only |
| Control | WebSocket, command validation, filter design (coefficients, FIR), snapshots, autosave | Never touched by audio thread except via lock-free queues / atomic swaps |

### State model

A single JSON-serialisable **SystemState** is the source of truth:

- device config (device name, sample rate, buffer size)
- input roles (console L/R, mic 1..n, physical loopback)
- output list (name, type, colour, mix matrix row, processing chain parameters)
- generator settings
- calibration data (I/O latency per device/rate/buffer, mic cal, SPL offset)

UI edits and automatic calibration both produce the same kind of state change. A
**snapshot** is a saved, named copy of the processing part of SystemState.

### Repository layout

```
VenueTuner/
  engine/        C++ engine (CMake), tests/ (Catch2)
  ui/            TypeScript web UI (Vite)
  app/           Electron shell (main process, preload, menu, windows)
  docs/          specs, plans
```

## 4. Engine — audio I/O and processing

### Audio I/O

- JUCE `AudioDeviceManager`; device, sample rate (48 kHz default, 96 kHz optional),
  buffer size (64 default; 32 / 128 / 256 selectable).
- The engine processes directly in the driver callback with no additional internal
  buffering.
- Reported round-trip latency = driver in + driver out + converter latency (from the
  device, or from loopback calibration).

### Routing

- **Input router:** each physical input is assigned a role: `Console`, `Mic`,
  `Loopback (physical)`, or unused.
- **Output matrix:** each output = Σ (gain × console input) + generator (if the output is
  a generator target). Example: Sub = 0.5·L + 0.5·R.

### Per-output chain (fixed order, every block bypassable)

```
Input mix → Gain → Polarity → Delay → PEQ (16 bands) → Crossover HP/LP
          → All-pass (4) → FIR (optional) → Limiter → Mute → Out
```

| Block | Implementation | Added latency |
|---|---|---|
| Gain / polarity / mute | Smoothed gain, ~20 ms ramp | 0 |
| Delay | Integer + fractional delay line, max 1 s | as set |
| PEQ | Biquads (RBJ): bell, low/high shelf, notch, HP/LP 12 dB/oct | 0 |
| Crossover | Butterworth and Linkwitz-Riley 12 / 24 / 48 dB/oct, cascaded biquads | 0 |
| All-pass | 1st / 2nd-order all-pass biquads | 0 |
| FIR | Non-uniform partitioned convolution (direct-form head + FFT tail) — zero processing latency; linear-phase filters add ½·taps by design | displayed |
| Limiter | Peak limiter, threshold / attack / release, no look-ahead | 0 |

### Minimum-latency / DSP quality rules

- Biquads in Transposed Direct Form II, **double precision** (stable for low crossovers
  at 96 kHz).
- Bypassed bands are skipped entirely, not computed as flat filters.
- FIR only runs when enabled on an output; minimum-phase FIR adds no latency.
- The UI shows a **latency budget per output**: driver + converter + delay + FIR.

### Live-safety rules

- Coefficients and FIR tables are designed on the control thread and swapped atomically.
  Biquad changes are interpolated over one block; FIR changes crossfade over ~50 ms.
  Snapshot recall crossfades.
- Outputs start **muted** on launch, on device reconnect, and after an engine restart;
  the user must unmute.
- Generator starts at −40 dBFS and fades in; a hard maximum level cannot be exceeded.
- **MUTE ALL** is always available and acts on the audio thread within one block.

## 5. Measurement

### Signals

- **Pink noise** (continuous, optionally periodic) for live dual-FFT measurement.
- **Log sweep** (Farina) for high-SNR impulse responses.
- Routed to any output(s), level in dBFS, fade in / out.

### Reference

- **Software loopback (default):** a copy of the generator signal, delayed by the
  calibrated I/O latency. No physical input is consumed.
- **Physical loopback (optional):** an input wired from an output.
- **Console as reference:** dual-FFT on programme material (measure during a show).

### Analysis

- Dual-channel FFT transfer function H = Sxy / Sxx (magnitude + phase).
- Coherence γ² shown as a trace and used to blank / fade low-confidence points.
- Multi-time-window FFT (longer windows in the lows, shorter in the highs), ~1/24 octave
  resolution.
- Averaging: exponential or fixed count. Smoothing: none, 1/48, 1/24, 1/12, 1/6, 1/3 oct.
- Impulse response (IFFT of H). **Delay finder:** IR peak with sub-sample interpolation;
  reference is automatically aligned; one-click "apply delay to output X".
- RTA (single-channel mic spectrum).
- **Traces:** capture, name, colour, show/hide, spatial average of several traces,
  export / import as CSV and REW-compatible text.

### Calibration

- **I/O latency calibration:** loopback cable, click, measured once per device /
  sample rate / buffer size, stored.
- **Mic calibration file:** standard `.cal` / `.txt` (frequency, dB offset).
- **SPL calibration (optional):** 94 dB calibrator → absolute dB SPL.

## 6. Automatic calibration (optional, editable)

Automatic features never create hidden processing. They write ordinary parameters
(delays, gains, PEQ bands, all-pass, FIR) into SystemState, saved as a **new snapshot**,
so the user can inspect, edit, bypass, A/B, or roll back everything.

| Feature | Milestone |
|---|---|
| I/O latency calibration | 1 |
| Auto delay alignment (per output, from IR peaks) | 2 |
| Auto-EQ: spatial average vs target curve (flat / house / tilt), cut-preferring, boosts capped at +3 dB, fitted only where coherence is high, after 1/6-oct smoothing, limited band count | 3 |
| Auto phase / crossover alignment suggestions (all-pass, FIR) | 3–4 |

## 7. Controller UI

Style: raw and dense. Flat dark grey, monospace numerics, no rounded cards, no
animation; one accent colour for live signals; red reserved for mute / clip / errors.
Keyboard-driven.

### Window 1 — Measurement (⌘1)

- Stacked plots: magnitude, phase, coherence; log frequency 20 Hz – 20 kHz.
- IR / delay finder strip.
- Trace list (live + stored).
- Generator controls (signal, level, target outputs) and mic / reference selection.

### Window 2 — Live / Tuning (⌘2)

- One strip per output: meters, gain, mute, polarity, delay, limiter activity, latency
  budget.
- Selected output's chain (PEQ, XO, AP, FIR, Lim) opens below with an **interactive EQ
  graph**: drag bands, scroll for Q; overlays the live measurement trace and the
  **predicted after EQ** trace (H_measured × H_filters).
- Link / gang outputs for paired editing.
- Snapshots panel: save, recall, A/B, duplicate, export / import JSON.
- **MUTE ALL** always visible.

### Settings (⌘,)

Audio device, sample rate, buffer size; input roles; routing matrix; output names /
types / colours; I/O latency calibration; mic calibration file; SPL calibration.

### Status bar (both windows)

Device / rate / buffer · CPU % · xrun counter · round-trip latency · engine connection ·
active snapshot.

The UI computes filter response curves locally (TypeScript) for instant display; the
engine remains the source of truth for audio.

## 8. Engine ↔ UI protocol

- WebSocket on localhost (configurable port; LAN later).
- **JSON messages** for commands and state: `state.get`, `state.patch` (JSON-Patch-like
  path + value), `snapshot.save/recall/list/delete`, `generator.set`,
  `measure.start/stop/capture`, `calibrate.latency`, `mute.all`.
  Every command gets an `ok` / `error` reply with a reason.
- **Binary frames** for streaming data (~30 fps): spectra (magnitude, phase, coherence
  arrays), IR, meter levels.
- Engine broadcasts state changes to all connected clients, so both windows stay in sync.

## 9. Error handling

| Situation | Behaviour |
|---|---|
| Device disconnected / sample-rate change | Mute outputs, keep state, retry every 1 s, UI banner; on reconnect outputs stay muted until the user unmutes |
| Audio callback overrun | xrun counter in status bar; repeated overruns → warning suggesting larger buffer or FIR bypass |
| Engine crash | Electron restarts it; last autosaved state (every few seconds + every snapshot) reloads; outputs muted |
| UI / WebSocket disconnect | Audio unaffected; UI shows disconnected and auto-reconnects |
| Invalid filter parameters | Validated / clamped on control thread before reaching audio; error returned to UI |
| Clip / limiter active | Per-output indicators; generator hard cap |
| Corrupt snapshot / cal file | Rejected with a message; current state never partially overwritten |

## 10. Testing

- **DSP unit tests (Catch2):** biquad responses vs RBJ formulas; LR crossover sum flat
  within ±0.01 dB; all-pass magnitude flat; delay sample-exact; partitioned FIR equals
  direct convolution.
- **Synthetic room test:** generator → known IR + delay + noise → analyzer must recover
  magnitude / phase, delay within ±0.1 sample, coherence drops with added noise.
- **Real-time safety:** debug builds assert on allocation / locking in the audio thread.
- **Protocol tests:** command round-trip; snapshot save / load.
- **Cross-check with REW:** same speaker, same mic position, magnitude within ~1 dB.
- **Manual live checklist:** device unplug, buffer change, snapshot recall under noise,
  MUTE ALL response.

## 11. Milestones

1. **Walking skeleton (PoC):** engine + Electron shell + both windows + settings;
   device selection; routing; per-output gain / polarity / delay / mute / limiter;
   pink noise; software and physical loopback; transfer function, coherence, IR, delay
   finder; traces; I/O latency calibration; mic cal import; snapshots; error handling
   above.
2. **Processing:** PEQ, crossovers, all-pass with interactive EQ graph and "predicted
   after EQ"; FIR (load WAV / text IR, partitioned convolution); auto delay alignment;
   log sweep.
3. **Auto-tuning:** spatial averaging, target curves, auto-EQ, phase / crossover
   alignment suggestions.
4. **Dedicated device:** Linux build (ALSA / JACK), headless service, UI over LAN from a
   browser / tablet; Mac mini deployment.
5. **Later:** Windows / ASIO, spectrogram / waterfall, RT60 / decay analysis, SPL logging.

## 12. Out of scope

- Network audio (Dante, AVB, AES67).
- Controlling third-party amplifiers or processors.
- Embedded DSP hardware targets (SHARC / ADAU / FPGA).
- Plugin formats (VST / AU).
