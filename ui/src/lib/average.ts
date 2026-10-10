// Spatial average of several captures (mic positions) for tuning decisions.
//
// - Every trace is put on the common frequency grid first (sweeps, imports, older captures).
// - Each position is level-matched before averaging. Otherwise the position nearest the speaker
//   (often 6–10 dB louder than the back) dominates a power average, and the EQ ends up fitted
//   to the front rows. The match uses the band where every position has usable coherence; the
//   average keeps the mean level of the positions.
// - Magnitude: power average. Coherence: the worst position (a point is only trusted if it is
//   trustworthy everywhere). Phase: the first trace's (a spatial phase average means nothing).
import { onGrid } from './traceimport';
import type { Trace } from './types';

const MATCH_COH = 0.5; // coherence every position needs at a point for it to be used in the level match

export function spatialAverage<T extends Trace>(traces: T[], name: string): T | null {
  if (!traces.length) return null;
  const ts = traces.map(onGrid);
  if (ts.length === 1) return ts[0];
  const n = ts[0].freq.length;

  let match = Array.from({ length: n }, (_, i) => ts.every((t) => t.coh[i] >= MATCH_COH && Number.isFinite(t.mag[i])));
  if (match.filter(Boolean).length < 6) match = match.map(() => true); // nothing common: use everything
  const levels = ts.map((t) => {
    let s = 0, c = 0;
    for (let i = 0; i < n; i++) if (match[i] && Number.isFinite(t.mag[i])) { s += t.mag[i]; c++; }
    return c ? s / c : 0;
  });
  const meanLevel = levels.reduce((a, b) => a + b, 0) / levels.length;

  const mag: number[] = [], coh: number[] = [];
  for (let i = 0; i < n; i++) {
    const p = ts.reduce((a, t, k) => a + Math.pow(10, (t.mag[i] - levels[k] + meanLevel) / 10), 0) / ts.length;
    mag.push(10 * Math.log10(p));
    coh.push(Math.min(...ts.map((t) => t.coh[i])));
  }
  return { ...ts[0], name, mag, coh };
}
