import './lib/style.css';
import { connectFromLocation, type TfFrame } from './lib/engine';
import { Plot, type PlotTrace } from './lib/plot';
import { mountStatusBar, esc } from './lib/statusbar';
import { coherenceAlpha } from './lib/coherence';
import { samplesToMs, SPEED_OF_SOUND } from './lib/scale';
import type { SystemState, Trace } from './lib/types';

const engine = connectFromLocation();
mountStatusBar(document.getElementById('status')!, engine);
const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

// ---------------------------------------------------------------- plots
const magDefaults = { yMin: -30, yMax: 15 };
const mag = new Plot($('mag'), { xMin: 20, xMax: 20000, logX: true, ...magDefaults, yStep: 5, yUnit: 'dB', title: 'MAGNITUDE' });
mag.onReset = () => { Object.assign(mag.opts, magDefaults); mag.draw(); };
const phase = new Plot($('phase'), { xMin: 20, xMax: 20000, logX: true, yMin: -180, yMax: 180, yStep: 90, yUnit: 'deg', title: 'PHASE', yLocked: true });
const coh = new Plot($('coh'), { xMin: 20, xMax: 20000, logX: true, yMin: 0, yMax: 1, yStep: 0.5, yUnit: '', title: 'COHERENCE', yLocked: true });
const irPlot = new Plot($('ir'), { xMin: 0, xMax: 100, logX: false, yMin: -1, yMax: 1, yStep: 0.5, yUnit: 'norm', title: 'IMPULSE RESPONSE', yLocked: true, xFormat: (x) => `${x.toFixed(1)}ms` });

const LIVE = getComputedStyle(document.documentElement).getPropertyValue('--accent').trim();
const PALETTE = ['#ff9f43', '#a78bfa', '#f472b6', '#60a5fa', '#facc15', '#34d399', '#fb7185', '#c084fc'];

// ---------------------------------------------------------------- traces (kept per window)
interface Stored extends Trace { color: string; visible: boolean }
let traces: Stored[] = [];
try { traces = JSON.parse(localStorage.getItem('vt.traces') ?? '[]'); } catch { traces = []; }
const saveTraces = () => { try { localStorage.setItem('vt.traces', JSON.stringify(traces)); } catch { /* storage unavailable */ } };

let live: TfFrame | null = null;
let referenceArrivalMs: number | null = null;

function redraw(): void {
  const magT: PlotTrace[] = [], phT: PlotTrace[] = [], cohT: PlotTrace[] = [];
  for (const t of traces.filter((t) => t.visible)) {
    magT.push({ x: t.freq, y: t.mag, color: t.color, label: t.name });
    phT.push({ x: t.freq, y: t.phase, color: t.color, wrap: 180 });
    cohT.push({ x: t.freq, y: t.coh, color: t.color });
  }
  if (live && live.frames > 0) {
    const alpha = Array.from(live.coh, coherenceAlpha);
    magT.push({ x: live.freq, y: live.mag, color: LIVE, width: 1.75, alpha, label: 'LIVE' });
    phT.push({ x: live.freq, y: live.phase, color: LIVE, width: 1.5, alpha, wrap: 180 });
    cohT.push({ x: live.freq, y: live.coh, color: LIVE });
  }
  mag.traces = magT; phase.traces = phT; coh.traces = cohT;
  mag.draw(); phase.draw(); coh.draw();
}

engine.on('tf', (f) => { live = f; redraw(); renderDelay(); });
engine.on('ir', (f) => {
  if (!f.samples.length) return;
  let peak = 0;
  for (const v of f.samples) peak = Math.max(peak, Math.abs(v));
  const x = Array.from(f.samples, (_, i) => samplesToMs(f.irStart + i, f.sampleRate));
  const y = Array.from(f.samples, (v) => v / (peak || 1));
  // Window around the arrival: 5 ms before (pre-ringing, early speakers) to 45 ms after.
  const arrival = samplesToMs(f.delaySamples, f.sampleRate);
  irPlot.opts.xMin = Math.max(x[0], arrival - 5);
  irPlot.opts.xMax = Math.min(x[x.length - 1], arrival + 45);
  irPlot.traces = [{ x, y, color: LIVE }];
  irPlot.markers = [{ x: samplesToMs(f.delaySamples, f.sampleRate), color: '#e0b341' }];
  irPlot.draw();
});

