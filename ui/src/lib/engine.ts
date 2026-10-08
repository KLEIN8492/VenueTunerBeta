// WebSocket client for the VenueTuner engine: request/reply, events, binary frames,
// automatic reconnect. One instance per window.
import type { Status, SystemState } from './types';

export interface TfFrame {
  type: 'tf'; delaySamples: number; frames: number; delayValid: boolean;
  freq: Float32Array; mag: Float32Array; phase: Float32Array; coh: Float32Array;
}
export interface IrFrame { type: 'ir'; sampleRate: number; delaySamples: number; irStart: number; samples: Float32Array }

// Binary layout (float32): see engine/src/server/Server.h
export function decodeBinary(buf: ArrayBuffer): TfFrame | IrFrame {
  const f = new Float32Array(buf);
  const n = f[1];
  if (f[0] === 1) {
    const at = (k: number) => f.subarray(5 + k * n, 5 + (k + 1) * n);
    return { type: 'tf', delaySamples: f[2], frames: f[3], delayValid: f[4] === 1, freq: at(0), mag: at(1), phase: at(2), coh: at(3) };
  }
  return { type: 'ir', sampleRate: f[2], delaySamples: f[3], irStart: f[4], samples: f.subarray(5, 5 + n) };
}

type Events = {
  state: SystemState; status: Status; tf: TfFrame; ir: IrFrame;
  calibration: { ok: boolean; samples: number; message: string };
  sweep: { ok: boolean; trace?: import('./types').Trace; message?: string };
  align: { stage: 'measuring' | 'measured' | 'done' | 'error'; output?: number; arrivalMs?: number;
           delays?: { output: number; delayMs: number }[]; snapshot?: string; message?: string };
  error: { message: string };
  open: void; close: void;
};
type Listener<T> = (v: T) => void;

export class Engine {
  state: SystemState | null = null;
  status: Status | null = null;
  connected = false;
  private ws: WebSocket | null = null;
  private nextId = 1;
  private pending = new Map<number, { resolve: (v: unknown) => void; reject: (e: Error) => void }>();
  private listeners = new Map<keyof Events, Listener<never>[]>();

  constructor(private url: string) { this.connect(); }

  on<K extends keyof Events>(event: K, cb: Listener<Events[K]>): void {
    if (!this.listeners.has(event)) this.listeners.set(event, []);
    this.listeners.get(event)!.push(cb as Listener<never>);
  }

  request<T = unknown>(cmd: string, args?: object): Promise<T> {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return Promise.reject(new Error('engine not connected'));
    const id = this.nextId++;
    this.ws.send(JSON.stringify({ id, cmd, args }));
    return new Promise<T>((resolve, reject) => this.pending.set(id, { resolve: resolve as (v: unknown) => void, reject }));
  }

  patch(path: string, value: unknown): Promise<SystemState> { return this.request('state.patch', { path, value }); }
  patchMany(patches: { path: string; value: unknown }[]): Promise<SystemState> { return this.request('state.patchMany', { patches }); }

  private emit<K extends keyof Events>(event: K, v: Events[K]): void {
    for (const cb of this.listeners.get(event) ?? []) (cb as Listener<Events[K]>)(v);
  }

  private connect(): void {
    const ws = new WebSocket(this.url);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    ws.onopen = () => { this.connected = true; this.emit('open', undefined); };
    ws.onclose = () => {
      const was = this.connected;
      this.connected = false;
      for (const p of this.pending.values()) p.reject(new Error('engine disconnected'));
      this.pending.clear();
      if (was) this.emit('close', undefined);
      setTimeout(() => this.connect(), 1000);
    };
    ws.onmessage = (e) => {
      if (typeof e.data !== 'string') {
        const frame = decodeBinary(e.data as ArrayBuffer);
        if (frame.type === 'tf') this.emit('tf', frame); else this.emit('ir', frame);
        return;
      }
      const m = JSON.parse(e.data);
      if (m.id !== undefined && this.pending.has(m.id)) {
        const p = this.pending.get(m.id)!;
        this.pending.delete(m.id);
        if (m.ok) p.resolve(m.result); else p.reject(new Error(m.error));
      } else if (m.event === 'state') { this.state = m.state; this.emit('state', m.state); }
      else if (m.event === 'status') { this.status = m; this.emit('status', m); }
      else if (m.event) this.emit(m.event, m);
    };
  }
}

// The Electron shell (app:// protocol) passes ?port=…; a plain browser served over
// http(s) connects to the engine on the same host, default port 47800.
export function connectFromLocation(): Engine {
  const params = new URLSearchParams(location.search);
  const port = params.get('port') ?? '47800';
  const web = location.protocol === 'http:' || location.protocol === 'https:';
  const host = web && location.hostname ? location.hostname : '127.0.0.1';
  return new Engine(`ws://${host}:${port}`);
}
