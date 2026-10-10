// Mirrors engine/src/core/SystemState.h (JSON form).

export type PeqType = 'bell' | 'lowshelf' | 'highshelf' | 'notch' | 'lowpass' | 'highpass';
export type XoType = 'off' | 'bw6' | 'bw12' | 'bw18' | 'bw24' | 'bw36' | 'bw48' | 'lr12' | 'lr24' | 'lr48';
export interface PeqBand { on: boolean; type: PeqType; freq: number; gainDb: number; q: number }
// All-pass: no level change, only phase. "Slope" = phase order: 1st turns 180°, 2nd 360° (Q sets
// how fast), LR24/LR48-matched copy the phase of that crossover (for aligning a path that has none).
export type ApType = '1st' | '2nd' | 'lr24' | 'lr48';
export interface ApBand { on: boolean; order: 1 | 2; type?: ApType; freq: number; q: number }
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
  measure: { micSource: number; ref: 'generator' | 'source'; refSource: number; averaging: number; smoothing: number; phaseSmoothing?: number; multiWindow: boolean;
             positions: string[]; positionMics: number[]; groups: { name: string; outputs: number[] }[] };
  limits: { enabled: boolean; laeq15: number; lceq15: number };
  air: { enabled: boolean; tempC: number; humidity: number; pressureKPa: number; refTempC: number; refHumidity: number };
  cal: { ioLatency: Record<string, number>; micCalPath: string; splOffset: Record<string, number>; splMethod: Record<string, string> };
}

export interface Status {
  event: 'status';
  inputDevice: string; outputDevice: string; sampleRate: number; bufferSize: number;
  numIn: number; numOut: number; cpu: number; xruns: number; callbacks: number;
  running: boolean; faultMuted: boolean; muteAll: boolean; error: string;
  sweeping: boolean; aligning: boolean; matrixRunning: boolean; canUndo: number; canRedo: number;
  // Sound level of the main mic, in dB SPL (offset applied). Arrays are [Z, A, C].
  spl: { fast: number[]; slow: number[]; impulse: number[]; leq: number[]; max: number[]; leqWindows: number[][];
         peak: number; cPeak: number; leqSeconds: number; calibrated: boolean;
         method: 'calibrator' | 'reference' | 'estimate' | 'default'; offset: number; micDbfs: number;
         gapSeconds: number }; // audio that never reached the Leq (analysis overload) since reset
  levels: { ref: number; mic: number }; // dBFS RMS, for gain matching
  driverLatency: number; ioLatency: number; ioLatencyCalibrated: boolean; calibrating: boolean;
  warning: string; // measurement setup problem the engine can see (e.g. separate input/output clocks)
  inPeak: number[]; outPeak: number[]; limGr: number[];
}

export interface Trace {
  name: string; freq: number[]; mag: number[]; phase: number[]; coh: number[];
  delaySamples: number; sampleRate: number;
  outputs?: OutputConfig[]; // processing at capture time, for "predicted after EQ"
  group?: string; position?: string; quality?: number; // from a sequenced (matrix) capture
}
