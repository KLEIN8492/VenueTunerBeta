// Shared one-line status bar: device, rate/buffer, CPU, xruns, latency, health.
import type { Engine } from './engine';
import { samplesToMs } from './scale';

export function mountStatusBar(el: HTMLElement, engine: Engine): void {
  let lastCallbacks = -1;
  let stalled = 0;
  const render = () => {
    const s = engine.status;
    if (!engine.connected || !s) {
      el.innerHTML = `<span class="bad">ENGINE DISCONNECTED: reconnecting…</span>`;
      return;
    }
    stalled = s.callbacks === lastCallbacks ? stalled + 1 : 0;
    lastCallbacks = s.callbacks;
    const audioOk = s.running && stalled < 5;
    const lat = samplesToMs(s.ioLatency, s.sampleRate || 48000);
    el.innerHTML = [
      `<span class="${audioOk ? 'ok' : 'bad'}">${audioOk ? '● AUDIO' : '○ NO AUDIO'}</span>`,
      `<span>${esc(s.outputDevice || '—')}${s.inputDevice && s.inputDevice !== s.outputDevice ? ' / ' + esc(s.inputDevice) : ''}</span>`,
      `<span>${(s.sampleRate / 1000).toFixed(1)} kHz / ${s.bufferSize}</span>`,
      `<span>CPU ${(s.cpu * 100).toFixed(1)}%</span>`,
      `<span class="${s.xruns ? 'warn' : ''}">XRUN ${s.xruns}</span>`,
      `<span>I/O ${lat.toFixed(2)} ms ${s.ioLatencyCalibrated ? '(cal)' : '(driver)'}</span>`,
      s.faultMuted ? `<span class="bad">OUTPUTS MUTED</span>` : '',
      s.muteAll ? `<span class="bad">MUTE ALL</span>` : '',
      s.error ? `<span class="bad">${esc(s.error)}</span>` : '',
    ].join('');
  };
  engine.on('status', render);
  engine.on('close', render);
  render();
}

export const esc = (s: string): string =>
  s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!);
