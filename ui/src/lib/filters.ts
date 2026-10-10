// Biquad designs and frequency responses — a mirror of engine/src/dsp/Biquad.h so the
// EQ graph can draw instantly. Keep both files in step.
import type { ApBand, ApType, OutputConfig, XoType } from './types';

export interface Biquad { b0: number; b1: number; b2: number; a1: number; a2: number }
export interface Complex { re: number; im: number }

export const identity = (): Biquad => ({ b0: 1, b1: 0, b2: 0, a1: 0, a2: 0 });

export function response(b: Biquad, f: number, sr: number): Complex {
  const w = (2 * Math.PI * f) / sr;
  const c1 = Math.cos(w), s1 = -Math.sin(w), c2 = Math.cos(2 * w), s2 = -Math.sin(2 * w);
  const nr = b.b0 + b.b1 * c1 + b.b2 * c2, ni = b.b1 * s1 + b.b2 * s2;
  const dr = 1 + b.a1 * c1 + b.a2 * c2, di = b.a1 * s1 + b.a2 * s2;
  const d = dr * dr + di * di;
  return { re: (nr * dr + ni * di) / d, im: (ni * dr - nr * di) / d };
}

const mul = (a: Complex, b: Complex): Complex => ({ re: a.re * b.re - a.im * b.im, im: a.re * b.im + a.im * b.re });
export const magDb = (c: Complex) => 10 * Math.log10(c.re * c.re + c.im * c.im);
export const phaseDeg = (c: Complex) => (Math.atan2(c.im, c.re) * 180) / Math.PI;

export function chainResponse(s: Biquad[], f: number, sr: number): Complex {
  let h: Complex = { re: 1, im: 0 };
  for (const b of s) h = mul(h, response(b, f, sr));
  return h;
}

const clampF = (f: number, sr: number) => Math.min(Math.max(f, 1), 0.49 * sr);
const norm = (b0: number, b1: number, b2: number, a0: number, a1: number, a2: number): Biquad =>
  ({ b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0 });
const wcs = (f: number, sr: number) => { const w0 = (2 * Math.PI * clampF(f, sr)) / sr; return [Math.cos(w0), Math.sin(w0)]; };
const k1 = (f: number, sr: number) => Math.tan((Math.PI * clampF(f, sr)) / sr);

export const design = {
  bell(f: number, q: number, g: number, sr: number): Biquad {
    if (g === 0) return identity();
    const [cs, sn] = wcs(f, sr), A = Math.pow(10, g / 40), al = sn / (2 * q);
    return norm(1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A);
  },
  lowShelf(f: number, q: number, g: number, sr: number): Biquad {
    if (g === 0) return identity();
    const [cs, sn] = wcs(f, sr), A = Math.pow(10, g / 40), al = sn / (2 * q), r = 2 * Math.sqrt(A) * al;
    return norm(A * ((A + 1) - (A - 1) * cs + r), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - r),
      (A + 1) + (A - 1) * cs + r, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - r);
  },
  highShelf(f: number, q: number, g: number, sr: number): Biquad {
    if (g === 0) return identity();
    const [cs, sn] = wcs(f, sr), A = Math.pow(10, g / 40), al = sn / (2 * q), r = 2 * Math.sqrt(A) * al;
    return norm(A * ((A + 1) + (A - 1) * cs + r), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - r),
      (A + 1) - (A - 1) * cs + r, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - r);
  },
  notch(f: number, q: number, sr: number): Biquad {
    const [cs, sn] = wcs(f, sr), al = sn / (2 * q);
    return norm(1, -2 * cs, 1, 1 + al, -2 * cs, 1 - al);
  },
  lowpass2(f: number, q: number, sr: number): Biquad {
    const [cs, sn] = wcs(f, sr), al = sn / (2 * q);
    return norm((1 - cs) / 2, 1 - cs, (1 - cs) / 2, 1 + al, -2 * cs, 1 - al);
  },
  highpass2(f: number, q: number, sr: number): Biquad {
    const [cs, sn] = wcs(f, sr), al = sn / (2 * q);
    return norm((1 + cs) / 2, -(1 + cs), (1 + cs) / 2, 1 + al, -2 * cs, 1 - al);
  },
  allpass2(f: number, q: number, sr: number): Biquad {
    const [cs, sn] = wcs(f, sr), al = sn / (2 * q);
    return norm(1 - al, -2 * cs, 1 + al, 1 + al, -2 * cs, 1 - al);
  },
  lowpass1(f: number, sr: number): Biquad { const K = k1(f, sr); return { b0: K / (1 + K), b1: K / (1 + K), b2: 0, a1: (K - 1) / (K + 1), a2: 0 }; },
  highpass1(f: number, sr: number): Biquad { const K = k1(f, sr); return { b0: 1 / (1 + K), b1: -1 / (1 + K), b2: 0, a1: (K - 1) / (K + 1), a2: 0 }; },
  allpass1(f: number, sr: number): Biquad { const K = k1(f, sr), c = (K - 1) / (K + 1); return { b0: c, b1: 1, b2: 0, a1: c, a2: 0 }; },
};

