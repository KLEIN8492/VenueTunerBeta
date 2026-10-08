# VenueTuner Patch & Routing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace "one input/strip per physical channel" with user-defined **sources** and **outputs** patched to physical channels, a Patch page in Settings, snapshots that include the patch (with remapping on another interface), and a latency breakdown.

**Architecture:** State holds `sources` (name, type, physical input channel) and `outputs` (processing + mix over sources + physical output channel). `toParams` resolves sources to physical channel gains, so the audio thread stays simple: per logical output, sum physical inputs → chain → write to its physical channel. Physical channels nothing is patched to stay silent.

**Spec:** `docs/superpowers/specs/2026-10-08-venuetuner-design.md` (routing, section 4) as amended by this plan. Decisions from the user (2026-10-08): outputs are user-defined (only the ones needed); the patch lives on its own page in Settings; snapshots include the patch and ask to remap when the interface differs.

## Global Constraints

- Up to 64 sources and 64 outputs; source types `program | mic | loopback`.
- `mic` and `loopback` sources are never routed to outputs (enforced in `toParams`).
- An output channel can be used by one output only (validated); `-1` = unassigned (processed, silent).
- Snapshots store sources, outputs and the device names; recall on a different device returns `needsRemap` with a prefilled mapping (same name → same channel; else same channel number if it exists; else unassigned) and applies only after the user confirms.
- Old autosaves/snapshots (M1/M2: `inputs` with roles, outputs = physical channels) migrate automatically.
- Fresh state (no sources, no outputs): 2 program sources (In 1/In 2 → "Console L/R") and 2 outputs ("Main L"/"Main R") when the device has ≥ 2 in / 2 out.

---

### Task 1: State model + migration
- `SourceConfig { name, type, channel }`; `OutputConfig::channel`; `OutputConfig::mix` indexed by source; `MeasureConfig { micSource, ref: generator|source, refSource, … }`.
- Migration in `fromVar` when `inputs` exists and `sources` doesn't.
- Validation: duplicate output channels rejected; mix length = sources length.
- `toParams`: physical gain matrix from program sources; output channel; mic/ref physical channels from sources.
- [ ] Tests: migration of an M2 state (roles, mixes, micInput); toParams excludes mic sources; duplicate channel rejected; defaults for fresh state.

### Task 2: Engine routing + channel names + latency breakdown
- `OutputParams::channel`; `processBlock` writes each logical output to its physical channel, silence elsewhere.
- `AudioEngine::channelNames()` (driver names) and `latencyBreakdown()` (macOS CoreAudio: device latency, safety offset, stream latency, buffer per direction).
- [ ] Tests: output 0 on channel 3 → audio on physical 3 only; unassigned output silent; generator targets logical outputs.

### Task 3: Routing commands + snapshots with patch
- `routing.addSource {type}`, `routing.removeSource {index}`, `routing.addOutput {preset: single|stereo|sub}`, `routing.removeOutput {index}`, `routing.duplicateOutput {index}`, `routing.moveOutput {index, to}`; `device.channels`; `device.latency`.
- Snapshots: save `{version 2, device, sources, outputs}`; `snapshot.recall {name, remap?}` → `needsRemap` result when devices differ.
- [ ] Tests: remove source drops its mix column and fixes mic/ref indices; stereo preset; recall on the same device applies; on another device returns needsRemap with the prefill; recall with remap applies channels.

### Task 4: Patch page (Settings) + updates elsewhere
- Settings gets tabs: Audio · Patch · Calibration. Patch: sources table (name, type, input channel with driver names, meter), outputs table (name, type, colour, output channel, ↑↓, duplicate, delete), routing grid (program sources × outputs, dB), presets.
- Audio tab: latency breakdown table.
- Live: strips show "→ <channel name>"; Measurement: mic/ref from sources; remap dialog on snapshot recall (Live window).
- [ ] Build passes; BlackHole end-to-end: add sources/outputs, patch output to channel 5, measure through it, snapshot recall with remap.
