// Sub ↔ top alignment at the crossover: search the relative delay and polarity that give
// the strongest coherent sum over the crossover band, from two separately captured traces
// (same mic position). Uses absolute phase: each trace's phase plus its own arrival time.
export interface PhaseTrace {
  freq: ArrayLike<number>; mag: ArrayLike<number>; phase: ArrayLike<number>;
  coh?: ArrayLike<number>; // when given, points weigh by the lower of the two coherences
  delaySamples: number; sampleRate: number;
}

export interface AlignResult {
  delayMs: number;      // apply to B (positive = delay B; negative = delay A by −delayMs)
  invert: boolean;      // invert B's polarity
  gainAtXoDb: number;   // summed level at the crossover point, aligned
  currentAtXoDb: number; // summed level at the crossover point, as measured now
  xoFreq: number;       // where A and B have equal level
  freq: number[]; current: number[]; aligned: number[]; // summed magnitude curves (dB)
}

type C = { re: number; im: number };

function absolute(t: PhaseTrace): C[] {
  const d = t.delaySamples / t.sampleRate;
  return Array.from(t.freq, (f, i) => {
    const a = Math.pow(10, t.mag[i] / 20), ph = (t.phase[i] * Math.PI) / 180 - 2 * Math.PI * f * d;
    return { re: a * Math.cos(ph), im: a * Math.sin(ph) };
  });
}

export function alignPair(A: PhaseTrace, B: PhaseTrace, fLo: number, fHi: number, rangeMs = 15, stepMs = 0.02): AlignResult {
  const f = Array.from(A.freq);
  const a = absolute(A), b = absolute(B);
  const band = f.map((x) => x >= fLo && x <= fHi);
  // A point where either measurement is unreliable (noise, a reflection) must not pick the delay.
  const weight = f.map((_, i) => Math.min(A.coh ? A.coh[i] : 1, B.coh ? B.coh[i] : 1));

  const sumAt = (i: number, tauS: number, sign: number): C => {
    const ph = -2 * Math.PI * f[i] * tauS;
    const c = Math.cos(ph), s = Math.sin(ph);
    return { re: a[i].re + sign * (b[i].re * c - b[i].im * s), im: a[i].im + sign * (b[i].re * s + b[i].im * c) };
  };
  const energy = (tauS: number, sign: number) => {
    let e = 0;
    for (let i = 0; i < f.length; i++) if (band[i]) { const s = sumAt(i, tauS, sign); e += weight[i] * (s.re * s.re + s.im * s.im); }
    return e;
  };

  // Exhaustive search, then keep only *local* maxima: other peaks sit one crossover period
  // away and only add latency, so among peaks within 5 % of the best take the smallest delay.
  const peaks: { tau: number; sign: number; e: number }[] = [];
  for (const sign of [1, -1]) {
    const row: number[] = [];
    for (let t = -rangeMs; t <= rangeMs + 1e-9; t += stepMs) row.push(energy(t / 1000, sign));
    for (let k = 0; k < row.length; k++)
      if ((k === 0 || row[k] >= row[k - 1]) && (k === row.length - 1 || row[k] > row[k + 1]))
        peaks.push({ tau: (-rangeMs + k * stepMs) / 1000, sign, e: row[k] });
  }
  const max = Math.max(...peaks.map((c) => c.e));
  const best = peaks.filter((c) => c.e >= 0.95 * max).sort((x, y) => Math.abs(x.tau) - Math.abs(y.tau) || y.e - x.e)[0];

  // Fine refinement around the pick.
  let tau = best.tau, e = best.e;
  for (let d = stepMs / 2000; d > 1e-7; d /= 2)
    for (const s of [d, -d]) { const t = tau + s; const v = energy(t, best.sign); if (v > e) { e = v; tau = t; } }

  // Crossover point: where the two levels are equal inside the band.
  let xi = -1, bestDiff = Infinity;
  for (let i = 0; i < f.length; i++) if (band[i]) { const d = Math.abs(A.mag[i] - B.mag[i]); if (d < bestDiff) { bestDiff = d; xi = i; } }
  const db = (c: C) => 10 * Math.log10(c.re * c.re + c.im * c.im + 1e-30);
  return {
    delayMs: tau * 1000,
    invert: best.sign < 0,
    gainAtXoDb: xi >= 0 ? db(sumAt(xi, tau, best.sign)) : NaN,
    currentAtXoDb: xi >= 0 ? db(sumAt(xi, 0, 1)) : NaN,
    xoFreq: xi >= 0 ? f[xi] : NaN,
    freq: f,
    current: f.map((_, i) => db(sumAt(i, 0, 1))),
    aligned: f.map((_, i) => db(sumAt(i, tau, best.sign))),
  };
}
