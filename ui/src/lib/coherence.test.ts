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
});
