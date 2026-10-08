// Mirrors engine/src/core/SystemState.h (JSON form).

export interface OutputConfig {
  name: string; type: string; colour: string;
  gainDb: number; polarityInvert: boolean; delayMs: number; mute: boolean;
  limiterOn: boolean; limThreshDb: number; limAttackMs: number; limReleaseMs: number;
  mix: number[];
}

export interface SystemState {
  device: { inputDevice: string; outputDevice: string; sampleRate: number; bufferSize: number };
  inputs: { name: string; role: 'console' | 'mic' | 'loopback' | 'none' }[];
  outputs: OutputConfig[];
  gen: { on: boolean; signal: string; levelDb: number; targets: number[] };
  measure: { micInput: number; ref: 'generator' | 'input'; refInput: number; averaging: number; smoothing: number };
  cal: { ioLatency: Record<string, number>; micCalPath: string };
}

export interface Status {
  event: 'status';
  inputDevice: string; outputDevice: string; sampleRate: number; bufferSize: number;
  numIn: number; numOut: number; cpu: number; xruns: number; callbacks: number;
  running: boolean; faultMuted: boolean; muteAll: boolean; error: string;
  driverLatency: number; ioLatency: number; ioLatencyCalibrated: boolean; calibrating: boolean;
  inPeak: number[]; outPeak: number[]; limGr: number[];
}

export interface Trace {
  name: string; freq: number[]; mag: number[]; phase: number[]; coh: number[];
  delaySamples: number; sampleRate: number;
}
