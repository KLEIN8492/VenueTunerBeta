// WebSocket client for the VenueTuner engine: request/reply, events, binary frames,
// automatic reconnect. One instance per window.
import type { Status, SystemState } from './types';

export interface TfFrame {
  type: 'tf'; delaySamples: number; frames: number; delayValid: boolean;
  delayLocked: boolean; autoDelaySamples: number; // locked = delaySamples is the user's, auto = the finder's
  freq: Float32Array; mag: Float32Array; phase: Float32Array; coh: Float32Array;
}
export interface IrFrame { type: 'ir'; sampleRate: number; delaySamples: number; irStart: number; samples: Float32Array }
export interface RtaFrame { type: 'rta'; offsetDb: number; calibrated: boolean; freq: Float32Array; levelDb: Float32Array }

// Binary layout (float32): see engine/src/server/Server.h
const MAX_BINS = 1 << 16;

function copy(f: Float32Array, start: number, n: number): Float32Array {
  const end = Math.min(f.length, start + n);
  return start >= f.length ? new Float32Array(0) : f.slice(start, Math.max(start, end));
}

export function decodeBinary(buf: ArrayBuffer): TfFrame | IrFrame | RtaFrame | null {
  if (buf.byteLength < 20 || buf.byteLength % 4 !== 0) return null;
  const f = new Float32Array(buf);
  const kind = f[0], n = f[1];
  if (!Number.isFinite(kind) || !Number.isFinite(n) || n < 0 || n > MAX_BINS || n !== (n | 0)) return null;
  const count = n | 0;
  if (kind === 3) {
    if (f.length < 4 + 2 * count) return null;
    return { type: 'rta', offsetDb: f[2], calibrated: f[3] === 1, freq: copy(f, 4, count), levelDb: copy(f, 4 + count, count) };
  }
  if (kind === 1) {
    if (f.length < 7 + 4 * count) return null;
    const at = (k: number) => copy(f, 7 + k * count, count);
    return { type: 'tf', delaySamples: f[2], frames: f[3], delayValid: f[4] === 1, delayLocked: f[5] === 1, autoDelaySamples: f[6],
             freq: at(0), mag: at(1), phase: at(2), coh: at(3) };
  }
  if (kind === 2) {
    if (f.length < 5 + count) return null;
    return { type: 'ir', sampleRate: f[2], delaySamples: f[3], irStart: f[4], samples: copy(f, 5, count) };
  }
  return null;
}

export interface DecayBand { fc: number; edt: number; t20: number; t30: number; curve: number[] }

type Events = {
  state: SystemState; status: Status; tf: TfFrame; ir: IrFrame; rta: RtaFrame;
  calibration: { ok: boolean; samples: number; message: string };
  sweep: { ok: boolean; trace?: import('./types').Trace; message?: string; warning?: string; decay?: DecayBand[] };
  align: { stage: 'measuring' | 'measured' | 'done' | 'error'; output?: number; arrivalMs?: number;
           delays?: { output: number; delayMs: number }[]; snapshot?: string; message?: string };
  error: { message: string };
  matrix: { stage: 'measuring' | 'retry' | 'captured' | 'rejected' | 'done' | 'error'; group?: number; position?: number;
            quality?: number; trace?: import('./types').Trace; message?: string };
  open: void; close: void;
};
type Listener<T> = (v: T) => void;

export class Engine {
  state: SystemState | null = null;
  status: Status | null = null;
  connected = false;
  private ws: WebSocket | null = null;
  private nextId = 1;
  private connGen = 0;
  private pending = new Map<number, { resolve: (v: unknown) => void; reject: (e: Error) => void; timer: ReturnType<typeof setTimeout> }>();
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
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error('engine request timed out'));
      }, 15_000);
      this.pending.set(id, { resolve: resolve as (v: unknown) => void, reject, timer });
    });
  }

  patch(path: string, value: unknown): Promise<SystemState> { return this.request('state.patch', { path, value }); }
  patchMany(patches: { path: string; value: unknown }[]): Promise<SystemState> { return this.request('state.patchMany', { patches }); }

  private emit<K extends keyof Events>(event: K, v: Events[K]): void {
    for (const cb of this.listeners.get(event) ?? []) (cb as Listener<Events[K]>)(v);
  }

  private failPending(message: string): void {
    for (const p of this.pending.values()) {
      clearTimeout(p.timer);
      p.reject(new Error(message));
    }
    this.pending.clear();
  }

  private connect(): void {
    const gen = ++this.connGen;
    const ws = new WebSocket(this.url);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    ws.onopen = () => {
      if (gen !== this.connGen) return;
      this.connected = true;
      this.emit('open', undefined);
    };
    ws.onclose = () => {
      if (gen !== this.connGen) return;
      const was = this.connected;
      this.connected = false;
      this.failPending('engine disconnected');
      if (was) this.emit('close', undefined);
      setTimeout(() => { if (gen === this.connGen) this.connect(); }, 1000);
    };
    ws.onmessage = (e) => {
      if (gen !== this.connGen) return;
      if (typeof e.data !== 'string') {
        const frame = decodeBinary(e.data as ArrayBuffer);
        if (!frame) return;
        if (frame.type === 'tf') this.emit('tf', frame);
        else if (frame.type === 'ir') this.emit('ir', frame);
        else this.emit('rta', frame);
        return;
      }
      let m: { id?: number; ok?: boolean; result?: unknown; error?: string; event?: string; state?: SystemState };
      try { m = JSON.parse(e.data); } catch { return; }
      if (m.id !== undefined && this.pending.has(m.id)) {
        const p = this.pending.get(m.id)!;
        this.pending.delete(m.id);
        clearTimeout(p.timer);
        if (m.ok) p.resolve(m.result); else p.reject(new Error(m.error));
      } else if (m.event === 'state') { this.state = m.state ?? null; this.emit('state', m.state as SystemState); }
      else if (m.event === 'status') { this.status = m as Status; this.emit('status', m as Status); }
      else if (m.event) this.emit(m.event as keyof Events, m as never);
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
