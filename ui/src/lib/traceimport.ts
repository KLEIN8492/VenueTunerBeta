// Import traces exported by REW, Smaart or VenueTuner (text: freq, mag [, phase [, coherence]]).
// Data is resampled onto VenueTuner's 1/24-octave grid so it can be averaged with captures.
import type { Trace } from './types';

export const GRID = Array.from({ length: 240 }, (_, i) => 20 * Math.pow(2, i / 24));

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

  // Unwrap phase before interpolating so a wrap between two points doesn't average to 0°.
  let prev = rows[0][2] ?? 0, offset = 0;
  const unwrapped = rows.map((r) => {
    const p = r[2] ?? 0;
    if (p - prev > 180) offset -= 360; else if (p - prev < -180) offset += 360;
    prev = p;
    return p + offset;
  });
  const at = (col: number[], f: number) => {
    if (f <= rows[0][0]) return col[0];
    if (f >= rows[rows.length - 1][0]) return col[col.length - 1];
    let i = 1;
    while (rows[i][0] < f) i++;
    const t = Math.log(f / rows[i - 1][0]) / Math.log(rows[i][0] / rows[i - 1][0]);
    return col[i - 1] + t * (col[i] - col[i - 1]);
  };
  const mags = rows.map((r) => r[1]);
  const cohs = rows.map((r) => (r.length >= 4 && Number.isFinite(r[3]) ? r[3] : 1));
  const wrap = (d: number) => ((((d + 180) % 360) + 360) % 360) - 180;
  return {
    name: fileName.replace(/\.[^.]+$/, ''),
    freq: GRID.slice(),
    mag: GRID.map((f) => at(mags, f)),
    phase: GRID.map((f) => wrap(at(unwrapped, f))),
    coh: GRID.map((f) => at(cohs, f)),
    delaySamples: 0,
    sampleRate: 48000,
  };
}