// ---------------------------------------------------------------- side panel
const side = $('side');
side.innerHTML = `
  <h2>Generator</h2>
  <div class="row"><button id="genOn" class="grow">Pink noise OFF</button></div>
  <div class="row"><label>Level</label><input id="genLevel" type="range" min="-60" max="-12" step="1" class="grow"/><span id="genLevelV" class="dim"></span></div>
  <div class="row"><label>Outputs</label></div>
  <div id="genTargets" class="row" style="flex-wrap:wrap"></div>

  <h2>Measurement</h2>
  <div class="row"><label>Mic</label><select id="mic" class="grow"></select></div>
  <div class="row"><label>Reference</label><select id="ref" class="grow"></select></div>
  <div class="row"><label>Average</label><select id="avg" class="grow"></select></div>
  <div class="row"><label>Smoothing</label><select id="smooth" class="grow"></select></div>
  <div class="row"><label>FFT</label><select id="fftMode" class="grow"></select></div>
  <div class="row"><button id="reset" class="grow">Reset avg [R]</button></div>
  <div class="row"><button id="sweep" class="grow">Log sweep [S]</button><select id="sweepLen"><option value="1">1 s</option><option value="3" selected>3 s</option><option value="5">5 s</option><option value="8">8 s</option></select></div>
  <div id="sweepMsg" class="dim"></div>

  <h2>Delay finder</h2>
  <div id="delay" class="dim">waiting for signal…</div>
  <div class="row"><button id="storeRef" class="grow">Store as reference arrival</button></div>
  <div id="align" class="dim"></div>
  <div class="row"><select id="alignOut" class="grow"></select><button id="alignBtn">Align to ref</button></div>

  <h2>Auto delay alignment</h2>
  <div id="alignOuts" class="row" style="flex-wrap:wrap"></div>
  <div class="row"><label>Align to</label><select id="alignRef" class="grow"></select></div>
  <div class="row"><button id="alignStart" class="grow">Auto-align</button><button id="alignCancel">Cancel</button></div>
  <div id="alignMsg" class="dim">Keep the mic still. Each output plays pink noise alone; delays are set so all arrive together. A before/after snapshot is saved.</div>

  <h2>Traces</h2>
  <div class="row"><input id="traceName" type="text" class="grow" placeholder="name"/><button id="capture">Capture [C]</button></div>
  <div id="traceList" class="list"></div>
  <div class="row"><button id="avgTraces" class="grow">Average visible</button><button id="exportCsv">Export CSV</button></div>
  <div class="dim" style="margin-top:8px">Wheel: move magnitude range · double-click: reset · G: generator</div>
`;

const opt = (v: string | number, label: string, sel: boolean) => `<option value="${v}"${sel ? ' selected' : ''}>${esc(label)}</option>`;
const report = (p: Promise<unknown>) => p.catch((e: Error) => alert(e.message));

