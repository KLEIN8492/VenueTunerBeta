// Shortcut labels for the platform: ⌘ on macOS, Ctrl on Windows and Linux.
export const IS_MAC = typeof navigator !== 'undefined' && /Mac/i.test(navigator.userAgent);
export const MOD = IS_MAC ? '⌘' : 'Ctrl+';
export const SHIFT = IS_MAC ? '⇧' : 'Shift+';

// Rewrites ⌘/⇧ in shortcut text for this platform (no-op on macOS).
export const keys = (s: string): string => (IS_MAC ? s : s.replace(/⇧⌘/g, 'Ctrl+Shift+').replace(/⌘/g, 'Ctrl+').replace(/⇧/g, 'Shift'));

// "Add to selection" modifier: ⌘-click on macOS, Ctrl-click elsewhere.
export const addKey = (e: MouseEvent): boolean => (IS_MAC ? e.metaKey : e.ctrlKey);
