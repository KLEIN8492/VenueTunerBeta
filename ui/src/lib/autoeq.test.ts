import { describe, expect, it } from 'vitest';
import { autoEq, targetDb } from './autoeq';
import { design, response, magDb, peqSection } from './filters';

const sr = 48000;
const FREQ = Array.from({ length: 240 }, (_, i) => 20 * Math.pow(2, i / 24));
const bellDb = (f: number, fc: number, q: number, g: number) => magDb(response(design.bell(fc, q, g, sr), f, sr));
const opts = { fLo: 40, fHi: 16000, maxBands: 8, sr };

function correctedDb(mag: number[], bands: ReturnType<typeof autoEq>['bands'], offset: number) {
  return FREQ.map((f, i) => mag[i] - offset + bands.reduce((a, b) => a + magDb(response(peqSection(b, sr), f, sr)), 0));
}

describe('autoEq', () => {
  it('corrects known peaks and dips to within ±1 dB', () => {
    const mag = FREQ.map((f) => bellDb(f, 250, 2, 6) + bellDb(f, 3000, 1, -4) - 20); // arbitrary level
    const coh = FREQ.map(() => 1);
    const r = autoEq({ freq: FREQ, mag, coh }, { kind: 'flat' }, opts);
    expect(r.bands.length).toBeLessThanOrEqual(8);
    const out = correctedDb(mag, r.bands, r.offsetDb);
    FREQ.forEach((f, i) => { if (f > 50 && f < 14000) expect(Math.abs(out[i])).toBeLessThan(1.0); });
    r.bands.forEach((b) => expect(b.gainDb).toBeLessThanOrEqual(3));
  });

  it('caps the combined boost, not just each band', () => {
    const mag = FREQ.map((f) => bellDb(f, 3000, 1.4, -5));
    const r = autoEq({ freq: FREQ, mag, coh: FREQ.map(() => 1) }, { kind: 'flat' }, opts);
    const eq = FREQ.map((f) => r.bands.reduce((a, b) => a + magDb(response(peqSection(b, sr), f, sr)), 0));
    expect(Math.max(...eq)).toBeLessThanOrEqual(3.05);
  });

  it('keeps cutting peaks after a dip ran out of boost headroom', () => {
    const mag = FREQ.map((f) => bellDb(f, 3000, 1.4, -8) + bellDb(f, 200, 2, 5));
    const r = autoEq({ freq: FREQ, mag, coh: FREQ.map(() => 1) }, { kind: 'flat' }, opts);
    expect(r.bands.some((b) => b.freq > 150 && b.freq < 270 && b.gainDb < -3)).toBe(true);
  });

  it('never fills a deep narrow dip', () => {
    const notch = design.notch(1000, 8, sr);
    const mag = FREQ.map((f) => Math.max(-15, magDb(response(notch, f, sr))));
    const r = autoEq({ freq: FREQ, mag, coh: FREQ.map(() => 1) }, { kind: 'flat' }, opts);
    r.bands.filter((b) => b.freq > 700 && b.freq < 1400).forEach((b) => expect(b.gainDb).toBeLessThanOrEqual(0));
  });

  it('ignores low-coherence regions and stays inside the range', () => {
    const mag = FREQ.map((f) => bellDb(f, 5000, 2, 8) + bellDb(f, 30, 2, 8));
    const coh = FREQ.map((f) => (f > 3500 && f < 7000 ? 0.3 : 1));
    const r = autoEq({ freq: FREQ, mag, coh }, { kind: 'flat' }, { ...opts, fLo: 60 });
    r.bands.forEach((b) => {
      expect(b.freq < 4000 || b.freq > 6500).toBe(true);
      expect(b.freq).toBeGreaterThan(45);
    });
  });

  it('targets: tilt and house curve shapes', () => {
    expect(targetDb({ kind: 'flat' }, 100)).toBe(0);
    expect(targetDb({ kind: 'tilt', tiltDbPerOct: -1 }, 2000)).toBeCloseTo(-1, 6);
    const house = { kind: 'house' as const, lfDb: 6, lfFreq: 100, hfDb: 3, hfFreq: 4000 };
    expect(targetDb(house, 25)).toBeGreaterThan(5);
    expect(Math.abs(targetDb(house, 1000))).toBeLessThan(0.5);
    expect(targetDb(house, 20000)).toBeCloseTo(-3, 1);
  });
});
