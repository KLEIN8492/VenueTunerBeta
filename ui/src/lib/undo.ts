// ⌘Z / ⇧⌘Z: inside a text field undo the typing (native); anywhere else undo the last tuning
// change in the engine. The history lives in the engine, so all windows share it.
import type { Engine } from './engine';

declare global {
  interface Window { vt?: { pathForFile(f: File): string; onHistory?(cb: (kind: 'undo' | 'redo') => void): void } }
}

function editingText(): boolean {
  const a = document.activeElement as HTMLInputElement | null;
  return !!a && (a.tagName === 'TEXTAREA' || (a.tagName === 'INPUT' && ['text', 'number', 'search'].includes(a.type)));
}

export function installUndo(engine: Engine): void {
  const run = (kind: 'undo' | 'redo') => {
    if (editingText()) { document.execCommand(kind); return; }
    engine.request(`history.${kind}`).catch(() => undefined); // "nothing to undo" is not an error worth a dialog
  };
  if (window.vt?.onHistory) window.vt.onHistory(run); // desktop app: from the Edit menu
  else
    document.addEventListener('keydown', (e) => { // plain browser
      if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'z') { e.preventDefault(); run(e.shiftKey ? 'redo' : 'undo'); }
    });
}
