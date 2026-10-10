// Import traces exported by REW, Smaart or VenueTuner (text: freq, mag [, phase [, coherence]]).
// Data is resampled onto VenueTuner's 1/24-octave grid so it can be averaged with captures.
import type { Trace } from './types';

// Same points as the engine's live measurement: 1/48 octave, 20 Hz – 20 kHz (fine enough for HF phase).
export const GRID_PPO = 48;
export const GRID = Array.from({ length: Math.floor(GRID_PPO * Math.log2(1000)) + 1 }, (_, i) => 20 * Math.pow(2, i / GRID_PPO));

// Value of `col` (sampled at increasing `freqs`) at f: linear in log-frequency, held at the ends.
function interp(freqs: number[], col: number[], f: number): number {
  if (f <= freqs[0]) return col[0];
  if (f >= freqs[freqs.length - 1]) return col[col.length - 1];
  let i = 1;
  while (freqs[i] < f) i++;
  const t = Math.log(f / freqs[i - 1]) / Math.log(freqs[i] / freqs[i - 1]);
  return col[i - 1] + t * (col[i] - col[i - 1]);
}

// Unwrap phase before interpolating so a wrap between two points doesn't average to 0°.
function unwrapDeg(p: number[]): number[] {
  let prev = p[0] ?? 0, offset = 0;
  return p.map((x) => {
    if (x - prev > 180) offset -= 360; else if (x - prev < -180) offset += 360;
    prev = x;
    return x + offset;
  });
}
const wrap = (d: number) => ((((d + 180) % 360) + 360) % 360) - 180;

const onGridAlready = (freq: ArrayLike<number>) =>
  freq.length === GRID.length && GRID.every((f, i) => Math.abs(freq[i] / f - 1) < 1e-4);

// The same trace on the common grid (unchanged if it already is). Older sweeps (1/48 octave),
// imports and anything else can then be averaged and compared point by point.
export function onGrid<T extends Trace>(t: T): T {
  if (onGridAlready(t.freq)) return t;
  const freq = Array.from(t.freq), ph = unwrapDeg(Array.from(t.phase));
  return {
    ...t,
    freq: GRID.slice(),
    mag: GRID.map((f) => interp(freq, Array.from(t.mag), f)),
    phase: GRID.map((f) => wrap(interp(freq, ph, f))),
    coh: GRID.map((f) => interp(freq, Array.from(t.coh), f)),
  };
}

export function parseTraceText(text: string, fileName: string): Trace {
  const rows: number[][] = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || /^[*#;"a-zA-Z]/.test(line)) continue; // comments and header lines
    const nums = line.split(/[\s,;]+/).map(Number);
    if (nums.length >= 2 && nums.slice(0, 2).every(Number.isFinite) && nums[0] > 0) rows.push(nums);
  }
  if (rows.length < 3) throw new Error(`No frequency/magnitude data found in ${fileName}`);
  rows.sort((a, b) => a[0] - b[0]);
  return onGrid({
    name: fileName.replace(/\.[^.]+$/, ''),
    freq: rows.map((r) => r[0]),
    mag: rows.map((r) => r[1]),
    phase: rows.map((r) => r[2] ?? 0),
    coh: rows.map((r) => (r.length >= 4 && Number.isFinite(r[3]) ? r[3] : 1)),
    delaySamples: 0,
    sampleRate: 48000,
  });
}