function renderControls(s: SystemState): void {
  $('genOn').textContent = `Pink noise ${s.gen.on ? 'ON' : 'OFF'} [G]`;
  $('genOn').className = `grow ${s.gen.on ? 'on' : ''}`;
  ($('genLevel') as HTMLInputElement).value = String(s.gen.levelDb);
  $('genLevelV').textContent = `${s.gen.levelDb.toFixed(0)} dBFS`;
  $('genTargets').innerHTML = s.outputs.map((o, i) =>
    `<button data-t="${i}" class="${s.gen.targets.includes(i) ? 'on' : ''}" title="${esc(o.name)}">${i + 1}</button>`).join('');

  // Measurement mics first; any input can be measured (e.g. a console return).
  const order = s.sources.map((src, i) => ({ src, i })).sort((a, b) => Number(b.src.type === 'mic') - Number(a.src.type === 'mic'));
  ($('mic') as HTMLSelectElement).innerHTML = opt(-1, s.sources.some((x) => x.type === 'mic') ? '— none —' : '— add a mic in Settings › Inputs —', s.measure.micSource < 0) +
    order.map(({ src, i }) => opt(i, `${src.name}${src.type === 'mic' ? '' : ` (${src.type})`}`, s.measure.micSource === i)).join('');
  ($('ref') as HTMLSelectElement).innerHTML = opt('gen', 'Internal loopback (generator)', s.measure.ref === 'generator') +
    s.sources.map((src, i) => opt(i, src.name, s.measure.ref === 'source' && s.measure.refSource === i)).join('');
  ($('avg') as HTMLSelectElement).innerHTML = [1, 2, 4, 8, 16, 32, 64, 128].map((n) => opt(n, `${n} frames`, s.measure.averaging === n)).join('');
  ($('smooth') as HTMLSelectElement).innerHTML = [[0, 'none'], [48, '1/48 oct'], [24, '1/24 oct'], [12, '1/12 oct'], [6, '1/6 oct'], [3, '1/3 oct']]
    .map(([n, l]) => opt(n, String(l), s.measure.smoothing === n)).join('');
  ($('fftMode') as HTMLSelectElement).innerHTML = opt(1, 'Multi-time-window', s.measure.multiWindow) + opt(0, 'Fixed 16k', !s.measure.multiWindow);
  $('alignOuts').innerHTML = s.outputs.map((o, i) =>
    `<button data-a="${i}" class="${alignPick.has(i) ? 'on' : ''}" title="${esc(o.name)}">${i + 1}</button>`).join('');
  const refSel = $('alignRef') as HTMLSelectElement, prevRef = refSel.value;
  refSel.innerHTML = opt(-1, 'latest arrival', prevRef === '-1' || !prevRef) + s.outputs.map((o, i) => opt(i, `${i + 1} · ${o.name}`, prevRef === String(i))).join('');
  const alignSel = $('alignOut') as HTMLSelectElement;
  const prev = alignSel.value;
  alignSel.innerHTML = s.outputs.map((o, i) => opt(i, `${i + 1} · ${o.name}`, String(i) === prev)).join('');
}

engine.on('state', renderControls);

$('genOn').onclick = () => engine.state && report(engine.patch('gen/on', !engine.state.gen.on));
$('genLevel').oninput = (e) => { $('genLevelV').textContent = `${(e.target as HTMLInputElement).value} dBFS`; };
$('genLevel').onchange = (e) => report(engine.patch('gen/levelDb', Number((e.target as HTMLInputElement).value)));
$('genTargets').onclick = (e) => {
  const t = (e.target as HTMLElement).dataset.t;
  if (t === undefined || !engine.state) return;
  const i = Number(t), cur = engine.state.gen.targets;
  report(engine.patch('gen/targets', cur.includes(i) ? cur.filter((x) => x !== i) : [...cur, i]));
};
$('mic').onchange = (e) => {
  report(engine.patch('measure/micSource', Number((e.target as HTMLSelectElement).value)));
};
$('ref').onchange = (e) => {
  const v = (e.target as HTMLSelectElement).value;
  report(v === 'gen' ? engine.patch('measure/ref', 'generator')
    : engine.patchMany([{ path: 'measure/refSource', value: Number(v) }, { path: 'measure/ref', value: 'source' }]));
};
$('avg').onchange = (e) => report(engine.patch('measure/averaging', Number((e.target as HTMLSelectElement).value)));
$('smooth').onchange = (e) => report(engine.patch('measure/smoothing', Number((e.target as HTMLSelectElement).value)));
$('reset').onclick = () => report(engine.request('measure.reset'));
$('fftMode').onchange = (e) => report(engine.patch('measure/multiWindow', (e.target as HTMLSelectElement).value === '1'));

