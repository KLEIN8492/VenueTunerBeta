import { describe, expect, it } from 'vitest';
import { coherenceAlpha } from './coherence';

describe('coherenceAlpha', () => {
  it('hides below 0.4, fades to full at 0.8', () => {
    expect(coherenceAlpha(0)).toBe(0);
    expect(coherenceAlpha(0.4)).toBe(0);
    expect(coherenceAlpha(0.6)).toBeCloseTo(0.5);
    expect(coherenceAlpha(0.8)).toBe(1);
    expect(coherenceAlpha(1)).toBe(1);
    expect(coherenceAlpha(NaN)).toBe(0);
  });
  it('follows an adjustable blanking threshold', () => {
    expect(coherenceAlpha(0.3, 0)).toBeCloseTo(0.75);
    expect(coherenceAlpha(0.65, 0.6)).toBeGreaterThan(0);
    expect(coherenceAlpha(0.59, 0.6)).toBe(0);
    expect(coherenceAlpha(0.8, 0.6)).toBeCloseTo(0.5);
    expect(coherenceAlpha(1, 0.6)).toBe(1);
    expect(coherenceAlpha(0.95, 0.9)).toBeCloseTo(0.5);
  });
});
