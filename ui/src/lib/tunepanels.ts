// Auto-EQ and sub↔top alignment panels for the Measurement window. Both work from
// captured traces, preview their result on the plots, and apply ordinary parameters
// after saving a "before" snapshot.
import { autoEq, type Target, type AutoEqResult } from './autoeq';
import { alignPair, type AlignResult } from './xoalign';
import { outputResponse } from './filters';
import { esc } from './statusbar';
import type { Engine } from './engine';
import type { PlotTrace } from './plot';
import type { OutputConfig, Trace } from './types';

export interface StoredTrace extends Trace { color: string; visible: boolean }
export interface TuneCtx {
  engine: Engine;
  traces: () => StoredTrace[];
  setOverlay: (t: PlotTrace[]) => void;
  report: (p: Promise<unknown>) => void;
}

const opt = (v: string | number, label: string, sel: boolean) => `<option value="${esc(String(v))}"${sel ? ' selected' : ''}>${esc(label)}</option>`;
const stamp = () => new Date().toTimeString().slice(0, 8).replace(/:/g, '-');
const sr = (e: Engine) => e.status?.sampleRate || e.state?.device.sampleRate || 48000;

// Power-average of the visible traces (spatial average of several mic positions).
function average(ts: StoredTrace[]): StoredTrace | null {
  if (!ts.length) return null;
  const n = ts[0].freq.length;
  const mag: number[] = [], coh: number[] = [];
  for (let i = 0; i < n; i++) {
    mag.push(10 * Math.log10(ts.reduce((a, t) => a + Math.pow(10, t.mag[i] / 10), 0) / ts.length));
    coh.push(Math.min(...ts.map((t) => t.coh[i])));
  }
  return { ...ts[0], name: ts.length > 1 ? `AVG(${ts.length})` : ts[0].name, mag, coh };
}

// PEQ-only response of an output (crossovers/all-pass stay: they're part of the system).
function peqOnly(o: OutputConfig, rate: number, freq: number[]): number[] {
  return outputResponse({ ...o, hp: { type: 'off', freq: 100 }, lp: { type: 'off', freq: 100 }, ap: [], polarityInvert: false }, rate, freq).magDb;
}

