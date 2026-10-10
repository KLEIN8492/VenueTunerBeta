import { describe, expect, it } from 'vitest';
import { spatialAverage } from './average';
import { GRID } from './traceimport';
import type { Trace } from './types';

const trace = (freq: number[], mag: (f: number) => number, coh = 1): Trace => ({
  name: 't', freq, mag: freq.map(mag), phase: freq.map(() => 0), coh: freq.map(() => coh), delaySamples: 0, sampleRate: 48000,
});

describe('spatialAverage', () => {
  it('level-matches positions so the loudest one does not dominate', () => {
    // Front row 10 dB louder; the back row has a 6 dB dip at 1 kHz that the front row doesn't.
    const front = trace(GRID, () => 90);
    const back = trace(GRID, (f) => 80 - (Math.abs(Math.log2(f / 1000)) < 0.1 ? 6 : 0));
    const avg = spatialAverage([front, back], 'AVG')!;
    const at = (f: number) => avg.mag[GRID.findIndex((x) => x >= f)];
    expect(at(200)).toBeCloseTo(85, 0); // mean level of the positions
    // The dip shows with equal weight from both positions (≈ −2.7 dB), not drowned by the front.
    expect(at(200) - at(1000)).toBeGreaterThan(2);
  });

  it('puts traces on the common grid before averaging (sweep at 1/48 octave)', () => {
    const fine = Array.from({ length: 479 }, (_, i) => 20 * Math.pow(2, i / 48));
    const avg = spatialAverage([trace(GRID, () => 80), trace(fine, () => 80)], 'AVG')!;
    expect(avg.freq).toEqual(GRID);
    for (const m of avg.mag) expect(m).toBeCloseTo(80, 3);
  });

  it('keeps the worst coherence of all positions', () => {
    const avg = spatialAverage([trace(GRID, () => 80, 0.9), trace(GRID, () => 80, 0.3)], 'AVG')!;
    expect(Math.max(...avg.coh)).toBeCloseTo(0.3, 6);
  });
});