function butterworth(n: number, hp: boolean, f: number, sr: number): Biquad[] {
  const s: Biquad[] = [];
  if (n % 2) s.push(hp ? design.highpass1(f, sr) : design.lowpass1(f, sr));
  for (let k = 1; k <= Math.floor(n / 2); k++) {
    const theta = n % 2 ? (Math.PI * k) / n : (Math.PI * (2 * k - 1)) / (2 * n);
    const q = 1 / (2 * Math.cos(theta));
    s.push(hp ? design.highpass2(f, q, sr) : design.lowpass2(f, q, sr));
  }
  return s;
}

// Type of an all-pass band (older states only have `order`).
export function apType(a: ApBand): ApType { return a.type ?? (a.order === 1 ? '1st' : '2nd'); }

// All-pass sections for a type. LR-matched = the all-pass whose phase equals that Linkwitz-Riley
// crossover's sum: half its order, Butterworth poles (LR24 → Q 0.707; LR48 → Q 0.541 + 1.307).
export function allpassMatched(type: ApType, f: number, sr: number, q = 0.707): Biquad[] {
  if (type === '1st') return [design.allpass1(f, sr)];
  if (type === 'lr24') return [design.allpass2(f, Math.SQRT1_2, sr)];
  if (type === 'lr48') return [1, 2].map((k) => design.allpass2(f, 1 / (2 * Math.cos((Math.PI * (2 * k - 1)) / 8)), sr));
  return [design.allpass2(f, q, sr)];
}

export function crossover(t: XoType, hp: boolean, f: number, sr: number): Biquad[] {
  const orders: Record<XoType, [number, boolean]> = {
    off: [0, false], bw6: [1, false], bw12: [2, false], bw18: [3, false], bw24: [4, false], bw36: [6, false], bw48: [8, false],
    lr12: [1, true], lr24: [2, true], lr48: [4, true],
  };
  const [n, lr] = orders[t] ?? [0, false];
  if (!n) return [];
  const s = butterworth(n, hp, f, sr);
  return lr ? [...s, ...s] : s; // LR = Butterworth squared (same response as the engine)
}

export function peqSection(b: OutputConfig['peq'][number], sr: number): Biquad {
  if (!b.on) return identity();
  switch (b.type) {
    case 'bell': return design.bell(b.freq, b.q, b.gainDb, sr);
    case 'lowshelf': return design.lowShelf(b.freq, b.q, b.gainDb, sr);
    case 'highshelf': return design.highShelf(b.freq, b.q, b.gainDb, sr);
    case 'notch': return design.notch(b.freq, b.q, sr);
    case 'lowpass': return design.lowpass2(b.freq, b.q, sr);
    case 'highpass': return design.highpass2(b.freq, b.q, sr);
  }
  return identity();
}

export function outputSections(o: OutputConfig, sr: number): Biquad[] {
  return [
    ...(o.peq ?? []).map((b) => peqSection(b, sr)),
    ...crossover(o.hp?.type ?? 'off', true, o.hp?.freq ?? 100, sr),
    ...crossover(o.lp?.type ?? 'off', false, o.lp?.freq ?? 100, sr),
    ...(o.ap ?? []).filter((a) => a.on).flatMap((a) => allpassMatched(apType(a), a.freq, sr, a.q)),
    ...(o.airGainDb ? [design.highShelf(o.airFreq, 0.707, o.airGainDb, sr)] : []), // air compensation (engine-derived)
  ];
}

// IIR + polarity response of one output (FIR is not included: it is measured, not modelled).
export function outputResponse(o: OutputConfig, sr: number, freqs: ArrayLike<number>): { magDb: number[]; phaseDeg: number[] } {
  const s = outputSections(o, sr);
  const mag: number[] = [], ph: number[] = [];
  for (let i = 0; i < freqs.length; i++) {
    let h = chainResponse(s, freqs[i], sr);
    if (o.polarityInvert) h = { re: -h.re, im: -h.im };
    mag.push(magDb(h));
    ph.push(phaseDeg(h));
  }
  return { magDb: mag, phaseDeg: ph };
}

export const wrapDeg = (d: number) => ((((d + 180) % 360) + 360) % 360) - 180;
