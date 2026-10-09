import { describe, expect, it } from 'vitest';
import { offsetFromDatasheet, readingName, limitState } from './spl';

describe('spl helpers', () => {
  it('datasheet offset: 10 mV/Pa, +18 dBu at 0 dBFS, 40 dB gain', () => {
    // 94 dB SPL = 1 Pa → 10 mV = −37.8 dBu, +40 dB gain = +2.2 dBu = −15.8 dBFS → offset 109.8
    expect(offsetFromDatasheet(10, 18, 40)).toBeCloseTo(109.8, 1);
    expect(offsetFromDatasheet(10, 18, 50)).toBeCloseTo(99.8, 1); // more gain → smaller offset
  });
  it('reading names follow the standard notation', () => {
    expect(readingName(1, 'slow')).toBe('LAS');
    expect(readingName(2, 'fast')).toBe('LCF');
    expect(readingName(0, 'impulse')).toBe('LZI');
  });
  it('limit states', () => {
    expect(limitState(95, 102)).toBe('ok');
    expect(limitState(99.5, 102)).toBe('near');
    expect(limitState(102.1, 102)).toBe('over');
  });
});
