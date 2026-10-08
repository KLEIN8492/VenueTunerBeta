// Automatic EQ: fit up to N PEQ bands so a measured response follows a target curve.
//
// Rules (engineering, not cosmetics):
// - 1/6-octave smoothing; points with coherence below `minCoherence` are ignored, so
//   reflections and noise are not "corrected".
// - Cuts are preferred. Boosts are capped (+3 dB) and only used for broad dips: a narrow
//   deep dip is a cancellation that EQ cannot fill, it only wastes amplifier headroom.
// - Only the [fLo, fHi] range is fitted (e.g. the output's crossover passband); outside it
//   the EQ is kept close to 0 dB.
import { design, magDb, response } from './filters';
import type { PeqBand } from './types';

export type Target =
  | { kind: 'flat' }
  | { kind: 'tilt'; tiltDbPerOct: number }
  | { kind: 'house'; lfDb: number; lfFreq: number; hfDb: number; hfFreq: number };

export interface AutoEqOptions {
  fLo: number; fHi: number; sr: number;
  maxBands?: number; maxBoostDb?: number; maxCutDb?: number; minCoherence?: number; toleranceDb?: number;
}

export interface AutoEqResult {
  bands: PeqBand[];
  offsetDb: number;   // level offset applied to the measurement before comparing to the target
  smoothed: number[]; // measurement, smoothed and level-matched (dB), NaN where ignored
  predicted: number[]; // smoothed + EQ
  target: number[];
}

export function targetDb(t: Target, f: number): number {
  switch (t.kind) {
    case 'flat': return 0;
    case 'tilt': return t.tiltDbPerOct * Math.log2(f / 1000);
    case 'house': {
      // LF: shelf-like rise below lfFreq (half the lift at lfFreq). HF: linear-in-octaves
      // roll-off from hfFreq down to −hfDb at 20 kHz.
      const lf = t.lfDb / (1 + Math.pow(f / t.lfFreq, 2));
      const hf = f <= t.hfFreq ? 0 : -t.hfDb * Math.min(1, Math.log2(f / t.hfFreq) / Math.log2(20000 / t.hfFreq));
      return lf + hf;
    }
  }
}

const bandDb = (b: PeqBand, f: number, sr: number) => magDb(response(design.bell(b.freq, b.q, b.gainDb, sr), f, sr));

