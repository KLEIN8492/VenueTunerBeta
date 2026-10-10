// Pure axis / unit math shared by all plots.

export const logX = (f: number, fMin: number, fMax: number, width: number): number => {
  if (!(f > 0) || !(fMin > 0) || !(fMax > fMin)) return 0;
  return (Math.log(f / fMin) / Math.log(fMax / fMin)) * width;
};

export const invLogX = (x: number, fMin: number, fMax: number, width: number): number =>
  fMin * Math.pow(fMax / fMin, x / width);

export const linX = (v: number, vMin: number, vMax: number, width: number): number =>
  ((v - vMin) / (vMax - vMin)) * width;

// Top of the plot is vMax.
export const linY = (v: number, vMin: number, vMax: number, height: number): number =>
  ((vMax - v) / (vMax - vMin)) * height;

export interface Tick { f: number; major: boolean }

export function freqTicks(fMin: number, fMax: number): Tick[] {
  const ticks: Tick[] = [];
  for (let decade = Math.pow(10, Math.floor(Math.log10(fMin))); decade <= fMax; decade *= 10) {
    for (let m = 1; m < 10; m++) {
      const f = m * decade;
      if (f >= fMin && f <= fMax) ticks.push({ f, major: m === 1 || m === 2 || m === 5 });
    }
  }
  return ticks;
}

export function fmtHz(f: number): string {
  if (f >= 1000) return `${+(f / 1000).toFixed(f % 1000 === 0 ? 0 : 1)}k`;
  return `${+f.toFixed(f < 100 && f % 1 !== 0 ? 1 : 0)}`;
}

export const dbToLin = (db: number): number => Math.pow(10, db / 20);
export const linToDb = (v: number): number => (v <= 0 ? -Infinity : 20 * Math.log10(v));
export const samplesToMs = (s: number, sr: number): number => (s / sr) * 1000;
export const SPEED_OF_SOUND = 343; // m/s at ~20 °C
