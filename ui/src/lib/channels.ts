// Physical channel names of the open device (from the driver), kept up to date.
import type { Engine } from './engine';

export class Channels {
  inputs: string[] = [];
  outputs: string[] = [];
  private key = '';
  private listeners: (() => void)[] = [];

  constructor(private engine: Engine) {
    engine.on('open', () => void this.refresh());
    engine.on('status', (s) => {
      const k = `${s.inputDevice}|${s.outputDevice}|${s.numIn}|${s.numOut}`;
      if (k !== this.key) { this.key = k; void this.refresh(); }
    });
  }

  onChange(cb: () => void): void { this.listeners.push(cb); }

  async refresh(): Promise<void> {
    try {
      const c = await this.engine.request<{ inputs: string[]; outputs: string[] }>('device.channels');
      // Some drivers (e.g. built-in Mac devices) name channels "1", "2"…: make them readable.
      const tidy = (names: string[], prefix: string) => names.map((n, i) => (/^\s*\d*\s*$/.test(n) ? `${prefix} ${i + 1}` : n));
      this.inputs = tidy(c.inputs, 'In');
      this.outputs = tidy(c.outputs, 'Out');
      this.listeners.forEach((cb) => cb());
    } catch { /* engine not connected yet */ }
  }

  // "Out 1" alone when the name already says it; "3 · Analog 3" style otherwise.
  private label(names: string[], ch: number, prefix: string): string {
    const n = names[ch];
    if (n === undefined) return `${prefix} ${ch + 1} (not on this interface)`;
    return n === `${prefix} ${ch + 1}` ? n : `${ch + 1} · ${n}`;
  }
  inLabel(ch: number): string { return ch < 0 ? '—' : this.label(this.inputs, ch, 'In'); }
  outLabel(ch: number): string { return ch < 0 ? 'unassigned' : this.label(this.outputs, ch, 'Out'); }
}
