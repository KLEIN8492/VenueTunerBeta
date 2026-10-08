# VenueTuner Milestone 3 (Auto-tuning) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Automatic EQ against a target curve and sub↔top phase alignment, both writing ordinary editable parameters; plus two fixes (stable processing slots per output, readable channel labels).

**Architecture:** Each output gets a persistent `id`; the dispatcher maps ids to engine slots (chain + convolution), so patch edits never move processing state. Auto-EQ and alignment are pure TypeScript functions (`ui/src/lib/autoeq.ts`, `ui/src/lib/xoalign.ts`) over captured traces, reusing the filter math mirror; results are applied with ordinary patches after saving a "before" snapshot.

**Spec:** `docs/superpowers/specs/2026-10-08-venuetuner-design.md` §6 (automatic calibration).

## Global Constraints

- Automatic features never create hidden processing: PEQ bands / delay / polarity only, saved "before" snapshot first.
- Auto-EQ: 1/6-oct smoothing, points with coherence < 0.6 ignored, boosts ≤ +3 dB, cuts ≥ −12 dB, Q 0.5–8, at most 16 bands (default 8), range limited to the output's HP/LP passband.
- Alignment uses absolute phase: H(f) = |H|·e^{j(φ − 2πf·delay/fs)} from each trace's own delay.

---

### Task 1: Stable output slots
- `OutputConfig::id` (unique, persisted; assigned on create/migration); `OutputParams::slot`; engine processes `chains[slot]`, convolutions per slot; dispatcher keeps id→slot and FIR-loaded paths per slot.
- [x] Tests: removing output 0 keeps output 1's slot (and its FIR) unchanged; ids unique after duplicate; migration assigns ids.

### Task 2: Channel labels
- UI: bare-number driver names display as "In n" / "Out n".

### Task 3: Auto-EQ (TS)
- `autoEq({ freq, mag, coh }, target, opts) → PeqBand[]` greedy placement + coordinate-descent refinement.
- Targets: flat, tilt (dB/oct around 1 kHz), house (LF shelf + HF roll-off), user-tunable `houseCurve`.
- [x] Tests: synthetic response with known bells is corrected to ±1 dB; a −15 dB narrow notch gets ≤ +3 dB; boosts never exceed +3 dB; low-coherence region untouched; output-range respected.

### Task 4: Sub↔top alignment (TS)
- `alignPair(top, sub, fLo, fHi, sr) → { delayMs, invert, gainAtXoDb, curves }` search ±15 ms (0.02 ms steps) × polarity maximising summed energy in the crossover band.
- [x] Tests: synthetic LR24 pair with sub 2.5 ms late → suggests +2.5 ms on the top (or −2.5 on sub) within 0.05 ms; inverted sub → suggests polarity invert.

### Task 5: Measurement sidebar tabs + Auto-EQ / Align panels
- Tabs Measure · Auto-EQ · Align. Auto-EQ: output, traces (average), target + params, range, bands, Compute (preview on plots), Apply. Align: top trace, sub trace, which output to adjust, Compute (curves), Apply.
- [x] Build; BlackHole end-to-end: synthetic room via PEQ "fault" on an output, measure, auto-EQ corrects it within ±1.5 dB.
