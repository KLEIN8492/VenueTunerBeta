// Sound level display: shared preferences, formatting, the uncalibrated-estimate notice and
// the calibration dialog (calibrator · match a reference meter · datasheet estimate).
import type { Engine } from './engine';
import type { Status } from './types';
import { esc } from './statusbar';

export type TimeW = 'fast' | 'slow' | 'impulse';
export interface SplPrefs { weighting: number; time: TimeW; refreshHz: number; window: number } // window: 0–4 sliding, 5 = since reset
export const WINDOW_NAMES = ['1 s', '10 s', '1 min', '5 min', '15 min', 'since reset'];

const get = (k: string, d: string) => { try { return localStorage.getItem(k) ?? d; } catch { return d; } };
const put = (k: string, v: string) => { try { localStorage.setItem(k, v); } catch { /* storage unavailable */ } };

export function loadPrefs(): SplPrefs {
  return {
    weighting: Number(get('vt.splW', '1')),
    time: (get('vt.splT', 'slow') as TimeW),
    refreshHz: Number(get('vt.splHz', '2')),
    window: Number(get('vt.splWin', '4')),
  };
}
export function savePrefs(p: SplPrefs): void {
  put('vt.splW', String(p.weighting)); put('vt.splT', p.time); put('vt.splHz', String(p.refreshHz)); put('vt.splWin', String(p.window));
}

export const readingName = (w: number, t: TimeW) => `L${['Z', 'A', 'C'][w]}${t === 'fast' ? 'F' : t === 'slow' ? 'S' : 'I'}`;
export const fmt = (v: number) => (v > -150 ? v.toFixed(1) : '—');
export const limitState = (v: number, limit: number): 'ok' | 'near' | 'over' => (v > limit ? 'over' : v > limit - 3 ? 'near' : 'ok');

// dB SPL offset from datasheet values: at 94 dB SPL (1 Pa) the mic gives `sensMvPa` mV;
// after `gainDb` of preamp gain that is (20·log10(V/0.775) + gain) dBu, i.e. minus the
// interface's dBu at 0 dBFS in dBFS.
export function offsetFromDatasheet(sensMvPa: number, dbuAt0dbfs: number, gainDb: number): number {
  const dbu = 20 * Math.log10(sensMvPa / 1000 / 0.775) + gainDb;
  return 94 - (dbu - dbuAt0dbfs);
}

// Short label telling how much to trust the number.
export function trustLabel(s: Status['spl']): { text: string; cls: string } {
  if (s.method === 'calibrator') return { text: 'calibrated', cls: 'ok' };
  if (s.method === 'reference') return { text: 'matched to a reference meter', cls: 'ok' };
  if (s.method === 'estimate') return { text: 'estimate from datasheets (±3 dB)', cls: 'warn' };
  return { text: 'not calibrated · rough estimate (±10 dB)', cls: 'warn' };
}

// Updates at most `hz` times per second (the measurement itself is unaffected).
export function throttled<T>(hz: () => number, fn: (v: T) => void): (v: T) => void {
  let last = 0;
  return (v: T) => { const now = performance.now(); if (now - last >= 1000 / hz() - 5) { last = now; fn(v); } };
}

// ---------------------------------------------------------------- calibration dialog
export function openCalibration(engine: Engine): void {
  document.getElementById('vt-cal')?.remove();
  const wrap = document.createElement('div');
  wrap.id = 'vt-cal';
  wrap.className = 'modal';
  const st = engine.status?.spl;
  const micName = engine.state && engine.state.measure.micSource >= 0 ? engine.state.sources[engine.state.measure.micSource]?.name : '';
  wrap.innerHTML = `<div style="max-width:560px">
    <h2>Calibrate ${esc(micName || 'the measurement mic')}</h2>
    <div class="dim">Calibration is valid for the current preamp gain: if you turn the mic gain knob afterwards, calibrate again.
      Now: ${st ? `${esc(trustLabel(st).text)}` : ''}</div>
    <h2>1 · Calibrator (most accurate)</h2>
    <div class="row"><span class="dim grow">Put the calibrator on the mic, switch it on, then:</span>
      <input id="calLvl" type="number" value="94" step="0.1" style="width:60px"/><span class="dim">dB</span><button id="calDo">Calibrate</button></div>
    <h2>2 · Match a reference meter</h2>
    <div class="row"><span class="dim grow">Play pink noise; type what the other meter (Smaart, SPL meter…) reads, Slow:</span></div>
    <div class="row"><input id="refLvl" type="number" step="0.1" placeholder="96.4" style="width:70px"/><span class="dim">dB</span>
      <select id="refW"><option value="1">A</option><option value="2">C</option><option value="0">Z</option></select><button id="refDo">Match</button></div>
    <h2>3 · From datasheets (estimate, ±3 dB)</h2>
    <div class="row"><label>Mic</label><input id="dsSens" type="number" value="10" step="0.1" style="width:60px"/><span class="dim">mV/Pa</span>
      <label style="min-width:0">0 dBFS =</label><input id="dsDbu" type="number" value="18" step="0.5" style="width:56px"/><span class="dim">dBu</span>
      <label style="min-width:0">gain</label><input id="dsGain" type="number" value="40" step="1" style="width:50px"/><span class="dim">dB</span></div>
    <div class="row"><span id="dsOut" class="dim grow"></span><button id="dsDo">Use estimate</button></div>
    <div class="row" style="margin-top:10px"><button id="calClear">Forget calibration</button><span class="grow"></span><button id="calClose">Close</button></div>
    <div id="calMsg"></div></div>`;
  document.body.appendChild(wrap);
  const $ = <T extends HTMLElement>(id: string) => wrap.querySelector<T>(`#${id}`)!;
  const num = (id: string) => Number(($(id) as HTMLInputElement).value);
  const done = (p: Promise<unknown>, what: string) => p.then(() => { $('calMsg').innerHTML = `<div class="ok">${what}</div>`; })
    .catch((e: Error) => { $('calMsg').innerHTML = `<div class="bad">${esc(e.message)}</div>`; });
  const showDs = () => { $('dsOut').textContent = `offset ${offsetFromDatasheet(num('dsSens'), num('dsDbu'), num('dsGain')).toFixed(1)} dB`; };
  ['dsSens', 'dsDbu', 'dsGain'].forEach((id) => ($(id).oninput = showDs));
  showDs();
  $('calDo').onclick = () => done(engine.request('spl.calibrate', { level: num('calLvl'), weighting: 0, method: 'calibrator' }), 'Calibrated with the calibrator.');
  $('refDo').onclick = () => {
    if (!($('refLvl') as HTMLInputElement).value) return;
    done(engine.request('spl.calibrate', { level: num('refLvl'), weighting: Number(($('refW') as HTMLSelectElement).value), method: 'reference' }), 'Matched to the reference meter.');
  };
  $('dsDo').onclick = () => done(engine.request('spl.setOffset', { offsetDb: offsetFromDatasheet(num('dsSens'), num('dsDbu'), num('dsGain')) }), 'Estimate from datasheets in use.');
  $('calClear').onclick = () => done(engine.request('spl.clearCal'), 'Back to the rough default estimate.');
  $('calClose').onclick = () => wrap.remove();
}
