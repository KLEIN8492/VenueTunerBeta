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

1. **Settings (⌘,)**, one page per job:
   - **Audio**: interface, sample rate, buffer, and where the latency comes from (driver vs VenueTuner).
   - **Inputs**: create only the inputs you use, name them, pick their interface channel.
     Type `program` = sound to process, `measurement mic` = never sent to speakers, `loopback` = cabled reference.
   - **Outputs**: create one output per speaker feed, pick its interface channel, and choose which
     inputs feed it ("fed by", in dB). Presets: stereo pair, sub (L+R with LR24 100 Hz).
   - **Calibration**: interface latency (cable loopback) and the mic calibration file.
2. **Live (⌘2)**: outputs start **muted** after launch, a device change or a crash.
   Check levels, then press *Unmute outputs*. *MUTE ALL* is always at the top.
3. **Measurement (⌘1)**: pick the mic, keep reference on *Internal loopback*, choose
   generator outputs, press G. The delay finder shows the arrival time (interface latency
   removed). To align: *Store as reference arrival* on the reference speaker, then measure
   the other speaker and press *Align to ref* on its output.
4. **Auto-tuning** (Measurement window, sidebar tabs):
   - **Auto-EQ**: capture one or more mic positions of an output (generator on that output only),
     pick a target (flat, tilt, house curve) and Compute. The yellow curve is the predicted result;
     Apply replaces that output's PEQ with ordinary bands (cuts preferred, combined boost ≤ +3 dB,
     narrow dips and low-coherence regions left alone). A "Before auto-EQ" snapshot is saved.
   - **Align**: capture the top alone and the sub alone from the same position; Compute finds the
     delay and polarity that sum best around the crossover (using phase). Apply sets them.
5. **Interface latency**: the driver-reported value is used automatically. For precision,
   cable an output to an input and run *Calibrate* in Settings once per device/rate/buffer.

Snapshots store the patch (inputs, outputs, channels), routing and all processing. Recalling one
made on another interface asks you to remap each input/output to the current channels.

Data (autosave, snapshots) lives in `~/Library/Application Support/VenueTuner`.

## Safety rules built in

- Outputs muted on start-up, device change/disconnect and engine restart until you unmute.
- Generator always comes back OFF after a restart; level hard-capped at −12 dBFS RMS
  and it fades in/out over 500 ms.
- Gain changes ramp over 20 ms, delay changes crossfade over 50 ms (no clicks).
- Per-output peak limiter (default −1 dBFS, zero latency).
- If the UI crashes or closes, the engine keeps passing audio.
