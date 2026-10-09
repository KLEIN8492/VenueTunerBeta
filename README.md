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

## Packaging (VenueTuner.app for any Mac)

```bash
npm run package   # universal engine + UI + Electron → app/dist/
```

Produces `app/dist/VenueTuner-<version>-universal.dmg` and `.zip`: one app for Apple Silicon and
Intel, macOS 13 Ventura or later. Engine and UI are inside the app bundle.

**Installing on another Mac:** open the .dmg and drag VenueTuner to Applications. The app is
ad-hoc signed, not notarized (that needs a paid Apple Developer account), so the first launch is:
right-click VenueTuner → **Open** → Open. On recent macOS versions, if there is no Open button:
System Settings → Privacy & Security → "VenueTuner was blocked" → **Open Anyway**. macOS then asks
for microphone access once: allow it, or the measurement inputs stay silent.

Settings, snapshots and calibration live in `~/Library/Application Support/VenueTuner` on each Mac.

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
5. **Sound level, RTA, room** (Measurement window):
   - **Sound level**: LAS/LCF/LZ… with Leq and peak. Calibrate once per mic with a calibrator
     (94 dB default); the reading is also shown in the Live window toolbar.
   - **View → RTA**: real-time 1/3 or 1/6-octave spectrum of the mic (works on show music, no generator needed).
   - **Room tab**: after a log sweep, EDT / T20 / T30 per octave (63 Hz–8 kHz) with decay curves.
   - **Traces → Import…**: REW / Smaart / VenueTuner text exports, to compare with old measurements.
6. **Matrix** (Measurement window): define groups (outputs played together) and mic positions.
   Put the mic at a position and press ▶: each group plays alone, is captured, quality-checked
   (retried once if disturbed) and labelled "Group @ Position". Move the mic, repeat. "avg" averages
   a group over all positions for Auto-EQ. One mic is enough.
7. **Speaker presets** (EQ editor in Live): save an output's speaker processing (crossover, EQ,
   all-pass, limiter, polarity, level, FIR) and apply it to other outputs; delay and routing stay.
   Export/import `.vtpreset` files to share them (the FIR is embedded).
8. **AIR** (Live toolbar): enter the temperature and humidity, press "Tuned now" when the system is
   tuned, and give each output its distance to the audience. As the air changes, a high shelf
   restores the HF the air absorbs (ISO 9613-1).
9. **Interface latency**: the driver-reported value is used automatically. For precision,
   cable an output to an input and run *Calibrate* in Settings once per device/rate/buffer.

Snapshots store the patch (inputs, outputs, channels), routing and all processing. Recalling one
made on another interface asks you to remap each input/output to the current channels.

Data (autosave, snapshots) lives in `~/Library/Application Support/VenueTuner`.

## Editing

- **Undo / redo** (⌘Z / ⇧⌘Z, or ↶ ↷ in the Live toolbar): every tuning and patch change, shared
  by all windows, up to 100 steps; a drag counts as one step. Generator and measurement settings
  are not part of the history. In a text field ⌘Z undoes the typing.
- Windows reopen where you left them.

## Performance

16 outputs, each with 16 PEQ bands + LR24 HP + LR48 LP + 4 all-pass (28 IIR sections), at
48 kHz / 64 samples: about **4 % of one Apple Silicon core** (≈ 0.25 % per fully loaded output).
`engine/build/vt_tests "[.bench]"` runs the benchmark.

## Safety rules built in

- Outputs muted on start-up, device change/disconnect and engine restart until you unmute.
- Generator always comes back OFF after a restart; level hard-capped at −12 dBFS RMS
  and it fades in/out over 500 ms.
- Gain changes ramp over 20 ms, delay changes crossfade over 50 ms (no clicks).
- Per-output peak limiter (default −1 dBFS, zero latency).
- FIR on/off crossfades (30 ms); patch edits never move another output's processing state.
- Denormals are flushed on the audio thread (no CPU spikes on silence on Intel machines).
- If the UI crashes or closes, the engine keeps passing audio.