// ---------------------------------------------------------------- log sweep
$('sweep').onclick = () => {
  const seconds = Number(($('sweepLen') as HTMLSelectElement).value);
  $('sweepMsg').textContent = `sweeping ${seconds} s… keep quiet`;
  report(engine.request('measure.sweep', { seconds }).catch((e: Error) => { $('sweepMsg').textContent = ''; throw e; }));
};
engine.on('sweep', (ev) => {
  if (!ev.ok || !ev.trace) { $('sweepMsg').innerHTML = `<span class="bad">${esc(ev.message ?? 'sweep failed')}</span>`; return; }
  addTrace(ev.trace);
  $('sweepMsg').textContent = `${ev.trace.name}: ${samplesToMs(ev.trace.delaySamples, ev.trace.sampleRate).toFixed(3)} ms total`;
});

// ---------------------------------------------------------------- auto alignment
const alignPick = new Set<number>();
$('alignOuts').onclick = (e) => {
  const a = (e.target as HTMLElement).dataset.a;
  if (a === undefined) return;
  const i = Number(a);
  if (alignPick.has(i)) alignPick.delete(i); else alignPick.add(i);
  if (engine.state) renderControls(engine.state);
};
$('alignStart').onclick = () => {
  if (alignPick.size < 2) return alert('Pick at least two outputs to align.');
  const reference = Number(($('alignRef') as HTMLSelectElement).value);
  report(engine.request('align.auto', { outputs: [...alignPick].sort((a, b) => a - b), reference }));
};
$('alignCancel').onclick = () => report(engine.request('align.cancel'));
engine.on('align', (ev) => {
  const name = (o?: number) => (o !== undefined && engine.state ? `${o + 1} · ${engine.state.outputs[o]?.name}` : '');
  const el = $('alignMsg');
  if (ev.stage === 'measuring') el.textContent = `measuring ${name(ev.output)}…`;
  if (ev.stage === 'measured') el.textContent = `${name(ev.output)}: ${ev.arrivalMs!.toFixed(3)} ms`;
  if (ev.stage === 'error') el.innerHTML = `<span class="bad">${esc(ev.message ?? 'failed')}</span>`;
  if (ev.stage === 'done')
    el.innerHTML = ev.delays!.map((d) => `${esc(name(d.output))} → ${d.delayMs.toFixed(3)} ms`).join('<br>') + `<br><span class="ok">saved as "${esc(ev.snapshot ?? '')}"</span>`;
});

// ---------------------------------------------------------------- delay readout + alignment
// Arrival time = total delay minus interface latency (internal loopback) — it still
// includes the output's own processing delay, which is what alignment needs.
function arrivalMs(): number | null {
  const s = engine.status;
  if (!live || !live.delayValid || !s || !engine.state) return null;
  const io = engine.state.measure.ref === 'generator' ? s.ioLatency : 0;
  const ms = samplesToMs(live.delaySamples - io, s.sampleRate);
  return Math.abs(ms) < 0.0005 ? 0 : ms; // no "-0.000"
}

function renderDelay(): void {
  const s = engine.status;
  if (!live || !s) return;
  if (!live.delayValid) { $('delay').innerHTML = `no clear arrival — check mic, generator, level`; return; }
  const total = samplesToMs(live.delaySamples, s.sampleRate);
  const arr = arrivalMs()!;
  $('delay').innerHTML = `
    <div class="big">${arr.toFixed(3)} ms</div>
    <div>${(arr / 1000 * SPEED_OF_SOUND).toFixed(2)} m · ${live.delaySamples.toFixed(2)} smp total (${total.toFixed(3)} ms)</div>
    <div>avg frames ${live.frames}</div>`;
  if (referenceArrivalMs !== null) {
    const d = referenceArrivalMs - arr;
    $('align').innerHTML = `ref ${referenceArrivalMs.toFixed(3)} ms · Δ <span class="${Math.abs(d) < 0.05 ? 'ok' : 'warn'}">${d >= 0 ? '+' : ''}${d.toFixed(3)} ms</span>`;
  }
}

$('storeRef').onclick = () => { referenceArrivalMs = arrivalMs(); renderDelay(); };
$('alignBtn').onclick = () => {
  const arr = arrivalMs();
  if (arr === null || referenceArrivalMs === null || !engine.state) return alert('Store a reference arrival first, then measure the speaker to align.');
  const o = Number(($('alignOut') as HTMLSelectElement).value);
  const next = Math.max(0, engine.state.outputs[o].delayMs + (referenceArrivalMs - arr));
  report(engine.patch(`outputs/${o}/delayMs`, Math.round(next * 1000) / 1000).then(() => engine.request('measure.reset')));
};

