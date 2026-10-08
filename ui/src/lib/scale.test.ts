import { describe, expect, it } from 'vitest';
import { logX, invLogX, linY, freqTicks, fmtHz, dbToLin, linToDb, samplesToMs } from './scale';

describe('scale', () => {
  it('maps log frequency to pixels', () => {
    expect(logX(20, 20, 20000, 1000)).toBeCloseTo(0);
    expect(logX(20000, 20, 20000, 1000)).toBeCloseTo(1000);
    expect(logX(632.4555, 20, 20000, 1000)).toBeCloseTo(500, 1);
  });
  it('inverts log mapping', () => {
    expect(invLogX(logX(1234, 20, 20000, 800), 20, 20000, 800)).toBeCloseTo(1234, 3);
  });
  it('maps y with top = max', () => {
    expect(linY(10, -30, 10, 200)).toBeCloseTo(0);
    expect(linY(-30, -30, 10, 200)).toBeCloseTo(200);
  });
  it('produces decade ticks', () => {
    const t = freqTicks(20, 20000).filter((x) => x.major).map((x) => x.f);
    expect(t).toEqual(expect.arrayContaining([100, 1000, 10000]));
  });
  it('formats and converts', () => {
    expect(fmtHz(1000)).toBe('1k');
    expect(fmtHz(31.5)).toBe('31.5');
    expect(dbToLin(-6)).toBeCloseTo(0.501, 3);
    expect(linToDb(0)).toBe(-Infinity);
    expect(samplesToMs(480, 48000)).toBeCloseTo(10);
  });
});