// ---------------------------------------------------------------- Auto-EQ
export function mountAutoEq(el: HTMLElement, ctx: TuneCtx): () => void {
  const { engine } = ctx;
  el.innerHTML = `
    <h2>Auto-EQ</h2>
    <div class="dim">Measure the output alone (generator on that output only), capture one or more positions and keep them visible. The EQ is fitted to their average.</div>
    <div class="row"><label>Output</label><select id="aeOut" class="grow"></select></div>
    <div class="row"><label>Using</label><span id="aeUsing" class="dim grow"></span></div>
    <div class="row"><label>Target</label><select id="aeTarget" class="grow">
      <option value="flat">Flat</option><option value="tilt">Tilt</option><option value="house" selected>House curve</option></select></div>
    <div id="aeTilt" class="row"><label>Tilt</label><input id="aeTiltV" type="number" step="0.1" value="-0.5"/><span class="dim">dB/oct around 1 kHz</span></div>
    <div id="aeHouse">
      <div class="row"><label>LF lift</label><input id="aeLfDb" type="number" step="0.5" value="4"/><span class="dim">dB below</span><input id="aeLfF" type="number" step="5" value="100"/><span class="dim">Hz</span></div>
      <div class="row"><label>HF roll</label><input id="aeHfDb" type="number" step="0.5" value="3"/><span class="dim">dB at 20k from</span><input id="aeHfF" type="number" step="100" value="4000"/></div>
    </div>
    <div class="row"><label>Range</label><input id="aeLo" type="number" value="40"/><span class="dim">–</span><input id="aeHi" type="number" value="16000"/><span class="dim">Hz</span></div>
    <div class="row"><label>Bands</label><input id="aeBands" type="number" min="1" max="16" value="8"/><label style="min-width:0">max boost</label><input id="aeBoost" type="number" step="0.5" min="0" max="6" value="3"/><span class="dim">dB</span></div>
    <div class="row"><button id="aeCompute" class="grow">Compute</button><button id="aeApply" disabled>Apply</button></div>
    <div id="aeResult" class="dim"></div>`;
  const $ = <T extends HTMLElement>(id: string) => el.querySelector<T>(`#${id}`)!;
  let result: (AutoEqResult & { out: number }) | null = null;

  const showTargetFields = () => {
    const k = ($('aeTarget') as HTMLSelectElement).value;
    $('aeTilt').hidden = k !== 'tilt';
    $('aeHouse').hidden = k !== 'house';
  };
  showTargetFields();
  $('aeTarget').onchange = showTargetFields;

  const render = () => {
    const s = engine.state;
    if (!s) return;
    const sel = $('aeOut') as HTMLSelectElement, prev = sel.value || String(s.gen.targets[0] ?? 0);
    sel.innerHTML = s.outputs.map((o, i) => opt(i, `${i + 1} · ${o.name}`, String(i) === prev)).join('');
    const vis = ctx.traces().filter((t) => t.visible);
    $('aeUsing').textContent = vis.length ? vis.map((t) => t.name).join(', ') : 'no visible traces: capture first';
  };
  // Range follows the chosen output's crossover.
  $('aeOut').onchange = () => {
    const o = engine.state?.outputs[Number(($('aeOut') as HTMLSelectElement).value)];
    if (!o) return;
    ($('aeLo') as HTMLInputElement).value = String(o.hp.type !== 'off' ? Math.round(o.hp.freq) : 40);
    ($('aeHi') as HTMLInputElement).value = String(o.lp.type !== 'off' ? Math.round(o.lp.freq) : 16000);
  };

  $('aeCompute').onclick = () => {
    const s = engine.state;
    const avg = average(ctx.traces().filter((t) => t.visible));
    if (!s || !avg) return alert('Capture at least one trace of this output and keep it visible.');
    const out = Number(($('aeOut') as HTMLSelectElement).value);
    const num = (id: string) => Number(($(id) as HTMLInputElement).value);
    const kind = ($('aeTarget') as HTMLSelectElement).value;
    const target: Target = kind === 'flat' ? { kind: 'flat' } : kind === 'tilt' ? { kind: 'tilt', tiltDbPerOct: num('aeTiltV') }
      : { kind: 'house', lfDb: num('aeLfDb'), lfFreq: num('aeLfF'), hfDb: num('aeHfDb'), hfFreq: num('aeHfF') };
    // Remove the PEQ that was active at capture: the new EQ replaces it.
    const atCapture = avg.outputs?.[out];
    const raw = atCapture ? avg.mag.map((m, i) => m - peqOnly(atCapture, avg.sampleRate, avg.freq)[i]) : avg.mag;
    const r = autoEq({ freq: avg.freq, mag: raw, coh: avg.coh }, target,
      { fLo: num('aeLo'), fHi: num('aeHi'), sr: sr(engine), maxBands: Math.min(16, num('aeBands')), maxBoostDb: num('aeBoost') });
    result = { ...r, out };
    ctx.setOverlay([
      { x: avg.freq, y: r.target, color: '#9aa0a6', label: 'TARGET' },
      { x: avg.freq, y: r.smoothed, color: '#6b7075', label: 'NOW' },
      { x: avg.freq, y: r.predicted, color: '#ffd166', width: 2, label: 'AFTER' },
    ]);
    $('aeApply').toggleAttribute('disabled', !r.bands.length);
    $('aeResult').innerHTML = r.bands.length
      ? `<table>${r.bands.map((b, i) => `<tr><td>${i + 1}</td><td>${b.freq} Hz</td><td class="${b.gainDb > 0 ? 'warn' : ''}">${b.gainDb > 0 ? '+' : ''}${b.gainDb} dB</td><td>Q ${b.q}</td></tr>`).join('')}</table>
         <div>Replaces the PEQ of <b>${esc(s.outputs[out].name)}</b>. Yellow = predicted result.</div>`
      : 'Already within tolerance: no bands needed.';
  };

  $('aeApply').onclick = () => {
    const s = engine.state;
    if (!s || !result) return;
    const name = `Before auto-EQ ${s.outputs[result.out].name} ${stamp()}`;
    const out = result.out, bands = result.bands;
    ctx.report(engine.request('snapshot.save', { name }).then(() => engine.patch(`outputs/${out}/peq`, bands)).then(() => {
      $('aeResult').innerHTML += `<div class="ok">Applied. Previous settings saved as "${esc(name)}". Re-measure to confirm.</div>`;
      $('aeApply').setAttribute('disabled', '');
    }));
  };

  engine.on('state', render);
  window.addEventListener('vt-traces', render);
  render();
  return render;
}

