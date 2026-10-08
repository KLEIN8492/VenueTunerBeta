import { describe, expect, it } from 'vitest';
import { alignPair, type PhaseTrace } from './xoalign';
import { crossover, chainResponse, magDb, phaseDeg } from './filters';

const sr = 48000;
const FREQ = Array.from({ length: 480 }, (_, i) => 20 * Math.pow(2, i / 48));

// A trace as the analyzer reports it: magnitude, phase with the arrival removed, and the arrival.
function trace(hp: boolean, delayMs: number, invert = false): PhaseTrace {
  const s = crossover('lr24', hp, 100, sr);
  const h = FREQ.map((f) => chainResponse(s, f, sr));
  return {
    freq: FREQ,
    mag: h.map(magDb),
    phase: h.map((c) => phaseDeg(c) + (invert ? 180 : 0)),
    delaySamples: (delayMs / 1000) * sr,
    sampleRate: sr,
  };
}

describe('alignPair', () => {
  it('finds the delay of a late sub', () => {
    const r = alignPair(trace(true, 10), trace(false, 12.5), 50, 200);
    // Sub arrives 2.5 ms late: the top must wait 2.5 ms (or the sub be advanced).
    expect(r.delayMs).toBeCloseTo(-2.5, 1);
    expect(r.invert).toBe(false);
    expect(r.gainAtXoDb).toBeCloseTo(0, 0); // LR24 sums flat at the crossover once aligned
    expect(r.currentAtXoDb).toBeLessThan(r.gainAtXoDb);
  });

  it('detects an inverted sub', () => {
    const r = alignPair(trace(true, 10), trace(false, 10, true), 50, 200);
    expect(r.invert).toBe(true);
    expect(Math.abs(r.delayMs)).toBeLessThan(0.1);
  });
});
