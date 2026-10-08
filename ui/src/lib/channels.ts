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
      this.inputs = c.inputs;
      this.outputs = c.outputs;
      this.listeners.forEach((cb) => cb());
    } catch { /* engine not connected yet */ }
  }

  inLabel(ch: number): string { return ch < 0 ? '—' : `${ch + 1} · ${this.inputs[ch] ?? 'missing'}`; }
  outLabel(ch: number): string { return ch < 0 ? 'unassigned' : `${ch + 1} · ${this.outputs[ch] ?? 'missing'}`; }
}
