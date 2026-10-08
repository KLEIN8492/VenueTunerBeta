import { describe, expect, it } from 'vitest';
import { decodeBinary } from './engine';

describe('decodeBinary', () => {
  it('decodes a TF frame', () => {
    const n = 2;
    const f = new Float32Array([1, n, 480.5, 12, 1, 100, 1000, -6, -3, 0, 45, 0.9, 1]);
    const d = decodeBinary(f.buffer);
    expect(d.type).toBe('tf');
    if (d.type !== 'tf') return;
    expect(d.delaySamples).toBeCloseTo(480.5);
    expect(d.frames).toBe(12);
    expect(d.delayValid).toBe(true);
    expect(Array.from(d.freq)).toEqual([100, 1000]);
    expect(Array.from(d.mag)).toEqual([-6, -3]);
    expect(Array.from(d.phase)).toEqual([0, 45]);
    expect(d.coh[1]).toBe(1);
  });
  it('decodes an IR frame', () => {
    const f = new Float32Array([2, 3, 48000, 100, 50, 0.1, 1, 0.2]);
    const d = decodeBinary(f.buffer);
    expect(d.type).toBe('ir');
    if (d.type !== 'ir') return;
    expect(d.sampleRate).toBe(48000);
    expect(d.irStart).toBe(50);
    expect(Array.from(d.samples)).toEqual([expect.closeTo(0.1), 1, expect.closeTo(0.2)]);
  });
});