// ---------------------------------------------------------------- trace list
function renderTraces(): void {
  $('traceList').innerHTML = traces.map((t, i) => `
    <div class="item">
      <input type="checkbox" data-vis="${i}" ${t.visible ? 'checked' : ''}/>
      <span class="swatch" style="background:${t.color}"></span>
      <span class="grow" data-ren="${i}" title="double-click to rename">${esc(t.name)}</span>
      <span class="dim">${samplesToMs(t.delaySamples, t.sampleRate).toFixed(2)}ms</span>
      <button data-del="${i}">×</button>
    </div>`).join('') || '<div class="item dim">no traces</div>';
  saveTraces();
  redraw();
}
$('traceList').onclick = (e) => {
  const el = e.target as HTMLElement;
  if (el.dataset.vis) { traces[+el.dataset.vis].visible = (el as HTMLInputElement).checked; renderTraces(); }
  if (el.dataset.del) { traces.splice(+el.dataset.del, 1); renderTraces(); }
};
$('traceList').ondblclick = (e) => {
  const i = (e.target as HTMLElement).dataset.ren;
  if (i === undefined) return;
  const name = prompt('Trace name', traces[+i].name);
  if (name) { traces[+i].name = name; renderTraces(); }
};

// Traces keep the processing that was active when captured: the EQ editor uses it to
// compute "predicted after EQ".
function addTrace(t: Trace): void {
  traces.push({ ...t, outputs: engine.state?.outputs, color: PALETTE[traces.length % PALETTE.length], visible: true });
  renderTraces();
}

async function capture(): Promise<void> {
  const input = $('traceName') as HTMLInputElement;
  const name = input.value.trim() || `T${traces.length + 1}`;
  addTrace(await engine.request<Trace>('measure.capture', { name }));
  input.value = '';
}
$('capture').onclick = () => report(capture());

// Spatial average: power-average magnitude of visible traces, coherence-weighted later (M3).
$('avgTraces').onclick = () => {
  const vis = traces.filter((t) => t.visible);
  if (vis.length < 2) return alert('Show at least two traces to average.');
  const n = vis[0].freq.length;
  const avg: Stored = { ...vis[0], name: `AVG(${vis.length})`, color: '#ffffff', visible: true, mag: [], phase: [], coh: [] };
  for (let i = 0; i < n; i++) {
    avg.mag.push(10 * Math.log10(vis.reduce((a, t) => a + Math.pow(10, t.mag[i] / 10), 0) / vis.length));
    avg.phase.push(vis[0].phase[i]);
    avg.coh.push(Math.min(...vis.map((t) => t.coh[i])));
  }
  traces.push(avg);
  renderTraces();
};

$('exportCsv').onclick = () => {
  const vis = traces.filter((t) => t.visible);
  if (!vis.length) return alert('No visible traces to export.');
  for (const t of vis) {
    // REW-compatible text: "freq mag phase" per line; '*' lines are comments.
    const lines = [`* VenueTuner trace: ${t.name}`, `* delay ${t.delaySamples.toFixed(3)} samples @ ${t.sampleRate} Hz`, '* Freq(Hz) SPL(dB) Phase(degrees) Coherence'];
    t.freq.forEach((f, i) => lines.push(`${f.toFixed(3)} ${t.mag[i].toFixed(3)} ${t.phase[i].toFixed(2)} ${t.coh[i].toFixed(4)}`));
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([lines.join('\n')], { type: 'text/plain' }));
    a.download = `${t.name}.txt`;
    a.click();
  }
};

// ---------------------------------------------------------------- keys
document.addEventListener('keydown', (e) => {
  if ((e.target as HTMLElement).tagName === 'INPUT' || e.metaKey || e.ctrlKey) return;
  if (e.key === 'g') $('genOn').click();
  if (e.key === 'c') $('capture').click();
  if (e.key === 'r') $('reset').click();
  if (e.key === 's') $('sweep').click();
});

renderTraces();
