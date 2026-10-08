// How strongly a transfer-function point is drawn, given its coherence (0..1).
//
// Low coherence means the mic is hearing something other than the reference at that
// frequency (reflections, noise, wind, crowd), so the magnitude/phase there can't be
// trusted. Policy: hidden below 0.4, linear fade-in up to fully drawn at 0.8.
export const COH_HIDE = 0.4;
export const COH_FULL = 0.8;

export function coherenceAlpha(coh: number): number {
  if (!(coh > COH_HIDE)) return 0; // also catches NaN
  if (coh >= COH_FULL) return 1;
  return (coh - COH_HIDE) / (COH_FULL - COH_HIDE);
}