// ---------------------------------------------------------------- sub ↔ top alignment
export function mountXoAlign(el: HTMLElement, ctx: TuneCtx): () => void {
  const { engine } = ctx;
  el.innerHTML = `
    <h2>Sub ↔ top phase alignment</h2>
    <div class="dim">Same mic position: capture the top alone, then the sub alone (generator on one output at a time). The best delay and polarity for a coherent sum around the crossover are searched using phase, not just the impulse peak.</div>
    <div class="row"><label>Top trace</label><select id="xaA" class="grow"></select></div>
    <div class="row"><label>Sub trace</label><select id="xaB" class="grow"></select></div>
    <div class="row"><label>Top output</label><select id="xaOA" class="grow"></select></div>
    <div class="row"><label>Sub output</label><select id="xaOB" class="grow"></select></div>
    <div class="row"><label>Band</label><input id="xaLo" type="number" value="40"/><span class="dim">–</span><input id="xaHi" type="number" value="250"/><span class="dim">Hz</span></div>
    <div class="row"><button id="xaCompute" class="grow">Compute</button><button id="xaApply" disabled>Apply</button></div>
    <div id="xaResult" class="dim"></div>`;
  const $ = <T extends HTMLElement>(id: string) => el.querySelector<T>(`#${id}`)!;
  let result: (AlignResult & { a: StoredTrace; b: StoredTrace; oa: number; ob: number }) | null = null;

  const render = () => {
    const s = engine.state;
    const ts = ctx.traces();
    const fill = (id: string, items: string[], guess: (n: string) => boolean) => {
      const sel = $(id) as HTMLSelectElement, prev = sel.value;
      sel.innerHTML = items.map((n, i) => opt(i, n, prev ? String(i) === prev : guess(n))).join('');
    };
    fill('xaA', ts.map((t) => t.name), (n) => !/sub/i.test(n));
    fill('xaB', ts.map((t) => t.name), (n) => /sub/i.test(n));
    if (s) {
      fill('xaOA', s.outputs.map((o) => o.name), (n) => !/sub/i.test(n));
      fill('xaOB', s.outputs.map((o) => o.name), (n) => /sub/i.test(n));
    }
  };

  $('xaCompute').onclick = () => {
    const ts = ctx.traces();
    const a = ts[Number(($('xaA') as HTMLSelectElement).value)], b = ts[Number(($('xaB') as HTMLSelectElement).value)];
    if (!a || !b || a === b) return alert('Choose two different traces: the top alone and the sub alone.');
    const r = alignPair(a, b, Number(($('xaLo') as HTMLInputElement).value), Number(($('xaHi') as HTMLInputElement).value));
    result = { ...r, a, b, oa: Number(($('xaOA') as HTMLSelectElement).value), ob: Number(($('xaOB') as HTMLSelectElement).value) };
    ctx.setOverlay([
      { x: a.freq, y: a.mag, color: '#60a5fa88', label: 'TOP' },
      { x: b.freq, y: b.mag, color: '#f472b688', label: 'SUB' },
      { x: r.freq, y: r.current, color: '#6b7075', label: 'SUM NOW' },
      { x: r.freq, y: r.aligned, color: '#ffd166', width: 2, label: 'SUM ALIGNED' },
    ]);
    const who = r.delayMs >= 0 ? 'sub' : 'top';
    $('xaResult').innerHTML = `<div>Delay the <b>${who}</b> by <b>${Math.abs(r.delayMs).toFixed(2)} ms</b>${r.invert ? ' and <b>invert the sub polarity</b>' : ''}.</div>
      <div>At the crossover (${Math.round(r.xoFreq)} Hz): ${r.currentAtXoDb.toFixed(1)} dB now → <span class="ok">${r.gainAtXoDb.toFixed(1)} dB</span> aligned.</div>`;
    $('xaApply').removeAttribute('disabled');
  };

  // Absolute values from the capture-time settings, so re-applying never accumulates.
  $('xaApply').onclick = () => {
    const s = engine.state;
    if (!s || !result) return;
    const { a, b, oa, ob, delayMs, invert } = result;
    const capA = a.outputs?.[oa] ?? s.outputs[oa], capB = b.outputs?.[ob] ?? s.outputs[ob];
    const patches = [
      { path: `outputs/${oa}/delayMs`, value: Math.round((capA.delayMs + Math.max(0, -delayMs)) * 1000) / 1000 },
      { path: `outputs/${ob}/delayMs`, value: Math.round((capB.delayMs + Math.max(0, delayMs)) * 1000) / 1000 },
      { path: `outputs/${ob}/polarityInvert`, value: capB.polarityInvert !== invert },
    ];
    const name = `Before sub align ${stamp()}`;
    ctx.report(engine.request('snapshot.save', { name }).then(() => engine.patchMany(patches)).then(() => {
      $('xaResult').innerHTML += `<div class="ok">Applied. Previous settings saved as "${esc(name)}". Measure top + sub together to confirm.</div>`;
      $('xaApply').setAttribute('disabled', '');
    }));
  };

  engine.on('state', render);
  window.addEventListener('vt-traces', render);
  render();
  return render;
}
