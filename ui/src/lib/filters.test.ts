import { describe, expect, it } from 'vitest';
import { allpassMatched, design, crossover, chainResponse, response, outputResponse, magDb, phaseDeg } from './filters';
import type { OutputConfig } from './types';

const sr = 48000;
const db = (c: ReturnType<typeof response>) => magDb(c);

describe('filters (mirror of engine/src/dsp/Biquad.h)', () => {
  it('bell and shelves', () => {
    expect(db(response(design.bell(1000, 2, 6, sr), 1000, sr))).toBeCloseTo(6, 2);
    expect(db(response(design.lowShelf(200, 0.707, 6, sr), 20, sr))).toBeCloseTo(6, 1);
    expect(db(response(design.highShelf(5000, 0.707, -4, sr), 100, sr))).toBeCloseTo(0, 1);
  });
  it('Butterworth -3 dB at fc, odd orders included', () => {
    for (const t of ['bw12', 'bw18', 'bw24', 'bw36', 'bw48'] as const) {
      expect(db(chainResponse(crossover(t, true, 500, sr), 500, sr))).toBeCloseTo(-3.01, 1);
    }
  });
  it('LR24 sums flat', () => {
    const hp = crossover('lr24', true, 100, sr), lp = crossover('lr24', false, 100, sr);
    for (let f = 20; f < 20000; f *= 1.3) {
      const a = chainResponse(hp, f, sr), b = chainResponse(lp, f, sr);
      expect(magDb({ re: a.re + b.re, im: a.im + b.im })).toBeCloseTo(0, 2);
    }
  });
  it('all-pass is flat, -180 deg at fc', () => {
    const ap = design.allpass2(1000, 0.7, sr);
    expect(db(response(ap, 300, sr))).toBeCloseTo(0, 6);
    expect(Math.abs(phaseDeg(response(ap, 1000, sr)))).toBeCloseTo(180, 1);
  });
  it('clamps frequencies above 0.49 fs', () => {
    expect(Number.isFinite(design.bell(30000, 1, 6, sr).b0)).toBe(true);
  });
  it('outputResponse combines PEQ, crossover, all-pass and polarity', () => {
    const o = {
      polarityInvert: true, peq: [{ on: true, type: 'bell', freq: 1000, gainDb: -6, q: 1 }],
      hp: { type: 'off', freq: 100 }, lp: { type: 'off', freq: 100 }, ap: [], fir: { on: false, path: '', taps: 0, latencySamples: 0 },
    } as unknown as OutputConfig;
    const r = outputResponse(o, sr, [1000]);
    expect(r.magDb[0]).toBeCloseTo(-6, 2);
    expect(Math.abs(r.phaseDeg[0])).toBeCloseTo(180, 1);
  });
});

describe('LR-matched all-pass', () => {
  it('equals the Linkwitz-Riley crossover sum (LR24 and LR48)', () => {
    const fc = 120;
    for (const [t, xo] of [['lr24', 'lr24'], ['lr48', 'lr48']] as const) {
      const ap = allpassMatched(t, fc, sr);
      const hp = crossover(xo, true, fc, sr), lp = crossover(xo, false, fc, sr);
      for (const f of [30, 80, 120, 200, 1000]) {
        const a = chainResponse(ap, f, sr), h = chainResponse(hp, f, sr), l = chainResponse(lp, f, sr);
        expect(Math.hypot(a.re, a.im)).toBeCloseTo(1, 9);
        expect(a.re).toBeCloseTo(h.re + l.re, 6);
        expect(a.im).toBeCloseTo(h.im + l.im, 6);
      }
    }
    expect(allpassMatched('1st', 500, sr)).toHaveLength(1);
    expect(allpassMatched('lr48', 500, sr)).toHaveLength(2);
  });
});
