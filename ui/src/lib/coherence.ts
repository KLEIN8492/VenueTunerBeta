// How strongly a transfer-function point is drawn, given its coherence (0..1).
//
// Low coherence means the mic is hearing something other than the reference at that
// frequency (reflections, noise, wind, crowd), so the magnitude/phase there can't be
// trusted. Smaart blanks points below a threshold; other tools fade them.
//
// Returns an opacity in [0, 1]: 1 = fully drawn, 0 = hidden.
export function coherenceAlpha(coh: number): number {
  // TODO(you): choose the blanking/fading policy, e.g. hard threshold vs gradual fade.
  void coh;
  return 1;
}
