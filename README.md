# VenueTuner

Live-PA system processor + measurement tool (Smaart/REW-style measurement, LA Network
Manager-style processing) for macOS, built as a headless C++ engine with a desktop UI.

- **Engine** (`engine/`): C++20 / JUCE. Audio I/O, routing, per-output gain · polarity ·
  delay · limiter · mute, pink-noise generator, dual-FFT analyzer (transfer function,
  coherence, impulse response, delay finder). Zero added latency on the processing path.
- **UI** (`ui/`): TypeScript + canvas. Measurement, Live/Tuning and Settings windows.
- **App** (`app/`): Electron shell that supervises the engine and hosts the windows.

Design: `docs/superpowers/specs/2026-10-08-venuetuner-design.md` ·
Milestone 1 plan: `docs/superpowers/plans/2026-10-08-milestone1-walking-skeleton.md`

## Build & run

Requires Xcode command-line tools, CMake ≥ 3.22, Node ≥ 20.

```bash
npm run setup     # install UI + Electron dependencies (once)
npm run build     # build engine (downloads JUCE the first time) + UI
npm run app       # launch VenueTuner
npm test          # engine (Catch2) + UI (Vitest) tests
```

## Using it

| Key | Action |
|---|---|
| ⌘1 / ⌘2 | Measurement / Live-Tuning window |
| ⌘, | Settings (audio device, input roles, routing matrix, calibration) |
| G / C / R | (Measurement) generator on/off · capture trace · reset averaging |

1. **Settings (⌘,)**: choose the interface, sample rate and buffer. Mark the measurement mic
   input as `mic` (mics are never routed to outputs). Route console inputs to outputs in the
   matrix (values in dB).
2. **Live (⌘2)**: outputs start **muted** after launch, a device change or a crash.
   Check levels, then press *Unmute outputs*. *MUTE ALL* is always at the top.
3. **Measurement (⌘1)**: pick the mic, keep reference on *Internal loopback*, choose
   generator outputs, press G. The delay finder shows the arrival time (interface latency
   removed). To align: *Store as reference arrival* on the reference speaker, then measure
   the other speaker and press *Align to ref* on its output.
4. **Interface latency**: the driver-reported value is used automatically. For precision,
   cable an output to an input and run *Calibrate* in Settings once per device/rate/buffer.

Data (autosave, snapshots) lives in `~/Library/Application Support/VenueTuner`.

## Safety rules built in

- Outputs muted on start-up, device change/disconnect and engine restart until you unmute.
- Generator always comes back OFF after a restart; level hard-capped at −12 dBFS RMS
  and it fades in/out over 500 ms.
- Gain changes ramp over 20 ms, delay changes crossfade over 50 ms (no clicks).
- Per-output peak limiter (default −1 dBFS, zero latency).
- If the UI crashes or closes, the engine keeps passing audio.