export function autoEq(m: { freq: ArrayLike<number>; mag: ArrayLike<number>; coh: ArrayLike<number> }, target: Target, o: AutoEqOptions): AutoEqResult {
  const maxBands = o.maxBands ?? 8, maxBoost = o.maxBoostDb ?? 3, maxCut = o.maxCutDb ?? -12;
  const minCoh = o.minCoherence ?? 0.6, tol = o.toleranceDb ?? 0.75;
  const n = m.freq.length;
  const f = Array.from(m.freq);
  const inRange = f.map((x) => x >= o.fLo && x <= o.fHi);

  // 1. Coherence-weighted 1/6-octave smoothing (power average).
  const w0 = Array.from(m.coh, (c) => (c >= minCoh ? c * c : 0));
  const smoothed: number[] = new Array(n).fill(NaN);
  for (let i = 0; i < n; i++) {
    let sw = 0, sp = 0;
    for (let k = 0; k < n; k++) {
      if (Math.abs(Math.log2(f[k] / f[i])) > 1 / 12 || w0[k] === 0) continue;
      sw += w0[k];
      sp += w0[k] * Math.pow(10, m.mag[k] / 10);
    }
    if (sw > 0) smoothed[i] = 10 * Math.log10(sp / sw);
  }
  const tgt = f.map((x) => targetDb(target, x));
  const weight = f.map((_, i) => (inRange[i] && isFinite(smoothed[i]) && w0[i] > 0 ? 1 : 0));

  // 2. Level match on the mid band (200 Hz – 5 kHz), else the whole valid range.
  const meanDiff = (lo: number, hi: number) => {
    let s = 0, c = 0;
    for (let i = 0; i < n; i++) if (weight[i] && f[i] >= lo && f[i] <= hi) { s += smoothed[i] - tgt[i]; c++; }
    return c ? s / c : NaN;
  };
  let offsetDb = meanDiff(200, 5000);
  if (!isFinite(offsetDb)) offsetDb = meanDiff(0, Infinity);
  if (!isFinite(offsetDb)) return { bands: [], offsetDb: 0, smoothed, predicted: smoothed, target: tgt };
  const err = f.map((_, i) => (weight[i] ? smoothed[i] - offsetDb - tgt[i] : 0)); // + = too loud

  const eqCurve = (bands: PeqBand[]) => f.map((x) => bands.reduce((a, b) => a + bandDb(b, x, o.sr), 0));
  // Cost: weighted error in range + a light penalty for EQ outside the range. The boost cap
  // applies to the *combined* curve (stacked bands must not add up past it).
  const overBoost = (e: number[]) => Math.max(0, ...e) - maxBoost;
  const cost = (bands: PeqBand[]) => {
    const e = eqCurve(bands);
    let c = 0;
    for (let i = 0; i < n; i++) c += weight[i] ? Math.pow(err[i] + e[i], 2) : inRange[i] ? 0 : 0.1 * e[i] * e[i];
    return overBoost(e) > 0.01 ? Infinity : c;
  };

  // Width (octaves) of a feature at index i where |residual| stays above half its value.
  const widthOct = (r: number[], i: number) => {
    const half = r[i] / 2;
    let a = i, b = i;
    while (a > 0 && weight[a - 1] && Math.sign(r[a - 1]) === Math.sign(r[i]) && Math.abs(r[a - 1]) > Math.abs(half)) a--;
    while (b < n - 1 && weight[b + 1] && Math.sign(r[b + 1]) === Math.sign(r[i]) && Math.abs(r[b + 1]) > Math.abs(half)) b++;
    return { lo: f[a], hi: f[b], oct: Math.log2(f[b] / f[a]) || 1 / 24 };
  };

  // 3. Greedy placement on the largest remaining deviation.
  const bands: PeqBand[] = [];
  const clampBand = (b: PeqBand): PeqBand => ({
    ...b,
    freq: Math.min(o.fHi, Math.max(o.fLo, b.freq)),
    gainDb: Math.min(maxBoost, Math.max(maxCut, b.gainDb)),
    q: Math.min(8, Math.max(0.5, b.q)),
  });
  const skip = new Array(n).fill(false); // dips we can't boost any further
  for (let guard = 0; bands.length < maxBands && guard < 64; guard++) {
    const e = eqCurve(bands);
    const r = err.map((x, i) => (weight[i] ? x + e[i] : 0));
    let best = -1, bestScore = tol;
    for (let i = 0; i < n; i++) {
      if (!weight[i] || skip[i]) continue;
      if (r[i] > 0 && r[i] > bestScore) { best = i; bestScore = r[i]; }
      // Dips: only broad ones (≥ 1/3 octave at half depth), and they score lower than peaks.
      if (r[i] < 0 && -r[i] * 0.7 > bestScore && widthOct(r, i).oct >= 1 / 3) { best = i; bestScore = -r[i] * 0.7; }
    }
    if (best < 0) break;
    const wd = widthOct(r, best);
    const fc = Math.sqrt(wd.lo * wd.hi);
    // A boost may only use the headroom the existing bands leave at that frequency.
    const room = maxBoost - Math.max(0, ...e);
    const gain = r[best] > 0 ? -r[best] : Math.min(-r[best], room);
    const band = clampBand({ on: true, type: 'bell', freq: fc, gainDb: gain, q: fc / Math.max(1, wd.hi - wd.lo) });
    if ((gain > 0 && gain < 0.3) || overBoost(eqCurve([...bands, band])) > 0.01) {
      // No boost headroom here: leave this dip alone and keep looking for other problems.
      for (let i = 0; i < n; i++) if (f[i] >= wd.lo && f[i] <= wd.hi) skip[i] = true;
      continue;
    }
    bands.push(band);
  }

  // 4. Refinement: coordinate descent on (freq, gain, Q) of every band.
  let current = cost(bands);
  const steps = [
    (b: PeqBand, s: number) => ({ ...b, freq: b.freq * Math.pow(2, s / 24) }),
    (b: PeqBand, s: number) => ({ ...b, gainDb: b.gainDb + 0.25 * s }),
    (b: PeqBand, s: number) => ({ ...b, q: b.q * Math.pow(1.12, s) }),
  ];
  for (let iter = 0; iter < 40; iter++) {
    let improved = false;
    for (let k = 0; k < bands.length; k++)
      for (const step of steps)
        for (const s of [1, -1]) {
          const trial = bands.slice();
          trial[k] = clampBand(step(bands[k], s));
          const c = cost(trial);
          if (c < current - 1e-6) { bands[k] = trial[k]; current = c; improved = true; }
        }
    if (!improved) break;
  }

  const kept = bands
    .filter((b) => Math.abs(b.gainDb) >= 0.3)
    .map((b) => ({ ...b, freq: Math.round(b.freq), gainDb: Math.round(b.gainDb * 10) / 10, q: Math.round(b.q * 100) / 100 }))
    .sort((a, b) => a.freq - b.freq);
  const e = eqCurve(kept);
  return {
    bands: kept,
    offsetDb,
    smoothed: smoothed.map((x, i) => (weight[i] || isFinite(x) ? x - offsetDb : NaN)),
    predicted: smoothed.map((x, i) => x - offsetDb + e[i]),
    target: tgt,
  };
}
