// Mirrors engine/src/core/SystemState.h (JSON form).

export type PeqType = 'bell' | 'lowshelf' | 'highshelf' | 'notch' | 'lowpass' | 'highpass';
export type XoType = 'off' | 'bw6' | 'bw12' | 'bw18' | 'bw24' | 'bw36' | 'bw48' | 'lr12' | 'lr24' | 'lr48';
export interface PeqBand { on: boolean; type: PeqType; freq: number; gainDb: number; q: number }
export interface ApBand { on: boolean; order: 1 | 2; freq: number; q: number }
export interface XoConfig { type: XoType; freq: number }
export interface FirConfig { on: boolean; path: string; taps: number; latencySamples: number }

export interface OutputConfig {
  name: string; type: string; colour: string;
  gainDb: number; polarityInvert: boolean; delayMs: number; mute: boolean;
  limiterOn: boolean; limThreshDb: number; limAttackMs: number; limReleaseMs: number;
  mix: number[];
  peq: PeqBand[]; hp: XoConfig; lp: XoConfig; ap: ApBand[]; fir: FirConfig;
}

export interface SystemState {
  device: { inputDevice: string; outputDevice: string; sampleRate: number; bufferSize: number };
  inputs: { name: string; role: 'console' | 'mic' | 'loopback' | 'none' }[];
  outputs: OutputConfig[];
  gen: { on: boolean; signal: string; levelDb: number; targets: number[] };
  measure: { micInput: number; ref: 'generator' | 'input'; refInput: number; averaging: number; smoothing: number; multiWindow: boolean };
  cal: { ioLatency: Record<string, number>; micCalPath: string };
}

export interface Status {
  event: 'status';
  inputDevice: string; outputDevice: string; sampleRate: number; bufferSize: number;
  numIn: number; numOut: number; cpu: number; xruns: number; callbacks: number;
  running: boolean; faultMuted: boolean; muteAll: boolean; error: string;
  sweeping: boolean; aligning: boolean;
  driverLatency: number; ioLatency: number; ioLatencyCalibrated: boolean; calibrating: boolean;
  inPeak: number[]; outPeak: number[]; limGr: number[];
}

export interface Trace {
  name: string; freq: number[]; mag: number[]; phase: number[]; coh: number[];
  delaySamples: number; sampleRate: number;
  outputs?: OutputConfig[]; // processing at capture time, for "predicted after EQ"
}
