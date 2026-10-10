// How strongly a transfer-function point is drawn, given its coherence (0..1).
//
// Low coherence means the mic is hearing something other than the reference at that
// frequency (reflections, noise, wind, crowd), so the magnitude/phase there can't be
// trusted. Policy: hidden below the blanking threshold (default 0.4), linear fade-in up to
// fully drawn 0.4 above it. The threshold is adjustable, like Smaart's coherence blanking:
// raise it outdoors / in a noisy room, lower it to see through a reverberant hall.
export const COH_HIDE = 0.4;
export const COH_FADE = 0.4;

// The viewer's blanking threshold, shared by every plot in this browser profile.
const KEY = 'vt.cohBlank';
export function blankThreshold(): number {
  try { const v = Number(localStorage.getItem(KEY)); return localStorage.getItem(KEY) !== null && v >= 0 && v <= 0.95 ? v : COH_HIDE; }
  catch { return COH_HIDE; }
}
export function setBlankThreshold(t: number): void { try { localStorage.setItem(KEY, String(t)); } catch { /* per-viewer only */ } }

export function coherenceAlpha(coh: number, threshold = COH_HIDE): number {
  if (!(coh > threshold)) return 0; // also catches NaN
  const full = Math.min(1, threshold + COH_FADE);
  if (coh >= full) return 1;
  return (coh - threshold) / (full - threshold);
}
