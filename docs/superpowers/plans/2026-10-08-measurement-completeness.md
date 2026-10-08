# VenueTuner Measurement Completeness Plan (SPL, RTA, RT60, import)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the remaining measurement items of the spec (§5): SPL meter with calibration, RTA, room decay (RT60) from the sweep, trace import.

**Architecture:** `SplMeter` and `Rta` live in `engine/src/measure/` and are fed by the analyzer with the same mic samples (analysis thread, never the audio thread). RT60 is computed from the sweep impulse response (`RoomDecay`). The server streams RTA as binary frame type 3 and SPL inside the status event. UI: TF/RTA plot switch, SPL readouts (Measurement + Live), a Room tab, trace import.

**Spec:** `docs/superpowers/specs/2026-10-08-venuetuner-design.md` §5 (RTA, SPL calibration, traces import), §11 later items (RT60).

## Global Constraints

- A and C weighting per IEC 61672 (analog prototype → bilinear, matched at 1 kHz = 0 dB); Z = flat.
- Fast = 125 ms, Slow = 1 s exponential time weighting; Leq resettable; peak = max |x| (Z).
- SPL calibration: `cal.splOffsetDb[<mic source name>]` = 94 − measured dBFS (Z, 1 kHz calibrator), any reference level allowed.
- RTA: 1/3 and 1/6-octave bands 20 Hz–20 kHz, power-summed from an 8192-point Welch spectrum, exponential averaging ~1 s.
- RT60: octave bands 63 Hz–8 kHz, Schroeder backward integration with noise-floor truncation; EDT (0…−10 dB), T20 (−5…−25), T30 (−5…−35), each ×(60/range).
- Sweep tail extended to 3 s so decays up to ~2.5 s are captured.

---

### Task 1: SplMeter (engine) — weighting filters, Fast/Slow, Leq, peak
- [x] Tests: 1 kHz sine at −20 dBFS RMS → Z/A/C read −20 ±0.1 dBFS; 100 Hz sine → A reads −19.1 ±0.5 dB relative to Z, C −0.3 ±0.3; Leq of 1 s at −20 + 1 s at −40 ≈ −23.0; peak of a 0.5 sine = −6.02 dBFS.

### Task 2: Rta (engine)
- [x] Tests: pink noise → 1/3-oct bands equal within ±1 dB (63 Hz–10 kHz); 1 kHz sine → the 1 kHz band dominates by > 30 dB.

### Task 3: RoomDecay (engine) from the sweep IR
- [x] Tests: synthetic IR = exponentially decaying noise with RT60 = 0.8 s (+ noise floor at −70 dB) → T20 and T30 within 5 % in the 250 Hz–4 kHz bands.

### Task 4: Server / dispatcher
- Status gets `spl {z, a, c fast/slow, leq, peak, weighting, calibrated}`; binary RTA frames (type 3); `spl.calibrate {level}`, `spl.resetLeq`, `rta.setResolution`; the sweep event carries `decay` (RT60 table + Schroeder curves).
- [x] Dispatcher test: calibration stores the offset under the mic source name.

### Task 5: UI
- TF/RTA switch on the magnitude plot; SPL panel (big value, weighting, Fast/Slow, Leq reset, calibrate); SPL readout in the Live toolbar; Room tab (RT60 table + decay plot); trace import (REW / Smaart text) with parser tests.
- [x] Build; BlackHole end-to-end: −20 dBFS pink → SPL and RTA read sensibly; sweep returns a decay table.
