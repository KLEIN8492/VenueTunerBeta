// Mirrors engine/src/core/SystemState.h (JSON form).

export type PeqType = 'bell' | 'lowshelf' | 'highshelf' | 'notch' | 'lowpass' | 'highpass';
export type XoType = 'off' | 'bw6' | 'bw12' | 'bw18' | 'bw24' | 'bw36' | 'bw48' | 'lr12' | 'lr24' | 'lr48';
export interface PeqBand { on: boolean; type: PeqType; freq: number; gainDb: number; q: number }
export interface ApBand { on: boolean; order: 1 | 2; freq: number; q: number }
export interface XoConfig { type: XoType; freq: number }
export interface FirConfig { on: boolean; path: string; taps: number; latencySamples: number }

export type SourceType = 'program' | 'mic' | 'loopback';
export interface SourceConfig { name: string; type: SourceType; channel: number }

export interface OutputConfig {
  name: string; type: string; colour: string;
  channel: number; // physical output, -1 = unassigned
  preset?: string;  // speaker preset last applied
  airDistanceM: number; airFreq: number; airGainDb: number; // air compensation (freq/gain derived by the engine)
  gainDb: number; polarityInvert: boolean; delayMs: number; mute: boolean;
  limiterOn: boolean; limThreshDb: number; limAttackMs: number; limReleaseMs: number;
  mix: number[];
  peq: PeqBand[]; hp: XoConfig; lp: XoConfig; ap: ApBand[]; fir: FirConfig;
}

export interface SystemState {
  device: { inputDevice: string; outputDevice: string; sampleRate: number; bufferSize: number };
  sources: SourceConfig[];
  outputs: OutputConfig[];
  gen: { on: boolean; signal: string; levelDb: number; targets: number[] };
  measure: { micSource: number; ref: 'generator' | 'source'; refSource: number; averaging: number; smoothing: number; multiWindow: boolean;
             positions: string[]; positionMics: number[]; groups: { name: string; outputs: number[] }[] };
  air: { enabled: boolean; tempC: number; humidity: number; pressureKPa: number; refTempC: number; refHumidity: number };
  cal: { ioLatency: Record<string, number>; micCalPath: string; splOffset: Record<string, number> };
}

export interface Status {
  event: 'status';
  inputDevice: string; outputDevice: string; sampleRate: number; bufferSize: number;
  numIn: number; numOut: number; cpu: number; xruns: number; callbacks: number;
  running: boolean; faultMuted: boolean; muteAll: boolean; error: string;
  sweeping: boolean; aligning: boolean; matrixRunning: boolean; canUndo: number; canRedo: number;
  spl: { fast: number[]; slow: number[]; leq: number[]; peak: number; leqSeconds: number; calibrated: boolean }; // [Z, A, C]
  driverLatency: number; ioLatency: number; ioLatencyCalibrated: boolean; calibrating: boolean;
  inPeak: number[]; outPeak: number[]; limGr: number[];
}

export interface Trace {
  name: string; freq: number[]; mag: number[]; phase: number[]; coh: number[];
  delaySamples: number; sampleRate: number;
  outputs?: OutputConfig[]; // processing at capture time, for "predicted after EQ"
  group?: string; position?: string; quality?: number; // from a sequenced (matrix) capture
}
