import { describe, expect, it } from 'vitest';
import { parseTraceText, GRID } from './traceimport';

describe('parseTraceText', () => {
  it('reads REW text exports and resamples to the trace grid', () => {
    const rew = [
      '* Measurement data measured by REW V5.31',
      '* Source: Main L',
      '* Freq(Hz) SPL(dB) Phase(degrees)',
      ...Array.from({ length: 200 }, (_, i) => { const f = 10 * Math.pow(2, i / 18); return `${f.toFixed(3)} ${(80 + (f > 1000 ? -3 : 0)).toFixed(2)} ${(-0.01 * f % 360).toFixed(2)}`; }),
    ].join('\n');
    const t = parseTraceText(rew, 'rew.txt');
    expect(t.freq).toEqual(GRID);
    expect(t.mag[GRID.findIndex((f) => f >= 100)]).toBeCloseTo(80, 1);
    expect(t.mag[GRID.findIndex((f) => f >= 5000)]).toBeCloseTo(77, 1);
    expect(t.coh.every((c) => c === 1)).toBe(true);
    expect(t.name).toBe('rew');
  });
  it('reads Smaart-style tab/comma separated data with coherence', () => {
    const smaart = 'Frequency (Hz)\tMagnitude (dB)\tPhase (deg)\tCoherence\n' +
      Array.from({ length: 100 }, (_, i) => { const f = 20 * Math.pow(1000, i / 99); return `${f},${-6},${0},${0.9}`; }).join('\n');
    const t = parseTraceText(smaart, 'main.csv');
    expect(t.mag[100]).toBeCloseTo(-6, 3);
    expect(t.coh[100]).toBeCloseTo(0.9, 3);
  });
  it('rejects files without data', () => {
    expect(() => parseTraceText('hello\nworld', 'x.txt')).toThrow();
  });
});
